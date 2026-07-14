#include "FindWnd.h"
#include "../AppState.h"
#include "../Platform/Constants.h"
#include "../Platform/FileHandler.h"
#include "../Renderer/IRenderer.h"
#include <algorithm>
#include <cwctype>

extern AppState app;

namespace UI {

// Case-insensitive wildcard match (* = any sequence, ? = any single char).
static bool WildcardMatch(const wchar_t *pat, const wchar_t *text) {
    const wchar_t *star = nullptr, *s = text;
    while (*text) {
        if (*pat == *text || *pat == L'?') { ++pat; ++text; }
        else if (*pat == L'*')             { star = pat++; s = text; }
        else if (star)                     { pat = star + 1; text = ++s; }
        else return false;
    }
    while (*pat == L'*') ++pat;
    return !*pat;
}

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
    m_results.clear();

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
    m_results.clear();
    m_selIdx          = 0;
    m_rowScroll       = 0;
    m_cachedExtraCount = 0;

    if (m_queryLen == 0) return;

    // Lowercase copy of query
    wchar_t lq[MAX_QUERY + 2];
    wcsncpy_s(lq, m_query, m_queryLen);
    lq[m_queryLen] = L'\0';
    for (int i = 0; lq[i]; ++i) lq[i] = static_cast<wchar_t>(towlower(lq[i]));

    // Wildcard mode: query contains '*' or '?'
    bool hasWildcard = false;
    for (int i = 0; i < m_queryLen; ++i)
        if (lq[i] == L'*' || lq[i] == L'?') { hasWildcard = true; break; }

    // Snapshot VRAM cache paths not already in the current playlist.
    // Done fresh on every RebuildMatches so it reflects the live cache.
    std::vector<std::wstring> extraPaths;
    if (app.renderer) {
        for (auto &item : app.renderer->GetCachedBitmaps()) {
            if (app.playlistIndexMap.find(item.filePath) == app.playlistIndexMap.end())
                extraPaths.push_back(item.filePath);
        }
    }

    // Helper: match one path and push result if it matches.
    auto tryMatch = [&](const std::wstring &path, int playlistIdx) {
        const wchar_t *name = path.c_str();
        size_t sl = path.find_last_of(L"\\/");
        if (sl != std::wstring::npos) name += sl + 1;

        wchar_t lname[512];
        int nameLen = 0;
        while (name[nameLen] && nameLen < 511) {
            lname[nameLen] = static_cast<wchar_t>(towlower(name[nameLen]));
            ++nameLen;
        }
        lname[nameLen] = L'\0';

        MatchResult r;
        r.playlistIdx = playlistIdx;
        r.path        = path;
        r.score       = 0;
        r.posCount    = 0;

        if (hasWildcard) {
            if (!WildcardMatch(lq, lname)) return;
            m_results.push_back(std::move(r));
            return;
        }

        int ni = 0, qi = 0, pi = 0;
        while (ni < nameLen && qi < m_queryLen) {
            if (lname[ni] == lq[qi]) { r.positions[pi++] = ni; ++qi; }
            ++ni;
        }
        if (qi < m_queryLen) return;

        int score = 0;
        if (r.positions[0] == 0) score += 8;
        for (int k = 0; k < pi; ++k) {
            if (k > 0 && r.positions[k] == r.positions[k - 1] + 1) score += 10;
            if (r.positions[k] > 0) {
                wchar_t prev = lname[r.positions[k] - 1];
                if (prev == L'_' || prev == L'-' || prev == L'.' || prev == L' ')
                    score += 4;
            }
        }
        score -= (r.positions[pi - 1] - r.positions[0]);
        r.score    = score;
        r.posCount = pi;
        m_results.push_back(std::move(r));
    };

    for (int i = 0; i < static_cast<int>(app.playlist.size()); ++i)
        tryMatch(app.playlist[i], i);

    int extraStart = static_cast<int>(m_results.size());
    for (auto &p : extraPaths)
        tryMatch(p, -1);
    m_cachedExtraCount = static_cast<int>(m_results.size()) - extraStart;

    if (!hasWildcard) {
        std::sort(m_results.begin(), m_results.end(),
                  [](const MatchResult &a, const MatchResult &b) { return a.score > b.score; });
    }
}

