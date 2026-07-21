#pragma once
#include <windows.h>

// =============================================================================
// ContextMenuHandler.h  —  THE menu.
//
// One definition, two entry points: the main-window right-click (MouseHandler)
// and the tray-icon right-click (TrayHandler) both call Show(), so the two can
// never drift apart.
//
// Ids live in two disjoint ranges (see the ID SPACE note in the .cpp):
//   • viewer actions  → mapped to a Command and run through
//                       InputManager::ExecuteCommand, so a menu pick behaves
//                       exactly like the matching keyboard shortcut.
//   • settings / app  → forwarded to TrayHandler::DispatchCommand, which owns
//                       the prompts, export/import, restore and backup logic.
//
// To add a viewer item: add an id to the MenuId enum, a row in CommandForId,
// and one AppendMenuW line in BuildMenu. Nothing else.
//
// Gating (WHEN the viewer shows it) lives in MouseHandler: only on a *pure*
// right-button click, and only when app.contextMenuEnabled is set.
// =============================================================================

namespace Input {
    class ContextMenuHandler {
    public:
        // x / y are SCREEN coordinates (TrackPopupMenu space).
        static void Show(HWND hWnd, int x, int y);

        // Exposed for reuse/testing: build the shared menu, or route one id to
        // its owner. Show() is just these two around a TrackPopupMenu call.
        static HMENU BuildMenu(HWND hWnd);
        static void  Dispatch(HWND hWnd, int id);
    };
}
