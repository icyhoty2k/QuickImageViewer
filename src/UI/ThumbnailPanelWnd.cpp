#include "ThumbnailPanelWnd.h"
#include <algorithm>
#include <windowsx.h>
#include <d2d1.h>
#include <shellscalingapi.h>

#include "FileHandler.h"
#include "UIManager.h"
#include "../AppState.h"
#include "../Platform/Constants.h"
#include "../Input/Shortcuts.h"
#include "../Renderer/RendererD2D.h"

namespace UI {
    // Track the currently active panel window for border styling
    HWND g_activePanelHwnd = nullptr;

    void SetActivePanelWindow(HWND hWnd) {
        if (g_activePanelHwnd == hWnd) return; // Already active

        // Just track which window is active; opacity stays at 210
        HWND oldActive = g_activePanelHwnd;
        g_activePanelHwnd = hWnd;

        // Invalidate both old and new active windows so they redraw with updated border
        if (oldActive && IsWindow(oldActive)) {
            InvalidateRect(oldActive, nullptr, FALSE);
        }
        if (hWnd && IsWindow(hWnd)) {
            InvalidateRect(hWnd, nullptr, FALSE);
        }
    }
    // =========================================================================
    // Init
    // =========================================================================
    void ThumbnailPanelWnd::Init(HINSTANCE hInstance, HWND hParent) {
        Init(hInstance, hParent, 0);
    }

    void ThumbnailPanelWnd::Init(HINSTANCE hInstance, HWND hParent, int8_t position) {
        m_hOwner = hParent;
        m_position = position;

        WNDCLASSW wc{};
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = IPanelWindow::WindowRouter;
        wc.hInstance = hInstance;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = ClassName();
        RegisterClassW(&wc);

        int x, y, w, h;
        GetWindowBounds(hParent, m_position, x, y, w, h);

        CreateWindowExW(
                WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
                ClassName(),
                WindowTitle(),
                WS_POPUP,
                x, y, w, h,
                hParent, nullptr, hInstance,
                this // stored by WindowRouter on WM_NCCREATE
                );

        if (!m_hWnd) return;

        SetLayeredWindowAttributes(m_hWnd, 0, Constants::THUMBNAIL_PANEL_WINDOW_OPACITY, LWA_ALPHA);
        CreateDeviceResources();
        ShowWindow(m_hWnd, SW_HIDE);
    }

    // =========================================================================
    // Toggle / Hide / MovePanel
    // =========================================================================
    void ThumbnailPanelWnd::Toggle() {
        if (!m_hWnd) return;
        if (IsWindowVisible(m_hWnd)) {
            Hide();
        } else {
            Show();
            SetFocus(m_hWnd);
            UpdateView();
            SyncSelectionRectangle();
        }
    }

    void ThumbnailPanelWnd::Hide() {
        if (m_hWnd && IsWindowVisible(m_hWnd)) {
            ShowWindow(m_hWnd, SW_HIDE);
            uiManager.OnPanelHidden(this);
            // If this was the active dir panel, clear it so we fall back to F5.
            if (IsDirPanel()) uiManager.SetActiveDirWnd(nullptr);
        }
    }

    void ThumbnailPanelWnd::Show() {
        if (!m_hWnd) return;
        const PanelLayout &layout = uiManager.GetLayout();
        if (layout.occupied(m_position) && layout.slots[m_position] != this) {
            int8_t free = uiManager.NextFreePosition(m_position);
            if (free >= 0) m_position = free;
        }
        m_offset = 0.0f;
        int x, y, w, h;
        GetWindowBounds(m_hOwner ? m_hOwner : m_hWnd, m_position, x, y, w, h);
        SetWindowPos(m_hWnd, HWND_TOPMOST, x, y, w, h, SWP_SHOWWINDOW | SWP_FRAMECHANGED);
        SetForegroundWindow(m_hWnd);
        uiManager.OnPanelShown(this, m_position);
    }

    void ThumbnailPanelWnd::MovePanel() {
        if (!m_hWnd) return;

        // Remove from current slot.
        uiManager.OnPanelHidden(this);

        // Find next free position — skip slots occupied by other panels.
        int8_t next = uiManager.NextFreePosition(m_position);
        if (next < 0) return; // all slots occupied
        m_position = next;
        m_offset = 0.0f;

        int x, y, w, h;
        GetWindowBounds(m_hOwner ? m_hOwner : m_hWnd, m_position, x, y, w, h);
        SetWindowPos(m_hWnd, HWND_TOPMOST, x, y, w, h,
                     SWP_SHOWWINDOW | SWP_FRAMECHANGED);

        // Register at new position — triggers RefreshVerticalPanels.
        uiManager.OnPanelShown(this, m_position);
    }

    void ThumbnailPanelWnd::ClearDirThumbnailCache() {
        DoClearDirThumbnailCache();
    }

    void ThumbnailPanelWnd::RefreshBounds() {
        if (!m_hWnd) return;
        int x, y, w, h;
        GetWindowBounds(m_hOwner ? m_hOwner : m_hWnd, m_position, x, y, w, h);
        SetWindowPos(m_hWnd, HWND_TOPMOST, x, y, w, h, SWP_FRAMECHANGED | SWP_NOACTIVATE);
    }

