#pragma once
#include <windows.h>

// =============================================================================
// ContextMenuHandler.h  —  Single source of truth for the MAIN-WINDOW
// right-click context menu.
//
// Every menu item funnels through InputManager::ExecuteCommand(), so a menu
// pick does *exactly* what the matching keyboard shortcut does — no duplicated
// side-effect logic lives here. To add / change an item, edit ONLY the table
// inside ContextMenuHandler.cpp (label + Command).
//
// Gating (who decides WHEN to show it) lives in MouseHandler: the menu is
// raised on a *pure* right-button click (no window drag, no RMB+wheel/LMB
// combo) and only when app.contextMenuEnabled is set (tray toggle).
// =============================================================================

namespace Input {
    class ContextMenuHandler {
    public:
        // x / y are SCREEN coordinates (TrackPopupMenu space).
        static void Show(HWND hWnd, int x, int y);
    };
}
