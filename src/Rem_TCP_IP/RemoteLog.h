// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#pragma once
#include <string>
#include <vector>

// =============================================================================
// RemoteLog — every line that crossed the wire, in one place.
//
// A record of remote COMMUNICATION, not of remote activity: one entry per
// request/response pair, both directions. What it answers is "what did the two
// instances actually say to each other, and how long did it take" — the
// question you have when a screen is a picture behind, or when a command
// visibly worked here and did nothing there.
//
// THE RECORD LIVES AT THE WIRE BOUNDARY, not at the call sites. This was got
// wrong first: two call sites were instrumented, and everything that sent or
// received by any other route — the `observe` re-arm, the handshake, the
// standalone probe, the verb replies, the parse-error refusals, and the entire
// inbound EVENT stream from a watched instance — was silently absent from a log
// that looked complete. A log with invisible holes is worse than no log,
// because it is believed. So every byte is recorded by the four functions it
// must physically pass through:
//
//   Client::Send        every request/reply this instance makes  (OUT, + IN for
//                       any EVENT that lands mid-exchange)
//   Client::PollLine    unsolicited lines from a watched target  (IN)
//   Client::DoConnect   the handshake, banner or failure         (OUT)
//   Server SendLine     every line this instance's listener writes (OUT)
//   Server RecvLine     every line it reads                        (IN)
//   EmitToObservers     the echo push, which bypasses SendLine to stay
//                       non-blocking and so needs its own                (OUT)
//
// Handshake lines are REDACTED at capture — see RedactForLog in RemoteServer.cpp.
//
// Producers are on several threads: mirror sender threads, per-client socket
// threads, and the UI thread.
//
// So the store is MUTEX-GUARDED, and deliberately not lock-free: a socket round
// trip is milliseconds and a mutex is nanoseconds, so the lock is free next to
// what it is recording. Nothing here touches `app`, GDI, or the playlist, which
// is what makes it callable from a sender thread at all.
//
// BOUNDED. A wall of screens running a slideshow generates entries for hours;
// an unbounded vector would be a slow leak that only shows up on the long runs
// this viewer is built for. Oldest entries are dropped — see CAPACITY.
//
// The panel (Ctrl+F12, RemoteLogWnd.h) only ever takes a Snapshot: it never
// holds the lock while painting, and it never sees the deque move under it. It
// is told when to do so — see SetNotifyWindow; nothing here polls.
// =============================================================================

#include <windows.h>   // HWND — SetNotifyWindow

namespace Remote::Log {

    // Which way the line went, from THIS instance's point of view.
    enum class Direction { Out, In };

    struct Entry {
        // 1-based, monotonic for the life of the process. NOT the row index: it
        // survives sorting, and it survives the oldest entries being dropped —
        // which is exactly what tells you entries WERE dropped.
        long long seq = 0;

        // UTC FILETIME (100 ns units). Stored as the raw number rather than a
        // formatted string so it sorts as a number, saves and loads without a
        // locale in the way, and can be turned into local time for display.
        long long whenFt = 0;

        // Round trip for an OUT entry, handling time for an IN one, in
        // microseconds. -1 when it was never measured.
        long long deltaUs = -1;

        // The OS thread that recorded it. Written to the file as the [thread]
        // column every standard log layout carries, and it earns its place here:
        // one socket thread per connection means the thread id is the cheapest
        // honest answer to "which conversation is this row part of" when two
        // peers are talking at once and their rows interleave.
        unsigned long threadId = 0;

        Direction    dir = Direction::Out;
        std::wstring sender;    // who issued the command
        std::wstring command;   // the line as it went on the wire
        std::wstring receiver;  // who was asked to run it
        std::wstring response;  // the reply line, or the error
    };

    // How many entries are kept. Beyond this the oldest are dropped.
    constexpr size_t CAPACITY = 20000;

    // --- The switch -----------------------------------------------------------
    //
    // OFF by default (Constants::RemoteTcpIp::REMOTE_LOG_DEFAULT). A diagnostic
    // that is always on is a cost every session pays for the one that needed it.
    //
    // ATOMIC, not `app.remoteLogEnabled`: the producers are a sender thread and
    // a socket-fed UI callback, and a socket thread may not read `app` at all.
    // The UI-thread bool and this atomic are flipped together — see AppState.h.
    //
    // CHECK IsCapturing() BEFORE BUILDING THE ARGUMENTS, not just before calling
    // Add. Add's own guard cannot stop a caller that has already concatenated
    // four strings to pass to it; the whole point of the switch is that a viewer
    // with logging off allocates nothing on the mirror path.
    void SetEnabled(bool on);
    bool IsEnabled();

    // IS ANYTHING LISTENING — the panel's store, the file, or both. THIS is the
    // gate every record point must use, and IsEnabled() is not.
    //
    // The two switches are independent: the file sink records whether or not the
    // in-memory store is recording, because the reason to write a file is that
    // you want a durable record, and having to remember a second switch to get
    // one is how you come back to an empty folder. A record point that asked
    // IsEnabled() would therefore go silent exactly when only the file was on —
    // and a log with invisible holes is worse than no log, because it is
    // believed.
    bool IsCapturing();

    // Record one exchange. Safe from any thread. Never throws, never blocks on
    // anything but the store's own mutex.
    void Add(Direction dir,
             const std::wstring &sender,
             const std::wstring &command,
             const std::wstring &receiver,
             const std::wstring &response,
             long long deltaUs);

    // A copy, under the lock. The panel sorts and filters its own copy, so the
    // producers are never held up by a repaint.
    std::vector<Entry> Snapshot();

