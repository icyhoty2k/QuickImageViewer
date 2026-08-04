#pragma once
#include "../IPanelWindow.h"
#include "UI/CustomControls/ScrollView.h"
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

        // Called on WM_MBUTTONUP before the base hides the panel.
        // Return true  → event consumed by the panel; base does NOT hide.
        // Return false → base hides the panel after this returns (default).
        virtual bool OnMButtonUp(int /*x*/, int /*y*/) { return false; }

        // =====================================================================
        // SCROLLING, OWNED BY THE BASE.
        //
        // A panel that scrolls returns its ScrollView from ScrollViewAt() and
        // gets ALL of this for free, with no message handling of its own:
        //
        //   • both wheels, with the shared step, Shift accelerator and the two
        //     wheels' opposite sign conventions
        //   • thumb drag, with SetCapture and a WM_CAPTURECHANGED release
        //   • track click paging
        //   • the hand cursor over either bar
        //
        // WHY HERE AND NOT IN EACH PANEL. Every one of those was written per
        // panel and every one of them drifted: six wheel speeds, one panel whose
        // bar was decoration you could not grab, one whose drag never took
        // capture (so releasing outside the window left it dragging for ever),
        // and one whose track showed an arrow while the rest showed a hand. None
        // of that is panel-specific behaviour — it is scrollbar behaviour, and
        // it now exists once.
        //
        // A panel that does not scroll returns nullptr and is unaffected, except
        // that the horizontal wheel is still swallowed for it — see the note in
        // HandleMessage.
        // =====================================================================

        // The view under `pt` (client coordinates), or nullptr when the panel
        // does not scroll there. `pt` matters only for panels with more than one
        // scrolled region; single-view panels ignore it and return their one.
        virtual ScrollView *ScrollViewAt(POINT /*pt*/) { return nullptr; }

        // What one wheel "line" means for that view, in pixels — a row height
        // for a list, a text line for a document. Zero disables wheel scrolling
        // while still allowing the bars to be dragged.
        virtual int ScrollLinePx(const ScrollView & /*sv*/) const { return 0; }

        // Called after the base changes a view's offset, so the panel can
        // repaint. Default invalidates, which is what every panel does.
        virtual void OnScrolled();

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

        // Handles the scroll-related messages against ScrollViewAt(). Returns
        // true when the message was consumed and must not reach the panel.
        bool HandleScrollMessage(UINT message, WPARAM wParam, LPARAM lParam,
                                 LRESULT &resultOut);

        // The view being dragged, and where inside its thumb it was grabbed.
        // A raw pointer is safe because a drag cannot outlive the panel, and
        // capture is released on WM_CAPTURECHANGED however the drag ends.
        //
        // PREFIXED, and not for tidiness. These are private members of a base
        // that every panel inherits, and several panels had their own
        // `m_dragGrabPx` for the same purpose. A derived class that deletes its
        // copy mid-migration silently resolves the name to THIS one and reports
        // "cannot access private member" from a line that looks unrelated —
        // which is exactly how a half-finished migration reads as a base-class
        // fault. A name nothing else uses cannot be picked up by accident.
        ScrollView *m_scrollDragView   = nullptr;
        bool        m_scrollDragHoriz  = false;
        int         m_scrollDragGrabPx = 0;
    };
} // namespace UI
