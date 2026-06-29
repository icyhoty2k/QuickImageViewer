#include "CacheWindow.h"
#include "../AppState.h"
#include "../Platform/FileHandler.h"
#include <windowsx.h>
#include <algorithm>
#include "../Renderer/RendererD2D.h"
#include "Constants.h"

namespace UI {
    HWND g_hCacheWnd = nullptr;
    HWND g_hOwner = nullptr;
    int g_selectedIndex = -1;
    bool g_isDragging = false;
    POINT g_lastMousePos = {0, 0};
    int g_hoverIndex = -1;
    float g_cacheOffset = 0.0f;
    std::vector<Thumbnail> g_thumbnailObjects;
    int8_t g_cachePosition = Constants::CACHE_WINDOW_POSITION;

    void UpdateCacheView() {
        if (!g_hCacheWnd || !g_app.renderer) return;

        g_thumbnailObjects.clear();
        auto items = g_app.renderer->GetCachedBitmaps();

        RECT cr{};
        GetClientRect(g_hCacheWnd, &cr);

        float surfaceW = static_cast<float>(cr.right);
        float surfaceH = static_cast<float>(cr.bottom);

        bool vertical = (g_cachePosition == 2 || g_cachePosition == 3);

        // Standardize sizes using explicit DPI scaling rather than stretching to the window bounds
        float thumbW = Constants::CACHE_THUMB_WIDTH * g_app.dpiScale;
        float thumbH = Constants::CACHE_THUMB_HEIGHT * g_app.dpiScale;
        float scaledMargin = Constants::CACHE_THUMB_MARGIN * g_app.dpiScale;
        float scaledSpacing = Constants::CACHE_THUMB_SPACING * g_app.dpiScale;

        float x = scaledMargin;
        float y = scaledMargin;

        if (!vertical) {
            // Horizontal alignment (Top / Bottom)
            // Center the thumbnail vertically within the window thickness
            y = (surfaceH - thumbH) / 2.0f;

            float totalWidth = items.size() * (thumbW + scaledSpacing) - scaledSpacing;

            if (totalWidth <= surfaceW) {
                x = (surfaceW - totalWidth) / 2.0f;
            } else {
                float minOffset = surfaceW - totalWidth - scaledMargin;
                if (g_cacheOffset > 0) g_cacheOffset = 0;
                if (g_cacheOffset < minOffset) g_cacheOffset = minOffset;
                x = scaledMargin + g_cacheOffset;
            }
        } else {
            // Vertical alignment (Left / Right)
            // Center the thumbnail horizontally within the window thickness
            x = (surfaceW - thumbW) / 2.0f;

            float totalHeight = items.size() * (thumbH + scaledSpacing) - scaledSpacing;

            if (totalHeight <= surfaceH) {
                y = (surfaceH - totalHeight) / 2.0f;
            } else {
                float minOffset = surfaceH - totalHeight - scaledMargin;
                if (g_cacheOffset > 0) g_cacheOffset = 0;
                if (g_cacheOffset < minOffset) g_cacheOffset = minOffset;
                y = scaledMargin + g_cacheOffset;
            }
        }

        // Build thumbnail objects
        for (const auto &item: items) {
            auto it = std::find(g_app.playlist.begin(), g_app.playlist.end(), item.filePath);
            int idx = (it != g_app.playlist.end()) ? static_cast<int>(std::distance(g_app.playlist.begin(), it)) : -1;

            g_thumbnailObjects.push_back({
                D2D1::RectF(x, y, x + thumbW, y + thumbH),
                item.filePath,
                idx
            });

            if (vertical) {
                y += thumbH + scaledSpacing;
            } else {
                x += thumbW + scaledSpacing;
            }
        }

        InvalidateRect(g_hCacheWnd, nullptr, TRUE);
        UpdateWindow(g_hCacheWnd);
    }

