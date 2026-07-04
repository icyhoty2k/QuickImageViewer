#include <algorithm>

#include "AppCommands.h"
#include "UIManager.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <dwmapi.h>
#include <intsafe.h>
#include <uxtheme.h>
#include "CacheWnd.h"
#include "../AppState.h"
#include "Platform/Constants.h"

#include "../DropTarget.h"
#include "Platform/FileHandler.h"
#include "UI/HelpWnd.h"
#include "UI/DirWnd.h"
#include "UI/HistoryListWnd.h"

#include "MouseHandler.h"
#include "Input/Command.h"
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
#include <shlobj.h>   // Required for SHOpenFolderAndSelectItems
#include <commdlg.h>  // GetSaveFileName dialog

// Global application state
AppState app;
DropTarget *g_pDropTarget = nullptr;

// Define the storage for the globals exactly once in your entry point file
//   g_ioWorker      – IoThreadPool: started lazily in FileHandler once the
//                     target drive is known (1 thread HDD, 2 threads SSD/NVMe)
//   g_decoderWorker – WorkerThread(true): WIC decode + pixel convert
IoThreadPool g_ioWorker;
WorkerThread g_decoderWorker(true);
// Dedicated worker for DirWnd thumbnail decoding.
// Kept separate so LoadImageIndex's ClearQueue() never wipes dir thumb tasks.
WorkerThread g_dirThumbWorker(true);


// Shift+Delete (Shortcuts::SC_APP_RESET_DEFAULTS) — restore default application
// state: window size/position centered on the current monitor, zoom/pan/
// rotation/flip/opacity reset, and every image effect cleared.


