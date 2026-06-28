#include <algorithm>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <dwmapi.h>
#include <intsafe.h>
#include <uxtheme.h>

#include "../AppState.h"
#include "Platform/Constants.h"
#include "../DropTarget.h"
#include "Platform/FileHandler.h"
#include "UI/HelpWindow.h"
#include "Platform/MouseHandler.h"
#include "../WicDecoder.h"

#include <windows.h>
#include <windowsx.h>

#include <shellapi.h> // Parsing command line arguments
#include <string>     // Handling string paths
#include <memory>     // Needed for std::unique_ptr for renderer management

#include "Platform/DpiAwareInit.h"

#include "../resources/resource.h"
#include "Platform/RegistrySetup.h"

#include "Renderer/RendererD2D.h"
#include "Renderer/RendererGDI.h"
#include "WorkerThread.h"
#include <shlobj.h> // Required for SHOpenFolderAndSelectItems

// Global application state
AppState g_app;
DropTarget *g_pDropTarget = nullptr;

// Define the storage for the globals exactly once in your entry point file
//   g_ioWorker      – IoThreadPool: started lazily in FileHandler once the
//                     target drive is known (1 thread HDD, 2 threads SSD/NVMe)
//   g_decoderWorker – WorkerThread(true): WIC decode + pixel convert
IoThreadPool g_ioWorker;
WorkerThread g_decoderWorker(true);

