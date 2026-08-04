#pragma once
#include <windows.h>
#include <algorithm>

#include "Platform/Constants.h"
#include "Platform/ConstantsTheme.h"   // the one themed scrollbar palette
#include "UI/GdiPool.h"   // pooled brushes — never DeleteObject them

// =============================================================================
// ScrollView — a scrolled region and its scrollbars, both axes.
//
// Rectangles and two offsets. Deliberately NOT a window: every panel in this app
// paints its own rows into a back buffer, and a real child control would bring
// its own HWND, its own message loop and its own theming to a surface that is
// already fully custom-drawn.
//
// WHY IT IS SHARED. This began as a private nested struct in RemoteCmdWnd, which
// was right while one panel scrolled. The moment a second one did, the choice
// was one copy or two, and two copies of thumb arithmetic drift silently — the
// bug being a thumb that no longer lands where the rows are, which nothing
// checks and nobody notices until they try to grab it.
//
// -----------------------------------------------------------------------------
// BOTH AXES, AND WHY THE HORIZONTAL ONE IS NOT AN AFTERTHOUGHT
//
// Mice with a second wheel are ordinary now (Logitech's MX series, most
// trackballs, every trackpad), and a tilt or thumb wheel sends WM_MOUSEHWHEEL.
// A panel that handles only WM_MOUSEWHEEL does not merely lack a feature: the
// horizontal wheel falls through to DefWindowProc and the window under the
// cursor does nothing at all, which reads as the panel being frozen.
//
// A view that scrolls in one direction only leaves the other axis at zero —
// contentW (or contentH) of 0 means MaxScroll is 0, every scroll clamps to
// nothing, and no bar is drawn. Nothing has to opt out.
//
// THE TWO WHEELS DISAGREE ABOUT SIGN, and it is not a mistake in one of them:
//
//   WM_MOUSEWHEEL   positive = rotated AWAY from the user = scroll UP   = scrollY DECREASES
//   WM_MOUSEHWHEEL  positive = tilted RIGHT               = scroll RIGHT = scrollX INCREASES
//
// So the vertical helper negates and the horizontal one does not. Getting this
// wrong gives a wheel that works but backwards, which is the kind of bug that
// survives review because the code looks symmetric. It is written once here.
//
// The two also read DIFFERENT system settings — lines for vertical, characters
// for horizontal — and Windows lets the user set them independently.
//
// -----------------------------------------------------------------------------
// HOW A CALLER USES IT, in paint order:
//
//   1. set contentH (and contentW, if it scrolls sideways)
//   2. work out which bars are needed — see NeedsV / NeedsH
//   3. set `view` to the content area, narrowed/shortened by whichever bars
//      those are
//   4. Clamp()                      — shorter content must not leave a stale offset
//   5. clip to `view`, draw from scrollX / scrollY
//   6. set the tracks and call DrawScrollBarV / DrawScrollBarH, or clear the
//      rects when a bar is not shown
//
// Step 4 is the one that matters and the easy one to skip: content shrinks for
// reasons outside the panel's control — a client disconnects, a filter narrows —
// and an offset past the new end paints an empty view that looks like a bug.
//
// Step 6's "or clear" matters too: the track rects ARE the hit boxes, so a stale
// one left behind is a strip of panel that swallows clicks.
// =============================================================================

namespace UI {

    // --- Wheel policy --------------------------------------------------------
    //
    // Every scrolling panel used to answer "how far is one notch" for itself,
    // and they answered differently: 30px, 40px, 48px, 60px, one row, three
    // rows. Nothing was wrong with any single number — they were six unrelated
    // decisions, so the same flick moved a different distance in every window.
    //
    // These ASK WINDOWS, because the user already answered in Mouse settings and
    // no panel is better placed to overrule them.

    namespace detail {
        // Shared by both axes: the same three degenerate values come back from
        // either query and each breaks the arithmetic differently.
        //
        //   WHEEL_PAGESCROLL (0xFFFFFFFF)  "one screen at a time". Cast to int
        //                                  that is -1, which does not scroll a
        //                                  page — it scrolls one unit the WRONG
        //                                  WAY. Mapped to a large count instead;
        //                                  every view here is short enough that a
        //                                  screenful clamps out.
        //   0                              "no scrolling". Honoured literally the
        //                                  wheel would be dead in every panel
        //                                  while the rest of Windows still
        //                                  scrolls, which reads as a broken app.
        //   absurdly large                 nothing sets it, but the multiply
        //                                  would overflow before any clamp.
        //
        // The API can also fail and leave the variable untouched, so it is
        // seeded rather than assumed written.
        inline int WheelUnits(UINT which, int fallback) {
            UINT n = static_cast<UINT>(fallback);
            if (!SystemParametersInfoW(which, 0, &n, 0)) return fallback;
            if (n == WHEEL_PAGESCROLL) return 32;
            if (n == 0) return fallback;
            return static_cast<int>(std::min<UINT>(n, 32));
        }
    }

