#pragma once
#include <windows.h>

// =============================================================================
// GdiPool — brushes and pens, created once and kept.
//
// THE PROBLEM. Every panel paints the same way: create a brush, fill, delete;
// create a pen, stroke, delete. Perfectly correct, and perfectly wasteful — the
// colours come from the theme, so a panel creates the SAME handful of objects on
// every repaint and throws them away again. A panel that repaints on mouse-move
// to track a hovered row does that for each row the pointer crosses: about
// thirty handles per paint, several hundred to drag down a list, for a highlight
// moving.
//
// GDI objects are also a process-wide resource with a 10,000-handle default
// quota. Churning them is not only slow, it is the shape of leak that only shows
// up after an hour of use.
//
// THE POOL. One entry per colour, created on first use and reused for the life
// of the theme. A dozen or so objects for the whole application, allocated once.
//
// -----------------------------------------------------------------------------
// CALLERS MUST NOT DELETE WHAT THEY GET BACK.
//
// That is the whole discipline, and the one way to get this wrong. A handle from
// here is owned by the pool and shared with every other caller that asked for
// the same colour — deleting it leaves every one of them holding a freed handle,
// and the failure appears later, somewhere else, as a panel that paints blank or
// takes the process down.
//
// Converting a call site means removing the DeleteObject, not just changing the
// creation:
//
//     HBRUSH b = CreateSolidBrush(c);          FillRect(dc, &r, Gdi::Brush(c));
//     FillRect(dc, &r, b);              →
//     DeleteObject(b);
//
// -----------------------------------------------------------------------------
// LIFETIME. Entries live until Flush(), which UIManager::NotifyThemeChanged
// calls — the colours are theme-derived, so a theme change is exactly when they
// stop being valid. Nothing else should call it.
//
// BOUNDED because the inputs are: panel colours come from Constants and the
// theme, a fixed set. This is not a general-purpose cache and must not be fed
// colours computed per pixel or per item — that would grow without limit.
//
// UI THREAD ONLY. GDI handles belong to the thread that creates them, and every
// panel paints on the UI thread. There is no lock here and there must not need
// to be one.
// =============================================================================

namespace UI::Gdi {

    // A solid brush of this colour. Owned by the pool — do not DeleteObject it.
    HBRUSH Brush(COLORREF colour);

    // A PS_SOLID pen. Owned by the pool — do not DeleteObject it.
    // Select it, use it, select the old object back; that is all.
    HPEN Pen(COLORREF colour, int width = 1);

    // Destroys every cached object. Called when the theme changes, because the
    // colours these were built from no longer apply.
    //
    // Safe only when nothing is mid-paint: a handle already selected into a DC
    // would be freed under it. Called from the UI thread between paints, which
    // is where NotifyThemeChanged runs.
    void Flush();

} // namespace UI::Gdi
