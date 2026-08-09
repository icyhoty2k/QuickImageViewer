// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

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

    // NOTE: ExecutePayloadCommand — the "unpack a RemoteRequest and call the
    // above" convenience — is gone. It had two callers, the server dispatch and
    // AppMain's observer replay, and both used it to reach the payload handlers
    // WITHOUT going through InputManager::ExecuteCommand. That was the detour
    // that left payload commands outside the crash breadcrumb and the observer
    // echo. Both now call the sink's payload overload, which unpacks nothing
    // because it is handed the two fields directly.
    //
    // Call ExecutePayload above only from inside that sink. Anything else wants
    // InputManager::ExecuteCommand(hWnd, cmd, payload, &reply).

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

    // INTERNAL, and never seen by a client.
    //
    // SendDisplayedPreview is answered in two stages: the UI thread returns the
    // displayed file's PATH behind this marker (cheap), and the socket thread
    // then decodes, scales and encodes it — work far too slow to do while the
    // viewer is blocked on it. ClientThread strips the marker and replaces the
    // reply with the real one.
    //
    // Distinctive on purpose: if this ever reached a client it would mean the
    // dispatch broke, and it should be obvious rather than look like an answer.
    constexpr const wchar_t *PREVIEW_PATH_MARKER = L"\x01qIV-preview-path\x01";

    // The same trick for the ORIGINAL file, and for the same reason.
    //
    // SendDisplayedImage used to be answered inline on the UI thread: the whole
    // file was read into memory and base64-encoded there. A 10 MB photo is
    // roughly 13 million characters of base64 — 26 MB as wchar_t — built while
    // the viewer could not paint. Preview had been moved off the UI thread for
    // exactly this reason; the original, which is BIGGER, had been left behind.
    //
    // It is the command a phone sends when the user saves the displayed picture,
    // so the freeze landed on an ordinary action rather than a rare one.
    //
    // The reply bytes are unchanged — same "<bytes>;<name>" shape, same DATA
    // body. Only the thread that builds them is different, so no client needs to
    // know this happened.
    constexpr const wchar_t *ORIGINAL_PATH_MARKER = L"\x01qIV-original-path\x01";

} // namespace Remote