    // --- The file sink ---------------------------------------------------------
    //
    // A SECOND destination for the same entries, persisted across restarts
    // (Constants::IS_TCP_IP_LOG) where the recording switch above is
    // session-only. The store answers "what just happened" and dies with the
    // process; a file answers "what happened before the crash, on the machine in
    // the other room" — and that is never a question you can switch on in advance.
    //
    // ON A THREAD OF ITS OWN, and this is not an optimisation.
    //
    // Add() is called from socket threads, from mirror sender threads, and — via
    // EmitToObservers — FROM THE UI THREAD. Writing to disk inside Add would put
    // a file write on the message pump: a slow disk, a network share, an
    // antivirus scan, and the viewer stops repainting. So Add only formats a row
    // and hands it to a queue, and one writer thread owns the file and does every
    // CreateFile, WriteFile and rotation.
    //
    // The queue is BOUNDED. A producer never blocks and never waits on the disk:
    // if the writer falls behind, the oldest queued rows are dropped and the file
    // records how many, because a diagnostic that stalls the thing it is
    // diagnosing has changed the problem it was meant to observe.
    //
    // Nothing is written while recording is off — Add is not called at all — so
    // the two switches are independent and the menu says so when they disagree.
    void SetFileLogging(bool on);
    bool FileLoggingIsOn();

    // The file being written right now, empty when the sink is off or has not
    // opened one yet. For the overlay message and the panel's status line: a
    // logger that will not say where it is writing is one you have to go looking
    // for.
    std::wstring CurrentFilePath();

    // "<exe folder>\logs" — where rotated files land. Returned whether or not it
    // exists yet, so a caller can name it in a message before anything is written.
    std::wstring LogDirectory();

    // Drains what is queued and closes the file. Called once as the app shuts
    // down: the writer thread is joined here rather than detached, because a
    // detached thread holding a file handle while the process exits is how a log
    // loses its last few rows — the ones written just before whatever went wrong.
    void ShutdownFileLogging();

    // --- This instance's name -------------------------------------------------
    //
    // Every entry names both ends, and one of them is always us. The name lives
    // in Remote::Config(), which is UI-THREAD-ONLY — and the outbound producer
    // is a sender thread, which may not read it. So it is snapshotted here,
    // under this store's own lock, by the UI thread at the two moments it starts
    // to matter: when the listener starts, and when a target is added.
    //
    // Falls back to "(this)" while unset. A viewer with no configured name is an
    // ordinary case — F10 works without one — and a blank column would read as a
    // bug rather than as an unnamed instance.
    void         SetSelfName(const std::wstring &name);

    // The listener's bound endpoint, for the saved file's preamble. Pushed in
    // from the server rather than read back out of it — this module is a leaf,
    // and a diagnostic that has to link against the thing it diagnoses is one
    // that cannot be used from anywhere else.
    void         SetSelfEndpoint(const std::wstring &endpoint);
    std::wstring SelfEndpoint();

    // "Monitor2 192.168.0.102:7777" — what this instance is called in a log
    // row, in the same name-then-address shape a peer is given. SelfName alone
    // could not answer "which machine wrote this", which is the question a
    // saved log exists to answer.
    std::wstring SelfLabel();
    std::wstring SelfName();

    size_t Count();

    // --- Being told, instead of asking --------------------------------------
    //
    // The open panel registers here; Add posts WM_QIV_REMOTE_LOG_ADDED to it.
    // A timer was the obvious thing and the wrong one: it repaints when nothing
    // happened, and lags by up to its period when something did.
    //
    // PostMessage, never Send: one producer is a mirror sender thread, and a
    // socket thread that blocked waiting for a UI repaint would stall the very
    // exchange it is recording.
    //
    // COALESCED — the post is skipped while one is already outstanding, so a
    // burst of mirrored keystrokes costs one message and one rebuild rather than
    // one of each per keystroke. The handler clears the flag BEFORE it
    // snapshots, so an entry arriving mid-rebuild triggers another post rather
    // than being silently missed.
    //
    // nullptr unregisters. The panel does that when it hides, so a closed panel
    // costs nothing at all.
    void SetNotifyWindow(HWND hwnd);

    // Called by the notified window, first thing. See above for why first.
    void ClearNotifyPending();

    // Same notification, for a change that is not a new entry — the recording
    // switch being flipped by something other than the panel's own button, which
    // is what happens when another instance sends `enablelog`. Without it the
    // panel's Recording button would keep claiming the old state.
    void NotifyChanged();

    // Empties the store. Does NOT reset the sequence counter: numbers that came
    // back after a Clear would look like the same exchanges twice in a saved
    // file, and the whole point of seq is that it never repeats.
    void Clear();

    // --- Disk -----------------------------------------------------------------
    //
    // Tab-separated, UTF-8 with a BOM, one entry per line under a header line.
    // Tabs and newlines inside a field are replaced on the way out, because a
    // response can carry anything and a log you cannot reload is not a log.
    //
    // Both return false and fill `errorOut` with a sentence fit to show a user.
    bool SaveTo(const std::wstring &path, std::wstring &errorOut);

    // REPLACES the store with the file's contents — this is "look at a log I
    // saved earlier", not "merge it into what is happening now". Mixing a
    // recorded session into a live one would produce a timeline that never
    // existed.
    bool LoadFrom(const std::wstring &path, std::wstring &errorOut);

    // --- Formatting (shared by the panel and the file) -------------------------

    // FILETIME → "14:07:32.416", local time. Empty for 0.
    std::wstring FormatTime(long long whenFt);

    // Microseconds → "0.4 ms" / "12 ms" / "1.8 s". "—" for a negative value,
    // which means it was never measured.
    std::wstring FormatDelta(long long us);

} // namespace Remote::Log
