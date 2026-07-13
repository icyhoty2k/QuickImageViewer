#include <algorithm>
#include <random>
#include "CMDArgs.h"
#include "SlideshowTransitions.h"

#include "AppCommands.h"
#include "Overlays/OverlayManager.h"

// Defined in FileHandler.cpp — updates all overlay text for the current image
extern void UpdateOverlaysForCurrentImage(HWND hWnd);

#include "UIManager.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <dwmapi.h>
#include <intsafe.h>
#include "CacheWnd.h"
#include "AppState.h"
#include "WorkerThread.h"
#include "Platform/Constants.h"
#include "Platform/ConstantsStrings.h"
#include "../DropTarget.h"
#include "Platform/FileHandler.h"
#include "UI/DirWnd.h"
#include "MouseHandler.h"
#include "Input/Command.h"
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
#include <shlobj.h>   // Required for SHOpenFolderAndSelectItems


// Global application state

DropTarget *g_pDropTarget = nullptr;
AppState app;
// Define the storage for the globals exactly once in your entry point file
//   g_ioWorker      – IoThreadPool: started lazily in FileHandler once the
//                     target drive is known (1 thread HDD, 2 threads SSD/NVMe)
//   g_decoderWorker – WorkerThread(true): WIC decode + pixel convert
IoThreadPool g_ioWorker;

//Main app decoderThreadPool
DecoderThreadPool g_decoderWorker;
// Dedicated worker for DirWnd thumbnail decoding.
// Kept separate so LoadImageIndex's ClearQueue() never wipes dir thumb tasks.
DecoderThreadPool g_dirThumbWorker;


// Shift+Delete (Shortcuts::SC_APP_RESET_DEFAULTS) — restore default application
// state: window size/position centered on the current monitor, zoom/pan/
// rotation/flip/opacity reset, and every image effect cleared.