    // =========================================================================
    // SyncSelectionRectangle
    // =========================================================================
    void ThumbnailPanelWnd::SyncSelectionRectangle() {
        if (!m_hWnd) return;

        m_selectedIdx = -1;
        for (size_t i = 0; i < m_thumbnails.size(); ++i) {
            if (m_thumbnails[i].playlistIndex == app.currentIndex) {
                m_selectedIdx = static_cast<int>(i);
                break;
            }
        }
        ScrollToSelected();
        // ScrollToSelected() may have changed m_offset. Recompute all thumbnail
        // rects so the render sees positions that match the new scroll position.
        // Without this, a large offset jump (e.g. image 800 → image 1) leaves
        // m_thumbnails with rects computed from the old offset, causing the
        // selection highlight to appear off-screen or at the wrong slot.
        RebuildGeometry();
        InvalidateRect(m_hWnd, nullptr, TRUE);
        UpdateWindow(m_hWnd);
    }

    // =========================================================================
    // UpdateView  —  rebuild geometry from source items + request async decodes
    // =========================================================================
    void ThumbnailPanelWnd::UpdateView() {
        if (!m_hWnd || !app.renderer || !IsWindowVisible(m_hWnd)) return;

        m_thumbnails.clear();

        // Spawned DirWnd instances own their own playlist and bypass this guard.
        // Primary DirWnd and CacheWnd still require app.playlist to be populated.
        if (!HasOwnPlaylist() && (app.playlist.empty() || app.currentIndex < 0)) {
            InvalidateRect(m_hWnd, nullptr, TRUE);
            return;
        }

        // Subclass provides the raw list; base owns all layout math.
        std::vector<std::wstring> items = GetSourceItems();
        if (items.empty()) {
            InvalidateRect(m_hWnd, nullptr, TRUE);
            return;
        }

        RECT cr{};
        GetClientRect(m_hWnd, &cr);

        float surfaceW = static_cast<float>(cr.right);
        float surfaceH = static_cast<float>(cr.bottom);
        float thumbW = Constants::THUMBNAIL_PANEL_THUMB_WIDTH * app.dpiScale;
        float thumbH = Constants::THUMBNAIL_PANEL_THUMB_HEIGHT * app.dpiScale;
        float margin = Constants::THUMBNAIL_PANEL_THUMB_MARGIN * app.dpiScale;
        float spacing = Constants::THUMBNAIL_PANEL_THUMB_SPACING * app.dpiScale;
        bool vertical = IsVertical();

        float x = margin;
        float y = margin;

        if (!vertical) {
            y = (surfaceH - thumbH) / 2.0f;
            float totalW = static_cast<float>(items.size()) * (thumbW + spacing) - spacing;
            if (totalW <= surfaceW - 2.0f * margin) {
                x = (surfaceW - totalW) / 2.0f;
            } else {
                float minOff = surfaceW - totalW - 2.0f * margin;
                m_offset = std::clamp(m_offset, minOff, 0.0f);
                x = margin + m_offset;
            }
        } else {
            x = (surfaceW - thumbW) / 2.0f;
            float totalH = static_cast<float>(items.size()) * (thumbH + spacing) - spacing;
            if (totalH <= surfaceH - 2.0f * margin) {
                y = (surfaceH - totalH) / 2.0f;
            } else {
                float minOff = surfaceH - totalH - 2.0f * margin;
                m_offset = std::clamp(m_offset, minOff, 0.0f);
                y = margin + m_offset;
            }
        }

        for (size_t i = 0; i < items.size(); ++i) {
            auto mapIt = app.playlistIndexMap.find(items[i]);

            // Assign -1 if the cached file is not in the current folder
            int idx = (mapIt != app.playlistIndexMap.end()) ? mapIt->second : -1;

            m_thumbnails.push_back({
                D2D1::RectF(x, y, x + thumbW, y + thumbH),
                items[i],
                idx
            });

            if (vertical) y += thumbH + spacing;
            else x += thumbW + spacing;
        }

        PostBuildHook();

        // Update m_selectedIdx to keep the highlight in sync with app.currentIndex,
        // but do NOT call SyncSelectionRectangle() / ScrollToSelected() here.
        // UpdateView() is called on every wheel tick and drag move; auto-scrolling
        // to the selected thumbnail on each call would prevent the user from
        // manually scrolling past it.  SyncSelectionRectangle() is called
        // explicitly by external navigation sites (AppMain, FileHandler) and by
        // WM_SIZE / Toggle() where a geometry rebuild warrants snapping to selection.
        m_selectedIdx = -1;
        for (size_t i = 0; i < m_thumbnails.size(); ++i) {
            if (m_thumbnails[i].playlistIndex == app.currentIndex) {
                m_selectedIdx = static_cast<int>(i);
                break;
            }
        }

        InvalidateRect(m_hWnd, nullptr, FALSE);
    }

