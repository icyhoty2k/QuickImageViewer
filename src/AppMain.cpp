#include <algorithm>
#include <random>

using std::min;
using std::max;

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
#include "UI/FloatingPanels/HistoryListWnd.h"
#include "Platform/WriteQueue.h"
#include <thread>

#include <miniz.h>
#include "MouseHandler.h"
#include "Input/Command.h"
#include "Input/TrayHandler.h"
#include "Dedicated/DedicatedInstance.h" // AppIconId — dedicated icon everywhere
#include "Dedicated/DedicatedSettings.h" // DetectStartupMode — ini vs registry
#include "Dedicated/DedicatedLists.h"    // image / promotion folder lists
#include "Rem_TCP_IP/RemoteSettings.h"   // Remote::Config — is the listener enabled?
#include "Rem_TCP_IP/RemoteServer.h"     // WM_QIV_REMOTE_COMMAND execution + shutdown
#include "Rem_TCP_IP/RemoteMirror.h"     // the driving half — targets + sender threads
#include "Rem_TCP_IP/RemoteInbound.h"    // InboundGuard — the loop cut
#include "Rem_TCP_IP/RemoteExec.h"       // BuildSyncPayload for the desync repair
#include "Rem_TCP_IP/RemotesFile.h"      // qivRemotes.ini — the saved target list
#include "Rem_TCP_IP/RemotesWnd.h"       // F10 console + startup AutoConnectAll
#include <windows.h>
#ifndef MF_RADIOCHECK
#define MF_RADIOCHECK 0x00000200L
#endif
#include <windowsx.h>

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
Input::TrayHandler trayHandler(uiManager, g_overlayManager);
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
        // Every hide-to-tray and restore passes through here, so one hook keeps
        // the display-awake request in step with whether anything is on screen.
        // DefWindowProc runs FIRST: WM_SHOWWINDOW arrives while the window is
        // still in its old state, so asking IsWindowVisible before it would read
        // the state we are leaving rather than the one we are entering.
        case WM_SHOWWINDOW: {
            const LRESULT r = DefWindowProcW(hWnd, message, wParam, lParam);
            AppCommands::ApplyDisplayAwake(hWnd);
            return r;
        }

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
        // A remote client thread parsed a command and is blocked waiting for the
        // answer. Execute it HERE, on the UI thread, because ExecuteCommand
        // touches app state, GDI and the swapchain — none of which a socket
        // thread may go near.
        //
        // LPARAM is a heap std::shared_ptr<Remote::RemoteCall> owned by this
        // handler. The client thread holds its own shared_ptr, so signalling and
        // deleting here is safe even if it already timed out and walked away.
        case Constants::WM_QIV_REMOTE_COMMAND: {
            auto *held = reinterpret_cast<std::shared_ptr<Remote::RemoteCall> *>(lParam);
            if (!held) return 0;
            if (std::shared_ptr<Remote::RemoteCall> call = *held) {
                call->result = Remote::ExecuteOnUiThread(hWnd, call->req, call->conn);
                if (call->doneEvent) SetEvent(call->doneEvent);
            }
            delete held;
            return 0;
        }

        // An OBSERVED instance told us what it just did. We are watching it, so
        // we do the same thing and end up showing the same picture.
        //
        // Executed under the inbound guard, which is what stops this from
        // looping: without it, a master mirroring to a slave that is also
        // observing the master would bounce one keystroke between the two
        // forever, at socket speed.
        case Constants::WM_QIV_REMOTE_EVENT: {
            auto *line = reinterpret_cast<std::wstring *>(lParam);
            if (!line) return 0;

            // "EVENT <command> [payload]" — drop the prefix, then parse exactly
            // as the listener parses an incoming request, so an observer accepts
            // precisely what a driven instance accepts and no more.
            std::wstring body = *line;
            delete line;
            if (_wcsnicmp(body.c_str(), Constants::RemoteTcpIp::RESP_EVENT, 5) == 0)
                body = body.substr(5);

            const Remote::RemoteRequest req = Remote::ParseLine(body);
            if (req.status == Remote::ParseStatus::Ok) {
                Remote::InboundGuard guard(Remote::CONN_NONE);
                std::wstring unused;
                if (!Remote::ExecutePayloadCommand(hWnd, req, unused))
                    InputManager::ExecuteCommand(hWnd, req.cmd);
            }
            return 0;
        }

        // A driven target replied naming a DIFFERENT file than the one we landed
        // on: same sort order, different file set, so the index we sent meant
        // another picture there. Push our whole view state (which carries the
        // folder and the sort order), then resend the position.
        //
        // Handled here because only the UI thread may read app.playlist — the
        // sender thread that noticed the mismatch cannot build the repair itself.
        case Constants::WM_QIV_REMOTE_DESYNC: {
            // Only same-machine targets are ever sent a position, so only they
            // can report one back that disagrees — which is why the folder is
            // included here without asking.
            const int targetId = static_cast<int>(wParam);
            Remote::Mirror::SendTo(targetId, L"sync " + Remote::BuildSyncPayload(true));
            if (app.currentIndex >= 0)
                Remote::Mirror::SendTo(targetId,
                                       L"goto " + std::to_wstring(app.currentIndex + 1));
            return 0;
        }

        // The listener stopped by itself (socket died, or Stop ran). Nothing to
        // clean up here — Stop() owns the teardown; this only exists so the
        // panel can drop to "stopped" without polling.
        case Constants::WM_QIV_REMOTE_STOPPED:
            return 0;

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
                    // ── Dedicated promotions ────────────────────────────────
                    // The ONLY seam the promotions system has in established
                    // code. A promotion replaces one slide: it is drawn from a
                    // separate playlist straight through the renderer's cache,
                    // so app.playlist and app.currentIndex never move and the
                    // image sequence resumes exactly where it left off.
                    {
                        auto &ded = Dedicated::State();
                        if (ded.showingPromotion) {
                            // Promotion's dwell has elapsed. Restore the normal
                            // slide interval and fall through to the advance —
                            // the promotion occupied this slot, so the images
                            // resume exactly where they were.
                            ded.showingPromotion = false;
                            SetTimer(hWnd, Constants::Slideshow::TIMER_ID,
                                     app.slideshow.intervalMs, nullptr);
                        } else if (ded.active && ded.promotions.ShouldShowNow()) {
                            if (Dedicated::ShowNextPromotion(hWnd)) {
                                // A promotion holds the screen for its OWN time,
                                // not the slide interval — it is a message, not
                                // a picture. 0 means "same as a slide".
                                const int secs = ded.config.promoShowSeconds;
                                const int ms = (secs > 0) ? secs * 1000
                                                          : app.slideshow.intervalMs;
                                SetTimer(hWnd, Constants::Slideshow::TIMER_ID, ms, nullptr);
                                return 0; // promotion shown — do not advance
                            }
                        }
                    }

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
                // Never hide the pointer while a context menu is up — the user
                // needs it to pick an item.
                if (app.slideshow.running && !app.slideshow.paused &&
                    !app.slideshow.cursorHidden && !app.isContextMenuOpen) {
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
        if (wParam == FALSE) break;
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
            MouseHandler::HandleButtonUp(hWnd, message, wParam, lParam);
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
            // A promotion is on screen and is NOT a playlist entry. Activating
            // the current playlist image here would rip it away the moment any
            // decode lands — which is exactly what made promotions flash and
            // vanish. Leave the screen alone; the slideshow timer restores the
            // images when the promotion's time is up.
            if (Dedicated::State().showingPromotion) {
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
                    // Dimensions are now the new image's — re-clamp a locked
                    // viewport (Y) so a carried zoom/pan stays inside its limits.
                    ReclampLockedViewport(hWnd);
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

            // Discard if user navigated away while bytes were in flight. Guard by
            // path identity (survives the post-open folder re-sort that renumbers
            // indices) — same fix as the raster path. On a match, hand the bytes to
            // the decoder worker; WM_QIV_REPAINT finalizes display when it lands.
            if (app.renderer &&
                std::hash<std::wstring>{}(payload->path) ==
                    app.wantedPathHash.load(std::memory_order_acquire)) {
                (void) app.renderer->PreloadSvgFromBytes(std::move(payload->bytes),
                                                         payload->path, arrivedIndex);
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
        case WM_TRAYICON:
            return trayHandler.Handle(hWnd, wParam, lParam);


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
            if (app.isContextMenuOpen) {
                SetCursor(LoadCursor(nullptr, IDC_ARROW));
                return TRUE;
            }
            if (g_scanInProgress.load(std::memory_order_relaxed)) {
                SetCursor(Constants::Cursors::CURR_APPSTARTING);
                return TRUE;
            }
            if (app.viewport.isDragging) {
                SetCursor(Constants::Cursors::CURR_GRAB);
                return TRUE;
            }
            if (MouseHandler::UpdateHoverCursor(hWnd))
                return TRUE;
            break;

        case WM_DESTROY:
            // Bring the listener down before the window goes, so no client
            // thread can post WM_QIV_REMOTE_COMMAND to an HWND that is on its
            // way out — that message would never be handled and the poster
            // would block for the full reply timeout.
            Remote::Stop();
            // Same reasoning for the driving half: every sender thread must be
            // joined before the HWND it posts results to stops existing.
            Remote::Mirror::Shutdown();
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

    // Decide where settings live BEFORE the first read. Driven by the
    // filesystem, not by a flag:
    //   an .ini beside the exe        → File      (registry never touched)
    //   exe NAMED *dedicated*, no ini → NeedsSetup(registry never touched;
    //                                              the F8 panel opens to create it)
    //   otherwise                     → Registry  (unchanged behaviour)
    // Deriving the .ini from the exe's own path is what makes collisions
    // impossible — one folder cannot hold two exes with the same name.
    // -config points at an explicit .ini. Must land before the first resolve.
    if (!earlyArgs.configPath.empty())
        Dedicated::SetSettingsFileOverride(earlyArgs.configPath);

    const Dedicated::StartupMode startupMode = Dedicated::DetectStartupMode();
    if (Dedicated::IsDedicatedFlag()) {
        app.isDedicated = true;
        // Resolve (and create, if missing) the two folder lists this instance
        // runs from, recording their names in the .ini. Replaces history and
        // favorites, which a dedicated instance does not keep at all.
        Dedicated::EnsureListFiles();
    }

    // -RestoreDefaults: delete all persisted settings, confirm, exit.
    // Run before any reads so a corrupted value can never block this path.
    if (earlyArgs.restoreDefaults) {
        // A file-backed instance owns an .ini, not a registry tree — wiping the
        // shared tree from here would destroy the MAIN app's settings.
        if (startupMode == Dedicated::StartupMode::Registry)
            RegDeleteTreeW(HKEY_CURRENT_USER, Constants::Registry::ROOT_KEY);
        else
            DeleteFileW(Dedicated::SettingsFilePath().c_str());
        MessageBoxW(nullptr,
            L"All settings have been restored to defaults.\n\nThe application will now exit.",
            L"qIV — Restore Defaults", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    // Source of truth for user preferences: the .ini for a dedicated instance,
    // the registry otherwise. LoadAllSettings routes itself.
    Persistence::Registry::LoadAllSettings(app);

    // Command-line overrides: args beat registry values.
    if (earlyArgs.runOnStartup) app.isEnableRunOnStartup = true;

    // Capture by value — background threads must not read app fields that could change.
    const bool bgDedicated          = app.isDedicated;
    const bool bgEnableRunOnStartup = app.isEnableRunOnStartup;

    // --- Parallel startup work — runs while window creation + renderer init proceed ---

    // Registry maintenance: open-with registration + startup entry (every launch, ~2-4 ms).
    std::thread registryThread([bgDedicated, bgEnableRunOnStartup]() {
        if (!bgDedicated)
            Persistence::Registry::RegisterAppForOpenWith();
        Persistence::Registry::EnableRunOnStartup(bgEnableRunOnStartup);
    });

    // WIC imaging factory: loads windowscodecs.dll cold (5-15 ms), warm ~0 ms.
    // IWICImagingFactory2 implements the free-threaded marshaler — safe to create
    // in an MTA thread and use on the main STA thread after join().
    std::atomic<bool> wicOk{false};
    std::thread wicThread([&wicOk]() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        wicOk = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                           CLSCTX_INPROC_SERVER,
                                           IID_PPV_ARGS(&app.wicFactory)));
        CoUninitialize();
    });

    // History + favorites from disk: cold HDD 1-10 ms, warm NVMe ~0 ms.
    std::thread historyThread([]() {
        UI::LoadFolderHistoryFromDisk();
    });

    // --- SINGLE INSTANCE & RAM RESIDENT LOGIC ---
    bool bypassMutex = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    if (GetEnvironmentVariableW(L"QIV_NEW_INSTANCE", nullptr, 0) > 0) bypassMutex = true;

    // Identity comes from the .ini's [Instance]Mutex when set, otherwise from
    // the exe's file name — so renaming a copy gives it its own slot and any
    // number of instances can run side by side.
    std::wstring mutexName = Dedicated::ResolveMutexName();
    mutexName += (bypassMutex ? std::to_wstring(GetTickCount()) : L"");
    HANDLE hMutex = CreateMutexW(NULL, TRUE, mutexName.c_str());

    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // Search OUR OWN class, so a relaunch wakes the copy it belongs to and
        // can never hand the file to a different instance.
        HWND hExistingWnd = FindWindowW(Dedicated::ResolveWindowClassName().c_str(), nullptr);
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
        registryThread.detach();
        wicThread.detach();
        historyThread.detach();
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
        return 0;
    }

    // --- Window Creation ---
    // A dedicated copy registers its OWN class. Cached in AppState because
    // GetInstanceCount() counts windows of this class — scoping it per instance
    // is what stops one copy from counting another as a duplicate of itself.
    app.windowClassName = Dedicated::ResolveWindowClassName();

    WNDCLASSW wc{0};
    wc.lpfnWndProc = MainAppWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = app.windowClassName.c_str();
    wc.style = CS_DBLCLKS;
    wc.hCursor = Constants::Cursors::CURR_DEFAULT;
    // One source of truth for the icon — a dedicated instance must look distinct
    // everywhere (window, taskbar, alt-tab, tray, panels). The small/taskbar
    // icons are set separately by AddTrayIcon via WM_SETICON.
    wc.hIcon = LoadIcon(hInstance, MAKEINTRESOURCEW(Dedicated::AppIconId()));
    RegisterClassW(&wc);

    HWND hWnd = CreateViewerWindow(hInstance, wc.lpszClassName);
    if (!hWnd) {
        return 1;
    }
    AppCommands::changeAppThemeToDarkMode(hWnd, app.isDarkThemed);
    UI::ThemedDialog::Init(hWnd);
    // Init UI Manager (The New Controller)
    uiManager.Init(hInstance, hWnd);

    // Validate the folder history now, in the background, while the user is
    // looking at images. Pressing Tab later then finds the list already scanned
    // instead of opening a panel that only starts walking folders at that moment.
    UI::StartBackgroundHistoryScan();

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
    AppCommands::changeAppThemeFactor(hWnd, app.themeFactor);

    // Join background threads — all must finish before ApplyCmdArgs decodes the first image.
    registryThread.join();
    wicThread.join();
    historyThread.join();
    if (!wicOk) return 0; // WIC unavailable — cannot decode images

    // 2. Apply command-line arguments SECOND (already parsed above; args beat registry)
    //
    // A dedicated instance folds its .ini config into the SAME CmdArgs struct
    // rather than applying anything itself — one apply path, so a configured
    // instance and a command line behave identically. An explicit switch always
    // wins: the .ini only fills what the command line left unset.
    CmdArgs runArgs = earlyArgs;
    if (Dedicated::IsDedicatedFlag()) {
        Dedicated::LoadConfig(Dedicated::State().config);
        const Dedicated::InstanceConfig &cfg = Dedicated::State().config;

        // Content folder: an explicit -startFolder, else the first entry of
        // imageLists_*.qim that still exists. Something must resolve here or the
        // instance has nothing to show — and it must never fall through to the
        // file chooser.
        if (runArgs.startFolder.empty()) {
            for (const std::wstring &folder : Dedicated::LoadImageFolders()) {
                // First entry that actually exists — a stale line in the list
                // should not blank the screen.
                const DWORD attr = GetFileAttributesW(folder.c_str());
                if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
                    runArgs.startFolder = folder;
                    break;
                }
            }
        }
        if (runArgs.monitorNum < 1 && cfg.monitorNum >= 1)
            runArgs.monitorNum = cfg.monitorNum;
        if (runArgs.slideshowIntervalMs <= 0 && cfg.intervalSeconds > 0)
            runArgs.slideshowIntervalMs = cfg.intervalSeconds * 1000;

        runArgs.fullscreen = runArgs.fullscreen || cfg.fullscreen;
        runArgs.slideshow  = runArgs.slideshow  || cfg.slideshow;
        runArgs.repeat     = runArgs.repeat     || cfg.loop;
        runArgs.hideMouse  = runArgs.hideMouse  || cfg.hideMouse;

        // Build the second playlist before the slideshow can tick.
        Dedicated::InitPromotions();
    }
    ApplyCmdArgs(hWnd, runArgs, nCmdShow);

    // Remote control. ApplyCmdArgs has just merged the .ini with the -remote*
    // switches, so this is the first moment the configuration is complete.
    // Starting is conditional on Enable, which defaults false and can only be
    // set by an .ini section or an explicit switch — a viewer nobody configured
    // for remote control never opens a socket.
    if (Remote::Config().enable) {
        std::wstring remoteErr;
        if (!Remote::Start(hWnd, remoteErr)) {
            // Non-fatal by design: a wall screen whose port is taken must still
            // come up and show pictures. The failure is reported, not thrown.
            g_overlayManager.PostCenterMessage(
                hWnd, std::wstring(Constants::Messages::REMOTE_START_FAILED_PREFIX) + remoteErr);
        }
    }

    // The DRIVING half. Reads qivRemotes.ini and opens a connection to every
    // row marked AutoConnect, each on its own thread — so a screen that is
    // switched off costs this startup nothing.
    //
    // Unconditional, unlike the listener above: connecting OUT opens no port and
    // accepts nothing, so there is no surface to gate. A copy with no
    // qivRemotes.ini simply has no targets. Note that mirroring itself is still
    // off (F11 and F12 always start false) — the connections exist, but nothing
    // travels down them until asked.
    UI::RemotesWnd::AutoConnectAll(hWnd);

    // One promotion kept warm, so the first one due appears without a stall.
    if (Dedicated::IsDedicatedFlag())
        Dedicated::PreloadUpcomingPromotion(hWnd);

    // 3. Registry-based start-in-fullscreen (only if not already in fullscreen via cmd-line)
    if (app.startInFullscreen && !app.isFullscreen)
        AppCommands::ToggleFullscreen(hWnd);

    if (app.openDirWndOnStart)
        uiManager.getDirWindow().Show();

    // A copy named *dedicated* that has no .ini yet is prepared but unconfigured.
    // Open the setup panel so the user creates one, rather than leaving a screen
    // silently running on defaults with nowhere to save its settings.
    if (startupMode == Dedicated::StartupMode::NeedsSetup)
        uiManager.getDedicatedWindow().Show();

    MSG msg{};
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // --- CRITICAL CLEANUP ---
    // Remember the image on screen so the next launch resumes here instead of
    // prompting. Written BEFORE the flush below so it makes it to disk.
    // Skipped for a dedicated instance: it always starts from its configured
    // folder, so a resume position would only be noise in its .ini.
    if (!Dedicated::IsDedicatedFlag() && app.currentIndex >= 0 &&
        app.currentIndex < static_cast<int>(app.playlist.size()))
        Persistence::Registry::SaveStringSetting(Constants::Registry::LAST_IMAGE,
                                                 app.playlist[app.currentIndex]);

    g_writeQueue.Flush(); // drain all pending registry + file writes before teardown
    app.renderer.reset();

    if (app.wicFactory) {
        app.wicFactory.Reset();
    }

    if (g_pDropTarget) {
        RevokeDragDrop(hWnd);
        g_pDropTarget->Release();
        g_pDropTarget = nullptr;
    }

    UnregisterClassW(app.windowClassName.c_str(), hInstance);

    CoUninitialize();
    OleUninitialize();

    if (hMutex) {
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
    }

    return static_cast<int>(msg.wParam);
}