void FindWnd::AdjustScroll() {
    if (m_results.empty()) return;
    m_selIdx = std::max(0, std::min(m_selIdx, static_cast<int>(m_results.size()) - 1));
    if (m_selIdx < m_rowScroll)
        m_rowScroll = m_selIdx;
    if (m_selIdx >= m_rowScroll + VISIBLE_ROWS)
        m_rowScroll = m_selIdx - VISIBLE_ROWS + 1;
}

void FindWnd::CommitOpen() {
    if (m_results.empty()) return;
    const MatchResult &mr = m_results[m_selIdx];
    Hide();
    if (mr.playlistIdx >= 0) {
        LoadImageIndex(m_hParent, mr.playlistIdx);
    } else {
        OpenSpecificImage(m_hParent, mr.path);
    }
    InvalidateRect(m_hParent, nullptr, FALSE);
}

// =============================================================================
//  Input
// =============================================================================

bool FindWnd::OnKeyDown(WPARAM vk, bool ctrl, bool shift, bool alt) {
    // Let Ctrl/Alt combos through to the parent so app shortcuts (e.g. Ctrl+F
    // to close this window) still work while the find box has focus.
    if (ctrl || alt) return false;

    (void)shift; // shift is passed through WM_CHAR with the translated character

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
            if (!m_results.empty()) {
                --m_selIdx;
                AdjustScroll();
                InvalidateRect(m_hWnd, nullptr, FALSE);
            }
            return true;

        case VK_DOWN:
            if (!m_results.empty()) {
                ++m_selIdx;
                AdjustScroll();
                InvalidateRect(m_hWnd, nullptr, FALSE);
            }
            return true;

        case VK_PRIOR:
            if (!m_results.empty()) {
                m_selIdx -= VISIBLE_ROWS;
                AdjustScroll();
                InvalidateRect(m_hWnd, nullptr, FALSE);
            }
            return true;

        case VK_NEXT:
            if (!m_results.empty()) {
                m_selIdx += VISIBLE_ROWS;
                AdjustScroll();
                InvalidateRect(m_hWnd, nullptr, FALSE);
            }
            return true;

        default:
            // Consume every other plain key so it never reaches the parent's
            // shortcut handler. The character arrives via WM_CHAR instead.
            return true;
    }
}

// =============================================================================
//  Paint
// =============================================================================

