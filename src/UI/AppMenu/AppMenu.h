#pragma once
#include <windows.h>

// =============================================================================
// AppMenu — THE application menu.
//
// One definition, two entry points: the main-window right-click (MouseHandler)
// and the tray-icon right-click (TrayHandler) both call Show(), so the two can
// never drift apart.
//
// -----------------------------------------------------------------------------
// WHY THIS IS ITS OWN FOLDER, AND NOT "ContextMenuHandler" ANY MORE.
//
// It was never only a context menu — the tray shows the same thing — and its
// implementation was split across two files by accident of history rather than
// by design: the menu STRUCTURE lived in ContextMenuHandler.cpp while half its
// BEHAVIOUR lived in TrayHandler.cpp, which was 93% menu dispatch and 7% tray.
//
// The concrete cost of that split was a hand-maintained agreement. The builder
// declared twenty id constants carrying the comment "values must match the cases
// in TrayHandler::DispatchCommand" — two files, one numbering, no compiler
// anywhere in between. AppMenuIds.h is now that numbering, in one place, seen by
// both the code that appends an item and the code that handles it.
// -----------------------------------------------------------------------------
//
// THE ID SPACE, in one sentence: ids below VIEWER_BASE are settings and are
// handled inside this folder; ids at or above it map to a `Command` and run
// through InputManager::ExecuteCommand, so a menu pick behaves exactly like the
// matching keyboard shortcut. See AppMenuIds.h.
//
// Gating (WHEN the viewer shows it) lives in MouseHandler: only on a *pure*
// right-button click, and only when app.contextMenuEnabled is set.
// =============================================================================

namespace UI::AppMenu {

    // Build, track, dispatch. x / y are SCREEN coordinates (TrackPopupMenu space).
    void Show(HWND hWnd, int x, int y);

    // Exposed for reuse and testing: build the shared menu, or route one id to
    // its owner. Show() is these two around a TrackPopupMenu call.
    HMENU Build(HWND hWnd);
    void  Dispatch(HWND hWnd, int id);

} // namespace UI::AppMenu
