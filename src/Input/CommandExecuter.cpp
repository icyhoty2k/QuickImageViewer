#include "Command.h"
#include "../AppState.h"
#include "../Overlays/OverlayManager.h"
#include "../Platform/Constants.h"
#include "../Platform/ConstantsStrings.h"
#include "../Platform/FileHandler.h"
#include "../Persistence/RegistryManager.h"
#include "../UI/ThumbnailPanels/CacheWnd.h"
#include "../UI/ThumbnailPanels/DirWnd.h"
#include "../UI/FloatingPanels/HistoryListWnd.h"
#include "../UI/FloatingPanels/HelpWnd.h"
#include "../UI/FloatingPanels/StatsWnd.h"
#include "../UI/ThemedDialog.h"
#include "../CMDArgs.h"
#include <algorithm>
#include <filesystem>
#include <numeric>
#include <random>
#include <cmath>
#include <commdlg.h>
#include <shlobj_core.h>
#include <shtypes.h>
#include "AppCommands.h"
#include "TrayHandler.h"   // RestoreWindow — the way back for ToggleAppVisibility
#include "UIManager.h"
#include "Rem_TCP_IP/RemoteExec.h"    // ExecutePayload — the shared payload body
#include "Rem_TCP_IP/RemoteProtocol.h" // CommandTable — QueryToggles walks it
#include "Rem_TCP_IP/RemoteMirror.h"  // the mirror gate at the top of ExecuteCommand
#include "Rem_TCP_IP/RemoteLog.h"     // Ctrl+F12 — the recording switch
#include "Rem_TCP_IP/RemoteInbound.h" // …and the loop cut that makes it safe
// The Ctrl+F11 selection panel reaches ExecuteCommand through UIManager, which
// CommandExecuter already includes — nothing extra is needed here.
#include "Rem_TCP_IP/RemoteServer.h"  // EmitToObservers — the echo half
#include "Persistence/HistoryFoldersManager.h" // QueryHistory reads the list from disk
#include "Rem_TCP_IP/RemoteSettings.h"         // QueryClients reports the configured cap

// These two functions live in AppMain.cpp.
// Declared here (not in a header) to keep them package-private.


extern AppState app;

