#pragma once
#include <windows.h>
#include <windowsx.h>
#include <string>
#include <functional>
#include <algorithm>
#include "AppState.h"
#include "Platform/Constants.h"
#include "Platform/ConstantsTheme.h"

extern AppState app;

namespace UI {

// Result of routing one input message into an InputBox. Unambiguous — the host
// branches on exactly one meaning, never guesses "consumed vs. needs repaint".
//   Ignored         — not the box's key; host should forward it to the app pipeline.
//   Consumed        — handled; no visual change.
//   ConsumedRepaint — handled AND the host must InvalidateRect.
//   RequestClear    — Esc with text present: the box cleared itself (same as the
//                     ✕ button); host repaints and stays open.
//   RequestClose    — Esc with empty text: host should hide/close the panel.
enum class InputResult { Ignored, Consumed, ConsumedRepaint, RequestClear, RequestClose };

// Self-contained, windowless single-line text control.
//
// Owns its text buffer, caret, selection, horizontal scroll, clipboard, and
// drawing (themed border, text, ✕ clear button). It is a COMPLETE editor:
//   • click to position the caret
//   • horizontal scroll-to-caret for text longer than the box
//   • selection + Ctrl+A/C/X/V, Home/End/arrows, backspace/delete
//
// A host wires up:
//   1. OnChanged callback — what to do when text changes
//   2. Draw() from WM_PAINT
//   3. Route*() from WM_CHAR / WM_KEYDOWN / WM_LBUTTONDOWN / WM_MOUSEMOVE /
//      WM_MOUSELEAVE, branching on the returned InputResult (see enum above).
//
// STANDARD HOST IDIOM (copy verbatim; only add genuine per-panel special keys
// BEFORE the RouteKey call — e.g. digit-only filters, list navigation):
//
//   // WM_KEYDOWN → OnKeyDown:
//   switch (m_box.RouteKey(vk, m_hWnd)) {
//       case InputResult::Ignored:         return false;                 // forward to app
//       case InputResult::RequestClose:    return false;                 // base hides panel
//       case InputResult::RequestClear:    InvalidateRect(m_hWnd,0,0); return true;
//       case InputResult::ConsumedRepaint: InvalidateRect(m_hWnd,0,0); return true;
//       case InputResult::Consumed:        return true;
//   }
//   // WM_CHAR:          if (m_box.RouteChar(ch,m_hWnd)==InputResult::ConsumedRepaint) InvalidateRect(...);
//   // WM_LBUTTONDOWN/…: if (m_box.RouteMouse(msg,wParam,lParam)==InputResult::ConsumedRepaint) InvalidateRect(...);
//   // OnLocalHide (Esc): RouteKey(VK_ESCAPE,…) → RequestClear (stay, repaint) or RequestClose (hide).
class InputBox {
public:
    std::function<void(const std::wstring&)> OnChanged;

    void SetPlaceholder(const wchar_t* text) { m_placeholder = text ? text : L""; }
    void SetMaxLength(int n)                 { m_maxLen = n; }

    const std::wstring& GetText()      const { return m_text; }
    bool                IsEmpty()      const { return m_text.empty(); }
    const RECT&         GetClearRect() const { return m_clearRect; }

    // Set text and notify — use for external programmatic changes.
    void SetText(const std::wstring& t) { m_text = t; m_caretPos = (int)t.size(); m_selAnchor = -1; notify(); }

    // Clear text and notify — use for ESC / programmatic clear / the ✕ button.
    void Clear()  { m_text.clear(); m_caretPos = 0; m_selAnchor = -1; m_scrollX = 0; m_clearRect = {}; m_clearHovered = false; notify(); }

    // Clear text silently — use in Show() where the caller drives layout itself.
    void Reset()  { m_text.clear(); m_caretPos = 0; m_selAnchor = -1; m_scrollX = 0; m_clearRect = {}; m_clearHovered = false; }

