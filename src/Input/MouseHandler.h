#pragma once
#include <windows.h>
#include "../Platform/Constants.h"
#include "../../resources/resource.h"

namespace MouseHandler {

    // Cached cursor handles — loaded once at startup via LoadCursors().
    inline HCURSOR g_hZoomCursor = nullptr; // magnifier (from embedded IDC_ZOOM_CURSOR resource)
    inline HCURSOR g_hPanCursor  = nullptr; // hand
    inline HCURSOR g_hDefault    = nullptr; // arrow

    // Call once after the main window is created so hInstance is valid.
    inline void LoadCursors(HINSTANCE hInstance) {
        g_hZoomCursor = LoadCursorW(hInstance, MAKEINTRESOURCEW(Constants::Cursors::LMB_ZOOM_RESOURCE_ID));
        if (!g_hZoomCursor) // fallback if resource is missing
            g_hZoomCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32515)); // IDC_CROSS
        WORD panId = Constants::Cursors::LMB_PAN;
        g_hPanCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(panId));
        WORD defId = Constants::Cursors::DEFAULT;
        g_hDefault   = LoadCursorW(nullptr, MAKEINTRESOURCEW(defId));
    }

    // Set zoom or pan cursor based on whether the image already overflows the viewport.
    inline void SetLmbCursor(bool imageOverflows) {
        SetCursor(imageOverflows ? g_hPanCursor : g_hZoomCursor);
    }

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
