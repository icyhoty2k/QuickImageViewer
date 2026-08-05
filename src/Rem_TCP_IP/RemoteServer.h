// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#pragma once
#include <windows.h>
#include <atomic>
#include <memory>
#include <string>
#include <vector>   // Connections() — the F9 panel's live list
#include "RemoteProtocol.h"
#include "RemoteInbound.h"

// =============================================================================
// RemoteServer — the TCP listener and its client threads.
//
// THREADING, which is the whole difficulty here:
//
//   • The listener thread owns the listening socket and nothing else.
//   • Each accepted client gets its own thread, capped at MaxConnections.
//   • NEITHER ever touches `app`, `app.playlist`, any GDI handle, or the
//     swapchain. That is the existing house rule and it is absolute.
//   • A parsed command is marshalled to the UI thread as WM_QIV_REMOTE_COMMAND
//     and executed there, exactly as the decoder and scan workers already do.
//
// HOW A CLIENT GETS ITS ANSWER, without deadlocking:
//
// A client thread must report whether its command actually ran, so it has to
// wait for the UI thread. It does NOT use SendMessage — a cross-thread
// SendMessage blocks forever if the UI thread is itself waiting on something.
// Instead the request is posted with a shared_ptr and the client waits on an
// event with a bounded timeout:
//
//   client: make_shared<RemoteCall>, PostMessage(new shared_ptr<RemoteCall>),
//           WaitForSingleObject(call->doneEvent, REPLY_TIMEOUT_MS)
//   UI:     execute, write result, SetEvent, delete the heap shared_ptr
//
// Both sides hold their own shared_ptr, so a client that times out and walks
// away cannot free memory the UI thread is still writing into, and the UI thread
// finishing after the client left cannot leak. Whichever releases last destroys.
//
// STOPPING: accept() is never left blocking indefinitely. The loop selects on
// the listening socket with a short timeout and re-checks an atomic stop flag,
// so Stop() is prompt and does not rely on closing a socket out from under a
// thread that is sitting inside a blocking call on it.
//
// Full design record: docs/REMOTE_TCP_IP_SPEC.md
// =============================================================================

namespace Remote {

    // One command in flight between a client thread and the UI thread.
    // Lifetime is shared: see the header comment above.
    struct RemoteCall {
        RemoteRequest req;
        std::wstring  result;                 // written by the UI thread
        HANDLE        doneEvent = nullptr;    // manual-reset; signalled by the UI thread
        // Which connection sent it. Needed for two things: `observe` has to know
        // who is asking to be added, and the echo has to know who NOT to send
        // back to.
        ConnId        conn = CONN_NONE;

        RemoteCall();
        ~RemoteCall();
        RemoteCall(const RemoteCall &)            = delete;
        RemoteCall &operator=(const RemoteCall &) = delete;
    };

    // Starts the listener using the current Remote::Config().
    // Returns false and fills `errorOut` when it cannot start — port already in
    // use, bind refused, disabled, no port configured. Never throws.
    //
    // `hOwner` receives WM_QIV_REMOTE_COMMAND and WM_QIV_REMOTE_STOPPED.
    bool Start(HWND hOwner, std::wstring &errorOut);

    // Stops the listener and waits for the listener thread to finish. Client
    // threads are asked to close and detached — a client blocked in recv() on a
    // dead socket ends when the socket closes. Safe to call when not running.
    void Stop();

    bool IsRunning();

    // Live client count, for the panel's status line.
    int ActiveConnections();

    // =========================================================================
    // LIVE CONNECTIONS, AND EJECTING THEM — the F9 panel's Kick and Ban.
    //
    // DELIBERATELY NOT ON THE WIRE. Neither of these appears in the command
    // table and neither should: `ban` writes access-control state to a file that
    // is read before every accept and survives a restart, which is the same
    // category as delete/move/save that NEVER_REMOTE keeps off the wire. A
    // remotely reachable ban would also let any authenticated peer blacklist the
    // OPERATOR's own address — turning one leaked password into a lockout that
    // can only be undone by hand-editing a file on the machine itself.
    //
    // These are called from the panel, on the UI thread, and nowhere else.
    // =========================================================================

    struct ClientInfo {
        ConnId       id;              // the value Kick/Ban take
        std::wstring address;         // bare peer address — no port, no brackets
        int          port        = 0; // the peer's source port
        std::wstring name;            // from `hello <name>`; empty until sent
        long long    sinceMs     = 0; // connected at, GetTickCount64 base
        bool         tls         = false;
        bool         sameMachine = false;
    };

    // Snapshot of what is connected right now, ordered oldest first. A snapshot
    // because a connection can end between the call and the next line — Kick and
    // Ban both tolerate an id that has since gone away.
    //
    // Includes connections that have not finished the TLS handshake or
    // authenticated: those are precisely the ones an operator wants to eject.
    std::vector<ClientInfo> Connections();

