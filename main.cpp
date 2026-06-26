#include <windows.h>
#include <windowsx.h> // Required for mouse coordinate macros
#include "AppState.h"
#include "FileHandler.h"
#include "WicDecoder.h"
#include "Renderer.h"
#include "DpiAwareInit.h"
#include <dwmapi.h>

AppState g_app;

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_KEYDOWN:
            // Check for ESC key OR (W key AND Ctrl is held down)
            if (wParam == VK_ESCAPE || (wParam == 'W' && (GetKeyState(VK_CONTROL) & 0x8000))) {
                PostQuitMessage(0);
            }
            return 0;

        // --- NEW: Resize logic ---
        case WM_NCHITTEST: {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            RECT rc;
            GetWindowRect(hWnd, &rc);

            const int border = 8;
            bool top = pt.y < rc.top + border;
            bool bottom = pt.y >= rc.bottom - border;
            bool left = pt.x < rc.left + border;
            bool right = pt.x >= rc.right - border;

            if (top && left) return HTTOPLEFT;
            if (top && right) return HTTOPRIGHT;
            if (bottom && left) return HTBOTTOMLEFT;
            if (bottom && right) return HTBOTTOMRIGHT;
            if (top) return HTTOP;
            if (bottom) return HTBOTTOM;
            if (left) return HTLEFT;
            if (right) return HTRIGHT;

            return HTCLIENT;
        }

        case WM_SIZING:
            InvalidateRect(hWnd, nullptr, FALSE);
            return TRUE;
        // -------------------------

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

        case WM_LBUTTONDOWN:
            g_app.viewport.isDragging = true;
            g_app.viewport.lastMouse = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            SetCapture(hWnd);
            return 0;


        case WM_LBUTTONUP:
            g_app.viewport.isDragging = false;
            ReleaseCapture();
            return 0;

            // --- RBUTTON Window Move Logic ---
        case WM_RBUTTONDOWN: {
            g_app.isWindowDragging = true;
            // Get mouse pos, convert to screen coords immediately
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ClientToScreen(hWnd, &pt);
            g_app.lastWindowMouse = pt;
            SetCapture(hWnd);
            return 0;
        }

        case WM_RBUTTONUP: {
            g_app.isWindowDragging = false;
            ReleaseCapture();
            return 0;
        }

            // --- Combined MouseMove logic ---
        case WM_MOUSEMOVE: {
            // 1. Viewport Panning (LMB)
            if (g_app.viewport.isDragging) {
                POINT curMouse = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                g_app.viewport.offsetX += (curMouse.x - g_app.viewport.lastMouse.x);
                g_app.viewport.offsetY += (curMouse.y - g_app.viewport.lastMouse.y);
                g_app.viewport.lastMouse = curMouse;
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            // 2. Window Moving (RMB)
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
        case WM_CREATE: {
            BOOL value = TRUE;
            // Enable rounded corners (DWMWA_WINDOW_CORNER_PREFERENCE = 33)
            DwmSetWindowAttribute(hWnd, 33, &value, sizeof(value));
            return 0;
        }
        case WM_NCCALCSIZE: {
            if (wParam == TRUE) {
                // Inflate the rectangle by 1 pixel to allow the DWM compositor
                // to render the shadow and rounded corners correctly.
                NCCALCSIZE_PARAMS* pParams = (NCCALCSIZE_PARAMS*)lParam;
                InflateRect(&pParams->rgrc[0], 1, 1);
                return 0;
            }
            return DefWindowProcW(hWnd, message, wParam, lParam);
        }

        case WM_NCPAINT:
            return 0; // Prevents the OS from drawing the caption bar

        case WM_ERASEBKGND:
            return 1; // Prevents flicker

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