#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <dwmapi.h>
#include <intsafe.h>
#include <uxtheme.h>

#include "AppState.h"
#include "Constants.h"
#include "DropTarget.h"
#include "FileHandler.h"
#include "HelpWindow.h"
#include "MouseHandler.h"
#include "WicDecoder.h"

#include <windows.h>
#include <windowsx.h>

#include <shellapi.h> // Parsing command line arguments
#include <string>     // Handling string paths
#include <memory>     // Needed for std::unique_ptr for renderer management

#include "DpiAwareInit.h"

#include "resources/resource.h"
#include "RegistrySetup.h"

#include "Renderer/RendererD2D.h"
#include "Renderer/RendererGDI.h"
#include "WorkerThread.h"
#include <shlobj.h> // Required for SHOpenFolderAndSelectItems
// Global application state
AppState g_app;
DropTarget *g_pDropTarget = nullptr;
// Define the storage for the globals exactly once in your entry point file
WorkerThread g_ioWorker;
WorkerThread g_decoderWorker;

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        // Handle file paths sent from other instances of the viewer
        case WM_COPYDATA: {
            COPYDATASTRUCT *cds = (COPYDATASTRUCT *) lParam;
            if (cds->dwData == 1) {
                // Create a local copy to ensure safety even if processing takes time
                std::wstring safePath((LPCWSTR) cds->lpData);

                // --- 1. LOAD THE NEW IMAGE ---
                OpenSpecificImage(hWnd, safePath.c_str());

                // --- 2. WAKE UP & CENTER ---
                // (viewport already reset inside LoadImageIndex via OpenSpecificImage)
                ShowWindow(hWnd, SW_RESTORE);
                SetForegroundWindow(hWnd); // Bring to front
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            return TRUE;
        }

        case WM_KEYDOWN: {
            bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            if (wParam == 'E' || wParam == VK_TAB) {
                if (!g_app.playlist.empty() && g_app.currentIndex >= 0) {
                    const std::wstring &path = g_app.playlist[g_app.currentIndex];
                    PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(path.c_str());
                    if (pidl) {
                        SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
                        ILFree(pidl);
                    }
                }
                return 0;
            }
            if (wParam == 'H') {
                g_app.viewport.flippedH = !g_app.viewport.flippedH;
                InvalidateRect(hWnd, nullptr, FALSE);
                return 0;
            }
            if (wParam == 'V') {
                g_app.viewport.flippedV = !g_app.viewport.flippedV;
                InvalidateRect(hWnd, nullptr, FALSE);
                return 0;
            }
            // Rotate Image: R (Clockwise) or Ctrl+R (Counter-Clockwise)
            if (wParam == 'R') {
                if (shift) {
                    // Counter-Clockwise
                    g_app.viewport.rotation = (g_app.viewport.rotation - 90 + 360) % 360;
                } else {
                    // Clockwise
                    g_app.viewport.rotation = (g_app.viewport.rotation + 90) % 360;
                }
                InvalidateRect(hWnd, nullptr, FALSE);
                return 0;
            }
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
                    MONITORINFO mi = {sizeof(mi)};
                    GetMonitorInfo(MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST), &mi);
                    SetWindowPos(hWnd, HWND_TOPMOST,
                                 mi.rcMonitor.left, mi.rcMonitor.top,
                                 mi.rcMonitor.right - mi.rcMonitor.left,
                                 mi.rcMonitor.bottom - mi.rcMonitor.top,
                                 SWP_FRAMECHANGED);
                    DWMNCRENDERINGPOLICY policy = DWMNCRP_DISABLED;
                    DwmSetWindowAttribute(hWnd, DWMWA_NCRENDERING_POLICY, &policy, sizeof(policy));
                    DWORD corner = 1; // DWMWCP_DONOTROUND
                    DwmSetWindowAttribute(hWnd, 33, &corner, sizeof(corner));
                    MARGINS margins = {0, 0, 0, 0};
                    DwmExtendFrameIntoClientArea(hWnd, &margins);
                    g_app.isFullscreen = true;
                } else {
                    SetWindowPos(hWnd, HWND_NOTOPMOST,
                                 g_app.savedWindowRect.left,
                                 g_app.savedWindowRect.top,
                                 g_app.savedWindowRect.right - g_app.savedWindowRect.left,
                                 g_app.savedWindowRect.bottom - g_app.savedWindowRect.top,
                                 SWP_FRAMECHANGED);
                    DWMNCRENDERINGPOLICY policy = DWMNCRP_ENABLED;
                    DwmSetWindowAttribute(hWnd, DWMWA_NCRENDERING_POLICY, &policy, sizeof(policy));
                    DWORD corner = 2; // DWMWCP_ROUND
                    DwmSetWindowAttribute(hWnd, 33, &corner, sizeof(corner));
                    MARGINS margins = {1, 1, 1, 1};
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
                            LoadImageIndex(
                                hWnd, (g_app.currentIndex - 1 + g_app.playlist.size()) % g_app.playlist.size());
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
                        g_app.viewport.zoom = 1.0f;
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

            POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
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

        // Window size changed: Update renderer
        case WM_SIZE:
        case WM_SIZING:
            if (g_app.renderer) {
                g_app.renderer->Resize(LOWORD(lParam), HIWORD(lParam));
            }
            InvalidateRect(hWnd, nullptr, FALSE);
            return TRUE;

        // --- CLEAN MOUSE HANDLERS ---
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
            // This is the direct entry point for zoom and drag
            MouseHandler::HandleButtonDown(hWnd, message, lParam);
            return 0;

        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
            MouseHandler::HandleButtonUp(hWnd, message, lParam);
            return 0;

        case WM_MOUSEMOVE:
            MouseHandler::HandleMouseMove(hWnd, lParam);
            return 0;

        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
            if (message == WM_MBUTTONDOWN) MouseHandler::HandleButtonDown(hWnd, message, lParam);
            else MouseHandler::HandleButtonUp(hWnd, message, lParam);
            return 0;
        case WM_MOUSEWHEEL: {
            // Check if Shilft is held down
            bool isShiftDown = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

            // Check for Middle Mouse button scroll (Standard wheel)
            // Note: If you want to restrict this specifically to Middle Mouse Scroll:
            // Windows WM_MOUSEWHEEL usually reports the wheel delta.
            // If you need to verify middle button is held, check GetKeyState(VK_MBUTTON).

            if (isShiftDown) {
                int delta = GET_WHEEL_DELTA_WPARAM(wParam);

                // Adjust opacity (stepping by 15 for a smooth fade feel)
                if (delta > 0) {
                    g_app.opacity = (g_app.opacity + Config::OPACITY_STEP > 255)
                                        ? 255
                                        : (g_app.opacity + Config::OPACITY_STEP);
                } else {
                    g_app.opacity = (g_app.opacity - Config::OPACITY_STEP < 10)
                                        ? 10
                                        : (g_app.opacity - Config::OPACITY_STEP);
                }

                // Apply to the window
                SetLayeredWindowAttributes(hWnd, 0, g_app.opacity, LWA_ALPHA);
                return 0; // Handled
            }

            // Fallback to standard zoom
            MouseHandler::HandleMouseWheel(hWnd, wParam, lParam);
            return 0;
        }

        case WM_LBUTTONDBLCLK: {
            // 1. Defer painting to prevent jitter
            SendMessageW(hWnd, WM_SETREDRAW, FALSE, 0);

            if (!g_app.isFullscreen) {
                GetWindowRect(hWnd, &g_app.savedWindowRect);
                MONITORINFO mi = {sizeof(mi)};
                GetMonitorInfo(MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST), &mi);

                SetWindowPos(hWnd, HWND_TOPMOST,
                             mi.rcMonitor.left, mi.rcMonitor.top,
                             mi.rcMonitor.right - mi.rcMonitor.left,
                             mi.rcMonitor.bottom - mi.rcMonitor.top,
                             SWP_FRAMECHANGED | SWP_NOCOPYBITS); // Add NOCOPYBITS

                DWMNCRENDERINGPOLICY policy = DWMNCRP_DISABLED;
                DwmSetWindowAttribute(hWnd, DWMWA_NCRENDERING_POLICY, &policy, sizeof(policy));
                DWORD corner = 1;
                DwmSetWindowAttribute(hWnd, 33, &corner, sizeof(corner));
                MARGINS margins = {0, 0, 0, 0};
                DwmExtendFrameIntoClientArea(hWnd, &margins);
                g_app.isFullscreen = true;
            } else {
                SetWindowPos(hWnd, HWND_NOTOPMOST,
                             g_app.savedWindowRect.left,
                             g_app.savedWindowRect.top,
                             g_app.savedWindowRect.right - g_app.savedWindowRect.left,
                             g_app.savedWindowRect.bottom - g_app.savedWindowRect.top,
                             SWP_FRAMECHANGED | SWP_NOCOPYBITS); // Add NOCOPYBITS

                DWMNCRENDERINGPOLICY policy = DWMNCRP_ENABLED;
                DwmSetWindowAttribute(hWnd, DWMWA_NCRENDERING_POLICY, &policy, sizeof(policy));
                DWORD corner = 2;
                DwmSetWindowAttribute(hWnd, 33, &corner, sizeof(corner));
                MARGINS margins = {1, 1, 1, 1};
                DwmExtendFrameIntoClientArea(hWnd, &margins);
                g_app.isFullscreen = false;
            }

            // 2. Resume drawing and force an immediate update
            SendMessageW(hWnd, WM_SETREDRAW, TRUE, 0);
            RedrawWindow(hWnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_FRAME);

            return 0;
        }
        case WM_CAPTURECHANGED: {
            // If capture is lost (e.g., ALT+TAB or mouse released outside),
            // kill timer and clean up state.
            g_app.viewport.isDragging = false;
            g_app.isWindowDragging = false;
            ReleaseCapture();
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

        case WM_CLOSE: {
            // Force exit if a debugger is attached, or if we are in Debug build
            if (IsDebuggerPresent()) {
                PostQuitMessage(0);
            } else {
#ifdef DEBUG_BUILD
                PostQuitMessage(0);
#else
                ShowWindow(hWnd, SW_HIDE);
#endif
            }
            return 0;
        }

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
    typedef BOOL (WINAPI *SETDPI)(DPI_AWARENESS_CONTEXT);
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
    std::wstring mutexName = L"QuickImageViewer_SingleInstanceMutex" + (bypassMutex
                                                                            ? std::to_wstring(GetTickCount())
                                                                            : L"");
    HANDLE hMutex = CreateMutexW(NULL, TRUE, mutexName.c_str());
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND hExistingWnd = FindWindowW(Config::WINDOW_CLASS_NAME, nullptr);
        if (hExistingWnd) {
            int argc;
            LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
            if (argc > 1) {
                // Safe, non-aggregate initialization
                COPYDATASTRUCT cds;
                cds.dwData = 1;
                cds.cbData = (DWORD) ((wcslen(argv[1]) + 1) * sizeof(wchar_t));
                cds.lpData = (void *) argv[1];

                SendMessageW(hExistingWnd, WM_COPYDATA, 0, (LPARAM) &cds);
            }
            LocalFree(argv);
        }
        ReleaseMutex(hMutex);
        return 0;
    }

    WNDCLASSW wc{0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = Config::WINDOW_CLASS_NAME;
    wc.style = CS_DBLCLKS;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_APP_ICON));
    RegisterClassW(&wc);

    HWND hWnd = CreateViewerWindow(hInstance, wc.lpszClassName);
    SetWindowLongW(hWnd, GWL_EXSTYLE, GetWindowLongW(hWnd, GWL_EXSTYLE) | WS_EX_LAYERED);
    SetLayeredWindowAttributes(hWnd, 0, g_app.opacity, LWA_ALPHA);
    UINT dpi = GetDpiForWindow(hWnd);
    g_app.dpiScale = (float) dpi / 96.0f; // Calculate scale factor once
    g_app.screenW = GetSystemMetrics(SM_CXSCREEN);
    g_app.screenH = GetSystemMetrics(SM_CYSCREEN);
    // Initialize Renderer (D2D with GDI fallback)
    g_app.renderer = std::make_unique<RendererD2D>();
    if (FAILED(g_app.renderer->Initialize(hWnd))) {
        g_app.renderer = std::make_unique<RendererGDI>();
        g_app.renderer->Initialize(hWnd);
    }
