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
#include "../../Input/AppCommands.h" // RevealInExplorer / DeleteFileToRecycleBin
#include "../../Platform/FolderIndex.h" // every folder qIV knows
#include "../../Renderer/IRenderer.h"
#include "Common/FuzzyMatch.h"
#include "CustomControls/InputBox.h"
#include "../../WorkerThread.h" // g_ioWorker — the preview thumbnail is filesystem work

#include <shlobj_core.h> // SHCreateItemFromParsingName / IShellItemImageFactory
#include <wrl/client.h>
#include <memory>
#include <algorithm>
#include "Common/PreviewStrip.h" // which thumbnail a click landed on

extern AppState app;

// The selected row's thumbnail, ready to draw. Same mechanism ExifWnd uses, and
// deliberately the same SOURCE - Windows' own thumbnail - so this panel cannot
// disagree with the strips about what a file looks like.
static constexpr UINT WM_FIND_PREVIEW_READY = WM_APP + 117;

namespace UI {

// =============================================================================
//  Init / Show
// =============================================================================

void FindWnd::Init(HINSTANCE hInstance, HWND hParent) {
    // SIZED FOR A PATH, NOT A FILE NAME.
    //
    // 460 was right when every row was one file name from the folder already on
    // screen. Rows now carry the FOLDER as well - a cross-folder search hit, or
    // a duplicate group where the folder is the only thing telling three
    // identically named copies apart - and at that width the path was
    // ellipsised down to almost nothing.
    //
    // Taller for the same reason: duplicates arrive in groups, and a group of
    // three that does not fit on screen at once has to be scrolled to be
    // compared, which is the one thing the list exists to make easy.
    const int w = static_cast<int>(820.0f * app.dpiScale);
    // Taller again for the preview strip under the list - see the paint.
    const int h = static_cast<int>(690.0f * app.dpiScale);
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

void FindWnd::ShowList(std::vector<std::wstring> paths, std::vector<int> groupIds,
                      std::wstring heading) {
    if (!m_hWnd) return;

    // THE BOX IS RESET FIRST, and the list built after.
    //
    // Clear() fires OnChanged, which calls RebuildMatches - and RebuildMatches
    // clears the heading and refills m_results from the playlist. Doing it the
    // other way round would build the list and then immediately throw it away,
    // leaving the panel showing the current folder under a duplicates heading.
    m_inputBox.Clear();

    m_results.clear();
    m_results.reserve(paths.size());
    for (std::wstring &p : paths) {
        MatchResult r;
        // -1 on purpose: these paths come from other folders, and -1 is what
        // routes Enter through OpenSpecificImage rather than a playlist jump.
        r.playlistIdx = -1;
        r.path        = std::move(p);
        r.score       = 0;
        r.posCount    = 0;
        m_results.push_back(std::move(r));
    }

    // Aligned with m_results, or empty when the caller has no grouping - in
    // which case a row previews only itself.
    m_rowGroup = (groupIds.size() == m_results.size()) ? std::move(groupIds)
                                                       : std::vector<int>{};

    m_listHeading = std::move(heading);
    m_selIdx      = 0;
    m_rowScroll   = 0;

    ShowCenterOverParent();
    AdjustScroll();
    InvalidateRect(m_hWnd, nullptr, FALSE);
}

void FindWnd::RebuildMatches() {
    // Any rebuild means the user is searching again, so a supplied list is no
    // longer what is on screen.
    m_listHeading.clear();
    m_results.clear();

    // The grouping belonged to the OLD list. Left behind it is read by index
    // against the new one, so search hit 3 would inherit whatever group row 3 of
    // the duplicate list had - and preview four unrelated pictures as copies of
    // each other.
    m_rowGroup.clear();

    // And the pictures on screen are of rows that no longer exist. A query that
    // matches nothing would otherwise leave the previous thumbnails under a
    // "No matches" list, which reads as those being the matches.
    ClearPreviews();

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
    if (m_searchEverywhere && m_queryLen > 0) {
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

// Re-runs the current query. Used when the scope changed under an open panel -
// Ctrl+Shift+F pressed while Ctrl+F's results are on screen.
void FindWnd::ClearPreviews() {
    for (Preview &pv : m_previews)
        if (pv.bmp) DeleteObject(pv.bmp);
    m_previews.clear();
    m_previewGroup = -1;
}

// A thumbnail is looked up by PATH rather than by index, because the strip is
// capped at PREVIEW_MAX while the list is not: preview 3 is not row 3 once a
// group has more copies than the strip can hold.
int FindWnd::PreviewRowAt(int x, int y) const {
    const int slot = Common::PreviewStrip::SlotAt(
        x, y, m_prevLeftPx, m_prevTopPx, m_prevBoxPx, m_prevCellPx,
        static_cast<int>(m_previews.size()));
    if (slot < 0) return -1;

    for (size_t i = 0; i < m_results.size(); ++i)
        if (m_results[i].path == m_previews[slot].path) return static_cast<int>(i);
    return -1;
}

void FindWnd::RequestPreview() {
    if (m_results.empty()) return;
    if (m_selIdx < 0 || m_selIdx >= static_cast<int>(m_results.size())) return;

    // WHICH PICTURES BELONG WITH THE SELECTED ONE. In a duplicate list that is
    // its whole group, because "are these really the same picture" cannot be
    // answered by one thumbnail - the copies have to be seen together. Anywhere
    // else there is no grouping and a row stands for itself.
    const int group = (m_selIdx < static_cast<int>(m_rowGroup.size()))
                          ? m_rowGroup[m_selIdx]
                          : -1;

    // Already loaded. Arrowing WITHIN a group must not re-read the same files on
    // every keystroke - the pictures on screen are already the right ones.
    if (group >= 0 && group == m_previewGroup && !m_previews.empty()) return;

    ClearPreviews();
    m_previewGroup = group;

    std::vector<std::wstring> wanted;
    if (group < 0) {
        wanted.push_back(m_results[m_selIdx].path);
    } else {
        for (size_t i = 0; i < m_results.size() && wanted.size() < PREVIEW_MAX; ++i)
            if (i < m_rowGroup.size() && m_rowGroup[i] == group)
                wanted.push_back(m_results[i].path);
    }

    // The slots exist before any thumbnail arrives, so the answers can be placed
    // by path rather than by arrival order - they come back out of order, and a
    // slow drive must not shuffle the pictures under the names.
    for (std::wstring &w : wanted) {
        Preview pv;
        pv.path = std::move(w);
        m_previews.push_back(std::move(pv));
    }

    const HWND hwnd = m_hWnd;
    const LONG size = static_cast<LONG>(Constants::FIND_PREVIEW_SIZE * app.dpiScale);

    for (const Preview &pv : m_previews) {
        const std::wstring path = pv.path;

        // On the IO pool: a shell thumbnail can touch a slow or absent drive, and
        // a list that stopped responding while somebody arrows down it would be
        // worse than no preview at all.
        (void) g_ioWorker.PushTask([path, hwnd, size]() {
            Microsoft::WRL::ComPtr<IShellItem> item;
            if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item)))) return;

            Microsoft::WRL::ComPtr<IShellItemImageFactory> factory;
            if (FAILED(item->QueryInterface(IID_PPV_ARGS(&factory)))) return;

            HBITMAP bmp = nullptr;
            const SIZE sz = { size, size };
            if (FAILED(factory->GetImage(sz, static_cast<SIIGBF>(Constants::SHELL_THUMB_FLAGS), &bmp)) || !bmp)
                return;

            auto *sent = new std::wstring(path);
            if (!PostMessageW(hwnd, WM_FIND_PREVIEW_READY,
                              reinterpret_cast<WPARAM>(sent), reinterpret_cast<LPARAM>(bmp))) {
                DeleteObject(bmp);
                delete sent;
            }
        });
    }
}

