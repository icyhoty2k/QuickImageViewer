#include "CacheWindow.h"
#include <algorithm>
#include "../AppState.h"
#include "../Platform/FileHandler.h"
#include <windowsx.h>
#include "../Renderer/RendererD2D.h"
#include "Constants.h"
#include "../Platform/Shortcuts.h"

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
    POINT g_clickPos = {0, 0};
    bool g_hasMoved = false;

    // The single source of truth for UI selection state
    void SyncSelectionRectangle() {
        if (!g_hCacheWnd) return;

        g_selectedIndex = -1;
        for (size_t i = 0; i < g_thumbnailObjects.size(); ++i) {
            if (g_thumbnailObjects[i].playlistIndex == g_app.currentIndex) {
                g_selectedIndex = static_cast<int>(i);
                break;
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

            case WM_MOUSEWHEEL: {
                int delta = GET_WHEEL_DELTA_WPARAM(wParam);
                float scroll = Constants::CACHE_WINDOW_MOUSE_WHEEL_SPEED;
                if (GetKeyState(VK_SHIFT) & 0x8000) scroll *= 3.0f;
                float amount = (delta > 0 ? scroll : -scroll) * Constants::CACHE_WINDOW_MOUSE_WHEEL_DIRECTION;
                g_cacheOffset += amount;
                UpdateCacheView();
                return 0;
            }

            case WM_KEYDOWN: {
                switch (wParam) {
                    case Shortcuts::SC_PANEL_CACHE_CLEAR:
                        ClearThumbnailCache();
                        return 0;
                    case Shortcuts::SC_LOCAL_HIDE:
                        ShowWindow(hWnd, SW_HIDE);
                        return 0;
                    case Shortcuts::SC_PANEL_CACHE_TOGGLE:
                        ToggleCacheWindow();
                        return 0;
                    case Shortcuts::SC_PANEL_CACHE_MOVE:
                        MoveCacheWindow();
                        return 0;
                    default:
                        if (g_hOwner) {
                            return SendMessageW(g_hOwner, message, wParam, lParam);
                        }
                        break;
                }
                break;
            }

            case WM_LBUTTONDOWN: {
                g_clickPos.x = GET_X_LPARAM(lParam);
                g_clickPos.y = GET_Y_LPARAM(lParam);
                g_hasMoved = false;

                g_isDragging = true;
                SetCapture(hWnd);
                GetCursorPos(&g_lastMousePos);
                return 0;
            }

            case WM_MOUSEMOVE: {
                int x = GET_X_LPARAM(lParam);
                int y = GET_Y_LPARAM(lParam);

                int newHoverIndex = -1;
                for (size_t i = 0; i < g_thumbnailObjects.size(); ++i) {
                    if (g_thumbnailObjects[i].HitTest(x, y)) {
                        newHoverIndex = static_cast<int>(i);
                        break;
                    }
                }

                if (newHoverIndex != g_hoverIndex) {
                    g_hoverIndex = newHoverIndex;
                    InvalidateRect(hWnd, nullptr, FALSE);
                }

                if (g_isDragging) {
                    if (abs(x - g_clickPos.x) > 5 || abs(y - g_clickPos.y) > 5) {
                        g_hasMoved = true;
                    }

                    POINT cur;
                    GetCursorPos(&cur);
                    float delta;

                    if (g_cachePosition == 1 || g_cachePosition == 3) {
                        delta = static_cast<float>(cur.y - g_lastMousePos.y);
                    } else {
                        delta = static_cast<float>(cur.x - g_lastMousePos.x);
                    }

                    g_cacheOffset += delta;
                    g_lastMousePos = cur;

                    UpdateCacheView();
                }
                return 0;
            }

            case WM_LBUTTONUP: {
                if (g_isDragging) {
                    ReleaseCapture();

                    if (!g_hasMoved) {
                        int x = GET_X_LPARAM(lParam);
                        int y = GET_Y_LPARAM(lParam);
                        for (size_t i = 0; i < g_thumbnailObjects.size(); ++i) {
                            if (g_thumbnailObjects[i].HitTest(x, y)) {
                                LoadImageIndex(g_hOwner, g_thumbnailObjects[i].playlistIndex);
                                break;
                            }
                        }
                    }
                    g_isDragging = false;
                }
                return 0;
            }

            case WM_CLOSE: {
                ShowWindow(hWnd, SW_HIDE);
                return 0;
            }
        }
        return DefWindowProcW(hWnd, message, wParam, lParam);
    }

    void ClearThumbnailCache() {
        std::wstring activeFile = L"";
        if (!g_app.playlist.empty() &&
            g_app.currentIndex >= 0 &&
            g_app.currentIndex < static_cast<int>(g_app.playlist.size())) {
            activeFile = g_app.playlist[g_app.currentIndex];
        }

        if (g_app.renderer) {
            g_app.renderer->ClearCache(activeFile);
        }

        g_cacheOffset = 0.0f;
        UpdateCacheView();
    }

    void UpdateCacheView() {
        if (!g_hCacheWnd || !g_app.renderer || !IsWindowVisible(g_hCacheWnd)) return;

        g_thumbnailObjects.clear();
        auto items = g_app.renderer->GetCachedBitmaps();
        RECT cr{};
        GetClientRect(g_hCacheWnd, &cr);

        float surfaceW = static_cast<float>(cr.right);
        float surfaceH = static_cast<float>(cr.bottom);
        bool vertical = (g_cachePosition == 1 || g_cachePosition == 3);

        float thumbW = Constants::CACHE_THUMB_WIDTH * g_app.dpiScale;
        float thumbH = Constants::CACHE_THUMB_HEIGHT * g_app.dpiScale;
        float scaledMargin = Constants::CACHE_THUMB_MARGIN * g_app.dpiScale;
        float scaledSpacing = Constants::CACHE_THUMB_SPACING * g_app.dpiScale;

        float x = scaledMargin;
        float y = scaledMargin;

        if (!vertical) {
            y = (surfaceH - thumbH) / 2.0f;
            float totalWidth = items.size() * (thumbW + scaledSpacing) - scaledSpacing;
            if (totalWidth <= surfaceW) {
                x = (surfaceW - totalWidth) / 2.0f;
            } else {
                float minOffset = surfaceW - totalWidth - scaledMargin;
                g_cacheOffset = std::clamp(g_cacheOffset, minOffset, 0.0f);
                x = scaledMargin + g_cacheOffset;
            }
        } else {
            x = (surfaceW - thumbW) / 2.0f;
            float totalHeight = items.size() * (thumbH + scaledSpacing) - scaledSpacing;
            if (totalHeight <= surfaceH) {
                y = (surfaceH - totalHeight) / 2.0f;
            } else {
                float minOffset = surfaceH - totalHeight - scaledMargin;
                g_cacheOffset = std::clamp(g_cacheOffset, minOffset, 0.0f);
                y = scaledMargin + g_cacheOffset;
            }
        }

        for (const auto &item: items) {
            auto mapIt = g_app.playlistIndexMap.find(item.filePath);
            int idx = (mapIt != g_app.playlistIndexMap.end()) ? mapIt->second : -1;

            g_thumbnailObjects.push_back({
                D2D1::RectF(x, y, x + thumbW, y + thumbH),
                item.filePath,
                idx
            });

            if (vertical) y += thumbH + scaledSpacing;
            else x += thumbW + scaledSpacing;
        }

        // Re-anchor the selection to match the newly built geometry vector
        SyncSelectionRectangle();
    }

    void GetCacheWindowBounds(HWND hRefWnd, int8_t position, int &x, int &y, int &width, int &height) {
        HMONITOR hMonitor = MonitorFromWindow(hRefWnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = {sizeof(mi)};
        GetMonitorInfoW(hMonitor, &mi);

        int monX = mi.rcMonitor.left;
        int monY = mi.rcMonitor.top;
        int monW = mi.rcMonitor.right - mi.rcMonitor.left;
        int monH = mi.rcMonitor.bottom - mi.rcMonitor.top;

        int horzThickness = static_cast<int>((Constants::CACHE_THUMB_HEIGHT + (Constants::CACHE_THUMB_MARGIN * 2.0f)) * g_app.dpiScale);
        int vertThickness = static_cast<int>((Constants::CACHE_THUMB_WIDTH + (Constants::CACHE_THUMB_MARGIN * 2.0f)) * g_app.dpiScale);

        x = monX;
        y = monY;
        width = monW;
        height = horzThickness;

        switch (position) {
            case 0: // top
                y = monY;
                height = horzThickness;
                break;
            case 1: // right
                x = monX + monW - vertThickness;
                width = vertThickness;
                height = monH;
                break;
            case 2: // bottom
                y = monY + monH - horzThickness;
                height = horzThickness;
                break;
            case 3: // left
                width = vertThickness;
                height = monH;
                break;
        }
    }

    void MoveCacheWindow() {
        if (!g_hCacheWnd) return;
        g_cachePosition++;
        if (g_cachePosition > 3) g_cachePosition = 0;
        int x, y, width, height;
        GetCacheWindowBounds(g_hOwner ? g_hOwner : g_hCacheWnd, g_cachePosition, x, y, width, height);
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

        int x, y, width, height;
        GetCacheWindowBounds(hParent, position, x, y, width, height);

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
        }
    }
}