    // =========================================================================
    // RebuildGeometry
    // Recomputes every thumbnail's rect from the current m_offset without
    // touching m_thumbnails membership or calling PostBuildHook.
    // Called by SyncSelectionRectangle after ScrollToSelected moves m_offset
    // so the renderer draws geometry that matches the new scroll position.
    // =========================================================================
    void ThumbnailPanelWnd::RebuildGeometry() {
        if (m_thumbnails.empty()) return;

        RECT cr{};
        GetClientRect(m_hWnd, &cr);
        float surfaceW = static_cast<float>(cr.right);
        float surfaceH = static_cast<float>(cr.bottom);
        float thumbW = Constants::THUMBNAIL_PANEL_THUMB_WIDTH * app.dpiScale;
        float thumbH = Constants::THUMBNAIL_PANEL_THUMB_HEIGHT * app.dpiScale;
        float margin = Constants::THUMBNAIL_PANEL_THUMB_MARGIN * app.dpiScale;
        float spacing = Constants::THUMBNAIL_PANEL_THUMB_SPACING * app.dpiScale;
        bool vertical = IsVertical();

        float x = margin;
        float y = margin;

        size_t n = m_thumbnails.size();

        if (!vertical) {
            y = (surfaceH - thumbH) / 2.0f;
            float totalW = static_cast<float>(n) * (thumbW + spacing) - spacing;
            if (totalW <= surfaceW - 2.0f * margin) {
                x = (surfaceW - totalW) / 2.0f;
            } else {
                float minOff = surfaceW - totalW - 2.0f * margin;
                m_offset = std::clamp(m_offset, minOff, 0.0f);
                x = margin + m_offset;
            }
        } else {
            x = (surfaceW - thumbW) / 2.0f;
            float totalH = static_cast<float>(n) * (thumbH + spacing) - spacing;
            if (totalH <= surfaceH - 2.0f * margin) {
                y = (surfaceH - totalH) / 2.0f;
            } else {
                float minOff = surfaceH - totalH - 2.0f * margin;
                m_offset = std::clamp(m_offset, minOff, 0.0f);
                y = margin + m_offset;
            }
        }

        for (auto &t: m_thumbnails) {
            t.rect = D2D1::RectF(x, y, x + thumbW, y + thumbH);
            if (vertical) y += thumbH + spacing;
            else x += thumbW + spacing;
        }
    }

    // =========================================================================
    // ScrollToSelected
    // =========================================================================
    void ThumbnailPanelWnd::ScrollToSelected() {
        if (m_selectedIdx < 0 || m_selectedIdx >= static_cast<int>(m_thumbnails.size()))
            return;

        RECT cr{};
        GetClientRect(m_hWnd, &cr);
        float surfaceW = static_cast<float>(cr.right);
        float surfaceH = static_cast<float>(cr.bottom);
        bool vertical = IsVertical();
        float thumbW = Constants::THUMBNAIL_PANEL_THUMB_WIDTH * app.dpiScale;
        float thumbH = Constants::THUMBNAIL_PANEL_THUMB_HEIGHT * app.dpiScale;
        float scaledSpacing = Constants::THUMBNAIL_PANEL_THUMB_SPACING * app.dpiScale;
        float scaledMargin = Constants::THUMBNAIL_PANEL_THUMB_MARGIN * app.dpiScale;

        if (!vertical) {
            float slotX = static_cast<float>(m_selectedIdx) * (thumbW + scaledSpacing);
            float visL = -m_offset + scaledMargin;
            float visR = visL + surfaceW - scaledMargin * 2.0f;
            if (slotX < visL) m_offset = -(slotX - scaledMargin);
            else if (slotX + thumbW > visR) m_offset = -(slotX + thumbW - surfaceW + scaledMargin);
        } else {
            float slotY = static_cast<float>(m_selectedIdx) * (thumbH + scaledSpacing);
            float visT = -m_offset + scaledMargin;
            float visB = visT + surfaceH - scaledMargin * 2.0f;
            if (slotY < visT) m_offset = -(slotY - scaledMargin);
            else if (slotY + thumbH > visB) m_offset = -(slotY + thumbH - surfaceH + scaledMargin);
        }
        InvalidateRect(m_hWnd, nullptr, FALSE);
    }

    // =========================================================================
    // IsVertical
    // Slot layout: 0=center-float  1=top  2=right  3=bottom  4=left
    // Vertical strips are right(2) and left(4).
    // =========================================================================
    bool ThumbnailPanelWnd::IsVertical() const {
        return (m_position == 2 || m_position == 4);
    }

