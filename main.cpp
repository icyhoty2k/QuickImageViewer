#include <windows.h>
#include <windowsx.h>
#include "AppState.h"
#include "FileHandler.h"
#include "WicDecoder.h"
#include "Renderer.h"
#include "DpiAwareInit.h"
#include <dwmapi.h>

AppState g_app;
static constexpr float ZOOM_STEP        = 1.1f;  // +/- keys and ctrl+wheel
static constexpr float ZOOM_LMB         = 2.0f;  // left click zoom multiplier

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {

        case WM_KEYDOWN: {
            bool shift = (GetKeyState(VK_SHIFT)   & 0x8000) != 0;
            bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;

            // Quit
            if (wParam == VK_ESCAPE || (wParam == 'W' && ctrl)) {
                PostQuitMessage(0);
                return 0;
            }

            // Fullscreen toggle: F, F11, Enter, Ctrl+Shift+T
            if (wParam == VK_F11 || wParam == 'F' || wParam == VK_RETURN ||
                (wParam == 'T' && ctrl && shift)) {

                if (!g_app.isFullscreen) {
                    GetWindowRect(hWnd, &g_app.savedWindowRect);
                    MONITORINFO mi = { sizeof(mi) };
                    GetMonitorInfo(MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST), &mi);
                    SetWindowPos(hWnd, HWND_TOPMOST,
                        mi.rcMonitor.left, mi.rcMonitor.top,
                        mi.rcMonitor.right  - mi.rcMonitor.left,
                        mi.rcMonitor.bottom - mi.rcMonitor.top,
                        SWP_FRAMECHANGED);
                    DWMNCRENDERINGPOLICY policy = DWMNCRP_DISABLED;
                    DwmSetWindowAttribute(hWnd, DWMWA_NCRENDERING_POLICY, &policy, sizeof(policy));
                    DWORD corner = 1; // DWMWCP_DONOTROUND
                    DwmSetWindowAttribute(hWnd, 33, &corner, sizeof(corner));
                    MARGINS margins = { 0, 0, 0, 0 };
                    DwmExtendFrameIntoClientArea(hWnd, &margins);
                    g_app.isFullscreen = true;
                } else {
                    SetWindowPos(hWnd, HWND_NOTOPMOST,
                        g_app.savedWindowRect.left,
                        g_app.savedWindowRect.top,
                        g_app.savedWindowRect.right  - g_app.savedWindowRect.left,
                        g_app.savedWindowRect.bottom - g_app.savedWindowRect.top,
                        SWP_FRAMECHANGED);
                    DWMNCRENDERINGPOLICY policy = DWMNCRP_ENABLED;
                    DwmSetWindowAttribute(hWnd, DWMWA_NCRENDERING_POLICY, &policy, sizeof(policy));
                    DWORD corner = 2; // DWMWCP_ROUND
                    DwmSetWindowAttribute(hWnd, 33, &corner, sizeof(corner));
                    MARGINS margins = { 1, 1, 1, 1 };
                    DwmExtendFrameIntoClientArea(hWnd, &margins);
                    g_app.isFullscreen = false;
                }
                InvalidateRect(hWnd, nullptr, FALSE);
                return 0;
            }

            // Navigation + zoom
            if (!g_app.playlist.empty()) {
                switch (wParam) {
                    case VK_LEFT:
                        LoadImageIndex(hWnd, (g_app.currentIndex - 1 + g_app.playlist.size()) % g_app.playlist.size());
                        InvalidateRect(hWnd, nullptr, FALSE);
                        break;
                    case VK_RIGHT:
                        LoadImageIndex(hWnd, (g_app.currentIndex + 1) % g_app.playlist.size());
                        InvalidateRect(hWnd, nullptr, FALSE);
                        break;
                    case VK_SPACE:
                        if (shift)
                            LoadImageIndex(hWnd, (g_app.currentIndex - 1 + g_app.playlist.size()) % g_app.playlist.size());
                        else
                            LoadImageIndex(hWnd, (g_app.currentIndex + 1) % g_app.playlist.size());
                        InvalidateRect(hWnd, nullptr, FALSE);
                        break;
                    case VK_ADD:
                    case VK_OEM_PLUS:
                        g_app.viewport.zoom *= 1.1f;
                        InvalidateRect(hWnd, nullptr, FALSE);
                        break;
                    case VK_SUBTRACT:
                    case VK_OEM_MINUS:
                        g_app.viewport.zoom *= 0.9f;
                        InvalidateRect(hWnd, nullptr, FALSE);
                        break;
                    case VK_NUMPAD0:
                    case '0':
                        g_app.viewport.zoom    = 1.0f;
                        g_app.viewport.offsetX = 0.0f;
                        g_app.viewport.offsetY = 0.0f;
                        InvalidateRect(hWnd, nullptr, FALSE);
                        break;
                }
            }
            return 0;
        }

        case WM_NCACTIVATE:
            return TRUE;

        case WM_NCHITTEST: {
            if (g_app.isFullscreen) return HTCLIENT;

            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            RECT rc;
            GetWindowRect(hWnd, &rc);

            const int border = 8;
            bool top    = pt.y <  rc.top    + border;
            bool bottom = pt.y >= rc.bottom - border;
            bool left   = pt.x <  rc.left   + border;
            bool right  = pt.x >= rc.right  - border;

            if (top    && left)  return HTTOPLEFT;
            if (top    && right) return HTTOPRIGHT;
            if (bottom && left)  return HTBOTTOMLEFT;
            if (bottom && right) return HTBOTTOMRIGHT;
            if (top)             return HTTOP;
            if (bottom)          return HTBOTTOM;
            if (left)            return HTLEFT;
            if (right)           return HTRIGHT;

            return HTCLIENT;
        }

        case WM_SIZING:
            InvalidateRect(hWnd, nullptr, FALSE);
            return TRUE;

        case WM_MOUSEWHEEL: {
            if (g_app.playlist.empty()) return 0;
            int zDelta = GET_WHEEL_DELTA_WPARAM(wParam);
            if (GET_KEYSTATE_WPARAM(wParam) & MK_CONTROL) {
                g_app.viewport.zoom *= (zDelta > 0) ? 1.1f : 0.9f;
            } else {
                int step = (zDelta < 0) ? 1 : -1;
                int newIdx = (g_app.currentIndex + step + g_app.playlist.size()) % g_app.playlist.size();
                LoadImageIndex(hWnd, newIdx);
            }
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;
        }

        // Left click: 2x zoom + pan while held, restore all on release
        case WM_LBUTTONDOWN:
            SetCursor(NULL);
            g_app.savedZoom    = g_app.viewport.zoom;
            g_app.savedOffsetX = g_app.viewport.offsetX;
            g_app.savedOffsetY = g_app.viewport.offsetY;
            g_app.viewport.zoom *= 2.0f;
            g_app.viewport.lastMouse = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            g_app.viewport.isDragging = true;
            SetCapture(hWnd);
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;

        case WM_LBUTTONUP:
            SetCursor(LoadCursor(nullptr, IDC_ARROW));
            g_app.viewport.zoom    = g_app.savedZoom;
            g_app.viewport.offsetX = g_app.savedOffsetX;
            g_app.viewport.offsetY = g_app.savedOffsetY;
            g_app.viewport.isDragging = false;
            ReleaseCapture();
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;

        // Middle mouse: pan on drag, reset zoom on click
        case WM_MBUTTONDOWN:
            g_app.isMidDragging = true;
            g_app.hasMidMoved   = false;
            g_app.lastMidMouse  = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            SetCapture(hWnd);
            return 0;

        case WM_MBUTTONUP:
            if (!g_app.hasMidMoved) {
                g_app.viewport.zoom    = 1.0f;
                g_app.viewport.offsetX = 0.0f;
                g_app.viewport.offsetY = 0.0f;
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            g_app.isMidDragging = false;
            g_app.hasMidMoved   = false;
            ReleaseCapture();
            return 0;

        // Right mouse: window drag (disabled in fullscreen)
        case WM_RBUTTONDOWN: {
            if (g_app.isFullscreen) return 0;
            g_app.isWindowDragging = true;
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ClientToScreen(hWnd, &pt);
            g_app.lastWindowMouse = pt;
            SetCapture(hWnd);
            return 0;
        }

        case WM_RBUTTONUP:
            g_app.isWindowDragging = false;
            ReleaseCapture();
            return 0;

        case WM_MOUSEMOVE: {
            // 1. Left mouse pan (while 2x zoom held)
            if (g_app.viewport.isDragging) {
                POINT curMouse = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                g_app.viewport.offsetX -= (curMouse.x - g_app.viewport.lastMouse.x);
                g_app.viewport.offsetY -= (curMouse.y - g_app.viewport.lastMouse.y);
                g_app.viewport.lastMouse = curMouse;

                // Clamp to image bounds
                RECT rc;
                GetClientRect(hWnd, &rc);
                float winW = (float)(rc.right  - rc.left);
                float winH = (float)(rc.bottom - rc.top);

                float base    = min(winW / g_app.imgWidth, winH / g_app.imgHeight);
                float renderW = g_app.imgWidth  * base * g_app.viewport.zoom;
                float renderH = g_app.imgHeight * base * g_app.viewport.zoom;

                float maxOffX = max(0.0f, (renderW - winW) / 2.0f);
                float maxOffY = max(0.0f, (renderH - winH) / 2.0f);

                g_app.viewport.offsetX = max(-maxOffX, min(maxOffX, g_app.viewport.offsetX));
                g_app.viewport.offsetY = max(-maxOffY, min(maxOffY, g_app.viewport.offsetY));

                InvalidateRect(hWnd, nullptr, FALSE);
            }
            // 2. Middle mouse pan
            else if (g_app.isMidDragging) {
                POINT curMouse = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                g_app.hasMidMoved = true;
                g_app.viewport.offsetX += (curMouse.x - g_app.lastMidMouse.x);
                g_app.viewport.offsetY += (curMouse.y - g_app.lastMidMouse.y);
                g_app.lastMidMouse = curMouse;
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            // 3. Right mouse window move
            else if (g_app.isWindowDragging) {
                POINT curMouse = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                ClientToScreen(hWnd, &curMouse);

                int dx = curMouse.x - g_app.lastWindowMouse.x;
                int dy = curMouse.y - g_app.lastWindowMouse.y;

                RECT rc;
                GetWindowRect(hWnd, &rc);

                SetWindowPos(hWnd, nullptr, rc.left + dx, rc.top + dy, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

                g_app.lastWindowMouse = curMouse;
            }
            return 0;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            RenderViewport(hWnd, hdc);
            EndPaint(hWnd, &ps);
            return 0;
        }

        case WM_NCCALCSIZE:
            return 0;

        case WM_NCPAINT:
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hWnd, message, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    typedef BOOL(WINAPI *SETDPI)(DPI_AWARENESS_CONTEXT);
    if (HMODULE hU32 = GetModuleHandleW(L"user32.dll")) {
        if (auto setDpi = reinterpret_cast<SETDPI>(GetProcAddress(hU32, "SetProcessDpiAwarenessContext"))) {
            setDpi(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        }
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_app.wicFactory));

    WNDCLASSW wc{ 0 };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"FastStoneCloneWIC";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    HWND hWnd = CreateViewerWindow(hInstance, wc.lpszClassName);

    // Apply DWM style before showing to avoid Win8 border flash
    DWORD corner = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(hWnd, 33, &corner, sizeof(corner));

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);
    OpenInitialImage(hWnd);

    MSG msg{};
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (g_app.hDIB) DeleteObject(g_app.hDIB);
    g_app.wicFactory.Reset();
    CoUninitialize();

    return static_cast<int>(msg.wParam);
}