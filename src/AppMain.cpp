#include <algorithm>
#include <random>
#include <fstream>
using std::min;
using std::max;
#include <cstdio>
#include <intrin.h>
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
#include "UI/ThumbnailPanels/CacheWnd.h"
#include "AppState.h"
#include "WorkerThread.h"
#include "Platform/Constants.h"
#include "Platform/ConstantsStrings.h"
#include "../DropTarget.h"
#include "Platform/FileHandler.h"
#include "GeoNames.h"
#include "UI/ThumbnailPanels/DirWnd.h"
#include "UI/ThemedDialog.h"
#include "Persistence/HistoryFoldersManager.h"
#include <miniz.h>
#include "MouseHandler.h"
#include "Input/Command.h"
#include <windows.h>
#ifndef MF_RADIOCHECK
#define MF_RADIOCHECK 0x00000200L
#endif
#include <windowsx.h>
#include <commdlg.h>
#include <shobjidl.h>
#include <commctrl.h>
#include <shellapi.h> // Parsing command line arguments
#include <string>     // Handling string paths
#include <memory>     // Needed for std::unique_ptr for renderer management
#include "Platform/DpiAwareInit.h"
#include "../resources/resource.h"
#include "Persistence/RegistryManager.h"
#include "Renderer/RendererD2D.h"
#include "Renderer/RendererGDI.h"


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
            // 3. Reposition vertical panels — their physical thickness changed.
            uiManager.RefreshVerticalPanels();
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;
        }

        // Handle file paths sent from other instances of the viewer
        case WM_COPYDATA: {
            COPYDATASTRUCT *cds = (COPYDATASTRUCT *) lParam;
            if (cds->dwData == 1) {
                // Post asynchronously — returning quickly unblocks the sending process.
                // WM_QIV_OPEN_FILE handler owns the pointer and deletes it.
                PostMessageW(hWnd, Constants::WM_QIV_OPEN_FILE, 0,
                             reinterpret_cast<LPARAM>(new std::wstring((LPCWSTR) cds->lpData)));
                ShowWindow(hWnd, SW_RESTORE);
                SetForegroundWindow(hWnd);
            } else if (cds->dwData == 2) {
                ShowWindow(hWnd, SW_RESTORE);
                SetForegroundWindow(hWnd);
            }
            return TRUE;
        }

        case Constants::WM_QIV_OPEN_FILE: {
            auto *path = reinterpret_cast<std::wstring *>(lParam);
            OpenSpecificImage(hWnd, path->c_str());
            delete path;
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;
        }
        case Constants::WM_QIV_SWITCH_TO_FIND:
            uiManager.ToggleFindWindow();
            return 0;
        case Constants::WM_QIV_SWITCH_TO_JUMP:
            uiManager.ToggleJumpToWindow();
            return 0;
        case Constants::WM_QIV_SCAN_COMPLETE:
            HandleScanComplete(hWnd, reinterpret_cast<ScanResult *>(lParam));
            return 0;

        case Constants::WM_QIV_DIR_CHANGED:
            // File-system change in the watched folder: (re)start the debounce
            // timer. Multiple rapid events collapse into a single reload.
            KillTimer(hWnd, Constants::DIR_WATCHER_TIMER_ID);
            SetTimer(hWnd, Constants::DIR_WATCHER_TIMER_ID,
                     Constants::DIR_WATCHER_DEBOUNCE_MS, nullptr);
            return 0;
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

            if (wParam == Constants::Slideshow::GIF_TIMER_ID) {
                KillTimer(hWnd, Constants::Slideshow::GIF_TIMER_ID);
                if (app.renderer && app.renderer->IsAnimatedGif()) {
                    int nextDelay = app.renderer->AdvanceGifFrame();
                    InvalidateRect(hWnd, nullptr, FALSE);
                    SetTimer(hWnd, Constants::Slideshow::GIF_TIMER_ID, nextDelay, nullptr);
                }
                return 0;
            }

            if (wParam == Constants::DIR_WATCHER_TIMER_ID) {
                KillTimer(hWnd, Constants::DIR_WATCHER_TIMER_ID);
                ReloadCurrentDirectory(hWnd);
                return 0;
            }

            if (wParam == TIMER_LOOKASIDE) {
                KillTimer(hWnd, TIMER_LOOKASIDE);

                if (app.playlist.empty()) return 0;

                int index = app.currentIndex;
                const int total = static_cast<int>(app.playlist.size());

                // Relay preloads through g_decoderWorker so they don't flood
                // g_ioWorker directly and starve thumbnail IO tasks that share it.
                auto preloadTask = [index](const std::wstring &path, int targetIdx) {
                    return [path, index, targetIdx](IWICImagingFactory2 * /*wic*/) {
                        if (app.wantedIndex.load(std::memory_order_acquire) != index) return;
                        if (app.renderer) (void) app.renderer->PreloadBitmap(path, targetIdx, index);
                    };
                };
                for (int i = 1; i <= app.preloadLookaside; ++i) {
                    int fwd = index + i;
                    int bwd = index - i;
                    if (fwd < total) g_decoderWorker.PushTask(preloadTask(app.playlist[fwd], fwd));
                    if (bwd >= 0) g_decoderWorker.PushTask(preloadTask(app.playlist[bwd], bwd));
                }
            }
            return 0;
        }
        case WM_KEYDOWN:
            InputManager::handleKeyboard(hWnd, wParam, lParam);
            return 0;

        case WM_SYSKEYDOWN:
            // Alt+key combinations arrive here (not WM_KEYDOWN).
            // Pass to our handler first, then DefWindowProc so Alt+F4 still works.
            InputManager::handleKeyboard(hWnd, wParam, lParam);
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

        // --- MOUSE HANDLERS ---
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_XBUTTONDOWN:
            MouseHandler::HandleButtonDown(hWnd, message, wParam, lParam);
            return 0;

        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
        case WM_XBUTTONUP:
            MouseHandler::HandleButtonUp(hWnd, message, wParam, lParam);
            return 0;

        case WM_MBUTTONDOWN:
            MouseHandler::HandleButtonDown(hWnd, message, wParam, lParam);
            return 0;

        case WM_MBUTTONUP:
            MouseHandler::HandleButtonDown(hWnd, message, wParam, lParam);
            return 0;

        case WM_MOUSEMOVE:
            MouseHandler::HandleMouseMove(hWnd, lParam);
            return 0;

        case WM_MOUSEWHEEL:
            MouseHandler::HandleMouseWheel(hWnd, wParam, lParam);
            return 0;

        case WM_MOUSEHWHEEL:
            MouseHandler::HandleMouseHWheel(hWnd, wParam, lParam);
            return 0;

        case WM_LBUTTONDBLCLK:
            MouseHandler::HandleDoubleClick(hWnd);
            return 0;

        case WM_CAPTURECHANGED: {
            app.viewport.isDragging = false;
            app.isWindowDragging = false;
            ReleaseCapture();
            return 0;
        }
        case Constants::WM_QIV_REPAINT: {
            // wParam=1: a neighbor preload landed — only refresh the cache panel.
            if (wParam == 1) {
                uiManager.getCacheWindow().UpdateCacheView();
                return 0;
            }
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
                    uiManager.RefreshStatsWindowIfVisible();
                    UpdateOverlaysForCurrentImage(hWnd);

                    InvalidateRect(hWnd, nullptr, FALSE); // Now, repaint with the correct image.
                    uiManager.getCacheWindow().UpdateCacheView();
                    uiManager.getActiveDirWnd().UpdateDirView();
                    // Scroll DirWnd to the newly active image. UpdateDirView()
                    // rebuilds geometry and sets m_selectedIdx but does not move
                    // m_offset (by design, so manual scrolling is not interrupted).
                    // On a cache-miss arrival we always want to snap to selection.
                    uiManager.getActiveDirWnd().SyncDirSelectionRectangle();

                    // Arm the GIF animation timer if this is an animated GIF.
                    KillTimer(hWnd, Constants::Slideshow::GIF_TIMER_ID);
                    if (app.renderer->IsAnimatedGif())
                        SetTimer(hWnd, Constants::Slideshow::GIF_TIMER_ID,
                                 app.renderer->GetCurrentGifDelay(), nullptr);
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
                    uiManager.RefreshInfoWindowIfVisible();
                    uiManager.RefreshStatsWindowIfVisible();
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
                int x = GET_X_LPARAM(wParam);
                int y = GET_Y_LPARAM(wParam);

                // ── Settings submenu: toggles first, separator, then input values ──────
                HMENU hSubMenu = CreatePopupMenu();
                // Toggles
                AppendMenuW(hSubMenu,
                    MF_STRING | (app.isKeepInBackground ? MF_CHECKED : MF_UNCHECKED),
                    4, L"Keep in Background");
                AppendMenuW(hSubMenu,
                    MF_STRING | (app.isEnableRunOnStartup ? MF_CHECKED : MF_UNCHECKED),
                    5, L"Run on Startup");
                AppendMenuW(hSubMenu,
                    MF_STRING | (app.thumbnailEffectsEnabled ? MF_CHECKED : MF_UNCHECKED),
                    6, L"Thumbnail Effects");
                AppendMenuW(hSubMenu,
                    MF_STRING | (app.historyFullModeEnabled ? MF_CHECKED : MF_UNCHECKED),
                    7, L"History: Open Full List");
                AppendMenuW(hSubMenu,
                    MF_STRING | (app.showOverlayInfoText ? MF_CHECKED : MF_UNCHECKED),
                    8, L"Info Overlays");
                AppendMenuW(hSubMenu,
                    MF_STRING | (app.openDirWndOnStart ? MF_CHECKED : MF_UNCHECKED),
                    9, L"Open Thumbnail Strip on Start");
                AppendMenuW(hSubMenu,
                    MF_STRING | (app.overlayShowBackground ? MF_CHECKED : MF_UNCHECKED),
                    13, L"Overlay Background");
                AppendMenuW(hSubMenu,
                    MF_STRING | (app.swapMouseButtons ? MF_CHECKED : MF_UNCHECKED),
                    14, L"Swap Mouse Buttons");
                AppendMenuW(hSubMenu,
                    MF_STRING | (app.invertWheelDirection ? MF_CHECKED : MF_UNCHECKED),
                    15, L"Invert Scroll Direction");
                AppendMenuW(hSubMenu,
                    MF_STRING | (app.invertWheelDirectionH ? MF_CHECKED : MF_UNCHECKED),
                    16, L"Invert Horizontal Scroll");
                AppendMenuW(hSubMenu,
                    MF_STRING | (app.startInFullscreen ? MF_CHECKED : MF_UNCHECKED),
                    25, L"Start in Fullscreen");
                // Separator between toggles and input values
                AppendMenuW(hSubMenu, MF_SEPARATOR, 0, nullptr);
                // Input values
                {
                    wchar_t vramLabel[64];
                    swprintf_s(vramLabel, L"VRAM Cache Size: %d", app.vramCacheCount);
                    AppendMenuW(hSubMenu, MF_STRING, 17, vramLabel);
                }
                {
                    wchar_t wLabel[64], hLabel[64];
                    swprintf_s(wLabel, L"Window Width: %d",  app.baseWidth);
                    swprintf_s(hLabel, L"Window Height: %d", app.baseHeight);
                    AppendMenuW(hSubMenu, MF_STRING, 23, wLabel);
                    AppendMenuW(hSubMenu, MF_STRING, 24, hLabel);
                }
                {
                    wchar_t dLabel[64], fLabel[64];
                    swprintf_s(dLabel, L"History Max Dirs: %d",  app.historyMaxDirs);
                    swprintf_s(fLabel, L"History Max Favs: %d",  app.historyMaxFavs);
                    AppendMenuW(hSubMenu, MF_STRING, 26, dLabel);
                    AppendMenuW(hSubMenu, MF_STRING, 27, fLabel);
                }
                {
                    wchar_t cLabel[64], pLabel[64];
                    swprintf_s(cLabel, L"Dir Thumb Cache: %d MB", app.dirThumbCacheMB);
                    swprintf_s(pLabel, L"Preload Lookaside: %d",  app.preloadLookaside);
                    AppendMenuW(hSubMenu, MF_STRING, 28, cLabel);
                    AppendMenuW(hSubMenu, MF_STRING, 29, pLabel);
                }
                {
                    wchar_t msLabel[64], dsLabel[64];
                    swprintf_s(msLabel, L"Overlay Message Duration: %d ms", app.msgCenterDisplayMs);
                    swprintf_s(dsLabel, L"History Save Limit: %d",           app.historyMaxDirsSave);
                    AppendMenuW(hSubMenu, MF_STRING, 30, msLabel);
                    AppendMenuW(hSubMenu, MF_STRING, 31, dsLabel);
                }
                AppendMenuW(hSubMenu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(hSubMenu, MF_STRING, 10, L"Export Settings");
                AppendMenuW(hSubMenu, MF_STRING, 11, L"Import Settings");
                AppendMenuW(hSubMenu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(hSubMenu, MF_STRING, 12, L"Restore Defaults");

                // ── View Mode submenu (lives in main tray) ───────────────────────────
                HMENU hViewMenu = CreatePopupMenu();
                {
                    const int vm = static_cast<int>(app.viewMode);
                    auto vmFlag = [&](int n) -> UINT {
                        return MF_STRING | MF_RADIOCHECK | (vm == n ? MF_CHECKED : MF_UNCHECKED);
                    };
                    AppendMenuW(hViewMenu, vmFlag(1), 18, L"1 — Fit to View (preserve aspect)");
                    AppendMenuW(hViewMenu, vmFlag(2), 19, L"2 — Fit to Width");
                    AppendMenuW(hViewMenu, vmFlag(3), 20, L"3 — Fit to Height");
                    AppendMenuW(hViewMenu, vmFlag(4), 21, L"4 — Stretch to Window");
                    AppendMenuW(hViewMenu, vmFlag(5), 22, L"5 — Original Size");
                }

                // ── Slideshow submenu (lives in main tray) ───────────────────────────
                HMENU hSlideshowMenu = CreatePopupMenu();
                {
                    wchar_t ivLabel[64];
                    swprintf_s(ivLabel, L"Interval: %d ms", app.slideshow.intervalMs);
                    AppendMenuW(hSlideshowMenu, MF_STRING, 32, ivLabel);
                    AppendMenuW(hSlideshowMenu,
                        MF_STRING | (app.slideshow.loop    ? MF_CHECKED : MF_UNCHECKED), 33, L"Loop");
                    AppendMenuW(hSlideshowMenu,
                        MF_STRING | (app.slideshow.shuffle ? MF_CHECKED : MF_UNCHECKED), 34, L"Shuffle");
                    HMENU hTransMenu = CreatePopupMenu();
                    {
                        const int tt = static_cast<int>(app.slideshow.transition.type);
                        auto ttFlag = [&](int n) -> UINT {
                            return MF_STRING | MF_RADIOCHECK | (tt == n ? MF_CHECKED : MF_UNCHECKED);
                        };
                        AppendMenuW(hTransMenu, ttFlag(0), 35, L"Cut (instant)");
                        AppendMenuW(hTransMenu, ttFlag(1), 36, L"Fade");
                        AppendMenuW(hTransMenu, ttFlag(2), 37, L"Dissolve");
                        AppendMenuW(hTransMenu, ttFlag(3), 38, L"Ripple");
                        AppendMenuW(hTransMenu, ttFlag(4), 39, L"Push");
                        AppendMenuW(hTransMenu, ttFlag(5), 40, L"Zoom");
                    }
                    AppendMenuW(hSlideshowMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hTransMenu), L"Transition");
                }

                // ── Main tray menu ───────────────────────────────────────────────────
                HMENU hMenu = CreatePopupMenu();
                AppendMenuW(hMenu, MF_STRING, 1, L"Restore QuickImageViewer");
                AppendMenuW(hMenu, MF_STRING, 2, L"Help / Shortcuts");
                AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hSubMenu),      L"Settings");
                AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hViewMenu),     L"View Mode");
                AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hSlideshowMenu),L"Slideshow");
                {
                    HMENU hSortMenu  = CreatePopupMenu();
                    HMENU hSortOrder = CreatePopupMenu();
                    const int so = app.fileHandlerDefaultSortOrder;
                    auto soFlag = [&](int n) -> UINT {
                        return MF_STRING | MF_RADIOCHECK | (so == n ? MF_CHECKED : MF_UNCHECKED);
                    };
                    AppendMenuW(hSortOrder, soFlag(0), 43, L"Name");
                    AppendMenuW(hSortOrder, soFlag(1), 44, L"Date Modified");
                    AppendMenuW(hSortOrder, soFlag(2), 45, L"Size");
                    AppendMenuW(hSortOrder, soFlag(3), 46, L"Type");
                    AppendMenuW(hSortOrder, soFlag(4), 47, L"Disk Order");
                    AppendMenuW(hSortMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hSortOrder), L"Sort Order");
                    AppendMenuW(hSortMenu, MF_SEPARATOR, 0, nullptr);
                    AppendMenuW(hSortMenu,
                        MF_STRING | (app.fileHandlerIsReverseSortOrder ? MF_CHECKED : MF_UNCHECKED),
                        48, L"Reverse Order");
                    AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hSortMenu), L"Sort");
                }
                {
                    HMENU hBackupMenu = CreatePopupMenu();
                    AppendMenuW(hBackupMenu, MF_STRING, 41, L"Backup History && Favorites");
                    AppendMenuW(hBackupMenu, MF_STRING, 42, L"Restore History && Favorites");
                    AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hBackupMenu), L"Backup");
                }
                AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(hMenu, MF_STRING, 3, L"Exit Completely");

                SetForegroundWindow(hWnd);
                int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, x, y, 0, hWnd, nullptr);
                PostMessage(hWnd, WM_NULL, 0, 0);
                DestroyMenu(hMenu); // also destroys hSubMenu

                if (cmd == 1) {
                    ShowWindow(hWnd, SW_SHOW);
                    ShowWindow(hWnd, SW_RESTORE);
                    SetForegroundWindow(hWnd);
                } else if (cmd == 2) {
                    ShowWindow(hWnd, SW_SHOW);
                    ShowWindow(hWnd, SW_RESTORE);
                    SetForegroundWindow(hWnd);
                    uiManager.getHelpWindow().Show();
                } else if (cmd == 3) {
                    AppCommands::RemoveTrayIcon(hWnd);
                    DestroyWindow(hWnd);
                } else if (cmd == 4) {
                    app.isKeepInBackground = !app.isKeepInBackground;
                    Persistence::Registry::SaveSetting(Constants::Registry::KEEP_IN_BACKGROUND,
                        static_cast<DWORD>(app.isKeepInBackground));
                } else if (cmd == 5) {
                    app.isEnableRunOnStartup = !app.isEnableRunOnStartup;
                    Persistence::Registry::SaveSetting(Constants::Registry::RUN_ON_STARTUP,
                        static_cast<DWORD>(app.isEnableRunOnStartup));
                    Persistence::Registry::EnableRunOnStartup(app.isEnableRunOnStartup);
                } else if (cmd == 6) {
                    app.thumbnailEffectsEnabled = !app.thumbnailEffectsEnabled;
                    Persistence::Registry::SaveSetting(Constants::Registry::THUMBNAIL_EFFECTS,
                        static_cast<DWORD>(app.thumbnailEffectsEnabled));
                    uiManager.RepaintAllPanels();
                } else if (cmd == 7) {
                    app.historyFullModeEnabled = !app.historyFullModeEnabled;
                    Persistence::Registry::SaveSetting(Constants::Registry::HISTORY_FULL_MODE,
                        static_cast<DWORD>(app.historyFullModeEnabled));
                } else if (cmd == 8) {
                    app.showOverlayInfoText = !app.showOverlayInfoText;
                    Persistence::Registry::SaveSetting(Constants::Registry::OVERLAY_VISIBLE,
                        static_cast<DWORD>(app.showOverlayInfoText));
                    g_overlayManager.SetAllVisible(app.showOverlayInfoText);
                    InvalidateRect(hWnd, nullptr, FALSE);
                } else if (cmd == 9) {
                    app.openDirWndOnStart = !app.openDirWndOnStart;
                    Persistence::Registry::SaveSetting(Constants::Registry::OPEN_DIRWND_ON_START,
                        static_cast<DWORD>(app.openDirWndOnStart));
                } else if (cmd == 13) {
                    app.overlayShowBackground = !app.overlayShowBackground;
                    Persistence::Registry::SaveSetting(Constants::Registry::OVERLAY_SHOW_BG,
                        static_cast<DWORD>(app.overlayShowBackground));
                    InvalidateRect(hWnd, nullptr, FALSE);
                } else if (cmd == 14) {
                    app.swapMouseButtons = !app.swapMouseButtons;
                    Persistence::Registry::SaveSetting(Constants::Registry::SWAP_MOUSE_BUTTONS,
                        static_cast<DWORD>(app.swapMouseButtons));
                } else if (cmd == 15) {
                    app.invertWheelDirection = !app.invertWheelDirection;
                    Persistence::Registry::SaveSetting(Constants::Registry::WHEEL_INVERT,
                        static_cast<DWORD>(app.invertWheelDirection));
                } else if (cmd == 16) {
                    app.invertWheelDirectionH = !app.invertWheelDirectionH;
                    Persistence::Registry::SaveSetting(Constants::Registry::WHEEL_INVERT_H,
                        static_cast<DWORD>(app.invertWheelDirectionH));
                } else if (cmd >= 18 && cmd <= 22) {
                    int modeNum = cmd - 17; // 18→1, 19→2, 20→3, 21→4, 22→5
                    app.viewMode = static_cast<Constants::ViewModes::ViewMode>(modeNum);
                    Persistence::Registry::SaveSetting(Constants::Registry::VIEW_MODE,
                        static_cast<DWORD>(modeNum));
                    InvalidateRect(hWnd, nullptr, FALSE);
                } else if (cmd == 23) {
                    int v = UI::ThemedDialog::PromptInt(hWnd, L"Window Width",
                        L"Default window width in pixels (240 – 16000):",
                        app.baseWidth, 240, 16000, Constants::IS_BASE_WIDTH);
                    if (v >= 0) {
                        app.baseWidth = v;
                        Persistence::Registry::SaveSetting(Constants::Registry::BASE_WIDTH_KEY,
                            static_cast<DWORD>(app.baseWidth));
                    }
                } else if (cmd == 24) {
                    int v = UI::ThemedDialog::PromptInt(hWnd, L"Window Height",
                        L"Default window height in pixels (240 – 16000):",
                        app.baseHeight, 240, 16000, Constants::IS_BASE_HEIGHT);
                    if (v >= 0) {
                        app.baseHeight = v;
                        Persistence::Registry::SaveSetting(Constants::Registry::BASE_HEIGHT_KEY,
                            static_cast<DWORD>(app.baseHeight));
                    }
                } else if (cmd == 25) {
                    app.startInFullscreen = !app.startInFullscreen;
                    Persistence::Registry::SaveSetting(Constants::Registry::START_FULLSCREEN,
                        static_cast<DWORD>(app.startInFullscreen));
                } else if (cmd == 26) {
                    int v = UI::ThemedDialog::PromptInt(hWnd, L"History Max Dirs",
                        L"Maximum history folders to show (0 – 999):",
                        app.historyMaxDirs, 0, 999, Constants::History::IS_HISTORY_MAX_DIRS_TO_SHOW);
                    if (v >= 0) {
                        app.historyMaxDirs = v;
                        Persistence::Registry::SaveSetting(Constants::Registry::HISTORY_MAX_DIRS,
                            static_cast<DWORD>(app.historyMaxDirs));
                    }
                } else if (cmd == 27) {
                    int v = UI::ThemedDialog::PromptInt(hWnd, L"History Max Favs",
                        L"Maximum favorite folders to show (0 – 999):",
                        app.historyMaxFavs, 0, 999, Constants::History::IS_HISTORY_MAX_FAVORITES_TO_SHOW);
                    if (v >= 0) {
                        app.historyMaxFavs = v;
                        Persistence::Registry::SaveSetting(Constants::Registry::HISTORY_MAX_FAVS,
                            static_cast<DWORD>(app.historyMaxFavs));
                    }
                } else if (cmd == 28) {
                    int v = UI::ThemedDialog::PromptInt(hWnd, L"Dir Thumb Cache Budget",
                        L"Thumbnail cache budget in MB (100 – 64000):",
                        app.dirThumbCacheMB, 100, 64000, Constants::IS_DIR_THUMB_CACHE_BUDGET_MB);
                    if (v >= 0) {
                        app.dirThumbCacheMB = v;
                        Persistence::Registry::SaveSetting(Constants::Registry::DIR_THUMB_CACHE_MB,
                            static_cast<DWORD>(app.dirThumbCacheMB));
                    }
                } else if (cmd == 29) {
                    int v = UI::ThemedDialog::PromptInt(hWnd, L"Preload Lookaside",
                        L"Images to preload ahead and behind (1 – 99):",
                        app.preloadLookaside, 1, 99, Constants::IS_PRELOAD_LOOKASIDE_COUNT);
                    if (v >= 0) {
                        app.preloadLookaside = v;
                        Persistence::Registry::SaveSetting(Constants::Registry::PRELOAD_LOOKASIDE,
                            static_cast<DWORD>(app.preloadLookaside));
                    }
                } else if (cmd == 30) {
                    int v = UI::ThemedDialog::PromptInt(hWnd, L"Overlay Message Duration",
                        L"How long center messages are shown in ms (250 – 10000):",
                        app.msgCenterDisplayMs, 250, 10000, static_cast<int>(Constants::Overlay::IS_MSG_CENTER_DISPLAY_MS));
                    if (v >= 0) {
                        app.msgCenterDisplayMs = v;
                        Persistence::Registry::SaveSetting(Constants::Registry::MSG_CENTER_MS,
                            static_cast<DWORD>(app.msgCenterDisplayMs));
                    }
                } else if (cmd == 31) {
                    int v = UI::ThemedDialog::PromptInt(hWnd, L"History Save Limit",
                        L"Maximum folders to remember on disk (1 – 99999):",
                        app.historyMaxDirsSave, 1, 99999, Constants::History::IS_HISTORY_MAX_DIRS_TO_SAVE);
                    if (v >= 0) {
                        app.historyMaxDirsSave = v;
                        Persistence::Registry::SaveSetting(Constants::Registry::HISTORY_MAX_DIRS_SAVE,
                            static_cast<DWORD>(app.historyMaxDirsSave));
                    }
                } else if (cmd == 32) {
                    int v = UI::ThemedDialog::PromptInt(hWnd, L"Slideshow Interval",
                        L"Time between slides in ms (100 – 60000):",
                        app.slideshow.intervalMs, 100, 60000, Constants::Slideshow::IS_INTERVAL_MS);
                    if (v >= 0) {
                        app.slideshow.intervalMs = v;
                        Persistence::Registry::SaveSetting(Constants::Registry::SLIDESHOW_INTERVAL_MS,
                            static_cast<DWORD>(app.slideshow.intervalMs));
                    }
                } else if (cmd == 33) {
                    app.slideshow.loop = !app.slideshow.loop;
                    Persistence::Registry::SaveSetting(Constants::Registry::SLIDESHOW_LOOP,
                        static_cast<DWORD>(app.slideshow.loop));
                } else if (cmd == 34) {
                    app.slideshow.shuffle = !app.slideshow.shuffle;
                    Persistence::Registry::SaveSetting(Constants::Registry::SLIDESHOW_SHUFFLE,
                        static_cast<DWORD>(app.slideshow.shuffle));
                } else if (cmd >= 35 && cmd <= 40) {
                    int typeNum = cmd - 35; // 35→Cut(0), 36→Fade(1), 37→Dissolve(2), 38→Ripple(3), 39→Push(4), 40→Zoom(5)
                    app.slideshow.transition.type = static_cast<TransitionType>(typeNum);
                    Persistence::Registry::SaveSetting(Constants::Registry::SLIDESHOW_TRANSITION,
                        static_cast<DWORD>(typeNum));
                } else if (cmd == 17) {
                    int v = UI::ThemedDialog::PromptInt(hWnd, L"VRAM Image Cache",
                        L"Number of images to cache in VRAM (0 – 999):",
                        app.vramCacheCount, 0, 999, Constants::IS_VRAM_CACHE_IMAGES_COUNT);
                    if (v >= 0) {
                        app.vramCacheCount = v;
                        Persistence::Registry::SaveSetting(Constants::Registry::VRAM_CACHE_COUNT,
                            static_cast<DWORD>(app.vramCacheCount));
                    }
                } else if (cmd == 10) {
                    SYSTEMTIME st{};
                    GetLocalTime(&st);
                    wchar_t defaultName[MAX_PATH];
                    swprintf_s(defaultName, L"%s%04d%02d%02d%s",
                               Constants::SettingsFile::EXPORT_PREFIX,
                               st.wYear, st.wMonth, st.wDay,
                               Constants::SettingsFile::EXPORT_EXTENSION);
                    std::wstring exportPath;
                    {
                        IFileSaveDialog *pfd = nullptr;
                        if (SUCCEEDED(CoCreateInstance(CLSID_FileSaveDialog, nullptr,
                                                       CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd)))) {
                            COMDLG_FILTERSPEC filters[] = {
                                {L"INI Settings", L"*.ini"},
                                {L"All Files",    L"*.*" }
                            };
                            pfd->SetFileTypes(ARRAYSIZE(filters), filters);
                            pfd->SetDefaultExtension(L"ini");
                            pfd->SetFileName(defaultName);
                            if (SUCCEEDED(pfd->Show(hWnd))) {
                                IShellItem *psi = nullptr;
                                if (SUCCEEDED(pfd->GetResult(&psi))) {
                                    PWSTR path = nullptr;
                                    if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                                        exportPath = path;
                                        CoTaskMemFree(path);
                                    }
                                    psi->Release();
                                }
                            }
                            pfd->Release();
                        }
                    }
                    if (!exportPath.empty()) {
                        FILE *f = nullptr;
                        if (_wfopen_s(&f, exportPath.c_str(), L"w,ccs=UTF-8") == 0 && f) {
                            fwprintf(f, L"[QuickImageViewer]\n");
                            fwprintf(f, L"%s=%d\n", Constants::Registry::KEEP_IN_BACKGROUND,  (int)app.isKeepInBackground);
                            fwprintf(f, L"%s=%d\n", Constants::Registry::RUN_ON_STARTUP,       (int)app.isEnableRunOnStartup);
                            fwprintf(f, L"%s=%d\n", Constants::Registry::THUMBNAIL_EFFECTS,    (int)app.thumbnailEffectsEnabled);
                            fwprintf(f, L"%s=%d\n", Constants::Registry::HISTORY_FULL_MODE,    (int)app.historyFullModeEnabled);
                            fwprintf(f, L"%s=%d\n", Constants::Registry::OVERLAY_VISIBLE,      (int)app.showOverlayInfoText);
                            fwprintf(f, L"%s=%d\n", Constants::Registry::OPEN_DIRWND_ON_START, (int)app.openDirWndOnStart);
                            fwprintf(f, L"%s=%d\n", Constants::Registry::OVERLAY_SHOW_BG,      (int)app.overlayShowBackground);
                            fwprintf(f, L"%s=%d\n", Constants::Registry::SWAP_MOUSE_BUTTONS,   (int)app.swapMouseButtons);
                            fwprintf(f, L"%s=%d\n", Constants::Registry::WHEEL_INVERT,         (int)app.invertWheelDirection);
                            fwprintf(f, L"%s=%d\n", Constants::Registry::WHEEL_INVERT_H,       (int)app.invertWheelDirectionH);
                            fwprintf(f, L"%s=%d\n", Constants::Registry::VRAM_CACHE_COUNT,     app.vramCacheCount);
                            fwprintf(f, L"%s=%d\n", Constants::Registry::VIEW_MODE,            static_cast<int>(app.viewMode));
                            fwprintf(f, L"%s=%d\n", Constants::Registry::BASE_WIDTH_KEY,       app.baseWidth);
                            fwprintf(f, L"%s=%d\n", Constants::Registry::BASE_HEIGHT_KEY,      app.baseHeight);
                            fwprintf(f, L"%s=%d\n", Constants::Registry::START_FULLSCREEN,     (int)app.startInFullscreen);
                            fwprintf(f, L"%s=%d\n", Constants::Registry::HISTORY_MAX_DIRS,     app.historyMaxDirs);
                            fwprintf(f, L"%s=%d\n", Constants::Registry::HISTORY_MAX_FAVS,     app.historyMaxFavs);
                            fwprintf(f, L"%s=%d\n", Constants::Registry::DIR_THUMB_CACHE_MB,   app.dirThumbCacheMB);
                            fwprintf(f, L"%s=%d\n", Constants::Registry::PRELOAD_LOOKASIDE,    app.preloadLookaside);
                            fwprintf(f, L"%s=%d\n", Constants::Registry::MSG_CENTER_MS,        app.msgCenterDisplayMs);
                            fwprintf(f, L"%s=%d\n", Constants::Registry::HISTORY_MAX_DIRS_SAVE, app.historyMaxDirsSave);
                            fwprintf(f, L"%s=%d\n", Constants::Registry::SLIDESHOW_INTERVAL_MS, app.slideshow.intervalMs);
                            fwprintf(f, L"%s=%d\n", Constants::Registry::SLIDESHOW_LOOP,         (int)app.slideshow.loop);
                            fwprintf(f, L"%s=%d\n", Constants::Registry::SLIDESHOW_SHUFFLE,      (int)app.slideshow.shuffle);
                            fwprintf(f, L"%s=%d\n", Constants::Registry::SLIDESHOW_TRANSITION,   static_cast<int>(app.slideshow.transition.type));
                            fwprintf(f, L"%s=%d\n", Constants::Registry::SORT_ORDER,             app.fileHandlerDefaultSortOrder);
                            fwprintf(f, L"%s=%d\n", Constants::Registry::SORT_REVERSE,           (int)app.fileHandlerIsReverseSortOrder);
                            fwprintf(f, L"%s=%d\n", Constants::Registry::THEME_FACTOR,           (int)(app.themeFactor * 100.0f));
                            fclose(f);
                            UI::ThemedDialog::Message(hWnd, L"Settings exported successfully.", L"Export Settings");
                        } else {
                            UI::ThemedDialog::Message(hWnd, L"Failed to write the settings file.", L"Export Settings");
                        }
                    }
                } else if (cmd == 11) {
                    std::wstring importPath;
                    {
                        IFileOpenDialog *pfd = nullptr;
                        if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                                       CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd)))) {
                            COMDLG_FILTERSPEC filters[] = {
                                {L"INI Settings", L"*.ini"},
                                {L"All Files",    L"*.*" }
                            };
                            pfd->SetFileTypes(ARRAYSIZE(filters), filters);
                            DWORD opts = 0;
                            pfd->GetOptions(&opts);
                            pfd->SetOptions(opts | FOS_FILEMUSTEXIST);
                            if (SUCCEEDED(pfd->Show(hWnd))) {
                                IShellItem *psi = nullptr;
                                if (SUCCEEDED(pfd->GetResult(&psi))) {
                                    PWSTR path = nullptr;
                                    if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                                        importPath = path;
                                        CoTaskMemFree(path);
                                    }
                                    psi->Release();
                                }
                            }
                            pfd->Release();
                        }
                    }
                    if (!importPath.empty() &&
                        UI::ThemedDialog::Confirm(hWnd, L"Importing will overwrite all current settings. Continue?", L"Import Settings")) {
                        FILE *f = nullptr;
                        if (_wfopen_s(&f, importPath.c_str(), L"r,ccs=UTF-8") == 0 && f) {
                            wchar_t line[512];
                            bool anyKey = false;
                            while (fgetws(line, 512, f)) {
                                size_t len = wcslen(line);
                                while (len > 0 && (line[len-1] == L'\n' || line[len-1] == L'\r')) line[--len] = L'\0';
                                if (len == 0 || line[0] == L'[') continue;
                                wchar_t *eq = wcschr(line, L'=');
                                if (!eq) continue;
                                *eq = L'\0';
                                const wchar_t *key = line;
                                int val = _wtoi(eq + 1);
                                anyKey = true;
                                auto applyBool = [&](const wchar_t *regKey, bool &field) {
                                    if (wcscmp(key, regKey) == 0) {
                                        field = val != 0;
                                        Persistence::Registry::SaveSetting(regKey, static_cast<DWORD>(field));
                                    }
                                };
                                applyBool(Constants::Registry::KEEP_IN_BACKGROUND,  app.isKeepInBackground);
                                applyBool(Constants::Registry::RUN_ON_STARTUP,       app.isEnableRunOnStartup);
                                applyBool(Constants::Registry::THUMBNAIL_EFFECTS,    app.thumbnailEffectsEnabled);
                                applyBool(Constants::Registry::HISTORY_FULL_MODE,    app.historyFullModeEnabled);
                                applyBool(Constants::Registry::OVERLAY_VISIBLE,      app.showOverlayInfoText);
                                applyBool(Constants::Registry::OPEN_DIRWND_ON_START, app.openDirWndOnStart);
                                applyBool(Constants::Registry::OVERLAY_SHOW_BG,      app.overlayShowBackground);
                                applyBool(Constants::Registry::SWAP_MOUSE_BUTTONS,   app.swapMouseButtons);
                                applyBool(Constants::Registry::WHEEL_INVERT,         app.invertWheelDirection);
                                applyBool(Constants::Registry::WHEEL_INVERT_H,       app.invertWheelDirectionH);
                                if (wcscmp(key, Constants::Registry::VRAM_CACHE_COUNT) == 0) {
                                    app.vramCacheCount = max(0, min(999, val));
                                    Persistence::Registry::SaveSetting(Constants::Registry::VRAM_CACHE_COUNT,
                                        static_cast<DWORD>(app.vramCacheCount));
                                }
                                if (wcscmp(key, Constants::Registry::VIEW_MODE) == 0) {
                                    int m = max(1, min(5, val));
                                    app.viewMode = static_cast<Constants::ViewModes::ViewMode>(m);
                                    Persistence::Registry::SaveSetting(Constants::Registry::VIEW_MODE,
                                        static_cast<DWORD>(m));
                                }
                                if (wcscmp(key, Constants::Registry::BASE_WIDTH_KEY) == 0) {
                                    app.baseWidth = max(240, min(16000, val));
                                    Persistence::Registry::SaveSetting(Constants::Registry::BASE_WIDTH_KEY,
                                        static_cast<DWORD>(app.baseWidth));
                                }
                                if (wcscmp(key, Constants::Registry::BASE_HEIGHT_KEY) == 0) {
                                    app.baseHeight = max(240, min(16000, val));
                                    Persistence::Registry::SaveSetting(Constants::Registry::BASE_HEIGHT_KEY,
                                        static_cast<DWORD>(app.baseHeight));
                                }
                                applyBool(Constants::Registry::START_FULLSCREEN, app.startInFullscreen);
                                if (wcscmp(key, Constants::Registry::HISTORY_MAX_DIRS) == 0) {
                                    app.historyMaxDirs = max(0, min(999, val));
                                    Persistence::Registry::SaveSetting(Constants::Registry::HISTORY_MAX_DIRS,
                                        static_cast<DWORD>(app.historyMaxDirs));
                                }
                                if (wcscmp(key, Constants::Registry::HISTORY_MAX_FAVS) == 0) {
                                    app.historyMaxFavs = max(0, min(999, val));
                                    Persistence::Registry::SaveSetting(Constants::Registry::HISTORY_MAX_FAVS,
                                        static_cast<DWORD>(app.historyMaxFavs));
                                }
                                if (wcscmp(key, Constants::Registry::DIR_THUMB_CACHE_MB) == 0) {
                                    app.dirThumbCacheMB = max(100, min(64000, val));
                                    Persistence::Registry::SaveSetting(Constants::Registry::DIR_THUMB_CACHE_MB,
                                        static_cast<DWORD>(app.dirThumbCacheMB));
                                }
                                if (wcscmp(key, Constants::Registry::PRELOAD_LOOKASIDE) == 0) {
                                    app.preloadLookaside = max(1, min(99, val));
                                    Persistence::Registry::SaveSetting(Constants::Registry::PRELOAD_LOOKASIDE,
                                        static_cast<DWORD>(app.preloadLookaside));
                                }
                                if (wcscmp(key, Constants::Registry::MSG_CENTER_MS) == 0) {
                                    app.msgCenterDisplayMs = max(250, min(10000, val));
                                    Persistence::Registry::SaveSetting(Constants::Registry::MSG_CENTER_MS,
                                        static_cast<DWORD>(app.msgCenterDisplayMs));
                                }
                                if (wcscmp(key, Constants::Registry::HISTORY_MAX_DIRS_SAVE) == 0) {
                                    app.historyMaxDirsSave = max(1, min(99999, val));
                                    Persistence::Registry::SaveSetting(Constants::Registry::HISTORY_MAX_DIRS_SAVE,
                                        static_cast<DWORD>(app.historyMaxDirsSave));
                                }
                                if (wcscmp(key, Constants::Registry::SLIDESHOW_INTERVAL_MS) == 0) {
                                    app.slideshow.intervalMs = max(100, min(60000, val));
                                    Persistence::Registry::SaveSetting(Constants::Registry::SLIDESHOW_INTERVAL_MS,
                                        static_cast<DWORD>(app.slideshow.intervalMs));
                                }
                                applyBool(Constants::Registry::SLIDESHOW_LOOP,    app.slideshow.loop);
                                applyBool(Constants::Registry::SLIDESHOW_SHUFFLE, app.slideshow.shuffle);
                                if (wcscmp(key, Constants::Registry::SLIDESHOW_TRANSITION) == 0) {
                                    int t = max(0, min(5, val));
                                    app.slideshow.transition.type = static_cast<TransitionType>(t);
                                    Persistence::Registry::SaveSetting(Constants::Registry::SLIDESHOW_TRANSITION,
                                        static_cast<DWORD>(t));
                                }
                                if (wcscmp(key, Constants::Registry::SORT_ORDER) == 0) {
                                    app.fileHandlerDefaultSortOrder = max(0, min(4, val));
                                    Persistence::Registry::SaveSetting(Constants::Registry::SORT_ORDER,
                                        static_cast<DWORD>(app.fileHandlerDefaultSortOrder));
                                }
                                if (wcscmp(key, Constants::Registry::SORT_REVERSE) == 0) {
                                    app.fileHandlerIsReverseSortOrder = val != 0;
                                    Persistence::Registry::SaveSetting(Constants::Registry::SORT_REVERSE,
                                        static_cast<DWORD>(app.fileHandlerIsReverseSortOrder));
                                }
                                if (wcscmp(key, Constants::Registry::THEME_FACTOR) == 0) {
                                    app.themeFactor = static_cast<float>(val) / 100.0f;
                                    Persistence::Registry::SaveSetting(Constants::Registry::THEME_FACTOR, static_cast<DWORD>(val));
                                }
                            }
                            fclose(f);
                            if (anyKey) {
                                // Re-init AppState from registry (mirrors startup logic)
                                app.isEnableRunOnStartup = Persistence::Registry::LoadSetting(
                                    Constants::Registry::RUN_ON_STARTUP,
                                    static_cast<DWORD>(Constants::IS_ENABLE_RUN_ON_STARTUP)) != 0;
                                app.isKeepInBackground = Persistence::Registry::LoadSetting(
                                    Constants::Registry::KEEP_IN_BACKGROUND,
                                    static_cast<DWORD>(Constants::IS_KEEP_IN_BACKGROUND)) != 0;
                                app.thumbnailEffectsEnabled = Persistence::Registry::LoadSetting(
                                    Constants::Registry::THUMBNAIL_EFFECTS,
                                    static_cast<DWORD>(Constants::ThumbnailPanel::ThumbnailEffects::EFFECTS_MASTER_ENABLED)) != 0;
                                app.historyFullModeEnabled = Persistence::Registry::LoadSetting(
                                    Constants::Registry::HISTORY_FULL_MODE,
                                    static_cast<DWORD>(Constants::History::HISTORY_SHOW_FULL_HISTORY)) != 0;
                                app.showOverlayInfoText = Persistence::Registry::LoadSetting(
                                    Constants::Registry::OVERLAY_VISIBLE,
                                    static_cast<DWORD>(Constants::Overlay::DEFAULT_SHOW_OVERLAY)) != 0;
                                app.openDirWndOnStart = Persistence::Registry::LoadSetting(
                                    Constants::Registry::OPEN_DIRWND_ON_START,
                                    static_cast<DWORD>(Constants::IS_OPEN_DIRWND_ON_START)) != 0;
                                app.overlayShowBackground = Persistence::Registry::LoadSetting(
                                    Constants::Registry::OVERLAY_SHOW_BG,
                                    static_cast<DWORD>(Constants::Overlay::IS_OVERLAY_SHOW_BACKGROUND)) != 0;
                                app.swapMouseButtons = Persistence::Registry::LoadSetting(
                                    Constants::Registry::SWAP_MOUSE_BUTTONS,
                                    static_cast<DWORD>(Constants::IS_SWAP_MOUSE_BUTTONS)) != 0;
                                app.invertWheelDirection = Persistence::Registry::LoadSetting(
                                    Constants::Registry::WHEEL_INVERT,
                                    static_cast<DWORD>(Constants::IS_MOUSE_VERTICAL_REVERSE_SCROLL_DIRECTION)) != 0;
                                app.invertWheelDirectionH = Persistence::Registry::LoadSetting(
                                    Constants::Registry::WHEEL_INVERT_H,
                                    static_cast<DWORD>(Constants::IS_MOUSE_HORIZONTAL_REVERSE_SCROLL_DIRECTION)) != 0;
                                app.vramCacheCount = max(0, min(999, static_cast<int>(
                                    Persistence::Registry::LoadSetting(Constants::Registry::VRAM_CACHE_COUNT,
                                        static_cast<DWORD>(Constants::IS_VRAM_CACHE_IMAGES_COUNT)))));
                                app.baseWidth = max(240, min(16000, static_cast<int>(
                                    Persistence::Registry::LoadSetting(Constants::Registry::BASE_WIDTH_KEY,
                                        static_cast<DWORD>(Constants::IS_BASE_WIDTH)))));
                                app.baseHeight = max(240, min(16000, static_cast<int>(
                                    Persistence::Registry::LoadSetting(Constants::Registry::BASE_HEIGHT_KEY,
                                        static_cast<DWORD>(Constants::IS_BASE_HEIGHT)))));
                                app.startInFullscreen = Persistence::Registry::LoadSetting(
                                    Constants::Registry::START_FULLSCREEN, 0u) != 0;
                                app.historyMaxDirs = max(0, min(999, static_cast<int>(
                                    Persistence::Registry::LoadSetting(Constants::Registry::HISTORY_MAX_DIRS,
                                        static_cast<DWORD>(Constants::History::IS_HISTORY_MAX_DIRS_TO_SHOW)))));
                                app.historyMaxFavs = max(0, min(999, static_cast<int>(
                                    Persistence::Registry::LoadSetting(Constants::Registry::HISTORY_MAX_FAVS,
                                        static_cast<DWORD>(Constants::History::IS_HISTORY_MAX_FAVORITES_TO_SHOW)))));
                                app.dirThumbCacheMB = max(100, min(64000, static_cast<int>(
                                    Persistence::Registry::LoadSetting(Constants::Registry::DIR_THUMB_CACHE_MB,
                                        static_cast<DWORD>(Constants::IS_DIR_THUMB_CACHE_BUDGET_MB)))));
                                app.preloadLookaside = max(1, min(99, static_cast<int>(
                                    Persistence::Registry::LoadSetting(Constants::Registry::PRELOAD_LOOKASIDE,
                                        static_cast<DWORD>(Constants::IS_PRELOAD_LOOKASIDE_COUNT)))));
                                app.msgCenterDisplayMs = max(250, min(10000, static_cast<int>(
                                    Persistence::Registry::LoadSetting(Constants::Registry::MSG_CENTER_MS,
                                        static_cast<DWORD>(Constants::Overlay::IS_MSG_CENTER_DISPLAY_MS)))));
                                app.historyMaxDirsSave = max(1, min(99999, static_cast<int>(
                                    Persistence::Registry::LoadSetting(Constants::Registry::HISTORY_MAX_DIRS_SAVE,
                                        static_cast<DWORD>(Constants::History::IS_HISTORY_MAX_DIRS_TO_SAVE)))));
                                app.slideshow.intervalMs = max(100, min(60000, static_cast<int>(
                                    Persistence::Registry::LoadSetting(Constants::Registry::SLIDESHOW_INTERVAL_MS,
                                        static_cast<DWORD>(Constants::Slideshow::IS_INTERVAL_MS)))));
                                app.slideshow.loop = Persistence::Registry::LoadSetting(
                                    Constants::Registry::SLIDESHOW_LOOP,
                                    static_cast<DWORD>(Constants::Slideshow::IS_LOOP)) != 0;
                                app.slideshow.shuffle = Persistence::Registry::LoadSetting(
                                    Constants::Registry::SLIDESHOW_SHUFFLE,
                                    static_cast<DWORD>(Constants::Slideshow::IS_SHUFFLE)) != 0;
                                {
                                    int t = max(0, min(5, static_cast<int>(
                                        Persistence::Registry::LoadSetting(Constants::Registry::SLIDESHOW_TRANSITION,
                                            static_cast<DWORD>(TransitionType::Cut)))));
                                    app.slideshow.transition.type = static_cast<TransitionType>(t);
                                }
                                app.fileHandlerDefaultSortOrder = max(0, min(4, static_cast<int>(
                                    Persistence::Registry::LoadSetting(Constants::Registry::SORT_ORDER,
                                        static_cast<DWORD>(Constants::FileHandler::FILE_HANDLER_DEFAULT_SORT_ORDER)))));
                                app.fileHandlerIsReverseSortOrder = Persistence::Registry::LoadSetting(
                                    Constants::Registry::SORT_REVERSE,
                                    static_cast<DWORD>(Constants::FileHandler::FILE_HANDLER_SORT_TYPE_IS_REVERSE)) != 0;
                                ReSortPlaylistAndRebuildMap(hWnd);
                                {
                                    int m = max(1, min(5, static_cast<int>(
                                        Persistence::Registry::LoadSetting(Constants::Registry::VIEW_MODE,
                                            static_cast<DWORD>(Constants::ViewModes::defaultViewMode)))));
                                    app.viewMode = static_cast<Constants::ViewModes::ViewMode>(m);
                                }
                                DWORD themeFactorDWORD = Persistence::Registry::LoadSetting(
                                    Constants::Registry::THEME_FACTOR,
                                    static_cast<DWORD>(Constants::Theme::DEFAULT_THEME_FACTOR));
                                app.themeFactor = static_cast<float>(themeFactorDWORD) / 100.0f;
                                // Apply side effects
                                Persistence::Registry::EnableRunOnStartup(app.isEnableRunOnStartup);
                                g_overlayManager.SetAllVisible(app.showOverlayInfoText);
                                AppCommands::changeAppThemeFactor(hWnd, app.themeFactor);
                                uiManager.RepaintAllPanels();
                                InvalidateRect(hWnd, nullptr, FALSE);
                                UI::ThemedDialog::Message(hWnd, L"Settings imported successfully.", L"Import Settings");
                            } else {
                                UI::ThemedDialog::Message(hWnd, L"The file appears to be empty or invalid.", L"Import Settings");
                            }
                        } else {
                            UI::ThemedDialog::Message(hWnd, L"Failed to read the settings file.", L"Import Settings");
                        }
                    }
                } else if (cmd == 12) {
                    if (!UI::ThemedDialog::Confirm(hWnd,
                            L"All settings will be reset to their default values.\nThis cannot be undone.",
                            L"Restore Defaults")) break;
                    app.isKeepInBackground   = Constants::IS_KEEP_IN_BACKGROUND;
                    app.isEnableRunOnStartup = Constants::IS_ENABLE_RUN_ON_STARTUP;
                    app.thumbnailEffectsEnabled = Constants::ThumbnailPanel::ThumbnailEffects::EFFECTS_MASTER_ENABLED;
                    app.historyFullModeEnabled  = Constants::History::HISTORY_SHOW_FULL_HISTORY;
                    app.showOverlayInfoText     = Constants::Overlay::DEFAULT_SHOW_OVERLAY;
                    app.openDirWndOnStart       = Constants::IS_OPEN_DIRWND_ON_START;
                    app.overlayShowBackground   = Constants::Overlay::IS_OVERLAY_SHOW_BACKGROUND;
                    app.swapMouseButtons        = Constants::IS_SWAP_MOUSE_BUTTONS;
                    app.invertWheelDirection    = Constants::IS_MOUSE_VERTICAL_REVERSE_SCROLL_DIRECTION;
                    app.invertWheelDirectionH   = Constants::IS_MOUSE_HORIZONTAL_REVERSE_SCROLL_DIRECTION;
                    app.vramCacheCount          = Constants::IS_VRAM_CACHE_IMAGES_COUNT;
                    app.viewMode                = Constants::ViewModes::defaultViewMode;
                    app.baseWidth               = Constants::IS_BASE_WIDTH;
                    app.baseHeight              = Constants::IS_BASE_HEIGHT;
                    app.startInFullscreen       = false;
                    app.historyMaxDirs          = Constants::History::IS_HISTORY_MAX_DIRS_TO_SHOW;
                    app.historyMaxFavs          = Constants::History::IS_HISTORY_MAX_FAVORITES_TO_SHOW;
                    app.dirThumbCacheMB         = Constants::IS_DIR_THUMB_CACHE_BUDGET_MB;
                    app.preloadLookaside        = Constants::IS_PRELOAD_LOOKASIDE_COUNT;
                    app.msgCenterDisplayMs         = static_cast<int>(Constants::Overlay::IS_MSG_CENTER_DISPLAY_MS);
                    app.historyMaxDirsSave         = Constants::History::IS_HISTORY_MAX_DIRS_TO_SAVE;
                    app.slideshow.intervalMs       = Constants::Slideshow::IS_INTERVAL_MS;
                    app.slideshow.loop             = Constants::Slideshow::IS_LOOP;
                    app.slideshow.shuffle          = Constants::Slideshow::IS_SHUFFLE;
                    app.slideshow.transition.type  = TransitionType::Cut;
                    Persistence::Registry::SaveSetting(Constants::Registry::KEEP_IN_BACKGROUND,   static_cast<DWORD>(app.isKeepInBackground));
                    Persistence::Registry::SaveSetting(Constants::Registry::RUN_ON_STARTUP,        static_cast<DWORD>(app.isEnableRunOnStartup));
                    Persistence::Registry::SaveSetting(Constants::Registry::THUMBNAIL_EFFECTS,     static_cast<DWORD>(app.thumbnailEffectsEnabled));
                    Persistence::Registry::SaveSetting(Constants::Registry::HISTORY_FULL_MODE,     static_cast<DWORD>(app.historyFullModeEnabled));
                    Persistence::Registry::SaveSetting(Constants::Registry::OVERLAY_VISIBLE,       static_cast<DWORD>(app.showOverlayInfoText));
                    Persistence::Registry::SaveSetting(Constants::Registry::OPEN_DIRWND_ON_START,  static_cast<DWORD>(app.openDirWndOnStart));
                    Persistence::Registry::SaveSetting(Constants::Registry::OVERLAY_SHOW_BG,       static_cast<DWORD>(app.overlayShowBackground));
                    Persistence::Registry::SaveSetting(Constants::Registry::SWAP_MOUSE_BUTTONS,    static_cast<DWORD>(app.swapMouseButtons));
                    Persistence::Registry::SaveSetting(Constants::Registry::WHEEL_INVERT,          static_cast<DWORD>(app.invertWheelDirection));
                    Persistence::Registry::SaveSetting(Constants::Registry::WHEEL_INVERT_H,        static_cast<DWORD>(app.invertWheelDirectionH));
                    Persistence::Registry::SaveSetting(Constants::Registry::VRAM_CACHE_COUNT,      static_cast<DWORD>(app.vramCacheCount));
                    Persistence::Registry::SaveSetting(Constants::Registry::VIEW_MODE,             static_cast<DWORD>(app.viewMode));
                    Persistence::Registry::SaveSetting(Constants::Registry::BASE_WIDTH_KEY,        static_cast<DWORD>(app.baseWidth));
                    Persistence::Registry::SaveSetting(Constants::Registry::BASE_HEIGHT_KEY,       static_cast<DWORD>(app.baseHeight));
                    Persistence::Registry::SaveSetting(Constants::Registry::START_FULLSCREEN,      static_cast<DWORD>(app.startInFullscreen));
                    Persistence::Registry::SaveSetting(Constants::Registry::HISTORY_MAX_DIRS,      static_cast<DWORD>(app.historyMaxDirs));
                    Persistence::Registry::SaveSetting(Constants::Registry::HISTORY_MAX_FAVS,      static_cast<DWORD>(app.historyMaxFavs));
                    Persistence::Registry::SaveSetting(Constants::Registry::DIR_THUMB_CACHE_MB,    static_cast<DWORD>(app.dirThumbCacheMB));
                    Persistence::Registry::SaveSetting(Constants::Registry::PRELOAD_LOOKASIDE,     static_cast<DWORD>(app.preloadLookaside));
                    Persistence::Registry::SaveSetting(Constants::Registry::MSG_CENTER_MS,         static_cast<DWORD>(app.msgCenterDisplayMs));
                    Persistence::Registry::SaveSetting(Constants::Registry::HISTORY_MAX_DIRS_SAVE, static_cast<DWORD>(app.historyMaxDirsSave));
                    Persistence::Registry::SaveSetting(Constants::Registry::SLIDESHOW_INTERVAL_MS, static_cast<DWORD>(app.slideshow.intervalMs));
                    Persistence::Registry::SaveSetting(Constants::Registry::SLIDESHOW_LOOP,         static_cast<DWORD>(app.slideshow.loop));
                    Persistence::Registry::SaveSetting(Constants::Registry::SLIDESHOW_SHUFFLE,      static_cast<DWORD>(app.slideshow.shuffle));
                    Persistence::Registry::SaveSetting(Constants::Registry::SLIDESHOW_TRANSITION,   0u);
                    app.fileHandlerDefaultSortOrder   = Constants::FileHandler::FILE_HANDLER_DEFAULT_SORT_ORDER;
                    app.fileHandlerIsReverseSortOrder = Constants::FileHandler::FILE_HANDLER_SORT_TYPE_IS_REVERSE;
                    Persistence::Registry::SaveSetting(Constants::Registry::SORT_ORDER,   static_cast<DWORD>(app.fileHandlerDefaultSortOrder));
                    Persistence::Registry::SaveSetting(Constants::Registry::SORT_REVERSE, static_cast<DWORD>(app.fileHandlerIsReverseSortOrder));
                    Persistence::Registry::EnableRunOnStartup(app.isEnableRunOnStartup);
                    g_overlayManager.SetAllVisible(app.showOverlayInfoText);
                    uiManager.RepaintAllPanels();
                    InvalidateRect(hWnd, nullptr, FALSE);
                    UI::ThemedDialog::Message(hWnd, L"All settings have been restored to defaults.", L"Restore Defaults");
                // ── Sort Order ────────────────────────────────────────────────────────
                } else if (cmd >= 43 && cmd <= 47) {
                    app.fileHandlerDefaultSortOrder  = cmd - 43; // 43→0(Name)…47→4(Disk)
                    app.fileHandlerIsReverseSortOrder = false;
                    Persistence::Registry::SaveSetting(Constants::Registry::SORT_ORDER,   static_cast<DWORD>(app.fileHandlerDefaultSortOrder));
                    Persistence::Registry::SaveSetting(Constants::Registry::SORT_REVERSE, 0u);
                    ReSortPlaylistAndRebuildMap(hWnd);
                } else if (cmd == 48) {
                    app.fileHandlerIsReverseSortOrder = !app.fileHandlerIsReverseSortOrder;
                    Persistence::Registry::SaveSetting(Constants::Registry::SORT_REVERSE, static_cast<DWORD>(app.fileHandlerIsReverseSortOrder));
                    ReSortPlaylistAndRebuildMap(hWnd);
                // ── Backup History & Favorites ────────────────────────────────────────
                } else if (cmd == 41) {
                    SYSTEMTIME st{};
                    GetLocalTime(&st);
                    wchar_t defaultName[MAX_PATH];
                    swprintf_s(defaultName, L"%s%04d%02d%02d.zip",
                               Constants::Backup::BACKUP_PREFIX,
                               st.wYear, st.wMonth, st.wDay);

                    std::wstring zipPath;
                    {
                        IFileSaveDialog *pfd = nullptr;
                        if (SUCCEEDED(CoCreateInstance(CLSID_FileSaveDialog, nullptr,
                                                       CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd)))) {
                            COMDLG_FILTERSPEC filters[] = {
                                {L"ZIP Archive", L"*.zip"},
                                {L"All Files",   L"*.*" }
                            };
                            pfd->SetFileTypes(ARRAYSIZE(filters), filters);
                            pfd->SetDefaultExtension(L"zip");
                            pfd->SetFileName(defaultName);
                            if (SUCCEEDED(pfd->Show(hWnd))) {
                                IShellItem *psi = nullptr;
                                if (SUCCEEDED(pfd->GetResult(&psi))) {
                                    PWSTR pPath = nullptr;
                                    if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &pPath))) {
                                        zipPath = pPath;
                                        CoTaskMemFree(pPath);
                                    }
                                    psi->Release();
                                }
                            }
                            pfd->Release();
                        }
                    }
                    if (!zipPath.empty()) {
                        HistoryFoldersManager hfm;
                        std::wstring histPath = hfm.GetFilePath();
                        std::wstring favPath  = hfm.GetFavoritesFilePath();

                        // Read both files as raw bytes
                        auto readBytes = [](const std::wstring &p) -> std::vector<char> {
                            std::ifstream f(p, std::ios::binary);
                            if (!f.is_open()) return {};
                            return {std::istreambuf_iterator<char>(f), {}};
                        };
                        auto histBytes = readBytes(histPath);
                        auto favBytes  = readBytes(favPath);

                        // Archive entry names (UTF-8 of the standard constant filenames)
                        auto toUtf8 = [](const wchar_t *ws) -> std::string {
                            int n = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
                            std::string s(n - 1, '\0');
                            WideCharToMultiByte(CP_UTF8, 0, ws, -1, s.data(), n, nullptr, nullptr);
                            return s;
                        };
                        std::string histEntry = toUtf8(Constants::History::HISTORY_FILE_NAME);
                        std::string favEntry  = toUtf8(Constants::History::FAVORITES_FILE_NAME);

                        mz_zip_archive zip{};
                        bool ok = mz_zip_writer_init_heap(&zip, 0, 65536) == MZ_TRUE;
                        if (ok && !histBytes.empty())
                            ok = mz_zip_writer_add_mem(&zip, histEntry.c_str(),
                                                       histBytes.data(), histBytes.size(),
                                                       MZ_BEST_SPEED) == MZ_TRUE;
                        if (ok && !favBytes.empty())
                            ok = mz_zip_writer_add_mem(&zip, favEntry.c_str(),
                                                       favBytes.data(), favBytes.size(),
                                                       MZ_BEST_SPEED) == MZ_TRUE;
                        void  *pBuf   = nullptr;
                        size_t bufSz  = 0;
                        if (ok)
                            ok = mz_zip_writer_finalize_heap_archive(&zip, &pBuf, &bufSz) == MZ_TRUE;
                        mz_zip_writer_end(&zip);

                        if (ok && pBuf) {
                            FILE *fz = nullptr;
                            _wfopen_s(&fz, zipPath.c_str(), L"wb");
                            if (fz) {
                                fwrite(pBuf, 1, bufSz, fz);
                                fclose(fz);
                                UI::ThemedDialog::Message(hWnd, L"Backup created successfully.", L"Backup");
                            } else {
                                UI::ThemedDialog::Message(hWnd, L"Failed to write backup file.", L"Backup");
                            }
                        } else {
                            UI::ThemedDialog::Message(hWnd, L"Failed to create backup archive.", L"Backup");
                        }
                        mz_free(pBuf);
                    }
                // ── Restore History & Favorites ───────────────────────────────────────
                } else if (cmd == 42) {
                    if (!UI::ThemedDialog::Confirm(hWnd,
                            L"This will overwrite your current history and favorites with the selected backup.\nContinue?",
                            L"Restore Backup")) break;

                    std::wstring zipPath;
                    {
                        IFileOpenDialog *pfd = nullptr;
                        if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                                       CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd)))) {
                            COMDLG_FILTERSPEC filters[] = {
                                {L"ZIP Archive", L"*.zip"},
                                {L"All Files",   L"*.*" }
                            };
                            pfd->SetFileTypes(ARRAYSIZE(filters), filters);
                            pfd->SetDefaultExtension(L"zip");
                            if (SUCCEEDED(pfd->Show(hWnd))) {
                                IShellItem *psi = nullptr;
                                if (SUCCEEDED(pfd->GetResult(&psi))) {
                                    PWSTR pPath = nullptr;
                                    if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &pPath))) {
                                        zipPath = pPath;
                                        CoTaskMemFree(pPath);
                                    }
                                    psi->Release();
                                }
                            }
                            pfd->Release();
                        }
                    }
                    if (!zipPath.empty()) {
                        // Read entire zip into memory (avoids narrow-path issues with miniz)
                        std::vector<char> zipBytes;
                        {
                            std::ifstream fz(zipPath, std::ios::binary);
                            if (!fz.is_open()) {
                                UI::ThemedDialog::Message(hWnd, L"Failed to open backup file.", L"Restore Backup");
                                break;
                            }
                            zipBytes = {std::istreambuf_iterator<char>(fz), {}};
                        }

                        mz_zip_archive zip{};
                        if (!mz_zip_reader_init_mem(&zip, zipBytes.data(), zipBytes.size(), 0)) {
                            UI::ThemedDialog::Message(hWnd, L"The selected file is not a valid backup archive.", L"Restore Backup");
                            break;
                        }

                        HistoryFoldersManager hfm;

                        auto toUtf8 = [](const wchar_t *ws) -> std::string {
                            int n = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
                            std::string s(n - 1, '\0');
                            WideCharToMultiByte(CP_UTF8, 0, ws, -1, s.data(), n, nullptr, nullptr);
                            return s;
                        };
                        std::string histEntry = toUtf8(Constants::History::HISTORY_FILE_NAME);
                        std::string favEntry  = toUtf8(Constants::History::FAVORITES_FILE_NAME);

                        // Extract each entry to memory then write via wide path
                        auto extractEntry = [&](const std::string &entry, const std::wstring &destPath) -> bool {
                            size_t sz = 0;
                            void *pData = mz_zip_reader_extract_file_to_heap(&zip, entry.c_str(), &sz, 0);
                            if (!pData) return false;
                            FILE *f = nullptr;
                            _wfopen_s(&f, destPath.c_str(), L"wb");
                            bool wrote = false;
                            if (f) {
                                wrote = fwrite(pData, 1, sz, f) == sz;
                                fclose(f);
                            }
                            mz_free(pData);
                            return wrote;
                        };

                        bool histOk = extractEntry(histEntry, hfm.GetFilePath());
                        bool favOk  = extractEntry(favEntry,  hfm.GetFavoritesFilePath());
                        mz_zip_reader_end(&zip);

                        if (histOk || favOk) {
                            UI::LoadFolderHistoryFromDisk();
                            UI::ThemedDialog::Message(hWnd, L"Backup restored successfully.", L"Restore Backup");
                        } else {
                            UI::ThemedDialog::Message(hWnd,
                                L"No history or favorites entries were found in the archive.",
                                L"Restore Backup");
                        }
                    }
                }
            }
            return 0;
        }


        case WM_CLOSE: {
            if (app.isKeepInBackground) {
                ShowWindow(hWnd, SW_HIDE);
                AppCommands::AddTrayIcon(hWnd);
            } else {
                AppCommands::RemoveTrayIcon(hWnd);
                DestroyWindow(hWnd);
            }
            return 0;
        }

        case WM_SETCURSOR:
            if (g_scanInProgress.load(std::memory_order_relaxed)) {
                SetCursor(LoadCursorW(nullptr, IDC_APPSTARTING));
                return TRUE;
            }
            break;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hWnd, message, wParam, lParam);
}

