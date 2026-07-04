#include "ThumbnailPanelWnd.h"
#include <algorithm>
#include <windowsx.h>
#include <d2d1.h>

#include "FileHandler.h"
#include "../AppState.h"
#include "../Platform/Constants.h"
#include "../Input/Shortcuts.h"
#include "../Renderer/RendererD2D.h"

namespace UI {
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

        SetLayeredWindowAttributes(m_hWnd, 0, Constants::CACHE_WINDOW_OPACITY, LWA_ALPHA);
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
            SetWindowPos(m_hWnd, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
            SetForegroundWindow(m_hWnd);
            SetFocus(m_hWnd);
            UpdateView();
        }
    }

    void ThumbnailPanelWnd::Hide() {
        if (m_hWnd && IsWindowVisible(m_hWnd))
            ShowWindow(m_hWnd, SW_HIDE);
    }

    void ThumbnailPanelWnd::MovePanel() {
        if (!m_hWnd) return;
        m_position++;
        if (m_position > 4) m_position = 0; // slots 0..4

        int x, y, w, h;
        GetWindowBounds(m_hOwner ? m_hOwner : m_hWnd, m_position, x, y, w, h);
        m_offset = 0.0f;

        SetWindowPos(m_hWnd, HWND_TOPMOST, x, y, w, h,
                     SWP_SHOWWINDOW | SWP_FRAMECHANGED);
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
        InvalidateRect(m_hWnd, nullptr, TRUE);
        UpdateWindow(m_hWnd);
    }

    // =========================================================================
    // UpdateView  —  rebuild geometry from source items + request async decodes
    // =========================================================================
    void ThumbnailPanelWnd::UpdateView() {
        if (!m_hWnd || !app.renderer || !IsWindowVisible(m_hWnd)) return;

        m_thumbnails.clear();

        if (app.playlist.empty() || app.currentIndex < 0) {
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
        float thumbW = Constants::CACHE_THUMB_WIDTH * app.dpiScale;
        float thumbH = Constants::CACHE_THUMB_HEIGHT * app.dpiScale;
        float margin = Constants::CACHE_THUMB_MARGIN * app.dpiScale;
        float spacing = Constants::CACHE_THUMB_SPACING * app.dpiScale;
        bool vertical = IsVertical();

        float x = margin;
        float y = margin;

        if (!vertical) {
            y = (surfaceH - thumbH) / 2.0f;
            float totalW = static_cast<float>(items.size()) * (thumbW + spacing) - spacing;
            if (totalW <= surfaceW) {
                x = (surfaceW - totalW) / 2.0f;
            } else {
                float minOff = surfaceW - totalW - margin;
                m_offset = std::clamp(m_offset, minOff, 0.0f);
                x = margin + m_offset;
            }
        } else {
            x = (surfaceW - thumbW) / 2.0f;
            float totalH = static_cast<float>(items.size()) * (thumbH + spacing) - spacing;
            if (totalH <= surfaceH) {
                y = (surfaceH - totalH) / 2.0f;
            } else {
                float minOff = surfaceH - totalH - margin;
                m_offset = std::clamp(m_offset, minOff, 0.0f);
                y = margin + m_offset;
            }
        }

        for (size_t i = 0; i < items.size(); ++i) {
            auto mapIt = app.playlistIndexMap.find(items[i]);
            int idx = (mapIt != app.playlistIndexMap.end()) ? mapIt->second : static_cast<int>(i);

            m_thumbnails.push_back({
                D2D1::RectF(x, y, x + thumbW, y + thumbH),
                items[i],
                idx
            });

            if (vertical) y += thumbH + spacing;
            else x += thumbW + spacing;
        }

        PostBuildHook();
        SyncSelectionRectangle();
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
        float thumbW = Constants::CACHE_THUMB_WIDTH * app.dpiScale;
        float thumbH = Constants::CACHE_THUMB_HEIGHT * app.dpiScale;
        float scaledSpacing = Constants::CACHE_THUMB_SPACING * app.dpiScale;
        float scaledMargin = Constants::CACHE_THUMB_MARGIN * app.dpiScale;

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

        int monX = mi.rcMonitor.left;
        int monY = mi.rcMonitor.top;
        int monW = mi.rcMonitor.right - mi.rcMonitor.left;
        int monH = mi.rcMonitor.bottom - mi.rcMonitor.top;

        int horzThick = static_cast<int>(
            (Constants::CACHE_THUMB_HEIGHT + Constants::CACHE_THUMB_MARGIN * 2.0f) * app.dpiScale);
        int vertThick = static_cast<int>(
            (Constants::CACHE_THUMB_WIDTH + Constants::CACHE_THUMB_MARGIN * 2.0f) * app.dpiScale);

        switch (position) {
            case 0: { // center floating — 80% of monitor width, thumb-height tall
                int panelW = static_cast<int>(monW * 0.80f);
                x = monX + (monW - panelW) / 2;
                y = monY + (monH - horzThick) / 2;
                w = panelW;
                h = horzThick;
                break;
            }
            case 1: // top
                x = monX;
                y = monY;
                w = monW;
                h = horzThick;
                break;
            case 2: // right
                x = monX + monW - vertThick;
                y = monY;
                w = vertThick;
                h = monH;
                break;
            case 3: // bottom
                x = monX;
                y = monY + monH - horzThick;
                w = monW;
                h = horzThick;
                break;
            case 4: // left
                x = monX;
                y = monY;
                w = vertThick;
                h = monH;
                break;
            default:
                x = monX;
                y = monY;
                w = monW;
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
                return 0;
            }

            // -----------------------------------------------------------------
            case WM_SETFOCUS:
            case WM_KILLFOCUS:
                return 0;

            // -----------------------------------------------------------------
            case WM_MOUSEWHEEL: {
                int delta = GET_WHEEL_DELTA_WPARAM(wParam);
                float scroll = Constants::CACHE_WINDOW_MOUSE_WHEEL_SPEED;
                if (GetKeyState(VK_SHIFT) & 0x8000) scroll *= 3.0f;
                m_offset += (delta > 0 ? scroll : -scroll)
                        * Constants::CACHE_WINDOW_MOUSE_WHEEL_DIRECTION;
                UpdateView();
                return 0;
            }

            // -----------------------------------------------------------------
            case WM_KEYDOWN: {
                int key = static_cast<int>(wParam);

                if (key == Shortcuts::SC_LOCAL_HIDE) {
                    ShowWindow(m_hWnd, SW_HIDE);
                    return 0;
                }
                if (key == GetKeyToggle()) {
                    Toggle();
                    return 0;
                }
                if (key == GetKeyMove()) {
                    MovePanel();
                    return 0;
                }
                if (GetKeyExtra() != -1 && key == GetKeyExtra()) {
                    OnExtraKey();
                    return 0;
                }

                if (key == VK_UP) {
                    if (!m_thumbnails.empty()) {
                        m_selectedIdx = (m_selectedIdx > 0)
                                            ? m_selectedIdx - 1
                                            : static_cast<int>(m_thumbnails.size()) - 1;
                        m_hoverIdx = m_selectedIdx;
                        ScrollToSelected();
                        InvalidateRect(m_hWnd, nullptr, FALSE);
                        UpdateWindow(m_hWnd);
                    }
                    return 0;
                }
                if (key == VK_DOWN) {
                    if (!m_thumbnails.empty()) {
                        m_selectedIdx = (m_selectedIdx < static_cast<int>(m_thumbnails.size()) - 1)
                                            ? m_selectedIdx + 1
                                            : 0;
                        m_hoverIdx = m_selectedIdx;
                        ScrollToSelected();
                        InvalidateRect(m_hWnd, nullptr, FALSE);
                        UpdateWindow(m_hWnd);
                    }
                    return 0;
                }

                // Forward unhandled keys to the main window
                if (m_hOwner) return SendMessageW(m_hOwner, message, wParam, lParam);
                break;
            }

            // -----------------------------------------------------------------
            case WM_LBUTTONDOWN: {
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
                if (s_dragging) {
                    ReleaseCapture();
                    if (!s_hasMoved) {
                        int x = GET_X_LPARAM(lParam);
                        int y = GET_Y_LPARAM(lParam);
                        for (size_t i = 0; i < m_thumbnails.size(); ++i) {
                            if (m_thumbnails[i].HitTest(x, y)) {
                                LoadImageIndex(m_hOwner, m_thumbnails[i].playlistIndex);
                                break;
                            }
                        }
                    }
                    s_dragging = false;
                }
                return 0;
            }

            // -----------------------------------------------------------------
            case WM_CLOSE:
                ShowWindow(m_hWnd, SW_HIDE);
                return 0;
        }

        return DefWindowProcW(m_hWnd, message, wParam, lParam);
    }

    // =========================================================================
    // Unified Renderer Calls
    // =========================================================================
    void ThumbnailPanelWnd::Render(int selectedIdx, int hoverIdx) {
        if (!app.renderer) return;
        auto *r = dynamic_cast<RendererD2D *>(app.renderer.get());
        if (r && r->GetPanelContext(GetPanelType())) {
            // Pass the protected m_thumbnails vector directly
            r->RenderPanel(GetPanelType(), selectedIdx, hoverIdx, m_thumbnails);
        }
    }

    void ThumbnailPanelWnd::ResizeSwapChain(UINT w, UINT h) {
        if (!app.renderer) return;
        auto *r = dynamic_cast<RendererD2D *>(app.renderer.get());
        if (r) {
            r->ResizePanel(GetPanelType(), w, h);
        }
    }

    void ThumbnailPanelWnd::CreateDeviceResources() {
        if (!app.renderer) return;
        auto *r = dynamic_cast<RendererD2D *>(app.renderer.get());
        if (r) {
            r->CreatePanelDeviceResources(GetPanelType(), m_hWnd);
        }
    }
} // namespace UI
