#include "DirWindow.h"
#include <algorithm>
#include <filesystem>
#include "../AppState.h"
#include "../Platform/FileHandler.h"
#include "../Platform/Constants.h"
#include "../Renderer/RendererD2D.h"
#include "../Input/Shortcuts.h"
#include <windowsx.h>

// ---------------------------------------------------------------------------
// DirWindow.cpp  —  Current-folder image browser panel.
//
// Architecture mirrors CacheWindow exactly:
//   - WS_POPUP | WS_EX_TOPMOST | WS_EX_LAYERED — same creation flags
//   - Own DXGI swap chain inside RendererD2D (m_pDirSwapChain / m_pDirDeviceContext)
//   - UpdateDirView()  rebuilds g_dirThumbnailObjects from the file-system
//   - RenderDirWindow() in RendererD2D reads that vector and draws thumbnails
//
// Position slots (g_dirPosition):
//   0 = centered floating (the default / "home" position)
//   1 = top edge strip
//   2 = right edge strip
//   3 = bottom edge strip
//   4 = left edge strip
// ---------------------------------------------------------------------------

namespace UI {
    static void ScrollDirViewToSelected();

    // -------------------------------------------------------------------------
    // File-scope state
    // -------------------------------------------------------------------------
    static HWND g_hDirWnd = nullptr;
    static HWND g_hDirOwner = nullptr;
    static int g_selectedIdx = -1;
    static int g_hoverIdx = -1;
    static bool g_isDragging = false;
    static bool g_hasMoved = false;
    static POINT g_lastMouse = {0, 0};
    static POINT g_clickPos = {0, 0};
    static int8_t g_dirPosition = Constants::CURRENT_DIR_WINDOW_POSITION; // 0 = centered

    float g_dirOffset = 0.0f;
    std::vector<DirThumbnail> g_dirThumbnailObjects;

    // -------------------------------------------------------------------------
    // Geometry helpers
    // -------------------------------------------------------------------------

