#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include "Input/Command.h"

// =============================================================================
// RemoteMirror — the driving half. One instance controlling several others.
//
// This is the client side of a session where a viewer on the main monitor drives
// the copies parked on the other screens. The listener half (RemoteServer) is
// unchanged and unaware of it: to a slave, a mirroring master is an ordinary
// client sending ordinary commands.
//
// -----------------------------------------------------------------------------
// ONE THREAD PER TARGET, AND THE UI THREAD NEVER WAITS.
//
// Remote::Client is strict request/reply and every call can block up to its
// timeout (RemoteClient.h). Mirroring happens inside ExecuteCommand, which runs
// on the UI thread on every keystroke — so nothing here may block it, not even
// briefly. Each target therefore owns:
//
//     a Client        the connection
//     a thread        the only thread that ever touches that Client
//     a queue         what the UI thread has asked to be sent
//
// The UI thread pushes a line and returns. It never touches a socket, never
// waits for a reply, and a target that has been switched off cannot slow it
// down. This is the same rule the decoder and scan workers already follow:
// results come back by message, never by making the UI thread wait.
//
// THE QUEUE IS BOUNDED AND DROPS ITS OLDEST. Holding a keystroke backlog for a
// machine that stopped answering would mean that when it came back it replayed
// minutes of stale navigation. A mirrored keystroke is only meaningful now.
// -----------------------------------------------------------------------------
//
// WHY INDICES AND NOT PATHS. A pick from a thumbnail strip is forwarded as an
// index rather than a file path, because an index costs nothing to produce
// (a path would mean a filesystem round trip on the UI thread for every click)
// and stays meaningful: sort order is itself a mirrored command, so both ends
// order their playlists identically.
//
// Identical sort still is not proof of identical indices — the two ends must
// also hold the same FILE SET, and one file added or removed on one side shifts
// everything after it. So every reply names the file that was actually landed
// on, the caller compares, and a mismatch triggers a `sync` push followed by a
// resend. Divergence is detected and repaired rather than assumed impossible.
//
// Full design record: docs/REMOTE_TCP_IP_SPEC.md
// =============================================================================

namespace Remote::Mirror {

    // One configured slave, as the F10 console shows it. This is a SNAPSHOT
    // type: Targets() copies the live state into these under the lock, so the
    // panel can paint from them without holding anything.
    // WHY a target is not connected. The list in qivRemotes.ini outlives the
    // instances it describes — one gets moved, renamed, reconfigured or simply
    // switched off — so "could not connect" is the normal state of at least one
    // row, and the useful question is which kind of not-connected it is. Each
    // one has a different remedy:
    //
    //   Offline    nothing is listening there. Start it (the dot does this when
    //              the instance is on this machine).
    //   Address    the host does not resolve. The row is wrong, not the target.
    //   Rejected   something accepted and hung up without speaking: almost
    //              always an AllowList that does not include this machine, and
    //              indistinguishable from "that port belongs to something else".
    //   Auth       it answered but refused the credentials — its password
    //              changed since this row was saved.
    //   Protocol   it talks, but not this protocol. Wrong port, or a version
    //              mismatch.
    enum class Down { None, Offline, Address, Rejected, Auth, Protocol };

    struct TargetView {
        // The handle AddTarget returned — what RemoveTarget, SendTo and
        // SetObserving expect. NOT the row's position: removing a target leaves
        // the others' ids alone, so a console that used positions would start
        // driving the wrong screen after any removal.
        int          id = 0;
        std::wstring name;
        std::wstring host;
        int          port = 0;
        std::wstring exePath;      // for the console's start button
        // Same box as us? Decides which command set it gets, and whether the
        // console's start button can do anything — CreateProcess only starts a
        // process here, so a remote row's dot cannot launch anything.
        bool         sameMachine = false;
        // Is this row TRYING to be connected? Distinct from `connected`, which
        // says whether it currently is. A row can be listed and idle: recorded
        // for later, or for a screen that is switched off today.
        bool         connecting = false;
        // Is this row in the current F11 selection? Distinct from both of the
        // above: a target can be connected — pollable, startable, watchable from
        // the console — while the mirrored keystrokes go elsewhere.
        bool         mirroring = true;
        bool         connected = false;
        bool         observing = false;  // we asked it to echo its actions to us
        long long    lagUs     = -1;     // last round trip, µs; -1 = never measured
        Down         down      = Down::None; // classified reason, for the list
        std::wstring lastError;          // the full sentence, for the footer
    };

    // A short word for the list column — "offline", "auth", "rejected". The
    // full sentence stays in lastError for the footer; a table needs something
    // that fits beside a port number.
    const wchar_t *DownLabel(Down d);

    // What to actually do about it, in one line. Shown under the list, because
    // knowing a target is "rejected" is only useful with "its AllowList probably
    // does not include this machine" beside it.
    const wchar_t *DownRemedy(Down d);

