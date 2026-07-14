#include "JumpToWnd.h"
#include "../AppState.h"
#include "../Platform/Constants.h"
#include "../Platform/FileHandler.h"

extern AppState app;

namespace UI {

void JumpToWnd::Init(HINSTANCE hInstance, HWND hParent) {
    const int w = static_cast<int>(340.0f * app.dpiScale);
    const int h = static_cast<int>(140.0f * app.dpiScale);
    InitFloating(hInstance, hParent, L"QivJumpToWndClass", L"Jump to Image", w, h);
}

void JumpToWnd::Init(HINSTANCE hInstance, HWND hParent, int8_t /*position*/) {
    Init(hInstance, hParent);
}

void JumpToWnd::Show() {
    if (!m_hWnd) return;
    m_inputLen   = 0;
    m_input[0]   = L'\0';
    m_outOfRange = false;
    m_total      = static_cast<int>(app.playlist.size());

    RECT pr;
    GetWindowRect(m_hParent, &pr);
    RECT wr;
    GetWindowRect(m_hWnd, &wr);
    int ww = wr.right  - wr.left;
    int wh = wr.bottom - wr.top;
    int px = pr.left + (pr.right  - pr.left - ww) / 2;
    int py = pr.top  + (pr.bottom - pr.top  - wh) / 2;
    SetWindowPos(m_hWnd, HWND_TOPMOST, px, py, 0, 0, SWP_NOSIZE | SWP_FRAMECHANGED);

    ShowWindow(m_hWnd, SW_SHOW);
    SetForegroundWindow(m_hWnd);
    InvalidateRect(m_hWnd, nullptr, FALSE);
}

void JumpToWnd::CommitJump() {
    if (m_inputLen == 0) { Hide(); return; }
    int number = _wtoi(m_input);
    if (m_total <= 0 || number < 1 || number > m_total) {
        m_outOfRange = true;
        InvalidateRect(m_hWnd, nullptr, FALSE);
        return;
    }
    Hide();
    LoadImageIndex(m_hParent, number - 1);
    InvalidateRect(m_hParent, nullptr, FALSE);
}

bool JumpToWnd::OnKeyDown(WPARAM vk, bool /*ctrl*/, bool /*shift*/, bool /*alt*/) {
    if (vk == VK_BACK) {
        if (m_inputLen > 0) {
            m_input[--m_inputLen] = L'\0';
            m_outOfRange          = false;
            InvalidateRect(m_hWnd, nullptr, FALSE);
        }
        return true;
    }
    if (vk == VK_RETURN) {
        CommitJump();
        return true;
    }
    // Digits are handled by WM_CHAR — consume here so they are not
    // forwarded to the main app's view-mode shortcuts (1–5).
    if (vk >= '0' && vk <= '9')
        return true;
    return false;
}

LRESULT JumpToWnd::HandlePanelMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(m_hWnd, &ps);
            RECT rc;
            GetClientRect(m_hWnd, &rc);

            HBRUSH bgBrush = CreateSolidBrush(GetBgColor());
            FillRect(hdc, &rc, bgBrush);
            DeleteObject(bgBrush);
            SetBkMode(hdc, TRANSPARENT);

            const int pad  = static_cast<int>(14.0f * app.dpiScale);
            const int gap  = static_cast<int>(8.0f  * app.dpiScale);
            const int fs   = static_cast<int>(13.0f * app.dpiScale);
            const int fsIn = static_cast<int>(16.0f * app.dpiScale);

