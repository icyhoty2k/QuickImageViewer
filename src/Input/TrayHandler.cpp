#include "TrayHandler.h"
#include "UI/AppMenu/AppMenu.h"

#include <windowsx.h>

namespace Input {

// =============================================================================
//  Entry point
// =============================================================================

LRESULT TrayHandler::Handle(HWND hWnd, WPARAM wParam, LPARAM lParam) {
    if (LOWORD(lParam) == WM_LBUTTONDBLCLK) {
        RestoreWindow(hWnd);
    } else if (LOWORD(lParam) == WM_RBUTTONUP) {
        // The same menu the main-window right-click shows — UI::AppMenu is the
        // single definition; this handler only supplies the anchor point.
        UI::AppMenu::Show(hWnd, GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam));
    }
    return 0;
}

// =============================================================================
//  Restore window (double-click)
//
// AttachThreadInput is the documented way to take the foreground from another
// process: Windows refuses SetForegroundWindow from a process that does not own
// it, and attaching to that thread's input queue is what lifts the refusal.
// Detached again immediately — leaving two input queues joined would make the
// two applications share focus and keyboard state.
// =============================================================================

void TrayHandler::RestoreWindow(HWND hWnd) {
    ShowWindow(hWnd, SW_SHOW);
    ShowWindow(hWnd, SW_RESTORE);

    HWND hForegroundWnd = GetForegroundWindow();
    if (hForegroundWnd && hForegroundWnd != hWnd) {
        DWORD foregroundThreadId = GetWindowThreadProcessId(hForegroundWnd, nullptr);
        DWORD currentThreadId    = GetCurrentThreadId();

        AttachThreadInput(foregroundThreadId, currentThreadId, TRUE);
        SetForegroundWindow(hWnd);
        SetWindowPos(hWnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        SetActiveWindow(hWnd);
        SetFocus(hWnd);
        AttachThreadInput(foregroundThreadId, currentThreadId, FALSE);
    } else {
        SetForegroundWindow(hWnd);
    }
}

} // namespace Input
