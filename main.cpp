#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <shellapi.h> // <-- Added for command line parsing
#include <string>     // <-- Added for wstring
#include "HelpWindow.h"
#include "AppState.h"
#include "FileHandler.h"
#include "WicDecoder.h"
#include "Renderer.h"
#include "DpiAwareInit.h"
#include "Constants.h"
#include "resources/resource.h"
#include "RegistrySetup.h"

AppState g_app;

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {

        case WM_COPYDATA: {
            COPYDATASTRUCT* cds = (COPYDATASTRUCT*)lParam;
            if (cds->dwData == 1) { // 1 is our custom ID for "Load this image"
                LPCWSTR filePath = (LPCWSTR)cds->lpData;

                // --- 1. LOAD THE NEW IMAGE ---
                OpenSpecificImage(hWnd, filePath);


                // --- 2. WAKE UP & CENTER ---
                g_app.viewport.zoom    = 1.0f;
                g_app.viewport.offsetX = 0.0f;
                g_app.viewport.offsetY = 0.0f;

                ShowWindow(hWnd, SW_RESTORE);
                SetForegroundWindow(hWnd); // Bring to front
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            return TRUE;
        }

        case WM_KEYDOWN: {
            bool shift = (GetKeyState(VK_SHIFT)   & 0x8000) != 0;
            bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;

            // True Hard Quit (Ctrl + Q) to flush from RAM completely
            if (wParam == 'Q' && ctrl) {
                PostQuitMessage(0);
                return 0;
            }
            if (wParam == VK_F1) {
                UI::ToggleHelpWindow();
                return 0;
            }
            // Hide to RAM instead of quitting (Esc or Ctrl + W)
            if (wParam == VK_ESCAPE || (wParam == 'W' && ctrl)) {
                ShowWindow(hWnd, SW_HIDE);
                return 0;
            }

            // Open a New Blank Window (Ctrl + N)
            if (wParam == 'N' && ctrl) {
                wchar_t exePath[MAX_PATH];
                GetModuleFileNameW(nullptr, exePath, MAX_PATH);

                SetEnvironmentVariableW(L"QIV_NEW_INSTANCE", L"1");
                ShellExecuteW(nullptr, L"open", exePath, nullptr, nullptr, SW_SHOW);
                SetEnvironmentVariableW(L"QIV_NEW_INSTANCE", nullptr);
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
                    case VK_UP:
                    case VK_ADD:
                    case VK_OEM_PLUS:
                        g_app.viewport.zoom *= Config::ZOOM_STEP;
                        InvalidateRect(hWnd, nullptr, FALSE);
                        break;
                    case VK_DOWN:
                    case VK_SUBTRACT:
                    case VK_OEM_MINUS:
                        g_app.viewport.zoom /= Config::ZOOM_STEP;
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
        case WM_SIZE:
        case WM_SIZING:
            InvalidateRect(hWnd, nullptr, FALSE);
            return TRUE;

        case WM_MOUSEWHEEL: {
            if (g_app.playlist.empty()) return 0;
            int zDelta = GET_WHEEL_DELTA_WPARAM(wParam);
            if (GET_KEYSTATE_WPARAM(wParam) & MK_CONTROL) {
                g_app.viewport.zoom *= (zDelta > 0) ? Config::ZOOM_STEP : (1.0f / Config::ZOOM_STEP);
            } else {
                int step = (zDelta < 0) ? 1 : -1;
                int newIdx = (g_app.currentIndex + step + g_app.playlist.size()) % g_app.playlist.size();
                LoadImageIndex(hWnd, newIdx);
            }
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;
        }

        case WM_LBUTTONDOWN: {
            if (g_app.isFullscreen) return 0;
            g_app.isWindowDragging = true;
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ClientToScreen(hWnd, &pt);
            g_app.lastWindowMouse = pt;
            SetCapture(hWnd);
            return 0;
        }

        case WM_LBUTTONUP:
            g_app.isWindowDragging = false;
            ReleaseCapture();
            return 0;

        case WM_MBUTTONDOWN: {
            g_app.isMidDragging = true;
            g_app.hasMidMoved   = false;
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ClientToScreen(hWnd, &pt);
            g_app.lastMidMouse  = pt;
            SetCapture(hWnd);
            return 0;
        }

        case WM_MBUTTONUP: {
            if (!g_app.hasMidMoved) {
                g_app.viewport.zoom    = 1.0f;
                g_app.viewport.offsetX = 0.0f;
                g_app.viewport.offsetY = 0.0f;

                if (!g_app.isFullscreen) {
                    UINT dpi = GetDpiForWindow(hWnd);
                    int winW = MulDiv(Config::BASE_WIDTH, dpi, 96);
                    int winH = MulDiv(Config::BASE_HEIGHT, dpi, 96);

                    MONITORINFO mi = { sizeof(mi) };
                    GetMonitorInfo(MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST), &mi);

                    int posX = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - winW) / 2;
                    int posY = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - winH) / 2;

                    SetWindowPos(hWnd, nullptr, posX, posY, winW, winH,
                                 SWP_NOZORDER | SWP_NOACTIVATE);
                }
                InvalidateRect(hWnd, nullptr, FALSE);
            }

            g_app.isMidDragging = false;
            g_app.hasMidMoved   = false;
            ReleaseCapture();
            return 0;
        }

        case WM_RBUTTONDOWN:
            SetCursor(NULL);
            g_app.savedZoom    = g_app.viewport.zoom;
            g_app.savedOffsetX = g_app.viewport.offsetX;
            g_app.savedOffsetY = g_app.viewport.offsetY;
            g_app.viewport.zoom *= Config::ZOOM_LMB;
            g_app.viewport.lastMouse = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            g_app.viewport.isDragging = true;
            SetCapture(hWnd);
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;

        case WM_RBUTTONUP:
            SetCursor(LoadCursor(nullptr, IDC_ARROW));
            g_app.viewport.zoom    = g_app.savedZoom;
            g_app.viewport.offsetX = g_app.savedOffsetX;
            g_app.viewport.offsetY = g_app.savedOffsetY;
            g_app.viewport.isDragging = false;
            ReleaseCapture();
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;

        case WM_MOUSEMOVE: {
            if (g_app.viewport.isDragging) {
                POINT curMouse = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                g_app.viewport.offsetX -= (curMouse.x - g_app.viewport.lastMouse.x);
                g_app.viewport.offsetY -= (curMouse.y - g_app.viewport.lastMouse.y);
                g_app.viewport.lastMouse = curMouse;

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
            else if (g_app.isMidDragging) {
                POINT curMouse = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                ClientToScreen(hWnd, &curMouse);
                g_app.hasMidMoved = true;

                if (!g_app.isFullscreen) {
                    int dx = curMouse.x - g_app.lastMidMouse.x;
                    int dy = curMouse.y - g_app.lastMidMouse.y;

                    RECT rc;
                    GetWindowRect(hWnd, &rc);

                    SetWindowPos(hWnd, nullptr, 0, 0,
                                 (rc.right - rc.left) + dx,
                                 (rc.bottom - rc.top) + dy,
                                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
                }

                g_app.lastMidMouse = curMouse;
                InvalidateRect(hWnd, nullptr, FALSE);
            }
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

        case WM_CLOSE:
            // Intercept the Top-Right 'X' button. Hide instead of destroy.
            ShowWindow(hWnd, SW_HIDE);
            return 0;

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

    // Check registry and auto-start rules
    System::RegisterAppForOpenWith();
    System::EnableRunOnStartup();

    // --- SINGLE INSTANCE & RAM RESIDENT LOGIC ---
    bool bypassMutex = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    if (GetEnvironmentVariableW(L"QIV_NEW_INSTANCE", nullptr, 0) > 0) {
        bypassMutex = true;
    }

    std::wstring mutexName = L"QuickImageViewer_SingleInstanceMutex";
    if (bypassMutex) {
        // Randomize Mutex to force Windows to spawn a new memory instance
        mutexName += std::to_wstring(GetTickCount());
    }

    HANDLE hMutex = CreateMutexW(NULL, TRUE, mutexName.c_str());
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // App is already running in RAM. Pass the file to it.
        HWND hExistingWnd = FindWindowW(L"FastStoneCloneWIC", nullptr);
        if (hExistingWnd) {
            int argc;
            LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
            if (argc > 1) {
                COPYDATASTRUCT cds;
                cds.dwData = 1;
                cds.cbData = (wcslen(argv[1]) + 1) * sizeof(wchar_t);
                cds.lpData = (PVOID)argv[1];
                SendMessageW(hExistingWnd, WM_COPYDATA, (WPARAM)NULL, (LPARAM)&cds);
            }
            LocalFree(argv);
        }
        ReleaseMutex(hMutex);
        return 0; // Close this duplicate process instantly
    }
    // --------------------------------------------

    WNDCLASSW wc{ 0 };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"FastStoneCloneWIC";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_APP_ICON));
    RegisterClassW(&wc);

    HWND hWnd = CreateViewerWindow(hInstance, wc.lpszClassName);
    UI::InitHelpWindow(hInstance, hWnd);
    DWORD corner = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(hWnd, 33, &corner, sizeof(corner));

    // --- CHECK FOR STARTUP FLAGS & HANDLE SHOWING ---
    bool startHidden = false;
    bool hasImage = false;

    int argc;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc > 1) {
        std::wstring firstArg = argv[1];
        if (firstArg == L"-background") {
            startHidden = true; // Windows booted us in the background
        } else {
            hasImage = true;    // Windows passed us a file via Open With
        }
    }

    if (startHidden) {
        // Hide in RAM instantly, wait for a WM_COPYDATA message
        ShowWindow(hWnd, SW_HIDE);
    } else {
        // Normal launch
        ShowWindow(hWnd, nCmdShow);
        UpdateWindow(hWnd);

        if (hasImage) {
            OpenSpecificImage(hWnd, argv[1]);
            // Example: OpenSpecificImage(hWnd, argv[1]);
        } else {
            // Double clicked the raw .exe, open the initial dialog/folder
            OpenInitialImage(hWnd);
        }
    }
    LocalFree(argv);
    // ------------------------------------------------

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