            const int dpiKey = static_cast<int>(app.dpiScale * 96);
            if (dpiKey != m_cachedFontDpi) {
                if (m_hFont)      { DeleteObject(m_hFont);      m_hFont      = nullptr; }
                if (m_hFontInput) { DeleteObject(m_hFontInput); m_hFontInput = nullptr; }
                m_hFont = CreateFontW(-fs, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                                      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                      CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                      DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
                m_hFontInput = CreateFontW(-fsIn, 0, 0, 0, FW_BOLD, 0, 0, 0,
                                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                           DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
                m_cachedFontDpi = dpiKey;
            }
            HFONT hOldFont = static_cast<HFONT>(SelectObject(hdc, m_hFont));

            // Label: "Image number  ( 1 – N )"
            wchar_t label[80];
            if (m_total > 0)
                swprintf_s(label, L"Image number  ( 1 – %d )", m_total);
            else
                swprintf_s(label, L"No images loaded");

            SetTextColor(hdc, Constants::Theme::ThemedGray(0.9020f, app.themeFactor));
            RECT labelRect = { pad, pad, rc.right - pad, pad + fs + 6 };
            DrawTextW(hdc, label, -1, &labelRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            // Input box
            const int boxTop = labelRect.bottom + gap;
            const int boxH   = static_cast<int>(34.0f * app.dpiScale);
            RECT boxRect = { pad, boxTop, rc.right - pad, boxTop + boxH };

            COLORREF boxBg = m_outOfRange
                ? Constants::Theme::ThemedColor(0.35f, 0.06f, 0.06f, app.themeFactor)
                : Constants::Theme::ThemedGray(0.14f, app.themeFactor);
            HBRUSH boxBrush = CreateSolidBrush(boxBg);
            FillRect(hdc, &boxRect, boxBrush);
            DeleteObject(boxBrush);

            COLORREF borderColor = m_outOfRange
                ? Constants::Theme::ThemedColor(0.80f, 0.25f, 0.25f, app.themeFactor)
                : Constants::Theme::ThemedGray(0.35f, app.themeFactor);
            HPEN hPen = CreatePen(PS_SOLID, 1, borderColor);
            HPEN  hOldPen   = static_cast<HPEN> (SelectObject(hdc, hPen));
            HBRUSH hNullBr  = static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
            HBRUSH hOldBr   = static_cast<HBRUSH>(SelectObject(hdc, hNullBr));
            Rectangle(hdc, boxRect.left, boxRect.top, boxRect.right, boxRect.bottom);
            SelectObject(hdc, hOldPen);
            SelectObject(hdc, hOldBr);
            DeleteObject(hPen);

            // Input text + blinking cursor
            wchar_t display[16];
            if (m_inputLen > 0) {
                wcsncpy_s(display, m_input, m_inputLen);
                display[m_inputLen]     = L'_';
                display[m_inputLen + 1] = L'\0';
            } else {
                wcscpy_s(display, L"_");
            }

            COLORREF inputColor = m_outOfRange
                ? Constants::Theme::ThemedColor(1.0f, 0.4f, 0.4f, app.themeFactor)
                : Constants::Theme::ThemedColor(0.39f, 0.78f, 1.0f, app.themeFactor);
            SetTextColor(hdc, inputColor);

            SelectObject(hdc, m_hFontInput);
            RECT inputRect = { boxRect.left + pad / 2, boxRect.top,
                               boxRect.right - pad / 2, boxRect.bottom };
            DrawTextW(hdc, display, -1, &inputRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(hdc, m_hFont);

            // Hint line
            RECT hintRect = { pad, boxRect.bottom + gap,
                              rc.right - pad, boxRect.bottom + gap + fs + 4 };
            SetTextColor(hdc, Constants::Theme::ThemedGray(0.50f, app.themeFactor));
            const wchar_t *hint = m_outOfRange
                ? L"Out of range — type a new number"
                : L"Enter = jump  •  Esc = cancel";
            DrawTextW(hdc, hint, -1, &hintRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            SelectObject(hdc, hOldFont);
            EndPaint(m_hWnd, &ps);
            return 0;
        }

        case WM_CHAR: {
            wchar_t ch = static_cast<wchar_t>(wParam);
            // Switch to Find dialog when the trigger char is the first input
            if (ch == Constants::PANEL_SWITCH_TO_FIND_CHAR && m_inputLen == 0) {
                Hide();
                PostMessageW(m_hParent, Constants::WM_QIV_SWITCH_TO_FIND, 0, 0);
                return 0;
            }
            if (ch >= L'0' && ch <= L'9' && m_inputLen < 6) {
                m_input[m_inputLen++] = ch;
                m_input[m_inputLen]   = L'\0';
                m_outOfRange          = false;
                InvalidateRect(m_hWnd, nullptr, FALSE);
            }
            return 0;
        }

        default:
            break;
    }
    return DefWindowProcW(m_hWnd, message, wParam, lParam);
}

} // namespace UI
