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

    void UpdateCacheView() {
        if (!g_hCacheWnd) return;

        g_thumbnailObjects.clear();

        if (g_app.renderer) {
            auto items = g_app.renderer->GetCachedBitmaps();

            RECT cr;
            GetClientRect(g_hCacheWnd, &cr);

            float surfaceW = static_cast<float>(cr.right);

            // Centering logic used by both drawing and hit-testing
            float totalThumbWidth =
                    items.size() * (Constants::CACHE_THUMB_WIDTH + Constants::CACHE_THUMB_SPACING)
                    - Constants::CACHE_THUMB_SPACING;

            float x;

            if (totalThumbWidth <= surfaceW) {
                // center when everything fits
                x = (surfaceW - totalThumbWidth) / 2.0f;
            } else {
                // scrolling mode
                float maxOffset = 0.0f;

                float minOffset =
                        surfaceW - totalThumbWidth - Constants::CACHE_MARGIN;
                if (g_cacheOffset > maxOffset)
                    g_cacheOffset = maxOffset;
                if (g_cacheOffset < minOffset)
                    g_cacheOffset = minOffset;
                x = Constants::CACHE_MARGIN + g_cacheOffset;
            }

            float y = Constants::CACHE_MARGIN;


            for (const auto &item: items) {
                auto it = std::find(
                        g_app.playlist.begin(),
                        g_app.playlist.end(),
                        item.filePath);

                int idx = (it != g_app.playlist.end())
                              ? static_cast<int>(std::distance(g_app.playlist.begin(), it))
                              : -1;


                g_thumbnailObjects.push_back({
                    D2D1::RectF(
                            x,
                            y,
                            x + Constants::CACHE_THUMB_WIDTH,
                            y + Constants::CACHE_THUMB_HEIGHT),
                    item.filePath,
                    idx
                });


                x += Constants::CACHE_THUMB_WIDTH + Constants::CACHE_THUMB_SPACING;
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
                    // Use the new getter method instead of direct private member access
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
                if (GetKeyState(VK_SHIFT) & 0x8000)
                    scroll *= 3.0f;
                g_cacheOffset += (delta > 0 ? scroll : -scroll) * Constants::CACHE_WINDOW_MOUSE_WHEEL_DIRECTION;
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
                    float delta = static_cast<float>(
                        cur.x - g_lastMousePos.x);
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
            case WM_SIZE: UpdateCacheView();
                return 0;
            case WM_KEYDOWN: {
                if (wParam == VK_ESCAPE) {
                    ShowWindow(hWnd, SW_HIDE);
                }
                // Add this to handle F3 while CacheWindow is focused
                else if (wParam == VK_F3) {
                    ToggleCacheWindow();
                }
                return 0;
            }
            case WM_CLOSE: ShowWindow(hWnd, SW_HIDE);
                return 0;
        }
        return DefWindowProcW(hWnd, message, wParam, lParam);
    }

    void InitCacheWindow(HINSTANCE hInstance, HWND hParent) {
        g_hOwner = hParent;

        WNDCLASSW wc = {0};
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = CacheWndProc;
        wc.hInstance = hInstance;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = L"QIV_CacheWindow";
        RegisterClassW(&wc);

        // Get primary monitor width
        int screenW = GetSystemMetrics(SM_CXSCREEN);
        int filmstripHeight = 240; // Your desired height

        g_hCacheWnd = CreateWindowExW(
                WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
                wc.lpszClassName,
                L"Cache",
                WS_POPUP, // Borderless
                0, 0, screenW, filmstripHeight, // X=0, Y=0, Width=ScreenW
                hParent, nullptr, hInstance, nullptr
                );

        if (g_hCacheWnd) {
            SetLayeredWindowAttributes(g_hCacheWnd, 0, Constants::CACHE_WINDOW_OPACITY, LWA_ALPHA);

            // Force the renderer link here so it's ready when shown
            if (g_app.renderer) {
                auto *r = dynamic_cast<RendererD2D *>(g_app.renderer.get());
                if (r) r->CreateCacheWindowDeviceResources(g_hCacheWnd);
            }
        }
    }

    void ToggleCacheWindow() {
        if (!g_hCacheWnd) return;

        if (IsWindowVisible(g_hCacheWnd)) {
            ShowWindow(g_hCacheWnd, SW_HIDE);
        } else {
            // Force the window to show FIRST
            ShowWindow(g_hCacheWnd, SW_SHOW);
            SetForegroundWindow(g_hCacheWnd);

            // Update view once visible to ensure RECTs are calculated
            UpdateCacheView();

            // Trigger a repaint
            InvalidateRect(g_hCacheWnd, NULL, TRUE);
            UpdateWindow(g_hCacheWnd);
        }
    }
}
