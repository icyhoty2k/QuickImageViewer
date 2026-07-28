#include "MirrorPicker.h"
#include "RemoteMirror.h"
#include "RemoteExec.h"   // BuildSyncPayload — the Sync now item

#include "AppState.h"
#include "Platform/Constants.h"

#include <algorithm>
#include <string>
#include <vector>

extern AppState app;

namespace Remote::Mirror {

namespace {

    // Menu ids. Local to this popup — it is tracked with TPM_RETURNCMD, so the
    // chosen id comes straight back here and never reaches the main WndProc,
    // which is why these do not have to be unique against AppMenuIds.h.
    constexpr UINT ID_DONE  = 1;
    constexpr UINT ID_ALL   = 2;
    constexpr UINT ID_NONE  = 3;
    constexpr UINT ID_SYNC  = 4;
    constexpr UINT ID_FIRST = 100; // + index into the connected-target list

    // Push this viewer's folder, picture and view state to the TICKED targets.
    //
    // The same operation as the console's Sync all, aimed at the selection
    // instead of the whole list — mirroring forwards toggles, and a toggle
    // applied to a different starting state diverges, so lining the screens up
    // before driving them is the natural first move once you have just said
    // which screens those are.
    //
    // Two spellings, chosen per target: the full one carries the folder and the
    // image path, and goes only to instances that share this filesystem. The
    // portable one names the file without a path, because a drive letter from
    // here means nothing there.
    //
    // SendTo, not BroadcastSync: this is aimed, and the broadcast form
    // deliberately ignores the mirror selection (it backs the console button,
    // whose label promises all of them).
    int SyncSelected(const std::vector<TargetView> &live) {
        const std::wstring full     = L"sync " + Remote::BuildSyncPayload(true);
        const std::wstring portable = L"sync " + Remote::BuildSyncPayload(false);

        int n = 0;
        for (const TargetView &v : live) {
            if (!v.mirroring) continue;
            SendTo(v.id, v.sameMachine ? full : portable);
            ++n;
        }
        return n;
    }

    // "Monitor2 — 127.0.0.1:8771". The address is there because two rows are
    // routinely named after the screen they drive rather than the machine, and
    // "Left" and "Right" alone do not say which port went where when one of them
    // is misbehaving.
    std::wstring RowLabel(const TargetView &v) {
        std::wstring s = v.name.empty() ? v.host : v.name;
        s += L"\t";                       // right-aligned column, as menus do
        s += v.host + L":" + std::to_wstring(v.port);
        return s;
    }

    // Screen point to open at: the middle of the viewer.
    //
    // NOT the cursor. Mirroring is driven from the keyboard — the hand that
    // pressed F11 is not on the mouse, and the pointer is commonly parked at an
    // edge or hidden by a running slideshow, which would put the popup somewhere
    // the eye is not. The middle of the window it is asking about is where the
    // user is already looking.
    POINT CenterOf(HWND hOwner) {
        RECT rc{};
        GetWindowRect(hOwner, &rc);
        return POINT{(rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2};
    }

    // One round trip. Takes ownership of hMenu, returns the chosen id (0 when
    // dismissed). Same SetForegroundWindow + WM_NULL dance as AppMenu::TrackMenuAt
    // — the documented workaround for a popup that otherwise needs two clicks to
    // dismiss.
    UINT TrackAt(HMENU hMenu, HWND hOwner, POINT pt) {
        if (!hMenu) return 0;
        SetForegroundWindow(hOwner);
        const UINT cmd = TrackPopupMenu(hMenu,
                                        TPM_RETURNCMD | TPM_NONOTIFY |
                                            TPM_LEFTBUTTON | TPM_CENTERALIGN,
                                        pt.x, pt.y, 0, hOwner, nullptr);
        PostMessageW(hOwner, WM_NULL, 0, 0);
        DestroyMenu(hMenu);
        return cmd;
    }

    // Built fresh on every re-show, from the live target list rather than from a
    // remembered one: a screen can connect or drop while the popup is up, and a
    // menu painted from a stale snapshot would offer a row that no longer exists
    // — or hide one that just appeared.
    HMENU Build(const std::vector<TargetView> &live) {
        HMENU m = CreatePopupMenu();
        if (!m) return nullptr;

        AppendMenuW(m, MF_STRING | MF_DISABLED, 0, L"Mirror F11 keystrokes to…");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);

        for (size_t i = 0; i < live.size(); ++i)
            AppendMenuW(m, MF_STRING | (live[i].mirroring ? MF_CHECKED : MF_UNCHECKED),
                        ID_FIRST + static_cast<UINT>(i), RowLabel(live[i]).c_str());

        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, ID_ALL,  L"All");
        AppendMenuW(m, MF_STRING, ID_NONE, L"None");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);