    // Closes one connection. False when the id is no longer live.
    //
    // shutdown(), NOT closesocket(): the client thread owns that socket and will
    // close it itself. Shutting it down makes its blocked recv() return, so the
    // thread unwinds through its ordinary path — logging its departure, leaving
    // the observer list, releasing its locks. Closing the descriptor from here
    // would race that thread and could shutdown a number the system had already
    // reissued.
    //
    // NOT a ban. The same peer may reconnect immediately.
    bool KickConnection(ConnId id);

    // Kick, and refuse this peer for `minutes` afterwards.
    //
    // THE MIDDLE OPTION, and the one that actually answers a bot: a plain kick
    // it reconnects through in a second, and a ban is a permanent decision
    // nobody wants to make about an address that may be a customer tomorrow. A
    // timed block outlasts a retry loop and then forgets.
    //
    // The block is IN MEMORY and does not survive a restart — see the timed
    // section of RemoteBlacklist.h. Scoped with BlockScope, like Ban, so a v6
    // peer cannot step around it with the next address in its /64. `scopeOut`
    // receives what was actually blocked.
    bool TimedKickConnection(ConnId id, int minutes, std::wstring &scopeOut);

    // Blacklists the peer, then kicks it. In that order, so a reconnect racing
    // the disconnect is refused at the accept gate rather than admitted.
    //
    // What gets WRITTEN is BlockScope(address) — the exact address for IPv4, the
    // /64 for IPv6. Banning one v6 address is close to useless, since the peer
    // has 2^64 of them; see the note on BlockScope in RemoteSettings.h, which is
    // the same rule the brute-force guard applies.
    //
    // `scopeOut` receives what was actually written, so the panel can tell the
    // operator they just banned a prefix rather than an address. False when the
    // id is no longer live, in which case nothing is written.
    bool BanConnection(ConnId id, std::wstring &scopeOut);

    // True when the RUNNING listener is speaking TLS. False when stopped, and
    // false for a loopback-bound listener, which is plaintext by design.
    //
    // Answered from the snapshot taken at Start(), NOT from live Config(): the
    // user can retype the bind address in the panel while clients are connected,
    // and an indicator that changed colour before Stop/Start would be reporting
    // an intention rather than the state of the socket.
    bool IsEncrypted();

    // The address:port actually bound, once running. Empty when stopped.
    // Reports what the socket really got, not what was configured.
    std::wstring BoundEndpoint();

    // Executes a parsed request on the UI thread and returns the reply line.
    // Called ONLY from the WM_QIV_REMOTE_COMMAND handler — never from a socket
    // thread, because it reaches straight into InputManager::ExecuteCommand.
    std::wstring ExecuteOnUiThread(HWND hWnd, const RemoteRequest &req,
                                   ConnId from = CONN_NONE);

    // =========================================================================
    // OBSERVERS — connections that asked to be told what this instance does.
    //
    // An observer is a connected client that sent `observe 1`. That is the whole
    // definition: the list IS the state, so there is no "am I being observed"
    // flag anywhere that could disagree with it. An entry cannot outlive its
    // connection, so nothing is persisted and nothing needs cleaning up after a
    // crash — a dropped socket removes itself.
    //
    // A client can only ever nominate ITSELF. There is deliberately no way to
    // say "send your events to that other machine": an observer is always the
    // connection that asked, which keeps this from becoming a way to make one
    // screen shout at a third party.
    // =========================================================================

    void AddObserver(ConnId conn);
    void RemoveObserver(ConnId conn);
    bool HasObservers();

    // Push one line to every observer except `except` (the connection the
    // command came from, which must not be told what it just told us).
    //
    // UI THREAD ONLY — it is called from ExecuteCommand and LoadImageIndex.
    // The sockets belong to client threads that are parked in recv() and cannot
    // be handed work, so this writes to them directly.
    //
    // THE WRITE IS NON-BLOCKING AND THE LINE IS DROPPED IF IT WOULD BLOCK. An
    // observer whose receive window has filled must never be able to freeze the
    // viewer it is watching: an observer is a convenience, a stalled UI thread
    // is a defect. Same reasoning as the bounded REPLY_TIMEOUT_MS on the other
    // side of this file.
    // `positional` marks a line that names a playlist INDEX (`goto 47`). Those
    // reach same-machine observers only: an index is meaningful against the same
    // set of files and nowhere else. Actions — zoom, rotate, effects, view mode,
    // slideshow — carry no such assumption and go to every observer.
    void EmitToObservers(const std::wstring &line, ConnId except,
                         bool positional = false);

} // namespace Remote
