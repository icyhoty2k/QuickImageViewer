#pragma once
#include "../IPanelWindow.h"
#include <windows.h>

namespace UI {
    // Intermediate base class for all floating info-style panels.
    //
    // Key behaviour (automatic for every subclass):
    //   WM_KEYDOWN is intercepted here and routed through OnKeyDown().
    //   If OnKeyDown() returns false the key is forwarded to the parent window
    //   (main app command pipeline) via PostMessageW — so pressing J/K/M/Tab/…
    //   while any panel has focus reaches the app's toggle/navigation commands.
    //   New panels get this for free without writing any keyboard code.
    class FloatingPanelWnd : public IPanelWindow {
    protected:
        // Called for every WM_KEYDOWN before the message reaches HandlePanelMessage.
        // Return true  → key was consumed by the panel (not forwarded).
        // Return false → key is forwarded to the parent (default: all keys forwarded).
        virtual bool OnKeyDown(WPARAM /*vk*/, bool /*ctrl*/, bool /*shift*/, bool /*alt*/) {
            return false;
        }

        // Creates the floating window and applies all common DWM / layered attrs.
        // pixelW / pixelH must already be DPI-scaled by the caller.
        void InitFloating(HINSTANCE hInstance, HWND hParent,
                          LPCWSTR className, LPCWSTR title,
                          int pixelW, int pixelH,
                          UINT classStyle = 0);

        COLORREF GetBgColor() const;

        virtual void OnSetFocus()  {}
        virtual void OnKillFocus() {}

        virtual LRESULT HandlePanelMessage(UINT message, WPARAM wParam, LPARAM lParam) = 0;

    private:
        LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) override;
    };
} // namespace UI
