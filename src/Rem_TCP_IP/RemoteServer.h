#pragma once
#include <windows.h>
#include <atomic>
#include <memory>
#include <string>
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