        // Greyed rather than hidden when nothing is ticked: a row that comes and
        // goes makes the item below it move under the cursor between two clicks.
        const bool any = std::any_of(live.begin(), live.end(),
                                     [](const TargetView &v) { return v.mirroring; });
        AppendMenuW(m, MF_STRING | (any ? MF_ENABLED : MF_GRAYED), ID_SYNC,
                    L"Sync now\tfolder · image · view");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, ID_DONE, L"Done");
        return m;
    }

    // Connected only. A listed-but-idle row receives nothing whatever this
    // popup says, so offering it would be offering a choice with no effect —
    // and connecting it is the F10 console's job, not this one's.
    std::vector<TargetView> LiveTargets() {
        std::vector<TargetView> live;
        for (const TargetView &v : Targets())
            if (v.connected) live.push_back(v);
        return live;
    }

} // namespace

int PickTargets(HWND hOwner, bool force) {
    std::vector<TargetView> live = LiveTargets();

    // Nothing to disambiguate. One connected screen is THE screen, and none at
    // all is a question about a list the caller already reports on. `force`
    // (Shift+F11) asks anyway, for the sake of the Sync now item and of simply
    // seeing what is selected.
    if (!hOwner || live.empty() || (live.size() < 2 && !force))
        return MirroredLiveCount();

    // Every connected target starts ticked the first time the picker is opened
    // in a session — the wall-of-screens case, and the one that should need no
    // clicks. Only a selection the user has already narrowed comes back narrow.
    //
    // Reconnected targets are included by that rule too: `mirroring` lives on
    // the target and survives a link dropping, so a row deliberately unticked
    // stays unticked when its screen comes back.

    // The slideshow may have hidden the pointer, and the cursor-hide timer must
    // not fire while a popup is up. Same handling as the main context menu.
    const bool wasMenuOpen = app.isContextMenuOpen;
    app.isContextMenuOpen  = true;
    if (app.slideshow.cursorHidden) {
        ShowCursor(TRUE);
        app.slideshow.cursorHidden = false;
    }
    KillTimer(hOwner, Constants::Slideshow::CURSOR_TIMER_ID);
    SetCursor(LoadCursor(nullptr, IDC_ARROW)); // popups inherit the current cursor

    const POINT pt = CenterOf(hOwner);

    // Win32 closes a menu on every pick, but ticking targets is inherently a
    // multi-select gesture — so put it straight back up after each toggle,
    // re-anchored at the SAME point so the rows do not move under the cursor.
    // Done, or a dismissal, ends it; whatever is ticked then is the answer.
    for (;;) {
        const UINT cmd = TrackAt(Build(live), hOwner, pt);
        if (cmd == 0 || cmd == ID_DONE) break;

        if (cmd == ID_SYNC) {
            SyncSelected(live);
        } else if (cmd == ID_ALL || cmd == ID_NONE) {
            // Acts on the CONNECTED rows only, matching what the popup shows. A
            // hidden row silently changed by a button the user cannot see it
            // under is exactly the kind of surprise this feature exists to fix.
            for (const TargetView &v : live) SetMirroring(v.id, cmd == ID_ALL);
        } else if (cmd >= ID_FIRST && cmd - ID_FIRST < live.size()) {
            const TargetView &v = live[cmd - ID_FIRST];
            SetMirroring(v.id, !v.mirroring);
        }

        live = LiveTargets(); // re-read: the toggle above, plus anything that moved
    }

    // Eat the click that dismissed the popup, or it lands on the image
    // underneath as a zoom.
    MSG peek;
    while (PeekMessageW(&peek, hOwner, WM_LBUTTONDOWN, WM_LBUTTONUP, PM_REMOVE));

    app.isContextMenuOpen = wasMenuOpen;
    if (app.slideshow.running && !app.slideshow.paused && app.slideshow.cursorHideMs > 0)
        SetTimer(hOwner, Constants::Slideshow::CURSOR_TIMER_ID,
                 app.slideshow.cursorHideMs, nullptr);

    return MirroredLiveCount();
}

} // namespace Remote::Mirror