LRESULT FindWnd::HandlePanelMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {

    case WM_CHAR: {
        wchar_t ch = static_cast<wchar_t>(wParam);
        // Switch to Jump-to dialog when the trigger char is the first input
        if (ch == Constants::PANEL_SWITCH_TO_JUMP_CHAR && m_queryLen == 0) {
            Hide();
            PostMessageW(m_hParent, Constants::WM_QIV_SWITCH_TO_JUMP, 0, 0);
            return 0;
        }
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

        const int dpiKey = static_cast<int>(dpi * 96);
        if (dpiKey != m_cachedFontDpi) {
            if (m_hFontNorm) { DeleteObject(m_hFontNorm); m_hFontNorm = nullptr; }
            if (m_hFontBold) { DeleteObject(m_hFontBold); m_hFontBold = nullptr; }
            m_hFontNorm = CreateFontW(-fs, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                                      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                      CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                      DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            m_hFontBold = CreateFontW(-fsIn, 0, 0, 0, FW_BOLD, 0, 0, 0,
                                      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                      CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                      DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            m_cachedFontDpi = dpiKey;
        }
        HFONT hOldFont = static_cast<HFONT>(SelectObject(hdc, m_hFontNorm));

        const COLORREF clrLabel   = Constants::Theme::ThemedGray(0.90f, app.themeFactor);
        const COLORREF clrDim     = Constants::Theme::ThemedGray(0.45f, app.themeFactor);
        const COLORREF clrInput   = Constants::Theme::ThemedColor(0.39f, 0.78f, 1.0f, app.themeFactor);
        const COLORREF clrSelBg   = Constants::Theme::ThemedColor(0.15f, 0.35f, 0.60f, app.themeFactor);
        const COLORREF clrSelText = Constants::Theme::ThemedGray(1.00f, app.themeFactor);
        const COLORREF clrRowText = Constants::Theme::ThemedGray(0.85f, app.themeFactor);
        const COLORREF clrOrange  = Constants::Theme::ThemedColor(1.0f, 0.65f, 0.1f, app.themeFactor);
        const COLORREF clrYellow  = Constants::Theme::ThemedColor(1.0f, 0.87f, 0.0f, app.themeFactor);

        int y = pad;

        // ── Label ─────────────────────────────────────────────────────────────
        {
            wchar_t lbl[96];
            int total = static_cast<int>(app.playlist.size());
            int cached = app.renderer
                         ? static_cast<int>(app.renderer->GetCachedBitmaps().size())
                         : 0;
            int extra = cached - static_cast<int>(app.playlist.size());
            if (extra < 0) extra = 0;
            if (total > 0 && extra > 0)
                swprintf_s(lbl, L"Find in %d images  +  %d cached", total, extra);
            else if (total > 0)
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
        SelectObject(hdc, m_hFontBold);
        SetTextColor(hdc, m_queryLen > 0 ? clrInput : clrDim);

        wchar_t display[MAX_QUERY + 4];
        if (m_queryLen > 0) {
            wcsncpy_s(display, m_query, m_queryLen);
            display[m_queryLen]     = L'_';
            display[m_queryLen + 1] = L'\0';
        } else {
            wcscpy_s(display, L"filename or *.ext…");
        }

        RECT inRect = { boxRect.left + pad / 2, boxRect.top,
                        boxRect.right - pad / 2, boxRect.bottom };
        DrawTextW(hdc, display, -1, &inRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(hdc, m_hFontNorm);

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
            SetTextColor(hdc, clrDim);
            RECT r = { pad, y, rc.right - pad, y + rowH * VISIBLE_ROWS };
            DrawTextW(hdc, L"Start typing to filter by filename", -1, &r,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else if (m_results.empty()) {
            SetTextColor(hdc, clrOrange);
            RECT r = { pad, y, rc.right - pad, y + rowH };
            DrawTextW(hdc, L"No matches", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else {
            TEXTMETRIC tm;
            GetTextMetrics(hdc, &tm);

            int visible = std::min(VISIBLE_ROWS, static_cast<int>(m_results.size()) - m_rowScroll);
            for (int i = 0; i < visible; ++i) {
                int ri = m_rowScroll + i;
                bool selected = (ri == m_selIdx);
                const MatchResult &mr = m_results[ri];

                if (selected) {
                    HBRUSH selBrush = CreateSolidBrush(clrSelBg);
                    RECT fillRect = { 0, y, rc.right, y + rowH };
                    FillRect(hdc, &fillRect, selBrush);
                    DeleteObject(selBrush);
                }

                const std::wstring &fullPath = mr.path;
                auto sep = fullPath.rfind(L'\\');
                const wchar_t *fname = (sep == std::wstring::npos)
                                       ? fullPath.c_str() : fullPath.c_str() + sep + 1;
                int fnameLen = static_cast<int>(wcslen(fname));

                // Build highlight map from fuzzy match positions
                bool isHL[512] = {};
                for (int k = 0; k < mr.posCount; ++k)
                    if (mr.positions[k] < 512) isHL[mr.positions[k]] = true;

                int textX = pad + (selected ? pad / 2 : 0);
                int textY = y + (rowH - tm.tmHeight) / 2;
                RECT clip = { textX, y, rc.right - pad, y + rowH };
                const COLORREF clrBase = selected ? clrSelText : clrRowText;

                // Draw segments: batch consecutive chars of the same color
                int segStart = 0;
                bool segHL = (fnameLen > 0) && isHL[0];
                for (int ci = 1; ci <= fnameLen; ++ci) {
                    bool curHL = (ci < fnameLen) && isHL[ci];
                    if (curHL != segHL || ci == fnameLen) {
                        int segLen = ci - segStart;
                        SetTextColor(hdc, segHL ? clrYellow : clrBase);
                        ExtTextOutW(hdc, textX, textY, ETO_CLIPPED, &clip,
                                    fname + segStart, segLen, nullptr);
                        SIZE sz;
                        GetTextExtentPoint32W(hdc, fname + segStart, segLen, &sz);
                        textX += sz.cx;
                        segStart = ci;
                        segHL = curHL;
                    }
                }

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
            if (!m_results.empty()) {
                swprintf_s(hint, L"%d / %d    ↑↓ select  •  Enter open  •  Esc cancel",
                           m_selIdx + 1, static_cast<int>(m_results.size()));
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
        EndPaint(m_hWnd, &ps);
        return 0;
    }

    default:
        break;
    }
    return DefWindowProcW(m_hWnd, message, wParam, lParam);
}

} // namespace UI
