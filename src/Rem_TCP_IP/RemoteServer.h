#pragma once
#include <windows.h>
#include <atomic>
#include <memory>
#include <string>
#include "RemoteProtocol.h"

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
    std::wstring ExecuteOnUiThread(HWND hWnd, const RemoteRequest &req);

} // namespace Remote