    // Lines per vertical notch. Default 3, as Windows itself defaults.
    inline int WheelLines() { return detail::WheelUnits(SPI_GETWHEELSCROLLLINES, 3); }

    // Characters per horizontal notch. Its own setting, and its own default.
    inline int WheelChars() { return detail::WheelUnits(SPI_GETWHEELSCROLLCHARS, 3); }

    // SHIFT IS THE ACCELERATOR, everywhere in this app.
    //
    // Read from the key state here rather than from the message's wParam: the
    // wheel messages do carry MK_SHIFT, but the thumbnail strips have always
    // read the key state directly, and one rule read two ways eventually
    // disagrees — a strip and a list would boost on different conditions.
    //
    // NOT "Shift means scroll sideways", which is the other convention some
    // apps use. This app has a second wheel for sideways, and a modifier that
    // silently changes WHICH AXIS moves is a worse trade than one that changes
    // how far: the horizontal wheel is unambiguous, and Shift stays useful on
    // hardware that has no second wheel.
    inline bool WheelBoostHeld() { return (GetKeyState(VK_SHIFT) & 0x8000) != 0; }

    inline int WheelBoost() {
        return WheelBoostHeld() ? Constants::WHEEL_SHIFT_ACCELERATOR : 1;
    }

    // WM_MOUSEWHEEL → pixels to ADD to scrollY. Negated: see the sign note above.
    //
    // Takes the whole wParam rather than a notch count so the accumulated-delta
    // case cannot be dropped by a caller — a signature handing out `delta > 0`
    // invites exactly that, and StatsWnd had it.
    inline int WheelDeltaToPixels(WPARAM wParam, int linePx) {
        const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        return -(delta * WheelLines() * linePx * WheelBoost()) / WHEEL_DELTA;
    }

    // WM_MOUSEHWHEEL → pixels to ADD to scrollX. NOT negated: see the sign note.
    inline int HWheelDeltaToPixels(WPARAM wParam, int charPx) {
        const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        return (delta * WheelChars() * charPx * WheelBoost()) / WHEEL_DELTA;
    }

    // --- The view ------------------------------------------------------------

    struct ScrollView {
        RECT view{};        // where content is drawn, bars already excluded

        int  scrollX  = 0;
        int  scrollY  = 0;
        int  contentW = 0;  // 0 = does not scroll sideways; no bar, no movement
        int  contentH = 0;

        // Empty when that bar is not shown — which is also how the hit tests
        // know there is nothing there to grab.
        RECT vTrack{}, vThumb{};  bool vThumbHot = false;
        RECT hTrack{}, hThumb{};  bool hThumbHot = false;

        int Width()  const { return view.right - view.left; }
        int Height() const { return view.bottom - view.top; }

        int MaxScrollX() const { return std::max(0, contentW - Width());  }
        int MaxScrollY() const { return std::max(0, contentH - Height()); }

        // Whether each bar is needed, against a region of the given size.
        //
        // ASKED BEFORE `view` IS SET, because the answer decides how much room
        // the content actually gets.
        //
        // TWO PASSES, and the second one is not pedantry: each bar steals from
        // the OTHER axis, so content that fits with no bars can need one once
        // the other appears. A list one pixel narrower than its window, with
        // enough rows to need a vertical bar, loses `barPx` of width to that bar
        // and now overflows horizontally too. One pass says "no horizontal bar",
        // draws the vertical one, and the last column is silently unreachable.
        //
        // Settles after two rounds by construction: adding a bar only ever
        // shrinks the other axis, so once both answers are computed against the
        // shrunken sizes neither can flip back.
        void ResolveBars(int regionW, int regionH, int barPx,
                         bool &needVOut, bool &needHOut) const {
            bool v = contentH > regionH;
            bool h = contentW > 0 && contentW > regionW;

            // Second round: re-ask each against the room the other one left.
            if (!h) h = contentW > 0 && contentW > (regionW - (v ? barPx : 0));
            if (!v) v = contentH > (regionH - (h ? barPx : 0));

            needVOut = v;
            needHOut = h;
        }

        // Single-axis shorthand, for the panels that only ever scroll one way.
        bool NeedsV(int regionH) const { return contentH > regionH; }
        bool NeedsH(int regionW) const { return contentW > 0 && contentW > regionW; }