    LRESULT CALLBACK CacheWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
            case WM_PAINT: {
                PAINTSTRUCT ps;
                BeginPaint(hWnd, &ps);
                if (g_app.renderer) {
                    auto *r = dynamic_cast<RendererD2D *>(g_app.renderer.get());
                    if (r && r->GetCacheContext()) {
                        r->RenderCacheWindow(g_selectedIndex, g_hoverIndex);
                    }
                }
                EndPaint(hWnd, &ps);
                return 0;
            }
            case WM_MOUSEWHEEL: {
                int delta = GET_WHEEL_DELTA_WPARAM(wParam);
                float scroll = Constants::CACHE_WINDOW_MOUSE_WHEEL_SPEED;
                if (GetKeyState(VK_SHIFT) & 0x8000) scroll *= 3.0f;
                float amount = (delta > 0 ? scroll : -scroll) * Constants::CACHE_WINDOW_MOUSE_WHEEL_DIRECTION;
                g_cacheOffset += amount;
                UpdateCacheView();
                return 0;
            }
            case WM_LBUTTONDOWN: {
                int x = GET_X_LPARAM(lParam);
                int y = GET_Y_LPARAM(lParam);

                for (size_t i = 0; i < g_thumbnailObjects.size(); ++i) {
                    if (g_thumbnailObjects[i].HitTest(x, y)) {
                        g_selectedIndex = static_cast<int>(i);
                        LoadImageIndex(g_hOwner, g_thumbnailObjects[i].playlistIndex);
                        break;
                    }
                }

                g_isDragging = true;
                SetCapture(hWnd);
                GetCursorPos(&g_lastMousePos);
                return 0;
            }
            case WM_MOUSEMOVE: {
                if (g_isDragging) {
                    POINT cur;
                    GetCursorPos(&cur);
                    float delta;

                    if (g_cachePosition == 2 || g_cachePosition == 3) {
                        delta = static_cast<float>(cur.y - g_lastMousePos.y);
                    } else {
                        delta = static_cast<float>(cur.x - g_lastMousePos.x);
                    }

                    g_cacheOffset += delta;
                    g_lastMousePos = cur;

                    UpdateCacheView();
                    InvalidateRect(hWnd, nullptr, FALSE);
                }
                return 0;
            }
            case WM_LBUTTONUP: {
                ReleaseCapture();
                g_isDragging = false;
                return 0;
            }
            case WM_SIZE: {
                if (g_app.renderer) {
                    auto *r = dynamic_cast<RendererD2D *>(g_app.renderer.get());
                    if (r) {
                        UINT w = LOWORD(lParam);
                        UINT h = HIWORD(lParam);
                        r->ResizeCacheWindow(w, h);
                    }
                }
                UpdateCacheView();
                return 0;
            }
            case WM_KEYDOWN: {
                if (wParam == VK_ESCAPE) {
                    ShowWindow(hWnd, SW_HIDE);
                    return 0;
                } else if (wParam == VK_F3) {
                    ToggleCacheWindow();
                    return 0;
                } else if (wParam == VK_F4) {
                    MoveCacheWindow();
                    return 0;
                }
                break;
            }
            case WM_CLOSE: {
                ShowWindow(hWnd, SW_HIDE);
                return 0;
            }
        }
        return DefWindowProcW(hWnd, message, wParam, lParam);
    }

    void InitCacheWindow(HINSTANCE hInstance, HWND hParent) {
        InitCacheWindow(hInstance, hParent, Constants::CACHE_WINDOW_POSITION);
    }

    void MoveCacheWindow() {
        if (!g_hCacheWnd) return;

        g_cachePosition++;
        if (g_cachePosition > 3) g_cachePosition = 0;

        HMONITOR hMonitor = MonitorFromWindow(g_hOwner ? g_hOwner : g_hCacheWnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = {sizeof(mi)};
        GetMonitorInfoW(hMonitor, &mi);

        int monX = mi.rcMonitor.left;
        int monY = mi.rcMonitor.top;
        int monW = mi.rcMonitor.right - mi.rcMonitor.left;
        int monH = mi.rcMonitor.bottom - mi.rcMonitor.top;

        // Dynamically calculate thickness based on thumbnail sizes to ensure a uniform margin
        int horzThickness = static_cast<int>((Constants::CACHE_THUMB_HEIGHT + (Constants::CACHE_THUMB_MARGIN * 2.0f)) * g_app.dpiScale);
        int vertThickness = static_cast<int>((Constants::CACHE_THUMB_WIDTH + (Constants::CACHE_THUMB_MARGIN * 2.0f)) * g_app.dpiScale);

        int x = monX;
        int y = monY;
        int width = monW;
        int height = horzThickness;

        switch (g_cachePosition) {
            case 0: // bottom
                y = monY + monH - horzThickness;
                height = horzThickness;
                break;
            case 1: // top
                y = monY;
                height = horzThickness;
                break;
            case 2: // left
                width = vertThickness;
                height = monH;
                break;
            case 3: // right
                x = monX + monW - vertThickness;
                width = vertThickness;
                height = monH;
                break;
        }

        g_cacheOffset = 0;

        SetWindowPos(
                g_hCacheWnd,
                HWND_TOPMOST,
                x, y,
                width, height,
                SWP_SHOWWINDOW | SWP_FRAMECHANGED
                );
    }

    void InitCacheWindow(HINSTANCE hInstance, HWND hParent, int8_t position) {
        g_cachePosition = position;
        g_hOwner = hParent;

        WNDCLASSW wc{};
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = CacheWndProc;
        wc.hInstance = hInstance;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = L"QIV_CacheWindow";

        RegisterClassW(&wc);

        HMONITOR hMonitor = MonitorFromWindow(hParent, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = {sizeof(mi)};
        GetMonitorInfoW(hMonitor, &mi);

        int monX = mi.rcMonitor.left;
        int monY = mi.rcMonitor.top;
        int monW = mi.rcMonitor.right - mi.rcMonitor.left;
        int monH = mi.rcMonitor.bottom - mi.rcMonitor.top;

        // Dynamically calculate thickness based on thumbnail sizes to ensure a uniform margin
        int horzThickness = static_cast<int>((Constants::CACHE_THUMB_HEIGHT + (Constants::CACHE_THUMB_MARGIN * 2.0f)) * g_app.dpiScale);
        int vertThickness = static_cast<int>((Constants::CACHE_THUMB_WIDTH + (Constants::CACHE_THUMB_MARGIN * 2.0f)) * g_app.dpiScale);

        int x = monX;
        int y = monY;
        int width = monW;
        int height = horzThickness;

        switch (position) {
            case 0: // bottom
                y = monY + monH - horzThickness;
                height = horzThickness;
                break;
            case 1: // top
                y = monY;
                height = horzThickness;
                break;
            case 2: // left
                width = vertThickness;
                height = monH;
                break;
            case 3: // right
                x = monX + monW - vertThickness;
                width = vertThickness;
                height = monH;
                break;
        }

        g_hCacheWnd = CreateWindowExW(
                WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
                wc.lpszClassName,
                L"Cache",
                WS_POPUP,
                x, y, width, height,
                hParent, nullptr, hInstance, nullptr
                );

        if (!g_hCacheWnd) return;

        SetLayeredWindowAttributes(g_hCacheWnd, 0, Constants::CACHE_WINDOW_OPACITY, LWA_ALPHA);

        if (g_app.renderer) {
            auto *r = dynamic_cast<RendererD2D *>(g_app.renderer.get());
            if (r) {
                r->CreateCacheWindowDeviceResources(g_hCacheWnd);
            }
        }

        ShowWindow(g_hCacheWnd, SW_HIDE);
    }

    void ToggleCacheWindow() {
        if (!g_hCacheWnd) return;

        if (IsWindowVisible(g_hCacheWnd)) {
            ShowWindow(g_hCacheWnd, SW_HIDE);
        } else {
            ShowWindow(g_hCacheWnd, SW_SHOW);
            SetForegroundWindow(g_hCacheWnd);
            UpdateCacheView();
            InvalidateRect(g_hCacheWnd, NULL, TRUE);
            UpdateWindow(g_hCacheWnd);
        }
    }
}