    // --- Session state ------------------------------------------------------

    // True while this instance is joined to any other — driving one (a target)
    // or being driven by one (a connected client). THE switch that puts the
    // viewer into its restricted mode: see IsBlockedInSession.
    //
    // DERIVED, never stored. A cached bool would have to be updated from the
    // socket threads as clients come and go, and would drift the first time one
    // of those paths was missed. Both halves are cheap — a vector size on the UI
    // thread and an atomic load — so there is nothing to gain by keeping a copy
    // that can disagree with the truth.
    bool SessionActive();

    // Is anything actually being driven right now? One atomic load.
    //
    // CONNECTED, not configured. F11 stays on when a screen is switched off —
    // the sender thread keeps retrying and mirroring resumes by itself — so the
    // flag alone is not enough to decide whether there is any work to do.
    bool HasLiveTargets();

    // --- Lifetime -----------------------------------------------------------

    // The window sender threads post their results to. Set once, after the main
    // window exists; cleared by Shutdown. A sender thread outliving the HWND it
    // reports to would post into nothing.
    void SetOwner(HWND hOwner);

    // --- Target list (UI thread only) ---------------------------------------

    // Adds a target and starts its sender thread. Connecting happens ON that
    // thread, so this returns immediately even for a host that is switched off.
    // `id` is the caller's handle to it — the row index the console shows.
    //
    // `connectNow` false LISTS the target without dialling it. Adding a remote
    // and connecting to it are separate acts: a screen can be recorded while it
    // is switched off, or before it is even set up, and the alternative was a
    // saved list that only contained things currently reachable.
    int  AddTarget(const std::wstring &name, const std::wstring &host, int port,
                   const std::wstring &password, const std::wstring &exePath,
                   bool connectNow = true);

    // Start or stop trying to reach one target. Stopping drops the connection
    // and leaves the row listed; the sender thread parks until asked again.
    void SetConnecting(int id, bool on);

    // Every target at once. The common case for a screen wall: bring the room
    // up, or put it down, without visiting each row.
    void SetConnectingAll(bool on);

    // Stops the thread, closes the connection, removes the row.
    void RemoveTarget(int id);

    // Drops every target and joins every thread. Call before the window dies —
    // a sender thread outliving the HWND it reports to would post into nothing.
    void Shutdown();

    int  TargetCount();
    std::vector<TargetView> Targets();

    // --- Sending (UI thread; never blocks) ----------------------------------

    // Queues `cmd` to every connected target. Silently does nothing for a
    // command that is not mirrorable — the caller does not have to pre-filter.
    void Broadcast(Command cmd);

    // Queues an already-formed protocol line ("goto 47", "sync …") to every
    // target. Used for the payload forms, which have no bare-Command spelling.
    void BroadcastLine(const std::wstring &line);

    // A playlist position — SAME-MACHINE TARGETS ONLY.
    //
    // An index is only meaningful against the same set of files. Instances on
    // this box share the folder, so index 47 is the same picture on both; an
    // instance elsewhere has its own content and would jump somewhere arbitrary.
    //
    // `expectFile` is the file name THIS instance landed on. The target names
    // the one IT landed on in its reply, and a mismatch means the two playlists
    // have drifted apart — reported as WM_QIV_REMOTE_DESYNC, repaired by the UI
    // thread with a `sync` and a resend. Bare file name, not a path: the two may
    // reach the same folder by different spellings.
    void BroadcastPosition(const std::wstring &line, const std::wstring &expectFile);

    // A `sync` in both spellings: `full` includes folder=, `portable` does not.
    // Each target is sent the one that suits it — a drive letter is worth
    // nothing on another machine, and applying it would send that instance to a
    // folder it cannot open.
    void BroadcastSync(const std::wstring &full, const std::wstring &portable);

    // --- Which targets F11 drives (UI thread; session state) ----------------
    //
    // Everything connected is mirrored until somebody says otherwise. Narrowing
    // is the exception — a wall of screens that should all follow along is the
    // normal case, and a picker that started empty would make the common setup
    // the one needing the most clicks.
    //
    // NOT persisted. See the Target::mirroring note in RemoteMirror.cpp: a
    // selection saved from last week would quietly leave a screen dark today.
    void SetMirroring(int id, bool on);
    void SetMirroringAll(bool on);

    // Connected AND selected — how many screens a keystroke would actually
    // reach. What the F11 overlay reports, and what tells the picker whether
    // there is anything to choose between.
    int  MirroredLiveCount();

    // How many targets are CONNECTED right now, out of TargetCount(). One
    // atomic load — the same counter HasLiveTargets tests, exposed as a number
    // so a status readout does not have to build the whole Targets() vector to
    // count it.
    int  ConnectedCount();

