#include "CacheWindow.h"
#include "../AppState.h"
#include "../Platform/FileHandler.h"
#include <windowsx.h>
#include <vector>
#include <string>
#include "../Renderer/RendererD2D.h"

namespace UI {
    static HWND g_hOwner = nullptr; // the main app window (owner, not parent)
    HWND g_hCacheWnd = nullptr;
    int g_selectedIndex = -1;

    // -------------------------------------------------------------------------
    // Layout constants — must match RenderCacheWindow in RendererD2D.cpp exactly
    // -------------------------------------------------------------------------
    static constexpr float THUMB_W = 240.0f;
    static constexpr float THUMB_H = 180.0f;
    static constexpr float THUMB_PAD = 12.0f;
    static constexpr float LABEL_H = 36.0f;
    static constexpr float THUMB_X0 = 10.0f;
    static constexpr float THUMB_Y0 = 50.0f; // below the D2D buttons

    // -------------------------------------------------------------------------
    // HitTestThumb
    // -------------------------------------------------------------------------
    static int HitTestThumb(int xPos, int yPos, int clientWidth,
                            const std::vector<IImageRenderer::CacheItem> &items) {
        float x = THUMB_X0;
        float y = THUMB_Y0;

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
                x = THUMB_X0;
                y += THUMB_H + THUMB_PAD + LABEL_H;
            }
        }
        return -1;
    }

    // -------------------------------------------------------------------------
    // HitTestButton  — 0=Clear, 1=Add, 2=Remove, -1=none
    // These rects must match what RenderCacheWindow draws.
    // -------------------------------------------------------------------------
    static constexpr float BTN_Y = 8.0f;
    static constexpr float BTN_H = 28.0f;
    static constexpr float BTN_W = 145.0f;
    static constexpr float BTN_GP = 8.0f;
    static constexpr float BTN_X0 = 10.0f;
    static constexpr float BTN_X1 = BTN_X0 + BTN_W + BTN_GP;
    static constexpr float BTN_X2 = BTN_X1 + BTN_W + BTN_GP;

    static int HitTestButton(int xPos, int yPos) {
        float bx[3] = {BTN_X0, BTN_X1, BTN_X2};
        float fx = static_cast<float>(xPos);
        float fy = static_cast<float>(yPos);
        for (int b = 0; b < 3; ++b) {
            if (fx >= bx[b] && fx <= bx[b] + BTN_W &&
                fy >= BTN_Y && fy <= BTN_Y + BTN_H) {
                return b;
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

                // --- D2D button area ---
                int btn = HitTestButton(xPos, yPos);
                if (btn == 0) {
                    if (g_app.renderer) {
                        g_app.renderer->ClearCache();
                        g_selectedIndex = -1;
                        UpdateCacheView();
                    }
                    break;
                }
                if (btn == 1) {
                    if (g_app.renderer && g_app.currentIndex != -1) {
                        (void) g_app.renderer->PreloadBitmap(
                                g_app.playlist[g_app.currentIndex], g_app.currentIndex);
                    }
                    break;
                }
                if (btn == 2) {
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

                // --- Thumbnail area ---
                if (!g_app.renderer) break;

                RECT rc;
                GetClientRect(hWnd, &rc);
                auto items = g_app.renderer->GetCachedBitmaps();
                int hit = HitTestThumb(xPos, yPos, rc.right, items);

                if (hit != -1) {
                    g_selectedIndex = hit;
                    InvalidateRect(hWnd, NULL, TRUE);

                    auto it = std::find(g_app.playlist.begin(), g_app.playlist.end(),
                                        items[hit].filePath);
                    if (it != g_app.playlist.end()) {
                        int idx = static_cast<int>(
                            std::distance(g_app.playlist.begin(), it));
                        // g_hOwner is the main window — GetParent() returns null
                        // on WS_OVERLAPPED owned windows.
                        LoadImageIndex(g_hOwner, idx);
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
                // F3 hides this window; the main window's WM_KEYDOWN won't
                // fire while cache window has focus, so we handle it here too.
                if (wParam == VK_F3) ShowWindow(hWnd, SW_HIDE);
                break;
            }

            // NOTE: no WM_ACTIVATE handler — returning focus to the main window
            // on deactivation caused F3 to immediately re-fire and close the
            // cache window. The main window receives keyboard input normally once
            // the user clicks back on it.

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
        g_hOwner = hParent; // store for use in WM_LBUTTONDOWN

        WNDCLASSW wc = {0};
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = CacheWndProc;
        wc.hInstance = hInstance;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = L"QIV_CacheWindow";
        RegisterClassW(&wc);

        // WS_OVERLAPPED owned window: hParent is the *owner*, not the parent.
        // GetParent() returns null on such windows — use g_hOwner instead.
        g_hCacheWnd = CreateWindowExW(
                0, wc.lpszClassName, L"Cache Viewer",
                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
                CW_USEDEFAULT, CW_USEDEFAULT, 1300, 400,
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
