#include "CacheWindow.h"
#include "../AppState.h"
#include "../Platform/FileHandler.h"
#include <windowsx.h>
#include <vector>
#include <string>
#include "../Renderer/RendererD2D.h"

namespace UI {
    HWND g_hCacheWnd = nullptr;
    int g_selectedIndex = -1;

    // Control IDs
    constexpr int IDC_CLEAR_CACHE_BUTTON = 101;
    constexpr int IDC_ADD_TO_CACHE_BUTTON = 102;
    constexpr int IDC_REMOVE_FROM_CACHE_BUTTON = 103;

    void UpdateCacheView() {
        InvalidateRect(g_hCacheWnd, NULL, TRUE);
    }

    LRESULT CALLBACK CacheWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
            case WM_CREATE: {
                if (g_app.renderer) {
                    auto* renderer = dynamic_cast<RendererD2D*>(g_app.renderer.get());
                    if (renderer) {
                        renderer->CreateCacheWindowDeviceResources(hWnd);
                    }
                }

                CreateWindowW(L"BUTTON", L"Clear All Cache", WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON,
                              10, 10, 150, 30, hWnd, (HMENU)IDC_CLEAR_CACHE_BUTTON,
                              (HINSTANCE)GetWindowLongPtr(hWnd, GWLP_HINSTANCE), NULL);

                CreateWindowW(L"BUTTON", L"Add Current Image", WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON,
                              170, 10, 150, 30, hWnd, (HMENU)IDC_ADD_TO_CACHE_BUTTON,
                              (HINSTANCE)GetWindowLongPtr(hWnd, GWLP_HINSTANCE), NULL);

                CreateWindowW(L"BUTTON", L"Remove Selected", WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON,
                              330, 10, 150, 30, hWnd, (HMENU)IDC_REMOVE_FROM_CACHE_BUTTON,
                              (HINSTANCE)GetWindowLongPtr(hWnd, GWLP_HINSTANCE), NULL);
                break;
            }
            case WM_COMMAND: {
                switch (LOWORD(wParam)) {
                    case IDC_CLEAR_CACHE_BUTTON:
                        if (g_app.renderer) {
                            g_app.renderer->ClearCache();
                            UpdateCacheView();
                        }
                        break;
                    case IDC_ADD_TO_CACHE_BUTTON:
                        if (g_app.renderer && g_app.currentIndex != -1) {
                            (void)g_app.renderer->PreloadBitmap(g_app.playlist[g_app.currentIndex], g_app.currentIndex);
                        }
                        break;
                    case IDC_REMOVE_FROM_CACHE_BUTTON:
                        if (g_app.renderer && g_selectedIndex != -1) {
                            auto items = g_app.renderer->GetCachedBitmaps();
                            if (g_selectedIndex < items.size()) {
                                g_app.renderer->RemoveFromCache(items[g_selectedIndex].filePath);
                                UpdateCacheView();
                            }
                        }
                        break;
                }
                break;
            }
            case WM_PAINT: {
                if (g_app.renderer) {
                    auto* renderer = dynamic_cast<RendererD2D*>(g_app.renderer.get());
                    if (renderer) {
                        renderer->RenderCacheWindow(g_selectedIndex);
                    }
                }
                ValidateRect(hWnd, NULL);
                break;
            }
            case WM_LBUTTONDBLCLK: {
                int xPos = GET_X_LPARAM(lParam);
                int yPos = GET_Y_LPARAM(lParam);

                RECT rc;
                GetClientRect(hWnd, &rc);
                int x = 10, y = 50;
                int thumbWidth = 120, thumbHeight = 90;
                int padding = 10;

                auto items = g_app.renderer->GetCachedBitmaps();
                for (size_t i = 0; i < items.size(); ++i) {
                    RECT thumbRect = {x, y, x + thumbWidth, y + thumbHeight};
                    if (PtInRect(&thumbRect, {xPos, yPos})) {
                        auto it = std::find(g_app.playlist.begin(), g_app.playlist.end(), items[i].filePath);
                        if (it != g_app.playlist.end()) {
                            int index = static_cast<int>(std::distance(g_app.playlist.begin(), it));
                            LoadImageIndex(GetParent(hWnd), index);
                        }
                        break;
                    }
                    x += thumbWidth + padding;
                    if (x + thumbWidth > rc.right) {
                        x = 10;
                        y += thumbHeight + padding + 30;
                    }
                }
                break;
            }
            case WM_LBUTTONDOWN: {
                int xPos = GET_X_LPARAM(lParam);
                int yPos = GET_Y_LPARAM(lParam);

                RECT rc;
                GetClientRect(hWnd, &rc);
                int x = 10, y = 50;
                int thumbWidth = 120, thumbHeight = 90;
                int padding = 10;
                g_selectedIndex = -1;

                auto items = g_app.renderer->GetCachedBitmaps();
                for (size_t i = 0; i < items.size(); ++i) {
                    RECT thumbRect = {x, y, x + thumbWidth, y + thumbHeight};
                    if (PtInRect(&thumbRect, {xPos, yPos})) {
                        g_selectedIndex = static_cast<int>(i);
                        break;
                    }
                    x += thumbWidth + padding;
                    if (x + thumbWidth > rc.right) {
                        x = 10;
                        y += thumbHeight + padding + 30;
                    }
                }
                InvalidateRect(hWnd, NULL, TRUE);
                break;
            }
            case WM_SIZE: {
                if (g_app.renderer) {
                    auto* renderer = dynamic_cast<RendererD2D*>(g_app.renderer.get());
                    if (renderer) {
                        renderer->ResizeCacheWindow(LOWORD(lParam), HIWORD(lParam));
                    }
                }
                break;
            }
            case WM_SHOWWINDOW:
                if (wParam == TRUE) {
                    UpdateCacheView();
                }
                break;
            case WM_KEYDOWN:
                if (wParam == VK_ESCAPE) {
                    ShowWindow(hWnd, SW_HIDE);
                }
                break;
            case WM_CLOSE:
                ShowWindow(hWnd, SW_HIDE);
                return 0;
            case WM_DESTROY:
                if (g_app.renderer) {
                    auto* renderer = dynamic_cast<RendererD2D*>(g_app.renderer.get());
                    if (renderer) {
                        renderer->DiscardCacheWindowDeviceResources();
                    }
                }
                break;
        }
        return DefWindowProcW(hWnd, message, wParam, lParam);
    }

    void InitCacheWindow(HINSTANCE hInstance, HWND hParent) {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc = CacheWndProc;
        wc.hInstance = hInstance;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = L"QIV_CacheWindow";
        RegisterClassW(&wc);

        g_hCacheWnd = CreateWindowExW(0, wc.lpszClassName, L"Cache Viewer",
                                     WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
                                     CW_USEDEFAULT, CW_USEDEFAULT, 800, 600,
                                     hParent, nullptr, hInstance, nullptr);
    }

    void ToggleCacheWindow() {
        if (!g_hCacheWnd) return;
        if (IsWindowVisible(g_hCacheWnd)) {
            ShowWindow(g_hCacheWnd, SW_HIDE);
        } else {
            ShowWindow(g_hCacheWnd, SW_SHOW);
            SetForegroundWindow(g_hCacheWnd);
        }
    }
}
