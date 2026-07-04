#include "DirWnd.h"
#include <algorithm>
#include <filesystem>
#include "../AppState.h"
#include "../Platform/FileHandler.h"
#include "../Platform/Constants.h"
#include "../Renderer/RendererD2D.h"
#include "../Input/Shortcuts.h"
#include <windowsx.h>

// ---------------------------------------------------------------------------
// DirWnd.cpp  —  Current-folder image browser panel.
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
    static HWND g_hDirOwner  = nullptr;
    static int  g_selectedIdx = -1;
    static int  g_hoverIdx    = -1;
    static bool g_isDragging  = false;
    static bool g_hasMoved    = false;
    static POINT g_lastMouse  = {0, 0};
    static POINT g_clickPos   = {0, 0};
    static int8_t g_dirPosition = Constants::CURRENT_DIR_WINDOW_POSITION;

    float g_dirOffset = 0.0f;
    std::vector<Thumbnail> g_dirThumbnailObjects;

    // -------------------------------------------------------------------------
    // Geometry helpers
    // -------------------------------------------------------------------------
    static void GetDirWindowBounds(HWND hRef, int8_t position, int &x, int &y, int &w, int &h) {
        HMONITOR hMonitor = MonitorFromWindow(hRef, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = {sizeof(mi)};
        GetMonitorInfoW(hMonitor, &mi);

        int monX = mi.rcMonitor.left;
        int monY = mi.rcMonitor.top;
        int monW = mi.rcMonitor.right  - mi.rcMonitor.left;
        int monH = mi.rcMonitor.bottom - mi.rcMonitor.top;

        int horzThick = static_cast<int>(
            (Constants::CACHE_THUMB_HEIGHT + Constants::CACHE_THUMB_MARGIN * 2.0f) * app.dpiScale);
        int vertThick = static_cast<int>(
            (Constants::CACHE_THUMB_WIDTH  + Constants::CACHE_THUMB_MARGIN * 2.0f) * app.dpiScale);

        switch (position) {
            case 0: { // centered floating — 80% of monitor width, thumb-height tall
                int panelW = static_cast<int>(monW * 0.80f);
                int panelH = horzThick;
                x = monX + (monW - panelW) / 2;
                y = monY + (monH - panelH) / 2;
                w = panelW;
                h = panelH;
                break;
            }
            case 1: // top
                x = monX; y = monY;
                w = monW; h = horzThick;
                break;
            case 2: // right
                x = monX + monW - vertThick; y = monY;
                w = vertThick; h = monH;
                break;
            case 3: // bottom
                x = monX; y = monY + monH - horzThick;
                w = monW; h = horzThick;
                break;
            case 4: // left
                x = monX; y = monY;
                w = vertThick; h = monH;
                break;
            default:
                x = monX; y = monY;
                w = monW; h = horzThick;
                break;
        }
    }

    // -------------------------------------------------------------------------
    // SyncDirSelectionRectangle
    // -------------------------------------------------------------------------
    void DirWnd::SyncDirSelectionRectangle() {
        if (!m_hWnd) return;

        g_selectedIdx = -1;
        for (size_t i = 0; i < g_dirThumbnailObjects.size(); ++i) {
            if (g_dirThumbnailObjects[i].playlistIndex == app.currentIndex) {
                g_selectedIdx = static_cast<int>(i);
                break;
            }
        }
        ScrollDirViewToSelected();
        InvalidateRect(m_hWnd, nullptr, TRUE);
        UpdateWindow(m_hWnd);
    }

    // -------------------------------------------------------------------------
    // ClearDirThumbnailCache
    // -------------------------------------------------------------------------
    void DirWnd::ClearDirThumbnailCache() {
        if (!app.renderer) return;
        auto *r = dynamic_cast<RendererD2D *>(app.renderer.get());
        if (r) r->ClearDirThumbnailCache();
    }

    // -------------------------------------------------------------------------
    // UpdateDirView
    // -------------------------------------------------------------------------
    void DirWnd::UpdateDirView() {
        if (!m_hWnd || !app.renderer || !IsWindowVisible(m_hWnd)) return;

        g_dirThumbnailObjects.clear();

        if (app.playlist.empty() || app.currentIndex < 0) {
            InvalidateRect(m_hWnd, nullptr, TRUE);
            return;
        }

        RECT cr{};
        GetClientRect(m_hWnd, &cr);

        float surfaceW = static_cast<float>(cr.right);
        float surfaceH = static_cast<float>(cr.bottom);
        bool vertical  = (g_dirPosition == 2 || g_dirPosition == 4);

        float thumbW        = Constants::CACHE_THUMB_WIDTH   * app.dpiScale;
        float thumbH        = Constants::CACHE_THUMB_HEIGHT  * app.dpiScale;
        float scaledMargin  = Constants::CACHE_THUMB_MARGIN  * app.dpiScale;
        float scaledSpacing = Constants::CACHE_THUMB_SPACING * app.dpiScale;

        const std::vector<std::wstring> &items = app.playlist;
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
            else          x += thumbW + scaledSpacing;
        }

        SyncDirSelectionRectangle();

        auto *r = dynamic_cast<RendererD2D *>(app.renderer.get());
        if (r) {
            for (const auto &obj : g_dirThumbnailObjects) {
                r->RequestDirThumbnail(obj.filePath);
            }
        }
    }

    // -------------------------------------------------------------------------
    // Window procedure  (routes via IPanelWindow::WindowRouter → m_hWnd)
    // -------------------------------------------------------------------------
    LRESULT DirWnd::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
            case WM_PAINT: {
                PAINTSTRUCT ps;
                BeginPaint(m_hWnd, &ps);
                if (app.renderer) {
                    auto *r = dynamic_cast<RendererD2D *>(app.renderer.get());
                    if (r && r->GetDirContext()) {
                        r->RenderDirWindow(g_selectedIdx, g_hoverIdx);
                    }
                }
                EndPaint(m_hWnd, &ps);
                return 0;
            }

            case WM_SIZE: {
                if (app.renderer) {
                    auto *r = dynamic_cast<RendererD2D *>(app.renderer.get());
                    if (r) {
                        UINT w = LOWORD(lParam);
                        UINT h = HIWORD(lParam);
                        r->ResizeDirWindow(w, h);
                    }
                }
                UpdateDirView();
                return 0;
            }

            case WM_SETFOCUS:
            case WM_KILLFOCUS:
                return 0;

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

            case WM_KEYDOWN: {
                switch (wParam) {
                    case Shortcuts::SC_LOCAL_HIDE:
                        ShowWindow(m_hWnd, SW_HIDE);
                        return 0;
                    case Shortcuts::SC_PANEL_DIR_TOGGLE:
                        ToggleDirWindow();
                        return 0;
                    case Shortcuts::SC_PANEL_DIR_MOVE:
                        MoveDirWindow();
                        return 0;

                    case VK_UP: {
                        if (!g_dirThumbnailObjects.empty()) {
                            g_selectedIdx = (g_selectedIdx > 0)
                                ? g_selectedIdx - 1
                                : static_cast<int>(g_dirThumbnailObjects.size()) - 1;
                            g_hoverIdx = g_selectedIdx;
                            ScrollDirViewToSelected();
                            InvalidateRect(m_hWnd, nullptr, FALSE);
                            UpdateWindow(m_hWnd);
                        }
                        return 0;
                    }
                    case VK_DOWN: {
                        if (!g_dirThumbnailObjects.empty()) {
                            g_selectedIdx = (g_selectedIdx < static_cast<int>(g_dirThumbnailObjects.size()) - 1)
                                ? g_selectedIdx + 1
                                : 0;
                            g_hoverIdx = g_selectedIdx;
                            ScrollDirViewToSelected();
                            InvalidateRect(m_hWnd, nullptr, FALSE);
                            UpdateWindow(m_hWnd);
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

            case WM_LBUTTONDOWN: {
                g_clickPos.x = GET_X_LPARAM(lParam);
                g_clickPos.y = GET_Y_LPARAM(lParam);
                g_hasMoved   = false;
                g_isDragging = true;
                SetCapture(m_hWnd);
                GetCursorPos(&g_lastMouse);
                return 0;
            }

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
                    InvalidateRect(m_hWnd, nullptr, FALSE);
                }

                if (g_isDragging) {
                    if (abs(x - g_clickPos.x) > 5 || abs(y - g_clickPos.y) > 5) {
                        g_hasMoved = true;
                    }

                    POINT cur;
                    GetCursorPos(&cur);
                    float delta;
                    if (g_dirPosition == 2 || g_dirPosition == 4) {
                        delta = static_cast<float>(cur.y - g_lastMouse.y);
                    } else {
                        delta = static_cast<float>(cur.x - g_lastMouse.x);
                    }
                    g_dirOffset += delta;
                    g_lastMouse  = cur;
                    UpdateDirView();
                }
                return 0;
            }

            case WM_LBUTTONUP: {
                if (g_isDragging) {
                    ReleaseCapture();
                    if (!g_hasMoved) {
                        int x = GET_X_LPARAM(lParam);
                        int y = GET_Y_LPARAM(lParam);
                        for (size_t i = 0; i < g_dirThumbnailObjects.size(); ++i) {
                            if (g_dirThumbnailObjects[i].HitTest(x, y)) {
                                LoadImageIndex(g_hDirOwner, g_dirThumbnailObjects[i].playlistIndex);
                                break;
                            }
                        }
                    }
                    g_isDragging = false;
                }
                return 0;
            }

            case WM_CLOSE:
                ShowWindow(m_hWnd, SW_HIDE);
                return 0;
        }

        return DefWindowProcW(m_hWnd, message, wParam, lParam);
    }

    // -------------------------------------------------------------------------
    // Public API
    // -------------------------------------------------------------------------
    void DirWnd::Init(HINSTANCE hInstance, HWND hParent, int8_t position) {
        g_dirPosition = position;
        Init(hInstance, hParent);
    }

    void DirWnd::Init(HINSTANCE hInstance, HWND hParent) {
        g_hDirOwner = hParent;

        WNDCLASSW wc{};
        wc.style         = CS_DBLCLKS;
        wc.lpfnWndProc   = IPanelWindow::WindowRouter;
        wc.hInstance     = hInstance;
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = L"QIV_DirWindow";
        RegisterClassW(&wc);

        int x, y, w, h;
        GetDirWindowBounds(hParent, g_dirPosition, x, y, w, h);

        CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
            wc.lpszClassName,
            L"Directory",
            WS_POPUP,
            x, y, w, h,
            hParent, nullptr, hInstance,
            this   // passed to WM_NCCREATE so WindowRouter can store the this-ptr
        );

        if (!m_hWnd) return;

        SetLayeredWindowAttributes(m_hWnd, 0, Constants::CACHE_WINDOW_OPACITY, LWA_ALPHA);

        if (app.renderer) {
            auto *r = dynamic_cast<RendererD2D *>(app.renderer.get());
            if (r) {
                r->CreateDirWindowDeviceResources(m_hWnd);
            }
        }

        ShowWindow(m_hWnd, SW_HIDE);
    }

    void DirWnd::ToggleDirWindow() {
        if (!m_hWnd) return;

        if (IsWindowVisible(m_hWnd)) {
            ShowWindow(m_hWnd, SW_HIDE);
        } else {
            ShowWindow(m_hWnd, SW_SHOW);
            SetWindowPos(m_hWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
            SetForegroundWindow(m_hWnd);
            SetFocus(m_hWnd);
            UpdateDirView();
        }
    }

    void DirWnd::MoveDirWindow() {
        if (!m_hWnd) return;
        g_dirPosition++;
        if (g_dirPosition > 4) g_dirPosition = 0;

        int x, y, w, h;
        GetDirWindowBounds(g_hDirOwner ? g_hDirOwner : m_hWnd, g_dirPosition, x, y, w, h);
        g_dirOffset = 0.0f;

        SetWindowPos(
            m_hWnd, HWND_TOPMOST,
            x, y, w, h,
            SWP_SHOWWINDOW | SWP_FRAMECHANGED
        );
    }

    void DirWnd::HideDirWindow() {
        if (m_hWnd && IsWindowVisible(m_hWnd))
            ShowWindow(m_hWnd, SW_HIDE);
    }

    // -------------------------------------------------------------------------
    // ScrollDirViewToSelected  (file-scope helper — no class access needed)
    // -------------------------------------------------------------------------
    static void ScrollDirViewToSelected() {
        if (g_selectedIdx < 0 || g_selectedIdx >= static_cast<int>(g_dirThumbnailObjects.size()))
            return;

        // We need the window handle — grab from the objects vector's owner context
        // by finding the first thumbnail's rect and backing-out the hwnd from the
        // global objects array. Simplest approach: forward-declare a free accessor.
        // Because m_hWnd is protected, we use the global g_hDirOwner sibling approach:
        // store the dir hwnd in a local static mirroring Init.
        // NOTE: We use the IPanelWindow-stored handle via the DirWnd instance, but
        // since this is a file-scope static we read from the object. The cleanest
        // way inside this file is to keep a module-level copy of the dir HWND.
        // We pull it from the already-visible window via FindWindowW as a fallback,
        // but the real fix is a module-level g_hDirWnd mirror kept in Init.
        // For now use FindWindowW — zero overhead since it's called only on nav events.
        HWND hDirWnd = FindWindowW(L"QIV_DirWindow", nullptr);
        if (!hDirWnd) return;

        RECT cr{};
        GetClientRect(hDirWnd, &cr);
        float surfaceW = static_cast<float>(cr.right);
        float surfaceH = static_cast<float>(cr.bottom);
        bool vertical  = (g_dirPosition == 2 || g_dirPosition == 4);

        float thumbW        = Constants::CACHE_THUMB_WIDTH   * app.dpiScale;
        float thumbH        = Constants::CACHE_THUMB_HEIGHT  * app.dpiScale;
        float scaledSpacing = Constants::CACHE_THUMB_SPACING * app.dpiScale;
        float scaledMargin  = Constants::CACHE_THUMB_MARGIN  * app.dpiScale;

        if (!vertical) {
            float slotX = static_cast<float>(g_selectedIdx) * (thumbW + scaledSpacing);
            float visL  = -g_dirOffset + scaledMargin;
            float visR  = visL + surfaceW - scaledMargin * 2.0f;
            if (slotX < visL)
                g_dirOffset = -(slotX - scaledMargin);
            else if (slotX + thumbW > visR)
                g_dirOffset = -(slotX + thumbW - surfaceW + scaledMargin);
        } else {
            float slotY = static_cast<float>(g_selectedIdx) * (thumbH + scaledSpacing);
            float visT  = -g_dirOffset + scaledMargin;
            float visB  = visT + surfaceH - scaledMargin * 2.0f;
            if (slotY < visT)
                g_dirOffset = -(slotY - scaledMargin);
            else if (slotY + thumbH > visB)
                g_dirOffset = -(slotY + thumbH - surfaceH + scaledMargin);
        }
        InvalidateRect(hDirWnd, nullptr, FALSE);
    }

} // namespace UI
