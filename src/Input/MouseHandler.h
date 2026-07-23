#pragma once
#include <windows.h>
#include "../Platform/Constants.h"
#include "../AppState.h"

namespace MouseHandler {
    // Decision logic
    inline bool IsDragAction(UINT message) {
        if (app.swapMouseButtons) return (message == WM_RBUTTONDOWN || message == WM_RBUTTONUP);
        return (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP);
    }

    inline bool IsViewControlAction(UINT message) {
        if (app.swapMouseButtons) return (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP);
        return (message == WM_RBUTTONDOWN || message == WM_RBUTTONUP);
    }

    //Mouse Handlers

    void HandleButtonDown(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);

    void HandleButtonUp(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);

    void HandleMouseMove(HWND hWnd, LPARAM lParam);

    // Call from WM_SETCURSOR and WM_MOUSEMOVE to update cursor based on
    // whether the rendered image overflows the viewport or hits an overlay path.
    bool UpdateHoverCursor(HWND hWnd);

    void HandleMouseWheel(HWND hWnd, WPARAM wParam, LPARAM lParam);

    void HandleMouseHWheel(HWND hWnd, WPARAM wParam, LPARAM lParam);

    void HandleDoubleClick(HWND hWnd);
}