LRESULT CALLBACK MainAppWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_DPICHANGED: {
            // 1. Update your global scale
            app.dpiScale = static_cast<float>(HIWORD(wParam)) / 96.0f;

            // 2. Refresh the Renderer's font format
            app.renderer->UpdateTextFormat();
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
                std::wstring safePath((LPCWSTR) cds->lpData);
                OpenSpecificImage(hWnd, safePath.c_str());

                AppCommands::RemoveTrayIcon(hWnd); // Clean up tray
                ShowWindow(hWnd, SW_RESTORE);
                SetForegroundWindow(hWnd);
                InvalidateRect(hWnd, nullptr, FALSE);
            } else if (cds->dwData == 2) {
                AppCommands::RemoveTrayIcon(hWnd); // Clean up tray
                ShowWindow(hWnd, SW_RESTORE);
                SetForegroundWindow(hWnd);
            }
            return TRUE;
        }
        case WM_TIMER: {
            constexpr UINT_PTR TIMER_LOOKASIDE = 1001;

            if (wParam == TIMER_LOOKASIDE) {
                KillTimer(hWnd, TIMER_LOOKASIDE);

                if (app.playlist.empty()) return 0;

                int index = app.currentIndex;
                const int total = static_cast<int>(app.playlist.size());

                for (int i = 1; i <= Constants::PRELOAD_LOOKASIDE_COUNT; ++i) {
                    int fwd = index + i;
                    int bwd = index - i;

                    if (fwd < total) {
                        std::wstring fwdPath = app.playlist[fwd];
                        g_decoderWorker.PushTask([fwdPath, index]() {
                            // ABORT if user started scrolling again
                            if (app.wantedIndex.load(std::memory_order_acquire) != index) return;
                            if (app.renderer) (void) app.renderer->PreloadBitmap(fwdPath, index);
                        });
                    }
                    if (bwd >= 0) {
                        std::wstring bwdPath = app.playlist[bwd];
                        g_decoderWorker.PushTask([bwdPath, index]() {
                            // ABORT if user started scrolling again
                            if (app.wantedIndex.load(std::memory_order_acquire) != index) return;
                            if (app.renderer) (void) app.renderer->PreloadBitmap(bwdPath, index);
                        });
                    }
                }
            }
            return 0;
        }
        case WM_KEYDOWN:
            InputManager::handleKeyboard(hWnd, wParam);
            return 0;

        case WM_NCACTIVATE:
            return TRUE;

        case WM_NCHITTEST: {
            if (app.isFullscreen) return HTCLIENT;

            POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            RECT rc;
            GetWindowRect(hWnd, &rc);

            // Scale the border by the DPI factor
            // Using int to maintain pixel alignment
            const int border = static_cast<int>(2 * app.dpiScale);

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
            if (app.renderer) {
                app.renderer->Resize(LOWORD(lParam), HIWORD(lParam));
            }
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;
        case WM_SIZING:
            if (app.renderer) {
                RECT *r = (RECT *) lParam;
                app.renderer->Resize(r->right - r->left, r->bottom - r->top);
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
                    app.opacity = static_cast<BYTE>(
                        (app.opacity + Constants::OPACITY_STEP > 255)
                            ? 255
                            : (app.opacity + Constants::OPACITY_STEP));
                } else {
                    app.opacity = static_cast<BYTE>(
                        (app.opacity - Constants::OPACITY_STEP < 10)
                            ? 10
                            : (app.opacity - Constants::OPACITY_STEP));
                }

                SetLayeredWindowAttributes(hWnd, 0, app.opacity, LWA_ALPHA);
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
                app.opacity = static_cast<BYTE>(std::min(255, app.opacity + Constants::OPACITY_STEP));
            } else {
                app.opacity = static_cast<BYTE>(std::max(10, app.opacity - Constants::OPACITY_STEP));
            }
            SetLayeredWindowAttributes(hWnd, 0, app.opacity, LWA_ALPHA);
            return 0;
        }

        case WM_LBUTTONDBLCLK: {
            SendMessageW(hWnd, WM_SETREDRAW, FALSE, 0);

            AppCommands::ToggleFullscreen(hWnd);

            SendMessageW(hWnd, WM_SETREDRAW, TRUE, 0);
            RedrawWindow(hWnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_FRAME);
            return 0;
        }

        case WM_CAPTURECHANGED: {
            app.viewport.isDragging = false;
            app.isWindowDragging = false;
            ReleaseCapture();
            return 0;
        }
        case Constants::WM_QIV_REPAINT: {
            // The background thread has finished decoding and caching the bitmap.
            // Now, on the UI thread, we probe the cache to make it the active bitmap.
            if (app.renderer && !app.playlist.empty()) {
                const std::wstring &currentPath = app.playlist[app.currentIndex];
                // This call will find the bitmap in the cache and set it as active.
                if (SUCCEEDED(app.renderer->LoadBitmap(nullptr, 0, 0, currentPath))) {
                    // --- CALL THE EFFECT UPDATER HERE ---
                    // Now the bitmap is ready, we can safely wire the effect graph.
                    app.UpdateRendererColorEffects(hWnd);
                    InvalidateRect(hWnd, nullptr, FALSE); // Now, repaint with the correct image.
                    uiManager.getCacheWindow().UpdateCacheView();
                    uiManager.getDirWindow().UpdateDirView();
                }
            }
            return 0;
        }

        case Constants::WM_QIV_SVG_READY: {
            // wParam = playlist index at the time of the IO request
            // lParam = heap-allocated SvgPayload* (we own it, must delete)
            struct SvgPayload {
                std::wstring path;
                std::vector<BYTE> bytes;
            };
            auto *payload = reinterpret_cast<SvgPayload *>(lParam);
            if (!payload) return 0;

            int arrivedIndex = static_cast<int>(wParam);

            // Discard if user navigated away while bytes were in flight
            if (arrivedIndex == app.wantedIndex.load(std::memory_order_acquire) &&
                app.renderer) {
                if (SUCCEEDED(app.renderer->LoadSvgFromBytes(payload->bytes, payload->path))) {
                    InvalidateRect(hWnd, nullptr, FALSE);
                    uiManager.getCacheWindow().UpdateCacheView();
                    uiManager.getDirWindow().UpdateDirView();
                }
            }

            delete payload;
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hWnd, &ps);
            if (app.renderer) {
                (void) app.renderer->Render();
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
            //Tray icon on close
            AppCommands::AddTrayIcon(hWnd);
            // 1. "Hide" instead of "Destroy"
            // This removes the window from sight but keeps the process and message loop alive.
            ShowWindow(hWnd, SW_HIDE);
            return 0; // Returning 0 prevents WM_DESTROY from being called
        }

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hWnd, message, wParam, lParam);
}