// Snaps the window to a half of the work area on its current monitor.
// zone: 0=left  1=right  2=top  3=bottom
static void SnapWindowToZone(HWND hWnd, int zone) {
    HMONITOR hMon = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {sizeof(mi)};
    if (!GetMonitorInfo(hMon, &mi)) return;
    const RECT &wa = mi.rcWork;
    int halfW = (wa.right - wa.left) / 2;
    int halfH = (wa.bottom - wa.top) / 2;
    RECT t;
    switch (zone) {
        case 0: t = {wa.left, wa.top, wa.left + halfW, wa.bottom};
            break; // left half
        case 1: t = {wa.left + halfW, wa.top, wa.right, wa.bottom};
            break; // right half
        case 2: t = {wa.left, wa.top, wa.right, wa.top + halfH};
            break; // top half
        case 3: t = {wa.left, wa.top + halfH, wa.right, wa.bottom};
            break; // bottom half
        case 4: t = {wa.left, wa.top, wa.left + halfW, wa.top + halfH};
            break; // top-left quarter
        case 5: t = {wa.left + halfW, wa.top, wa.right, wa.top + halfH};
            break; // top-right quarter
        case 6: t = {wa.left, wa.top + halfH, wa.left + halfW, wa.bottom};
            break; // bottom-left quarter
        case 7: t = {wa.left + halfW, wa.top + halfH, wa.right, wa.bottom};
            break; // bottom-right quarter
        default: return;
    }
    SetWindowPos(hWnd, nullptr, t.left, t.top,
                 t.right - t.left, t.bottom - t.top,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    InvalidateRect(hWnd, nullptr, FALSE);
}

// -----------------------------------------------------------------------------
// Ctrl+M — move the window to the NEXT monitor, wrapping at the last.
//
// Monitors are collected with EnumDisplayMonitors and then SORTED by their
// virtual-desktop coordinates (left, then top). The enumeration order is
// whatever the display driver reports and bears no relation to the physical
// arrangement, so "next" built on it would jump around unpredictably on a
// three-screen desk. Sorting makes "next" mean "the one to the right", which is
// what the key appears to promise.
//
// Placement is PROPORTIONAL, not absolute: the window keeps its relative
// position and its relative size within the work area. Copying the pixel rect
// straight across puts a window sized for a 4K screen half off a 1080p one, and
// two monitors of different resolution is the normal case, not the exotic one.
// -----------------------------------------------------------------------------
static BOOL CALLBACK CollectMonitorProc(HMONITOR hMon, HDC, LPRECT, LPARAM lParam) {
    auto *out = reinterpret_cast<std::vector<MONITORINFO> *>(lParam);
    MONITORINFO mi = {sizeof(mi)};
    if (GetMonitorInfo(hMon, &mi)) out->push_back(mi);
    return TRUE;
}

// Returns false when there is nowhere to go — a single monitor, or the
// enumeration failed. The caller reports that rather than doing nothing.
static bool MoveWindowToNextMonitor(HWND hWnd, int &monitorNumberOut, int &monitorCountOut) {
    std::vector<MONITORINFO> mons;
    if (!EnumDisplayMonitors(nullptr, nullptr, CollectMonitorProc,
                             reinterpret_cast<LPARAM>(&mons)))
        return false;

    monitorCountOut = static_cast<int>(mons.size());
    if (mons.size() < 2) return false;

    std::sort(mons.begin(), mons.end(),
              [](const MONITORINFO &a, const MONITORINFO &b) {
                  if (a.rcMonitor.left != b.rcMonitor.left)
                      return a.rcMonitor.left < b.rcMonitor.left;
                  return a.rcMonitor.top < b.rcMonitor.top;
              });

    // Which one the window is on now. Matched by the monitor rect rather than by
    // HMONITOR, because the handles were not kept — the rects are unique and are
    // what the sort already ordered by.
    HMONITOR hCur = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO cur = {sizeof(cur)};
    if (!GetMonitorInfo(hCur, &cur)) return false;

    size_t curIdx = 0;
    for (size_t i = 0; i < mons.size(); ++i) {
        if (mons[i].rcMonitor.left == cur.rcMonitor.left &&
            mons[i].rcMonitor.top  == cur.rcMonitor.top) {
            curIdx = i;
            break;
        }
    }

    const MONITORINFO &dst = mons[(curIdx + 1) % mons.size()];
    monitorNumberOut = static_cast<int>((curIdx + 1) % mons.size()) + 1; // 1-based, for the message

    // Fullscreen is a separate case: the window IS the monitor, so it simply
    // becomes the new one. savedWindowRect is left alone — it holds the
    // pre-fullscreen geometry on the OLD screen, and rewriting it here would
    // make the eventual exit from fullscreen land somewhere the user never put
    // the window.
    if (app.isFullscreen) {
        SetWindowPos(hWnd, HWND_TOPMOST,
                     dst.rcMonitor.left, dst.rcMonitor.top,
                     dst.rcMonitor.right - dst.rcMonitor.left,
                     dst.rcMonitor.bottom - dst.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_NOCOPYBITS);
        InvalidateRect(hWnd, nullptr, FALSE);
        return true;
    }

    RECT wr;
    if (!GetWindowRect(hWnd, &wr)) return false;

    const double srcW = static_cast<double>(cur.rcWork.right - cur.rcWork.left);
    const double srcH = static_cast<double>(cur.rcWork.bottom - cur.rcWork.top);
    const double dstW = static_cast<double>(dst.rcWork.right - dst.rcWork.left);
    const double dstH = static_cast<double>(dst.rcWork.bottom - dst.rcWork.top);
    if (srcW <= 0.0 || srcH <= 0.0 || dstW <= 0.0 || dstH <= 0.0) return false;

    const double fx = (wr.left - cur.rcWork.left) / srcW;
    const double fy = (wr.top  - cur.rcWork.top)  / srcH;
    const double fw = (wr.right - wr.left) / srcW;
    const double fh = (wr.bottom - wr.top) / srcH;

    int newW = static_cast<int>(fw * dstW);
    int newH = static_cast<int>(fh * dstH);
    newW = std::max(newW, 100);
    newH = std::max(newH, 100);
    newW = std::min(newW, static_cast<int>(dstW));
    newH = std::min(newH, static_cast<int>(dstH));

    int newX = dst.rcWork.left + static_cast<int>(fx * dstW);
    int newY = dst.rcWork.top  + static_cast<int>(fy * dstH);
    // Clamp so a window that sat near the right/bottom edge of a wider screen
    // cannot end up entirely past the edge of a narrower one.
    newX = std::min(newX, static_cast<int>(dst.rcWork.right)  - newW);
    newY = std::min(newY, static_cast<int>(dst.rcWork.bottom) - newH);
    newX = std::max(newX, static_cast<int>(dst.rcWork.left));
    newY = std::max(newY, static_cast<int>(dst.rcWork.top));

    SetWindowPos(hWnd, nullptr, newX, newY, newW, newH,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    InvalidateRect(hWnd, nullptr, FALSE);

    // The window may now be under a different DPI. WM_DPICHANGED arrives on its
    // own for a cross-DPI move, so nothing is recomputed here — doing it twice
    // is what produces the half-scaled frame.
    app.isAutosized = false; // it no longer fills the work area it was fitted to
    return true;
}

// ClampViewportOffset now lives in AppState.h (next to GetRenderSize) so every
// zoom/pan call site — keyboard, mouse wheel, zoom panel — shares one copy.

// Ctrl+1..9 — advance one overlay slot through Compact → Full → Off and report
// the new state centre-screen. The message text comes from OverlayManager so
// the keyboard and the Overlays submenu word it identically.
static void CycleOverlaySlot(HWND hWnd, OverlayManager::Slot slot) {
    g_overlayManager.CycleSlotState(slot);
    g_overlayManager.PostCenterMessage(hWnd, g_overlayManager.SlotStateMessage(slot));
    InvalidateRect(hWnd, nullptr, FALSE);
}

// Compact decimal for the GetCommandValue readouts. std::to_wstring on a float
// yields six decimal places ("1.000000"), which is noise in a reply line meant
// to be read by a human at a socket.
static std::wstring Fmt1(float v) {
    wchar_t buf[32];
    swprintf_s(buf, L"%.2f", static_cast<double>(v));
    // Trim trailing zeros, then a bare trailing point: 1.00 → 1, 1.50 → 1.5
    std::wstring s(buf);
    if (s.find(L'.') != std::wstring::npos) {
        while (!s.empty() && s.back() == L'0') s.pop_back();
        if (!s.empty() && s.back() == L'.') s.pop_back();
    }
    return s;
}

static std::wstring OnOff(bool b) { return b ? L"1" : L"0"; }

// "<current>/<total> <filename>", 1-based — the same numbering the overlay and
// the JumpTo panel show, so a caller reading the screen and a caller reading
// this reply see the same figures.
//
// THE FILE NAME IS THE POINT, not decoration. A driving instance sends an INDEX
// (indices are cheap, and stay meaningful because sort order is itself a
// mirrored command, so both ends sort identically). But identical sort only
// yields identical indices when both ends also hold the same FILE SET — one
// file added or deleted on one side shifts everything after it, and from then
// on every index lands on the wrong picture, silently.
//
// So the reply names what was actually landed on. The caller compares; on a
// mismatch it pushes `sync` (folder + sort + view state) and resends the index.
// Cheap to include, and it turns a silent divergence into a self-correcting one.
static std::wstring PosOfTotal() {
    const int total = static_cast<int>(app.playlist.size());
    if (total <= 0 || app.currentIndex < 0 || app.currentIndex >= total) return L"0/0";

    const std::wstring &path = app.playlist[app.currentIndex];
    const size_t slash = path.find_last_of(L"\\/");
    const std::wstring name = (slash == std::wstring::npos) ? path : path.substr(slash + 1);

    return std::to_wstring(app.currentIndex + 1) + L"/" + std::to_wstring(total) +
           L" " + name;
}

// =============================================================================
// handleKeyboard — public entry point called from WM_KEYDOWN
// =============================================================================
void InputManager::handleKeyboard(HWND hWnd, WPARAM wParam, LPARAM lParam) {
    Command cmd = ResolveKeyboardKeys(static_cast<UINT>(wParam), lParam);
    if (cmd != Command::None) {
        ExecuteCommand(hWnd, cmd);
    }
}

// =============================================================================
// ExecuteCommand (payload form) — "do this WITH this value".
//
// Pressing J and sending `goto 42` are the same command; one carries the number
// and the other asks for it. This overload is the carrying form, and its body
// lives in Remote::ExecutePayload so that a value arriving from a panel and one
// arriving from a socket run identical code.
//
// The reply line it produces is discarded here. Callers that need it — the
// socket path — go to Remote::ExecutePayload directly rather than through the
// input pipeline.
// =============================================================================
void InputManager::ExecuteCommand(HWND hWnd, Command cmd, const std::wstring &payload) {
    std::wstring reply;
    if (Remote::ExecutePayload(hWnd, cmd, payload, reply)) return;

    // Not a payload-carrying command — the value is meaningless, so run the
    // ordinary form rather than silently doing nothing.
    ExecuteCommand(hWnd, cmd);
}

// =============================================================================
// ExecuteCommand — runs a fully-resolved Command and applies all side effects.
// Call from any input path (keyboard, mouse click, tray) to guarantee identical
// behavior regardless of how the action was triggered.
// =============================================================================
void InputManager::ExecuteCommand(HWND hWnd, Command cmd) {
    // =========================================================================
    // THE MIRROR GATE.
    //
    // Deliberately the FIRST thing in this function — ahead of the transition
    // range below, ahead of the switch, ahead of every side effect. Two reasons:
    //
    //   1. The transition block returns early. A gate placed after it would
    //      never see that whole range of commands, and the omission would be
    //      invisible.
    //   2. "Forward but do not execute here" (F11 on, F12 off — the pure
    //      remote-control mode) has to return before anything has happened
    //      locally. At the top there is nothing to undo.
    //
    // It lives INSIDE ExecuteCommand rather than in a Dispatch() wrapper around
    // it on purpose. A wrapper would leave every existing direct caller of
    // ExecuteCommand silently un-mirrored — precisely the class of bug that
    // routing the mouse and the panels through here was meant to eliminate. The
    // gate has to be somewhere nothing can go around.
    //
    // Anything not mirrorable falls straight through and runs locally, which is
    // what the deny-list means: not forwarded is not the same as not allowed.
    // =========================================================================
    // =========================================================================
    // THE SESSION FILTER — checked before anything else, including the mirror
    // gate below, so a refused command neither runs here nor travels.
    //
    // The connection IS the switch: an instance on its own behaves exactly as it
    // always did, and joining one to another is what puts both into the
    // restricted mode. One table (SESSION_BLOCKED, RemoteProtocol.cpp) decides
    // what that mode excludes — so a command that would otherwise slip through
    // some path nobody thought about is caught HERE, at the one place every
    // input path already funnels into, rather than at each of them.
    //
    // Always says what it dropped and why. A keypress that silently does nothing
    // is indistinguishable from a bug, and the user would rightly report it as
    // one.
    // =========================================================================
    {
        const wchar_t *reason = nullptr;
        if (Remote::BlockedNow(cmd, reason)) {
            std::wstring name;
            if (!Remote::NameForCommand(cmd, name))
                name = std::to_wstring(static_cast<int>(cmd));
            g_overlayManager.PostCenterMessage(
                hWnd, Constants::Messages::REMOTE_BLOCKED_PREFIX + name +
                          L" — " + reason);
            return;
        }
    }

    // HasLiveTargets() before IsMirrorable(): mirroring stays switched on while
    // the screens are off — the sender threads reconnect and it resumes by
    // itself — so the flag alone would put a command-table walk on every
    // keystroke for as long as nothing was answering.
    const bool forwarded = app.passCommandToRemote && Remote::Mirror::HasLiveTargets() &&
                           !Remote::InboundActive() && Remote::IsMirrorable(cmd);
    if (forwarded) {
        Remote::Mirror::Broadcast(cmd);
        if (!app.resendCommandToCaller) return; // drive the others, stay put here
    }

    // Held for the rest of this dispatch. If executing the command below changes
    // the picture, LoadImageIndex must NOT also forward the resulting index —
    // we already said `next`, and each target applies that to its own playlist.
    // See RemoteInbound.h.
    Remote::ForwardGuard forwardGuard(forwarded);

    // The echo half. An observed instance reports what it does to whoever asked
    // to watch — skipped for anything that arrived from the wire, or a command
    // would bounce back to the connection that sent it.
    //
    // HasObservers() first: it is one atomic load, and it is false in any viewer
    // nobody is watching. IsMirrorable is a switch and NameForCommand walks the
    // command table — neither belongs on the keystroke path of a standalone
    // viewer, and this ordering keeps them off it.
    if (Remote::HasObservers() && !Remote::InboundActive() &&
        Remote::IsMirrorable(cmd)) {
        std::wstring wireName;
        if (Remote::NameForCommand(cmd, wireName))
            Remote::EmitToObservers(wireName, Remote::CONN_NONE);
    }

    // Direct transition pick — handled ahead of the switch because it is a
    // contiguous RANGE of commands (one per TransitionType), not discrete cases.
    static_assert(static_cast<int>(Command::SetTransitionLast) -
                  static_cast<int>(Command::SetTransitionFirst) + 1 ==
                  Constants::Slideshow::TRANSITION_COUNT,
                  "SetTransition command range must cover every TransitionType");
    if (cmd >= Command::SetTransitionFirst && cmd <= Command::SetTransitionLast) {
        const int t = static_cast<int>(cmd) - static_cast<int>(Command::SetTransitionFirst);
        auto &tr = app.slideshow.transition;

        // The same menu row does two jobs, decided by the current source:
        //   LIST → tick / untick membership of the custom pool
        //   else → pick THE transition, which implies source NONE
        if (tr.source == Constants::Slideshow::TransitionSource::LIST) {
            tr.listMask ^= (1u << t);
            Persistence::Registry::SaveSetting(Constants::Registry::SLIDESHOW_TRANS_LIST,
                                               static_cast<DWORD>(tr.listMask));
            const bool on = (tr.listMask & (1u << t)) != 0u;
            g_overlayManager.PostCenterMessage(hWnd,
                std::wstring(Constants::Messages::TRANSITION_NAMES[t]) +
                (on ? Constants::Messages::STATE_ON_SUFFIX
                    : Constants::Messages::STATE_OFF_SUFFIX));
            if (tr.listMask == 0u)
                g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::TRANSITION_LIST_EMPTY);
            return;
        }

        tr.type = static_cast<TransitionType>(t);
        tr.source = Constants::Slideshow::TransitionSource::NONE;
        Persistence::Registry::SaveSetting(Constants::Registry::SLIDESHOW_TRANSITION,
                                           static_cast<DWORD>(t));
        Persistence::Registry::SaveSetting(Constants::Registry::SLIDESHOW_TRANS_SOURCE,
                                           static_cast<DWORD>(Constants::Slideshow::TransitionSource::NONE));
        g_overlayManager.PostCenterMessage(hWnd,
            std::wstring(Constants::Messages::TRANSITION_PREFIX) +
            Constants::Messages::TRANSITION_NAMES[t]);
        return;
    }

    switch (cmd) {
        // -----------------------------------------------------------------------
        // Navigation
        // -----------------------------------------------------------------------
        case Command::NextImage:
            if (!app.playlist.empty()) {
                int size = static_cast<int>(app.playlist.size());
                LoadImageIndex(hWnd, (app.currentIndex + 1) % size);
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            break;

        case Command::PrevImage:
            if (!app.playlist.empty()) {
                int size = static_cast<int>(app.playlist.size());
                LoadImageIndex(hWnd, (app.currentIndex - 1 + size) % size);
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            break;

        case Command::ToggleLastDir: {
            const auto &history = UI::GetFolderHistory();
            if (history.size() < 2) {
                g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::TOGGLE_DIR_NO_PREV);
            } else {
                std::wstring prevDir = history[1];
                std::error_code ec;
                if (!std::filesystem::is_directory(prevDir, ec) || ec) {
                    g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::TOGGLE_DIR_MISSING);
                    break;
                }
                OpenDirectory(hWnd, prevDir);
                g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::TOGGLE_DIR_CHANGED + prevDir);
            }
            break;
        }

        case Command::ToggleLastImage: {
            if (app.previousImageIndex < 0 ||
                app.previousImageIndex >= static_cast<int>(app.playlist.size())) {
                g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::TOGGLE_IMAGE_NO_PREV);
                break;
            }
            int target = app.previousImageIndex;
            const std::wstring &path = app.playlist[target];
            std::error_code ec;
            if (!std::filesystem::is_regular_file(path, ec) || ec) {
                g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::TOGGLE_IMAGE_MISSING);
                app.previousImageIndex = -1;
                break;
            }
            LoadImageIndex(hWnd, target);
            size_t slash = path.find_last_of(L"\\/");
            std::wstring name = (slash == std::wstring::npos) ? path : path.substr(slash + 1);
            g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::TOGGLE_IMAGE_CHANGED + name);
            break;
        }

        case Command::ShowInExplorer:
            if (!app.playlist.empty() && app.currentIndex >= 0) {
                const std::wstring &path = app.playlist[app.currentIndex];
                PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(path.c_str());
                if (pidl) {
                    SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
                    ILFree(pidl);
                }
            }
            break;

        // -----------------------------------------------------------------------
        // View modes
        // -----------------------------------------------------------------------
        case Command::ViewMode1:
        case Command::ViewMode2:
        case Command::ViewMode3:
        case Command::ViewMode4:
        case Command::ViewMode5: {
            int modeNum = static_cast<int>(cmd) - static_cast<int>(Command::ViewMode1) + 1;
            app.viewMode = static_cast<Constants::ViewModes::ViewMode>(modeNum);
            Persistence::Registry::SaveSetting(Constants::Registry::VIEW_MODE,
                                               static_cast<DWORD>(modeNum));
            // Reset zoom multiplier so the new mode starts from its natural
            // fit scale rather than carrying over wheel zoom from the old mode.
            app.viewport.zoom = 1.0f;
            app.viewport.offsetX = 0.0f;
            app.viewport.offsetY = 0.0f;
            ClampViewportOffset(hWnd);
            InvalidateRect(hWnd, nullptr, FALSE);
            g_overlayManager.PostCenterMessage(hWnd,
                std::wstring(Constants::Messages::VIEW_MODE_PREFIX) +
                Constants::Messages::VIEW_MODE_NAMES[modeNum - 1]);
            break;
        }

        // -----------------------------------------------------------------------
        // Zoom
        // -----------------------------------------------------------------------
        case Command::ZoomIn:
            app.viewport.zoom *= Constants::ZoomPanel::ZOOM_STEP;
            // Bounds the EFFECTIVE zoom, not the multiplier; tell the user when
            // the keypress was capped instead of leaving it looking ignored.
            AnnounceZoomClamp(hWnd, ClampZoomToLimits(hWnd));
            ClampViewportOffset(hWnd); // zoom changed the legal pan range
            InvalidateRect(hWnd, nullptr, FALSE);
            break;

        case Command::ZoomOut:
            app.viewport.zoom /= Constants::ZoomPanel::ZOOM_STEP;
            AnnounceZoomClamp(hWnd, ClampZoomToLimits(hWnd));
            ClampViewportOffset(hWnd); // zooming out shrinks it — stale offset would leave a black gap
            InvalidateRect(hWnd, nullptr, FALSE);
            break;

        case Command::ZoomReset:
            app.viewport.zoom = 1.0f;
            app.viewport.offsetX = 0.0f;
            app.viewport.offsetY = 0.0f;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;

        case Command::ZoomTo: {
            uiManager.getZoomWindow().Show();
            break;
        }

        // -----------------------------------------------------------------------
        // Transform
        // -----------------------------------------------------------------------
        case Command::RotateCW:
            app.viewport.rotation = (app.viewport.rotation + 90) % 360;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;

        case Command::RotateCCW:
            app.viewport.rotation = (app.viewport.rotation - 90 + 360) % 360;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;

        case Command::FlipH:
            app.viewport.flippedH = !app.viewport.flippedH;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;

        case Command::ToggleThumbnailWrapAround:
            app.thumbnailPanelWheelWrapAround = !app.thumbnailPanelWheelWrapAround;
            g_overlayManager.PostCenterMessage(hWnd,
                                               app.thumbnailPanelWheelWrapAround
                                                   ? Constants::Messages::THUMB_STRIP_WRAP_ON
                                                   : Constants::Messages::THUMB_STRIP_WRAP_OFF);
            break;

        case Command::ToggleViewportLock:
            app.lockViewport = !app.lockViewport;
            Persistence::Registry::SaveSetting(Constants::Registry::LOCK_VIEWPORT,
                                               static_cast<DWORD>(app.lockViewport));
            g_overlayManager.PostCenterMessage(hWnd,
                                               app.lockViewport
                                                   ? Constants::Messages::VIEWPORT_LOCK_ON
                                                   : Constants::Messages::VIEWPORT_LOCK_OFF);
            break;

        case Command::ToggleThumbnailEffects:
            app.thumbnailEffectsEnabled = !app.thumbnailEffectsEnabled;
            Persistence::Registry::SaveSetting(Constants::Registry::THUMBNAIL_EFFECTS,
                                               static_cast<DWORD>(app.thumbnailEffectsEnabled));
            g_overlayManager.PostCenterMessage(hWnd,
                                               app.thumbnailEffectsEnabled
                                                   ? Constants::Messages::THUMB_EFFECTS_ON
                                                   : Constants::Messages::THUMB_EFFECTS_OFF);
            uiManager.RepaintAllPanels();
            break;

        case Command::FlipV:
            app.viewport.flippedV = !app.viewport.flippedV;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;

        // -----------------------------------------------------------------------
        // Fullscreen
        // -----------------------------------------------------------------------
        case Command::ToggleFullscreen:
            AppCommands::ToggleFullscreen(hWnd);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;

        // -----------------------------------------------------------------------
        // Panels / overlays
        // -----------------------------------------------------------------------
        case Command::ToggleHelp:
            uiManager.Toggle(uiManager.getHelpWindow());
            break;

        case Command::ShowInfo:
            uiManager.Toggle(uiManager.getInfoWindow());
            break;

        case Command::JumpToImage:
            uiManager.ToggleJumpToWindow();
            break;

        case Command::FindImage:
            uiManager.ToggleFindWindow();
            break;

        case Command::ToggleStats:
            uiManager.Toggle(uiManager.getStatsWindow());
            break;

        case Command::OpenFile:
            OpenInitialImage(hWnd);
            break;
        case Command::ReloadCurrentDir:
            ReloadCurrentDirectory(hWnd);
            g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::RELOAD_CURRENT_DIR_MSG);
            break;


        case Command::ToggleCache: {
            UI::CacheWnd &cacheWnd = uiManager.getCacheWindow();
            const bool cacheWasVisible = cacheWnd.IsVisible();
            if (!cacheWasVisible) {
                const UI::PanelLayout &layout = uiManager.GetLayout();
                if (!layout.occupied(Constants::CACHE_WINDOW_POSITION))
                    cacheWnd.SetPosition(Constants::CACHE_WINDOW_POSITION);
            }
            uiManager.Toggle(cacheWnd);
            g_overlayManager.PostCenterMessage(hWnd, cacheWasVisible
                                                         ? Constants::Messages::CACHE_WINDOW_HIDDEN_MSG
                                                         : Constants::Messages::CACHE_WINDOW_VISIBLE_MSG);
            break;
        }

        case Command::ClearCache:
            uiManager.getCacheWindow().ClearThumbnailCache();
            g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::CACHE_WINDOW_CLEAR_CACHE_MSG);

            break;

        case Command::ToggleDir: {
            UI::DirWnd &dirWnd = uiManager.getDirWindow();
            const bool dirWasVisible = dirWnd.IsVisible();
            if (!dirWasVisible) {
                const UI::PanelLayout &layout = uiManager.GetLayout();
                if (!layout.occupied(Constants::CURRENT_DIR_WINDOW_POSITION))
                    dirWnd.SetPosition(Constants::CURRENT_DIR_WINDOW_POSITION);
            }
            uiManager.Toggle(dirWnd);
            g_overlayManager.PostCenterMessage(hWnd, dirWasVisible
                                                         ? Constants::Messages::DIR_WINDOW_HIDDEN_MSG
                                                         : Constants::Messages::DIR_WINDOW_VISIBLE_MSG);
            break;
        }

        case Command::ToggleHistory:
            uiManager.Toggle(uiManager.getHistoryListWindow());
            break;

        case Command::ToggleHistoryFull:
            UI::ToggleHistoryFull();
            break;

        // ── Cycle overlay layout mode (O) ────────────────────────────────────
        case Command::CycleOverlayLayout: {
            int &mode = app.overlayLayoutMode;
            mode = (mode + 1) % Constants::Overlay::LAYOUT_MODE_COUNT;
            g_overlayManager.OnLayoutModeChanged(hWnd);
            const wchar_t *labels[] = {Constants::Messages::LAYOUT_GRID, Constants::Messages::LAYOUT_STACKED, Constants::Messages::LAYOUT_SUMMARY};
            g_overlayManager.PostCenterMessage(hWnd, labels[mode]);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }

        // ── Toggle overlay background (P) ─────────────────────────────────────
        case Command::ToggleOverlayBackground: {
            app.overlayShowBackground = !app.overlayShowBackground;
            Persistence::Registry::SaveSetting(Constants::Registry::OVERLAY_SHOW_BG,
                                               static_cast<DWORD>(app.overlayShowBackground));
            g_overlayManager.PostCenterMessage(hWnd,
                                               app.overlayShowBackground ? Constants::Messages::OVERLAY_BG_ON : Constants::Messages::OVERLAY_BG_OFF);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }

        // ── Master overlay toggle (N / I / Ctrl+0) ───────────────────────────
        case Command::ToggleOverlay: {
            app.showOverlayInfoText = !app.showOverlayInfoText;
            Persistence::Registry::SaveSetting(Constants::Registry::OVERLAY_VISIBLE,
                                               static_cast<DWORD>(app.showOverlayInfoText));
            g_overlayManager.SetAllVisible(app.showOverlayInfoText);
            // Always post the state change to center-center — it survives the hide
            // because MID_CENTER is independently controlled by PostCenterMessage.
            g_overlayManager.PostCenterMessage(hWnd,
                                               app.showOverlayInfoText ? Constants::Messages::INFO_PANELS_ON : Constants::Messages::INFO_PANELS_OFF);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }

        // ── Per-slot state cycle (Ctrl+1..9) ─────────────────────────────────
        // One key per slot walks Compact → Full → Off, the same three states
        // the Overlays submenu offers. There is deliberately no second shortcut
        // for compact mode — this is it.
        case Command::ToggleOverlaySlot1: CycleOverlaySlot(hWnd, OverlayManager::TOP_LEFT);   break;
        case Command::ToggleOverlaySlot2: CycleOverlaySlot(hWnd, OverlayManager::TOP_CENTER); break;
        case Command::ToggleOverlaySlot3: CycleOverlaySlot(hWnd, OverlayManager::TOP_RIGHT);  break;
        case Command::ToggleOverlaySlot4: CycleOverlaySlot(hWnd, OverlayManager::MID_LEFT);   break;
        case Command::ToggleOverlaySlot5: CycleOverlaySlot(hWnd, OverlayManager::MID_CENTER); break;
        case Command::ToggleOverlaySlot6: CycleOverlaySlot(hWnd, OverlayManager::MID_RIGHT);  break;
        case Command::ToggleOverlaySlot7: CycleOverlaySlot(hWnd, OverlayManager::BOT_LEFT);   break;
        case Command::ToggleOverlaySlot8: CycleOverlaySlot(hWnd, OverlayManager::BOT_CENTER); break;
        case Command::ToggleOverlaySlot9: CycleOverlaySlot(hWnd, OverlayManager::BOT_RIGHT);  break;

        // -----------------------------------------------------------------------
        // App control
        // -----------------------------------------------------------------------
        case Command::HideToTray: {
            uiManager.HideAllPanelWindows();
            if (!app.isKeepInBackground || app.GetInstanceCount() > 1) {
                AppCommands::RemoveTrayIcon(hWnd);
                DestroyWindow(hWnd);
            } else {
                AppCommands::AddTrayIcon(hWnd);
                ShowWindow(hWnd, SW_HIDE);
            }
            break;
        }
        // Hide ⇄ show, as one command, so a caller who cannot see the screen
        // still has a way back. Deliberately NOT the HideToTray body:
        //
        //  - it always takes the keep-alive path (tray icon + SW_HIDE) even
        //    when app.isKeepInBackground is off, because the DestroyWindow path
        //    takes the TCP listener down with it, and a remote "hide" that
        //    kills the connection can never be followed by a remote "show";
        //  - the panels are not restored on the way back. HideAllPanelWindows
        //    records what it hid, but a viewer that pops open five panels on a
        //    wall screen because a phone asked for the picture back is not what
        //    the button appears to promise.
        case Command::ToggleAppVisibility:
            if (IsWindowVisible(hWnd)) {
                uiManager.HideAllPanelWindows();
                AppCommands::AddTrayIcon(hWnd);
                ShowWindow(hWnd, SW_HIDE);
            } else {
                Input::TrayHandler::RestoreWindow(hWnd);
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            break;

        case Command::MoveToNextMonitor: {
            int monNum = 0, monCount = 0;
            if (MoveWindowToNextMonitor(hWnd, monNum, monCount)) {
                g_overlayManager.PostCenterMessage(
                    hWnd, std::wstring(Constants::Messages::MONITOR_MOVED_PREFIX) +
                          std::to_wstring(monNum) + L"/" + std::to_wstring(monCount));
            } else {
                g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::MONITOR_ONLY_ONE);
            }
            break;
        }

        // A notification, not an instruction — see Command.h. It exists so that
        // an observer on ANOTHER MACHINE learns the picture changed; acting on
        // it here would mean this viewer responding to its own announcement.
        case Command::ImageChanged:
            break;

        case Command::NewWindow: {
            std::wstring exePath = Persistence::Registry::GetExePathW();
            if (!exePath.empty()) {
                SetEnvironmentVariableW(L"QIV_NEW_INSTANCE", L"1");
                ShellExecuteW(nullptr, L"open", exePath.c_str(), nullptr, nullptr, SW_SHOW);
                SetEnvironmentVariableW(L"QIV_NEW_INSTANCE", nullptr);
            }
            break;
        }

        case Command::CloseAllPanels:
            uiManager.HideAllPanelWindows();
            InvalidateRect(hWnd, nullptr, FALSE);
            break;

        case Command::RestoreAllPanels:
            uiManager.RestoreAllPanels();
            InvalidateRect(hWnd, nullptr, FALSE);
            break;

        case Command::ToggleAllPanels:
            if (uiManager.AnyPanelVisible())
                uiManager.HideAllPanelWindows();
            else
                uiManager.RestoreAllPanels();
            InvalidateRect(hWnd, nullptr, FALSE);
            break;

        case Command::HardQuit:
            AppCommands::RemoveTrayIcon(hWnd);
            DestroyWindow(hWnd);
            break;

        case Command::ResetAll:
            AppCommands::ResetWindowLayoutAndEffects(hWnd);
            g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::RESET_TO_DEFAULTS);
            break;

        // Middle-click reset. Deliberately NOT ResetAll: this restores the
        // window and viewport but leaves every image effect alone, which is what
        // the middle button has always done. Centres on the MONITOR rather than
        // the work area — also the existing behaviour, so a taskbar does not
        // shift the result.
        case Command::ResetWindowLayout: {
            app.viewport.zoom    = 1.0f;
            app.viewport.offsetX = 0.0f;
            app.viewport.offsetY = 0.0f;

            app.opacity = 255;
            SetLayeredWindowAttributes(hWnd, 0, app.opacity, LWA_ALPHA);

            const int targetW = static_cast<int>(app.baseWidth  * app.dpiScale);
            const int targetH = static_cast<int>(app.baseHeight * app.dpiScale);

            HMONITOR hMonitor = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi = {sizeof(mi)};
            if (GetMonitorInfo(hMonitor, &mi)) {
                const int monitorW = mi.rcMonitor.right - mi.rcMonitor.left;
                const int monitorH = mi.rcMonitor.bottom - mi.rcMonitor.top;
                SetWindowPos(hWnd, nullptr,
                             mi.rcMonitor.left + (monitorW - targetW) / 2,
                             mi.rcMonitor.top  + (monitorH - targetH) / 2,
                             targetW, targetH,
                             SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
            }
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }

        // Shift+Wheel. Floor is 10, not 0: an invisible window cannot be found
        // again with the mouse, and the wheel is the only way back up.
        case Command::OpacityUp:
            app.opacity = static_cast<BYTE>(
                std::min(255, static_cast<int>(app.opacity) + Constants::OPACITY_STEP));
            SetLayeredWindowAttributes(hWnd, 0, app.opacity, LWA_ALPHA);
            break;

        case Command::OpacityDown:
            app.opacity = static_cast<BYTE>(
                std::max(10, static_cast<int>(app.opacity) - Constants::OPACITY_STEP));
            SetLayeredWindowAttributes(hWnd, 0, app.opacity, LWA_ALPHA);
            break;

        // -----------------------------------------------------------------------
        // Color effect toggles
        // -----------------------------------------------------------------------
        case Command::ToggleEffectPreview:
            app.effectPreviewEnabled = !app.effectPreviewEnabled;
            app.UpdateRendererColorEffects(hWnd);
            g_overlayManager.UpdateEffects();
            break;

        // Each toggle records itself in app.activeEffectsList FIRST, so a newly
        // enabled effect lands at the end of the chain and therefore operates on
        // what is currently on screen.
        case Command::ToggleGrayscale:
            app.ToggleEffectChronological(Constants::Strings::EFFECT_GRAYSCALE);
            app.WakeUpAndApplyEffects(hWnd, app.effectGrayscale);
            break;

        case Command::ToggleInvert:
            app.ToggleEffectChronological(Constants::Strings::EFFECT_INVERT);
            app.WakeUpAndApplyEffects(hWnd, app.effectInvert);
            break;

        case Command::ToggleSepia:
            app.ToggleEffectChronological(Constants::Strings::EFFECT_SEPIA);
            app.WakeUpAndApplyEffects(hWnd, app.effectSepia);
            break;

        case Command::ToggleSolarize:
            app.ToggleEffectChronological(Constants::Strings::EFFECT_SOLARIZE);
            app.WakeUpAndApplyEffects(hWnd, app.effectSolarize);
            break;

        case Command::ToggleOutline:
            app.ToggleEffectChronological(Constants::Strings::EFFECT_OUTLINE);
            app.WakeUpAndApplyEffects(hWnd, app.effectOutline);
            break;

        case Command::ToggleThreshold:
            app.ToggleEffectChronological(Constants::Strings::EFFECT_THRESHOLD);
            app.WakeUpAndApplyEffects(hWnd, app.effectThreshold);
            break;


        // -----------------------------------------------------------------------
        // Continuous adjustments
        // -----------------------------------------------------------------------
        case Command::GammaUp:
            app.gamma = std::min(Constants::MAX_GAMMA, app.gamma + Constants::GAMMA_STEP);
            app.WakeUpAndApplyEffects(hWnd);
            break;

        case Command::GammaDown:
            app.gamma = std::max(Constants::MIN_GAMMA, app.gamma - Constants::GAMMA_STEP);
            app.WakeUpAndApplyEffects(hWnd);
            break;

        case Command::BrightnessUp:
            app.brightness = std::clamp(
                    app.brightness + Constants::COLOR_ADJUST_STEP,
                    -Constants::MIN_MAX_BRIGHTNESS, Constants::MIN_MAX_BRIGHTNESS);
            app.WakeUpAndApplyEffects(hWnd);
            break;

        case Command::BrightnessDown:
            app.brightness = std::clamp(
                    app.brightness - Constants::COLOR_ADJUST_STEP,
                    -Constants::MIN_MAX_BRIGHTNESS, Constants::MIN_MAX_BRIGHTNESS);
            app.WakeUpAndApplyEffects(hWnd);
            break;

        case Command::ContrastUp:
            app.contrast = std::clamp(
                    app.contrast + Constants::COLOR_ADJUST_STEP,
                    0.0f, Constants::MIN_MAX_CONTRAST);
            app.WakeUpAndApplyEffects(hWnd);
            break;

        case Command::ContrastDown:
            app.contrast = std::clamp(
                    app.contrast - Constants::COLOR_ADJUST_STEP,
                    0.0f, Constants::MIN_MAX_CONTRAST);
            app.WakeUpAndApplyEffects(hWnd);
            break;

        case Command::SaturationUp:
            app.saturation = std::min(
                    Constants::MIN_MAX_SATURATION, app.saturation + Constants::COLOR_ADJUST_STEP);
            app.WakeUpAndApplyEffects(hWnd);
            break;

        case Command::SaturationDown:
            app.saturation = std::max(0.0f, app.saturation - Constants::COLOR_ADJUST_STEP);
            app.WakeUpAndApplyEffects(hWnd);
            break;

        // -----------------------------------------------------------------------
        // Save / reset
        // -----------------------------------------------------------------------
        case Command::ResetEffects:
            app.ResetEffects();
            app.UpdateRendererColorEffects(hWnd);
            g_overlayManager.UpdateEffects();
            g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::ALL_EFFECTS_RESET);
            break;

        case Command::SaveImage: {
            AppCommands::SaveImageToDisk(hWnd);
            break;
        }

        case Command::CopyToClipboard:
            AppCommands::CopyImageToClipboard(hWnd);
            break;

        // ── File operations on the active thumbnail panel's selection ────────
        // Commands purely so that ONE gate governs them: the session filter at
        // the top of this function refuses the three destructive ones while a
        // connection is live, and NEVER_REMOTE keeps all four off the wire with
        // a static_assert behind it. Four scattered checks in the panel's menu
        // handler would be four places for a fifth call site to be forgotten.
        //
        // The selection comes from the panel rather than a payload: these act on
        // "what is selected right now", which is state the panel owns and no
        // caller could sensibly supply.
        case Command::FileCopySelection:
        case Command::FileMoveSelection:
        case Command::FileDeleteSelection: {
            const auto &sel = uiManager.getActiveDirWnd().m_selectedPaths;
            if (sel.empty()) break;
            const std::vector<std::wstring> paths(sel.begin(), sel.end());

            if (cmd == Command::FileDeleteSelection)
                AppCommands::DeleteFilesToRecycleBin(paths);
            else
                AppCommands::CopyFilesToClipboard(hWnd, paths,
                                                  cmd == Command::FileMoveSelection);
            break;
        }

        case Command::FilePasteIntoFolder: {
            const std::wstring dir = uiManager.getActiveDirWnd().GetPanelFolder();
            if (dir.empty()) break;
            AppCommands::PasteFilesFromClipboard(hWnd, dir);
            break;
        }

        // ── Dedicated instances ──────────────────────────────────────────────
        case Command::ToggleDedicatedPanel:
            uiManager.Toggle(uiManager.getDedicatedWindow());
            break;

        // ── Remote control over TCP/IP (src/Rem_TCP_IP) ──────────────────────
        case Command::ToggleRemotePanel:
            uiManager.Toggle(uiManager.getRemoteWindow());
            break;

        // Ctrl+F9 — who is connected to the listener above, and kick / timed
        // kick / ban. A separate panel because that one is a form and this is a
        // live list; see RemoteClientsWnd.h.
        case Command::ToggleRemoteClients:
            uiManager.Toggle(uiManager.getRemoteClientsWindow());
            break;

        // ── Mirroring (F11 / F12) ────────────────────────────────────────────
        // Both are pure state flips reported on screen. The forwarding itself
        // happens in the gate at the top of this function, not here.
        case Command::MirrorToggle:
            app.passCommandToRemote = !app.passCommandToRemote;
            if (!app.passCommandToRemote) {
                g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::MIRROR_OFF);
            } else if (!Remote::Mirror::HasLiveTargets()) {
                // Nothing is joined — either no rows at all, or rows whose
                // screens are off. The flag deliberately STAYS on: the sender
                // threads keep dialling and mirroring resumes by itself, which
                // is the behaviour a wall of screens being switched on one at a
                // time depends on. There is simply nobody to pick between yet.
                g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::MIRROR_NO_TARGETS);
            } else if (Remote::Mirror::MirroredLiveCount() == 0) {
                // Connected, but every one of them unticked in the Ctrl+F11
                // panel. Switching the flag on would forward to nobody, so say
                // what is actually wrong instead of showing "mirror ON" over a
                // viewer that drives nothing.
                app.passCommandToRemote = false;
                g_overlayManager.PostCenterMessage(
                    hWnd, Constants::Messages::MIRROR_NONE_PICKED);
            } else {
                // WHICH of them is NOT asked here. F11 is a toggle, and making
                // it also put a question up cost a keypress and a dismissal on
                // the one path that has to be instant. The selection lives in
                // the Ctrl+F11 panel (MirrorPickerWnd.h); this reports it.
                g_overlayManager.PostCenterMessage(
                    hWnd, Constants::Messages::MIRROR_ON_PREFIX +
                              Remote::Mirror::SelectionSummary());
            }
            break;

        // Ctrl+F11 — the selection panel. Opens whatever the target list looks
        // like, including empty: it explains what to do about that, which is
        // more use than an overlay that appears and is gone.
        //
        // Does NOT touch app.passCommandToRemote. F11 owns that flag; a panel
        // that also flipped it would be a second place deciding one bool, and
        // the panel is meant to be left open while F11 goes on and off.
        case Command::MirrorPick:
            uiManager.Toggle(uiManager.getMirrorPickerWindow());
            break;

        // Ctrl+F12 — the wire log. Opening it does NOT start recording: the
        // switch is a button inside the panel, so looking at what was recorded
        // and deciding to record are two separate acts.
        case Command::ToggleRemoteLog:
            uiManager.Toggle(uiManager.getRemoteLogWindow());
            break;

        // Ctrl+F10 — type a command and send it to the controlled instances.
        case Command::ToggleRemoteCmd:
            uiManager.Toggle(uiManager.getRemoteCmdWindow());
            break;

        // Reachable from a script or another instance as `enablelog 0|1`; that
        // route is handled in RemoteExec and never lands here. Locally it is the
        // panel's button, which calls this so the state change goes through the
        // one sink like everything else.
        case Command::EnableRemoteLog:
            app.remoteLogEnabled = !app.remoteLogEnabled;
            Remote::Log::SetEnabled(app.remoteLogEnabled);
            Remote::Mirror::BroadcastEnableLog(app.remoteLogEnabled);
            break;

        case Command::MirrorLocalToggle:
            app.resendCommandToCaller = !app.resendCommandToCaller;
            if (!app.passCommandToRemote) {
                // F12 alone does nothing observable. Saying so beats a keypress
                // that silently changes a flag nobody can see the effect of.
                g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::MIRROR_LOCAL_IDLE);
            } else {
                g_overlayManager.PostCenterMessage(hWnd,
                    app.resendCommandToCaller ? Constants::Messages::MIRROR_LOCAL_ON
                                              : Constants::Messages::MIRROR_LOCAL_OFF);
            }
            break;

        case Command::ToggleRemotesConsole:
            uiManager.Toggle(uiManager.getRemotesConsoleWindow());
            break;

        // Ctrl+Enter — put THIS viewer's picture on the screens under Control.
        //
        // Folder, sort order and position ONLY. Nothing else is sent, and that is
        // the requirement, not an omission: a target running a fullscreen
        // slideshow must carry on running it, from the pushed image, in its own
        // view mode with its own effects. `sync` is the opposite instrument — it
        // stamps this viewer's whole look onto the far end — so it is deliberately
        // not reused here.
        //
        // The state is snapshotted HERE because `app` belongs to this thread; the
        // sender threads negotiate entirely from this copy (RemoteMirror.h).
        // Ctrl+Shift+Enter shares every line of this: same payload, same
        // reporting, same failure modes. The ONLY difference is whether the
        // Ctrl+F11 ticks are consulted, so it is one case with one flag rather
        // than a copy that would have to be kept in step.
        case Command::SendImagePositionToAllRemotes:
        case Command::SendImagePositionToRemotes: {
            const bool everyConnected = (cmd == Command::SendImagePositionToAllRemotes);

            if (app.currentIndex < 0 ||
                app.currentIndex >= static_cast<int>(app.playlist.size())) {
                g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::PUSH_NO_IMAGE);
                break;
            }

            const std::filesystem::path cur(app.playlist[app.currentIndex]);
            Remote::Mirror::PushRequest req;
            req.imagePath = cur.wstring();
            req.folder    = cur.parent_path().wstring();
            req.fileName  = cur.filename().wstring();
            req.index     = app.currentIndex + 1;  // 1-based, as JumpToImage counts
            req.sortOrder = app.fileHandlerDefaultSortOrder;
            req.sortRev   = app.fileHandlerIsReverseSortOrder;

            int skipped = 0;
            const int n = Remote::Mirror::SendImagePosition(req, &skipped, everyConnected);

            // Says what actually happened, including the two ways it can do
            // nothing: no screen ticked in Ctrl+F11, or every ticked one on
            // another machine, where a path and an index mean nothing.
            if (n == 0) {
                g_overlayManager.PostCenterMessage(
                    hWnd, skipped > 0 ? Constants::Messages::PUSH_ONLY_REMOTE
                                      : Constants::Messages::PUSH_NO_TARGETS);
            } else {
                std::wstring msg = Constants::Messages::PUSH_SENT_PREFIX +
                                   std::to_wstring(n) +
                                   Constants::Messages::PUSH_SENT_SUFFIX;
                if (skipped > 0)
                    msg += Constants::Messages::PUSH_SKIPPED_PREFIX +
                           std::to_wstring(skipped) +
                           Constants::Messages::PUSH_SKIPPED_SUFFIX;
                g_overlayManager.PostCenterMessage(hWnd, msg);
            }
            break;
        }

        // "Sync now" — stamp this viewer's whole look onto the controlled
        // instances: folder, image, view mode, zoom, effects.
        //
        // The two spellings are built HERE because this is where RemoteExec is
        // already reachable; RemoteMirror picks which target gets which, since
        // it is the one holding the same-machine flags.
        case Command::MirrorSyncNow: {
            const std::wstring full     = L"Sync " + Remote::BuildSyncPayload(true);
            const std::wstring portable = L"Sync " + Remote::BuildSyncPayload(false);

            const int n = Remote::Mirror::SyncNow(full, portable);
            g_overlayManager.PostCenterMessage(
                hWnd, n == 0 ? std::wstring(Constants::Messages::PUSH_NO_TARGETS)
                             : Constants::Messages::SYNC_SENT_PREFIX +
                                   std::to_wstring(n) +
                                   Constants::Messages::PUSH_SENT_SUFFIX);
            break;
        }

        // Alt+Enter — STREAM this picture to those screens, shown once, changing
        // nothing else about them. The file's BYTES travel, so unlike the position
        // send above this reaches an instance on any machine.
        case Command::StreamImageToRemotes: {
            if (app.currentIndex < 0 ||
                app.currentIndex >= static_cast<int>(app.playlist.size())) {
                g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::PUSH_NO_IMAGE);
                break;
            }

            // Only the PATH is handed over; the file is read on each sender thread,
            // because a 20 MB read does not belong on the thread that paints.
            const int n =
                Remote::Mirror::StreamImageToTargets(app.playlist[app.currentIndex]);
            if (n == 0) {
                g_overlayManager.PostCenterMessage(hWnd,
                                                   Constants::Messages::PUSH_NO_TARGETS);
            } else {
                g_overlayManager.PostCenterMessage(
                    hWnd, Constants::Messages::PUSH_ONCE_PREFIX + std::to_wstring(n) +
                              Constants::Messages::PUSH_SENT_SUFFIX);
            }
            break;
        }

        // Ctrl+Alt+Enter — ask ONE instance what it is displaying and show that
        // picture here, once. This only starts it and says which screen was asked:
        // the image arrives later as WM_QIV_REMOTE_PULLED.
        case Command::StreamImageFromRemote: {
            std::wstring who;
            if (Remote::Mirror::RequestDisplayedImage(who) == 0) {
                g_overlayManager.PostCenterMessage(
                    hWnd, Constants::Messages::STREAM_IN_NO_TARGET);
                break;
            }
            // Acknowledged while waiting: the transfer takes as long as the picture
            // is large, and a keypress with nothing on screen reads as a dud.
            g_overlayManager.PostCenterMessage(
                hWnd, Constants::Messages::STREAM_IN_ASKING_PREFIX + who +
                          Constants::Messages::STREAM_IN_ASKING_SUFFIX);
            break;
        }

        // Read-only, and therefore does nothing here. The whole command IS its
        // reply — see GetCommandValue. Listed so the switch stays exhaustive
        // rather than letting it look unhandled in `default`.
        case Command::QueryState:
        case Command::QueryHistory:
        case Command::QueryClients:
        case Command::QueryToggles:
            break;

        // Payload-only, and handled entirely in RemoteExec (DoInterject) — it
        // never reaches this switch. Listed for the same reason Observe and Sync
        // are: so it does not look forgotten.
        case Command::ShowImageOnce:
            break;

        // Payload-only: they cannot arrive without a value, and the bare forms
        // would have nothing to act on. Listed so the switch is exhaustive
        // rather than letting them fall into `default` and look unhandled.
        case Command::Observe:
        case Command::Sync:
            break;

        case Command::ToggleDedicated:
            // Runtime flag only — it selects the registry/history namespace, so
            // flipping it mid-session affects subsequent reads/writes and any
            // cmdArgs generated from here. Existing instances are untouched.
            app.isDedicated = !app.isDedicated;
            g_overlayManager.PostCenterMessage(hWnd,
                std::wstring(L"Dedicated") +
                (app.isDedicated ? Constants::Messages::STATE_ON_SUFFIX
                                 : Constants::Messages::STATE_OFF_SUFFIX));
            break;

        case Command::CmdArgsExport:
            ExportCmdArgsFile(hWnd);
            break;

        case Command::CmdArgsImport:
            ImportCmdArgsFile(hWnd);
            break;

        case Command::CmdArgsGenerateShortcut:
            CreateCmdArgsShortcut(hWnd);
            break;

        case Command::CmdArgsTest:
            TestCmdArgsFile(hWnd);
            break;

        // Wallpaper styles — the enum order mirrors Constants::Wallpaper::FILL..SPAN,
        // so the offset is the position index. Same trick as ViewMode1..5 above.
        case Command::SetWallpaperFill:
        case Command::SetWallpaperFit:
        case Command::SetWallpaperStretch:
        case Command::SetWallpaperTile:
        case Command::SetWallpaperCenter:
        case Command::SetWallpaperSpan:
            AppCommands::SetDesktopWallpaper(hWnd,
                static_cast<int>(cmd) - static_cast<int>(Command::SetWallpaperFill));
            break;
        case Command::ToggleFirstLastImageInCurrentFolder: {
            if (app.playlist.empty()) return;

            int total = static_cast<int>(app.playlist.size() - 1);
            int distToStart = app.currentIndex;
            int distToEnd = total - app.currentIndex;

            // Use the further endpoint as the target
            int targetIndex = (distToStart <= distToEnd) ? total : 0;
            LoadImageIndex(hWnd, targetIndex);

            g_overlayManager.PostCenterMessage(hWnd, std::wstring(
                                                       (targetIndex == 0)
                                                           ? Constants::Messages::TOGGLE_FIRST_IMAGE_IN_FOLDER + std::to_wstring(1)
                                                           : Constants::Messages::TOGGLE_LAST_IMAGE_IN_FOLDER + std::to_wstring(targetIndex + 1)));
            if (distToStart != 0 && distToStart != total) app.lastImageBeforeToggleFirstLastImageInCurrentFolder = distToStart;
            break;
        }
        // PageUp / PageDown walk the non-favorite rows of the History panel;
        // Insert / Delete walk the starred rows. Neither reorders the list.
        case Command::PrevHistoryFolder:
            (void) UI::WalkHistoryFolder(hWnd, UI::WalkScope::NonFavoritesOnly, true);
            break;
        case Command::NextHistoryFolder:
            (void) UI::WalkHistoryFolder(hWnd, UI::WalkScope::NonFavoritesOnly, false);
            break;
        case Command::NextFavoriteFolder:
            (void) UI::WalkHistoryFolder(hWnd, UI::WalkScope::FavoritesOnly, false);
            break;
        case Command::PrevFavoriteFolder:
            (void) UI::WalkHistoryFolder(hWnd, UI::WalkScope::FavoritesOnly, true);
            break;

        // The horizontal wheel's scope: every row the panel shows, favorites
        // included. The four above split that list into halves; these two walk
        // the whole of it.
        case Command::PrevHistoryFolderAll:
            (void) UI::WalkHistoryFolder(hWnd, UI::WalkScope::All, true);
            break;
        case Command::NextHistoryFolderAll:
            (void) UI::WalkHistoryFolder(hWnd, UI::WalkScope::All, false);
            break;

        // Home / End — unconditional jump to either end of the playlist. Unlike
        // ToggleFirstLastImageInCurrentFolder (Backspace) these do not pick the
        // further endpoint and do not remember where you were.
        case Command::GoToFirstImage: {
            if (app.playlist.empty() || app.currentIndex == 0) return;
            LoadImageIndex(hWnd, 0);
            g_overlayManager.PostCenterMessage(
                    hWnd, Constants::Messages::TOGGLE_FIRST_IMAGE_IN_FOLDER + std::to_wstring(1));
            break;
        }
        case Command::GoToLastImage: {
            if (app.playlist.empty()) return;
            const int lastIndex = static_cast<int>(app.playlist.size()) - 1;
            if (app.currentIndex == lastIndex) return;
            LoadImageIndex(hWnd, lastIndex);
            g_overlayManager.PostCenterMessage(
                    hWnd, Constants::Messages::TOGGLE_LAST_IMAGE_IN_FOLDER + std::to_wstring(lastIndex + 1));
            break;
        }
        case Command::GoToLastImageInCurrentFolder: {
            if (app.playlist.empty() || app.currentIndex == app.lastImageBeforeToggleFirstLastImageInCurrentFolder) return;
            LoadImageIndex(hWnd, app.lastImageBeforeToggleFirstLastImageInCurrentFolder);
            g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::GO_TO_LAST_IMAGE_BEFORE_TOGGLE
                                                     + std::to_wstring(app.lastImageBeforeToggleFirstLastImageInCurrentFolder + 1));
            break;
        }

        // -----------------------------------------------------------------------
        // Runtime theme factor  (Ctrl+Alt+Shift+Numpad+/-/0)
        // -----------------------------------------------------------------------
        case Command::ThemeFactorUp:
            AppCommands::changeAppThemeFactor(hWnd, app.themeFactor + Constants::Theme::THEME_FACTOR_STEP);
            Persistence::Registry::SaveSetting(Constants::Registry::THEME_FACTOR,
                                               static_cast<DWORD>(std::round(app.themeFactor * 100.0f)));
            g_overlayManager.PostCenterMessage(hWnd,
                                               Constants::Messages::THEME_FACTOR_PREFIX +
                                               std::to_wstring(static_cast<int>(std::round(app.themeFactor * 100))) + L"%");
            break;

        case Command::ThemeFactorDown:
            AppCommands::changeAppThemeFactor(hWnd, app.themeFactor - Constants::Theme::THEME_FACTOR_STEP);
            Persistence::Registry::SaveSetting(Constants::Registry::THEME_FACTOR,
                                               static_cast<DWORD>(std::round(app.themeFactor * 100.0f)));
            g_overlayManager.PostCenterMessage(hWnd,
                                               Constants::Messages::THEME_FACTOR_PREFIX +
                                               std::to_wstring(static_cast<int>(std::round(app.themeFactor * 100))) + L"%");
            break;

        case Command::ThemeFactorReset:
            AppCommands::changeAppThemeFactor(hWnd, Constants::Theme::DEFAULT_THEME_FACTOR);
            Persistence::Registry::SaveSetting(Constants::Registry::THEME_FACTOR,
                                               static_cast<DWORD>(std::round(app.themeFactor * 100.0f)));
            g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::THEME_FACTOR_RESET_MSG);
            break;

        // -----------------------------------------------------------------------
        // Window chrome  (Ctrl+Shift+Numpad* / Numpad/)
        // -----------------------------------------------------------------------
        case Command::ToggleCornerPreference:
            AppCommands::changeAppCornerPreference(hWnd,
                                                   app.cornerPreference == DWMWCP_ROUND ? DWMWCP_DONOTROUND : DWMWCP_ROUND);
            g_overlayManager.PostCenterMessage(hWnd,
                                               app.cornerPreference == DWMWCP_ROUND
                                                   ? Constants::Messages::CORNER_ROUND
                                                   : Constants::Messages::CORNER_SQUARE);
            break;

        case Command::CycleBackdropType:
            AppCommands::changeAppBackdropType(hWnd, (app.backdropType + 1) % 4);
            {
                constexpr const wchar_t *labels[] = {
                    Constants::Messages::BACKDROP_NONE,
                    Constants::Messages::BACKDROP_MICA,
                    Constants::Messages::BACKDROP_ACRYLIC,
                    Constants::Messages::BACKDROP_MICA_ALT
                };
                g_overlayManager.PostCenterMessage(hWnd, labels[app.backdropType]);
            }
            break;

        case Command::ToggleAlwaysOnTop:
            app.isAlwaysOnTop = !app.isAlwaysOnTop;
            SetWindowPos(hWnd,
                         app.isAlwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST,
                         0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            uiManager.ApplyAlwaysOnTop(app.isAlwaysOnTop);
            Persistence::Registry::SaveSetting(Constants::Registry::ALWAYS_ON_TOP,
                static_cast<DWORD>(app.isAlwaysOnTop));
            g_overlayManager.PostCenterMessage(hWnd,
                                               app.isAlwaysOnTop
                                                   ? Constants::Messages::ALWAYS_ON_TOP_ON
                                                   : Constants::Messages::ALWAYS_ON_TOP_OFF);
            break;

        // -----------------------------------------------------------------------
        // Sort order  (Ctrl+Alt+Shift+0/6/7/8/9)
        // First press:  sets the sort mode ascending (reverse = false)
        // Second press: toggles to descending (reverse = true), third press back, etc.
        // -----------------------------------------------------------------------
        case Command::SortByName: {
            if (app.fileHandlerDefaultSortOrder == 0)
                app.fileHandlerIsReverseSortOrder = !app.fileHandlerIsReverseSortOrder;
            else {
                app.fileHandlerDefaultSortOrder = 0;
                app.fileHandlerIsReverseSortOrder = false;
            }
            Persistence::Registry::SaveSetting(Constants::Registry::SORT_ORDER,   static_cast<DWORD>(app.fileHandlerDefaultSortOrder));
            Persistence::Registry::SaveSetting(Constants::Registry::SORT_REVERSE, static_cast<DWORD>(app.fileHandlerIsReverseSortOrder));
            ReSortPlaylistAndRebuildMap(hWnd);
            g_overlayManager.PostCenterMessage(hWnd, app.fileHandlerIsReverseSortOrder
                                                         ? Constants::Messages::SORT_BY_NAME_REV
                                                         : Constants::Messages::SORT_BY_NAME);
            break;
        }

        case Command::SortByDate: {
            if (app.fileHandlerDefaultSortOrder == 1)
                app.fileHandlerIsReverseSortOrder = !app.fileHandlerIsReverseSortOrder;
            else {
                app.fileHandlerDefaultSortOrder = 1;
                app.fileHandlerIsReverseSortOrder = false;
            }
            Persistence::Registry::SaveSetting(Constants::Registry::SORT_ORDER,   static_cast<DWORD>(app.fileHandlerDefaultSortOrder));
            Persistence::Registry::SaveSetting(Constants::Registry::SORT_REVERSE, static_cast<DWORD>(app.fileHandlerIsReverseSortOrder));
            ReSortPlaylistAndRebuildMap(hWnd);
            g_overlayManager.PostCenterMessage(hWnd, app.fileHandlerIsReverseSortOrder
                                                         ? Constants::Messages::SORT_BY_DATE_REV
                                                         : Constants::Messages::SORT_BY_DATE);
            break;
        }

        case Command::SortBySize: {
            if (app.fileHandlerDefaultSortOrder == 2)
                app.fileHandlerIsReverseSortOrder = !app.fileHandlerIsReverseSortOrder;
            else {
                app.fileHandlerDefaultSortOrder = 2;
                app.fileHandlerIsReverseSortOrder = false;
            }
            Persistence::Registry::SaveSetting(Constants::Registry::SORT_ORDER,   static_cast<DWORD>(app.fileHandlerDefaultSortOrder));
            Persistence::Registry::SaveSetting(Constants::Registry::SORT_REVERSE, static_cast<DWORD>(app.fileHandlerIsReverseSortOrder));
            ReSortPlaylistAndRebuildMap(hWnd);
            g_overlayManager.PostCenterMessage(hWnd, app.fileHandlerIsReverseSortOrder
                                                         ? Constants::Messages::SORT_BY_SIZE_REV
                                                         : Constants::Messages::SORT_BY_SIZE);
            break;
        }

        case Command::SortByType: {
            if (app.fileHandlerDefaultSortOrder == 3)
                app.fileHandlerIsReverseSortOrder = !app.fileHandlerIsReverseSortOrder;
            else {
                app.fileHandlerDefaultSortOrder = 3;
                app.fileHandlerIsReverseSortOrder = false;
            }
            Persistence::Registry::SaveSetting(Constants::Registry::SORT_ORDER,   static_cast<DWORD>(app.fileHandlerDefaultSortOrder));
            Persistence::Registry::SaveSetting(Constants::Registry::SORT_REVERSE, static_cast<DWORD>(app.fileHandlerIsReverseSortOrder));
            ReSortPlaylistAndRebuildMap(hWnd);
            g_overlayManager.PostCenterMessage(hWnd, app.fileHandlerIsReverseSortOrder
                                                         ? Constants::Messages::SORT_BY_TYPE_REV
                                                         : Constants::Messages::SORT_BY_TYPE);
            break;
        }

        case Command::SortByDisk: {
            // Disk order has no meaningful reverse — pressing again is a no-op toggle
            app.fileHandlerDefaultSortOrder = 4;
            app.fileHandlerIsReverseSortOrder = false;
            Persistence::Registry::SaveSetting(Constants::Registry::SORT_ORDER,   static_cast<DWORD>(app.fileHandlerDefaultSortOrder));
            Persistence::Registry::SaveSetting(Constants::Registry::SORT_REVERSE, static_cast<DWORD>(app.fileHandlerIsReverseSortOrder));
            ReSortPlaylistAndRebuildMap(hWnd);
            g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::SORT_BY_DISK);
            break;
        }

        case Command::SlideshowToggle: {
            bool wasRunning = app.slideshow.running;
            AppCommands::toggleSlideshow(hWnd);
            if (!wasRunning) {
                std::wstring msg = std::wstring(Constants::Messages::SLIDESHOW_PLAYING)
                                   + L"  " + std::to_wstring(app.slideshow.intervalMs / 1000) + L"s"
                                   + (app.slideshow.loop ? L"  Loop" : L"")
                                   + (app.slideshow.shuffle ? L"  Shuffle" : L"");
                g_overlayManager.PostCenterMessage(hWnd, msg);
            } else {
                g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::SLIDESHOW_STOPPED);
            }
            break;
        }

        case Command::SlideshowPauseResume: {
            bool wasPaused = app.slideshow.paused;
            AppCommands::pauseResumeSlideshow(hWnd);
            g_overlayManager.PostCenterMessage(hWnd,
                                               wasPaused
                                                   ? Constants::Messages::SLIDESHOW_PLAYING
                                                   : Constants::Messages::SLIDESHOW_PAUSED);
            break;
        }

        case Command::SlideshowToggleLoop:
            app.slideshow.loop = !app.slideshow.loop;
            Persistence::Registry::SaveSetting(Constants::Registry::SLIDESHOW_LOOP,
                                               static_cast<DWORD>(app.slideshow.loop));
            g_overlayManager.PostCenterMessage(hWnd,
                                               app.slideshow.loop
                                                   ? Constants::Messages::SLIDESHOW_LOOP_ON
                                                   : Constants::Messages::SLIDESHOW_LOOP_OFF);
            break;

        case Command::SlideshowSetInterval: {
            int v = UI::ThemedDialog::PromptInt(hWnd, L"Slideshow Interval",
                                                L"Time between slides in ms (100 – 60000):",
                                                app.slideshow.intervalMs, 100, 60000,
                                                Constants::Slideshow::IS_INTERVAL_MS);
            if (v >= 0) {
                app.slideshow.intervalMs = v;
                Persistence::Registry::SaveSetting(Constants::Registry::SLIDESHOW_INTERVAL_MS,
                                                   static_cast<DWORD>(v));
                g_overlayManager.PostCenterMessage(hWnd,
                    std::wstring(Constants::Messages::SLIDESHOW_INTERVAL_PREFIX) +
                    std::to_wstring(v) + L" ms");
            }
            break;
        }

        case Command::SetTransitionSourceNone:
        case Command::SetTransitionSourceAll:
        case Command::SetTransitionSourceList: {
            const int src = static_cast<int>(cmd) -
                            static_cast<int>(Command::SetTransitionSourceFirst);
            app.slideshow.transition.source = src;
            app.slideshow.transition.seqIndex = 0; // restart the walk
            Persistence::Registry::SaveSetting(Constants::Registry::SLIDESHOW_TRANS_SOURCE,
                                               static_cast<DWORD>(src));
            g_overlayManager.PostCenterMessage(hWnd,
                std::wstring(Constants::Messages::TRANSITION_SOURCE_PREFIX) +
                Constants::Messages::TRANSITION_SOURCE_NAMES[src]);
            if (src == Constants::Slideshow::TransitionSource::LIST &&
                app.slideshow.transition.listMask == 0u)
                g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::TRANSITION_LIST_EMPTY);
            break;
        }

        case Command::SetTransitionOrderSequential:
        case Command::SetTransitionOrderRandom: {
            const int ord = static_cast<int>(cmd) -
                            static_cast<int>(Command::SetTransitionOrderFirst);
            app.slideshow.transition.order = ord;
            app.slideshow.transition.seqIndex = 0;
            Persistence::Registry::SaveSetting(Constants::Registry::SLIDESHOW_TRANS_ORDER,
                                               static_cast<DWORD>(ord));
            g_overlayManager.PostCenterMessage(hWnd,
                std::wstring(Constants::Messages::TRANSITION_ORDER_PREFIX) +
                Constants::Messages::TRANSITION_ORDER_NAMES[ord]);
            break;
        }


        case Command::SlideshowToggleShuffle: {
            app.slideshow.shuffle = !app.slideshow.shuffle;
            Persistence::Registry::SaveSetting(Constants::Registry::SLIDESHOW_SHUFFLE,
                                               static_cast<DWORD>(app.slideshow.shuffle));
            if (app.slideshow.shuffle && !app.playlist.empty()) {
                int n = static_cast<int>(app.playlist.size());
                app.slideshow.shuffleOrder.resize(n);
                std::iota(app.slideshow.shuffleOrder.begin(), app.slideshow.shuffleOrder.end(), 0);
                std::shuffle(app.slideshow.shuffleOrder.begin(), app.slideshow.shuffleOrder.end(),
                             std::mt19937{std::random_device{}()});
                app.slideshow.shufflePos = 0;
            } else {
                app.slideshow.shuffleOrder.clear();
            }
            g_overlayManager.PostCenterMessage(hWnd,
                                               app.slideshow.shuffle
                                                   ? Constants::Messages::SLIDESHOW_SHUFFLE_ON
                                                   : Constants::Messages::SLIDESHOW_SHUFFLE_OFF);
            break;
        }

        case Command::SlideshowCycleTransition: {
            // Every type is implemented now, so this is a plain wrap-around cycle.
            const int next = (static_cast<int>(app.slideshow.transition.type) + 1) %
                             Constants::Slideshow::TRANSITION_COUNT;
            app.slideshow.transition.type = static_cast<TransitionType>(next);
            Persistence::Registry::SaveSetting(Constants::Registry::SLIDESHOW_TRANSITION,
                                               static_cast<DWORD>(next));
            g_overlayManager.PostCenterMessage(hWnd,
                std::wstring(Constants::Messages::TRANSITION_PREFIX) +
                Constants::Messages::TRANSITION_NAMES[next]);
            break;
        }

        // -----------------------------------------------------------------------
        // Keyboard snap to screen half  (Alt+W/A/S/D)
        // -----------------------------------------------------------------------
        case Command::SnapLeft:
            SnapWindowToZone(hWnd, 0);
            g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::SNAP_LEFT);
            break;
        case Command::SnapRight:
            SnapWindowToZone(hWnd, 1);
            g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::SNAP_RIGHT);
            break;
        case Command::SnapTop:
            SnapWindowToZone(hWnd, 2);
            g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::SNAP_TOP);
            break;
        case Command::SnapBottom:
            SnapWindowToZone(hWnd, 3);
            g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::SNAP_BOTTOM);
            break;

        // -----------------------------------------------------------------------
        // Keyboard snap to screen quarter  (Alt+Q/E/Z/C)
        // -----------------------------------------------------------------------
        case Command::SnapTopLeft:
            SnapWindowToZone(hWnd, 4);
            g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::SNAP_TOP_LEFT);
            break;
        case Command::SnapTopRight:
            SnapWindowToZone(hWnd, 5);
            g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::SNAP_TOP_RIGHT);
            break;
        case Command::SnapBottomLeft:
            SnapWindowToZone(hWnd, 6);
            g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::SNAP_BOTTOM_LEFT);
            break;
        case Command::SnapBottomRight:
            SnapWindowToZone(hWnd, 7);
            g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::SNAP_BOTTOM_RIGHT);
            break;

        // -----------------------------------------------------------------------
        // Viewport pan  (W/A/S/D — DPI-scaled step)
        // Offsets match the mouse-drag convention: positive offsetX → see left side.
        // -----------------------------------------------------------------------
        case Command::PanUp: {
            float step = Constants::KEYBOARD_PAN_STEP * app.dpiScale;
            app.viewport.offsetY += step;
            ClampViewportOffset(hWnd);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }
        case Command::PanDown: {
            float step = Constants::KEYBOARD_PAN_STEP * app.dpiScale;
            app.viewport.offsetY -= step;
            ClampViewportOffset(hWnd);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }
        case Command::PanLeft: {
            float step = Constants::KEYBOARD_PAN_STEP * app.dpiScale;
            app.viewport.offsetX += step;
            ClampViewportOffset(hWnd);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }
        case Command::PanRight: {
            float step = Constants::KEYBOARD_PAN_STEP * app.dpiScale;
            app.viewport.offsetX -= step;
            ClampViewportOffset(hWnd);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }

        // -----------------------------------------------------------------------
        // Window move  (Shift+W/A/S/D — DPI-scaled step)
        // -----------------------------------------------------------------------
        case Command::MoveWindowUp: {
            int step = static_cast<int>(Constants::KEYBOARD_WINDOW_MOVE_STEP * app.dpiScale);
            RECT rc;
            GetWindowRect(hWnd, &rc);
            SetWindowPos(hWnd, nullptr, rc.left, rc.top - step, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            break;
        }
        case Command::MoveWindowDown: {
            int step = static_cast<int>(Constants::KEYBOARD_WINDOW_MOVE_STEP * app.dpiScale);
            RECT rc;
            GetWindowRect(hWnd, &rc);
            SetWindowPos(hWnd, nullptr, rc.left, rc.top + step, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            break;
        }
        case Command::MoveWindowLeft: {
            int step = static_cast<int>(Constants::KEYBOARD_WINDOW_MOVE_STEP * app.dpiScale);
            RECT rc;
            GetWindowRect(hWnd, &rc);
            SetWindowPos(hWnd, nullptr, rc.left - step, rc.top, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            break;
        }
        case Command::MoveWindowRight: {
            int step = static_cast<int>(Constants::KEYBOARD_WINDOW_MOVE_STEP * app.dpiScale);
            RECT rc;
            GetWindowRect(hWnd, &rc);
            SetWindowPos(hWnd, nullptr, rc.left + step, rc.top, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            break;
        }

        // -----------------------------------------------------------------------
        // Window resize from center  (Shift+Numpad+/- and Shift++/-)
        // -----------------------------------------------------------------------
        case Command::ResizeWindowLarger: {
            int step = static_cast<int>(Constants::KEYBOARD_WINDOW_RESIZE_STEP * app.dpiScale);
            RECT rc;
            GetWindowRect(hWnd, &rc);
            SetWindowPos(hWnd, nullptr,
                         rc.left - step, rc.top - step,
                         (rc.right - rc.left) + 2 * step, (rc.bottom - rc.top) + 2 * step,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            break;
        }
        case Command::ResizeWindowSmaller: {
            int step = static_cast<int>(Constants::KEYBOARD_WINDOW_RESIZE_STEP * app.dpiScale);
            RECT rc;
            GetWindowRect(hWnd, &rc);
            int newW = std::max(static_cast<int>(rc.right - rc.left) - 2 * step, 100);
            int newH = std::max(static_cast<int>(rc.bottom - rc.top) - 2 * step, 100);
            SetWindowPos(hWnd, nullptr,
                         rc.left + step, rc.top + step,
                         newW, newH,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            break;
        }

        // -----------------------------------------------------------------------
        // Autosize viewer to fill available screen space  (Ctrl+Space)
        // Starts from the work area (excludes taskbar), then subtracts the rect
        // of every visible thumbnail panel (F3 CacheWnd, F6 DirWnd, spawned
        // DirWnds) that is docked at a screen edge.
        // -----------------------------------------------------------------------
        case Command::AutosizeToWorkArea: {
            if (app.isAutosized) {
                HMONITOR hMon2 = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
                MONITORINFO mi2 = {sizeof(mi2)};
                if (!GetMonitorInfo(hMon2, &mi2)) break;
                int tw = (int)(app.baseWidth  * app.dpiScale);
                int th = (int)(app.baseHeight * app.dpiScale);
                int mw = mi2.rcWork.right - mi2.rcWork.left;
                int mh = mi2.rcWork.bottom - mi2.rcWork.top;
                SetWindowPos(hWnd, nullptr,
                             mi2.rcWork.left  + (mw - tw) / 2,
                             mi2.rcWork.top   + (mh - th) / 2,
                             tw, th,
                             SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
                InvalidateRect(hWnd, nullptr, FALSE);
                app.isAutosized = false;
                g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::AUTOSIZE_RESTORE);
                break;
            }

            HMONITOR hMon = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi = {sizeof(mi)};
            if (!GetMonitorInfo(hMon, &mi)) break;

            RECT r = mi.rcWork;

            const UI::PanelLayout &layout = uiManager.GetLayout();
            // positions: 1=top, 2=right, 3=bottom, 4=left
            for (int8_t pos = 1; pos <= 4; ++pos) {
                const UI::SlotInfo *slot = layout.getSlot(pos);
                if (!slot || slot->isEmpty() || !slot->panel) continue;
                HWND ph = slot->panel->GetHwnd();
                if (!ph || !IsWindowVisible(ph)) continue;
                RECT pr;
                if (!GetWindowRect(ph, &pr)) continue;
                switch (pos) {
                    case 1: r.top    = std::max(r.top,    pr.bottom); break;
                    case 2: r.right  = std::min(r.right,  pr.left);   break;
                    case 3: r.bottom = std::min(r.bottom, pr.top);    break;
                    case 4: r.left   = std::max(r.left,   pr.right);  break;
                }
            }

            if (r.right > r.left && r.bottom > r.top) {
                SetWindowPos(hWnd, nullptr, r.left, r.top,
                             r.right - r.left, r.bottom - r.top,
                             SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
                InvalidateRect(hWnd, nullptr, FALSE);
                app.isAutosized = true;
            }
            g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::AUTOSIZE_TO_WORK_AREA);
            break;
        }

        default:
            break;
    }
}

// =============================================================================
// GetCommandValue — what `cmd` controls, as it stands NOW.
//
// Feeds the "OK <name>=<value>" reply a remote caller receives, so a driving
// instance can verify that what it sent actually took effect rather than
// assuming it did. Values are read straight from `app`, which is the source of
// truth; nothing is cached or recomputed here.
//
// THIS IS A SECOND SWITCH OVER THE SAME ENUM AS ExecuteCommand ABOVE, and the
// two must be edited together. It sits directly below its partner for that
// reason. A command with a case there and none here returns "?" — deliberately
// visible the first time it is driven, rather than an empty string that reads
// like a working command with nothing to report.
//
// Commands whose state is not a value report what a caller would actually want
// to check instead: `next` answers "14/238", not "next=ok".
// =============================================================================
std::wstring InputManager::GetCommandValue(HWND hWnd, Command cmd) {
    // Whole contiguous ranges answer with the same reading.
    if (cmd >= Command::SetTransitionFirst && cmd <= Command::SetTransitionLast)
        return std::to_wstring(static_cast<int>(app.slideshow.transition.type));
    if (cmd >= Command::ViewMode1 && cmd <= Command::ViewMode5)
        return std::to_wstring(static_cast<int>(app.viewMode));
    if (cmd >= Command::SetWallpaperFill && cmd <= Command::SetWallpaperSpan)
        return L"set";

    switch (cmd) {
        // --- Anything that moves the playlist position -----------------------
        case Command::NextImage:
        case Command::PrevImage:
        case Command::GoToFirstImage:
        case Command::GoToLastImage:
        case Command::GoToLastImageInCurrentFolder:
        case Command::ToggleFirstLastImageInCurrentFolder:
        case Command::ToggleLastImage:
        case Command::JumpToImage:
        case Command::FindImage:
        case Command::OpenFile:
        case Command::ReloadCurrentDir:
        case Command::PrevHistoryFolder:
        case Command::NextHistoryFolder:
        case Command::PrevFavoriteFolder:
        case Command::NextFavoriteFolder:
        case Command::PrevHistoryFolderAll:
        case Command::NextHistoryFolderAll:
        case Command::ToggleLastDir:
        case Command::SortByName:
        case Command::SortByDate:
        case Command::SortBySize:
        case Command::SortByType:
        case Command::SortByDisk:
            return PosOfTotal();

        // --- Zoom / viewport --------------------------------------------------
        case Command::ZoomIn:
        case Command::ZoomOut:
        case Command::ZoomReset:
        case Command::ZoomTo:
            return Converters::FormatZoomPercent(app.GetRealZoom(hWnd));
        case Command::PanLeft:
        case Command::PanRight:
        case Command::PanUp:
        case Command::PanDown:
            return Fmt1(app.viewport.offsetX) + L"," + Fmt1(app.viewport.offsetY);
        case Command::ToggleViewportLock: return OnOff(app.lockViewport);

        // --- Transform --------------------------------------------------------
        case Command::RotateCW:
        case Command::RotateCCW: return std::to_wstring(app.viewport.rotation);
        case Command::FlipH:     return OnOff(app.viewport.flippedH);
        case Command::FlipV:     return OnOff(app.viewport.flippedV);

        // --- Colour effects ---------------------------------------------------
        case Command::ToggleGrayscale:      return OnOff(app.effectGrayscale);
        case Command::ToggleInvert:         return OnOff(app.effectInvert);
        case Command::ToggleSepia:          return OnOff(app.effectSepia);
        case Command::ToggleSolarize:       return OnOff(app.effectSolarize);
        case Command::ToggleOutline:        return OnOff(app.effectOutline);
        case Command::ToggleThreshold:      return OnOff(app.effectThreshold);
        case Command::ToggleEffectPreview:  return OnOff(app.effectPreviewEnabled);
        case Command::GammaUp:
        case Command::GammaDown:            return Fmt1(app.gamma);
        case Command::BrightnessUp:
        case Command::BrightnessDown:       return Fmt1(app.brightness);
        case Command::ContrastUp:
        case Command::ContrastDown:         return Fmt1(app.contrast);
        case Command::SaturationUp:
        case Command::SaturationDown:       return Fmt1(app.saturation);
        // The effect CHAIN, in application order — the one piece of effect state
        // the booleans above cannot express, and the reason two instances can
        // report identical flags while showing visibly different images.
        case Command::ResetEffects: {
            std::wstring out;
            for (const std::wstring &e : app.activeEffectsList) {
                if (!out.empty()) out += L',';
                out += e;
            }
            return out.empty() ? L"none" : out;
        }

        // --- Window / chrome --------------------------------------------------
        case Command::ToggleFullscreen:        return OnOff(app.isFullscreen);
        case Command::ToggleAlwaysOnTop:       return OnOff(app.isAlwaysOnTop);
        case Command::AutosizeToWorkArea:      return OnOff(app.isAutosized);
        // 1 = the window is on screen. Named after what it reports, not after
        // the command: a caller asking "is it showing?" wants that, not an echo
        // of which way the toggle just went.
        case Command::ToggleAppVisibility:     return OnOff(IsWindowVisible(hWnd) != FALSE);
        // Which monitor it landed on, 1-based, in the same left-to-right order
        // the move itself uses — so a caller can tell a wrap from a step.
        case Command::MoveToNextMonitor: {
            std::vector<MONITORINFO> mons;
            EnumDisplayMonitors(nullptr, nullptr, CollectMonitorProc,
                                reinterpret_cast<LPARAM>(&mons));
            if (mons.empty()) return L"1/1";
            std::sort(mons.begin(), mons.end(),
                      [](const MONITORINFO &a, const MONITORINFO &b) {
                          if (a.rcMonitor.left != b.rcMonitor.left)
                              return a.rcMonitor.left < b.rcMonitor.left;
                          return a.rcMonitor.top < b.rcMonitor.top;
                      });
            MONITORINFO cur = {sizeof(cur)};
            if (!GetMonitorInfo(MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST), &cur))
                return L"1/" + std::to_wstring(mons.size());
            for (size_t i = 0; i < mons.size(); ++i) {
                if (mons[i].rcMonitor.left == cur.rcMonitor.left &&
                    mons[i].rcMonitor.top  == cur.rcMonitor.top)
                    return std::to_wstring(i + 1) + L"/" + std::to_wstring(mons.size());
            }
            return L"1/" + std::to_wstring(mons.size());
        }
        case Command::OpacityUp:
        case Command::OpacityDown:             return std::to_wstring(app.opacity);
        case Command::CycleBackdropType:       return std::to_wstring(app.backdropType);
        case Command::ToggleCornerPreference:  return std::to_wstring(app.cornerPreference);
        case Command::ThemeFactorUp:
        case Command::ThemeFactorDown:
        case Command::ThemeFactorReset:        return Fmt1(app.themeFactor);

        // --- Overlays ---------------------------------------------------------
        case Command::ToggleOverlay:            return OnOff(app.showOverlayInfoText);
        case Command::ToggleOverlayBackground:  return OnOff(app.overlayShowBackground);
        case Command::CycleOverlayLayout:       return std::to_wstring(app.overlayLayoutMode);
        case Command::ToggleThumbnailWrapAround:return OnOff(app.thumbnailPanelWheelWrapAround);
        case Command::ToggleThumbnailEffects:   return OnOff(app.thumbnailEffectsEnabled);

        // --- Slideshow --------------------------------------------------------
        case Command::SlideshowToggle:
        case Command::SlideshowPauseResume:
            return app.slideshow.running
                       ? (app.slideshow.paused ? L"paused" : L"running")
                       : L"stopped";
        case Command::SlideshowToggleLoop:      return OnOff(app.slideshow.loop);
        case Command::SlideshowToggleShuffle:   return OnOff(app.slideshow.shuffle);
        case Command::SlideshowSetInterval:     return std::to_wstring(app.slideshow.intervalMs);
        case Command::SlideshowCycleTransition:
            return std::to_wstring(static_cast<int>(app.slideshow.transition.type));

        // --- Panels -----------------------------------------------------------
        case Command::ToggleHelp:    return OnOff(uiManager.getHelpWindow().IsVisible());
        case Command::ToggleCache:   return OnOff(uiManager.getCacheWindow().IsVisible());
        case Command::ToggleDir:     return OnOff(uiManager.getDirWindow().IsVisible());
        case Command::ToggleHistory: return OnOff(uiManager.getHistoryListWindow().IsVisible());
        case Command::ShowInfo:      return OnOff(uiManager.getInfoWindow().IsVisible());
        case Command::ToggleStats:   return OnOff(uiManager.getStatsWindow().IsVisible());
        case Command::ToggleAllPanels:
        case Command::CloseAllPanels:
        case Command::RestoreAllPanels: return OnOff(uiManager.AnyPanelVisible());

        // --- Mirroring --------------------------------------------------------
        case Command::MirrorToggle:      return OnOff(app.passCommandToRemote);
        // A panel now, not a flag — report whether it is open, like the others.
        case Command::MirrorPick:
            return OnOff(uiManager.getMirrorPickerWindow().IsVisible());
        case Command::ToggleRemoteLog:
            return OnOff(uiManager.getRemoteLogWindow().IsVisible());
        case Command::ToggleRemoteCmd:
            return OnOff(uiManager.getRemoteCmdWindow().IsVisible());
        // The RECORDING state, not the panel's. This is the value a driving
        // instance reads back to confirm `enablelog 1` actually took.
        case Command::EnableRemoteLog: return OnOff(app.remoteLogEnabled);
        case Command::MirrorLocalToggle: return OnOff(app.resendCommandToCaller);

        // The read-only one. Everything a driving instance needs in order to
        // decide whether an index is safe to send it: which folder this viewer is
        // in, in what order, how long the list is, and where it is standing.
        //
        // KEY ORDER MATTERS. The two free-text values go LAST, because a ';' in a
        // file name (legal on Windows, if unusual) truncates its own value and
        // whatever follows. With them at the end, the worst case is a folder that
        // compares unequal — one extra rescan, still the right picture — instead
        // of a mangled number.
        // Every row's value in one reply, for a client that has just connected
        // and knows nothing about this viewer.
        //
        // WALKS THE TABLE rather than naming the commands, so a toggle added
        // later is included the day it is added. The alternative — a list here —
        // is a second place to remember, and forgetting it fails silently: the
        // new button would simply never initialise.
        //
        // Three kinds of row are left out, each for a reason that would otherwise
        // corrupt the reply rather than merely pad it:
        //
        //   - the Query* commands themselves. This one would recurse; the others
        //     answer with `k=v;k=v` bodies of their own, and nesting one inside
        //     this one's `;`-separated list makes both unparseable.
        //   - "?" — a command with no GetCommandValue case. Nothing to report,
        //     and the marker is for a developer reading a single reply, not for
        //     a client to store.
        //   - any value containing ';' or '=' — a file name may legally hold
        //     either, and one such name would silently truncate the rest of the
        //     list. Dropped rather than escaped: no client needs a file name
        //     from here (QueryState reports it, in a field of its own), so an
        //     escaping scheme would be complexity bought for nothing.
        case Command::QueryToggles: {
            size_t rowCount = 0;
            const Remote::CommandEntry *rows = Remote::CommandTable(rowCount);

            std::wstring out;
            for (size_t i = 0; i < rowCount; ++i) {
                const Command rc = rows[i].cmd;
                if (rc == Command::QueryToggles || rc == Command::QueryState ||
                    rc == Command::QueryHistory  || rc == Command::QueryClients)
                    continue;

                // Safe to call for every row: GetCommandValue only READS `app`
                // and panel visibility — it is the reporting half, and nothing
                // in it has a side effect.
                const std::wstring v = GetCommandValue(hWnd, rc);
                if (v == L"?" || v.empty()) continue;
                if (v.find(L';') != std::wstring::npos ||
                    v.find(L'=') != std::wstring::npos) continue;

                if (!out.empty()) out += L';';
                out += rows[i].name;
                out += L'=';
                out += v;
            }
            return out;
        }

        case Command::QueryState: {
            const int total = static_cast<int>(app.playlist.size());
            const bool have = app.currentIndex >= 0 && app.currentIndex < total;

            // The folder is reported even with nothing displayed — a viewer part
            // way through its first scan holds a playlist before it holds a
            // current index, and answering "no folder" there would make a pusher
            // reopen the folder it is already in.
            std::wstring folder, name;
            if (have) {
                const std::filesystem::path cur(app.playlist[app.currentIndex]);
                name   = cur.filename().wstring();
                folder = cur.parent_path().wstring();
            } else if (total > 0) {
                folder = std::filesystem::path(app.playlist[0]).parent_path().wstring();
            }

            std::wstring s = L"count=" + std::to_wstring(total);
            s += L";index=" + std::to_wstring(have ? app.currentIndex + 1 : 0);
            s += L";sort="  + std::to_wstring(app.fileHandlerDefaultSortOrder);
            s += L";sortrev=";
            s += app.fileHandlerIsReverseSortOrder ? L"1" : L"0";
            s += L";name="   + name;
            s += L";folder=" + folder;
            return s;
        }

        // Read straight from disk rather than from the History panel's list.
        // That list is a file-static inside HistoryListWnd and only exists once
        // the panel has been opened, so asking it would answer "no history" on
        // an instance nobody has pressed F3 on. The file is the source of truth
        // and AppMenuIO already loads it this way.
        case Command::QueryHistory: {
            HistoryFoldersManager hfm;
            hfm.LoadHistoryFromDisk();

            // Pipe-separated: a Windows path cannot contain '|', but it very
            // often contains spaces and commas. Favourites carry a leading '*'
            // so a client can group them without a second round trip.
            std::wstring list;
            for (const std::wstring &folder : hfm.folderHistory) {
                if (HistoryPath::IsBroken(folder)) continue; // never offer a path that cannot open
                if (!list.empty()) list += L'|';
                if (hfm.favorites.count(folder)) list += L'*';
                list += folder;
            }
            return L"count=" + std::to_wstring(hfm.folderHistory.size()) +
                   L";folders=" + list;
        }

        // The count INCLUDES the caller — it is asking over one of the very
        // connections being counted. Saying so here rather than subtracting one:
        // a client that wants "others" can do that arithmetic, and a client
        // showing a status line wants the number the server's own F9 panel shows.
        case Command::QueryClients:
            return L"clients="  + std::to_wstring(Remote::ActiveConnections()) +
                   L";max="     + std::to_wstring(Remote::Config().maxConnections) +
                   L";endpoint=" + Remote::BoundEndpoint();

        // Whether the picture a driving instance interjected is up yet: "shown",
        // "queued" for the next slide boundary, or "none" once it has been retired.
        case Command::ShowImageOnce:
            return app.interject.showing ? L"shown"
                 : app.interject.queued  ? L"queued"
                                         : L"none";

        // --- Actions with no lasting state; report that they ran --------------
        case Command::ClearCache:
        case Command::SaveImage:
        case Command::CopyToClipboard:
        case Command::ShowInExplorer:
        case Command::ResetAll:
        case Command::ResetWindowLayout:
        // Local only — they have no table row, so no caller can ask. A case here
        // costs nothing and stops the next reader wondering.
        case Command::SendImagePositionToRemotes:
        case Command::SendImagePositionToAllRemotes:
        case Command::MirrorSyncNow:
        case Command::StreamImageToRemotes:
        case Command::StreamImageFromRemote:
        case Command::StreamImageBegin:
        case Command::StreamImageChunk:
        case Command::StreamImageShow:
        case Command::SendDisplayedImage:
        case Command::SendDisplayedPreview:
        // Listed for completeness only. These are in NEVER_REMOTE, so the wire
        // path refuses them before a value is ever asked for — but a case here
        // costs nothing and stops the next reader wondering whether the omission
        // was deliberate.
        case Command::FileCopySelection:
        case Command::FileMoveSelection:
        case Command::FileDeleteSelection:
        case Command::FilePasteIntoFolder:
            return L"done";

        default:
            // Visible on first use rather than silently blank — see the header
            // comment. Add the case above when you add the command.
            return L"?";
    }
}
