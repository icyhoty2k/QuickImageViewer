#pragma once
#include "IPanelWindow.h"
#include <windows.h>

namespace UI {
    // Intermediate base class for floating info-style panels (ExifWnd, HelpWnd,
    // HistoryListWnd).  Owns the creation boilerplate that all three share:
    //   - WS_EX_LAYERED + WS_CAPTION + WS_BORDER window style
    //   - SetLayeredWindowAttributes (THUMBNAIL_PANEL_WINDOW_OPACITY)
    //   - DWM dark-mode, corner preference, caption colour
    //   - WM_SETFOCUS / WM_KILLFOCUS → InvalidateRect (active/inactive repaint)
    //   - GetBgColor() — Panel::BACKGROUND_ACTIVE or INACTIVE per focus state
    //
    // Derived classes call InitFloating() from their Init() and implement
    // HandlePanelMessage() instead of HandleMessage().
    class FloatingPanelWnd : public IPanelWindow {
    protected:
        // Creates the floating window and applies all common DWM / layered
        // attributes.  pixelW / pixelH must already be DPI-scaled by the caller.
        // classStyle is ORed into the WNDCLASSW.style field (e.g. CS_DBLCLKS).
        void InitFloating(HINSTANCE hInstance, HWND hParent,
                          LPCWSTR className, LPCWSTR title,
                          int pixelW, int pixelH,
                          UINT classStyle = 0);

        // Returns a themed COLORREF for the window body background.
        // Switches between Panel::BACKGROUND_ACTIVE and INACTIVE based on focus.
        COLORREF GetBgColor() const;

        // Optional hooks — called by the base after InvalidateRect.
        virtual void OnSetFocus()  {}
        virtual void OnKillFocus() {}

        // Derived classes implement this instead of HandleMessage.
        virtual LRESULT HandlePanelMessage(UINT message, WPARAM wParam, LPARAM lParam) = 0;

    private:
        LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) override;
    };
} // namespace UI