static void ToggleFullscreen(HWND hWnd) {
    if (!g_app.isFullscreen) {
        GetWindowRect(hWnd, &g_app.savedWindowRect);
        MONITORINFO mi = {sizeof(mi)};
        GetMonitorInfo(MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST), &mi);

        SetWindowPos(hWnd, HWND_TOPMOST,
                     mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_NOCOPYBITS);

        DWMNCRENDERINGPOLICY policy = DWMNCRP_DISABLED;
        DwmSetWindowAttribute(hWnd, DWMWA_NCRENDERING_POLICY, &policy, sizeof(policy));
        DWORD corner = 1; // DWMWCP_DONOTROUND
        DwmSetWindowAttribute(hWnd, Constants::DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
        MARGINS margins = {0, 0, 0, 0};
        DwmExtendFrameIntoClientArea(hWnd, &margins);

        g_app.isFullscreen = true;
    } else {
        SetWindowPos(hWnd, HWND_NOTOPMOST,
                     g_app.savedWindowRect.left,
                     g_app.savedWindowRect.top,
                     g_app.savedWindowRect.right - g_app.savedWindowRect.left,
                     g_app.savedWindowRect.bottom - g_app.savedWindowRect.top,
                     SWP_FRAMECHANGED | SWP_NOCOPYBITS);

        DWMNCRENDERINGPOLICY policy = DWMNCRP_ENABLED;
        DwmSetWindowAttribute(hWnd, DWMWA_NCRENDERING_POLICY, &policy, sizeof(policy));
        DWORD corner = 2; // DWMWCP_ROUND
        DwmSetWindowAttribute(hWnd, Constants::DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
        MARGINS margins = {1, 1, 1, 1};
        DwmExtendFrameIntoClientArea(hWnd, &margins);

        g_app.isFullscreen = false;
    }
}

static void UpdateRendererColorEffects(HWND hWnd) {
    if (g_app.renderer) {
        g_app.renderer->UpdateColorEffects();
    }

    InvalidateRect(hWnd, nullptr, FALSE);
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_DPICHANGED: {
            // 1. Update your global scale
            g_app.dpiScale = static_cast<float>(HIWORD(wParam)) / 96.0f;

            // 2. Refresh the Renderer's font format
            g_app.renderer->UpdateTextFormat();
            RECT *const prcNewWindow = (RECT *) lParam;
            SetWindowPos(hWnd,
                         nullptr,
                         prcNewWindow->left,
                         prcNewWindow->top,
                         prcNewWindow->right - prcNewWindow->left,
                         prcNewWindow->bottom - prcNewWindow->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;
        }

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
        case WM_TIMER: {
            constexpr UINT_PTR TIMER_LOOKASIDE = 1001;

            if (wParam == TIMER_LOOKASIDE) {
                KillTimer(hWnd, TIMER_LOOKASIDE);

                if (g_app.playlist.empty()) return 0;

                int index = g_app.currentIndex;
                const int total = static_cast<int>(g_app.playlist.size());

                for (int i = 1; i <= Constants::PRELOAD_LOOKASIDE_COUNT; ++i) {
                    int fwd = index + i;
                    int bwd = index - i;

                    if (fwd < total) {
                        std::wstring fwdPath = g_app.playlist[fwd];
                        g_decoderWorker.PushTask([fwdPath, index]() {
                            // ABORT if user started scrolling again
                            if (g_app.wantedIndex.load(std::memory_order_acquire) != index) return;
                            if (g_app.renderer) (void) g_app.renderer->PreloadBitmap(fwdPath, index);
                        });
                    }
                    if (bwd >= 0) {
                        std::wstring bwdPath = g_app.playlist[bwd];
                        g_decoderWorker.PushTask([bwdPath, index]() {
                            // ABORT if user started scrolling again
                            if (g_app.wantedIndex.load(std::memory_order_acquire) != index) return;
                            if (g_app.renderer) (void) g_app.renderer->PreloadBitmap(bwdPath, index);
                        });
                    }
                }
            }
            return 0;
        }
        case WM_KEYDOWN: {
            bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            // 2. Handle your new ViewModes (keys '1' through '5')
            if (wParam >= '1' && wParam <= '5') {
                g_app.viewMode = static_cast<Constants::ViewModes::ViewMode>(wParam - '0');
                InvalidateRect(hWnd, nullptr, FALSE);
                return 0;
            }
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
            // ===============================
            // COLOR EFFECTS
            // ===============================
            // Shift + Delete reset
            if (wParam == VK_DELETE && shift) {
                g_app.saturation = Constants::DEFAULT_SATURATION;
                g_app.brightness = Constants::DEFAULT_BRIGHTNESS;
                g_app.contrast = Constants::DEFAULT_CONTRAST;
                UpdateRendererColorEffects(hWnd);
                return 0;
            }

            // I = grayscale
            if (wParam == 'I') {
                g_app.saturation =
                        (g_app.saturation == 0.0f)
                            ? 1.0f
                            : 0.0f;
                UpdateRendererColorEffects(hWnd);
                return 0;
            }

            // [ saturation -
            if (wParam == VK_OEM_4) {
                g_app.saturation =
                        std::max(
                                0.0f,
                                g_app.saturation - Constants::COLOR_ADJUST_STEP
                                );
                UpdateRendererColorEffects(hWnd);
                return 0;
            }

            // ] saturation +
            if (wParam == VK_OEM_6) {
                g_app.saturation =
                        std::min(
                                2.0f,
                                g_app.saturation + Constants::COLOR_ADJUST_STEP
                                );
                UpdateRendererColorEffects(hWnd);
                return 0;
            }

            // B brightness +
            // Shift+B brightness -
            if (wParam == 'B') {
                if (shift)
                    g_app.brightness -= Constants::COLOR_ADJUST_STEP;
                else
                    g_app.brightness += Constants::COLOR_ADJUST_STEP;
                g_app.brightness =
                        std::clamp(
                                g_app.brightness,
                                -1.0f,
                                1.0f
                                );
                UpdateRendererColorEffects(hWnd);
                return 0;
            }

            // C contrast +
            // Shift+C contrast -
            if (wParam == 'C') {
                if (shift)
                    g_app.contrast -= Constants::COLOR_ADJUST_STEP;
                else
                    g_app.contrast += Constants::COLOR_ADJUST_STEP;
                g_app.contrast =
                        std::clamp(
                                g_app.contrast,
                                0.0f,
                                3.0f
                                );
                UpdateRendererColorEffects(hWnd);
                return 0;
            }
            // Rotate Image: R (Clockwise) or Shift+R (Counter-Clockwise)
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
                if (g_app.GetInstanceCount() <= 1) {
                    ShowWindow(hWnd, SW_HIDE);
                } else {
                    // This is a disposable instance: kill it completely
                    PostQuitMessage(0);
                }
                return 0;
            }
            if (wParam == 'N' && !ctrl) {
                g_app.showOverlayInfoText = !g_app.showOverlayInfoText;
                InvalidateRect(hWnd, nullptr, FALSE);
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
                ToggleFullscreen(hWnd);
                InvalidateRect(hWnd, nullptr, FALSE);
                return 0;
            }

            // Navigation + zoom
            if (!g_app.playlist.empty()) {
                // Cache size as int once to avoid repeated size_t->int casts below
                const int playlistSize = static_cast<int>(g_app.playlist.size());
                switch (wParam) {
                    case VK_LEFT:
                        LoadImageIndex(hWnd, (g_app.currentIndex - 1 + playlistSize) % playlistSize);
                        InvalidateRect(hWnd, nullptr, FALSE);
                        break;
                    case VK_RIGHT:
                        LoadImageIndex(hWnd, (g_app.currentIndex + 1) % playlistSize);
                        InvalidateRect(hWnd, nullptr, FALSE);
                        break;
                    case VK_SPACE:
                        if (shift)
                            LoadImageIndex(hWnd, (g_app.currentIndex - 1 + playlistSize) % playlistSize);
                        else
                            LoadImageIndex(hWnd, (g_app.currentIndex + 1) % playlistSize);
                        InvalidateRect(hWnd, nullptr, FALSE);
                        break;
                    case VK_UP:
                    case VK_ADD:
                    case VK_OEM_PLUS:
                        g_app.viewport.zoom *= Constants::ZOOM_STEP;
                        InvalidateRect(hWnd, nullptr, FALSE);
                        break;
                    case VK_DOWN:
                    case VK_SUBTRACT:
                    case VK_OEM_MINUS:
                        g_app.viewport.zoom /= Constants::ZOOM_STEP;
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

            // Scale the border by the DPI factor
            // Using int to maintain pixel alignment
            const int border = static_cast<int>(8 * g_app.dpiScale);

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
            if (g_app.renderer) {
                g_app.renderer->Resize(LOWORD(lParam), HIWORD(lParam));
            }
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;
        case WM_SIZING:
            if (g_app.renderer) {
                RECT *r = (RECT *) lParam;
                g_app.renderer->Resize(r->right - r->left, r->bottom - r->top);
            }
            InvalidateRect(hWnd, nullptr, FALSE);
            return TRUE;

        // --- CLEAN MOUSE HANDLERS ---
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
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
            bool isShiftDown = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

            if (isShiftDown) {
                int delta = GET_WHEEL_DELTA_WPARAM(wParam);

                if (delta > 0) {
                    g_app.opacity = static_cast<BYTE>(
                        (g_app.opacity + Constants::OPACITY_STEP > 255)
                            ? 255
                            : (g_app.opacity + Constants::OPACITY_STEP));
                } else {
                    g_app.opacity = static_cast<BYTE>(
                        (g_app.opacity - Constants::OPACITY_STEP < 10)
                            ? 10
                            : (g_app.opacity - Constants::OPACITY_STEP));
                }

                SetLayeredWindowAttributes(hWnd, 0, g_app.opacity, LWA_ALPHA);
                return 0;
            }

            MouseHandler::HandleMouseWheel(hWnd, wParam, lParam);
            return 0;
        }

        case WM_MOUSEHWHEEL: {
            bool isRmbDown = (GetKeyState(VK_RBUTTON) & 0x8000) != 0;
            int hDelta = GET_WHEEL_DELTA_WPARAM(wParam);

            if (isRmbDown) {
                RECT rc;
                GetWindowRect(hWnd, &rc);
                int currentW = rc.right - rc.left;
                int currentH = rc.bottom - rc.top;

                int resizeStep = (hDelta > 0) ? 20 : -20;
                int newW = currentW + resizeStep;
                int newH = static_cast<int>(std::round(
                        currentH + resizeStep * (static_cast<float>(currentH) / currentW)));

                int newX = rc.left - (resizeStep / 2);
                int newY = rc.top - (resizeStep / 2);

                SetWindowPos(hWnd, nullptr, newX, newY, newW, newH,
                             SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
                InvalidateRect(hWnd, nullptr, FALSE);
                return 0;
            }

            if (hDelta > 0) {
                g_app.opacity = static_cast<BYTE>(std::min(255, g_app.opacity + Constants::OPACITY_STEP));
            } else {
                g_app.opacity = static_cast<BYTE>(std::max(10, g_app.opacity - Constants::OPACITY_STEP));
            }
            SetLayeredWindowAttributes(hWnd, 0, g_app.opacity, LWA_ALPHA);
            return 0;
        }

        case WM_LBUTTONDBLCLK: {
            SendMessageW(hWnd, WM_SETREDRAW, FALSE, 0);

            ToggleFullscreen(hWnd);

            SendMessageW(hWnd, WM_SETREDRAW, TRUE, 0);
            RedrawWindow(hWnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_FRAME);
            return 0;
        }

        case WM_CAPTURECHANGED: {
            g_app.viewport.isDragging = false;
            g_app.isWindowDragging = false;
            ReleaseCapture();
            return 0;
        }
        case Constants::WM_QIV_REPAINT: {
            // Signal the UI to redraw once the IOWorker confirms the bitmap is ready
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hWnd, &ps);
            if (g_app.renderer) {
                (void) g_app.renderer->Render();
            }
            EndPaint(hWnd, &ps);
            return 0;
        }

        case Constants::WM_QIV_PENDING_UPLOADS: {
            if (g_app.renderer) {
                // 1. Upload the background-decoded VRAM
                g_app.renderer->ProcessPendingUploads();

                // 2. Check if the current image is now ready in the cache
                if (!g_app.playlist.empty()) {
                    const std::wstring &currentPath = g_app.playlist[g_app.currentIndex];

                    // Sending nullptr probes the cache. If it hits, it updates dimensions and returns S_OK
                    if (SUCCEEDED(g_app.renderer->LoadBitmap(nullptr, 0, 0, currentPath))) {
                        InvalidateRect(hWnd, nullptr, FALSE); // Draw it!
                    }
                }
            }
            return 0;
        }

        case WM_NCCALCSIZE:
            return 0;

        case WM_NCPAINT:
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_CLOSE: {
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
    typedef BOOL (WINAPI
                *SETDPI
            )
            (DPI_AWARENESS_CONTEXT);
    if (HMODULE hU32 = GetModuleHandleW(L"user32.dll")) {
        if (auto setDpi = reinterpret_cast<SETDPI>(GetProcAddress(hU32, "SetProcessDpiAwarenessContext"))) {
            setDpi(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        }
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_app.wicFactory)))) return 0;

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
        HWND hExistingWnd = FindWindowW(Constants::WINDOW_CLASS_NAME, nullptr);
        if (hExistingWnd) {
            int argc;
            LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
            if (argc > 1) {
                COPYDATASTRUCT cds;
                cds.dwData = 1;
                cds.cbData = (DWORD) ((wcslen(argv[1]) + 1) * sizeof(wchar_t));
                cds.lpData = (void *) argv[1];
                SendMessageW(hExistingWnd, WM_COPYDATA, 0, (LPARAM) &cds);
            }
            LocalFree(argv);
        }
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
        return 0;
    }

    WNDCLASSW wc{0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = Constants::WINDOW_CLASS_NAME;
    wc.style = CS_DBLCLKS;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_APP_ICON));
    RegisterClassW(&wc);

    HWND hWnd = CreateViewerWindow(hInstance, wc.lpszClassName);
    if (!hWnd) return 1;

    SetWindowLongW(hWnd, GWL_EXSTYLE, GetWindowLongW(hWnd, GWL_EXSTYLE) | WS_EX_LAYERED);
    SetLayeredWindowAttributes(hWnd, 0, g_app.opacity, LWA_ALPHA);
    UINT dpi = GetDpiForWindow(hWnd);
    g_app.dpiScale = static_cast<float>(dpi) / 96.0f;
    g_app.screenW = GetSystemMetrics(SM_CXSCREEN);
    g_app.screenH = GetSystemMetrics(SM_CYSCREEN);

    // Initialize Renderer (D2D with GDI fallback)
    g_app.renderer = std::make_unique<RendererD2D>();
    if (FAILED(g_app.renderer->Initialize(hWnd))) {
        g_app.renderer = std::make_unique<RendererGDI>();
        (void) g_app.renderer->Initialize(hWnd); // GDI init: S_OK always, failure is non-fatal
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

    RegisterDragDrop(hWnd, (g_pDropTarget = new DropTarget(hWnd)));
    UI::InitHelpWindow(hInstance, hWnd);

    DWORD corner = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(hWnd, Constants::DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));

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

    // --- CRITICAL CLEANUP ---
    g_app.renderer.reset();

    if (g_app.wicFactory) {
        g_app.wicFactory.Reset();
    }

    if (g_pDropTarget) {
        RevokeDragDrop(hWnd);
        g_pDropTarget->Release();
        g_pDropTarget = nullptr;
    }

    UnregisterClassW(Constants::WINDOW_CLASS_NAME, hInstance);

    CoUninitialize();
    OleUninitialize();

    if (hMutex) {
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
    }

    return static_cast<int>(msg.wParam);
}