void FindWnd::ShowRowMenu(int row) {
    if (row < 0 || row >= static_cast<int>(m_results.size())) return;
    const std::wstring path = m_results[row].path; // copied: the list may change under us

    enum : UINT { ID_OPEN = 1, ID_REVEAL, ID_RECYCLE };

    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, ID_OPEN,    L"Open");
    AppendMenuW(menu, MF_STRING, ID_REVEAL,  L"Show in Explorer");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_RECYCLE, L"Delete to Recycle Bin");

    POINT pt{};
    GetCursorPos(&pt);

    // TPM_RETURNCMD, so the command comes back here rather than as a WM_COMMAND
    // this panel would have to route. SetForegroundWindow first is the documented
    // requirement for a popup on a window that may not be active, without which
    // the menu can refuse to dismiss.
    SetForegroundWindow(m_hWnd);
    const UINT cmd = static_cast<UINT>(TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, m_hWnd, nullptr));
    DestroyMenu(menu);

    switch (cmd) {
        case ID_OPEN:
            CommitOpen();
            break;

        case ID_REVEAL:
            AppCommands::RevealInExplorer(path);
            break;

        case ID_RECYCLE: {
            AppCommands::DeleteFileToRecycleBin(path);

            // The row goes even if the delete was refused - it is checked below
            // by asking the filesystem rather than by trusting the call, because
            // a row still listing a file that is gone is the one thing that
            // makes somebody delete the WRONG copy next.
            if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
                m_results.erase(m_results.begin() + row);

                // ⚠ THE GROUP IDS ARE A PARALLEL ARRAY AND MUST BE ERASED WITH
                // IT. Left alone, every row below the deleted one reads the id
                // of the row above it: the numbers in the gutter shift, and -
                // far worse - RequestPreview then gathers the wrong set of
                // pictures as "this row's group" and shows copies of a
                // DIFFERENT picture as if they were this one's. That is the
                // exact mistake that ends in the wrong file being deleted next.
                if (row < static_cast<int>(m_rowGroup.size()))
                    m_rowGroup.erase(m_rowGroup.begin() + row);

                if (m_selIdx >= static_cast<int>(m_results.size()))
                    m_selIdx = static_cast<int>(m_results.size()) - 1;
                if (m_selIdx < 0) m_selIdx = 0;

                // The preview is of a file that no longer exists.
                // The pictures on screen include one that no longer exists, and
                // the group is now smaller - both are reasons to load it again
                // rather than patch what is displayed.
                ClearPreviews();

                AdjustScroll();
                InvalidateRect(m_hWnd, nullptr, FALSE);
            }
            break;
        }

        default:
            break; // dismissed
    }
}

