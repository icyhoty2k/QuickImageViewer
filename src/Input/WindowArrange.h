// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#pragma once
#include <windows.h>
#include <vector>

// =============================================================================
// WindowArrange — placing this window, and laying out every instance at once.
//
// WHO COUNTS AS AN INSTANCE. Every VISIBLE top-level window of this process's
// own window class (app.windowClassName). Ctrl+N copies share the main class,
// so they arrange together; a dedicated copy registers its own class, so the
// main app never moves a screen somebody placed on purpose. Hidden-to-tray
// copies are skipped — moving a window nobody can see would only surprise
// whoever restores it.
//
// EACH INSTANCE PLACES ITSELF. The arranging copy computes the slots and sends
// every other window its rectangle over WM_COPYDATA (COPYDATA_PLACE). Moving
// another process's window with SetWindowPos would work on screen and leave
// that copy's own state wrong — still believing it is fullscreen, or autosized
// — so the receiver applies the rect through PlaceSelf, the same path a local
// snap takes. A copy that predates this ignores the unknown dwData; a hung one
// is skipped after a short timeout rather than stalling the menu.
// =============================================================================

namespace WindowArrange {

    enum class Layout {
        SideBySide, // 2 — left | right
        Stacked,    // 2 — top / bottom
        Columns,    // 3 — three columns
        Rows,       // 3 — three rows
        Corners,    // 4 — quarters
        Grid,       // 2+ — ceil(sqrt(n)) columns
    };

    // WM_COPYDATA dwData for a placement request. 1 and 2 are the
    // single-instance handoff in AppMain.cpp.
    constexpr ULONG_PTR COPYDATA_PLACE = 3;

    // Visible windows of this app's class on this desktop, this one included.
    std::vector<HWND> FindInstances();

    // Whether `layout` can be applied to `count` windows. The menu greys the
    // rest out; Arrange re-checks because the count can change in between.
    bool IsAvailable(Layout layout, size_t count);

    // The window count a fixed layout needs; 0 for Grid (any count from 2).
    size_t RequiredCount(Layout layout);

    struct Result {
        size_t found  = 0; // instances found
        size_t placed = 0; // instances that confirmed the move (this one included)
    };

    // Lays out every instance on the work area of the monitor `self` is on.
    // Does nothing when the layout does not fit the count — see IsAvailable.
    // A one-off: it neither remembers positions nor moves the Cycle.
    Result Arrange(HWND self, Layout layout);

    // Display name, the same words as the menu.
    const wchar_t *LayoutName(Layout layout);

    // WM_COPYDATA dwData for "restore what the cycle moved".
    constexpr ULONG_PTR COPYDATA_RESTORE = 4;

    // Steps a lone window takes under Ctrl+Alt+Space: the four halves, the
    // four quarters, then default size centred — and round again.
    constexpr int SINGLE_STEPS = 9;

    struct CycleResult {
        enum class Kind { Arranged, Restored, Single };
        Kind   kind = Kind::Single;
        Layout layout = Layout::Grid; // Arranged: the layout applied
        int    singleStep = 0;        // Single: 0..SINGLE_STEPS-1, for the caller to apply
        size_t found  = 0;
        size_t placed = 0;
    };

    // Ctrl+Alt+Space. With 2+ windows: the layouts for that count in turn
    // (2: Side by Side, Top and Bottom; 3: Columns, Rows; 4: Corners; 5+:
    // Grid), then RESTORE — every window back where it was before the cycle
    // first moved it, unless it has been moved by hand since. The step is
    // shared, so the key continues the cycle from any window. With one window
    // it reports the next single-window step instead; the caller applies it.
    CycleResult Cycle(HWND self);

    // WM_COPYDATA handler for COPYDATA_RESTORE. TRUE when the window moved.
    BOOL HandleRestoreRequest(HWND hWnd, const COPYDATASTRUCT &cds);

    // Moves THIS window to `rect` (screen coordinates): leaves fullscreen,
    // restores from minimised/maximised, and clears autosize first.
    void PlaceSelf(HWND hWnd, const RECT &rect);

    // WM_COPYDATA handler for COPYDATA_PLACE. Returns TRUE when the window was
    // placed, FALSE when the request was malformed or refused (kiosk lock).
    BOOL HandlePlaceRequest(HWND hWnd, const COPYDATASTRUCT &cds);
}