    // =========================================================================
    // GetWindowBounds  —  shared 5-slot geometry for all thumbnail panels
    // =========================================================================
    void ThumbnailPanelWnd::GetWindowBounds(HWND hRef, int8_t position,
                                            int &x, int &y, int &w, int &h) const {
        HMONITOR hMonitor = MonitorFromWindow(hRef, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = {sizeof(mi)};
        GetMonitorInfoW(hMonitor, &mi);

        // rcWork is the authoritative usable area — it already excludes the
        // taskbar on whichever edge it sits. Use it directly for all positions.
        int wx = mi.rcWork.left;
        int wy = mi.rcWork.top;
        int ww = mi.rcWork.right  - mi.rcWork.left;
        int wh = mi.rcWork.bottom - mi.rcWork.top;

        // Shrink the work area by a small gap on every side that borders the
        // taskbar, so panels never appear glued to the taskbar edge.
        const int gap = Constants::THUMBNAIL_PANEL_TASKBAR_BOTTOM_GAP_HORIZONTAL_PANEL;
        if (mi.rcWork.left   > mi.rcMonitor.left)   { wx += gap; ww -= gap; }
        if (mi.rcWork.top    > mi.rcMonitor.top)     { wy += gap; wh -= gap; }
        if (mi.rcWork.right  < mi.rcMonitor.right)   { ww -= gap; }
        if (mi.rcWork.bottom < mi.rcMonitor.bottom)  { wh -= gap; }

        UINT dpiX = 96, dpiY = 96;
        GetDpiForMonitor(hMonitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
        float dpiScale = static_cast<float>(dpiX) / 96.0f;

        int horzThick = static_cast<int>(
            (Constants::THUMBNAIL_PANEL_THUMB_HEIGHT + Constants::THUMBNAIL_PANEL_THUMB_MARGIN * 2.0f) * dpiScale);
        int vertThick = static_cast<int>(
            (Constants::THUMBNAIL_PANEL_THUMB_WIDTH  + Constants::THUMBNAIL_PANEL_THUMB_MARGIN * 2.0f) * dpiScale);

        // Vertical panels shrink to avoid overlapping visible horizontal panels.
        // Read directly from PanelLayout — always current, no polling needed.
        bool topOccupied    = false;
        bool bottomOccupied = false;
        if (position == 2 || position == 4) {
            const PanelLayout &layout = uiManager.GetLayout();
            topOccupied    = layout.topOccupied();
            bottomOccupied = layout.bottomOccupied();
        }

        int neighbourGap = Constants::THUMBNAIL_PANEL_NEIGHBOUR_GAP_VERTICAL_PANEL;
        auto verticalBounds = [&](int &vy, int &vh) {
            vy       = wy + (topOccupied    ? horzThick + neighbourGap : 0);
            int vBot = wy + wh - (bottomOccupied ? horzThick + neighbourGap : 0);
            vh = vBot - vy;
            if (vh < 0) vh = 0;
        };

        switch (position) {
            case 0: { // center floating
                int pw = static_cast<int>(ww * 0.80f);
                x = wx + (ww - pw) / 2;
                y = wy + (wh - horzThick) / 2;
                w = pw;
                h = horzThick;
                break;
            }
            case 1: // top
                x = wx;
                y = wy;
                w = ww;
                h = horzThick;
                break;
            case 2: { // right
                int vy, vh;
                verticalBounds(vy, vh);
                x = wx + ww - vertThick;
                y = vy;
                w = vertThick;
                h = vh;
                break;
            }
            case 3: // bottom
                x = wx;
                y = wy + wh - horzThick;
                w = ww;
                h = horzThick;
                break;
            case 4: { // left
                int vy, vh;
                verticalBounds(vy, vh);
                x = wx;
                y = vy;
                w = vertThick;
                h = vh;
                break;
            }
            default:
                x = wx;
                y = wy;
                w = ww;
                h = horzThick;
                break;
        }
    }

    // =========================================================================
    // HandleMessage  —  all shared Win32 logic
    // =========================================================================
    LRESULT ThumbnailPanelWnd::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
        // File-scope drag state (per-instance via member vars)
        static POINT s_clickPos = {0, 0};
        static POINT s_lastMouse = {0, 0};
        static bool s_hasMoved = false;
        static bool s_dragging = false;

        // Handle focus events before the main switch
        if (message == WM_SETFOCUS) {
            SetActivePanelWindow(m_hWnd);
            return 0;
        }
        if (message == WM_KILLFOCUS) {
            // Invalidate to remove active border when focus is lost
            if (g_activePanelHwnd == m_hWnd) {
                InvalidateRect(m_hWnd, nullptr, FALSE);
            }
            return 0;
        }

        switch (message) {
            // -----------------------------------------------------------------
            case WM_PAINT: {
                PAINTSTRUCT ps;
                BeginPaint(m_hWnd, &ps);
                Render(m_selectedIdx, m_hoverIdx);
                EndPaint(m_hWnd, &ps);
                return 0;
            }

            // -----------------------------------------------------------------
            case WM_SIZE: {
                ResizeSwapChain(LOWORD(lParam), HIWORD(lParam));
                UpdateView();
                SyncSelectionRectangle(); // geometry changed — snap to current image
                return 0;
            }

            // -----------------------------------------------------------------
            case WM_SETFOCUS:
            case WM_KILLFOCUS:
                return 0;

            // -----------------------------------------------------------------
            case WM_MOUSEWHEEL: {
                int delta = GET_WHEEL_DELTA_WPARAM(wParam);
                float scroll = Constants::THUMBNAIL_PANEL_WINDOW_MOUSE_WHEEL_SPEED;
                if (GetKeyState(VK_SHIFT) & 0x8000) scroll *= 3.0f;
                m_offset += (delta > 0 ? scroll : -scroll)
                        * Constants::THUMBNAIL_PANEL_WINDOW_MOUSE_WHEEL_DIRECTION;
                UpdateView();
                return 0;
            }

            // -----------------------------------------------------------------
            case WM_KEYDOWN: {
                int key = static_cast<int>(wParam);

                if (key == GetKeyToggle()) {
                    uiManager.Toggle(*this);
                    return 0;
                }
                if (key == GetKeyMove()) {
                    MovePanel(); // notifies layout, refreshes verticals internally
                    return 0;
                }
                if (GetKeyExtra() != -1 && key == GetKeyExtra()) {
                    OnExtraKey();
                    return 0;
                }

                // Forward unhandled keys to the main window
                if (m_hOwner) return SendMessageW(m_hOwner, message, wParam, lParam);
                break;
            }

            // -----------------------------------------------------------------
            case WM_LBUTTONDOWN: {
                if (IsDirPanel()) uiManager.SetActiveDirWnd(this);

                // Check if the click landed on the scrollbar strip.
                {
                    const int cx = GET_X_LPARAM(lParam);
                    const int cy = GET_Y_LPARAM(lParam);
                    RECT cr2{};
                    GetClientRect(m_hWnd, &cr2);
                    const float sw2  = static_cast<float>(cr2.right);
                    const float sh2  = static_cast<float>(cr2.bottom);
                    const float bar  = Constants::ThumbnailPanel::SCROLLBAR_THICKNESS * app.dpiScale;
                    const bool  vert = IsVertical();

                    bool inScrollbar = false;
                    if (vert) {
                        // Vertical panels: check x position based on m_position
                        if (m_position == 2) {
                            // Right panel: scrollbar on LEFT if SCROLLBAR_POS_RIGHT_PANEL == LEFT
                            inScrollbar = (Constants::ThumbnailPanel::SCROLLBAR_POS_RIGHT_PANEL
                                          == Constants::ThumbnailPanel::ScrollbarSide::LEFT)
                                          ? (static_cast<float>(cx) < bar)
                                          : (static_cast<float>(cx) >= sw2 - bar);
                        } else {
                            // Left panel: scrollbar on RIGHT if SCROLLBAR_POS_LEFT_PANEL == RIGHT
                            inScrollbar = (Constants::ThumbnailPanel::SCROLLBAR_POS_LEFT_PANEL
                                          == Constants::ThumbnailPanel::ScrollbarSide::LEFT)
                                          ? (static_cast<float>(cx) < bar)
                                          : (static_cast<float>(cx) >= sw2 - bar);
                        }
                    } else {
                        // Horizontal panels: check y position based on m_position
                        if (m_position == 1) {
                            // Top panel: scrollbar at TOP or BOTTOM
                            inScrollbar = (Constants::ThumbnailPanel::SCROLLBAR_POS_TOP_PANEL
                                          == Constants::ThumbnailPanel::ScrollbarEdge::TOP)
                                          ? (static_cast<float>(cy) < bar)
                                          : (static_cast<float>(cy) >= sh2 - bar);
                        } else {
                            // Bottom panel: scrollbar at TOP or BOTTOM
                            inScrollbar = (Constants::ThumbnailPanel::SCROLLBAR_POS_BOTTOM_PANEL
                                          == Constants::ThumbnailPanel::ScrollbarEdge::TOP)
                                          ? (static_cast<float>(cy) < bar)
                                          : (static_cast<float>(cy) >= sh2 - bar);
                        }
                    }

                    if (inScrollbar && !m_thumbnails.empty()) {
                        const float tw2 = Constants::THUMBNAIL_PANEL_THUMB_WIDTH  * app.dpiScale;
                        const float th2 = Constants::THUMBNAIL_PANEL_THUMB_HEIGHT * app.dpiScale;
                        const float sp2 = Constants::THUMBNAIL_PANEL_THUMB_SPACING * app.dpiScale;
                        const float mg2 = Constants::THUMBNAIL_PANEL_THUMB_MARGIN  * app.dpiScale;
                        const float n2  = static_cast<float>(m_thumbnails.size());
                        const float tsz = vert ? th2 : tw2;
                        const float content2 = n2 * (tsz + sp2) - sp2;
                        const float visible2 = (vert ? sh2 : sw2) - 2.0f * mg2;

                        if (content2 > visible2) {
                            m_scrollDragging       = true;
                            m_scrollDragStartMouse  = vert
                                                      ? static_cast<float>(cy)
                                                      : static_cast<float>(cx);
                            m_scrollDragStartOffset = m_offset;
                            SetCapture(m_hWnd);
                            return 0;
                        }
                    }
                }

                s_clickPos.x = GET_X_LPARAM(lParam);
                s_clickPos.y = GET_Y_LPARAM(lParam);
                s_hasMoved = false;
                s_dragging = true;
                SetCapture(m_hWnd);
                GetCursorPos(&s_lastMouse);
                return 0;
            }

            // -----------------------------------------------------------------
            case WM_MOUSEMOVE: {
                int x = GET_X_LPARAM(lParam);
                int y = GET_Y_LPARAM(lParam);

                // Check if hovering over scrollbar strip
                RECT cr3{};
                GetClientRect(m_hWnd, &cr3);
                const float sw3  = static_cast<float>(cr3.right);
                const float sh3  = static_cast<float>(cr3.bottom);
                const float bar3 = Constants::ThumbnailPanel::SCROLLBAR_THICKNESS * app.dpiScale;
                const bool  vert3 = IsVertical();

                bool inScrollbar3 = false;
                if (vert3) {
                    if (m_position == 2) {
                        inScrollbar3 = (Constants::ThumbnailPanel::SCROLLBAR_POS_RIGHT_PANEL
                                       == Constants::ThumbnailPanel::ScrollbarSide::LEFT)
                                       ? (static_cast<float>(x) < bar3)
                                       : (static_cast<float>(x) >= sw3 - bar3);
                    } else {
                        inScrollbar3 = (Constants::ThumbnailPanel::SCROLLBAR_POS_LEFT_PANEL
                                       == Constants::ThumbnailPanel::ScrollbarSide::LEFT)
                                       ? (static_cast<float>(x) < bar3)
                                       : (static_cast<float>(x) >= sw3 - bar3);
                    }
                } else {
                    if (m_position == 1) {
                        inScrollbar3 = (Constants::ThumbnailPanel::SCROLLBAR_POS_TOP_PANEL
                                       == Constants::ThumbnailPanel::ScrollbarEdge::TOP)
                                       ? (static_cast<float>(y) < bar3)
                                       : (static_cast<float>(y) >= sh3 - bar3);
                    } else {
                        inScrollbar3 = (Constants::ThumbnailPanel::SCROLLBAR_POS_BOTTOM_PANEL
                                       == Constants::ThumbnailPanel::ScrollbarEdge::TOP)
                                       ? (static_cast<float>(y) < bar3)
                                       : (static_cast<float>(y) >= sh3 - bar3);
                    }
                }

                if (!m_thumbnails.empty() && inScrollbar3) {
                    const float tw3 = Constants::THUMBNAIL_PANEL_THUMB_WIDTH  * app.dpiScale;
                    const float th3 = Constants::THUMBNAIL_PANEL_THUMB_HEIGHT * app.dpiScale;
                    const float sp3 = Constants::THUMBNAIL_PANEL_THUMB_SPACING * app.dpiScale;
                    const float mg3 = Constants::THUMBNAIL_PANEL_THUMB_MARGIN  * app.dpiScale;
                    const float n3  = static_cast<float>(m_thumbnails.size());
                    const float tsz3 = vert3 ? th3 : tw3;
                    const float content3 = n3 * (tsz3 + sp3) - sp3;
                    const float visible3 = (vert3 ? sh3 : sw3) - 2.0f * mg3;

                    if (content3 > visible3) {
                        SetCursor(LoadCursor(nullptr, IDC_HAND));
                    } else {
                        SetCursor(LoadCursor(nullptr, IDC_ARROW));
                    }
                } else {
                    SetCursor(LoadCursor(nullptr, IDC_ARROW));
                }

                int newHover = -1;
                for (size_t i = 0; i < m_thumbnails.size(); ++i) {
                    if (m_thumbnails[i].HitTest(x, y)) {
                        newHover = static_cast<int>(i);
                        break;
                    }
                }
                if (newHover != m_hoverIdx) {
                    m_hoverIdx = newHover;
                    InvalidateRect(m_hWnd, nullptr, FALSE);
                }

                if (m_scrollDragging) {
                    RECT cr2{};
                    GetClientRect(m_hWnd, &cr2);
                    const float sw2  = static_cast<float>(cr2.right);
                    const float sh2  = static_cast<float>(cr2.bottom);
                    const bool  vert = IsVertical();
                    const float mouseAxis = vert ? static_cast<float>(y) : static_cast<float>(x);
                    const float tw2 = Constants::THUMBNAIL_PANEL_THUMB_WIDTH  * app.dpiScale;
                    const float th2 = Constants::THUMBNAIL_PANEL_THUMB_HEIGHT * app.dpiScale;
                    const float sp2 = Constants::THUMBNAIL_PANEL_THUMB_SPACING * app.dpiScale;
                    const float mg2 = Constants::THUMBNAIL_PANEL_THUMB_MARGIN  * app.dpiScale;
                    const float n2  = static_cast<float>(m_thumbnails.size());
                    const float tsz = vert ? th2 : tw2;
                    const float content2  = n2 * (tsz + sp2) - sp2;
                    const float visible2  = (vert ? sh2 : sw2) - 2.0f * mg2;
                    const float scrollMax = content2 - visible2;
                    const float trackLen  = vert ? sh2 : sw2;
                    const float barThumb  = std::max(Constants::ThumbnailPanel::SCROLLBAR_MIN_THUMB * app.dpiScale,
                                                     trackLen * visible2 / content2);
                    const float scrollPx  = trackLen - barThumb;
                    if (scrollPx > 0.0f) {
                        const float delta = mouseAxis - m_scrollDragStartMouse;
                        const float newScroll = std::clamp(
                            -m_scrollDragStartOffset + delta * scrollMax / scrollPx,
                            0.0f, scrollMax);
                        m_offset = -newScroll;
                        RebuildGeometry();
                        InvalidateRect(m_hWnd, nullptr, FALSE);
                    }
                    return 0;
                }

                if (s_dragging) {
                    if (abs(x - s_clickPos.x) > 5 || abs(y - s_clickPos.y) > 5)
                        s_hasMoved = true;

                    POINT cur;
                    GetCursorPos(&cur);
                    float delta = IsVertical()
                                      ? static_cast<float>(cur.y - s_lastMouse.y)
                                      : static_cast<float>(cur.x - s_lastMouse.x);
                    m_offset += delta;
                    s_lastMouse = cur;
                    UpdateView();
                }
                return 0;
            }

            // -----------------------------------------------------------------
            case WM_LBUTTONUP: {
                if (m_scrollDragging) {
                    m_scrollDragging = false;
                    ReleaseCapture();
                    return 0;
                }
                if (s_dragging) {
                    ReleaseCapture();
                    if (!s_hasMoved) {
                        int x = GET_X_LPARAM(lParam);
                        int y = GET_Y_LPARAM(lParam);
                        for (size_t i = 0; i < m_thumbnails.size(); ++i) {
                            if (m_thumbnails[i].HitTest(x, y)) {
                                // If it belongs to the current folder, just jump to the index
                                if (m_thumbnails[i].playlistIndex >= 0) {
                                    LoadImageIndex(m_hOwner, m_thumbnails[i].playlistIndex);
                                }
                                // If it is from another folder, open it as a specific file
                                else {
                                    OpenSpecificImage(m_hOwner, m_thumbnails[i].filePath);
                                }
                                UpdateView();
                                break;
                            }
                        }
                    }
                    s_dragging = false;
                }
                return 0;
            }
            case Constants::WM_QIV_REPAINT:
                InvalidateRect(m_hWnd, nullptr, FALSE);
                return 0;
            // -----------------------------------------------------------------
            case WM_CLOSE:
                ShowWindow(m_hWnd, SW_HIDE);
                return 0;
        }

        return DefWindowProcW(m_hWnd, message, wParam, lParam);
    }

    // =========================================================================
    // Unified Renderer Calls — each panel owns its swap chain + device context
    // =========================================================================
    void ThumbnailPanelWnd::CreateDeviceResources() {
        if (!app.renderer) return;
        auto *r = dynamic_cast<RendererD2D *>(app.renderer.get());
        if (!r) return;

        ID3D11Device *d3dDev = r->GetD3DDevice();
        ID2D1Device6 *d2dDev = r->GetD2DDevice();
        if (!d3dDev || !d2dDev) return;

        // Create a DXGI swap chain bound to this panel's HWND.
        DXGI_SWAP_CHAIN_DESC1 swapDesc{};
        swapDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        swapDesc.BufferCount = 2;
        swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swapDesc.SampleDesc.Count = 1;

        Microsoft::WRL::ComPtr<IDXGIDevice1> dxgiDevice;
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
        if (FAILED(d3dDev->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) return;
        if (FAILED(dxgiDevice->GetAdapter(&adapter))) return;
        if (FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) return;
        if (FAILED(factory->CreateSwapChainForHwnd(d3dDev, m_hWnd, &swapDesc,
                                                    nullptr, nullptr, &m_swapChain))) return;

        // Create a D2D device context for this panel.
        Microsoft::WRL::ComPtr<ID2D1DeviceContext> baseCtx;
        if (FAILED(d2dDev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &baseCtx))) return;
        if (FAILED(baseCtx.As(&m_panelContext))) return;

        // Size the swap chain and wire the back buffer.
        RECT rc{};
        GetClientRect(m_hWnd, &rc);
        UINT w = static_cast<UINT>(rc.right - rc.left);
        UINT h = static_cast<UINT>(rc.bottom - rc.top);
        if (w == 0 || h == 0) { w = 1200; h = Constants::THUMBNAIL_PANEL_WINDOW_THICKNESS; }
        ResizeSwapChain(w, h);

        // Create brushes using theme colors
        m_panelContext->CreateSolidColorBrush(
            D2D1::ColorF(D2D1::ColorF::DarkSlateGray), &m_placeholderBrush);
        m_panelContext->CreateSolidColorBrush(
            D2D1::ColorF(D2D1::ColorF::LightGreen), &m_borderBrush);
        m_panelContext->CreateSolidColorBrush(
            D2D1::ColorF(D2D1::ColorF::White), &m_hoverBrush);
        m_panelContext->CreateSolidColorBrush(
            Constants::Theme::ThemedGrayD2D(Constants::Theme::Panel::SCROLLBAR_TRACK,
                                            app.themeFactor, Constants::Theme::Panel::SCROLLBAR_TRACK_ALPHA), &m_scrollTrackBrush);
        m_panelContext->CreateSolidColorBrush(
            Constants::Theme::ThemedGrayD2D(Constants::Theme::Panel::SCROLLBAR_THUMB,
                                            app.themeFactor, Constants::Theme::Panel::SCROLLBAR_THUMB_ALPHA), &m_scrollThumbBrush);
    }

    void ThumbnailPanelWnd::ResizeSwapChain(UINT w, UINT h) {
        if (!m_swapChain || !m_panelContext) return;

        m_panelContext->SetTarget(nullptr);
        m_panelBackBuffer.Reset();

        if (FAILED(m_swapChain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0))) return;

        Microsoft::WRL::ComPtr<IDXGISurface> surface;
        if (FAILED(m_swapChain->GetBuffer(0, IID_PPV_ARGS(&surface)))) return;

        D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
        if (FAILED(m_panelContext->CreateBitmapFromDxgiSurface(surface.Get(), &props,
                                                                &m_panelBackBuffer))) return;

        m_panelContext->SetTarget(m_panelBackBuffer.Get());
    }

