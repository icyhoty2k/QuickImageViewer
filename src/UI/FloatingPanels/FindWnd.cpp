// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#include "FindWnd.h"
#include "UI/GdiPool.h" // pooled brushes and pens — never DeleteObject them
#include "../../AppState.h"
#include "../../Platform/Constants.h"
#include "../../Platform/ConstantsIcons.h"
#include "../../Platform/FileHandler.h"
#include "../../Platform/FolderIndex.h" // every folder qIV knows
#include "../../Renderer/IRenderer.h"
#include "Common/FuzzyMatch.h"
#include "CustomControls/InputBox.h"
#include <algorithm>

extern AppState app;

namespace UI {

// =============================================================================
//  Init / Show
// =============================================================================

void FindWnd::Init(HINSTANCE hInstance, HWND hParent) {
    const int w = static_cast<int>(460.0f * app.dpiScale);
    const int h = static_cast<int>(330.0f * app.dpiScale);
    InitFloating(hInstance, hParent, L"QivFindWndClass", L"Find Image", w, h);

    m_inputBox.SetPlaceholder(L"filename or *.ext…");
    m_inputBox.SetMaxLength(MAX_QUERY);
    m_inputBox.OnChanged = [this](const std::wstring& t) {
        int len = static_cast<int>(t.size());
        wcsncpy_s(m_query, t.c_str(), len);
        m_query[len] = L'\0';
        m_queryLen   = len;
        RebuildMatches();
        InvalidateRect(m_hWnd, nullptr, FALSE);
    };
}

void FindWnd::Init(HINSTANCE hInstance, HWND hParent, int8_t /*position*/) {
    Init(hInstance, hParent);
}

void FindWnd::Show() {
    if (!m_hWnd) return;

    m_inputBox.Clear(); // OnChanged → RebuildMatches (all items) + resets internal state

    ShowCenterOverParent();
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

    // Snapshot VRAM cache paths not already in the current playlist.
    // Done fresh on every RebuildMatches so it reflects the live cache.
    std::vector<std::wstring> extraPaths;
    if (app.renderer) {
        extraPaths.reserve(app.renderer->GetCachedBitmaps().size());
        for (auto &item : app.renderer->GetCachedBitmaps()) {
            if (app.playlistIndexMap.find(item.filePath) == app.playlistIndexMap.end())
                extraPaths.push_back(item.filePath);
        }
    }

    m_results.reserve(app.playlist.size() + extraPaths.size());

    // Empty query — show every item in playlist order, then VRAM-only extras.
    if (m_queryLen == 0) {
        for (int i = 0; i < static_cast<int>(app.playlist.size()); ++i) {
            MatchResult r;
            r.playlistIdx = i;
            r.path        = app.playlist[i];
            r.score       = 0;
            r.posCount    = 0;
            m_results.push_back(std::move(r));
        }
        int extraStart = static_cast<int>(m_results.size());
        for (auto &p : extraPaths) {
            MatchResult r;
            r.playlistIdx = -1;
            r.path        = p;
            r.score       = 0;
            r.posCount    = 0;
            m_results.push_back(std::move(r));
        }
        m_cachedExtraCount = static_cast<int>(m_results.size()) - extraStart;
        return;
    }

    // Lowercase copy of query
    wchar_t lq[MAX_QUERY + 2];
    Common::LowerCopy(m_query, m_queryLen, lq);

    // Wildcard mode: query contains '*' or '?'
    bool hasWildcard = false;
    hasWildcard = Common::IsWildcardQuery(lq, m_queryLen);

    // Helper: match one path and push result if it matches.
    auto tryMatch = [&](const std::wstring &path, int playlistIdx) {
        const wchar_t *name = path.c_str();
        size_t sl = path.find_last_of(L"\\/");
        if (sl != std::wstring::npos) name += sl + 1;

        wchar_t lname[512];
        int nameLen = static_cast<int>(wcsnlen(name, 511));
        Common::LowerCopy(name, nameLen, lname);

        MatchResult r;
        r.playlistIdx = playlistIdx;
        r.path        = path;
        r.score       = 0;
        r.posCount    = 0;

        if (hasWildcard) {
            if (!Common::WildcardMatch(lq, lname)) return;
            m_results.push_back(std::move(r));
            return;
        }

        Common::FuzzyMatchResult fm;
        if (!Common::FuzzyMatch(lq, m_queryLen, lname, nameLen, fm)) return;
        r.score    = fm.score;
        r.posCount = fm.posCount;
        for (int i = 0; i < fm.posCount; ++i) r.positions[i] = fm.positions[i];
        m_results.push_back(std::move(r));
    };

    for (int i = 0; i < static_cast<int>(app.playlist.size()); ++i)
        tryMatch(app.playlist[i], i);

    int extraStart = static_cast<int>(m_results.size());
    for (auto &p : extraPaths)
        tryMatch(p, -1);
    m_cachedExtraCount = static_cast<int>(m_results.size()) - extraStart;

    // --- EVERY OTHER FOLDER qIV KNOWS ---------------------------------------
    //
    // "Go to name" has always searched the folder you are standing in. This is
    // the same typing across every folder in the history, so a picture is found
    // by its name whether or not you remember where it lives - and opening one
    // takes you there.
    //
    // ONLY WHEN SOMETHING WAS TYPED. An empty query lists the current playlist,
    // and folding thousands of remembered files into that would replace a view
    // of "where I am" with a view of "everything", which is not what an empty
    // box means.
    //
    // Paths already in the playlist are skipped: they matched above WITH a
    // playlist index, and that index is what makes Enter jump within the folder
    // instead of reopening it.
    //
    // Capped, because a human reads this list. One letter can match tens of
    // thousands of names; the ones past the first screenful are neither read nor
    // useful, and the cap also bounds the sort below.
    if (m_queryLen > 0) {
        int crossFolder = 0;
        for (const auto &e : Platform::FolderIndex::Snapshot()) {
            if (crossFolder >= CROSS_FOLDER_MAX) break;
            if (app.playlistIndexMap.find(e.path) != app.playlistIndexMap.end()) continue;

            const size_t before = m_results.size();
            tryMatch(e.path, -1);
            if (m_results.size() != before) ++crossFolder;
        }
    }

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

// Esc: if the query box has text, clear it (same as the ✕ button) and keep the
// panel open. Empty box → return false so the base hides the panel.
bool FindWnd::OnLocalHide() {
    switch (m_inputBox.RouteKey(VK_ESCAPE, m_hWnd)) {
        case InputResult::RequestClear:
            InvalidateRect(m_hWnd, nullptr, FALSE);
            return true;
        default:
            return false; // RequestClose (empty) → base hides the panel
    }
}

bool FindWnd::OnKeyDown(WPARAM vk, bool ctrl, bool shift, bool alt) {
    (void)ctrl; (void)shift; (void)alt; // modifiers read via GetKeyState inside RouteKey

    // Host-specific keys first: result-list navigation + commit.
    switch (vk) {
        case VK_RETURN:
            CommitOpen();
            return true;
        case VK_UP:
            if (!m_results.empty()) { --m_selIdx; AdjustScroll(); InvalidateRect(m_hWnd, nullptr, FALSE); }
            return true;
        case VK_DOWN:
            if (!m_results.empty()) { ++m_selIdx; AdjustScroll(); InvalidateRect(m_hWnd, nullptr, FALSE); }
            return true;
        case VK_PRIOR:
            if (!m_results.empty()) { m_selIdx -= VISIBLE_ROWS; AdjustScroll(); InvalidateRect(m_hWnd, nullptr, FALSE); }
            return true;
        case VK_NEXT:
            if (!m_results.empty()) { m_selIdx += VISIBLE_ROWS; AdjustScroll(); InvalidateRect(m_hWnd, nullptr, FALSE); }
            return true;
    }

    // Everything else → the text box (editing, Ctrl+A/C/X/V, forward policy).
    switch (m_inputBox.RouteKey(vk, m_hWnd)) {
        case InputResult::Ignored:         return false; // forward to app pipeline
        case InputResult::RequestClose:    return false; // (Esc arrives via OnLocalHide, not here)
        case InputResult::RequestClear:    InvalidateRect(m_hWnd, nullptr, FALSE); return true;
        case InputResult::ConsumedRepaint: InvalidateRect(m_hWnd, nullptr, FALSE); return true;
        case InputResult::Consumed:        return true;
    }
    return true;
}

// =============================================================================
//  Back-buffer helpers
// =============================================================================

void FindWnd::EnsureBackBuffer(HDC refDC, int w, int h) {
    if (m_bbDC && w == m_bbW && h == m_bbH) return;
    DestroyBackBuffer();
    m_bbDC = CreateCompatibleDC(refDC);
    m_bbBmp = CreateCompatibleBitmap(refDC, w, h);
    m_bbBmpOld = static_cast<HBITMAP>(SelectObject(m_bbDC, m_bbBmp));
    m_bbW = w;
    m_bbH = h;
}

void FindWnd::DestroyBackBuffer() {
    if (m_bbDC) {
        if (m_bbBmpOld) SelectObject(m_bbDC, m_bbBmpOld);
        DeleteDC(m_bbDC);
        m_bbDC = nullptr;
    }
    if (m_bbBmp) {
        DeleteObject(m_bbBmp);
        m_bbBmp = nullptr;
    }
    m_bbBmpOld = nullptr;
    m_bbW = m_bbH = 0;
}

// =============================================================================
//  Paint
// =============================================================================

LRESULT FindWnd::HandlePanelMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_ERASEBKGND) return 1;
    switch (message) {

    case WM_CHAR: {
        wchar_t ch = static_cast<wchar_t>(wParam);
        if (ch == Constants::PANEL_SWITCH_TO_JUMP_CHAR && m_inputBox.IsEmpty()) {
            Hide();
            PostMessageW(m_hParent, Constants::WM_QIV_SWITCH_TO_JUMP, 0, 0);
            return 0;
        }
        if (m_inputBox.RouteChar(ch, m_hWnd) == InputResult::ConsumedRepaint)
            InvalidateRect(m_hWnd, nullptr, FALSE);
        return 0;
    }

    case WM_LBUTTONDOWN:
        if (m_inputBox.RouteMouse(WM_LBUTTONDOWN, wParam, lParam, m_hWnd) == InputResult::ConsumedRepaint)
            InvalidateRect(m_hWnd, nullptr, FALSE);
        return 0;

    case WM_LBUTTONUP:
        // Ends a drag-select. Without it the box only drops m_dragging on the
        // next WM_MOUSEMOVE, so moving after release keeps extending the selection.
        if (m_inputBox.RouteMouse(WM_LBUTTONUP, wParam, lParam, m_hWnd) == InputResult::ConsumedRepaint)
            InvalidateRect(m_hWnd, nullptr, FALSE);
        return 0;

    case WM_RBUTTONUP:
        if (m_inputBox.RouteMouse(WM_RBUTTONUP, wParam, lParam, m_hWnd) == InputResult::ConsumedRepaint)
            InvalidateRect(m_hWnd, nullptr, FALSE);
        return 0;

    case WM_MOUSEMOVE:
        if (m_inputBox.RouteMouse(WM_MOUSEMOVE, wParam, lParam, m_hWnd) == InputResult::ConsumedRepaint)
            InvalidateRect(m_hWnd, nullptr, FALSE);
        return 0;

    case WM_MOUSELEAVE:
        if (m_inputBox.RouteMouse(WM_MOUSELEAVE, wParam, lParam, m_hWnd) == InputResult::ConsumedRepaint)
            InvalidateRect(m_hWnd, nullptr, FALSE);
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC screenDC = BeginPaint(m_hWnd, &ps);
        RECT rc;
        GetClientRect(m_hWnd, &rc);
        EnsureBackBuffer(screenDC, rc.right, rc.bottom);
        HDC hdc = m_bbDC;

        // ── Background ───────────────────────────────────────────────────────
        FillRect(hdc, &rc, UI::Gdi::Brush(GetBgColor()));
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

        m_inputBox.Draw(hdc, m_hFontBold, boxRect, pad / 2, GetFocus() == m_hWnd);
        SelectObject(hdc, m_hFontNorm);

        y += boxH + gap;

        // ── Divider ───────────────────────────────────────────────────────────
        {
            HPEN old = static_cast<HPEN>(SelectObject(hdc,
                UI::Gdi::Pen(Constants::Theme::ThemedGray(0.22f, app.themeFactor))));
            MoveToEx(hdc, pad, y, nullptr);
            LineTo(hdc, rc.right - pad, y);
            SelectObject(hdc, old);
            y += gap;
        }

        // ── Match list ────────────────────────────────────────────────────────
        const int listTop = y;

        if (m_results.empty()) {
            const bool hasQuery = m_queryLen > 0;
            SetTextColor(hdc, hasQuery ? clrOrange : clrDim);
            RECT r = { pad, y, rc.right - pad, y + rowH * VISIBLE_ROWS };
            DrawTextW(hdc, hasQuery ? L"No matches" : L"No images in playlist",
                      -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else {
            TEXTMETRIC tm;
            GetTextMetrics(hdc, &tm);

            int visible = std::min(VISIBLE_ROWS, static_cast<int>(m_results.size()) - m_rowScroll);
            for (int i = 0; i < visible; ++i) {
                int ri = m_rowScroll + i;
                bool selected = (ri == m_selIdx);
                const MatchResult &mr = m_results[ri];

                if (selected) {
                    RECT fillRect = { 0, y, rc.right, y + rowH };
                    FillRect(hdc, &fillRect, UI::Gdi::Brush(clrSelBg));
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

                Common::DrawMatchText(hdc, fname, fnameLen, isHL, textX, textY, clip,
                                      selected ? clrSelText : clrRowText, clrYellow);

                y += rowH;
            }
        }

        y = listTop + rowH * VISIBLE_ROWS + gap;

        // ── Bottom divider ────────────────────────────────────────────────────
        {
            HPEN old = static_cast<HPEN>(SelectObject(hdc,
                UI::Gdi::Pen(Constants::Theme::ThemedGray(0.22f, app.themeFactor))));
            MoveToEx(hdc, pad, y, nullptr);
            LineTo(hdc, rc.right - pad, y);
            SelectObject(hdc, old);
            y += gap;
        }

        // ── Hint / count row ──────────────────────────────────────────────────
        {
            wchar_t hint[128];
            if (!m_results.empty()) {
                swprintf_s(hint, L"%d / %d    " QIV_ICON_ARROWS_UP_DOWN L" select  " QIV_ICON_BULLET
                                 L"  Enter open  " QIV_ICON_BULLET L"  Esc cancel",
                           m_selIdx + 1, static_cast<int>(m_results.size()));
            } else {
                wcscpy_s(hint, L"Esc cancel");
            }
            SetTextColor(hdc, clrDim);
            RECT r = { pad, y, rc.right - pad, y + fs + 4 };
            DrawTextW(hdc, hint, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        SelectObject(hdc, hOldFont);
        BitBlt(screenDC, 0, 0, rc.right, rc.bottom, hdc, 0, 0, SRCCOPY);
        EndPaint(m_hWnd, &ps);
        return 0;
    }

    default:
        break;
    }
    return DefWindowProcW(m_hWnd, message, wParam, lParam);
}

} // namespace UI
