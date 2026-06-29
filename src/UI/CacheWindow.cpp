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

    // Thumbnail layout constants — must match RenderCacheWindow in RendererD2D.cpp
    static constexpr float THUMB_X_START = 10.0f;
    static constexpr float THUMB_Y_START = 50.0f;
    static constexpr float THUMB_W = 120.0f;
    static constexpr float THUMB_H = 90.0f;
    static constexpr float THUMB_PADDING = 10.0f;
    static constexpr float THUMB_LABEL_H = 30.0f; // height reserved for filename label

    // Control IDs
    constexpr int IDC_CLEAR_CACHE_BUTTON = 101;
    constexpr int IDC_ADD_TO_CACHE_BUTTON = 102;
    constexpr int IDC_REMOVE_FROM_CACHE_BUTTON = 103;

    // -------------------------------------------------------------------------
    // HitTestThumb
    //   Returns the index of the thumbnail under (xPos, yPos), or -1 if none.
    //   clientWidth is the width of the cache window client area.
    // -------------------------------------------------------------------------
    static int HitTestThumb(int xPos, int yPos, int clientWidth,
                            const std::vector<IImageRenderer::CacheItem> &items) {
        float x = THUMB_X_START;
        float y = THUMB_Y_START;

        for (size_t i = 0; i < items.size(); ++i) {
            RECT thumbRect = {
                static_cast<LONG>(x),
                static_cast<LONG>(y),
                static_cast<LONG>(x + THUMB_W),
                static_cast<LONG>(y + THUMB_H)
            };

            if (PtInRect(&thumbRect, {xPos, yPos})) {
                return static_cast<int>(i);
            }

            x += THUMB_W + THUMB_PADDING;
            if (x + THUMB_W > static_cast<float>(clientWidth) - THUMB_PADDING) {
                x = THUMB_X_START;
                y += THUMB_H + THUMB_PADDING + THUMB_LABEL_H;
            }
        }

        return -1;
    }

    // -------------------------------------------------------------------------
    // SyncSelectionToCurrentImage
    //   Scans the cached items and sets g_selectedIndex to match g_app.currentIndex.
    //   Called when the window is shown so the current image is pre-highlighted.
    // -------------------------------------------------------------------------
    static void SyncSelectionToCurrentImage() {
        if (!g_app.renderer) return;
        if (g_app.currentIndex < 0 || g_app.currentIndex >= static_cast<int>(g_app.playlist.size())) return;

        const std::wstring &currentPath = g_app.playlist[g_app.currentIndex];
        auto items = g_app.renderer->GetCachedBitmaps();

        g_selectedIndex = -1;
        for (size_t i = 0; i < items.size(); ++i) {
            if (items[i].filePath == currentPath) {
                g_selectedIndex = static_cast<int>(i);
                break;
            }
        }
    }

    void UpdateCacheView() {
        InvalidateRect(g_hCacheWnd, NULL, TRUE);
    }

    LRESULT CALLBACK CacheWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
            case WM_CREATE: {
                if (g_app.renderer) {
                    auto *renderer = dynamic_cast<RendererD2D *>(g_app.renderer.get());
                    if (renderer) {
                        renderer->CreateCacheWindowDeviceResources(hWnd);
                    }
                }

                CreateWindowW(L"BUTTON", L"Clear All Cache", WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON,
                              10, 10, 150, 30, hWnd, (HMENU)IDC_CLEAR_CACHE_BUTTON,
                              (HINSTANCE)GetWindowLongPtr(hWnd, GWLP_HINSTANCE), NULL);

                CreateWindowW(L"BUTTON", L"Add Current Image", WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                              170, 10, 150, 30, hWnd, (HMENU)IDC_ADD_TO_CACHE_BUTTON,
                              (HINSTANCE)GetWindowLongPtr(hWnd, GWLP_HINSTANCE), NULL);

                CreateWindowW(L"BUTTON", L"Remove Selected", WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                              330, 10, 150, 30, hWnd, (HMENU)IDC_REMOVE_FROM_CACHE_BUTTON,
                              (HINSTANCE)GetWindowLongPtr(hWnd, GWLP_HINSTANCE), NULL);
                break;
            }

            case WM_COMMAND: {
                switch (LOWORD(wParam)) {
                    case IDC_CLEAR_CACHE_BUTTON:
                        if (g_app.renderer) {
                            g_app.renderer->ClearCache();
                            g_selectedIndex = -1;
                            UpdateCacheView();
                        }
                        break;

                    case IDC_ADD_TO_CACHE_BUTTON:
                        if (g_app.renderer && g_app.currentIndex != -1) {
                            (void) g_app.renderer->PreloadBitmap(
                                    g_app.playlist[g_app.currentIndex], g_app.currentIndex);
                        }
                        break;

                    case IDC_REMOVE_FROM_CACHE_BUTTON:
                        if (g_app.renderer && g_selectedIndex != -1) {
                            auto items = g_app.renderer->GetCachedBitmaps();
                            if (g_selectedIndex < static_cast<int>(items.size())) {
                                g_app.renderer->RemoveFromCache(items[g_selectedIndex].filePath);
                                g_selectedIndex = -1;
                                UpdateCacheView();
                            }
                        }
                        break;
                }
                break;
            }

            case WM_PAINT: {
                if (g_app.renderer) {
                    auto *renderer = dynamic_cast<RendererD2D *>(g_app.renderer.get());
                    if (renderer) {
                        renderer->RenderCacheWindow(g_selectedIndex);
                    }
                }
                ValidateRect(hWnd, NULL);
                break;
            }

            // Single click: select the thumbnail AND load it in the main window.
            case WM_LBUTTONDOWN: {
                int xPos = GET_X_LPARAM(lParam);
                int yPos = GET_Y_LPARAM(lParam);

                RECT rc;
                GetClientRect(hWnd, &rc);

                if (!g_app.renderer) break;

                auto items = g_app.renderer->GetCachedBitmaps();
                int hit = HitTestThumb(xPos, yPos, rc.right, items);

                if (hit != -1 && hit != g_selectedIndex) {
                    g_selectedIndex = hit;
                    InvalidateRect(hWnd, NULL, TRUE); // redraw selection border

                    // Find this file in the playlist and load it
                    auto it = std::find(g_app.playlist.begin(), g_app.playlist.end(),
                                        items[hit].filePath);
                    if (it != g_app.playlist.end()) {
                        int index = static_cast<int>(std::distance(g_app.playlist.begin(), it));
                        LoadImageIndex(GetParent(hWnd), index);
                    }
                } else if (hit == -1) {
                    // Clicked on empty area — deselect
                    g_selectedIndex = -1;
                    InvalidateRect(hWnd, NULL, TRUE);
                }
                break;
            }

            // Double-click: same behaviour as single click (already loaded above),
            // but we keep this handler so rapid double-clicking doesn't cause issues.
            // CS_DBLCLKS is set on the window class so this message is generated.
            case WM_LBUTTONDBLCLK: {
                int xPos = GET_X_LPARAM(lParam);
                int yPos = GET_Y_LPARAM(lParam);

                RECT rc;
                GetClientRect(hWnd, &rc);

                if (!g_app.renderer) break;

                auto items = g_app.renderer->GetCachedBitmaps();
                int hit = HitTestThumb(xPos, yPos, rc.right, items);

                if (hit != -1) {
                    g_selectedIndex = hit;
                    InvalidateRect(hWnd, NULL, TRUE);

                    auto it = std::find(g_app.playlist.begin(), g_app.playlist.end(),
                                        items[hit].filePath);
                    if (it != g_app.playlist.end()) {
                        int index = static_cast<int>(std::distance(g_app.playlist.begin(), it));
                        LoadImageIndex(GetParent(hWnd), index);
                    }
                }
                break;
            }

            case WM_SIZE: {
                if (g_app.renderer) {
                    auto *renderer = dynamic_cast<RendererD2D *>(g_app.renderer.get());
                    if (renderer) {
                        renderer->ResizeCacheWindow(LOWORD(lParam), HIWORD(lParam));
                    }
                }
                break;
            }

            case WM_SHOWWINDOW: {
                if (wParam == TRUE) {
                    // Pre-select whichever thumbnail matches the currently displayed image
                    SyncSelectionToCurrentImage();
                    UpdateCacheView();
                }
                break;
            }

            case WM_KEYDOWN: {
                if (wParam == VK_ESCAPE) {
                    ShowWindow(hWnd, SW_HIDE);
                }
                break;
            }

            case WM_ACTIVATE: {
                // When the cache window loses activation return focus to the
                // main window so keyboard shortcuts keep working there.
                if (LOWORD(wParam) == WA_INACTIVE) {
                    HWND hParent = GetParent(hWnd);
                    if (hParent) {
                        SetForegroundWindow(hParent);
                    }
                }
                break;
            }

            case WM_CLOSE: {
                ShowWindow(hWnd, SW_HIDE);
                return 0;
            }

            case WM_DESTROY: {
                if (g_app.renderer) {
                    auto *renderer = dynamic_cast<RendererD2D *>(g_app.renderer.get());
                    if (renderer) {
                        renderer->DiscardCacheWindowDeviceResources();
                    }
                }
                break;
            }
        }
        return DefWindowProcW(hWnd, message, wParam, lParam);
    }

    void InitCacheWindow(HINSTANCE hInstance, HWND hParent) {
        WNDCLASSW wc = {0};
        wc.style = CS_DBLCLKS; // required for WM_LBUTTONDBLCLK to be generated
        wc.lpfnWndProc = CacheWndProc;
        wc.hInstance = hInstance;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = L"QIV_CacheWindow";
        RegisterClassW(&wc);

        g_hCacheWnd = CreateWindowExW(
                0, wc.lpszClassName, L"Cache Viewer",
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