LRESULT CALLBACK MainAppWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    // KIOSK lock: swallow all user-input messages so the viewer cannot be controlled
    if (app.isLocked) {
        switch (message) {
            case WM_KEYDOWN:
            case WM_KEYUP:
            case WM_LBUTTONDOWN:
            case WM_LBUTTONUP:
            case WM_LBUTTONDBLCLK:
            case WM_RBUTTONDOWN:
            case WM_RBUTTONUP:
            case WM_MBUTTONDOWN:
            case WM_MBUTTONUP:
            case WM_MOUSEWHEEL:
            case WM_MOUSEMOVE:
                return 0;
            default:
                break;
        }
    }

    switch (message) {
        case WM_DPICHANGED: {
            // 1. Update your global scale
            app.dpiScale = static_cast<float>(HIWORD(wParam)) / 96.0f;

            // 2. Refresh the Renderer's font format
            g_overlayManager.UpdateTextFormat();
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

                ShowWindow(hWnd, SW_RESTORE);
                SetForegroundWindow(hWnd);
                InvalidateRect(hWnd, nullptr, FALSE);
            } else if (cds->dwData == 2) {
                ShowWindow(hWnd, SW_RESTORE);
                SetForegroundWindow(hWnd);
            }
            return TRUE;
        }
        case WM_TIMER: {
            constexpr UINT_PTR TIMER_LOOKASIDE = 1001;
            constexpr UINT_PTR TIMER_CENTER_MSG = 1002;

            if (wParam == TIMER_CENTER_MSG) {
                g_overlayManager.OnCenterMessageTimer(hWnd);
                return 0;
            }

            if (wParam == Constants::Slideshow::TIMER_ID) {
                if (app.slideshow.running && !app.slideshow.paused && !app.playlist.empty()) {
                    int size = static_cast<int>(app.playlist.size());
                    if (app.slideshow.shuffle && !app.slideshow.shuffleOrder.empty()) {
                        int next = app.slideshow.shufflePos + 1;
                        if (next >= size) {
                            if (!app.slideshow.loop) {
                                AppCommands::stopSlideshow(hWnd);
                                g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::SLIDESHOW_STOPPED);
                                return 0;
                            }
                            std::shuffle(app.slideshow.shuffleOrder.begin(), app.slideshow.shuffleOrder.end(),
                                         std::mt19937{std::random_device{}()});
                            next = 0;
                        }
                        app.slideshow.shufflePos = next;
                        LoadImageIndex(hWnd, app.slideshow.shuffleOrder[next]);
                        StartTransition(hWnd, app.slideshow.transition);
                    } else {
                        int next = (app.currentIndex + 1) % size;
                        if (next == 0 && !app.slideshow.loop) {
                            AppCommands::stopSlideshow(hWnd);
                            g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::SLIDESHOW_STOPPED);
                            return 0;
                        }
                        LoadImageIndex(hWnd, next);
                        StartTransition(hWnd, app.slideshow.transition);
                    }
                    InvalidateRect(hWnd, nullptr, FALSE);
                }
                return 0;
            }

            if (wParam == Constants::Slideshow::CURSOR_TIMER_ID) {
                KillTimer(hWnd, Constants::Slideshow::CURSOR_TIMER_ID);
                if (app.slideshow.running && !app.slideshow.paused && !app.slideshow.cursorHidden) {
                    ShowCursor(FALSE);
                    app.slideshow.cursorHidden = true;
                }
                return 0;
            }

            if (wParam == Constants::Slideshow::TRANSITION_TIMER_ID) {
                StepTransition(hWnd, app.slideshow.transition);
                if (IsTransitionComplete(app.slideshow.transition)) {
                    KillTimer(hWnd, Constants::Slideshow::TRANSITION_TIMER_ID);
                    FinishTransition(hWnd, app.slideshow.transition);
                }
                return 0;
            }

            if (wParam == TIMER_LOOKASIDE) {
                KillTimer(hWnd, TIMER_LOOKASIDE);

                if (app.playlist.empty()) return 0;

                int index = app.currentIndex;
                const int total = static_cast<int>(app.playlist.size());

                // Relay preloads through g_decoderWorker so they don't flood
                // g_ioWorker directly and starve thumbnail IO tasks that share it.
                auto preloadTask = [index](const std::wstring &path) {
                    return [path, index](IWICImagingFactory2 * /*wic*/) {
                        if (app.wantedIndex.load(std::memory_order_acquire) != index) return;
                        if (app.renderer) (void) app.renderer->PreloadBitmap(path, index);
                    };
                };
                for (int i = 1; i <= Constants::PRELOAD_LOOKASIDE_COUNT; ++i) {
                    int fwd = index + i;
                    int bwd = index - i;
                    if (fwd < total) g_decoderWorker.PushTask(preloadTask(app.playlist[fwd]));
                    if (bwd >= 0) g_decoderWorker.PushTask(preloadTask(app.playlist[bwd]));
                }
            }
            return 0;
        }
        case WM_KEYDOWN:
            InputManager::handleKeyboard(hWnd, wParam);
            return 0;

        case WM_SYSKEYDOWN:
            // Alt+key combinations arrive here (not WM_KEYDOWN).
            // Pass to our handler first, then DefWindowProc so Alt+F4 still works.
            InputManager::handleKeyboard(hWnd, wParam);
            return DefWindowProcW(hWnd, message, wParam, lParam);

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

        // Window size changed: Update renderer.
        // No WM_SIZING handler on purpose: WM_SIZE already fires continuously
        // during interactive resize with the correct CLIENT size. Resizing in
        // WM_SIZING too meant two swap-chain ResizeBuffers per drag tick, one
        // of them at the wrong size (window rect incl. frame).
        case WM_SIZE:
            if (app.renderer) {
                app.renderer->Resize(LOWORD(lParam), HIWORD(lParam));
            }
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;

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
            if (app.slideshow.running) {
                if (app.slideshow.cursorHidden) {
                    ShowCursor(TRUE);
                    app.slideshow.cursorHidden = false;
                }
                if (app.slideshow.cursorHideMs > 0 && !app.slideshow.paused) {
                    KillTimer(hWnd, Constants::Slideshow::CURSOR_TIMER_ID);
                    SetTimer(hWnd, Constants::Slideshow::CURSOR_TIMER_ID, app.slideshow.cursorHideMs, nullptr);
                }
            }
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
                    // Apply EXIF orientation stored in the cache entry during decode.
                    // The viewport was already reset in LoadImageIndex; orientation
                    // could not be applied earlier because the file wasn't decoded yet.
                    ApplyOrientationToViewport(app.renderer->GetCachedOrientation(currentPath));
                    // --- CALL THE EFFECT UPDATER HERE ---
                    // Now the bitmap is ready, we can safely wire the effect graph.
                    app.UpdateRendererColorEffects(hWnd);
                    uiManager.RefreshInfoWindowIfVisible();
                    UpdateOverlaysForCurrentImage(hWnd);

                    InvalidateRect(hWnd, nullptr, FALSE); // Now, repaint with the correct image.
                    uiManager.getCacheWindow().UpdateCacheView();
                    uiManager.getActiveDirWnd().UpdateDirView();
                    // Scroll DirWnd to the newly active image. UpdateDirView()
                    // rebuilds geometry and sets m_selectedIdx but does not move
                    // m_offset (by design, so manual scrolling is not interrupted).
                    // On a cache-miss arrival we always want to snap to selection.
                    uiManager.getActiveDirWnd().SyncDirSelectionRectangle();
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
                    UpdateOverlaysForCurrentImage(hWnd);

                    InvalidateRect(hWnd, nullptr, FALSE);
                    uiManager.getCacheWindow().UpdateCacheView();
                    uiManager.getActiveDirWnd().UpdateDirView();
                    uiManager.getActiveDirWnd().SyncDirSelectionRectangle();
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
        case WM_TRAYICON: {
            if (LOWORD(lParam) == WM_LBUTTONDBLCLK) {
                // 1. Remove the tray icon and make the window visible
                ShowWindow(hWnd, SW_SHOW);
                ShowWindow(hWnd, SW_RESTORE);

                // 2. Bulletproof focus-stealing logic
                HWND hForegroundWnd = GetForegroundWindow();
                if (hForegroundWnd && hForegroundWnd != hWnd) {
                    DWORD foregroundThreadId = GetWindowThreadProcessId(hForegroundWnd, nullptr);
                    DWORD currentThreadId = GetCurrentThreadId();

                    AttachThreadInput(foregroundThreadId, currentThreadId, TRUE);

                    SetForegroundWindow(hWnd);
                    SetWindowPos(hWnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
                    SetActiveWindow(hWnd);
                    SetFocus(hWnd);

                    AttachThreadInput(foregroundThreadId, currentThreadId, FALSE);
                } else {
                    SetForegroundWindow(hWnd);
                }
            } else if (LOWORD(lParam) == WM_RBUTTONUP) {
                // --- OPTIMIZED: Extract coordinates directly from wParam ---
                int x = GET_X_LPARAM(wParam);
                int y = GET_Y_LPARAM(wParam);

                HMENU hMenu = CreatePopupMenu();
                AppendMenuW(hMenu, MF_STRING, 1, L"Restore QuickImageViewer");
                AppendMenuW(hMenu, MF_STRING, 2, L"Help / Shortcuts"); // --- NEW ---
                AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr); // Visual separator
                AppendMenuW(hMenu, MF_STRING, 3, L"Exit Completely");

                // SetForegroundWindow is REQUIRED here
                SetForegroundWindow(hWnd);

                // Pass the extracted x and y directly
                int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, x, y, 0, hWnd, nullptr);
                PostMessage(hWnd, WM_NULL, 0, 0);
                DestroyMenu(hMenu);

                if (cmd == 1) { // Restore clicked
                    ShowWindow(hWnd, SW_SHOW);
                    ShowWindow(hWnd, SW_RESTORE);
                    SetForegroundWindow(hWnd);
                } else if (cmd == 2) { // --- NEW: Help clicked ---
                    // 1. Bring the main app to the front first
                    ShowWindow(hWnd, SW_SHOW);
                    ShowWindow(hWnd, SW_RESTORE);
                    SetForegroundWindow(hWnd);

                    // 2. Trigger your help window.
                    // Assuming uiManager holds the help window instance based on your architecture:
                    uiManager.getHelpWindow().Show();
                } else if (cmd == 3) { // Exit clicked
                    AppCommands::RemoveTrayIcon(hWnd);
                    DestroyWindow(hWnd);
                }
            }
            return 0;
        }


        case WM_DELETE_SPAWNED_PANEL:
            delete reinterpret_cast<UI::SpawnedDirWnd *>(lParam);
            return 0;

        case WM_CLOSE: {
            // 1. "Hide" instead of "Destroy"
            // This removes the window from sight but keeps the process and message loop alive.
            ShowWindow(hWnd, SW_HIDE);
            AppCommands::AddTrayIcon(hWnd);

            return 0; // Returning 0 prevents WM_DESTROY from being called
        }

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hWnd, message, wParam, lParam);
}

