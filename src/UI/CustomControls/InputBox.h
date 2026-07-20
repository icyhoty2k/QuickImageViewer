        #pragma once
#include <windows.h>
#include <windowsx.h>
#include <string>
#include <functional>
#include "AppState.h"
#include "Platform/Constants.h"
#include "Platform/ConstantsTheme.h"

extern AppState app;

namespace UI {

// Self-contained text input control.
//
// Owns its text buffer, draws itself (themed border, text, ✕ clear button),
// and processes WM_CHAR / WM_KEYDOWN / WM_LBUTTONDOWN / WM_MOUSEMOVE /
// WM_MOUSELEAVE internally.
//
// Callers wire up:
//   1. OnChanged callback — what to do when text changes
//   2. Draw() call from WM_PAINT
//   3. HandleMessage() forwarding for WM_CHAR, WM_KEYDOWN, WM_LBUTTONDOWN,
//      WM_MOUSEMOVE, WM_MOUSELEAVE
//
// Everything else (colors, ✕ hover, hit testing, backspace) lives here.
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

    // Clear text and notify — use for ESC / programmatic clear.
    void Clear()  { m_text.clear(); m_caretPos = 0; m_selAnchor = -1; m_clearRect = {}; m_clearHovered = false; notify(); }

    // Clear text silently — use in Show() where the caller drives layout itself.
    void Reset()  { m_text.clear(); m_caretPos = 0; m_selAnchor = -1; m_clearRect = {}; m_clearHovered = false; }

    // Draw the control into hdc. Records rects for subsequent hit-testing.
    // hasFocus: pass GetFocus() == m_hWnd from the owner window.
    // isError:  red color variant (JumpTo out-of-range state).
    // dtFlags:  DT_LEFT or DT_CENTER; DT_VCENTER|DT_SINGLELINE are always appended.
    void Draw(HDC hdc, HFONT hFont, const RECT& boxRect, int padX,
              bool hasFocus, bool isError = false,
              UINT dtFlags = DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // Feed raw messages. Returns true if the message was consumed or caused a
    // visual state change that requires a repaint.
    // Forward: WM_CHAR, WM_KEYDOWN, WM_LBUTTONDOWN, WM_MOUSEMOVE, WM_MOUSELEAVE.
    // Callers may pre-filter (e.g. allow only digits) before delegating.
    bool HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);


private:
    std::wstring m_text;
    std::wstring m_placeholder;
    RECT         m_clearRect    = {};
    int          m_maxLen       = 0;     // 0 = unlimited
    int          m_caretPos     = 0;     // insertion point within m_text [0, size]
    int          m_selAnchor    = -1;    // selection anchor; -1 = no selection
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
    // SIZE_HIGHLIGHT = RGB(240, 50, 50) → 240/255≈0.941, 50/255≈0.196
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

    // Text area (padded from left, does not overlap ✕)
    RECT textRect = { boxRect.left + padX, boxRect.top,
                      boxRect.right - padX - clearW, boxRect.bottom };