#ifdef _DEBUG
    if (g_app.renderer) {
        if (dynamic_cast<RendererD2D *>(g_app.renderer.get())) {
            OutputDebugStringW(L"RENDERER: Using Direct2D (GPU Acceleration Active)\n");
        } else {
            OutputDebugStringW(L"RENDERER: Using GDI (Fallback Mode)\n");
        }
    }
#endif
    // Register Drag/Drop and Help
    RegisterDragDrop(hWnd, (g_pDropTarget = new DropTarget(hWnd)));
    UI::InitHelpWindow(hInstance, hWnd);
    DWORD corner = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(hWnd, 33, &corner, sizeof(corner));

    // Handle startup arguments
    int argc;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc > 1 && std::wstring(argv[1]) == L"-background") ShowWindow(hWnd, SW_HIDE);
    else {
        ShowWindow(hWnd, nCmdShow);
        UpdateWindow(hWnd);
        if (argc > 1) OpenSpecificImage(hWnd, argv[1]);
        else OpenInitialImage(hWnd);
    }
    LocalFree(argv);

    MSG msg{};
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // 11. --- CRITICAL CLEANUP ---
    // The message loop has exited; this is where we safely destroy
    // resources before the process fully terminates.
    g_app.renderer.reset();

    if (g_app.wicFactory) {
        g_app.wicFactory.Reset();
    }

    if (g_pDropTarget) {
        RevokeDragDrop(hWnd);
        g_pDropTarget->Release();
        g_pDropTarget = nullptr;
    }

    // Optional: Unregister the class for total cleanliness
    UnregisterClassW(Config::WINDOW_CLASS_NAME, hInstance);

    CoUninitialize();
    OleUninitialize();

    // Release the single-instance mutex
    if (hMutex) {
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
    }

    return static_cast<int>(msg.wParam); // WinMain concludes here
}