    // Returns the window rectangle for the given position slot.
    //   position 0 : centered floating panel (80 % wide, thumb-height tall)
    //   position 1 : top edge strip (full width)
    //   position 2 : right edge strip (full height)
    //   position 3 : bottom edge strip (full width)
    //   position 4 : left edge strip (full height)
    static void GetDirWindowBounds(HWND hRef, int8_t position, int &x, int &y, int &w, int &h) {
        HMONITOR hMonitor = MonitorFromWindow(hRef, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = {sizeof(mi)};
        GetMonitorInfoW(hMonitor, &mi);

        int monX = mi.rcMonitor.left;
        int monY = mi.rcMonitor.top;
        int monW = mi.rcMonitor.right - mi.rcMonitor.left;
        int monH = mi.rcMonitor.bottom - mi.rcMonitor.top;

        // Thickness for a horizontal strip (thumb height + margins on both sides)
        int horzThick = static_cast<int>(
            (Constants::CACHE_THUMB_HEIGHT + Constants::CACHE_THUMB_MARGIN * 2.0f)
            * g_app.dpiScale);
        // Thickness for a vertical strip (thumb width + margins on both sides)
        int vertThick = static_cast<int>(
            (Constants::CACHE_THUMB_WIDTH + Constants::CACHE_THUMB_MARGIN * 2.0f)
            * g_app.dpiScale);

        switch (position) {
            case 0: { // centered floating — 80 % of monitor width, thumb-height tall
                int panelW = static_cast<int>(monW * 0.80f);
                int panelH = horzThick;
                x = monX + (monW - panelW) / 2;
                y = monY + (monH - panelH) / 2;
                w = panelW;
                h = panelH;
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

    // -------------------------------------------------------------------------
    // SyncDirSelectionRectangle
    // -------------------------------------------------------------------------
    void SyncDirSelectionRectangle() {
        if (!g_hDirWnd) return;

        g_selectedIdx = -1;
        for (size_t i = 0; i < g_dirThumbnailObjects.size(); ++i) {
            if (g_dirThumbnailObjects[i].playlistIndex == g_app.currentIndex) {
                g_selectedIdx = static_cast<int>(i);
                break;
            }
        }
        InvalidateRect(g_hDirWnd, nullptr, TRUE);
        UpdateWindow(g_hDirWnd);
    }

    // -------------------------------------------------------------------------
    // UpdateDirView  —  Rebuild thumbnail geometry from the current folder
    // -------------------------------------------------------------------------
    void UpdateDirView() {
        if (!g_hDirWnd || !g_app.renderer || !IsWindowVisible(g_hDirWnd)) return;

        g_dirThumbnailObjects.clear();

        // Nothing loaded yet — nothing to show
        if (g_app.playlist.empty() || g_app.currentIndex < 0) {
            InvalidateRect(g_hDirWnd, nullptr, TRUE);
            return;
        }

        RECT cr{};
        GetClientRect(g_hDirWnd, &cr);

        float surfaceW = static_cast<float>(cr.right);
        float surfaceH = static_cast<float>(cr.bottom);
        bool vertical = (g_dirPosition == 2 || g_dirPosition == 4); // right or left

        float thumbW = Constants::CACHE_THUMB_WIDTH * g_app.dpiScale;
        float thumbH = Constants::CACHE_THUMB_HEIGHT * g_app.dpiScale;
        float scaledMargin = Constants::CACHE_THUMB_MARGIN * g_app.dpiScale;
        float scaledSpacing = Constants::CACHE_THUMB_SPACING * g_app.dpiScale;

        // Build the source list from the playlist (same folder = entire playlist)
        const std::vector<std::wstring> &items = g_app.playlist;
        size_t count = items.size();

        float x = scaledMargin;
        float y = scaledMargin;

        if (!vertical) {
            y = (surfaceH - thumbH) / 2.0f;
            float totalW = static_cast<float>(count) * (thumbW + scaledSpacing) - scaledSpacing;
            if (totalW <= surfaceW) {
                x = (surfaceW - totalW) / 2.0f;
            } else {
                float minOffset = surfaceW - totalW - scaledMargin;
                g_dirOffset = std::clamp(g_dirOffset, minOffset, 0.0f);
                x = scaledMargin + g_dirOffset;
            }
        } else {
            x = (surfaceW - thumbW) / 2.0f;
            float totalH = static_cast<float>(count) * (thumbH + scaledSpacing) - scaledSpacing;
            if (totalH <= surfaceH) {
                y = (surfaceH - totalH) / 2.0f;
            } else {
                float minOffset = surfaceH - totalH - scaledMargin;
                g_dirOffset = std::clamp(g_dirOffset, minOffset, 0.0f);
                y = scaledMargin + g_dirOffset;
            }
        }

        for (size_t i = 0; i < count; ++i) {
            g_dirThumbnailObjects.push_back({
                D2D1::RectF(x, y, x + thumbW, y + thumbH),
                items[i],
                static_cast<int>(i)
            });

            if (vertical) y += thumbH + scaledSpacing;
            else x += thumbW + scaledSpacing;
        }

        SyncDirSelectionRectangle();
    }

    // -------------------------------------------------------------------------
    // Window procedure
    // -------------------------------------------------------------------------
    static LRESULT CALLBACK DirWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
            // -----------------------------------------------------------------
            case WM_PAINT: {
                PAINTSTRUCT ps;
                BeginPaint(hWnd, &ps);
                if (g_app.renderer) {
                    auto *r = dynamic_cast<RendererD2D *>(g_app.renderer.get());
                    if (r && r->GetDirContext()) {
                        r->RenderDirWindow(g_selectedIdx, g_hoverIdx);
                    }
                }
                EndPaint(hWnd, &ps);
                return 0;
            }

            // -----------------------------------------------------------------
            case WM_SIZE: {
                if (g_app.renderer) {
                    auto *r = dynamic_cast<RendererD2D *>(g_app.renderer.get());
                    if (r) {
                        UINT w = LOWORD(lParam);
                        UINT h = HIWORD(lParam);
                        r->ResizeDirWindow(w, h);
                    }
                }
                UpdateDirView();
                return 0;
            }

            // -----------------------------------------------------------------
            case WM_MOUSEWHEEL: {
                int delta = GET_WHEEL_DELTA_WPARAM(wParam);
                float scroll = Constants::CACHE_WINDOW_MOUSE_WHEEL_SPEED;
                if (GetKeyState(VK_SHIFT) & 0x8000) scroll *= 3.0f;
                float amount = (delta > 0 ? scroll : -scroll)
                               * Constants::CACHE_WINDOW_MOUSE_WHEEL_DIRECTION;
                g_dirOffset += amount;
                UpdateDirView();
                return 0;
            }

            // -----------------------------------------------------------------
            case WM_KEYDOWN: {
                switch (wParam) {
                    case Shortcuts::SC_LOCAL_HIDE:
                        ShowWindow(hWnd, SW_HIDE);
                        return 0;
                    case Shortcuts::SC_PANEL_DIR_TOGGLE:
                        ToggleDirWindow();
                        return 0;
                    case Shortcuts::SC_PANEL_DIR_MOVE:
                        MoveDirWindow();
                        return 0;

                    case VK_UP:
                    case VK_LEFT: {
                        if (!g_dirThumbnailObjects.empty()) {
                            int newIdx = (g_selectedIdx > 0)
                                             ? g_selectedIdx - 1
                                             : static_cast<int>(g_dirThumbnailObjects.size()) - 1;
                            LoadImageIndex(g_hDirOwner, g_dirThumbnailObjects[newIdx].playlistIndex);
                            // SyncDirSelectionRectangle() will be called by the load path,
                            // but we also need to scroll the view so the thumb is visible.
                            ScrollDirViewToSelected();
                        }
                        return 0;
                    }

                    case VK_DOWN:
                    case VK_RIGHT: {
                        if (!g_dirThumbnailObjects.empty()) {
                            int newIdx = (g_selectedIdx < static_cast<int>(g_dirThumbnailObjects.size()) - 1)
                                             ? g_selectedIdx + 1
                                             : 0;
                            LoadImageIndex(g_hDirOwner, g_dirThumbnailObjects[newIdx].playlistIndex);
                            ScrollDirViewToSelected();
                        }
                        return 0;
                    }

                    default:
                        if (g_hDirOwner) {
                            return SendMessageW(g_hDirOwner, message, wParam, lParam);
                        }
                        break;
                }
                break;
            }

            // -----------------------------------------------------------------
            case WM_LBUTTONDOWN: {
                g_clickPos.x = GET_X_LPARAM(lParam);
                g_clickPos.y = GET_Y_LPARAM(lParam);
                g_hasMoved = false;
                g_isDragging = true;
                SetCapture(hWnd);
                GetCursorPos(&g_lastMouse);
                return 0;
            }

            // -----------------------------------------------------------------
            case WM_MOUSEMOVE: {
                int x = GET_X_LPARAM(lParam);
                int y = GET_Y_LPARAM(lParam);

                int newHover = -1;
                for (size_t i = 0; i < g_dirThumbnailObjects.size(); ++i) {
                    if (g_dirThumbnailObjects[i].HitTest(x, y)) {
                        newHover = static_cast<int>(i);
                        break;
                    }
                }
                if (newHover != g_hoverIdx) {
                    g_hoverIdx = newHover;
                    InvalidateRect(hWnd, nullptr, FALSE);
                }

                if (g_isDragging) {
                    if (abs(x - g_clickPos.x) > 5 || abs(y - g_clickPos.y) > 5) {
                        g_hasMoved = true;
                    }

                    POINT cur;
                    GetCursorPos(&cur);

                    float delta;
                    if (g_dirPosition == 2 || g_dirPosition == 4) { // vertical strips
                        delta = static_cast<float>(cur.y - g_lastMouse.y);
                    } else {
                        delta = static_cast<float>(cur.x - g_lastMouse.x);
                    }
                    g_dirOffset += delta;
                    g_lastMouse = cur;
                    UpdateDirView();
                }
                return 0;
            }

            // -----------------------------------------------------------------
            case WM_LBUTTONUP: {
                if (g_isDragging) {
                    ReleaseCapture();

                    if (!g_hasMoved) {
                        int x = GET_X_LPARAM(lParam);
                        int y = GET_Y_LPARAM(lParam);
                        for (size_t i = 0; i < g_dirThumbnailObjects.size(); ++i) {
                            if (g_dirThumbnailObjects[i].HitTest(x, y)) {
                                // Navigate to the clicked file
                                LoadImageIndex(g_hDirOwner, g_dirThumbnailObjects[i].playlistIndex);
                                break;
                            }
                        }
                    }
                    g_isDragging = false;
                }
                return 0;
            }

            // -----------------------------------------------------------------
            case WM_CLOSE:
                ShowWindow(hWnd, SW_HIDE);
                return 0;
        }

        return DefWindowProcW(hWnd, message, wParam, lParam);
    }

    // -------------------------------------------------------------------------
    // Public API
    // -------------------------------------------------------------------------

    void InitDirWindow(HINSTANCE hInstance, HWND hParent) {
        g_hDirOwner = hParent;

        WNDCLASSW wc{};
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = DirWndProc;
        wc.hInstance = hInstance;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = L"QIV_DirWindow";
        RegisterClassW(&wc);

        int x, y, w, h;
        GetDirWindowBounds(hParent, g_dirPosition, x, y, w, h);

        g_hDirWnd = CreateWindowExW(
                WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
                wc.lpszClassName,
                L"Directory",
                WS_POPUP,
                x, y, w, h,
                hParent, nullptr, hInstance, nullptr
                );

        if (!g_hDirWnd) return;

        SetLayeredWindowAttributes(g_hDirWnd, 0, Constants::CACHE_WINDOW_OPACITY, LWA_ALPHA);

        if (g_app.renderer) {
            auto *r = dynamic_cast<RendererD2D *>(g_app.renderer.get());
            if (r) {
                r->CreateDirWindowDeviceResources(g_hDirWnd);
            }
        }

        ShowWindow(g_hDirWnd, SW_HIDE);
    }

    void ToggleDirWindow() {
        if (!g_hDirWnd) return;

        if (IsWindowVisible(g_hDirWnd)) {
            ShowWindow(g_hDirWnd, SW_HIDE);
        } else {
            g_dirOffset = 0.0f;
            ShowWindow(g_hDirWnd, SW_SHOW);
            SetForegroundWindow(g_hDirWnd);
            SetForegroundWindow(g_hDirWnd); // Brings to front
            SetFocus(g_hDirWnd); // Forces keyboard focus
            UpdateDirView();
        }
    }

    void MoveDirWindow() {
        if (!g_hDirWnd) return;
        g_dirPosition++;
        if (g_dirPosition > 4) g_dirPosition = 0; // 5 slots: 0-4

        int x, y, w, h;
        GetDirWindowBounds(g_hDirOwner ? g_hDirOwner : g_hDirWnd, g_dirPosition, x, y, w, h);
        g_dirOffset = 0.0f;

        SetWindowPos(
                g_hDirWnd,
                HWND_TOPMOST,
                x, y, w, h,
                SWP_SHOWWINDOW | SWP_FRAMECHANGED
                );
    }

    static void ScrollDirViewToSelected() {
        if (g_selectedIdx < 0 || g_selectedIdx >= static_cast<int>(g_dirThumbnailObjects.size()))
            return;

        RECT cr{};
        GetClientRect(g_hDirWnd, &cr);
        float surfaceW = static_cast<float>(cr.right);
        float surfaceH = static_cast<float>(cr.bottom);
        bool vertical = (g_dirPosition == 2 || g_dirPosition == 4);

        float thumbW = Constants::CACHE_THUMB_WIDTH * g_app.dpiScale;
        float thumbH = Constants::CACHE_THUMB_HEIGHT * g_app.dpiScale;
        float scaledSpacing = Constants::CACHE_THUMB_SPACING * g_app.dpiScale;
        float scaledMargin = Constants::CACHE_THUMB_MARGIN * g_app.dpiScale;

        if (!vertical) {
            // Horizontal layout: bring thumb into [margin .. surfaceW - margin - thumbW]
            float slotX = static_cast<float>(g_selectedIdx) * (thumbW + scaledSpacing);
            float visL = -g_dirOffset + scaledMargin;
            float visR = visL + surfaceW - scaledMargin * 2.0f;
            if (slotX < visL)
                g_dirOffset = -(slotX - scaledMargin);
            else if (slotX + thumbW > visR)
                g_dirOffset = -(slotX + thumbW - surfaceW + scaledMargin);
        } else {
            // Vertical layout
            float slotY = static_cast<float>(g_selectedIdx) * (thumbH + scaledSpacing);
            float visT = -g_dirOffset + scaledMargin;
            float visB = visT + surfaceH - scaledMargin * 2.0f;
            if (slotY < visT)
                g_dirOffset = -(slotY - scaledMargin);
            else if (slotY + thumbH > visB)
                g_dirOffset = -(slotY + thumbH - surfaceH + scaledMargin);
        }
        UpdateDirView();
    }
} // namespace UI