        void Clamp() {
            scrollX = std::clamp(scrollX, 0, MaxScrollX());
            scrollY = std::clamp(scrollY, 0, MaxScrollY());
        }

        void ScrollBy(int dx, int dy) { scrollX += dx; scrollY += dy; Clamp(); }

        // Scrolls the smallest amount that brings a vertical span into view —
        // the keyboard's counterpart to the wheel. Nothing when it is already
        // there, so walking a selection through visible rows does not jump.
        void EnsureVisibleY(int top, int h) {
            if (top < scrollY)                     scrollY = top;
            else if (top + h > scrollY + Height()) scrollY = top + h - Height();
            Clamp();
        }

        void EnsureVisibleX(int left, int w) {
            if (left < scrollX)                    scrollX = left;
            else if (left + w > scrollX + Width()) scrollX = left + w - Width();
            Clamp();
        }

        // Where a drag that grabbed the thumb `grabPx` along its length should
        // put the offset, for a pointer now at the given client coordinate.
        // Here rather than in each panel's WM_MOUSEMOVE because it is the same
        // arithmetic and the same off-by-one every time.
        void DragToY(int pointerY, int grabPx) {
            const int travel = Height() - (vThumb.bottom - vThumb.top);
            if (travel <= 0) return;
            const int want = pointerY - vTrack.top - grabPx;
            scrollY = MulDiv(std::clamp(want, 0, travel), MaxScrollY(), travel);
            Clamp();
        }

        void DragToX(int pointerX, int grabPx) {
            const int travel = Width() - (hThumb.right - hThumb.left);
            if (travel <= 0) return;
            const int want = pointerX - hTrack.left - grabPx;
            scrollX = MulDiv(std::clamp(want, 0, travel), MaxScrollX(), travel);
            Clamp();
        }

        // Clears both bars' geometry. Call when nothing needs one, so no stale
        // rect is left hit-testing against a strip that is no longer a bar.
        void ClearBars() { vTrack = vThumb = hTrack = hThumb = RECT{}; }

        // Is that bar currently shown? An empty track IS the "no bar" state —
        // there is no separate flag to fall out of step with the rectangles.
        bool HasV() const { return vTrack.right > vTrack.left; }
        bool HasH() const { return hTrack.bottom > hTrack.top; }

        // =====================================================================
        // THE WHOLE LAYOUT DECISION, IN ONE CALL.
        //
        // Give it the full rectangle the content may occupy and the width of a
        // bar; it decides which bars are needed, sets `view` to what is left,
        // sets the track rects for the bars that ARE shown, clears the ones that
        // are not, and clamps the offsets.
        //
        // WHY THIS IS NOT LEFT TO THE PANELS. Every panel was writing the same
        // six lines and getting a different subset of them right: one reserved
        // the bar's column whether or not a bar was drawn, one left a stale
        // track rect behind so a strip of empty panel still swallowed clicks,
        // one never cleared the thumb. None of that is panel-specific — it is
        // the same question every time, and it now has one answer.
        //
        // AFTER THIS, `view` IS THE CLIP RECT and the tracks are the hit boxes.
        // A caller draws content clipped to `view`, then calls DrawBars.
        //
        // Both bars showing leaves the bottom-right corner unclaimed by either
        // track — neither runs into the other, so a click in the corner falls
        // through to the panel rather than paging a bar the pointer is not on.
        void Layout(const RECT &region, int barPx) {
            const int regionW = region.right - region.left;
            const int regionH = region.bottom - region.top;

            bool needV = false, needH = false;
            ResolveBars(regionW, regionH, barPx, needV, needH);

            view = {region.left, region.top,
                    region.right - (needV ? barPx : 0),
                    region.bottom - (needH ? barPx : 0)};

            ClearBars();
            // Each track spans only its own axis's view extent, so the two stop
            // short of each other at the corner.
            if (needV)
                vTrack = {view.right, view.top, view.right + barPx, view.bottom};
            if (needH)
                hTrack = {view.left, view.bottom, view.right, view.bottom + barPx};

            // LAST, because MaxScroll is measured against the view this call
            // just decided — clamping before it would use the old height and
            // leave an offset that is out of range for the layout being drawn.
            Clamp();
        }
    };

    // The three colours a bar needs. Grouped so a caller passes one thing and
    // cannot get the order wrong between the two draw calls — that mistake
    // shows up as a thumb that never highlights, which is easy to miss.
    struct ScrollBarColors {
        COLORREF track    = 0;
        COLORREF thumb    = 0;
        COLORREF thumbHot = 0;
    };

