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
        if (!g_hCacheWnd) return;

        g_thumbnailObjects.clear();

        if (!g_app.renderer)
            return;


        auto items = g_app.renderer->GetCachedBitmaps();


        RECT cr{};
        GetClientRect(g_hCacheWnd, &cr);


        float surfaceW = static_cast<float>(cr.right);
        float surfaceH = static_cast<float>(cr.bottom);


        bool vertical =
                g_cachePosition == 2 ||
                g_cachePosition == 3;


        //
        // Calculate thumbnail size once
        //
        float thumbW = Constants::CACHE_THUMB_WIDTH;
        float thumbH = Constants::CACHE_THUMB_HEIGHT;


        if (vertical) {
            thumbW = surfaceW -
                     (Constants::CACHE_MARGIN * 2.0f);


            float aspect =
                    Constants::CACHE_THUMB_HEIGHT /
                    Constants::CACHE_THUMB_WIDTH;


            thumbH = thumbW * aspect;
        }


        float x = Constants::CACHE_MARGIN;
        float y = Constants::CACHE_MARGIN;


        //
        // Calculate scrolling / centering
        //
        if (!vertical) {
            float totalWidth =
                    items.size() *
                    (thumbW + Constants::CACHE_THUMB_SPACING)
                    -
                    Constants::CACHE_THUMB_SPACING;


            if (totalWidth <= surfaceW) {
                x = (surfaceW - totalWidth) / 2.0f;
            } else {
                float minOffset =
                        surfaceW -
                        totalWidth -
                        Constants::CACHE_MARGIN;


                if (g_cacheOffset > 0)
                    g_cacheOffset = 0;


                if (g_cacheOffset < minOffset)
                    g_cacheOffset = minOffset;


                x = Constants::CACHE_MARGIN +
                    g_cacheOffset;
            }
        } else {
            float totalHeight =
                    items.size() *
                    (thumbH + Constants::CACHE_THUMB_SPACING)
                    -
                    Constants::CACHE_THUMB_SPACING;


            if (totalHeight <= surfaceH) {
                y = (surfaceH - totalHeight) / 2.0f;
            } else {
                float minOffset =
                        surfaceH -
                        totalHeight -
                        Constants::CACHE_MARGIN;


                if (g_cacheOffset > 0)
                    g_cacheOffset = 0;


                if (g_cacheOffset < minOffset)
                    g_cacheOffset = minOffset;


                y = Constants::CACHE_MARGIN +
                    g_cacheOffset;
            }
        }


        //
        // Build thumbnail objects
        //
        for (const auto &item: items) {
            auto it = std::find(
                    g_app.playlist.begin(),
                    g_app.playlist.end(),
                    item.filePath);


            int idx =
                    (it != g_app.playlist.end())
                        ? static_cast<int>(
                            std::distance(
                                    g_app.playlist.begin(),
                                    it))
                        : -1;


            g_thumbnailObjects.push_back({

                D2D1::RectF(
                        x,
                        y,
                        x + thumbW,
                        y + thumbH),

                item.filePath,

                idx

            });


            if (vertical) {
                y += thumbH +
                        Constants::CACHE_THUMB_SPACING;
            } else {
                x += thumbW +
                        Constants::CACHE_THUMB_SPACING;
            }
        }


        InvalidateRect(
                g_hCacheWnd,
                nullptr,
                TRUE);

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

                float amount =
                        (delta > 0 ? scroll : -scroll) *
                        Constants::CACHE_WINDOW_MOUSE_WHEEL_DIRECTION;

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
                        // left/right = vertical scrolling
                        delta = static_cast<float>(cur.y - g_lastMousePos.y);
                    } else {
                        // top/bottom = horizontal scrolling
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
            case WM_SIZE: UpdateCacheView();
                return 0;
            case WM_KEYDOWN: {
                if (wParam == VK_ESCAPE) {
                    ShowWindow(hWnd, SW_HIDE);
                }
                // Add this to handle F3 while CacheWindow is focused
                else if (wParam == VK_F3) {
                    ToggleCacheWindow();
                } else if (wParam == VK_F4) {
                    MoveCacheWindow();
                }
                return 0;
            }
            case WM_CLOSE: ShowWindow(hWnd, SW_HIDE);
                return 0;
        }

        return
                DefWindowProcW(hWnd, message, wParam, lParam);
    }

    void InitCacheWindow(HINSTANCE hInstance, HWND hParent) {
        InitCacheWindow(hInstance, hParent, Constants::CACHE_WINDOW_POSITION);
    };

    void MoveCacheWindow() {
        if (!g_hCacheWnd)
            return;


        g_cachePosition++;


        if (g_cachePosition > 3)
            g_cachePosition = 0;


        int screenW =
                GetSystemMetrics(SM_CXSCREEN);

        int screenH =
                GetSystemMetrics(SM_CYSCREEN);


        int thickness =
                static_cast<int>(
                    Constants::CACHE_WINDOW_THICKNESS *
                    g_app.dpiScale);


        int x = 0;
        int y = 0;

        int width = screenW;
        int height = thickness;


        switch (g_cachePosition) {
            case 0: // bottom

                y = screenH - height;

                break;


            case 1: // top

                y = 0;

                break;


            case 2: // left

                width = thickness;
                height = screenH;

                break;


            case 3: // right

                x = screenW - thickness;

                width = thickness;
                height = screenH;

                break;
        }


        SetWindowPos(

                g_hCacheWnd,

                HWND_TOPMOST,

                x,
                y,

                width,
                height,

                SWP_SHOWWINDOW |
                SWP_FRAMECHANGED
                );


        RECT rc{};

        GetClientRect(
                g_hCacheWnd,
                &rc);


        UINT clientW =
                static_cast<UINT>(
                    rc.right - rc.left);


        UINT clientH =
                static_cast<UINT>(
                    rc.bottom - rc.top);


        if (g_app.renderer) {
            auto *r =
                    dynamic_cast<RendererD2D *>(
                        g_app.renderer.get());


            if (r) {
                r->ResizeCacheWindow(
                        clientW,
                        clientH);
            }
        }


        g_cacheOffset = 0;


        UpdateCacheView();


        InvalidateRect(
                g_hCacheWnd,
                nullptr,
                TRUE);


        UpdateWindow(
                g_hCacheWnd);
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


        int screenW =
                GetSystemMetrics(SM_CXSCREEN);

        int screenH =
                GetSystemMetrics(SM_CYSCREEN);


        int thickness =
                static_cast<int>(
                    Constants::CACHE_WINDOW_THICKNESS *
                    g_app.dpiScale);


        int x = 0;
        int y = 0;

        int width = screenW;
        int height = thickness;


        switch (position) {
            case 0: // bottom

                x = 0;
                y = screenH - height;

                break;


            case 1: // top

                x = 0;
                y = 0;

                break;


            case 2: // left

                x = 0;
                y = 0;

                width = thickness;
                height = screenH;

                break;


            case 3: // right

                x = screenW - thickness;
                y = 0;

                width = thickness;
                height = screenH;

                break;
        }


        g_hCacheWnd =
                CreateWindowExW(

                        WS_EX_TOPMOST |
                        WS_EX_TOOLWINDOW |
                        WS_EX_LAYERED,

                        wc.lpszClassName,

                        L"Cache",

                        WS_POPUP,

                        x,
                        y,
                        width,
                        height,

                        hParent,
                        nullptr,
                        hInstance,
                        nullptr
                        );


        if (!g_hCacheWnd)
            return;


        SetLayeredWindowAttributes(
                g_hCacheWnd,
                0,
                Constants::CACHE_WINDOW_OPACITY,
                LWA_ALPHA);


        if (g_app.renderer) {
            auto *r =
                    dynamic_cast<RendererD2D *>(
                        g_app.renderer.get());


            if (r) {
                r->CreateCacheWindowDeviceResources(
                        g_hCacheWnd);
            }
        }


        ShowWindow(
                g_hCacheWnd,
                SW_HIDE);
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