//Main entry point
int WINAPI wWinMain(HINSTANCE hInstance, [[maybe_unused]] HINSTANCE hPrevInstance, [[maybe_unused]] PWSTR pCmdLine, int nCmdShow) {
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
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&app.wicFactory)))) return 0;

    System::RegisterAppForOpenWith();
    System::EnableRunOnStartup();

    // --- CONSOLIDATED COMMAND LINE PARSING ---
    int argc;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    // --- SINGLE INSTANCE & RAM RESIDENT LOGIC ---
    bool bypassMutex = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    if (GetEnvironmentVariableW(L"QIV_NEW_INSTANCE", nullptr, 0) > 0) bypassMutex = true;

    std::wstring mutexName = L"QuickImageViewer_SingleInstanceMutex" + (bypassMutex ? std::to_wstring(GetTickCount()) : L"");
    HANDLE hMutex = CreateMutexW(NULL, TRUE, mutexName.c_str());

    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND hExistingWnd = FindWindowW(Constants::WINDOW_CLASS_NAME, nullptr);
        if (hExistingWnd) {
            // Allow the background instance to steal focus from this closing instance
            DWORD existingProcId;
            GetWindowThreadProcessId(hExistingWnd, &existingProcId);
            AllowSetForegroundWindow(existingProcId);

            COPYDATASTRUCT cds;
            if (argc > 1) {
                // Signal 1: Load new image and wake up
                cds.dwData = 1;
                cds.cbData = (DWORD) ((wcslen(argv[1]) + 1) * sizeof(wchar_t));
                cds.lpData = (void *) argv[1];
            } else {
                // Signal 2: Wake up only (no file passed)
                cds.dwData = 2;
                cds.cbData = 0;
                cds.lpData = nullptr;
            }
            SendMessageW(hExistingWnd, WM_COPYDATA, 0, (LPARAM) &cds);
        }
        LocalFree(argv);
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
        return 0;
    }

    // --- Window Creation ---
    WNDCLASSW wc{0};
    wc.lpfnWndProc = MainAppWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = Constants::WINDOW_CLASS_NAME;
    wc.style = CS_DBLCLKS;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_APP_ICON));
    RegisterClassW(&wc);

    HWND hWnd = CreateViewerWindow(hInstance, wc.lpszClassName);
    if (!hWnd) {
        LocalFree(argv);
        return 1;
    }

    // Init UI Manager (The New Controller)
    uiManager.Init(hInstance, hWnd);

    // Renderer & Setup
    SetWindowLongW(hWnd, GWL_EXSTYLE, GetWindowLongW(hWnd, GWL_EXSTYLE) | WS_EX_LAYERED);
    SetLayeredWindowAttributes(hWnd, 0, app.opacity, LWA_ALPHA);
    app.dpiScale = static_cast<float>(GetDpiForWindow(hWnd)) / 96.0f;
    app.screenW = GetSystemMetrics(SM_CXSCREEN);
    app.screenH = GetSystemMetrics(SM_CYSCREEN);

    app.renderer = std::make_unique<RendererD2D>();
    if (FAILED(app.renderer->Initialize(hWnd))) {
        app.renderer = std::make_unique<RendererGDI>();
        (void) app.renderer->Initialize(hWnd);
    }
    app.renderer->onImageChangedCallback = [](int) {
        uiManager.getCacheWindow().SyncSelectionRectangle();
        uiManager.getDirWindow().SyncDirSelectionRectangle();
    };

    RegisterDragDrop(hWnd, (g_pDropTarget = new DropTarget(hWnd)));

    DwmSetWindowAttribute(hWnd, Constants::DWMWA_WINDOW_CORNER_PREFERENCES, &Constants::APP_CORNER_PREFERENCES, sizeof(Constants::APP_CORNER_PREFERENCES));
    // Handle startup arguments using the already-parsed 'argv'
    if (argc > 1 && std::wstring(argv[1]) == L"-background") {
        ShowWindow(hWnd, SW_HIDE);
    } else {
        ShowWindow(hWnd, nCmdShow);
        UpdateWindow(hWnd);
        if (argc > 1) OpenSpecificImage(hWnd, argv[1]);
        else OpenInitialImage(hWnd);
    }

    // Cleanup command line memory ONCE
    LocalFree(argv);

    MSG msg{};
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // --- CRITICAL CLEANUP ---
    app.renderer.reset();

    if (app.wicFactory) {
        app.wicFactory.Reset();
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
