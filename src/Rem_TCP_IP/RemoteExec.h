#pragma once
#include <windows.h>
#include <string>
#include "RemoteProtocol.h"
#include "Input/Command.h"

// =============================================================================
// RemoteExec — execution of the payload-carrying commands.
//
// WHY THIS FILE EXISTS: several commands take a VALUE, and the bare enumerator
// for each of them opens a picker instead of doing the work — JumpToImage raises
// a panel, ZoomTo raises the zoom panel, OpenFile raises a file chooser,
// SlideshowSetInterval prompts a dialog. A remote client routed into any of
// those would hold its connection open until somebody physically dismissed a
// window on the machine.
//
// So each gets a path here that does the work and nothing else.
//
// THIS IS NOW THE ONLY IMPLEMENTATION, not a parallel one. It began as a
// deliberate duplicate of what the panels did, kept in step by hand, with a
// comment warning that a command behaving differently depending on whether it
// arrived over a socket or from the keyboard would be a bug that only showed up
// in the field. The panels now call InputManager::ExecuteCommand's payload
// overload, which lands here — so "kept in step by hand" became "is the same
// code", and the warning no longer describes a risk that exists.
//
// Everything here runs on the UI THREAD: from the WM_QIV_REMOTE_COMMAND handler
// for a socket caller, or directly for a panel. Nothing here is safe to call
// from a socket thread.
//
// Full design record: docs/REMOTE_TCP_IP_SPEC.md
// =============================================================================

namespace Remote {

    // THE payload path. Returns true when `cmd` takes a value and `replyOut`
    // has been filled; false when the command belongs on the ordinary
    // no-payload ExecuteCommand path.
    //
    // `replyOut` is an "OK …"/"ERR …" protocol line even for a local caller.
    // The panels ignore it; keeping one return shape means the socket and the
    // panel cannot diverge on what counts as success.
    bool ExecutePayload(HWND hWnd, Command cmd, const std::wstring &payload,
                        std::wstring &replyOut);

    // Wire-side convenience: unpacks a parsed request and calls the above.
    bool ExecutePayloadCommand(HWND hWnd, const RemoteRequest &req,
                               std::wstring &replyOut);

    // This instance's view/effect state as a `sync` payload — "k=v;k=v;…".
    //
    // Built in the same file that parses it, deliberately: a key spelled one way
    // by the sender and another by the receiver would fail silently, leaving the
    // two screens looking different with nothing reporting an error.
    //
    // `includeFolder` false omits the folder= field, for a target on another
    // machine where a local drive letter names nothing — or names something
    // else, which is worse. Everything else in the payload (view mode, the
    // effect chain, slideshow settings) is machine-independent and travels
    // either way, so drift repair keeps working across machines even though the
    // content does not match.
    //
    // UI thread only — it reads `app`.
    std::wstring BuildSyncPayload(bool includeFolder = true);

} // namespace Remote