//////////////////////////////////////////////////////
//////////////////// Main entry point /////////////////
////////////////////////////////////////////////////////
int WINAPI wWinMain(HINSTANCE hInstance, [[maybe_unused]] HINSTANCE hPrevInstance, [[maybe_unused]] PWSTR pCmdLine, int nCmdShow) {
    // 1. Initialize OLE
    if (FAILED(OleInitialize(nullptr))) return 0;
    // Enable process-wide dark standard controls for the tray menu


    // Set DPI awareness
    typedef BOOL (WINAPI *SETDPI)(DPI_AWARENESS_CONTEXT);
    if (HMODULE hU32 = GetModuleHandleW(L"user32.dll")) {
        if (auto setDpi = reinterpret_cast<SETDPI>(GetProcAddress(hU32, "SetProcessDpiAwarenessContext"))) {
            setDpi(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        }
    }
    const unsigned int hc = std::thread::hardware_concurrency();
    app.hardwareThreads = static_cast<int>(hc > 0 ? hc : 1); // Default to 1 if OS returns 0
    //dynamic thread selection
    g_decoderWorker.setThreadCount(app.hardwareThreads > 3 ? Constants::VRAM_CACHE_DECODER_THREADS_COUNT : 1);
    int dirThumbThreads = (app.hardwareThreads >= 8) ? (app.hardwareThreads / 2) : (Constants::VRAM_CACHE_THUMBS_THREADS_COUNT);
    g_dirThumbWorker.setThreadCount(std::max(1, dirThumbThreads));
#ifdef _DEBUG
    // Use the public getter instead of accessing private member m_threads
    std::wstring debugMsg = L"DecoderThreadPool: Initialized with " +
                            std::to_wstring(g_decoderWorker.getThreadCount()) +
                            L" threads.\n" +
                            L"DecoderThreadPool: Initialized with " +
                            std::to_wstring(g_dirThumbWorker.getThreadCount()) +
                            L" threads.\n";
    OutputDebugStringW(debugMsg.c_str());

#endif

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&app.wicFactory)))) return 0;

    // --- COMMAND LINE PARSING (early — -dedicated must be known before registry calls) ---
    int argc;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"-dedicated") == 0) {
            app.isDedicated = true;
            break;
        }
    }

    if (!app.isDedicated) {
        System::RegisterAppForOpenWith();
        System::EnableRunOnStartup(Constants::IS_ENABLE_RUN_ON_STARTUP);
    }

    // --- SINGLE INSTANCE & RAM RESIDENT LOGIC ---
    bool bypassMutex = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    if (GetEnvironmentVariableW(L"QIV_NEW_INSTANCE", nullptr, 0) > 0) bypassMutex = true;

    std::wstring mutexName = L"QuickImageViewer_SingleInstanceMutex";
    if (app.isDedicated) mutexName += L"_dedicated";
    mutexName += (bypassMutex ? std::to_wstring(GetTickCount()) : L"");
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
    wc.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(app.isDedicated ? IDI_APP_ICON_DEDICATED : IDI_APP_ICON));
    RegisterClassW(&wc);

    HWND hWnd = CreateViewerWindow(hInstance, wc.lpszClassName);
    if (!hWnd) {
        LocalFree(argv);
        return 1;
    }
    AppCommands::changeAppThemeToDarkMode(hWnd, app.isDarkThemed);
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
        uiManager.getActiveDirWnd().SyncDirSelectionRectangle();
    };

    // Set initial overlay visibility from app defaults.
    // OverlayManager::Init() was already called inside renderer->Initialize(),
    // so text resources are ready. OnResize() will be called by WM_SIZE on first paint.
    g_overlayManager.SetAllVisible(app.showOverlayInfoText);

    RegisterDragDrop(hWnd, (g_pDropTarget = new DropTarget(hWnd)));

    AppCommands::changeAppCornerPreference(hWnd, app.cornerPreference);

    // Parse and apply all command-line arguments
    const CmdArgs cmdArgs = ParseCmdArgs(argc, argv);
    LocalFree(argv); // free early — ApplyCmdArgs works from the parsed struct

    ApplyCmdArgs(hWnd, cmdArgs, nCmdShow);

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