    if (hasText) {
        if (HasSelection()) {
            TEXTMETRIC tm;
            GetTextMetrics(hdc, &tm);
            const int ty = textRect.top + (textRect.bottom - textRect.top - tm.tmHeight) / 2;
            const COLORREF colorSel     = GetSysColor(COLOR_HIGHLIGHT);
            const COLORREF colorSelText = GetSysColor(COLOR_HIGHLIGHTTEXT);
            const int lo = SelMin(), hi = SelMax();

            SIZE szA = {}, szB = {};
            GetTextExtentPoint32W(hdc, m_text.c_str(), lo, &szA);
            GetTextExtentPoint32W(hdc, m_text.c_str(), hi, &szB);
            const int xA = textRect.left + (int)szA.cx;
            const int xB = (textRect.left + (int)szB.cx < textRect.right)
                               ? (textRect.left + (int)szB.cx) : textRect.right;

            // Segment before selection
            if (lo > 0) {
                SetTextColor(hdc, colorActive);
                const RECT r1 = { textRect.left, textRect.top, xA, textRect.bottom };
                ExtTextOutW(hdc, textRect.left, ty, ETO_CLIPPED, &r1,
                            m_text.c_str(), lo, nullptr);
            }
            // Selection background
            if (xB > xA) {
                HBRUSH hBr = CreateSolidBrush(colorSel);
                const RECT sr = { xA, textRect.top, xB, textRect.bottom };
                FillRect(hdc, &sr, hBr);
                DeleteObject(hBr);
            }
            // Selected text
            if (hi > lo && xA < textRect.right) {
                SetTextColor(hdc, colorSelText);
                const RECT r2 = { xA, textRect.top, xB, textRect.bottom };
                ExtTextOutW(hdc, xA, ty, ETO_CLIPPED, &r2,
                            m_text.c_str() + lo, hi - lo, nullptr);
            }
            // Segment after selection
            if (hi < (int)m_text.size() && xB < textRect.right) {
                SetTextColor(hdc, colorActive);
                const RECT r3 = { xB, textRect.top, textRect.right, textRect.bottom };
                ExtTextOutW(hdc, xB, ty, ETO_CLIPPED, &r3,
                            m_text.c_str() + hi, (int)m_text.size() - hi, nullptr);
            }
        } else {
            // No selection: normal text + _ caret overlay
            SetTextColor(hdc, colorActive);
            DrawTextW(hdc, m_text.c_str(), (int)m_text.size(), &textRect, dtFlags | DT_END_ELLIPSIS);
            SIZE szPre = {};
            GetTextExtentPoint32W(hdc, m_text.c_str(), m_caretPos, &szPre);
            const int cx = textRect.left + szPre.cx;
            if (cx < textRect.right) {
                RECT cr = { cx, textRect.top, textRect.right, textRect.bottom };
                DrawTextW(hdc, L"_", 1, &cr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            }
        }
    } else if (hasFocus) {
        SetTextColor(hdc, colorActive);
        DrawTextW(hdc, L"_", 1, &textRect, dtFlags);
    } else {
        SetTextColor(hdc, colorInactive);
        if (!m_placeholder.empty())
            DrawTextW(hdc, m_placeholder.c_str(), -1, &textRect, dtFlags);
    }
}

// ---------------------------------------------------------------------------
inline bool InputBox::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CHAR: {
        wchar_t ch = static_cast<wchar_t>(wParam);
        if (ch == L'\b') {
            if (HasSelection()) { DeleteSelection(); notify(); }
            else if (m_caretPos > 0) { m_text.erase(m_caretPos - 1, 1); --m_caretPos; notify(); }
            return true;
        }
        if (ch >= L' ') {
            if (HasSelection()) DeleteSelection();
            if (m_maxLen <= 0 || (int)m_text.size() < m_maxLen) {
                m_text.insert(m_caretPos, 1, ch);
                ++m_caretPos;
                notify();
            }
            return true;
        }
        return false;
    }
    case WM_KEYDOWN: {
        const bool shift = (GetKeyState(VK_SHIFT)   & 0x8000) != 0;
        const bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        switch ((UINT)wParam) {
        case VK_DELETE:
            if (HasSelection()) { DeleteSelection(); notify(); }
            else if (m_caretPos < (int)m_text.size()) { m_text.erase(m_caretPos, 1); notify(); }
            return true;
        case VK_LEFT:
            if (shift) {
                if (m_selAnchor < 0) m_selAnchor = m_caretPos;
                if (m_caretPos > 0) --m_caretPos;
            } else {
                if (HasSelection()) { m_caretPos = SelMin(); }
                else if (m_caretPos > 0) --m_caretPos;
                m_selAnchor = -1;
            }
            return true;
        case VK_RIGHT:
            if (shift) {
                if (m_selAnchor < 0) m_selAnchor = m_caretPos;
                if (m_caretPos < (int)m_text.size()) ++m_caretPos;
            } else {
                if (HasSelection()) { m_caretPos = SelMax(); }
                else if (m_caretPos < (int)m_text.size()) ++m_caretPos;
                m_selAnchor = -1;
            }
            return true;
        case VK_HOME:
            if (shift) { if (m_selAnchor < 0) m_selAnchor = m_caretPos; m_caretPos = 0; }
            else { m_caretPos = 0; m_selAnchor = -1; }
            return true;
        case VK_END:
            if (shift) { if (m_selAnchor < 0) m_selAnchor = m_caretPos; m_caretPos = (int)m_text.size(); }
            else { m_caretPos = (int)m_text.size(); m_selAnchor = -1; }
            return true;
        case VK_ESCAPE:
            if (!m_text.empty()) { Clear(); return true; } // same action as the ✕ button
            return false; // no text — caller decides (panel closes)
        case 'A':
            if (ctrl) { m_selAnchor = 0; m_caretPos = (int)m_text.size(); return true; }
            return false;
        case 'C':
            if (ctrl && HasSelection()) {
                CopyToClipboard(m_text.substr(SelMin(), SelMax() - SelMin()));
                return true;
            }
            return false;
        case 'X':
            if (ctrl && HasSelection()) {
                CopyToClipboard(m_text.substr(SelMin(), SelMax() - SelMin()));
                DeleteSelection();
                notify();
                return true;
            }
            return false;
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
                return true;
            }
            return false;
        }
        return false;
    }

    case WM_LBUTTONDOWN: {
        m_selAnchor = -1; // clear selection on any click
        if (m_clearRect.right > m_clearRect.left) {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (PtInRect(&m_clearRect, pt)) {
                Clear(); // resets text, caretPos, selAnchor, rects, notifies
                return true;
            }
        }
        return false;
    }

    case WM_MOUSEMOVE: {
        if (m_clearRect.right > m_clearRect.left) {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            bool hovered = PtInRect(&m_clearRect, pt) != 0;
            if (hovered != m_clearHovered) {
                m_clearHovered = hovered;
                return true; // state changed — caller should InvalidateRect
            }
        } else if (m_clearHovered) {
            m_clearHovered = false;
            return true;
        }
        return false;
    }

    case WM_MOUSELEAVE:
        if (m_clearHovered) {
            m_clearHovered = false;
            return true;
        }
        return false;
    }
    return false;
}


} // namespace UI
