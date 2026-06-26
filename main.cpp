#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <shellapi.h> // Parsing command line arguments
#include <string>     // Handling string paths
#include <memory>     // Needed for std::unique_ptr for renderer management
#include "HelpWindow.h"
#include "AppState.h"
#include "FileHandler.h"
#include "WicDecoder.h"
#include "Renderer.h"
#include "DpiAwareInit.h"
#include "Constants.h"
#include "resources/resource.h"
#include "RegistrySetup.h"
#include "DropTarget.h"
#include "Renderer/RendererD2D.h"
#include "Renderer/RendererGDI.h"

// Global application state
AppState g_app;
DropTarget* g_pDropTarget = nullptr;

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {

        // Handle file paths sent from other instances of the viewer
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
            // Toggle Help Menu (F1)
            if (wParam == VK_F1) {
                UI::ToggleHelpWindow();
                return 0;
            }
            // Open File Dialog (F2)
            if (wParam == VK_F2) {
                OpenInitialImage(hWnd);
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

        // Window size changed: Update renderer
        case WM_SIZE:
        case WM_SIZING:
            if (g_app.renderer) {
                g_app.renderer->Resize(LOWORD(lParam), HIWORD(lParam));
            }
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

        // PAINT: Using the polymorphic renderer
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hWnd, &ps);
            if (g_app.renderer) {
                g_app.renderer->Render();
            }
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
#ifdef DEBUG_BUILD
            DestroyWindow(hWnd);
#else
            ShowWindow(hWnd, SW_HIDE);
#endif
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hWnd, message, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    // 1. Initialize OLE
    if (FAILED(OleInitialize(nullptr))) return 0;

    // Set DPI awareness
    typedef BOOL(WINAPI *SETDPI)(DPI_AWARENESS_CONTEXT);
    if (HMODULE hU32 = GetModuleHandleW(L"user32.dll")) {
        if (auto setDpi = reinterpret_cast<SETDPI>(GetProcAddress(hU32, "SetProcessDpiAwarenessContext"))) {
            setDpi(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        }
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_app.wicFactory));

    // Register and startup
    System::RegisterAppForOpenWith();
    System::EnableRunOnStartup();

    // --- SINGLE INSTANCE & RAM RESIDENT LOGIC ---
    bool bypassMutex = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    if (GetEnvironmentVariableW(L"QIV_NEW_INSTANCE", nullptr, 0) > 0) bypassMutex = true;
    std::wstring mutexName = L"QuickImageViewer_SingleInstanceMutex" + (bypassMutex ? std::to_wstring(GetTickCount()) : L"");
    HANDLE hMutex = CreateMutexW(NULL, TRUE, mutexName.c_str());
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND hExistingWnd = FindWindowW(L"FastStoneCloneWIC", nullptr);
        if (hExistingWnd) {
            int argc; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
            if (argc > 1) {
                COPYDATASTRUCT cds{ 1, (DWORD)(wcslen(argv[1]) + 1) * sizeof(wchar_t), (PVOID)argv[1] };
                SendMessageW(hExistingWnd, WM_COPYDATA, 0, (LPARAM)&cds);
            }
            LocalFree(argv);
        }
        ReleaseMutex(hMutex);
        return 0;
    }

    WNDCLASSW wc{ 0 };
    wc.lpfnWndProc = WndProc; wc.hInstance = hInstance; wc.lpszClassName = L"FastStoneCloneWIC";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_APP_ICON));
    RegisterClassW(&wc);

    HWND hWnd = CreateViewerWindow(hInstance, wc.lpszClassName);

    // Initialize Renderer (D2D with GDI fallback)
    g_app.renderer = std::make_unique<RendererD2D>();
    if (FAILED(g_app.renderer->Initialize(hWnd))) {
        g_app.renderer = std::make_unique<RendererGDI>();
        g_app.renderer->Initialize(hWnd);
    }

    // Register Drag/Drop and Help
    RegisterDragDrop(hWnd, (g_pDropTarget = new DropTarget(hWnd)));
    UI::InitHelpWindow(hInstance, hWnd);
    DWORD corner = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(hWnd, 33, &corner, sizeof(corner));

    // Handle startup arguments
    int argc; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc > 1 && std::wstring(argv[1]) == L"-background") ShowWindow(hWnd, SW_HIDE);
    else { ShowWindow(hWnd, nCmdShow); UpdateWindow(hWnd); if (argc > 1) OpenSpecificImage(hWnd, argv[1]); else OpenInitialImage(hWnd); }
    LocalFree(argv);

    MSG msg{};
    while (GetMessage(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); }

    // Cleanup
    g_app.renderer.reset();
    g_app.wicFactory.Reset();
    RevokeDragDrop(hWnd);
    if (g_pDropTarget) g_pDropTarget->Release();
    CoUninitialize();

    return static_cast<int>(msg.wParam);
}