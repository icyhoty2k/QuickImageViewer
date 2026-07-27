#pragma once
#include <windows.h>
#include <string>
#include "RemoteProtocol.h"

// =============================================================================
// RemoteExec — headless execution of the payload-carrying commands.
//
// WHY THIS FILE EXISTS: five commands in the protocol table take a value, and
// all five reach UI when driven through InputManager::ExecuteCommand —
// JumpToImage opens a panel, ZoomTo opens the zoom panel, OpenFile raises a file
// chooser, SlideshowSetInterval prompts a dialog. A remote client routed into
// any of those would hold its connection open until somebody physically
// dismissed a window on the machine.
//
// So each one gets a direct path here that does the work and nothing else. Each
// mirrors the semantics of its interactive counterpart EXACTLY — same index
// base, same clamping, same persistence, same overlay message — because a
// command that behaves differently depending on whether it arrived over a socket
// or a keyboard would be a bug that only shows up in the field.
//
// Everything here runs on the UI THREAD, called from the WM_QIV_REMOTE_COMMAND
// handler. Nothing in this file is safe to call from a socket thread.
//
// Full design record: docs/REMOTE_TCP_IP_SPEC.md
// =============================================================================

namespace Remote {

    // Handles a payload command. Returns true when `req.cmd` was one of the five
    // and `replyOut` has been filled; false when the command belongs on the
    // ordinary ExecuteCommand path.
    bool ExecutePayloadCommand(HWND hWnd, const RemoteRequest &req,
                               std::wstring &replyOut);

} // namespace Remote
