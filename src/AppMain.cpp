// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#include <algorithm>
#include <climits>    // INT_MAX — the dedicated-interval overflow guard
#include <vector>     // WM_QIV_OPEN_FILE payload — the dropped-path list
#include <filesystem> // parent_path() of the open playlist — the exit-path folder record
#include <numeric>    // std::iota — the shuffle order. Was arriving only through
                      // an MSVC transitive include, so any stricter compiler or
                      // static-analysis pass reported it as undeclared.
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
#include "Platform/CrashHandler.h"  // installed as the first statement of wWinMain
#include "../DropTarget.h"
#include "Platform/FileHandler.h"
// GeoNames.h intentionally not included: the geocoding tables are loaded by
// ExifWnd::Show, the only screen that consumes them.
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
#include "Rem_TCP_IP/RemoteBeacon.h"     // withdraw the network announcement on exit
#include "Rem_TCP_IP/RemoteLog.h"        // the wire log's file sink — applied at startup, drained at exit
#include "Platform/AppLog.h"             // the General log — lifecycle lines live here
#include "Rem_TCP_IP/RemoteInbound.h"    // InboundGuard — the loop cut
#include "Rem_TCP_IP/RemoteExec.h"       // BuildSyncPayload for the desync repair
#include "Rem_TCP_IP/RemotesFile.h"      // qivRemoteServers.ini — the saved target list
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
#include "Persistence/SessionFile.h"   // qivSession.ini — the resume position
#include "Renderer/RendererD2D.h"
#include "Renderer/RendererGDI.h"


// Global application state

DropTarget *g_pDropTarget = nullptr;
AppState app;
// Tray icon only. The menu it shows lives in UI::AppMenu and needs nothing from
// here — the constructor used to take the UI and overlay managers because the
// settings dispatch lived in this class, and both are globals anyway.
Input::TrayHandler trayHandler;
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

