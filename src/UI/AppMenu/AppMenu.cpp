// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#include "AppMenu.h"
#include "AppMenuIds.h"
#include "AppMenuInternal.h"

#include "AppState.h"
#include "Input/Command.h"
#include "Platform/Constants.h"

extern AppState app;

namespace UI::AppMenu {

using namespace UI::AppMenu::detail;
namespace Id = UI::AppMenu::Ids;

namespace {
    // One TrackPopupMenu round-trip. Takes ownership of hMenu.
    int TrackMenuAt(HMENU hMenu, HWND hWnd, int x, int y) {
        if (!hMenu) return 0;
        // SetForegroundWindow + the WM_NULL kick are the documented workaround so
        // the popup dismisses on the first click outside it.
        SetForegroundWindow(hWnd);
        const int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                       x, y, 0, hWnd, nullptr);
        PostMessage(hWnd, WM_NULL, 0, 0);
        DestroyMenu(hMenu); // frees the whole tree, submenus included
        return cmd;
    }
}

// =============================================================================
// Dispatch — routes one chosen id to its owner (see AppMenuIds.h).
// =============================================================================
void Dispatch(HWND hWnd, int id) {
    if (id <= 0) return;

    if (id >= Id::VIEWER_BASE) {
        // Through the shared sink, not a private implementation: a menu pick
        // then behaves exactly like the keyboard shortcut, including the
        // session filter and the mirror gate at the top of ExecuteCommand.
        const Command c = CommandForId(id);
        if (c != Command::None)
            InputManager::ExecuteCommand(hWnd, c);
        return;
    }

    DispatchSetting(hWnd, id);
}

// =============================================================================
// Show — build, track, dispatch. x / y are SCREEN coordinates.
// =============================================================================
void Show(HWND hWnd, int x, int y) {
    // The slideshow may have hidden the pointer — bring it back for the menu and
    // block the hide timer for as long as any popup is up.
    app.isContextMenuOpen = true;
    if (app.slideshow.cursorHidden) {
        ShowCursor(TRUE);
        app.slideshow.cursorHidden = false;
    }
    KillTimer(hWnd, Constants::Slideshow::CURSOR_TIMER_ID);

    // Force arrow — TrackPopupMenu inherits the current cursor for its popup window
    SetCursor(LoadCursor(nullptr, IDC_ARROW));

    // TPM_RETURNCMD makes TrackPopupMenu return the chosen id instead of posting
    // WM_COMMAND, so everything dispatches inline.
    int cmd = TrackMenuAt(Build(hWnd), hWnd, x, y);
    Dispatch(hWnd, cmd);

    // Ticking a transition in LIST mode is inherently a multi-select gesture, but
    // Win32 closes the menu on every pick. Put the transition popup straight back
    // up — rebuilt, so the new tick shows — until the user chooses something else
    // or dismisses it. Re-anchored at the ORIGINAL point, not the cursor: a fixed
    // position keeps every row where it was, so ticking several in a row is just
    // moving down the list. Following the cursor would shift the rows each time.
    while (IsTransitionListToggle(cmd)) {
        cmd = TrackMenuAt(BuildTransitionMenu(), hWnd, x, y);
        Dispatch(hWnd, cmd);
    }

    // Eat the click that dismissed the menu so it doesn't trigger zoom
    MSG peek;
    while (PeekMessageW(&peek, hWnd, WM_LBUTTONDOWN, WM_LBUTTONUP, PM_REMOVE));

    app.isContextMenuOpen = false;
    // Restart the inactivity countdown only if a slideshow is actually running.
    if (app.slideshow.running && !app.slideshow.paused && app.slideshow.cursorHideMs > 0)
        SetTimer(hWnd, Constants::Slideshow::CURSOR_TIMER_ID,
                 app.slideshow.cursorHideMs, nullptr);
}

} // namespace UI::AppMenu