    // What the F11 overlay says after the arrow: "3 targets" when everything
    // connected is following along, "Monitor2 (1 of 3)" when the selection has
    // been narrowed. A narrowed selection is invisible otherwise — this line is
    // the only place it is stated.
    std::wstring SelectionSummary();

    // Queues to ONE target. The console's per-row buttons use this, and so does
    // the desync repair — both name a target explicitly, so neither is filtered
    // by the mirror selection.
    void SendTo(int id, const std::wstring &line);

    // --- Hand-sent commands (Ctrl+F10) ----------------------------------------

    // One target's answer to a line somebody typed. Delivered as
    // WM_QIV_REMOTE_CMD_REPLY; the handler owns the pointer and deletes it.
    struct CmdReply {
        std::wstring target;   // which instance answered
        std::wstring line;     // what was sent, verbatim
        std::wstring reply;    // its answer, or the error when ok is false
        bool         ok = false;
        long long    deltaUs = -1;
    };

    // Send one typed line to every CONTROLLED target — the ☑ rows in Ctrl+F11,
    // which is the same selection F11 drives, so "send a command" and "mirror a
    // keystroke" cannot reach different machines.
    //
    // Each target answers separately to `replyTo`. Returns how many were sent,
    // so the caller can say "0 targets" rather than waiting for replies that
    // will never come.
    int SendToControlled(const std::wstring &line, HWND replyTo);

    // `ping` every target and record the round trip in TargetView::lagUs.
    //
    // Deliberately reuses each target's LIVE connection rather than opening a
    // fresh one: MaxConnections defaults to 1 (Constants.h), so a second
    // connection to a slave already driven by this master would simply be
    // refused. Measuring the channel actually in use is also the more honest
    // number — it reflects how busy that viewer's UI thread is, which on
    // loopback is the only thing that varies.
    void PingAll();

    // --- Telling the console, instead of it asking -----------------------------
    //
    // The F10 Remote Servers console registers here while it is open and is sent
    // WM_QIV_REMOTE_TARGETS_CHANGED whenever a target CONNECTS, DISCONNECTS, or
    // changes the reason it is down. Without this the console showed whatever
    // was true when it was last touched: a target coming up while it sat open
    // did not appear until something forced a repaint.
    //
    // PostMessage from a sender thread, never Send: a socket thread must not
    // wait on the UI thread, least of all for a repaint.
    //
    // CONNECTION STATE ONLY — not lag. Lag is rewritten on every mirrored
    // keystroke, and the console's rebuild stats each row's exe; notifying on it
    // would put a filesystem call per target on the mirror path.
    //
    // Coalesced: the post is skipped while one is outstanding, so a batch of
    // targets coming up together costs one message. nullptr unregisters, and a
    // closed console costs the sender threads one atomic load.
    void SetPanelNotifyWindow(HWND hwnd);

    // Called by the console, FIRST THING in the handler — before it re-reads the
    // list, so a change landing mid-rebuild posts again rather than being lost.
    void ClearPanelNotifyPending();

    // Put every connected target's wire log into the given state (`enablelog
    // 1|0`). Sent to ALL of them, not just the mirrored selection: the log
    // describes the session, and a screen left recording because it happened to
    // be unticked in Remotes Control is a growing file nobody will look at.
    //
    // Does NOT touch this instance — the caller sets its own flag, because the
    // caller is the one that owns app.remoteLogEnabled.
    void BroadcastEnableLog(bool on);

    // Ask every target to re-answer "are you on this machine?".
    //
    // The flag is resolved when a target is added, but the answer can go stale:
    // DHCP renews to a different address, a laptop changes network, a name
    // starts resolving elsewhere. A stale answer picks the wrong command set for
    // the mirror, and greys out (or wrongly enables) the console's start button.
    //
    // Returns IMMEDIATELY — it only raises a flag per target. Each sender thread
    // does its own getaddrinfo, because that call blocks and the UI thread may
    // not. Fresh values appear in the next Targets() snapshot, so a caller pairs
    // this with the repaint it was already scheduling rather than waiting.
    void RefreshSameMachine();

    // Asks one target to start/stop echoing its own actions back to us. The
    // events arrive as EVENT lines on the same connection; there is no reverse
    // connection and nothing to configure at the far end.
    //
    // EXCLUSIVE. Switching one on switches every other off, and tells them so —
    // observing means executing what the other instance does, and two at once
    // would interleave two unrelated streams of actions into one screen,
    // following neither. Enforced here rather than in the console so the
    // invariant does not depend on a particular window remembering it.
    void SetObserving(int id, bool on);

    // --- Receiving ----------------------------------------------------------
    //
    // Nothing to call: an EVENT line from an observed target is posted by its
    // sender thread as WM_QIV_REMOTE_EVENT and handled in the main WndProc. It
    // is executed there under the inbound guard, so an observer never
    // re-forwards what it was shown — which is what stops two mutually-bound
    // instances from trading a single keystroke forever.

} // namespace Remote::Mirror
