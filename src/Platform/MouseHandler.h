#pragma once
#include <windows.h>
#include "Constants.h"

namespace MouseHandler {
    // Decision logic
    inline bool IsDragAction(UINT message) {
        if (Constants::SWAP_MOUSE_BUTTONS) return (message == WM_RBUTTONDOWN || message == WM_RBUTTONUP);
        return (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP);
    }

    inline bool IsViewControlAction(UINT message) {
        if (Constants::SWAP_MOUSE_BUTTONS) return (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP);
        return (message == WM_RBUTTONDOWN || message == WM_RBUTTONUP);
    }

    // Handlers
    void HandleButtonDown(HWND hWnd, UINT message, LPARAM lParam);

    void HandleButtonUp(HWND hWnd, UINT message, LPARAM lParam);

    void HandleMouseMove(HWND hWnd, LPARAM lParam);

    void HandleMouseWheel(HWND hWnd, WPARAM wParam, LPARAM lParam);
}
