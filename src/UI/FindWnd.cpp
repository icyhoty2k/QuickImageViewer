#include "FindWnd.h"
#include "../AppState.h"
#include "../Platform/Constants.h"
#include "../Platform/FileHandler.h"
#include <algorithm>
#include <cwctype>

extern AppState app;

namespace UI {

// =============================================================================
//  Init / Show
// =============================================================================

void FindWnd::Init(HINSTANCE hInstance, HWND hParent) {
    const int w = static_cast<int>(460.0f * app.dpiScale);
    const int h = static_cast<int>(330.0f * app.dpiScale);
    InitFloating(hInstance, hParent, L"QivFindWndClass", L"Find Image", w, h);
}

void FindWnd::Init(HINSTANCE hInstance, HWND hParent, int8_t /*position*/) {
    Init(hInstance, hParent);
}

void FindWnd::Show() {
    if (!m_hWnd) return;

    m_queryLen  = 0;
    m_query[0]  = L'\0';
    m_selIdx    = 0;
    m_rowScroll = 0;
    m_matches.clear();

    RECT pr; GetWindowRect(m_hParent, &pr);
    RECT wr; GetWindowRect(m_hWnd,    &wr);
    int ww = wr.right - wr.left;
    int wh = wr.bottom - wr.top;
    int px = pr.left + (pr.right  - pr.left - ww) / 2;
    int py = pr.top  + (pr.bottom - pr.top  - wh) / 2;
    SetWindowPos(m_hWnd, HWND_TOPMOST, px, py, 0, 0, SWP_NOSIZE | SWP_FRAMECHANGED);
    ShowWindow(m_hWnd, SW_SHOW);
    SetForegroundWindow(m_hWnd);
    InvalidateRect(m_hWnd, nullptr, FALSE);
}

// =============================================================================
//  Search logic
// =============================================================================

void FindWnd::RebuildMatches() {
    m_matches.clear();
    m_selIdx    = 0;
    m_rowScroll = 0;

    if (m_queryLen == 0) return;

    // Lowercase copy of query
    wchar_t lq[MAX_QUERY + 2];
    wcsncpy_s(lq, m_query, m_queryLen);
    lq[m_queryLen] = L'\0';
    for (int i = 0; lq[i]; ++i) lq[i] = static_cast<wchar_t>(towlower(lq[i]));

    wchar_t lname[512];
    for (int i = 0; i < static_cast<int>(app.playlist.size()); ++i) {
        const std::wstring &path = app.playlist[i];

        // Extract filename only
        auto sep = path.rfind(L'\\');
        const wchar_t *name = (sep == std::wstring::npos)
                              ? path.c_str() : path.c_str() + sep + 1;

        // Lowercase filename
        int j = 0;
        while (name[j] && j < 511) { lname[j] = static_cast<wchar_t>(towlower(name[j])); ++j; }
        lname[j] = L'\0';

        if (wcsstr(lname, lq)) m_matches.push_back(i);
    }
}

void FindWnd::AdjustScroll() {
    if (m_matches.empty()) return;
    // Clamp selection
    m_selIdx = std::max(0, std::min(m_selIdx, static_cast<int>(m_matches.size()) - 1));
    // Scroll so selection is within the visible window
    if (m_selIdx < m_rowScroll)
        m_rowScroll = m_selIdx;
    if (m_selIdx >= m_rowScroll + VISIBLE_ROWS)
        m_rowScroll = m_selIdx - VISIBLE_ROWS + 1;
}

void FindWnd::CommitOpen() {
    if (m_matches.empty()) return;
    int playlistIdx = m_matches[m_selIdx];
    Hide();
    LoadImageIndex(m_hParent, playlistIdx);
    InvalidateRect(m_hParent, nullptr, FALSE);
}

// =============================================================================
//  Input
// =============================================================================

bool FindWnd::OnKeyDown(WPARAM vk, bool /*ctrl*/, bool /*shift*/, bool /*alt*/) {
    switch (vk) {
        case VK_BACK:
            if (m_queryLen > 0) {
                m_query[--m_queryLen] = L'\0';
                RebuildMatches();
                InvalidateRect(m_hWnd, nullptr, FALSE);
            }
            return true;

        case VK_RETURN:
            CommitOpen();
            return true;

        case VK_UP:
            if (!m_matches.empty()) {
                --m_selIdx;
                AdjustScroll();
                InvalidateRect(m_hWnd, nullptr, FALSE);
            }
            return true;

        case VK_DOWN:
            if (!m_matches.empty()) {
                ++m_selIdx;
                AdjustScroll();
                InvalidateRect(m_hWnd, nullptr, FALSE);
            }
            return true;

        case VK_PRIOR: // Page Up
            if (!m_matches.empty()) {
                m_selIdx -= VISIBLE_ROWS;
                AdjustScroll();
                InvalidateRect(m_hWnd, nullptr, FALSE);
            }
            return true;

        case VK_NEXT: // Page Down
            if (!m_matches.empty()) {
                m_selIdx += VISIBLE_ROWS;
                AdjustScroll();
                InvalidateRect(m_hWnd, nullptr, FALSE);
            }
            return true;

        default:
            break;
    }
    return false;
}

// =============================================================================
//  Paint
// =============================================================================

LRESULT FindWnd::HandlePanelMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {

    case WM_CHAR: {
        wchar_t ch = static_cast<wchar_t>(wParam);
        // Accept any printable character, ignore control chars
        if (ch >= L' ' && m_queryLen < MAX_QUERY) {
            m_query[m_queryLen++] = ch;
            m_query[m_queryLen]   = L'\0';
            RebuildMatches();
            InvalidateRect(m_hWnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(m_hWnd, &ps);
        RECT rc;
        GetClientRect(m_hWnd, &rc);

        // ── Background ───────────────────────────────────────────────────────
        HBRUSH bgBrush = CreateSolidBrush(GetBgColor());
        FillRect(hdc, &rc, bgBrush);
        DeleteObject(bgBrush);
        SetBkMode(hdc, TRANSPARENT);

        const float dpi = app.dpiScale;
        const int pad   = static_cast<int>(14.0f * dpi);
        const int gap   = static_cast<int>(8.0f  * dpi);
        const int fs    = static_cast<int>(13.0f * dpi);
        const int fsIn  = static_cast<int>(15.0f * dpi);
        const int rowH  = static_cast<int>(26.0f * dpi);

        HFONT hFontNorm = CreateFontW(-fs, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                                      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                      CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                      DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        HFONT hFontBold = CreateFontW(-fsIn, 0, 0, 0, FW_BOLD, 0, 0, 0,
                                      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                      CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                      DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        HFONT hOldFont = static_cast<HFONT>(SelectObject(hdc, hFontNorm));

        const COLORREF clrLabel   = Constants::Theme::ThemedGray(0.90f, app.themeFactor);
        const COLORREF clrDim     = Constants::Theme::ThemedGray(0.45f, app.themeFactor);
        const COLORREF clrInput   = Constants::Theme::ThemedColor(0.39f, 0.78f, 1.0f, app.themeFactor);
        const COLORREF clrSelBg   = Constants::Theme::ThemedColor(0.15f, 0.35f, 0.60f, app.themeFactor);
        const COLORREF clrSelText = Constants::Theme::ThemedGray(1.00f, app.themeFactor);
        const COLORREF clrRowText = Constants::Theme::ThemedGray(0.85f, app.themeFactor);
        const COLORREF clrOrange  = Constants::Theme::ThemedColor(1.0f, 0.65f, 0.1f, app.themeFactor);

        int y = pad;

        // ── Label ─────────────────────────────────────────────────────────────
        {
            wchar_t lbl[64];
            int total = static_cast<int>(app.playlist.size());
            if (total > 0)
                swprintf_s(lbl, L"Find in %d images", total);
            else
                swprintf_s(lbl, L"No images loaded");
            SetTextColor(hdc, clrLabel);
            RECT r = { pad, y, rc.right - pad, y + fs + 4 };
            DrawTextW(hdc, lbl, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            y += fs + 4 + gap;
        }

        // ── Input box ─────────────────────────────────────────────────────────
        const int boxH   = static_cast<int>(34.0f * dpi);
        RECT boxRect = { pad, y, rc.right - pad, y + boxH };

        COLORREF boxBg = Constants::Theme::ThemedGray(0.14f, app.themeFactor);
        HBRUSH boxBrush = CreateSolidBrush(boxBg);
        FillRect(hdc, &boxRect, boxBrush);
        DeleteObject(boxBrush);

        HPEN hPen   = CreatePen(PS_SOLID, 1, Constants::Theme::ThemedGray(0.35f, app.themeFactor));
        HPEN hOldPen = static_cast<HPEN>(SelectObject(hdc, hPen));
        HBRUSH hNullBr = static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
        HBRUSH hOldBr  = static_cast<HBRUSH>(SelectObject(hdc, hNullBr));
        Rectangle(hdc, boxRect.left, boxRect.top, boxRect.right, boxRect.bottom);
        SelectObject(hdc, hOldPen);
        SelectObject(hdc, hOldBr);
        DeleteObject(hPen);

        // Query text + cursor
        SelectObject(hdc, hFontBold);
        SetTextColor(hdc, m_queryLen > 0 ? clrInput : clrDim);

        wchar_t display[MAX_QUERY + 4];
        if (m_queryLen > 0) {
            wcsncpy_s(display, m_query, m_queryLen);
            display[m_queryLen]     = L'_';
            display[m_queryLen + 1] = L'\0';
        } else {
            wcscpy_s(display, L"type filename…");
        }

        RECT inRect = { boxRect.left + pad / 2, boxRect.top,
                        boxRect.right - pad / 2, boxRect.bottom };
        DrawTextW(hdc, display, -1, &inRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(hdc, hFontNorm);

        y += boxH + gap;

        // ── Divider ───────────────────────────────────────────────────────────
        {
            HPEN divPen = CreatePen(PS_SOLID, 1, Constants::Theme::ThemedGray(0.22f, app.themeFactor));
            HPEN old    = static_cast<HPEN>(SelectObject(hdc, divPen));
            MoveToEx(hdc, pad, y, nullptr);
            LineTo(hdc, rc.right - pad, y);
            SelectObject(hdc, old);
            DeleteObject(divPen);
            y += gap;
        }

        // ── Match list ────────────────────────────────────────────────────────
        const int listTop = y;

        if (m_queryLen == 0) {
            // No query yet — placeholder
            SetTextColor(hdc, clrDim);
            RECT r = { pad, y, rc.right - pad, y + rowH * VISIBLE_ROWS };
            DrawTextW(hdc, L"Start typing to filter by filename", -1, &r,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else if (m_matches.empty()) {
            SetTextColor(hdc, clrOrange);
            RECT r = { pad, y, rc.right - pad, y + rowH };
            DrawTextW(hdc, L"No matches", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else {
            int visible = std::min(VISIBLE_ROWS, static_cast<int>(m_matches.size()) - m_rowScroll);
            for (int i = 0; i < visible; ++i) {
                int matchIdx = m_rowScroll + i;
                bool selected = (matchIdx == m_selIdx);

                RECT rowRect = { pad, y, rc.right - pad, y + rowH };

                if (selected) {
                    HBRUSH selBrush = CreateSolidBrush(clrSelBg);
                    RECT fillRect = { 0, y, rc.right, y + rowH };
                    FillRect(hdc, &fillRect, selBrush);
                    DeleteObject(selBrush);
                    SetTextColor(hdc, clrSelText);
                } else {
                    SetTextColor(hdc, clrRowText);
                }

                // Extract filename from full path
                const std::wstring &fullPath = app.playlist[m_matches[matchIdx]];
                auto sep = fullPath.rfind(L'\\');
                const wchar_t *fname = (sep == std::wstring::npos)
                                       ? fullPath.c_str() : fullPath.c_str() + sep + 1;

                RECT textRect = { pad + (selected ? pad/2 : 0), y,
                                  rc.right - pad, y + rowH };
                DrawTextW(hdc, fname, -1, &textRect,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

                y += rowH;
            }
        }

        y = listTop + rowH * VISIBLE_ROWS + gap;

        // ── Bottom divider ────────────────────────────────────────────────────
        {
            HPEN divPen = CreatePen(PS_SOLID, 1, Constants::Theme::ThemedGray(0.22f, app.themeFactor));
            HPEN old    = static_cast<HPEN>(SelectObject(hdc, divPen));
            MoveToEx(hdc, pad, y, nullptr);
            LineTo(hdc, rc.right - pad, y);
            SelectObject(hdc, old);
            DeleteObject(divPen);
            y += gap;
        }

        // ── Hint / count row ──────────────────────────────────────────────────
        {
            wchar_t hint[128];
            if (!m_matches.empty()) {
                swprintf_s(hint, L"%d / %d    ↑↓ select  •  Enter open  •  Esc cancel",
                           m_selIdx + 1, static_cast<int>(m_matches.size()));
            } else if (m_queryLen > 0) {
                wcscpy_s(hint, L"No matches  •  Esc cancel");
            } else {
                wcscpy_s(hint, L"Ctrl+F  •  Enter open  •  Esc cancel");
            }
            SetTextColor(hdc, clrDim);
            RECT r = { pad, y, rc.right - pad, y + fs + 4 };
            DrawTextW(hdc, hint, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        SelectObject(hdc, hOldFont);
        DeleteObject(hFontNorm);
        DeleteObject(hFontBold);
        EndPaint(m_hWnd, &ps);
        return 0;
    }

    default:
        break;
    }
    return DefWindowProcW(m_hWnd, message, wParam, lParam);
}

} // namespace UI
