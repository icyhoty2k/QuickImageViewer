// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#pragma once
#include <windows.h>

// =============================================================================
// TrayHandler — the notification-area icon, and nothing else.
//
// Double-click restores the window; right-click shows the application menu.
// That menu is UI::AppMenu — the same one the main window's right-click shows —
// and this class only supplies the anchor point.
//
// It used to own DispatchCommand as well: 1,145 of this file's 1,226 lines were
// the menu's settings dispatch, which is not a tray concern and only lived here
// because the menu had been split build-here / execute-there. That half now
// lives beside the menu it belongs to, in src/UI/AppMenu.
// =============================================================================

namespace Input {

class TrayHandler {
public:
    LRESULT Handle(HWND hWnd, WPARAM wParam, LPARAM lParam);

    // Public and static because the tray double-click is no longer the only
    // way back: Command::ToggleAppVisibility restores the window too, and a
    // remote caller has no tray icon to click. One copy of the foreground
    // hand-over (AttachThreadInput — see the .cpp) rather than two that drift.
    static void RestoreWindow(HWND hWnd);
};

} // namespace Input