    // --- Geometry and palette, resolved here and nowhere else ----------------
    //
    // DIP constants live in Constants::Scrollbar and the theme levels in
    // Constants::Theme::Scrollbar; these three functions are what turn them into
    // the pixels and COLORREFs a panel draws with. A panel that reaches past
    // them is how a bar ends up a different size or colour from its neighbours,
    // which is the state this replaced.
    //
    // `dpiScale` and `themeFactor` are passed in rather than read from `app`, so
    // this header stays free of AppState — it is included by every panel.

    inline int ScrollBarThicknessPx(float dpiScale) {
        return static_cast<int>(Constants::Scrollbar::THICKNESS * dpiScale);
    }

    inline int ScrollBarMinThumbPx(float dpiScale) {
        return static_cast<int>(Constants::Scrollbar::MIN_THUMB * dpiScale);
    }

    inline ScrollBarColors ThemeScrollBarColors(float themeFactor) {
        namespace S = Constants::Theme::Scrollbar;
        return ScrollBarColors{
            Constants::Theme::ThemedColor(S::TRACK_R, S::TRACK_G, S::TRACK_B, themeFactor),
            Constants::Theme::ThemedColor(S::THUMB_R, S::THUMB_G, S::THUMB_B, themeFactor),
            Constants::Theme::ThemedColor(S::THUMB_HOT_R, S::THUMB_HOT_G,
                                          S::THUMB_HOT_B, themeFactor)};
    }

    // Paints a bar and RECOMPUTES its thumb as it goes — the hit box is
    // therefore always exactly what was drawn, which is the only way the two
    // cannot disagree.
    //
    // Call only when that bar is needed; its track must already be set.
    inline void DrawScrollBarV(HDC bb, ScrollView &sv, float s, COLORREF trackCol,
                               COLORREF thumbCol, COLORREF thumbHotCol, bool dragging) {
        FillRect(bb, &sv.vTrack, Gdi::Brush(trackCol));

        const int span = sv.Height();
        const int minT = ScrollBarMinThumbPx(s);
        // Floored so it stays grabbable: the proportional size against a long
        // list is a couple of pixels.
        int th = std::max(minT, MulDiv(span, span, std::max(1, sv.contentH)));
        th = std::min(th, span);

        const int travel = std::max(0, span - th);
        const int ty = sv.vTrack.top + (sv.MaxScrollY() > 0
                                            ? MulDiv(sv.scrollY, travel, sv.MaxScrollY())
                                            : 0);

        sv.vThumb = {sv.vTrack.left + static_cast<int>(2 * s), ty,
                     sv.vTrack.right - static_cast<int>(2 * s), ty + th};
        FillRect(bb, &sv.vThumb,
                 Gdi::Brush((sv.vThumbHot || dragging) ? thumbHotCol : thumbCol));
    }

    inline void DrawScrollBarH(HDC bb, ScrollView &sv, float s, COLORREF trackCol,
                               COLORREF thumbCol, COLORREF thumbHotCol, bool dragging) {
        FillRect(bb, &sv.hTrack, Gdi::Brush(trackCol));

        const int span = sv.Width();
        const int minT = ScrollBarMinThumbPx(s);
        int tw = std::max(minT, MulDiv(span, span, std::max(1, sv.contentW)));
        tw = std::min(tw, span);

        const int travel = std::max(0, span - tw);
        const int tx = sv.hTrack.left + (sv.MaxScrollX() > 0
                                             ? MulDiv(sv.scrollX, travel, sv.MaxScrollX())
                                             : 0);

        sv.hThumb = {tx, sv.hTrack.top + static_cast<int>(2 * s),
                     tx + tw, sv.hTrack.bottom - static_cast<int>(2 * s)};
        FillRect(bb, &sv.hThumb,
                 Gdi::Brush((sv.hThumbHot || dragging) ? thumbHotCol : thumbCol));
    }

    // Draws whichever bars Layout decided on — vertical only, horizontal only,
    // both, or neither. The companion to Layout: between them a panel never
    // writes "if it needs a bar" anywhere.
    //
    // Safe to call unconditionally. A bar that is not shown has an empty track
    // and is skipped, so there is no state for a caller to test first and no way
    // to draw a bar that Layout did not lay out.
    inline void DrawBars(HDC bb, ScrollView &sv, float s, const ScrollBarColors &c,
                         bool draggingV = false, bool draggingH = false) {
        if (sv.HasV())
            DrawScrollBarV(bb, sv, s, c.track, c.thumb, c.thumbHot, draggingV);
        if (sv.HasH())
            DrawScrollBarH(bb, sv, s, c.track, c.thumb, c.thumbHot, draggingH);
    }

} // namespace UI
