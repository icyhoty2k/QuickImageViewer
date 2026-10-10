// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#pragma once
#include <windows.h>
#include "Command.h"

// =============================================================================
// WindowSync — "Sync Instances" (Ctrl+Shift+N): navigation, zoom, pan and
// rotate/flip pressed in one window happen in every other one too.
//
// WHO. The same windows Arrange All moves (WindowArrange::FindInstances):
// visible copies of this app's own window class on this desktop. Each one
// applies the action to ITS OWN folder — `next` means its next picture, not
// the sender's.
//
// ONE SWITCH. Turning sync on or off in any window sets every window to that
// state; the message carries the new state, not a flip, so windows that
// disagreed (a fresh copy started off) end up agreeing. A copy started with
// Ctrl+N while sync is on starts synced (QIV_SYNC_INSTANCES).
//
// WIRE. A registered window message, POSTED: wParam is a SyncAction code,
// fixed below and independent of the Command enum, whose order is not stable
// between builds. Posting never waits, so holding an arrow key cannot stall on
// a slow or hung window. A copy that predates this never registered the
// message and ignores it.
//
// NO LOOPS. A window executing a received action does not forward it again
// (InboundActive), so two synced windows cannot ping-pong a keystroke.
//
// Runtime only — never saved. A viewer that came back up silently driving
// other windows would be a surprise.
// =============================================================================

namespace WindowSync {

    // The registered message, the same value in every process.
    UINT Message();

    // Forwards `cmd` to the other instances when sync is on, `cmd` is one of
    // the synced actions and it did not itself arrive from a peer. Called at
    // the top of ExecuteCommand, so every input path is covered.
    void Broadcast(HWND self, Command cmd);

    // Sets sync on/off here and in every other instance, and says so.
    void SetEnabled(HWND self, bool on);

    // Handles Message() in the main window procedure.
    void HandleMessage(HWND hWnd, WPARAM wParam);

    // True while an action received from a peer is being executed.
    bool InboundActive();

    // Environment variable a Ctrl+N launch carries so the new copy starts synced.
    constexpr const wchar_t *ENV_START_SYNCED = L"QIV_SYNC_INSTANCES";
}
