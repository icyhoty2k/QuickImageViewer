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

    // -------------------------------------------------------------------------
    // Layout constants — MUST match the static constexpr values at the top of
    // RenderCacheWindow in RendererD2D.cpp exactly.
    // -------------------------------------------------------------------------
    static constexpr float BTN_Y = 8.0f;
    static constexpr float BTN_H = 28.0f;
    static constexpr float BTN_W = 145.0f;
    static constexpr float BTN_PAD = 8.0f;
    static constexpr float BTN_X0 = 10.0f;
    static constexpr float BTN_X1 = BTN_X0 + BTN_W + BTN_PAD;
    static constexpr float BTN_X2 = BTN_X1 + BTN_W + BTN_PAD;
    static constexpr float THUMB_Y = BTN_Y + BTN_H + BTN_PAD; // 44 px

    static constexpr float THUMB_W = 120.0f;
    static constexpr float THUMB_H = 90.0f;
    static constexpr float THUMB_PAD = 10.0f;
    static constexpr float LABEL_H = 30.0f;

    // -------------------------------------------------------------------------
    // HitTestButton — returns 0/1/2 for the three buttons, -1 if not hit
    // -------------------------------------------------------------------------
    static int HitTestButton(int xPos, int yPos) {
        float fx = static_cast<float>(xPos);
        float fy = static_cast<float>(yPos);
        float btnXArr[3] = {BTN_X0, BTN_X1, BTN_X2};
        for (int b = 0; b < 3; ++b) {
            if (fx >= btnXArr[b] && fx <= btnXArr[b] + BTN_W &&
                fy >= BTN_Y && fy <= BTN_Y + BTN_H) {
                return b;
            }
        }
        return -1;
    }

    // -------------------------------------------------------------------------
    // HitTestThumb — returns thumbnail index, or -1
    // -------------------------------------------------------------------------
    static int HitTestThumb(int xPos, int yPos, int clientWidth,
                            const std::vector<IImageRenderer::CacheItem> &items) {
        float x = 10.0f;
        float y = THUMB_Y;

        for (size_t i = 0; i < items.size(); ++i) {
            RECT r = {
                static_cast<LONG>(x),
                static_cast<LONG>(y),
                static_cast<LONG>(x + THUMB_W),
                static_cast<LONG>(y + THUMB_H)
            };
            if (PtInRect(&r, {xPos, yPos})) {
                return static_cast<int>(i);
            }

            x += THUMB_W + THUMB_PAD;
            if (x + THUMB_W > static_cast<float>(clientWidth) - THUMB_PAD) {
                x = 10.0f;
                y += THUMB_H + THUMB_PAD + LABEL_H;
            }
        }
        return -1;
    }

    // -------------------------------------------------------------------------
    // SyncSelectionToCurrentImage
    // -------------------------------------------------------------------------
    static void SyncSelectionToCurrentImage() {
        if (!g_app.renderer) return;
        if (g_app.currentIndex < 0 ||
            g_app.currentIndex >= static_cast<int>(g_app.playlist.size()))
            return;

        const std::wstring &cur = g_app.playlist[g_app.currentIndex];
        auto items = g_app.renderer->GetCachedBitmaps();
        g_selectedIndex = -1;
        for (size_t i = 0; i < items.size(); ++i) {
            if (items[i].filePath == cur) {
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
                    auto *r = dynamic_cast<RendererD2D *>(g_app.renderer.get());
                    if (r) r->CreateCacheWindowDeviceResources(hWnd);
                }
                break;
            }

            case WM_PAINT: {
                if (g_app.renderer) {
                    auto *r = dynamic_cast<RendererD2D *>(g_app.renderer.get());
                    if (r) r->RenderCacheWindow(g_selectedIndex);
                }
                ValidateRect(hWnd, NULL);
                break;
            }

            case WM_LBUTTONDOWN: {
                int xPos = GET_X_LPARAM(lParam);
                int yPos = GET_Y_LPARAM(lParam);

                // --- Check buttons first ---
                int btn = HitTestButton(xPos, yPos);
                if (btn == 0) {
                    // Clear All Cache
                    if (g_app.renderer) {
                        g_app.renderer->ClearCache();
                        g_selectedIndex = -1;
                        UpdateCacheView();
                    }
                    break;
                }
                if (btn == 1) {
                    // Add Current Image
                    if (g_app.renderer && g_app.currentIndex != -1) {
                        (void) g_app.renderer->PreloadBitmap(
                                g_app.playlist[g_app.currentIndex], g_app.currentIndex);
                    }
                    break;
                }
                if (btn == 2) {
                    // Remove Selected
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

                // --- Check thumbnails ---
                if (!g_app.renderer) break;

                RECT rc;
                GetClientRect(hWnd, &rc);
                auto items = g_app.renderer->GetCachedBitmaps();
                int hit = HitTestThumb(xPos, yPos, rc.right, items);

                if (hit != -1) {
                    g_selectedIndex = hit;
                    InvalidateRect(hWnd, NULL, TRUE);

                    // Find the file in the playlist and load it in the main window
                    auto it = std::find(g_app.playlist.begin(), g_app.playlist.end(),
                                        items[hit].filePath);
                    if (it != g_app.playlist.end()) {
                        int index = static_cast<int>(
                            std::distance(g_app.playlist.begin(), it));
                        LoadImageIndex(GetParent(hWnd), index);
                    }
                } else {
                    g_selectedIndex = -1;
                    InvalidateRect(hWnd, NULL, TRUE);
                }
                break;
            }

            case WM_SIZE: {
                if (g_app.renderer) {
                    auto *r = dynamic_cast<RendererD2D *>(g_app.renderer.get());
                    if (r) r->ResizeCacheWindow(LOWORD(lParam), HIWORD(lParam));
                }
                break;
            }

            case WM_SHOWWINDOW: {
                if (wParam == TRUE) {
                    SyncSelectionToCurrentImage();
                    UpdateCacheView();
                }
                break;
            }

            case WM_KEYDOWN: {
                if (wParam == VK_ESCAPE) ShowWindow(hWnd, SW_HIDE);
                break;
            }

            case WM_ACTIVATE: {
                // Return focus to main window when cache window loses activation
                // so keyboard shortcuts keep working there.
                if (LOWORD(wParam) == WA_INACTIVE) {
                    HWND hParent = GetParent(hWnd);
                    if (hParent) SetForegroundWindow(hParent);
                }
                break;
            }

            case WM_CLOSE: {
                ShowWindow(hWnd, SW_HIDE);
                return 0;
            }

            case WM_DESTROY: {
                if (g_app.renderer) {
                    auto *r = dynamic_cast<RendererD2D *>(g_app.renderer.get());
                    if (r) r->DiscardCacheWindowDeviceResources();
                }
                break;
            }
        }
        return DefWindowProcW(hWnd, message, wParam, lParam);
    }

    void InitCacheWindow(HINSTANCE hInstance, HWND hParent) {
        WNDCLASSW wc = {0};
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = CacheWndProc;
        wc.hInstance = hInstance;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = L"QIV_CacheWindow";
        RegisterClassW(&wc);

        g_hCacheWnd = CreateWindowExW(
                0, wc.lpszClassName, L"Cache Viewer",
                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
                CW_USEDEFAULT, CW_USEDEFAULT, 900, 320,
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