//////////////////////////////////////////////////////
//////////////////// Main entry point /////////////////
////////////////////////////////////////////////////////
static bool HasAVX2Support() {
    int cpu[4] = {};
    __cpuid(cpu, 1);
    if ((cpu[2] & (1 << 27)) == 0) return false; // no OSXSAVE — OS didn't enable XSAVE
    if ((cpu[2] & (1 << 28)) == 0) return false; // no AVX
    if ((_xgetbv(0) & 0x6) != 0x6) return false; // OS hasn't enabled YMM state saving
    __cpuidex(cpu, 7, 0);
    return (cpu[1] & (1 << 5)) != 0; // EBX bit 5 = AVX2
}

int WINAPI wWinMain(HINSTANCE hInstance, [[maybe_unused]] HINSTANCE hPrevInstance, [[maybe_unused]] PWSTR pCmdLine, int nCmdShow) {
    if (!HasAVX2Support()) {
        TaskDialog(nullptr, nullptr,
                   L"QuickImageViewer — CPU not supported",
                   L"AVX2 instructions are required",
                   L"Your CPU does not support AVX2 instructions.\n\n"
                   L"QIV requires a processor with AVX2 support\n"
                   L"(Intel Core 4th gen / AMD Ryzen or newer).",
                   TDCBF_OK_BUTTON, TD_ERROR_ICON, nullptr);
        return 1;
    }

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
    dirThumbThreads = std::min(dirThumbThreads, 8); // IShellItemImageFactory::GetImage serializes internally; >8 gives no gain
    g_dirThumbWorker.setThreadCount(std::max(1, dirThumbThreads));

    // Warm up GeoNames data in the background so the first ExifWnd GPS lookup
    // doesn't stall the IO worker with a 100-500 ms decompress+parse.
    g_decoderWorker.PushTask([](IWICImagingFactory2 *) {
        GeoNames::WarmUp();
    });
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

    // --- COMMAND LINE PARSING (before registry — args are highest priority) ---
    int argc;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const CmdArgs earlyArgs = ParseCmdArgs(argc, argv);
    // Save the raw first argument before freeing — used by the single-instance wake signal.
    const std::wstring firstRawArg = (argc > 1) ? std::wstring(argv[1]) : L"";
    LocalFree(argv);
    argv = nullptr; // guard against accidental use below

    // Dedicated mode must be known before any registry call so PrefixedName() works.
    if (earlyArgs.dedicated) app.isDedicated = true;

    // Registry is the source of truth for user preferences in all modes.
    app.isEnableRunOnStartup = Persistence::Registry::LoadSetting(
        Constants::Registry::RUN_ON_STARTUP,
        static_cast<DWORD>(Constants::IS_ENABLE_RUN_ON_STARTUP)) != 0;
    app.isKeepInBackground = Persistence::Registry::LoadSetting(
        Constants::Registry::KEEP_IN_BACKGROUND,
        static_cast<DWORD>(Constants::IS_KEEP_IN_BACKGROUND)) != 0;
    app.thumbnailEffectsEnabled = Persistence::Registry::LoadSetting(
        Constants::Registry::THUMBNAIL_EFFECTS,
        static_cast<DWORD>(Constants::ThumbnailPanel::ThumbnailEffects::EFFECTS_MASTER_ENABLED)) != 0;
    app.historyFullModeEnabled = Persistence::Registry::LoadSetting(
        Constants::Registry::HISTORY_FULL_MODE,
        static_cast<DWORD>(Constants::History::HISTORY_SHOW_FULL_HISTORY)) != 0;
    app.showOverlayInfoText = Persistence::Registry::LoadSetting(
        Constants::Registry::OVERLAY_VISIBLE,
        static_cast<DWORD>(Constants::Overlay::DEFAULT_SHOW_OVERLAY)) != 0;
    app.openDirWndOnStart = Persistence::Registry::LoadSetting(
        Constants::Registry::OPEN_DIRWND_ON_START,
        static_cast<DWORD>(Constants::IS_OPEN_DIRWND_ON_START)) != 0;
    app.overlayShowBackground = Persistence::Registry::LoadSetting(
        Constants::Registry::OVERLAY_SHOW_BG,
        static_cast<DWORD>(Constants::Overlay::IS_OVERLAY_SHOW_BACKGROUND)) != 0;
    app.swapMouseButtons = Persistence::Registry::LoadSetting(
        Constants::Registry::SWAP_MOUSE_BUTTONS,
        static_cast<DWORD>(Constants::IS_SWAP_MOUSE_BUTTONS)) != 0;
    app.invertWheelDirection = Persistence::Registry::LoadSetting(
        Constants::Registry::WHEEL_INVERT,
        static_cast<DWORD>(Constants::IS_MOUSE_VERTICAL_REVERSE_SCROLL_DIRECTION)) != 0;
    app.invertWheelDirectionH = Persistence::Registry::LoadSetting(
        Constants::Registry::WHEEL_INVERT_H,
        static_cast<DWORD>(Constants::IS_MOUSE_HORIZONTAL_REVERSE_SCROLL_DIRECTION)) != 0;
    app.vramCacheCount = max(0, min(999, static_cast<int>(
        Persistence::Registry::LoadSetting(Constants::Registry::VRAM_CACHE_COUNT,
            static_cast<DWORD>(Constants::IS_VRAM_CACHE_IMAGES_COUNT)))));
    {
        int m = max(1, min(5, static_cast<int>(
            Persistence::Registry::LoadSetting(Constants::Registry::VIEW_MODE,
                static_cast<DWORD>(Constants::ViewModes::defaultViewMode)))));
        app.viewMode = static_cast<Constants::ViewModes::ViewMode>(m);
    }
    app.baseWidth = max(240, min(16000, static_cast<int>(
        Persistence::Registry::LoadSetting(Constants::Registry::BASE_WIDTH_KEY,
            static_cast<DWORD>(Constants::IS_BASE_WIDTH)))));
    app.baseHeight = max(240, min(16000, static_cast<int>(
        Persistence::Registry::LoadSetting(Constants::Registry::BASE_HEIGHT_KEY,
            static_cast<DWORD>(Constants::IS_BASE_HEIGHT)))));
    app.startInFullscreen = Persistence::Registry::LoadSetting(
        Constants::Registry::START_FULLSCREEN, 0u) != 0;
    app.historyMaxDirs = max(0, min(999, static_cast<int>(
        Persistence::Registry::LoadSetting(Constants::Registry::HISTORY_MAX_DIRS,
            static_cast<DWORD>(Constants::History::IS_HISTORY_MAX_DIRS_TO_SHOW)))));
    app.historyMaxFavs = max(0, min(999, static_cast<int>(
        Persistence::Registry::LoadSetting(Constants::Registry::HISTORY_MAX_FAVS,
            static_cast<DWORD>(Constants::History::IS_HISTORY_MAX_FAVORITES_TO_SHOW)))));
    app.dirThumbCacheMB = max(100, min(64000, static_cast<int>(
        Persistence::Registry::LoadSetting(Constants::Registry::DIR_THUMB_CACHE_MB,
            static_cast<DWORD>(Constants::IS_DIR_THUMB_CACHE_BUDGET_MB)))));
    app.preloadLookaside = max(1, min(99, static_cast<int>(
        Persistence::Registry::LoadSetting(Constants::Registry::PRELOAD_LOOKASIDE,
            static_cast<DWORD>(Constants::IS_PRELOAD_LOOKASIDE_COUNT)))));
    app.msgCenterDisplayMs = max(250, min(10000, static_cast<int>(
        Persistence::Registry::LoadSetting(Constants::Registry::MSG_CENTER_MS,
            static_cast<DWORD>(Constants::Overlay::IS_MSG_CENTER_DISPLAY_MS)))));
    app.historyMaxDirsSave = max(1, min(99999, static_cast<int>(
        Persistence::Registry::LoadSetting(Constants::Registry::HISTORY_MAX_DIRS_SAVE,
            static_cast<DWORD>(Constants::History::IS_HISTORY_MAX_DIRS_TO_SAVE)))));
    app.slideshow.intervalMs = max(100, min(60000, static_cast<int>(
        Persistence::Registry::LoadSetting(Constants::Registry::SLIDESHOW_INTERVAL_MS,
            static_cast<DWORD>(Constants::Slideshow::IS_INTERVAL_MS)))));
    app.slideshow.loop = Persistence::Registry::LoadSetting(
        Constants::Registry::SLIDESHOW_LOOP,
        static_cast<DWORD>(Constants::Slideshow::IS_LOOP)) != 0;
    app.slideshow.shuffle = Persistence::Registry::LoadSetting(
        Constants::Registry::SLIDESHOW_SHUFFLE,
        static_cast<DWORD>(Constants::Slideshow::IS_SHUFFLE)) != 0;
    {
        int t = max(0, min(5, static_cast<int>(
            Persistence::Registry::LoadSetting(Constants::Registry::SLIDESHOW_TRANSITION,
                static_cast<DWORD>(TransitionType::Cut)))));
        app.slideshow.transition.type = static_cast<TransitionType>(t);
    }
    app.fileHandlerDefaultSortOrder = max(0, min(4, static_cast<int>(
        Persistence::Registry::LoadSetting(Constants::Registry::SORT_ORDER,
            static_cast<DWORD>(Constants::FileHandler::FILE_HANDLER_DEFAULT_SORT_ORDER)))));
    app.fileHandlerIsReverseSortOrder = Persistence::Registry::LoadSetting(
        Constants::Registry::SORT_REVERSE,
        static_cast<DWORD>(Constants::FileHandler::FILE_HANDLER_SORT_TYPE_IS_REVERSE)) != 0;

    // Command-line overrides: args beat registry values.
    if (earlyArgs.runOnStartup) app.isEnableRunOnStartup = true;

    // Shell integration (open-with) only for normal instances.
    if (!app.isDedicated)
        Persistence::Registry::RegisterAppForOpenWith();

    // Startup entry: always apply so exe relocation is picked up automatically.
    // EnableRunOnStartup is mode-aware — dedicated writes to its own Run key entry.
    Persistence::Registry::EnableRunOnStartup(app.isEnableRunOnStartup);

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
            if (!firstRawArg.empty()) {
                // Signal 1: Load new image and wake up
                cds.dwData = 1;
                cds.cbData = (DWORD) ((firstRawArg.size() + 1) * sizeof(wchar_t));
                cds.lpData = (void *) firstRawArg.c_str();
            } else {
                // Signal 2: Wake up only (no file passed)
                cds.dwData = 2;
                cds.cbData = 0;
                cds.lpData = nullptr;
            }
            SendMessageW(hExistingWnd, WM_COPYDATA, 0, (LPARAM) &cds);
        }
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
        return 1;
    }
    AppCommands::changeAppThemeToDarkMode(hWnd, app.isDarkThemed);
    UI::ThemedDialog::Init(hWnd);
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
    // 1. Load your saved persistent settings FIRST
    // This establishes the user's baseline state.
    DWORD savedFactor = Persistence::Registry::LoadSetting(Constants::Registry::THEME_FACTOR, static_cast<DWORD>(Constants::Theme::DEFAULT_THEME_FACTOR));
    app.themeFactor = static_cast<float>(savedFactor) / 100.0f;
    AppCommands::changeAppThemeFactor(hWnd, app.themeFactor);
    // 2. Apply command-line arguments SECOND (already parsed above; args beat registry)
    ApplyCmdArgs(hWnd, earlyArgs, nCmdShow);

    // 3. Registry-based start-in-fullscreen (only if not already in fullscreen via cmd-line)
    if (app.startInFullscreen && !app.isFullscreen)
        AppCommands::ToggleFullscreen(hWnd);

    if (app.openDirWndOnStart)
        uiManager.getDirWindow().Show();

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