    void ThumbnailPanelWnd::Render(int selectedIdx, int hoverIdx) {
        if (!m_panelContext || !m_swapChain || !app.renderer) return;
        auto *r = dynamic_cast<RendererD2D *>(app.renderer.get());
        if (!r) return;

        // -------------------------------------------------------------------------
        // Phase 1: resolve bitmap pointers under lock, then release before GPU work.
        // -------------------------------------------------------------------------
        std::vector<RendererD2D::ResolvedThumb> resolved;
        resolved.reserve(m_thumbnails.size());

        r->ResolveThumbnailBitmaps(m_thumbnails, UsesDirThumbCache() ? m_hWnd : nullptr, resolved);

        // -------------------------------------------------------------------------
        // Phase 2: GPU draw — no locks held.
        // -------------------------------------------------------------------------
        m_panelContext->BeginDraw();

        // Use active background color if this panel is active
        D2D1_COLOR_F clearColor = Constants::Theme::ThemedGrayD2D(
            Constants::Theme::Panel::BACKGROUND_INACTIVE, app.themeFactor,
            Constants::Theme::Panel::BACKGROUND_INACTIVE_ALPHA);
        if (g_activePanelHwnd == m_hWnd) {
            clearColor = Constants::Theme::ThemedGrayD2D(
                Constants::Theme::Panel::BACKGROUND_ACTIVE, app.themeFactor,
                Constants::Theme::Panel::BACKGROUND_ACTIVE_ALPHA);
        }
        m_panelContext->Clear(clearColor);

        // Draw separator lines at the gap between this vertical panel and
        // any horizontal neighbour above or below it.
        for (size_t i = 0; i < resolved.size(); ++i) {
            const auto &rv = resolved[i];
            if (rv.bitmap) {
                m_panelContext->DrawBitmap(rv.bitmap.Get(), rv.rect, 1.0f,
                                           D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            } else {
                m_panelContext->FillRectangle(rv.rect, m_placeholderBrush.Get());
            }
            if (static_cast<int>(i) == selectedIdx)
                m_panelContext->DrawRectangle(rv.rect, m_borderBrush.Get(),
                                              Constants::ThumbnailPanel::SELECTION_BORDER_THICKNESS);
            if (static_cast<int>(i) == hoverIdx)
                m_panelContext->DrawRectangle(rv.rect, m_hoverBrush.Get(),
                                              Constants::ThumbnailPanel::HOVER_THICKNESS);
        }

        // Draw scrollbar when content overflows the visible area.
        if (!m_thumbnails.empty() && m_scrollTrackBrush && m_scrollThumbBrush) {
            RECT cr2{};
            GetClientRect(m_hWnd, &cr2);
            const float sw = static_cast<float>(cr2.right);
            const float sh = static_cast<float>(cr2.bottom);
            const float tw = Constants::THUMBNAIL_PANEL_THUMB_WIDTH  * app.dpiScale;
            const float th = Constants::THUMBNAIL_PANEL_THUMB_HEIGHT * app.dpiScale;
            const float sp = Constants::THUMBNAIL_PANEL_THUMB_SPACING * app.dpiScale;
            const float mg = Constants::THUMBNAIL_PANEL_THUMB_MARGIN  * app.dpiScale;
            const float n  = static_cast<float>(m_thumbnails.size());
            const bool  vert = IsVertical();

            const float thumbSz  = vert ? th : tw;
            const float content  = n * (thumbSz + sp) - sp;
            const float visible  = (vert ? sh : sw) - 2.0f * mg;

            if (content > visible) {
                const float BAR      = Constants::ThumbnailPanel::SCROLLBAR_THICKNESS * app.dpiScale;
                const float trackLen = vert ? sh : sw;
                const float barThumb = std::max(Constants::ThumbnailPanel::SCROLLBAR_MIN_THUMB * app.dpiScale,
                                                trackLen * visible / content);
                const float scrollMax = content - visible;
                const float scrollNow = std::clamp(-m_offset, 0.0f, scrollMax);
                const float thumbOff  = (scrollMax > 0.0f)
                                        ? (scrollNow / scrollMax) * (trackLen - barThumb)
                                        : 0.0f;

                D2D1_RECT_F track, thumb;
                if (vert) {
                    // Vertical panels (positions 2=right, 4=left)
                    if (m_position == 2) {
                        // Right panel: use SCROLLBAR_POS_RIGHT_PANEL (LEFT)
                        if (Constants::ThumbnailPanel::SCROLLBAR_POS_RIGHT_PANEL == Constants::ThumbnailPanel::ScrollbarSide::LEFT) {
                            track = D2D1::RectF(0.0f, 0.0f, BAR, sh);
                            thumb = D2D1::RectF(0.0f, thumbOff, BAR, thumbOff + barThumb);
                        } else {
                            track = D2D1::RectF(sw - BAR, 0.0f, sw, sh);
                            thumb = D2D1::RectF(sw - BAR, thumbOff, sw, thumbOff + barThumb);
                        }
                    } else {
                        // Left panel: use SCROLLBAR_POS_LEFT_PANEL (RIGHT)
                        if (Constants::ThumbnailPanel::SCROLLBAR_POS_LEFT_PANEL == Constants::ThumbnailPanel::ScrollbarSide::LEFT) {
                            track = D2D1::RectF(0.0f, 0.0f, BAR, sh);
                            thumb = D2D1::RectF(0.0f, thumbOff, BAR, thumbOff + barThumb);
                        } else {
                            track = D2D1::RectF(sw - BAR, 0.0f, sw, sh);
                            thumb = D2D1::RectF(sw - BAR, thumbOff, sw, thumbOff + barThumb);
                        }
                    }
                } else {
                    // Horizontal panels (positions 1=top, 3=bottom)
                    if (m_position == 1) {
                        // Top panel: use SCROLLBAR_POS_TOP_PANEL
                        if (Constants::ThumbnailPanel::SCROLLBAR_POS_TOP_PANEL == Constants::ThumbnailPanel::ScrollbarEdge::TOP) {
                            track = D2D1::RectF(0.0f, 0.0f, sw, BAR);
                            thumb = D2D1::RectF(thumbOff, 0.0f, thumbOff + barThumb, BAR);
                        } else {
                            track = D2D1::RectF(0.0f, sh - BAR, sw, sh);
                            thumb = D2D1::RectF(thumbOff, sh - BAR, thumbOff + barThumb, sh);
                        }
                    } else {
                        // Bottom panel: use SCROLLBAR_POS_BOTTOM_PANEL
                        if (Constants::ThumbnailPanel::SCROLLBAR_POS_BOTTOM_PANEL == Constants::ThumbnailPanel::ScrollbarEdge::TOP) {
                            track = D2D1::RectF(0.0f, 0.0f, sw, BAR);
                            thumb = D2D1::RectF(thumbOff, 0.0f, thumbOff + barThumb, BAR);
                        } else {
                            track = D2D1::RectF(0.0f, sh - BAR, sw, sh);
                            thumb = D2D1::RectF(thumbOff, sh - BAR, thumbOff + barThumb, sh);
                        }
                    }
                }
                m_panelContext->FillRectangle(track, m_scrollTrackBrush.Get());
                m_panelContext->FillRectangle(thumb, m_scrollThumbBrush.Get());
            }
        }

        HRESULT hr = m_panelContext->EndDraw();
        if (hr != D2DERR_RECREATE_TARGET && hr != static_cast<HRESULT>(DXGI_ERROR_DEVICE_REMOVED))
            m_swapChain->Present(1, 0);
    }
} // namespace UI