void FindWnd::RefreshMatches() {
    RebuildMatches();
    m_selIdx = 0;
    m_rowScroll = 0;
    AdjustScroll();
    InvalidateRect(m_hWnd, nullptr, FALSE);
}

void FindWnd::AdjustScroll() {
    if (m_results.empty()) return;
    m_selIdx = std::max(0, std::min(m_selIdx, static_cast<int>(m_results.size()) - 1));

    // EVERY selection change funnels through here - the arrows, page keys, a
    // click, a rebuild - so the preview is asked for once, in one place, rather
    // than from each of those separately where one would eventually be missed.
    RequestPreview();
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

    case WM_FIND_PREVIEW_READY: {
        // OWNS BOTH. The worker handed over the bitmap and the path it belongs
        // to when the post succeeded; nothing else will free them.
        std::unique_ptr<std::wstring> forPath(reinterpret_cast<std::wstring *>(wParam));
        HBITMAP bmp = reinterpret_cast<HBITMAP>(lParam);

        // A LATE ANSWER FOR A ROW THE USER HAS LEFT IS DROPPED. Arrowing down a
        // list starts a request per row and they finish out of order; without
        // this check a slow drive's thumbnail would appear under whatever name
        // is selected by the time it lands.
        // PLACED BY PATH, never by arrival order. Several thumbnails are in
        // flight at once and they finish in whatever order the disk allows; a
        // slow one landing last must not end up under the wrong name.
        //
        // A path nobody is waiting for any more - the selection moved to another
        // group while it was loading - is simply dropped.
        bool placed = false;
        if (forPath && bmp) {
            for (Preview &pv : m_previews) {
                if (pv.path != *forPath) continue;
                if (pv.bmp) DeleteObject(pv.bmp);
                pv.bmp = bmp;
                BITMAP bm{};
                GetObject(bmp, sizeof(bm), &bm);
                pv.w = bm.bmWidth;
                pv.h = bm.bmHeight;
                placed = true;
                break;
            }
        }
        if (!placed) {
            if (bmp) DeleteObject(bmp);
            return 0;
        }

        InvalidateRect(m_hWnd, nullptr, FALSE);
        return 0;
    }

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

    case WM_LBUTTONDOWN: {
        // A CLICK ON A ROW SELECTS IT, which is what puts that picture in the
        // preview below. The list was keyboard-only - tolerable for a search you
        // are typing into, wrong for a list of duplicates: that list is READ,
        // and the natural way to inspect an entry is to click it.
        //
        // Tested BEFORE the input box sees the message, because a click in the
        // list is not a click in the box, and the box would otherwise swallow it
        // and move its caret instead.
        const int mx = GET_X_LPARAM(lParam);
        const int my = GET_Y_LPARAM(lParam);

        // A PICTURE IS A ROW. Clicking a thumbnail selects the copy it shows, so
        // the list highlight, the frame and the path all move together - having
        // to look at a picture and then hunt for its line above would undo the
        // reason the strip is there.
        const int pvRow = PreviewRowAt(mx, my);
        if (pvRow >= 0) {
            m_selIdx = pvRow;
            AdjustScroll();     // scrolls the list to it; the group is unchanged
            InvalidateRect(m_hWnd, nullptr, FALSE);
            return 0;
        }

        if (!m_results.empty() && m_rowHPx > 0 && my >= m_listTopPx) {
            const int row = m_rowScroll + (my - m_listTopPx) / m_rowHPx;
            if (row >= 0 && row < static_cast<int>(m_results.size()) &&
                row < m_rowScroll + VISIBLE_ROWS) {
                m_selIdx = row;
                AdjustScroll();     // also asks for that row's preview
                InvalidateRect(m_hWnd, nullptr, FALSE);
                return 0;
            }
        }

        if (m_inputBox.RouteMouse(WM_LBUTTONDOWN, wParam, lParam, m_hWnd) == InputResult::ConsumedRepaint)
            InvalidateRect(m_hWnd, nullptr, FALSE);
        return 0;
    }

    case WM_LBUTTONUP:
        // Ends a drag-select. Without it the box only drops m_dragging on the
        // next WM_MOUSEMOVE, so moving after release keeps extending the selection.
        if (m_inputBox.RouteMouse(WM_LBUTTONUP, wParam, lParam, m_hWnd) == InputResult::ConsumedRepaint)
            InvalidateRect(m_hWnd, nullptr, FALSE);
        return 0;

    case WM_RBUTTONUP: {
        // RIGHT-CLICK RESOLVES. Finding duplicates is only half the job; the
        // other half is doing something about one, and making the user leave
        // for Explorer to do it means losing the list that told them which copy
        // it was.
        const int mx = GET_X_LPARAM(lParam);
        const int my = GET_Y_LPARAM(lParam);

        // Right-clicking a picture resolves that copy. The menu has to be
        // reachable from whichever half of the panel the eye is on.
        const int pvRow = PreviewRowAt(mx, my);
        if (pvRow >= 0) {
            m_selIdx = pvRow;
            AdjustScroll();
            InvalidateRect(m_hWnd, nullptr, FALSE);
            UpdateWindow(m_hWnd);
            ShowRowMenu(pvRow);
            return 0;
        }

        if (!m_results.empty() && m_rowHPx > 0 && my >= m_listTopPx) {
            const int row = m_rowScroll + (my - m_listTopPx) / m_rowHPx;
            if (row >= 0 && row < static_cast<int>(m_results.size()) &&
                row < m_rowScroll + VISIBLE_ROWS) {
                // Select first, so the preview shows what the menu is about -
                // acting on a row you cannot see is how the wrong copy goes.
                m_selIdx = row;
                AdjustScroll();
                InvalidateRect(m_hWnd, nullptr, FALSE);
                UpdateWindow(m_hWnd);
                ShowRowMenu(row);
                return 0;
            }
        }

        if (m_inputBox.RouteMouse(WM_RBUTTONUP, wParam, lParam, m_hWnd) == InputResult::ConsumedRepaint)
            InvalidateRect(m_hWnd, nullptr, FALSE);
        return 0;
    }

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
            // THE SCOPE IS NAMED, not implied. Two keys open this panel and the
            // results differ completely between them; a user who cannot see
            // which one they pressed has to guess why a picture is missing.
            //
            // The wide form states the size of the index rather than the
            // playlist, because that is what is actually being searched, and it
            // is also the honest answer to "why did that not appear" - a folder
            // qIV has never opened is not in it.
            // A supplied list names itself; the search headings below describe
            // a search, which is not what is on screen.
            if (!m_listHeading.empty()) {
                swprintf_s(lbl, L"%s", m_listHeading.c_str());
            } else if (m_searchEverywhere) {
                const int indexed = static_cast<int>(Platform::FolderIndex::Count());
                if (indexed > 0)
                    swprintf_s(lbl, L"Find in ALL folders  ·  %d pictures indexed", indexed);
                else
                    swprintf_s(lbl, L"Find in ALL folders  ·  building the index…");
            } else if (total > 0 && extra > 0)
                swprintf_s(lbl, L"Find in this folder  ·  %d images  +  %d cached", total, extra);
            else if (total > 0)
                swprintf_s(lbl, L"Find in this folder  ·  %d images", total);
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
        m_listTopPx = listTop;   // for the click hit-test - see WM_LBUTTONDOWN
        m_rowHPx    = rowH;

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

                // ── THE GROUP NUMBER ──────────────────────────────────────────
                //
                // WHICH COPIES BELONG TOGETHER. A flat list of eight names is
                // unreadable as duplicates: it says every one of them is a
                // duplicate of SOMETHING and never which. "1. 1. 1. 2. 2." is
                // the whole answer, and it is answered by a gutter rather than
                // by blank separator lines, because a list that has to scroll
                // cannot spend rows on nothing.
                //
                // Only when the list IS grouped. A search result has no groups,
                // and an empty gutter there would be indentation with no meaning.
                if (!m_rowGroup.empty() && ri < static_cast<int>(m_rowGroup.size())) {
                    // Sized off the widest number actually in this list, so the
                    // names line up with each other and a two-digit group does
                    // not push its own row out of step with the rest.
                    wchar_t widest[16];
                    swprintf_s(widest, L"%d.", *std::max_element(m_rowGroup.begin(),
                                                                 m_rowGroup.end()) + 1);
                    SIZE gutSz{};
                    GetTextExtentPoint32W(hdc, widest, static_cast<int>(wcslen(widest)), &gutSz);

                    wchar_t num[16];
                    swprintf_s(num, L"%d.", m_rowGroup[ri] + 1);

                    // Right-aligned in the gutter: the dots form a column, which
                    // is what makes "same number" readable at a glance instead of
                    // something to compare digit by digit.
                    SetTextColor(hdc, selected ? clrSelText : clrLabel);
                    RECT nr = { textX, y, textX + gutSz.cx, y + rowH };
                    DrawTextW(hdc, num, -1, &nr,
                              DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

                    textX += gutSz.cx + static_cast<int>(10.0f * dpi);
                }

                RECT clip = { textX, y, rc.right - pad, y + rowH };

                Common::DrawMatchText(hdc, fname, fnameLen, isHL, textX, textY, clip,
                                      selected ? clrSelText : clrRowText, clrYellow);


                // THE FOLDER, for any row that is not in the current playlist.
                //
                // Without it a duplicate list is unusable: three rows reading
                // copy1.png, copy2.png, IMG_0042.png say nothing about WHICH
                // copy to keep, and the folder is the only thing telling them
                // apart. The same is true of a cross-folder search hit - "found
                // it" is half an answer if you cannot see where.
                //
                // Rows that ARE in the playlist all live in the folder already
                // on screen, so naming it on every line would be noise.
                if (mr.playlistIdx < 0 && sep != std::wstring::npos) {
                    const std::wstring folder = fullPath.substr(0, sep);

                    // Whatever room is left after the name, and the path is
                    // COMPRESSED to fit rather than dropped.
                    //
                    // DT_PATH_ELLIPSIS is the right tool: it removes from the
                    // MIDDLE, so both the drive and the last folder survive -
                    // "T:" and "dupA" are exactly what tells two copies apart,
                    // and they are the first things lost to a plain trailing
                    // ellipsis. A full path is wider than this panel almost
                    // always, so the first draft's "drop it if it does not fit"
                    // meant never drawing it at all.
                    SIZE nameSz{};
                    GetTextExtentPoint32W(hdc, fname, fnameLen, &nameSz);

                    const int gapPx    = static_cast<int>(12.0f * dpi);
                    const int folderL  = textX + nameSz.cx + gapPx;
                    const int minRoom  = static_cast<int>(60.0f * dpi);
                    if (rc.right - pad - folderL >= minRoom) {
                        SetTextColor(hdc, selected ? clrSelText : clrLabel);
                        RECT fr = { folderL, y, rc.right - pad, y + rowH };
                        DrawTextW(hdc, folder.c_str(), static_cast<int>(folder.size()), &fr,
                                  DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                                  DT_PATH_ELLIPSIS);
                    }
                }

                y += rowH;
            }
        }

        y = listTop + rowH * VISIBLE_ROWS + gap;

        // --- The selected picture -----------------------------------------------
        //
        // THE POINT OF THE WHOLE PANEL WHEN IT IS SHOWING DUPLICATES. Three
        // byte-identical files with different names in different folders: the
        // list says they are the same, and the only way to be comfortable
        // deleting one is to see what it is. A path is not an answer to "is this
        // the picture I think it is".
        //
        // Drawn for any selection that has one, so a cross-folder search hit
        // gets the same confirmation - "found it" and "found the right one" are
        // different claims.
        m_prevBoxPx = 0; // nothing hit-testable unless the strip is drawn below
        if (!m_previews.empty()) {
            const int box = static_cast<int>(Constants::FIND_PREVIEW_SIZE * dpi);
            const int cell = box + gap;
            const int count = static_cast<int>(m_previews.size());

            // Centred as a row, so a group of three reads as three pictures side
            // by side rather than one picture and some space.
            int px = (rc.right - (cell * count - gap)) / 2;
            const int py = y + gap;
            int tallest = 0;

            // Handed to the mouse handler - see PreviewRowAt.
            m_prevLeftPx = px;
            m_prevTopPx  = py;
            m_prevBoxPx  = box;
            m_prevCellPx = cell;

            for (int i = 0; i < count; ++i) {
                const Preview &pv = m_previews[i];

                // The SELECTED copy is framed. With several identical pictures on
                // screen the frame is the only thing saying which row the menu
                // and Enter will act on - without it, acting on "this one" is a
                // guess.
                const bool isSel = (m_selIdx < static_cast<int>(m_results.size())) &&
                                   (m_results[m_selIdx].path == pv.path);
                if (isSel) {
                    RECT fr = { px - 2, py - 2, px + box + 2, py + box + 2 };
                    FrameRect(hdc, &fr, UI::Gdi::Brush(clrOrange));
                }

                if (pv.bmp && pv.w > 0 && pv.h > 0) {
                    // Aspect preserved and never enlarged past the box: a
                    // stretched preview would misrepresent the very thing being
                    // compared.
                    const float scale = std::min(static_cast<float>(box) / pv.w,
                                                 static_cast<float>(box) / pv.h);
                    const int dstW = std::max(1, static_cast<int>(pv.w * scale));
                    const int dstH = std::max(1, static_cast<int>(pv.h * scale));

                    HDC hMem = CreateCompatibleDC(hdc);
                    HBITMAP prev = static_cast<HBITMAP>(SelectObject(hMem, pv.bmp));

                    // HALFTONE would look better and SILENTLY FAILS here: it
                    // needs a SetBrushOrgEx beside it, and without one StretchBlt
                    // returns 0 and draws nothing at all.
                    SetStretchBltMode(hdc, COLORONCOLOR);
                    StretchBlt(hdc, px + (box - dstW) / 2, py + (box - dstH) / 2,
                               dstW, dstH, hMem, 0, 0, pv.w, pv.h, SRCCOPY);
                    SelectObject(hMem, prev);
                    DeleteDC(hMem);
                    tallest = std::max(tallest, dstH);
                } else {
                    // Still loading. Saying so beats an empty rectangle that
                    // looks like a picture which failed to open.
                    SetTextColor(hdc, clrDim);
                    RECT lr = { px, py, px + box, py + box };
                    DrawTextW(hdc, L"…", -1, &lr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    tallest = std::max(tallest, box / 3);
                }
                px += cell;
            }

            y = py + std::max(tallest, box / 3) + gap;
        }

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