    // Draw the control into hdc. Records geometry (text rect, font) for click
    // hit-testing and updates the horizontal scroll so the caret stays visible.
    // hasFocus: pass GetFocus() == m_hWnd from the owner window.
    // isError:  red color variant (JumpTo out-of-range state).
    // dtFlags:  DT_LEFT or DT_CENTER (vertical centering is applied internally).
    void Draw(HDC hdc, HFONT hFont, const RECT& boxRect, int padX,
              bool hasFocus, bool isError = false,
              UINT dtFlags = DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // -----------------------------------------------------------------------
    // Routing — one entry per Win32 input message, each returning an InputResult.
    // Hosts pre-filter genuine per-panel keys/chars BEFORE delegating here.
    // -----------------------------------------------------------------------
    InputResult RouteKey(WPARAM vk, HWND host);
    InputResult RouteChar(wchar_t ch, HWND host);
    InputResult RouteMouse(UINT msg, WPARAM wParam, LPARAM lParam);

    // Keys that are never text input and must always reach the host's parent,
    // even while the box has focus: function keys (F1–F24) and bare modifier
    // presses are global app shortcuts, not editor input. RouteKey returns
    // Ignored for these; kept public so hosts can classify keys before RouteKey
    // if they need to.
    static bool IsForwardableShortcutKey(WPARAM vk) {
        return (vk >= VK_F1 && vk <= VK_F24) ||
               vk == VK_SHIFT   || vk == VK_LSHIFT   || vk == VK_RSHIFT ||
               vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL ||
               vk == VK_MENU    || vk == VK_LMENU    || vk == VK_RMENU;
    }

private:
    std::wstring m_text;
    std::wstring m_placeholder;
    RECT         m_clearRect    = {};
    RECT         m_textRect     = {};    // last-drawn text area — click hit-testing
    HFONT        m_lastFont     = nullptr; // last-drawn font — click hit-testing
    int          m_maxLen       = 0;     // 0 = unlimited
    int          m_caretPos     = 0;     // insertion point within m_text [0, size]
    int          m_selAnchor    = -1;    // selection anchor; -1 = no selection
    int          m_scrollX      = 0;     // horizontal scroll offset in pixels
    bool         m_clearHovered = false;

    void notify() { if (OnChanged) OnChanged(m_text); }

    bool HasSelection() const { return m_selAnchor >= 0 && m_selAnchor != m_caretPos; }
    int  SelMin()      const  { return m_selAnchor < m_caretPos ? m_selAnchor : m_caretPos; }
    int  SelMax()      const  { return m_selAnchor > m_caretPos ? m_selAnchor : m_caretPos; }

    void DeleteSelection() {
        const int lo = SelMin(), hi = SelMax();
        m_text.erase(lo, hi - lo);
        m_caretPos  = lo;
        m_selAnchor = -1;
    }

    // Pixel width of m_text[0..count) using hdc's currently-selected font.
    static int PrefixWidth(HDC hdc, const std::wstring& s, int count) {
        SIZE sz{};
        GetTextExtentPoint32W(hdc, s.c_str(), count, &sz);
        return sz.cx;
    }

    // Map a client-x pixel to a caret index using the last-drawn font + geometry.
    int CaretFromX(int clientX) const {
        if (!m_lastFont || m_text.empty()) return 0;
        HDC hdc = GetDC(nullptr);
        HGDIOBJ old = SelectObject(hdc, m_lastFont);
        int target = clientX - m_textRect.left + m_scrollX;
        if (target < 0) target = 0;
        int fit = 0;
        SIZE sz{};
        GetTextExtentExPointW(hdc, m_text.c_str(), (int)m_text.size(),
                              target, &fit, nullptr, &sz);
        SelectObject(hdc, old);
        ReleaseDC(nullptr, hdc);
        if (fit < 0) fit = 0;
        if (fit > (int)m_text.size()) fit = (int)m_text.size();
        return fit;
    }

    static void CopyToClipboard(const std::wstring& text) {
        if (!OpenClipboard(nullptr)) return;
        EmptyClipboard();
        const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (hg) {
            void* p = GlobalLock(hg);
            if (p) { memcpy(p, text.c_str(), bytes); GlobalUnlock(hg); }
            SetClipboardData(CF_UNICODETEXT, hg);
        }
        CloseClipboard();
    }

    static std::wstring GetClipboardText() {
        std::wstring result;
        if (!OpenClipboard(nullptr)) return result;
        HANDLE h = GetClipboardData(CF_UNICODETEXT);
        if (h) {
            const wchar_t* p = static_cast<const wchar_t*>(GlobalLock(h));
            if (p) { result = p; GlobalUnlock(h); }
        }
        CloseClipboard();
        return result;
    }
};

// ---------------------------------------------------------------------------
inline void InputBox::Draw(HDC hdc, HFONT hFont, const RECT& boxRect, int padX,
                            bool hasFocus, bool isError, UINT dtFlags)
{
    const float tf      = app.themeFactor;
    const bool  hasText = !m_text.empty();
    m_caretPos = std::min(m_caretPos, (int)m_text.size()); // guard against stale pos

    const COLORREF bg = isError
        ? Constants::Theme::ThemedColor(0.35f, 0.06f, 0.06f, tf)
        : Constants::Theme::ThemedGray (0.14f, tf);
    const COLORREF border = isError
        ? Constants::Theme::ThemedColor(0.80f, 0.25f, 0.25f, tf)
        : Constants::Theme::ThemedGray (0.35f, tf);
    const COLORREF colorActive   = isError
        ? Constants::Theme::ThemedColor(1.00f, 0.40f, 0.40f, tf)
        : Constants::Theme::ThemedColor(0.39f, 0.78f, 1.00f, tf);
    const COLORREF colorInactive  = Constants::Theme::ThemedGray(0.45f, tf);
    const COLORREF colorClear     = Constants::Theme::ThemedGray(0.50f, tf);
    const COLORREF colorClearHover =
        Constants::Theme::ThemedColor(0.941f, 0.196f, 0.196f, tf);

    // Background
    HBRUSH hBg = CreateSolidBrush(bg);
    FillRect(hdc, &boxRect, hBg);
    DeleteObject(hBg);

    // Border
    HPEN   hPen  = CreatePen(PS_SOLID, 1, border);
    HPEN   hOldP = (HPEN)  SelectObject(hdc, hPen);
    HBRUSH hNul  = (HBRUSH)GetStockObject(NULL_BRUSH);
    HBRUSH hOldB = (HBRUSH)SelectObject(hdc, hNul);
    Rectangle(hdc, boxRect.left, boxRect.top, boxRect.right, boxRect.bottom);
    SelectObject(hdc, hOldP);
    SelectObject(hdc, hOldB);
    DeleteObject(hPen);

    SelectObject(hdc, hFont);
    SetBkMode(hdc, TRANSPARENT);

    // ✕ clear button — square on the right, only when text is present
    const int clearW = hasText ? (boxRect.bottom - boxRect.top) : 0;
    if (hasText) {
        m_clearRect = { boxRect.right - clearW, boxRect.top,
                        boxRect.right,          boxRect.bottom };
        RECT cr = m_clearRect;
        SetTextColor(hdc, m_clearHovered ? colorClearHover : colorClear);
        DrawTextW(hdc, L"✕", -1, &cr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else {
        m_clearRect    = {};
        m_clearHovered = false;
    }

    // Text area (padded from left, does not overlap ✕). Cached for hit-testing.
    RECT textRect = { boxRect.left + padX, boxRect.top,
                      boxRect.right - padX - clearW, boxRect.bottom };
    m_textRect = textRect;
    m_lastFont = hFont;

    TEXTMETRICW tm{};
    GetTextMetricsW(hdc, &tm);
    const int ty = textRect.top + (textRect.bottom - textRect.top - tm.tmHeight) / 2;
    const int visW = std::max(0, (int)(textRect.right - textRect.left));

    if (hasText) {
        // --- Horizontal scroll: keep the caret inside the visible text area ---
        const int caretPx = PrefixWidth(hdc, m_text, m_caretPos);
        const int totalPx = PrefixWidth(hdc, m_text, (int)m_text.size());
        if (caretPx - m_scrollX < 0)          m_scrollX = caretPx;          // caret left of view
        else if (caretPx - m_scrollX > visW)  m_scrollX = caretPx - visW;   // caret right of view
        if (totalPx <= visW)                  m_scrollX = 0;                 // whole text fits
        else if (m_scrollX > totalPx - visW)  m_scrollX = totalPx - visW;   // no empty tail gap
        if (m_scrollX < 0)                    m_scrollX = 0;

        const int baseX = textRect.left - m_scrollX;

        // Selection background
        if (HasSelection()) {
            const int lo = SelMin(), hi = SelMax();
            int xa = baseX + PrefixWidth(hdc, m_text, lo);
            int xb = baseX + PrefixWidth(hdc, m_text, hi);
            if (xa < textRect.left)  xa = textRect.left;
            if (xb > textRect.right) xb = textRect.right;
            if (xb > xa) {
                HBRUSH hSel = CreateSolidBrush(GetSysColor(COLOR_HIGHLIGHT));
                RECT sr = { xa, textRect.top, xb, textRect.bottom };
                FillRect(hdc, &sr, hSel);
                DeleteObject(hSel);
            }
        }

        // Whole text in the active color, clipped to the text area.
        SetTextColor(hdc, colorActive);
        ExtTextOutW(hdc, baseX, ty, ETO_CLIPPED, &textRect,
                    m_text.c_str(), (int)m_text.size(), nullptr);

        // Re-draw the selected run in the highlight-text color over the bg.
        if (HasSelection()) {
            const int lo = SelMin(), hi = SelMax();
            SetTextColor(hdc, GetSysColor(COLOR_HIGHLIGHTTEXT));
            const int xlo = baseX + PrefixWidth(hdc, m_text, lo);
            ExtTextOutW(hdc, xlo, ty, ETO_CLIPPED, &textRect,
                        m_text.c_str() + lo, hi - lo, nullptr);
        }

        // Caret — a thin vertical bar, only while focused.
        if (hasFocus) {
            const int cx = baseX + caretPx;
            if (cx >= textRect.left && cx <= textRect.right) {
                RECT caret = { cx, textRect.top + 2, cx + 1, textRect.bottom - 2 };
                HBRUSH hCaret = CreateSolidBrush(colorActive);
                FillRect(hdc, &caret, hCaret);
                DeleteObject(hCaret);
            }
        }
    } else {
        m_scrollX = 0;
        if (hasFocus) {
            // Empty + focused: draw the caret at the start.
            RECT caret = { textRect.left, textRect.top + 2, textRect.left + 1, textRect.bottom - 2 };
            HBRUSH hCaret = CreateSolidBrush(colorActive);
            FillRect(hdc, &caret, hCaret);
            DeleteObject(hCaret);
        } else if (!m_placeholder.empty()) {
            SetTextColor(hdc, colorInactive);
            DrawTextW(hdc, m_placeholder.c_str(), -1, &textRect, dtFlags);
        }
    }
}

// ---------------------------------------------------------------------------
inline InputResult InputBox::RouteChar(wchar_t ch, HWND /*host*/)
{
    if (ch == L'\b') { // backspace
        if (HasSelection()) { DeleteSelection(); notify(); return InputResult::ConsumedRepaint; }
        if (m_caretPos > 0) { m_text.erase(m_caretPos - 1, 1); --m_caretPos; notify(); return InputResult::ConsumedRepaint; }
        return InputResult::Consumed;
    }
    if (ch >= L' ') {
        if (HasSelection()) DeleteSelection();
        if (m_maxLen <= 0 || (int)m_text.size() < m_maxLen) {
            m_text.insert(m_caretPos, 1, ch);
            ++m_caretPos;
            notify();
            return InputResult::ConsumedRepaint;
        }
        return InputResult::Consumed; // at max length — swallowed, no change
    }
    return InputResult::Ignored;
}

// ---------------------------------------------------------------------------
inline InputResult InputBox::RouteKey(WPARAM vk, HWND /*host*/)
{
    // Function keys and bare modifiers are global app shortcuts — never editor
    // input. Let the host forward them.
    if (IsForwardableShortcutKey(vk)) return InputResult::Ignored;

    const bool shift = (GetKeyState(VK_SHIFT)   & 0x8000) != 0;
    const bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;

    switch ((UINT)vk) {
    case VK_DELETE:
        if (HasSelection()) { DeleteSelection(); notify(); return InputResult::ConsumedRepaint; }
        if (m_caretPos < (int)m_text.size()) { m_text.erase(m_caretPos, 1); notify(); return InputResult::ConsumedRepaint; }
        return InputResult::Consumed;

    case VK_LEFT:
        if (shift) {
            if (m_selAnchor < 0) m_selAnchor = m_caretPos;
            if (m_caretPos > 0) --m_caretPos;
        } else {
            if (HasSelection()) m_caretPos = SelMin();
            else if (m_caretPos > 0) --m_caretPos;
            m_selAnchor = -1;
        }
        return InputResult::ConsumedRepaint;

    case VK_RIGHT:
        if (shift) {
            if (m_selAnchor < 0) m_selAnchor = m_caretPos;
            if (m_caretPos < (int)m_text.size()) ++m_caretPos;
        } else {
            if (HasSelection()) m_caretPos = SelMax();
            else if (m_caretPos < (int)m_text.size()) ++m_caretPos;
            m_selAnchor = -1;
        }
        return InputResult::ConsumedRepaint;

    case VK_HOME:
        if (shift) { if (m_selAnchor < 0) m_selAnchor = m_caretPos; m_caretPos = 0; }
        else { m_caretPos = 0; m_selAnchor = -1; }
        return InputResult::ConsumedRepaint;

    case VK_END:
        if (shift) { if (m_selAnchor < 0) m_selAnchor = m_caretPos; m_caretPos = (int)m_text.size(); }
        else { m_caretPos = (int)m_text.size(); m_selAnchor = -1; }
        return InputResult::ConsumedRepaint;

    case VK_ESCAPE:
        if (!m_text.empty()) { Clear(); return InputResult::RequestClear; } // same as the ✕ button
        return InputResult::RequestClose; // empty — host hides

    case 'A':
        if (ctrl) { m_selAnchor = 0; m_caretPos = (int)m_text.size(); return InputResult::ConsumedRepaint; }
        break;
    case 'C':
        if (ctrl && HasSelection()) {
            CopyToClipboard(m_text.substr(SelMin(), SelMax() - SelMin()));
            return InputResult::Consumed;
        }
        break;
    case 'X':
        if (ctrl && HasSelection()) {
            CopyToClipboard(m_text.substr(SelMin(), SelMax() - SelMin()));
            DeleteSelection();
            notify();
            return InputResult::ConsumedRepaint;
        }
        break;
    case 'V':
        if (ctrl) {
            std::wstring clip = GetClipboardText();
            if (!clip.empty()) {
                if (HasSelection()) DeleteSelection();
                for (wchar_t c : clip) {
                    if (c >= L' ' && (m_maxLen <= 0 || (int)m_text.size() < m_maxLen)) {
                        m_text.insert(m_caretPos, 1, c);
                        ++m_caretPos;
                    }
                }
                notify();
            }
            return InputResult::ConsumedRepaint;
        }
        break;
    }

    // Not an editing key. Other Ctrl/Alt combos are app shortcuts → forward.
    const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    if (ctrl || alt) return InputResult::Ignored;

    // Plain non-editing key (e.g. a letter's WM_KEYDOWN — the character itself
    // arrives via WM_CHAR): swallow so it can't misfire a main-window hotkey.
    return InputResult::Consumed;
}

// ---------------------------------------------------------------------------
inline InputResult InputBox::RouteMouse(UINT msg, WPARAM /*wParam*/, LPARAM lParam)
{
    switch (msg) {
    case WM_LBUTTONDOWN: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        // ✕ clear button
        if (m_clearRect.right > m_clearRect.left && PtInRect(&m_clearRect, pt)) {
            Clear();
            return InputResult::ConsumedRepaint;
        }
        // Click inside the text area → position the caret.
        if (PtInRect(&m_textRect, pt)) {
            m_caretPos  = CaretFromX(pt.x);
            m_selAnchor = -1;
            return InputResult::ConsumedRepaint;
        }
        return InputResult::Ignored;
    }

    case WM_MOUSEMOVE: {
        if (m_clearRect.right > m_clearRect.left) {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            bool hovered = PtInRect(&m_clearRect, pt) != 0;
            if (hovered != m_clearHovered) {
                m_clearHovered = hovered;
                return InputResult::ConsumedRepaint;
            }
        } else if (m_clearHovered) {
            m_clearHovered = false;
            return InputResult::ConsumedRepaint;
        }
        return InputResult::Ignored;
    }

    case WM_MOUSELEAVE:
        if (m_clearHovered) {
            m_clearHovered = false;
            return InputResult::ConsumedRepaint;
        }
        return InputResult::Ignored;
    }
    return InputResult::Ignored;
}

} // namespace UI