// Did THIS run set the "running" mark in qivSession.ini?
//
// Only set when the General log is on, because the mark exists solely to be
// reported into that log — see the note where it is written. Remembered rather
// than re-tested at exit: the log can be switched off mid-session, and clearing
// a mark this run did not set, or leaving one it did, would each fake the
// opposite answer next launch.
static bool g_markedRunning = false;


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

        // A monitor was unplugged, or the arrangement/resolution changed. The
        // window can be left somewhere the mouse can no longer reach — the one
        // way a placement goes bad WHILE the app is running rather than between
        // launches, and the user cannot drag it back because there is nothing
        // left to grab.
        //
        // Only the unreachable case is touched. A window that is merely on a
        // different monitor than before, or partly off an edge, is left exactly
        // where it is: moving a window the user can still see and grab would be
        // the app fighting them.
        case WM_DISPLAYCHANGE: {
            // The new desktop, as Windows describes it in this message: bit
            // depth in wParam, the primary monitor's pixels in lParam. Recorded
            // whether or not the window had to be rescued, because for a screen
            // in another room "the monitor was replaced at 03:12" is the fact
            // that explains everything after it.
            if (AppLog::IsEnabled())
                AppLog::Info(AppLog::COMP_DISPLAY,
                             L"display change — " + std::to_wstring(LOWORD(lParam)) +
                             L"x" + std::to_wstring(HIWORD(lParam)) + L", " +
                             std::to_wstring(static_cast<unsigned>(wParam)) + L" bpp");

            RECT rc{};
            if (GetWindowRect(hWnd, &rc)) {
                if (!IsUsableWindowRect(rc.left, rc.top,
                                        rc.right - rc.left, rc.bottom - rc.top)) {
                    app.ResetWindowGeometry(hWnd);
                    g_overlayManager.PostCenterMessage(hWnd,
                        Constants::Messages::WINDOW_RECOVERED);

                    // A SEPARATE LINE, and a warning. The overlay says this once
                    // and vanishes; that the window had become unreachable and
                    // was moved back is the kind of thing somebody needs to find
                    // out about the following morning.
                    if (AppLog::IsEnabled())
                        AppLog::Warn(AppLog::COMP_DISPLAY,
                                     L"window was off every monitor — geometry reset");
                }
            }
            return 0;
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
            // A CROSS-PROCESS MESSAGE, and the sender is only USUALLY another
            // copy of this program. Any process on the desktop may post one to
            // this window, so cbData is the only thing that bounds the buffer.
            //
            // `std::wstring((LPCWSTR) lpData)` scans for a NUL that a sender is
            // under no obligation to have written, and it reads straight off the
            // end of the shared mapping when there is none.
            COPYDATASTRUCT *cds = (COPYDATASTRUCT *) lParam;
            if (!cds) return TRUE;

            if (cds->dwData == 1 && cds->lpData && cds->cbData >= sizeof(wchar_t)) {
                const wchar_t *raw = static_cast<const wchar_t *>(cds->lpData);
                const size_t   cap = cds->cbData / sizeof(wchar_t);
                const size_t   len = wcsnlen(raw, cap);   // bounded scan
                if (len > 0) {
                    // Post asynchronously — returning quickly unblocks the
                    // sending process. WM_QIV_OPEN_FILE owns the pointer and
                    // deletes it; if the post fails it never gets there, so it
                    // is deleted here instead.
                    //
                    // A ONE-ELEMENT vector: a forwarded launch carries a single
                    // path, and sharing the drop's payload type is what keeps
                    // both routes going through identical folder/file handling
                    // instead of drifting into two rules.
                    auto *p = new std::vector<std::wstring>{std::wstring(raw, len)};
                    if (!PostMessageW(hWnd, Constants::WM_QIV_OPEN_FILE, 0,
                                      reinterpret_cast<LPARAM>(p)))
                        delete p;
                }
                ShowWindow(hWnd, SW_RESTORE);
                SetForegroundWindow(hWnd);
            } else if (cds->dwData == 2) {
                ShowWindow(hWnd, SW_RESTORE);
                SetForegroundWindow(hWnd);
            }
            return TRUE;
        }

        case Constants::WM_QIV_OPEN_FILE: {
            // Owned here — the unique_ptr frees it on every way out of this case,
            // including the early return below. Same arrangement as
            // WM_QIV_REMOTE_PULLED further down.
            std::unique_ptr<std::vector<std::wstring>> dropped(
                reinterpret_cast<std::vector<std::wstring> *>(lParam));
            if (!dropped || dropped->empty()) return 0;

            const std::wstring &first = dropped->front();
            if (first.empty()) return 0;

            std::error_code ec;
            const bool firstIsDir = std::filesystem::is_directory(first, ec) && !ec;
            const std::wstring firstFolder = firstIsDir
                                                 ? first
                                                 : std::filesystem::path(first).parent_path().wstring();

            // EVERY DROPPED ITEM PUTS ITS FOLDER IN HISTORY — before the open, so
            // that the one actually opened is pushed last and lands at the top of
            // the MRU list where it belongs.
            //
            // Only one item can open, so without this the rest of a multi-drop
            // simply vanished and had to be dragged again. Recording their folders
            // makes them a Tab away instead, which is also what turns the
            // "N others not opened" line below into something the user can act on.
            //
            // Duplicates need no guarding here: PushFolderHistory normalises case,
            // trailing separators, quotes and whitespace, matches
            // case-insensitively, and on a hit merely promotes the existing row to
            // the front WITHOUT writing to disk. Dropping forty photos from one
            // folder therefore touches one row, once.
            //
            // Unreachable is counted in the same pass. NOT simply count-1: opening
            // an image builds the playlist from its WHOLE folder, so pictures
            // dropped together out of one folder are already reachable with an
            // arrow key and must not be reported as lost. Genuinely unreachable is
            // any second FOLDER — only one can be open — and any file from a
            // different folder than the one that opened.
            UINT unreachable = 0;
            for (size_t i = 1; i < dropped->size(); ++i) {
                const std::wstring &item = (*dropped)[i];
                if (item.empty()) continue;

                // One is_directory per item, and the folder derived from it: an
                // item IS its folder when it is one, otherwise it contributes the
                // folder it sits in.
                ec.clear();
                const bool itemIsDir = std::filesystem::is_directory(item, ec) && !ec;
                const std::wstring itemFolder =
                    itemIsDir ? item
                              : std::filesystem::path(item).parent_path().wstring();
                if (!itemFolder.empty()) UI::PushFolderHistory(itemFolder);

                if (firstIsDir || itemIsDir)
                    ++unreachable;                       // a folder never rides along
                else if (_wcsicmp(itemFolder.c_str(), firstFolder.c_str()) != 0)
                    ++unreachable;                       // a file from somewhere else
            }

            // A FOLDER OR A FILE. This message carries whichever the user handed
            // over, and both are legitimate: it is the funnel for drag-and-drop
            // AND for the single-instance handoff, where `qIV.exe <path>` reaches
            // an already-running copy through WM_COPYDATA.
            //
            // It used to go straight to OpenSpecificImage, which refuses anything
            // that is not a regular file — so dropping a FOLDER on the window did
            // nothing whatsoever and gave no hint why. Only images could be
            // dropped, which is not what a drop target that accepts the drag
            // implies.
            //
            // The rule is not new: the file dialog routes on is_directory the same
            // way (see OpenFileDialog), and so does the remote `open` verb. This is
            // the third caller of a decision both of those already make.
            //
            // An empty folder is not a failure case to guard here — OpenDirectory
            // scans it, finds nothing and raises the "No Images" placeholder,
            // which is the feedback the silent version never gave.
            const bool opened = firstIsDir ? OpenDirectory(hWnd, first)
                                           : OpenSpecificImage(hWnd, first);

            // A REFUSAL THAT RAISED NO PLACEHOLDER leaves no other trace.
            // SetFolderOverlay logs the Unsupported and Missing cases already, so
            // what is left here is the quiet one: a path that vanished between
            // the drag starting and the drop landing, where OpenDirectory /
            // OpenSpecificImage return false without putting anything on screen.
            if (!opened && AppLog::IsEnabled())
                AppLog::Warn(AppLog::COMP_DISPLAY,
                             L"dropped item could not be opened: " + first);

            // Said after the open, so the screen shows the picture and the
            // explanation together.
            //
            // ONLY WHEN SOMETHING OPENED. "Opened the first item" is false after a
            // refusal, and worse, posting it would overwrite the Unsupported
            // placeholder that explains WHY nothing opened — replacing the answer
            // with a remark about the items that were not the problem.
            if (opened && unreachable > 0)
                g_overlayManager.PostCenterMessage(
                    hWnd,
                    std::wstring(Constants::Messages::DROP_EXTRAS_PREFIX) +
                        std::to_wstring(unreachable) +
                        Constants::Messages::DROP_EXTRAS_SUFFIX);

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

                // THE SINK, payload form — the same one the socket path uses.
                //
                // This used to call Remote::ExecutePayloadCommand first and fall
                // back to the bare ExecuteCommand, which is the detour the
                // server path also took: two entry points into one dispatch,
                // each missing whatever hangs off the other. The overload does
                // both branches itself, so an observed peer's event is now
                // executed by exactly the same code as a keypress.
                //
                // No reply is asked for. An EVENT is an announcement — there is
                // nobody waiting on an answer, which is what distinguishes this
                // from the server path.
                InputManager::ExecuteCommand(hWnd, req.cmd, req.payload);
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
            Remote::Mirror::SendTo(targetId, L"Sync " + Remote::BuildSyncPayload(true));
            if (app.currentIndex >= 0)
                Remote::Mirror::SendTo(targetId,
                                       L"JumpToImage " + std::to_wstring(app.currentIndex + 1));
            return 0;
        }

        // Ctrl+Alt+Enter's answer: the picture another instance is displaying,
        // already decoded from base64 and written to a temp file by the sender
        // thread. LPARAM is that path — EMPTY when the far end is showing nothing
        // or refused, which is an answer and gets said out loud.
        case Constants::WM_QIV_REMOTE_PULLED: {
            std::unique_ptr<std::wstring> path(reinterpret_cast<std::wstring *>(lParam));
            if (!path) return 0;

            if (path->empty()) {
                g_overlayManager.PostCenterMessage(hWnd,
                                                   Constants::Messages::STREAM_IN_EMPTY);
                return 0;
            }

            // immediate: the user asked for this AT THIS KEYBOARD and is waiting to
            // look at it, so it does not queue behind a slide boundary the way an
            // arriving advert does. ownsTempFile: retiring it deletes the file.
            if (!ArmInterjection(hWnd, *path, /*immediate=*/true,
                                 /*ownsTempFile=*/true)) {
                // Not decoded yet — WM_QIV_REPAINT puts it up when it lands. Only a
                // failure to even arm it is worth a message.
                if (!app.interject.queued)
                    g_overlayManager.PostCenterMessage(
                        hWnd, Constants::Messages::STREAM_IN_FAILED);
            }
            return 0;
        }

        // The listener stopped by itself (socket died, or Stop ran). Nothing to
        // clean up here — Stop() owns the teardown; this only exists so the
        // panel can drop to "stopped" without polling.
        case Constants::WM_QIV_REMOTE_STOPPED:
            return 0;

        // The listener started, stopped, or gained/lost a client. Repaint the
        // overlay's server indicator — the only thing that shows it.
        // wParam carries WHY the list changed — see ClientEvent. The socket
        // thread is the only place that knows, so it travels with the message
        // rather than being guessed at from the count.
        case Constants::WM_QIV_REMOTE_CLIENTS:
            g_overlayManager.UpdateRemoteStatus(
                hWnd, static_cast<Constants::RemoteTcpIp::ClientEvent>(wParam));
            InvalidateRect(hWnd, nullptr, FALSE);
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
            if (wParam == Constants::CENTER_MSG_TIMER_ID) {
                g_overlayManager.OnCenterMessageTimer(hWnd);
                return 0;
            }

            // The server dot's connect / disconnect blink. It kills its own
            // timer when the count runs out — nothing ticks between blinks.
            // Rate and count are OVERLAY_SERVER_BLINK_MS / _COUNT.
            if (wParam == Constants::SERVER_BLINK_TIMER_ID) {
                g_overlayManager.OnServerBlinkTimer(hWnd);
                return 0;
            }

            if (wParam == Constants::Slideshow::TIMER_ID) {
                if (app.slideshow.running && !app.slideshow.paused && !app.playlist.empty()) {
                    // ── A one-shot interjected image ────────────────────────
                    // Another instance sent one picture to be shown once
                    // (Alt+Enter there). It occupies exactly ONE slide and leaves
                    // nothing behind: the playlist and currentIndex never moved,
                    // so clearing it and falling through advances from precisely
                    // where the sequence was.
                    //
                    // Shown at this boundary rather than the instant it arrived,
                    // so the slide already on screen is not cut short.
                    if (app.interject.showing) {
                        ClearInterjection();
                        // fall through — this tick is the normal advance
                    } else if (app.interject.queued) {
                        if (ShowInterjectedImage(hWnd)) {
                            SetTimer(hWnd, Constants::Slideshow::TIMER_ID,
                                     app.slideshow.intervalMs, nullptr);
                            return 0;   // it holds this slide — do not advance
                        }
                        // Still decoding. Leave it queued and advance normally;
                        // it goes up at the next boundary rather than stalling
                        // the slideshow for a file that is not ready.
                    }

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

                    // The playlist can be REPLACED under a running slideshow: the
                    // folder watcher reloads it, and a Ctrl+Enter push from
                    // another instance changes the folder outright. The shuffle
                    // permutation is built once, when the slideshow starts, so a
                    // stale one indexes a list that no longer exists.
                    //
                    // Rebuilt here, and started AT the picture on screen, so a
                    // pushed image is the one the slideshow carries on from —
                    // which is the whole point of pushing to a screen that is
                    // already running one.
                    if (app.slideshow.shuffle &&
                        static_cast<int>(app.slideshow.shuffleOrder.size()) != size) {
                        app.slideshow.shuffleOrder.resize(size);
                        std::iota(app.slideshow.shuffleOrder.begin(),
                                  app.slideshow.shuffleOrder.end(), 0);
                        std::shuffle(app.slideshow.shuffleOrder.begin(),
                                     app.slideshow.shuffleOrder.end(),
                                     std::mt19937{std::random_device{}()});
                        app.slideshow.shufflePos = 0;
                        for (int i = 0; i < size && app.currentIndex >= 0 &&
                                        app.currentIndex < size; ++i) {
                            if (app.slideshow.shuffleOrder[i] != app.currentIndex) continue;
                            std::swap(app.slideshow.shuffleOrder[0],
                                      app.slideshow.shuffleOrder[i]);
                            break;
                        }
                    }

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

            if (wParam == Constants::LOOKASIDE_TIMER_ID) {
                KillTimer(hWnd, Constants::LOOKASIDE_TIMER_ID);

                const int total = static_cast<int>(app.playlist.size());
                const int index = app.currentIndex;

                // BOTH ENDS. The look-ahead below tests `fwd < total` but the
                // look-behind only tested `bwd >= 0`, so an index left past the
                // end of a list that shrank — a folder rescanned while the timer
                // was armed — read out of the playlist on the backward pass.
                // Nothing to preload around an index that is not in the list,
                // and the timer is re-armed by the next image that does load.
                if (index < 0 || index >= total) return 0;

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
            // A queued interjection whose decode has just landed, on a viewer with
            // no slideshow running — so there is no slide boundary coming and
            // nothing else will show it. BEFORE the wParam==1 early return on
            // purpose: an interjection is warmed through the NEIGHBOUR preload
            // path, so its arrival IS a wParam==1 message.
            if (app.interject.queued && !app.interject.showing &&
                (app.interject.immediate ||
                 !(app.slideshow.running && !app.slideshow.paused))) {
                if (ShowInterjectedImage(hWnd)) {
                    uiManager.getCacheWindow().UpdateCacheView();
                    return 0;
                }
            }
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
            // Same for an interjection: it is not a playlist entry either, and
            // activating the current one here would rip it off the screen the
            // moment any decode landed.
            if (Dedicated::State().showingPromotion || app.interject.showing) {
                uiManager.getCacheWindow().UpdateCacheView();
                return 0;
            }

            // The background thread has finished decoding and caching the bitmap.
            // Now, on the UI thread, we probe the cache to make it the active bitmap.
            // THE INDEX IS CHECKED, not just the playlist. A non-empty playlist
            // does not imply a current index: the first scan fills the list
            // before anything is selected, and currentIndex is -1 for that
            // window — which is exactly when the first decode finishes and this
            // message arrives. app.playlist[-1] is a read before the vector.
            if (app.renderer && app.currentIndex >= 0 &&
                app.currentIndex < static_cast<int>(app.playlist.size())) {
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
                } else if (app.renderer->DecodeFailed(currentPath)) {
                    // The probe found nothing AND the decoder has already given
                    // up on this file — so this is not "still decoding", it is
                    // never going to decode. A .txt renamed to .jpg gets this
                    // far because the playlist is built from extensions, and so
                    // does a supported format whose bytes are damaged.
                    //
                    // Say which file, rather than leaving the black rectangle
                    // that used to be the whole report. The FULL PATH, exactly
                    // as the other two states report theirs — this used to
                    // compose a "dir \ file \ format" string of its own, which
                    // made this the one placeholder whose second line was not a
                    // real path and not what its (L) hint opened.
                    //
                    // This also rewrites the window title, which LoadImageIndex
                    // set back when the file was still expected to decode — so
                    // it currently names it as though it were on screen.
                    SetFolderOverlay(hWnd, AppState::FolderOverlayState::Unsupported,
                                     currentPath);
                    InvalidateRect(hWnd, nullptr, FALSE);
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
            // RECORDED BEFORE ANY TEARDOWN, so this line exists even if
            // something below it throws or hangs. Its presence in the file is
            // what makes a CLEAN exit distinguishable from a killed process —
            // a log that simply stops means the second one.
            AppLog::Info(AppLog::COMP_SHUTDOWN, L"closing normally");

            // THE MARK COMES OFF ONLY HERE, on the one path that means a clean
            // exit. Everything that skips this point — killed, power lost,
            // crashed — leaves it set, and the next launch reports it. That is
            // the entire abnormal-shutdown mechanism: a process that dies
            // suddenly cannot report itself, so what testifies is the thing it
            // failed to clean up.
            //
            // Guarded, so a run that never set one does not pay an .ini rewrite
            // to clear something that was not there.
            if (g_markedRunning) Persistence::Session::MarkRunning(false);

            Remote::Stop();
            // Drains the queue and JOINS the writer, so the rows recorded in
            // the last moments — which are the ones worth having — reach the
            // file rather than dying with the process.
            Remote::Log::ShutdownFileLogging();
            AppLog::Shutdown();
            // Same reasoning for the driving half: every sender thread must be
            // joined before the HWND it posts results to stops existing.
            Remote::Mirror::Shutdown();
            // Remote::Stop above already withdraws the announcement through
            // Refresh; this frees the instance outright. Belt to that brace, and
            // it matters more than most teardown: a service record left behind
            // makes this machine visible on the network after it has quit.
            Remote::Beacon::Shutdown();
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
    // FIRST STATEMENT IN THE PROGRAM, before the CPU check and before OLE.
    // Anything that runs ahead of this crashes invisibly, which is the state
    // qIV was in until now — see CrashHandler.h.
    Platform::Crash::Install();

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

    // COM IS NOT INITIALISED HERE. It moved below the single-instance check —
    // see the note there. Nothing between this point and that check touches COM.

    // Set DPI awareness
    typedef BOOL (WINAPI *SETDPI)(DPI_AWARENESS_CONTEXT);
    if (HMODULE hU32 = GetModuleHandleW(L"user32.dll")) {
        if (auto setDpi = reinterpret_cast<SETDPI>(GetProcAddress(hU32, "SetProcessDpiAwarenessContext"))) {
            setDpi(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        }
    }
    const unsigned int hc = std::thread::hardware_concurrency();
    app.hardwareThreads = static_cast<int>(hc > 0 ? hc : 1); // Default to 1 if OS returns 0

    // THE WORKER POOLS ARE NOT STARTED HERE. See "STARTUP ORDER" below the
    // single-instance check — nothing that spawns a thread, allocates a cache or
    // parses a data file may run before this process knows whether it is going to
    // exist at all.

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

    // =========================================================================
    // SINGLE INSTANCE — AS EARLY AS IT CAN POSSIBLY BE
    //
    // A launch that is going to hand its file to an already-running copy must do
    // NOTHING before it finds that out. It used to sit far below this point,
    // after the thread pools were started and a 100-500 ms GeoNames parse had
    // been queued — so every "open with qIV" on a running instance spun up a
    // process that allocated caches, started threads, parsed data files, and then
    // discovered it should exit. Returning from wWinMain then destroyed the
    // statics out from under its own still-running threads, which is a crash that
    // went unnoticed for as long as it existed because it happened in a process
    // nobody was looking at.
    //
    // WHY IT CANNOT MOVE ANY HIGHER:
    //   * ResolveMutexName reads the .ini's [Instance]Mutex, so the Dedicated
    //     setup above must have run.
    //   * -RestoreDefaults must be handled BEFORE this. Otherwise, with a copy
    //     already running, that flag would be forwarded as a wake-up and the
    //     settings would never be reset.
    //
    // Everything after this point — settings, pools, background threads, the
    // window — belongs only to the instance that is actually going to run.
    // =========================================================================
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
            // BOUNDED, because a plain SendMessage here waits forever.
            //
            // This is the forwarded-launch path — the comment below calls it the
            // most common way the program is started — and it blocks on ANOTHER
            // process pumping its message queue. An existing instance stuck in a
            // long synchronous operation left this one hanging with no window and
            // no error, still holding the single-instance mutex, so every further
            // double-click piled onto the same dead window.
            //
            // SendMessageTimeout, not Post: WM_COPYDATA has to be delivered
            // synchronously because cds and the buffer it points at live on this
            // stack. ABORTIFHUNG gives up immediately on a window Windows already
            // knows is not responding, rather than spending the full timeout.
            constexpr UINT HANDOFF_TIMEOUT_MS = 5000;
            DWORD_PTR sendResult = 0;
            SendMessageTimeoutW(hExistingWnd, WM_COPYDATA, 0, (LPARAM) &cds,
                                SMTO_NORMAL | SMTO_ABORTIFHUNG,
                                HANDOFF_TIMEOUT_MS, &sendResult);
        }

        // Nothing to stop and nothing to join: no pool has been started and no
        // background thread exists yet. That is the whole point of the check
        // living here rather than a hundred lines further down.
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
        return 0;
    }

    // =========================================================================
    // STARTUP ORDER — everything below runs ONLY in the surviving instance.
    // =========================================================================

    // COM, AS LATE AS IT CAN BE — and this is the same argument as the thread
    // pools below, applied to something that looks free and is not.
    //
    // A forwarded launch — "open with qIV" while a copy is already running — is
    // the most common way this program is started, and it never touches COM: it
    // parses a command line, reads an .ini, takes a mutex, finds a window and
    // sends WM_COPYDATA. Initialising an apartment for that process was work
    // done purely to be torn down again a few hundred microseconds later, on the
    // exact path a user is watching.
    //
    // VERIFIED SAFE, not assumed: nothing between the top of wWinMain and the
    // check above uses COM. Dedicated's only CoCreateInstance is in
    // CreateInstanceShortcut, which the F8 panel calls and startup does not.
    // DPI awareness stays where it was — it is not COM, and it must be set
    // before any window exists.
    if (FAILED(OleInitialize(nullptr))) return 0;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    // Thread pools. Moved down from the top of wWinMain: starting threads before
    // knowing whether this process survives is what made the crash above
    // possible, and it is wasted work on every forwarded launch besides.
    g_decoderWorker.setThreadCount(app.hardwareThreads > 3 ? Constants::VRAM_CACHE_DECODER_THREADS_COUNT : 1);
    int dirThumbThreads = (app.hardwareThreads >= 8) ? (app.hardwareThreads / 2) : (Constants::VRAM_CACHE_THUMBS_THREADS_COUNT);
    dirThumbThreads = std::min(dirThumbThreads, 8); // IShellItemImageFactory::GetImage serializes internally; >8 gives no gain
    g_dirThumbWorker.setThreadCount(std::max(1, dirThumbThreads));

#ifdef _DEBUG
    // Use the public getter instead of accessing private member m_threads
    std::wstring debugMsg = L"DecoderThreadPool: Initialized with " +
                            std::to_wstring(g_decoderWorker.getThreadCount()) +
                            L" threads.\n" +
                            L"DirThumbWorker: Initialized with " +
                            std::to_wstring(g_dirThumbWorker.getThreadCount()) +
                            L" threads.\n";
    OutputDebugStringW(debugMsg.c_str());
#endif

    // Source of truth for user preferences: the .ini for a dedicated instance,
    // the registry otherwise. LoadAllSettings routes itself.
    Persistence::Registry::LoadAllSettings(app);

    // The file sink is PERSISTED, so it has to be applied here rather than only
    // when the menu item is clicked — the whole point of persisting it is that a
    // machine which misbehaves comes back logging without anybody being present
    // to switch it on. Opens no file by itself; the first recorded exchange does.
    Remote::Log::SetFileLogging(app.remoteLogToFile);

    // The General log, and the FIRST thing it records.
    //
    // Started here, immediately after the settings that decide it — everything
    // before this point is too early to have anything worth saying, and
    // everything after it is something a dedicated screen might fail at.
    AppLog::SetEnabled(app.generalLog);

    // ONLY WHEN THE LOG IS ON — and this is a startup-cost decision, not a
    // correctness one.
    //
    // Reading and writing the marker is three .ini operations, and
    // WritePrivateProfileString rewrites the whole file for one key. Doing that
    // on every launch to record something nobody will read is a cost the DEFAULT
    // configuration would pay forever for no benefit. With the log off there is
    // nowhere to report a crash to, so there is nothing to detect.
    //
    // The mark is cleared at exit only if it was set here, so turning the log
    // off mid-session cannot leave one behind and fake a crash next launch.
    // Drop values older builds left behind. Deliberately OUTSIDE the log check
    // below: the cleanup has nothing to do with logging, and gating it there
    // would leave the stale value in place forever for the majority of users,
    // who never switch logging on.
    Persistence::Session::RemoveObsoleteValues();

    if (AppLog::IsEnabled()) {
        // READ BEFORE THE MARK IS RESET, or the evidence is destroyed by the run
        // that was supposed to report it.
        const Persistence::Session::PreviousRun previous =
            Persistence::Session::TakePreviousRun();
        Persistence::Session::MarkRunning(true);
        g_markedRunning = true;

        std::wstring line = std::wstring(Constants::APP_NAME) + L" " +
                            Constants::APP_VERSION + L" starting";
        if (app.isDedicated) line += L" (dedicated instance)";
        AppLog::Info(AppLog::COMP_STARTUP, line);

        // THE PREVIOUS RUN'S OBITUARY, written by the one process able to
        // deliver it. A dump means an exception the handler caught; no dump
        // means killed, powered off, or a failure so complete that nothing ran
        // — and that difference is most of the diagnosis.
        if (previous.crashed) {
            if (!previous.dumpPath.empty())
                AppLog::Error(AppLog::COMP_CRASH,
                              L"the previous run CRASHED — minidump: " + previous.dumpPath);
            else
                AppLog::Error(AppLog::COMP_CRASH,
                              L"the previous run ended ABNORMALLY and wrote no dump "
                              L"— killed, power lost, or stopped by the debugger");
        }
    }

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
        // In long long — intervalSeconds comes from a hand-editable INI or
        // registry value, and `* 1000` in int is signed overflow from 2147484
        // up. Too large is ignored, same as CMDArgs treats the CLI switch.
        if (runArgs.slideshowIntervalMs <= 0 && cfg.intervalSeconds > 0) {
            const long long ms = static_cast<long long>(cfg.intervalSeconds) * 1000LL;
            if (ms <= INT_MAX)
                runArgs.slideshowIntervalMs = static_cast<int>(ms);
        }

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
    if (Remote::Config().autostart) {
        std::wstring remoteErr;
        if (!Remote::Start(hWnd, remoteErr)) {
            // Non-fatal by design: a wall screen whose port is taken must still
            // come up and show pictures. The failure is reported, not thrown.
            g_overlayManager.PostCenterMessage(
                hWnd, std::wstring(Constants::Messages::REMOTE_START_FAILED_PREFIX) + remoteErr);
        }
    }

    // The DRIVING half. Reads qivRemoteServers.ini and opens a connection to every
    // row marked AutoConnect, each on its own thread — so a screen that is
    // switched off costs this startup nothing.
    //
    // Unconditional, unlike the listener above: connecting OUT opens no port and
    // accepts nothing, so there is no surface to gate. A copy with no
    // qivRemoteServers.ini simply has no targets. Note that mirroring itself is still
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
    // prompting. Its own small file (qivSession.ini) rather than the settings
    // store: this changes at every close, and an .ini write rewrites the whole
    // file, so parking it with the settings meant rewriting every setting the
    // application has for one line — see Persistence/SessionFile.h.
    //
    // Skipped for a dedicated instance: it always starts from its configured
    // folder, so a resume position would only be noise.
    if (!Dedicated::IsDedicatedFlag()) {
        if (app.currentIndex >= 0 && app.currentIndex < static_cast<int>(app.playlist.size()))
            Persistence::Session::SaveLastImage(app.playlist[app.currentIndex]);

        // The folder is recorded separately, and on its own condition. An empty
        // folder — or one whose images all failed to decode — leaves currentIndex
        // at -1, so the branch above writes nothing and the next launch would fall
        // through to the folder history, which a user who turned history off does
        // not have. The playlist's first entry is the cheapest handle on the open
        // folder; there is no current-directory member to read.
        if (!app.playlist.empty()) {
            const std::filesystem::path parent =
                    std::filesystem::path(app.playlist[0]).parent_path();
            if (!parent.empty())
                Persistence::Session::SaveLastFolder(parent.wstring());
        }

        // Window placement, when the user asked for it to be remembered.
        // GetWindowPlacement rather than GetWindowRect: the rect wanted is the
        // RESTORED one, so closing while maximised or minimised still records
        // the size the window returns to rather than a full-screen rect or the
        // (-32000, -32000) a minimised window reports.
        if (app.rememberWindowPosition) {
            WINDOWPLACEMENT wp{};
            wp.length = sizeof(wp);
            if (GetWindowPlacement(hWnd, &wp)) {
                const RECT &r = wp.rcNormalPosition;
                const int x = r.left, y = r.top;
                const int w = r.right - r.left, h = r.bottom - r.top;

                // Checked on the way OUT as well as on the way in. If something
                // has already gone wrong with the window — driven off screen,
                // sized to nothing — writing that down would hand the same
                // broken state to the next launch. Declining to save leaves the
                // last good placement in the store, which is the better answer
                // than either saving rubbish or clearing it.
                if (IsUsableWindowRect(x, y, w, h)) {
                    Persistence::Session::SaveWindowRect(x, y, w, h);

                    // Which screen that was, by device name. Saved from the
                    // window rather than from the rect so a maximised window
                    // still records the display it was maximised on.
                    MONITORINFOEXW mi{};
                    mi.cbSize = sizeof(mi);
                    HMONITOR mon = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
                    if (mon && GetMonitorInfoW(mon, &mi))
                        Persistence::Session::SaveWindowMonitor(mi.szDevice);
                }
            }
        }
    }

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
