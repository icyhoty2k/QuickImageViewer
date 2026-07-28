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

private:
    void RestoreWindow(HWND hWnd);
};

} // namespace Input
