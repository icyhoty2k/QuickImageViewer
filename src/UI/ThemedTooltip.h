#pragma once
//
// ThemedTooltip — one themed hover popup, reusable by any panel.
//
// WHY NOT TOOLTIPS_CLASS: the common-controls tooltip paints itself from the
// system theme, so it comes out white-on-light while qIV is dark. There is no
// supported way to recolour it — TTM_SETTIPBKCOLOR / TTM_SETTIPTEXTCOLOR are
// ignored once visual styles are active, and subclassing it to fight its
// custom-draw is more code than owning the window outright.
//
// So this is a plain WS_POPUP window that draws its own text with the same
// Constants::Theme colours as everything else, and follows the theme when it
// changes — the same reasoning that produced ThemedDialog for message boxes.
//
// USAGE — all-static, one popup for the whole app; it is a transient:
//     UI::ThemedTooltip::Show(hOwner, L"line one\nline two", ptScreen);
//     UI::ThemedTooltip::Hide();
//     UI::ThemedTooltip::Destroy();     // from the owner's WM_DESTROY
//
// Text is plain; "\n" separates lines. The window sizes itself to its content,
// caps its width so long paths wrap instead of running off the monitor, and
// flips above the cursor when it would spill past the bottom of the screen.
//
// It never activates and is hit-test transparent, so showing one cannot steal
// focus from the panel underneath or block the WM_MOUSEMOVE that dismisses it.

#include <windows.h>
#include <string>

namespace UI {

    class ThemedTooltip {
        public:
            // Shows (or moves) the popup with its top-left at ptScreen — callers
            // usually offset that clear of the cursor. Empty text hides it.
            //
            // anchorScreen is the SCREEN rect the popup belongs to — the glyph,
            // cell or label being explained. The popup polls the cursor and hides
            // itself as soon as it leaves that rect.
            //
            // Self-dismissal lives here on purpose. Leaving it to each caller's
            // WM_MOUSEMOVE / WM_MOUSELEAVE meant every hover target had to get the
            // bookkeeping right, and one that sat against a window edge — a footer
            // total, say — could be left behind when the cursor exited the window
            // without a final mouse-move. One rule, one place, same behaviour for
            // every popup in the app.
            //
            // hOwner is the window being described, and it OWNS the popup for the
            // duration: ownership is re-pointed on every call. That matters
            // because the floating panels are HWND_TOPMOST — a popup owned by some
            // other window is not reliably drawn above them, and would flicker
            // behind the very panel it is explaining.
            //
            // Windows destroys owned windows with their owner, so closing that
            // panel takes the popup with it. Show() notices and rebuilds — once
            // per panel lifetime, not per hover.
            static void Show(HWND hOwner, const std::wstring &text, POINT ptScreen,
                             const RECT &anchorScreen);

            // True while a popup is on screen — lets a caller skip rebuilding text
            // it has already shown.
            static bool IsVisible();

            static void Hide();

            // Optional. The caches detect their own staleness, so this only forces
            // an immediate repaint if a popup is on screen when the theme changes.
            // Nothing breaks if it is never called.
            static void ApplyTheme();

            // Shutdown only. Panels must NOT call this — the popup outlives them
            // by design and is reused; tearing it down per panel would mean
            // rebuilding a window that is shown on almost every mouse move.
            static void Destroy();

        private:
            static const wchar_t *ClassName();
            static void EnsureClass();
            static void EnsureFont();
            static HBRUSH EnsureBrush(HBRUSH &brush, COLORREF &cached, COLORREF wanted);
            static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

            static constexpr UINT_PTR TIMER_ID = 1;
            static constexpr UINT     POLL_MS  = 100;

            static HWND         s_hwnd;   // the reused popup
            static HWND         s_hOwner; // window it is currently owned by
            static RECT         s_anchor; // screen rect the popup is tied to

            // Cached GDI objects. Each brush remembers the colour it was built
            // with and the font the DPI it was built for, so a paint reuses them
            // and only rebuilds when the value actually changed. That keeps the
            // steady state allocation-free without needing a theme-change hook to
            // invalidate anything — the cache validates itself.
            static HFONT    s_font;
            static int      s_fontDpi;
            static HBRUSH   s_bgBrush;
            static COLORREF s_bgColor;
            static HBRUSH   s_borderBrush;
            static COLORREF s_borderColor;

            static std::wstring s_text;
    };

} // namespace UI
