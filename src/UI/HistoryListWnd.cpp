#include "HistoryListWnd.h"
#include "../Platform/Constants.h"
#include "../Platform/FileHandler.h"
#include "../AppState.h"
#include "../Input/Shortcuts.h"
#include "../Persistence/HistoryFoldersManager.h"
#include "../UI/UIManager.h"
#include <algorithm>
#include <windowsx.h>
#include <filesystem>
#include <dwmapi.h>

// Forward declarations from ThumbnailPanelWnd.cpp
namespace UI {
    extern void SetActivePanelWindow(HWND hWnd);

    extern HWND g_activePanelHwnd;
}

// ---------------------------------------------------------------------------
// HistoryListWnd.cpp  —  Last-visited folder history panel.
//
// DATA MODEL
//   historyFoldersManager.folderHistory — MRU vector, index 0 = most recent,
//                                         up to HISTORY_MAX_DIRS_TO_SAVE entries.
//   historyFoldersManager.favorites     — unordered_set for O(1) favorite lookup.
//
// DISPLAY MODEL
//   BuildDisplayList() assembles g_displayList from the MRU vector each time
//   the panel is shown or invalidated.  The display list respects
//   HISTORY_FAVORITES_POSITION (0=top, 1=bottom, 2=in-place) and is capped
//   at HISTORY_MAX_DIRS_TO_SHOW regular rows + HISTORY_MAX_FAVORITES_TO_SHOW
//   favorite rows.
//
// FILE STRATEGY
//   Append-only for new unique paths.  Full rewrite only on ToggleFavorite
//   or ClearHistoryKeepFavorites.
// ---------------------------------------------------------------------------

namespace UI {
    // ---------------------------------------------------------------------------
    // File-scope state
    // ---------------------------------------------------------------------------
    static HWND g_hHistOwner = nullptr;
    static int g_hoverRow = -1;
    static int g_scrollOffsetY = 0;
    static bool g_sbDragging = false;
    static int g_sbDragStartY = 0;
    static int g_sbDragStartOff = 0;
    static bool g_showFullHistory = false; // Ctrl+Tab toggles full history view
    static bool g_headerDragging = false;
    static int g_headerDragStartX = 0;     // screen X at drag start
    static int g_headerDragStartY = 0;     // screen Y at drag start
    static RECT g_headerDragWindowRect = {}; // window rect at drag start
    static int g_bodyTop = 0;    // updated each WM_PAINT — actual top of scrollable body
    static int g_bodyBottom = 0; // updated each WM_PAINT — actual bottom of scrollable body
    static int g_rowH = 0;       // updated each WM_PAINT — row height in pixels
    static HistoryFoldersManager historyFoldersManager;

    // Ctrl+Z undo state for single-row Delete
    static std::wstring g_lastDeletedPath;
    static int          g_lastDeletedIndex      = -1;
    static bool         g_lastDeletedWasFavorite = false;

    // ---------------------------------------------------------------------------
    // CalcTotalContentH — single formula for virtual scroll height used in
    // GetHistoryWindowBounds, WM_PAINT, WM_LBUTTONDOWN and WM_MOUSEMOVE.
    // Includes header area + rows + footer so window sizing and scroll are consistent.
    // ---------------------------------------------------------------------------
    static int CalcTotalContentH(int nEntries, UINT dpi) {
        int padding = MulDiv(Constants::History::HISTORY_PADDING, dpi, 96);
        int rowH    = MulDiv(Constants::History::HISTORY_ROW_HEIGHT, dpi, 96);
        int footerH = MulDiv(Constants::History::HISTORY_FONT_SIZE + 2 + 8, dpi, 96);
        return padding * 2
               + MulDiv(30 + Constants::History::HISTORY_FONT_SIZE + 4, dpi, 96)
               + MulDiv(8, dpi, 96)
               + nEntries * rowH
               + footerH;
    }

    // Convert a VK_Fx code to its display label ("F3", "F5", …).
    static std::wstring FKeyLabel(UINT vk) {
        if (vk >= VK_F1 && vk <= VK_F24)
            return L"F" + std::to_wstring(vk - VK_F1 + 1);
        UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        wchar_t buf[16] = {};
        GetKeyNameTextW(static_cast<LONG>(sc) << 16, buf, 16);
        return buf;
    }
    static std::vector<RECT> g_rowRects;
    static std::vector<RECT> g_indexRects; // clickable rects for directory indexes (parallel to g_displayList)
    static RECT g_exeLinkRect = {}; // clickable rect for the "QIV" exe-dir link
    static RECT g_f5IndexRect = {}; // clickable rect for F5 directory index
    static RECT g_cacheIndexRect = {}; // clickable rect for Cache index

    // Display list: what the panel actually renders.
    // Each entry is (path, isFavorite).
    struct DisplayEntry {
        std::wstring path;
        bool isFavorite;
    };

    static std::vector<DisplayEntry> g_displayList;

    // ---------------------------------------------------------------------------
    // BuildDisplayList
    //   Rebuilds g_displayList from the current MRU vector + favorites set.
    //   Respects HISTORY_FAVORITES_POSITION and per-category display caps.
    // ---------------------------------------------------------------------------
    static void BuildDisplayList() {
        g_displayList.clear();

        const auto &history = historyFoldersManager.folderHistory;
        const auto &favSet = historyFoldersManager.favorites;
        const int favPos = Constants::History::HISTORY_FAVORITES_POSITION;
        // Use unlimited caps when showing full history, otherwise use the constants
        const int maxNormal = g_showFullHistory ? INT_MAX : Constants::History::HISTORY_MAX_DIRS_TO_SHOW;
        const int maxFavs = g_showFullHistory ? INT_MAX : Constants::History::HISTORY_MAX_FAVORITES_TO_SHOW;

        if (favPos == 2) {
            // In-place: iterate MRU order, count normals and favs separately
            int normalCount = 0;
            int favCount = 0;
            for (const auto &path: history) {
                bool isFav = (favSet.count(path) > 0);
                if (isFav) {
                    if (favCount >= maxFavs) continue;
                    ++favCount;
                } else {
                    if (normalCount >= maxNormal) continue;
                    ++normalCount;
                }
                g_displayList.push_back({path, isFav});
                if (normalCount >= maxNormal && favCount >= maxFavs)
                    break;
            }
        } else {
            // Separate favorites and normals, then combine
            std::vector<DisplayEntry> favRows;
            std::vector<DisplayEntry> normalRows;

            for (const auto &path: history) {
                bool isFav = (favSet.count(path) > 0);
                if (isFav && static_cast<int>(favRows.size()) < maxFavs)
                    favRows.push_back({path, true});
                else if (!isFav && static_cast<int>(normalRows.size()) < maxNormal)
                    normalRows.push_back({path, false});
            }

            if (favPos == 0) {
                // Favorites on top
                for (auto &e: favRows) g_displayList.push_back(e);
                for (auto &e: normalRows) g_displayList.push_back(e);
            } else {
                // Favorites on bottom
                for (auto &e: normalRows) g_displayList.push_back(e);
                for (auto &e: favRows) g_displayList.push_back(e);
            }
        }
    }

    // ---------------------------------------------------------------------------
    // Free functions — used by FileHandler, UIManager, CommandExecuter
    // ---------------------------------------------------------------------------

    void LoadFolderHistoryFromDisk() {
        historyFoldersManager.LoadHistoryFromDisk();
    }

    void PushFolderHistory(const std::wstring &folderPath) {
        if (folderPath.empty())
            return;

        auto &history = historyFoldersManager.folderHistory;

        // Check if this path is already known
        auto it = std::find(history.begin(), history.end(), folderPath);
        bool isNew = (it == history.end());

        if (!isNew) {
            // Already exists: promote to front (MRU), no file write needed
            std::wstring tmp = *it;
            history.erase(it);
            history.insert(history.begin(), tmp);
            return;
        }

        // Genuinely new: prepend to RAM list
        history.insert(history.begin(), folderPath);

        // Cap RAM at HISTORY_MAX_DIRS_TO_SAVE
        if (static_cast<int>(history.size()) > Constants::History::HISTORY_MAX_DIRS_TO_SAVE)
            history.resize(static_cast<size_t>(Constants::History::HISTORY_MAX_DIRS_TO_SAVE));

        // Append-only to disk — no rewrite
        historyFoldersManager.AppendNewFolderToDisk(folderPath);
    }

    void ToggleFavorite(int rowIndex) {
        if (rowIndex < 0 || rowIndex >= static_cast<int>(g_displayList.size()))
            return;

        const std::wstring &path = g_displayList[rowIndex].path;
        auto &favSet = historyFoldersManager.favorites;

        if (favSet.count(path) > 0) {
            favSet.erase(path);
        } else {
            // Enforce max favorites cap — silently ignore if already full
            if (static_cast<int>(favSet.size()) >= Constants::History::HISTORY_MAX_FAVORITES_TO_SHOW)
                return;
            favSet.insert(path);
        }

        // Only rewrite the small favorites file — history file is untouched
        historyFoldersManager.RewriteFavoritesToDisk();
    }

    void ClearHistoryKeepFavorites() {
        // Backup first — before any RAM or file change
        historyFoldersManager.BackupHistoryToDisk();

        auto &history = historyFoldersManager.folderHistory;
        const auto &favSet = historyFoldersManager.favorites;

        // Remove all non-favorite entries from the MRU vector
        history.erase(
                std::remove_if(history.begin(), history.end(),
                               [&](const std::wstring &p) {
                                   return favSet.count(p) == 0;
                               }),
                history.end()
                );

        // Rewrite history file only — favorites file is untouched
        historyFoldersManager.RewriteHistoryToDisk();
        g_hoverRow = -1;
    }

    void ClearFavoritesKeepHistory() {
        // Backup first — before any RAM or file change
        historyFoldersManager.BackupFavoritesToDisk();

        historyFoldersManager.favorites.clear();

        // Rewrite favorites file only — history file is untouched
        historyFoldersManager.RewriteFavoritesToDisk();
        g_hoverRow = -1;
    }

    const std::vector<std::wstring> &GetFolderHistory() {
        return historyFoldersManager.folderHistory;
    }

    // ---------------------------------------------------------------------------
    // GetHistoryWindowBounds  —  centered on the parent's monitor
    // ---------------------------------------------------------------------------
    static void GetHistoryWindowBounds(HWND hRef, int &x, int &y, int &w, int &h) {
        HMONITOR hMonitor = MonitorFromWindow(hRef, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = {sizeof(mi)};
        GetMonitorInfoW(hMonitor, &mi);

        int monX = mi.rcMonitor.left;
        int monY = mi.rcMonitor.top;
        int monW = mi.rcMonitor.right - mi.rcMonitor.left;
        int monH = mi.rcMonitor.bottom - mi.rcMonitor.top;

        UINT dpi = static_cast<UINT>(app.dpiScale * 96.0f);

        int entries = std::max(1, static_cast<int>(g_displayList.size()));
        int totalH  = CalcTotalContentH(entries, dpi);

        w = static_cast<int>(monW * 0.30f);
        h = std::min(totalH, static_cast<int>(monH * 0.80f));
        x = monX + (monW - w) / 2;
        y = monY + (monH - h) / 2;
    }

    // ---------------------------------------------------------------------------
    // Window procedure
    // ---------------------------------------------------------------------------
    LRESULT HistoryListWnd::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
        // Handle focus events before the main switch
        if (message == WM_SETFOCUS) {
            UI::SetActivePanelWindow(m_hWnd);
            return 0;
        }
        if (message == WM_KILLFOCUS) {
            // Invalidate to remove active border when focus is lost
            InvalidateRect(m_hWnd, nullptr, FALSE);
            return 0;
        }

        switch (message) {
            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC hdc = BeginPaint(m_hWnd, &ps);
                RECT rc;
                GetClientRect(m_hWnd, &rc);

                UINT dpi = static_cast<UINT>(app.dpiScale * 96.0f);
                int padding = MulDiv(Constants::History::HISTORY_PADDING, dpi, 96);
                int rowH = MulDiv(Constants::History::HISTORY_ROW_HEIGHT, dpi, 96);
                int fontSize = MulDiv(Constants::History::HISTORY_FONT_SIZE, dpi, 96);
                int titleSz = MulDiv(Constants::History::HISTORY_FONT_SIZE + 2, dpi, 96);
                int indexW = MulDiv(28, dpi, 96);
                int starW = MulDiv(18, dpi, 96);

                // Scrollbar geometry — computed before any drawing.
                int SB_W = static_cast<int>(
                    MulDiv(Constants::History::SCROLLBAR_THICKNESS, dpi, 96));
                int totalContentH = CalcTotalContentH(
                    static_cast<int>(g_displayList.size()), dpi);
                int windowH = rc.bottom - rc.top;
                int maxScroll = std::max(0, totalContentH - windowH);
                g_scrollOffsetY = std::clamp(g_scrollOffsetY, 0, maxScroll);
                bool needsScrollbar = (maxScroll > 0);

                // Background — use active color if this panel is active
                COLORREF bgColor = Constants::Theme::ThemedGray(Constants::Theme::Panel::BACKGROUND_INACTIVE, app.themeFactor);
                if (UI::g_activePanelHwnd == m_hWnd) {
                    bgColor = Constants::Theme::ThemedGray(Constants::Theme::Panel::BACKGROUND_ACTIVE, app.themeFactor);
                }
                HBRUSH hBg = CreateSolidBrush(bgColor);
                FillRect(hdc, &rc, hBg);
                DeleteObject(hBg);
                SetBkMode(hdc, TRANSPARENT);

                int listFontSz = MulDiv(Constants::History::HISTORY_LIST_FONT_SIZE, dpi, 96);
                HFONT hTitleFont = CreateFontW(
                        titleSz, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
                        CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
                HFONT hBodyFont = CreateFontW(
                        fontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
                        CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
                HFONT hListFont = CreateFontW(
                        listFontSz, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
                        CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");

                // Title (fixed — not scrolled)
                SelectObject(hdc, hTitleFont);
                SetTextColor(hdc, RGB(100, 200, 255));
                int totalSaved = static_cast<int>(historyFoldersManager.folderHistory.size());
                int totalShown = static_cast<int>(g_displayList.size());
                std::wstring title = L"Folder History  (showing "
                                     + std::to_wstring(totalShown) + L" of "
                                     + std::to_wstring(totalSaved) + L" saved)  \x2605=Space(toggle fav) , CTRL+Tab=full list";
                RECT titleRect = {
                    rc.left + padding, rc.top + padding,
                    rc.right - padding, rc.top + padding + titleSz + 4
                };
                DrawTextW(hdc, title.c_str(), -1, &titleRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

                // Hint line 2: all shortcuts in gray (Del + Ctrl+Z added; QIV link moved to footer)
                {
                    SelectObject(hdc, hBodyFont);
                    int lineTop = titleRect.bottom + MulDiv(2, dpi, 96);
                    int lineBot = lineTop + fontSize + 2;

                    constexpr wchar_t SHORTCUTS[] =
                        L"Ctrl+Shift+Del = clear history"
                        L"     Ctrl+Alt+Shift+Del = clear favorites"
                        L"     Del = delete entry"
                        L"     Ctrl+Z = restore";
                    SetTextColor(hdc, RGB(150, 150, 150));
                    RECT scRect = { rc.left + padding, lineTop, rc.right - padding, lineBot };
                    DrawTextW(hdc, SHORTCUTS, -1, &scRect,
                              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

                    g_exeLinkRect = {}; // link now lives in footer
                    SelectObject(hdc, hTitleFont);
                }

                // Separator (fixed) — placed after hint line
                int sepY = titleRect.bottom + MulDiv(2 + fontSize + 2 + 4, dpi, 96);
                HPEN hPen = CreatePen(PS_SOLID, 1, RGB(50, 50, 50));
                HPEN hOldPen = (HPEN) SelectObject(hdc, hPen);
                MoveToEx(hdc, rc.left + padding, sepY, nullptr);
                LineTo(hdc, rc.right - padding, sepY);
                SelectObject(hdc, hOldPen);
                DeleteObject(hPen);

                int footerSepY = rc.bottom - MulDiv(fontSize + 2 + 8, dpi, 96);

                // Rows (scrolled) — clipped to the body area between separator and footer
                SelectObject(hdc, hListFont);
                g_rowRects.clear();
                g_indexRects.clear();
                int rowsTop = sepY + MulDiv(6, dpi, 96);
                int bodyBottom = footerSepY;
                g_bodyTop = rowsTop;
                g_bodyBottom = bodyBottom;
                g_rowH = rowH;

                SaveDC(hdc);
                IntersectClipRect(hdc, rc.left, rowsTop, rc.right, bodyBottom);

                // Create link font for indexes
                HFONT hIndexLinkFont = CreateFontW(
                    listFontSz, 0, 0, 0, FW_NORMAL, FALSE, TRUE, FALSE,
                    DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
                    CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");

                if (g_displayList.empty()) {
                    int y = rowsTop - g_scrollOffsetY;
                    SetTextColor(hdc, RGB(100, 100, 100));
                    RECT emptyRect = {rc.left + padding, y, rc.right - padding, y + rowH};
                    DrawTextW(hdc, L"No folders visited yet.", -1, &emptyRect,
                              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                } else {
                    for (int i = 0; i < static_cast<int>(g_displayList.size()); ++i) {
                        int rowTop = rowsTop - g_scrollOffsetY + i * rowH;
                        int rowBottom = rowTop + rowH;

                        const DisplayEntry &entry = g_displayList[i];
                        // Always push — index must match g_displayList for hit-testing.
                        g_rowRects.push_back({rc.left, rowTop, rc.right, rowBottom});

                        // Skip drawing rows outside the visible area.
                        if (rowBottom <= rowsTop || rowTop >= rc.bottom) {
                            g_indexRects.push_back({0, 0, 0, 0}); // placeholder for off-screen row
                            continue;
                        }

                        RECT rowRect = {rc.left, rowTop, rc.right, rowBottom};

                        // Hover background
                        if (i == g_hoverRow) {
                            HBRUSH hHover = CreateSolidBrush(
                                    entry.isFavorite ? RGB(50, 50, 10) : RGB(40, 60, 80));
                            FillRect(hdc, &rowRect, hHover);
                            DeleteObject(hHover);
                        }

                        // Row index number — blue, underlined, clickable
                        SetTextColor(hdc, RGB(100, 180, 255));
                        SelectObject(hdc, hIndexLinkFont);
                        std::wstring idxStr = std::to_wstring(i + 1);
                        RECT idxRect = {
                            rc.left + padding, rowTop,
                            rc.left + padding + indexW, rowBottom
                        };
                        DrawTextW(hdc, idxStr.c_str(), -1, &idxRect,
                                  DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
                        // Store index rect for click detection
                        g_indexRects.push_back(idxRect);
                        SelectObject(hdc, hListFont);

                        // Favorite star
                        if (entry.isFavorite) {
                            SetTextColor(hdc, RGB(255, 220, 0));
                            RECT starRect = {
                                rc.left + padding + indexW + MulDiv(4, dpi, 96), rowTop,
                                rc.left + padding + indexW + MulDiv(4, dpi, 96) + starW, rowBottom
                            };
                            DrawTextW(hdc, L"\x2605", -1, &starRect,
                                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                        }

                        // Path text — three segments: drive, middle, folder
                        const bool isHov = (i == g_hoverRow);
                        const bool isFav = entry.isFavorite;

                        COLORREF driveColor  = isFav
                            ? (isHov ? Constants::Theme::HistoryPanel::PATH_DRIVE_FAV_HOVER   : Constants::Theme::HistoryPanel::PATH_DRIVE_FAV)
                            : (isHov ? Constants::Theme::HistoryPanel::PATH_DRIVE_HOVER        : Constants::Theme::HistoryPanel::PATH_DRIVE);
                        COLORREF middleColor = isFav
                            ? (isHov ? RGB(255, 255, 160) : RGB(255, 240, 120))
                            : (isHov ? RGB(255, 255, 255) : RGB(200, 200, 200));
                        COLORREF folderColor = isFav
                            ? (isHov ? Constants::Theme::HistoryPanel::PATH_FOLDER_FAV_HOVER  : Constants::Theme::HistoryPanel::PATH_FOLDER_FAV)
                            : (isHov ? Constants::Theme::HistoryPanel::PATH_FOLDER_HOVER       : Constants::Theme::HistoryPanel::PATH_FOLDER);

                        // Split path into (drive, middle, folder)
                        const std::wstring& fp = entry.path;
                        std::wstring segDrive, segMiddle, segFolder;
                        if (fp.size() >= 2 && fp[1] == L':') {
                            segDrive = fp.substr(0, 2);
                            size_t lastSep = fp.find_last_of(L"\\/");
                            if (lastSep != std::wstring::npos && lastSep >= 2) {
                                segMiddle = fp.substr(2, lastSep - 1); // "\rest\of\path\"
                                segFolder = fp.substr(lastSep + 1);    // "FolderName"
                            } else {
                                segFolder = fp.substr(2);
                            }
                        } else {
                            segMiddle = fp;
                        }

                        // Append spawned DirWnd position label if this folder has one open
                        std::wstring posLabel = uiManager.GetSpawnedDirWndPositionLabel(fp);
                        segFolder += posLabel;

                        LONG rowLeft  = rc.left + padding + indexW + starW + MulDiv(10, dpi, 96);
                        LONG rowRight = rc.right - padding - (needsScrollbar ? SB_W + 2 : 0);
                        LONG curX     = rowLeft;

                        // 1. Drive letter
                        if (!segDrive.empty() && curX < rowRight) {
                            SetTextColor(hdc, driveColor);
                            SIZE sz = {};
                            GetTextExtentPoint32W(hdc, segDrive.c_str(),
                                                  static_cast<int>(segDrive.size()), &sz);
                            RECT dr = { curX, rowTop, curX + sz.cx, rowBottom };
                            DrawTextW(hdc, segDrive.c_str(), -1, &dr,
                                      DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                            curX += sz.cx;
                        }

                        // 2. Middle path — reserve space for folder before drawing
                        if (!segMiddle.empty() && curX < rowRight) {
                            LONG folderReserve = 0;
                            if (!segFolder.empty()) {
                                SIZE szF = {};
                                GetTextExtentPoint32W(hdc, segFolder.c_str(),
                                                      static_cast<int>(segFolder.size()), &szF);
                                folderReserve = std::min(szF.cx, (rowRight - curX) * 2 / 5);
                            }
                            LONG midRight = rowRight - folderReserve;
                            SetTextColor(hdc, middleColor);
                            SIZE szM = {};
                            GetTextExtentPoint32W(hdc, segMiddle.c_str(),
                                                  static_cast<int>(segMiddle.size()), &szM);
                            RECT mr = { curX, rowTop, midRight, rowBottom };
                            DrawTextW(hdc, segMiddle.c_str(), -1, &mr,
                                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                            curX = (szM.cx < midRight - curX) ? curX + szM.cx : midRight;
                        }

                        // 3. Folder name
                        if (!segFolder.empty() && curX < rowRight) {
                            SetTextColor(hdc, folderColor);
                            RECT fr = { curX, rowTop, rowRight, rowBottom };
                            DrawTextW(hdc, segFolder.c_str(), -1, &fr,
                                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                        }
                    }
                }

                RestoreDC(hdc, -1);

                // Scrollbar (drawn only in body area between header and footer)
                if (needsScrollbar) {
                    int sbX = rc.right - SB_W;
                    int bodyHeight = bodyBottom - rowsTop;

                    HBRUSH hTrack = CreateSolidBrush(
                            Constants::Theme::ThemedGray(Constants::Theme::HistoryPanel::SCROLLBAR_TRACK, app.themeFactor));
                    RECT sbTrack = {sbX, rowsTop, rc.right, bodyBottom};
                    FillRect(hdc, &sbTrack, hTrack);
                    DeleteObject(hTrack);

                    float visibleFrac = static_cast<float>(bodyHeight) / static_cast<float>(totalContentH);
                    int minThumb = MulDiv(static_cast<int>(Constants::History::SCROLLBAR_MIN_THUMB), dpi, 96);
                    int thumbLen = std::max(minThumb, static_cast<int>(bodyHeight * visibleFrac));
                    int thumbOff = static_cast<int>(
                        static_cast<float>(g_scrollOffsetY) / static_cast<float>(maxScroll)
                        * static_cast<float>(bodyHeight - thumbLen));
                    thumbOff = std::clamp(thumbOff, 0, bodyHeight - thumbLen);

                    HBRUSH hThumb = CreateSolidBrush(
                            Constants::Theme::ThemedGray(Constants::Theme::HistoryPanel::SCROLLBAR_THUMB, app.themeFactor));
                    RECT sbThumb = {sbX, rowsTop + thumbOff, rc.right, rowsTop + thumbOff + thumbLen};
                    FillRect(hdc, &sbThumb, hThumb);
                    DeleteObject(hThumb);
                }

                // Footer separator line
                {
                    HPEN hFooterPen = CreatePen(PS_SOLID, 1, RGB(50, 50, 50));
                    HPEN hOldFooterPen = (HPEN) SelectObject(hdc, hFooterPen);
                    MoveToEx(hdc, rc.left + padding, footerSepY, nullptr);
                    LineTo(hdc, rc.right - padding, footerSepY);
                    SelectObject(hdc, hOldFooterPen);
                    DeleteObject(hFooterPen);
                }

                // FOOTER — Panel status (left) and file size (right)
                {
                    int footerTop = rc.bottom - MulDiv(fontSize + 2 + 4, dpi, 96);
                    int footerBot = rc.bottom;

                    SelectObject(hdc, hBodyFont);
                    SetTextColor(hdc, RGB(150, 150, 150));

                    // LEFT: Panel status
                    {
                        LONG curX = rc.left + padding;
                        LONG midBound = rc.left + (rc.right - rc.left) / 2;

                        HFONT hLinkFont = CreateFontW(
                            fontSize, 0, 0, 0, FW_NORMAL, FALSE, TRUE, FALSE,
                            DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");

                        const UI::PanelLayout &layout = uiManager.GetLayout();
                        const UI::SlotInfo *slots[] = {&layout.center, &layout.top, &layout.right, &layout.bottom, &layout.left};

                        // Cache toggle key label (derived from Shortcuts constant)
                        std::wstring cacheKeyLabel = L"[" + FKeyLabel(Shortcuts::SC_PANEL_CACHE_TOGGLE) + L"]";
                        SetTextColor(hdc, RGB(100, 180, 255));
                        SelectObject(hdc, hLinkFont);
                        RECT cacheToggleRect = {curX, footerTop, midBound, footerBot};
                        DrawTextW(hdc, cacheKeyLabel.c_str(), -1, &cacheToggleRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                        SIZE szToggle = {};
                        GetTextExtentPoint32W(hdc, cacheKeyLabel.c_str(),
                                             static_cast<int>(cacheKeyLabel.size()), &szToggle);
                        g_cacheIndexRect = {curX, footerTop, curX + szToggle.cx, footerBot};
                        curX += szToggle.cx;

                        // Cache status
                        SelectObject(hdc, hBodyFont);
                        SetTextColor(hdc, RGB(150, 150, 150));
                        bool cacheFound = false;
                        std::wstring cachePosName;
                        for (auto *slot : slots) {
                            if (slot->panel == &uiManager.getCacheWindow() && slot->panel->IsVisible()) {
                                cachePosName = slot->name;
                                cacheFound = true;
                                break;
                            }
                        }
                        std::wstring cacheRest = cacheFound ? (L"Cache -> " + cachePosName) : L"Cache -> Hidden";
                        RECT cacheRestRect = {curX, footerTop, midBound, footerBot};
                        DrawTextW(hdc, cacheRest.c_str(), -1, &cacheRestRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

                        // Dir-panel toggle key label (derived from Shortcuts constant)
                        std::wstring dirKeyLabel = L"[" + FKeyLabel(Shortcuts::SC_PANEL_DIR_TOGGLE) + L"]";
                        curX = rc.left + padding + MulDiv(100, dpi, 96);
                        SetTextColor(hdc, RGB(100, 180, 255));
                        SelectObject(hdc, hLinkFont);
                        RECT f5ToggleRect = {curX, footerTop, midBound, footerBot};
                        DrawTextW(hdc, dirKeyLabel.c_str(), -1, &f5ToggleRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                        SIZE szF5 = {};
                        GetTextExtentPoint32W(hdc, dirKeyLabel.c_str(),
                                             static_cast<int>(dirKeyLabel.size()), &szF5);
                        g_f5IndexRect = {curX, footerTop, curX + szF5.cx, footerBot};
                        curX += szF5.cx;

                        // F5 status
                        SelectObject(hdc, hBodyFont);
                        SetTextColor(hdc, RGB(150, 150, 150));
                        bool f5Found = false;
                        std::wstring f5PosName;
                        int f5HistoryIndex = -1;
                        for (auto *slot : slots) {
                            if (slot->panel == &uiManager.getDirWindow() && slot->panel->IsVisible()) {
                                if (app.currentIndex >= 0 && app.currentIndex < static_cast<int>(app.playlist.size())) {
                                    std::wstring currentImagePath = app.playlist[app.currentIndex];
                                    std::wstring currentFolder = std::filesystem::path(currentImagePath).parent_path().wstring();
                                    const auto &history = historyFoldersManager.folderHistory;
                                    for (int i = 0; i < static_cast<int>(history.size()); ++i) {
                                        try {
                                            if (std::filesystem::equivalent(currentFolder, history[i])) {
                                                f5HistoryIndex = i + 1;
                                                break;
                                            }
                                        } catch (...) {}
                                    }
                                }
                                f5PosName = slot->name;
                                f5Found = true;
                                break;
                            }
                        }
                        if (f5Found) {
                            std::wstring f5Rest = (f5HistoryIndex > 0) ? (L" #" + std::to_wstring(f5HistoryIndex) + L" " + f5PosName) : (L" #? " + f5PosName);
                            RECT f5RestRect = {curX, footerTop, midBound, footerBot};
                            DrawTextW(hdc, f5Rest.c_str(), -1, &f5RestRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                        } else {
                            RECT f5HiddenRect = {curX, footerTop, midBound, footerBot};
                            DrawTextW(hdc, L" Hidden", -1, &f5HiddenRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                        }

                        DeleteObject(hLinkFont);
                    }

                    // RIGHT: QIV→dir link + file size
                    {
                        LONG rightHalfLeft = rc.left + (rc.right - rc.left) / 2;
                        LONG rightEdge     = rc.right - padding;

                        // File size — measure first so QIV link avoids it
                        std::wstring sizeValue;
                        {
                            std::error_code ec;
                            auto bytes = std::filesystem::file_size(
                                historyFoldersManager.GetFilePath(), ec);
                            if (!ec) {
                                wchar_t sizeBuf[64];
                                if (bytes >= 1024ULL * 1024)
                                    swprintf_s(sizeBuf, L"History - %.3f MB",
                                               static_cast<double>(bytes) / (1024.0 * 1024.0));
                                else if (bytes >= 1024)
                                    swprintf_s(sizeBuf, L"History - %.3f KB",
                                               static_cast<double>(bytes) / 1024.0);
                                else
                                    swprintf_s(sizeBuf, L"History - %llu Bytes",
                                               static_cast<unsigned long long>(bytes));
                                sizeValue = sizeBuf;
                            } else {
                                sizeValue = L"History - n/a";
                            }
                        }
                        SelectObject(hdc, hBodyFont);
                        SIZE szSize = {};
                        GetTextExtentPoint32W(hdc, sizeValue.c_str(),
                                             static_cast<int>(sizeValue.size()), &szSize);
                        LONG sizeLeft = rightEdge - szSize.cx;

                        // QIV→dir link — blue underlined, clickable, left of file size
                        {
                            wchar_t exeBuf[MAX_PATH] = {};
                            GetModuleFileNameW(nullptr, exeBuf, MAX_PATH);
                            std::wstring linkText = L"QIV \x2192 "
                                + std::filesystem::path(exeBuf).parent_path().wstring() + L"\\";

                            HFONT hLinkFont = CreateFontW(
                                fontSize, 0, 0, 0, FW_NORMAL, FALSE, TRUE, FALSE,
                                DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
                                CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
                            SelectObject(hdc, hLinkFont);
                            SetTextColor(hdc, RGB(100, 180, 255));

                            LONG linkAreaRight = std::max(rightHalfLeft, sizeLeft - MulDiv(8, dpi, 96));
                            g_exeLinkRect = { rightHalfLeft, footerTop, linkAreaRight, footerBot };
                            DrawTextW(hdc, linkText.c_str(), -1, &g_exeLinkRect,
                                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                            DeleteObject(hLinkFont);
                            SelectObject(hdc, hBodyFont);
                        }

                        // File size — right-aligned
                        SetTextColor(hdc, Constants::Theme::HistoryPanel::SIZE_HIGHLIGHT);
                        RECT sizeRect = { sizeLeft, footerTop, rightEdge, footerBot };
                        DrawTextW(hdc, sizeValue.c_str(), -1, &sizeRect,
                                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                    }
                }

                SelectObject(hdc, GetStockObject(SYSTEM_FONT));
                DeleteObject(hTitleFont);
                DeleteObject(hBodyFont);
                DeleteObject(hListFont);
                if (hIndexLinkFont) DeleteObject(hIndexLinkFont);
                EndPaint(m_hWnd, &ps);
                return 0;
            }

            case WM_NCHITTEST: {
                POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                RECT wrc;
                GetWindowRect(m_hWnd, &wrc);
                const int border = std::max(4, static_cast<int>(6 * app.dpiScale));
                bool top    = pt.y <  wrc.top    + border;
                bool bottom = pt.y >= wrc.bottom - border;
                bool left   = pt.x <  wrc.left   + border;
                bool right  = pt.x >= wrc.right  - border;
                if (top    && left)  return HTTOPLEFT;
                if (top    && right) return HTTOPRIGHT;
                if (bottom && left)  return HTBOTTOMLEFT;
                if (bottom && right) return HTBOTTOMRIGHT;
                if (top)             return HTTOP;
                if (bottom)          return HTBOTTOM;
                if (left)            return HTLEFT;
                if (right)           return HTRIGHT;
                return HTCLIENT;
            }

            case WM_LBUTTONDOWN: {
                int mx = GET_X_LPARAM(lParam);
                int my = GET_Y_LPARAM(lParam);
                RECT rc2{};
                GetClientRect(m_hWnd, &rc2);
                UINT dpi2 = static_cast<UINT>(app.dpiScale * 96.0f);

                // Calculate header area
                int padding2 = MulDiv(Constants::History::HISTORY_PADDING, dpi2, 96);
                int titleSz2 = MulDiv(Constants::History::HISTORY_FONT_SIZE + 2, dpi2, 96);
                int fontSize2 = MulDiv(Constants::History::HISTORY_FONT_SIZE, dpi2, 96);
                int headerBottom = padding2 + titleSz2 + 4 + MulDiv(2, dpi2, 96) + fontSize2 + 2 + MulDiv(4, dpi2, 96);

                // Check if clicking in header area — track screen coords to avoid drift
                if (my < headerBottom) {
                    g_headerDragging = true;
                    POINT ptScreen = { mx, my };
                    ClientToScreen(m_hWnd, &ptScreen);
                    g_headerDragStartX = ptScreen.x;
                    g_headerDragStartY = ptScreen.y;
                    GetWindowRect(m_hWnd, &g_headerDragWindowRect);
                    SetCapture(m_hWnd);
                    return 0;
                }

                // Check scrollbar drag
                int sbW = MulDiv(Constants::History::SCROLLBAR_THICKNESS, dpi2, 96);
                if (mx >= rc2.right - sbW) {
                    int totalH2 = CalcTotalContentH(static_cast<int>(g_displayList.size()), dpi2);
                    int winH2 = rc2.bottom - rc2.top;
                    int maxScr2 = std::max(0, totalH2 - winH2);
                    if (maxScr2 > 0) {
                        g_sbDragging = true;
                        g_sbDragStartY = my;
                        g_sbDragStartOff = g_scrollOffsetY;
                        SetCapture(m_hWnd);
                        return 0;
                    }
                }
                return 0;
            }

            case WM_MOUSEWHEEL: {
                UINT dpi = static_cast<UINT>(app.dpiScale * 96.0f);
                int rowH = MulDiv(Constants::History::HISTORY_ROW_HEIGHT, dpi, 96);
                int delta = GET_WHEEL_DELTA_WPARAM(wParam);
                g_scrollOffsetY -= (delta / WHEEL_DELTA) * rowH;
                g_scrollOffsetY = std::max(0, g_scrollOffsetY);
                InvalidateRect(m_hWnd, nullptr, FALSE);
                return 0;
            }

            case WM_MOUSEMOVE: {
                int mx = GET_X_LPARAM(lParam);
                int my = GET_Y_LPARAM(lParam);

                // Handle header dragging to move window
                if (g_headerDragging) {
                    POINT ptScreen = { mx, my };
                    ClientToScreen(m_hWnd, &ptScreen);
                    int newX = g_headerDragWindowRect.left + (ptScreen.x - g_headerDragStartX);
                    int newY = g_headerDragWindowRect.top  + (ptScreen.y - g_headerDragStartY);
                    SetWindowPos(m_hWnd, nullptr, newX, newY, 0, 0,
                                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                    return 0;
                }

                // Check if hovering over scrollbar
                RECT rcSb{};
                GetClientRect(m_hWnd, &rcSb);
                UINT dpiSbHover = static_cast<UINT>(app.dpiScale * 96.0f);
                int sbWHover = MulDiv(Constants::History::SCROLLBAR_THICKNESS, dpiSbHover, 96);
                if (mx >= rcSb.right - sbWHover) {
                    UINT dpiSb = static_cast<UINT>(app.dpiScale * 96.0f);
                    int totalHSb = CalcTotalContentH(static_cast<int>(g_displayList.size()), dpiSb);
                    int winHSb = rcSb.bottom - rcSb.top;
                    int maxScrSb = std::max(0, totalHSb - winHSb);
                    SetCursor(LoadCursor(nullptr, (maxScrSb > 0) ? IDC_HAND : IDC_ARROW));
                } else {
                    SetCursor(LoadCursor(nullptr, IDC_ARROW));
                }

                if (g_sbDragging) {
                    RECT rc3{};
                    GetClientRect(m_hWnd, &rc3);
                    UINT dpi3 = static_cast<UINT>(app.dpiScale * 96.0f);
                    int totalH3 = CalcTotalContentH(static_cast<int>(g_displayList.size()), dpi3);
                    int winH3 = rc3.bottom - rc3.top;
                    int maxScr3 = std::max(0, totalH3 - winH3);
                    if (maxScr3 > 0) {
                        float visF = static_cast<float>(winH3) / static_cast<float>(totalH3);
                        int minThumbD = MulDiv(static_cast<int>(Constants::History::SCROLLBAR_MIN_THUMB), dpi3, 96);
                        int thumbL = std::max(minThumbD, static_cast<int>(winH3 * visF));
                        int scrollPx = winH3 - thumbL;
                        if (scrollPx > 0) {
                            int delta3 = my - g_sbDragStartY;
                            g_scrollOffsetY = std::clamp(
                                    g_sbDragStartOff + static_cast<int>(
                                        static_cast<float>(delta3) * maxScr3 / scrollPx),
                                    0, maxScr3);
                            InvalidateRect(m_hWnd, nullptr, FALSE);
                        }
                    }
                    return 0;
                }

                int newHover = -1;
                for (int i = 0; i < static_cast<int>(g_rowRects.size()); ++i) {
                    const RECT &r = g_rowRects[i];
                    if (mx >= r.left && mx < r.right && my >= r.top && my < r.bottom) {
                        newHover = i;
                        break;
                    }
                }
                // Hand cursor over clickable links/indexes
                if (g_exeLinkRect.right > g_exeLinkRect.left) {
                    POINT pt = { mx, my };
                    if (PtInRect(&g_exeLinkRect, pt)) {
                        SetCursor(LoadCursor(nullptr, IDC_HAND));
                        return 0;
                    }
                }
                if (g_f5IndexRect.right > g_f5IndexRect.left) {
                    POINT pt = { mx, my };
                    if (PtInRect(&g_f5IndexRect, pt)) {
                        SetCursor(LoadCursor(nullptr, IDC_HAND));
                        return 0;
                    }
                }
                // Hand cursor over history row indexes
                for (const auto &idxRect : g_indexRects) {
                    if (idxRect.right > idxRect.left) {
                        POINT pt = { mx, my };
                        if (PtInRect(&idxRect, pt)) {
                            SetCursor(LoadCursor(nullptr, IDC_HAND));
                            return 0;
                        }
                    }
                }

                if (newHover != g_hoverRow) {
                    g_hoverRow = newHover;
                    SetCursor(LoadCursor(nullptr, (g_hoverRow >= 0) ? IDC_HAND : IDC_ARROW));
                    InvalidateRect(m_hWnd, nullptr, FALSE);
                }
                return 0;
            }

            case WM_LBUTTONUP: {
                if (g_headerDragging) {
                    g_headerDragging = false;
                    ReleaseCapture();
                    return 0;
                }
                if (g_sbDragging) {
                    g_sbDragging = false;
                    ReleaseCapture();
                    return 0;
                }
                int mx = GET_X_LPARAM(lParam);
                int my = GET_Y_LPARAM(lParam);

                // Exe-dir link click
                if (g_exeLinkRect.right > g_exeLinkRect.left) {
                    POINT pt = { mx, my };
                    if (PtInRect(&g_exeLinkRect, pt)) {
                        wchar_t exeBuf[MAX_PATH] = {};
                        GetModuleFileNameW(nullptr, exeBuf, MAX_PATH);
                        std::wstring dir = std::filesystem::path(exeBuf).parent_path().wstring();
                        ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOW);
                        return 0;
                    }
                }

                // [F5] label click — toggle F5
                if (g_f5IndexRect.right > g_f5IndexRect.left) {
                    POINT pt = { mx, my };
                    if (PtInRect(&g_f5IndexRect, pt)) {
                        uiManager.Toggle(uiManager.getDirWindow());
                        return 0;
                    }
                }

                // [F3] (Cache) label click — toggle Cache
                if (g_cacheIndexRect.right > g_cacheIndexRect.left) {
                    POINT pt = { mx, my };
                    if (PtInRect(&g_cacheIndexRect, pt)) {
                        uiManager.Toggle(uiManager.getCacheWindow());
                        return 0;
                    }
                }

                // History row index click — open in explorer
                for (int i = 0; i < static_cast<int>(g_indexRects.size()); ++i) {
                    const RECT &idxRect = g_indexRects[i];
                    if (idxRect.right > idxRect.left) {
                        POINT pt = { mx, my };
                        if (PtInRect(&idxRect, pt)) {
                            if (i < static_cast<int>(g_displayList.size())) {
                                std::wstring folder = g_displayList[i].path;
                                ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOW);
                            }
                            return 0;
                        }
                    }
                }

                // History row path click — open in app
                for (int i = 0; i < static_cast<int>(g_rowRects.size()); ++i) {
                    const RECT &r = g_rowRects[i];
                    if (mx >= r.left && mx < r.right && my >= r.top && my < r.bottom) {
                        std::wstring folder = g_displayList[i].path;
                        ShowWindow(m_hWnd, SW_HIDE);
                        OpenDirectory(g_hHistOwner, folder);
                        return 0;
                    }
                }
                return 0;
            }

            case WM_MOUSELEAVE: {
                g_hoverRow = -1;
                InvalidateRect(m_hWnd, nullptr, FALSE);
                return 0;
            }

            case WM_KEYDOWN: {
                bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
                // Check Ctrl+Tab first (toggle full history view)
                if (wParam == VK_TAB && ctrl) {
                    g_showFullHistory = !g_showFullHistory;
                    g_scrollOffsetY = 0; // Reset scroll when toggling
                    BuildDisplayList();
                    g_hoverRow = 0;
                    InvalidateRect(m_hWnd, nullptr, TRUE);
                    return 0;
                }

                // Ctrl+Z — restore last deleted entry
                if (wParam == 'Z' && ctrl && !shift && !alt && g_lastDeletedIndex >= 0) {
                    auto &hist = historyFoldersManager.folderHistory;
                    int insertAt = std::min(g_lastDeletedIndex, static_cast<int>(hist.size()));
                    hist.insert(hist.begin() + insertAt, g_lastDeletedPath);
                    historyFoldersManager.RewriteHistoryToDisk();
                    if (g_lastDeletedWasFavorite) {
                        historyFoldersManager.favorites.insert(g_lastDeletedPath);
                        historyFoldersManager.RewriteFavoritesToDisk();
                    }
                    g_lastDeletedIndex = -1; // consume undo slot
                    BuildDisplayList();
                    int newMax = static_cast<int>(g_displayList.size());
                    if (g_hoverRow >= newMax) g_hoverRow = newMax - 1;
                    int x, y, w, h;
                    GetHistoryWindowBounds(g_hHistOwner ? g_hHistOwner : m_hWnd, x, y, w, h);
                    SetWindowPos(m_hWnd, HWND_TOPMOST, x, y, w, h, SWP_FRAMECHANGED);
                    InvalidateRect(m_hWnd, nullptr, TRUE);
                    return 0;
                }

                int navMax = static_cast<int>(g_displayList.size());
                switch (wParam) {
                    case Shortcuts::SC_PANEL_HISTORY_TOGGLE:
                        ToggleHistoryWindow();
                        return 0;

                    case VK_UP:
                        if (navMax > 0) {
                            g_hoverRow = (g_hoverRow <= 0) ? navMax - 1 : g_hoverRow - 1;
                            if (g_rowH > 0) {
                                int bodyH = g_bodyBottom - g_bodyTop;
                                if (g_hoverRow == navMax - 1) { // wrapped to bottom
                                    g_scrollOffsetY = std::max(0, navMax * g_rowH - bodyH);
                                } else {
                                    int rowStart = g_hoverRow * g_rowH;
                                    if (rowStart < g_scrollOffsetY)
                                        g_scrollOffsetY = rowStart;
                                }
                            }
                            InvalidateRect(m_hWnd, nullptr, FALSE);
                        }
                        return 0;

                    case VK_DOWN:
                        if (navMax > 0) {
                            g_hoverRow = (g_hoverRow < navMax - 1) ? g_hoverRow + 1 : 0;
                            if (g_rowH > 0) {
                                if (g_hoverRow == 0) {
                                    g_scrollOffsetY = 0;
                                } else {
                                    int bodyH = g_bodyBottom - g_bodyTop;
                                    int rowEnd = (g_hoverRow + 1) * g_rowH;
                                    if (rowEnd - g_scrollOffsetY > bodyH)
                                        g_scrollOffsetY = rowEnd - bodyH;
                                }
                            }
                            InvalidateRect(m_hWnd, nullptr, FALSE);
                        }
                        return 0;

                    case VK_RETURN: {
                        if (g_hoverRow >= 0 && g_hoverRow < navMax) {
                            std::wstring folder = g_displayList[g_hoverRow].path;
                            bool shiftHeld = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                            if (shiftHeld) {
                                // Shift+Enter — toggle spawned DirWnd for this folder.
                                // Check if a panel already exists by reading the position label.
                                std::wstring posLabel = uiManager.GetSpawnedDirWndPositionLabel(folder);
                                if (!posLabel.empty()) {
                                    // Extract position name from " (name)" format
                                    std::wstring posName = posLabel.substr(2, posLabel.length() - 3);
                                    SlotInfo *slot = uiManager.GetLayout().getSlotByName(posName);
                                    if (slot && slot->panel) {
                                        slot->panel->Hide();
                                    }
                                } else {
                                    // No panel exists, spawn one
                                    uiManager.SpawnDirWndForFolder(folder, m_hWnd);
                                }
                            } else {
                                // Plain Enter — load folder in main viewer (original behaviour).
                                ShowWindow(m_hWnd, SW_HIDE);
                                OpenDirectory(g_hHistOwner, folder);
                            }
                        }
                        return 0;
                    }

                    case Shortcuts::HISTORY_FAVORITES_TOGGLE_KEY: // Space
                        if (g_hoverRow >= 0 && g_hoverRow < navMax) {
                            ToggleFavorite(g_hoverRow);
                            // Rebuild display list after favorite change
                            BuildDisplayList();
                            // Re-clamp hover in case list shrank
                            int newMax = static_cast<int>(g_displayList.size());
                            if (g_hoverRow >= newMax)
                                g_hoverRow = newMax - 1;
                            // Resize window to fit new list height
                            int x, y, w, h;
                            GetHistoryWindowBounds(g_hHistOwner ? g_hHistOwner : m_hWnd, x, y, w, h);
                            SetWindowPos(m_hWnd, HWND_TOPMOST, x, y, w, h, SWP_FRAMECHANGED);
                            InvalidateRect(m_hWnd, nullptr, TRUE);
                        }
                        return 0;

                    case VK_DELETE:
                        if (ctrl && shift && alt) {
                            // Ctrl+Alt+Shift+Delete — clear favorites, keep history
                            ClearFavoritesKeepHistory();
                            BuildDisplayList();
                            {
                                int x, y, w, h;
                                GetHistoryWindowBounds(g_hHistOwner ? g_hHistOwner : m_hWnd, x, y, w, h);
                                SetWindowPos(m_hWnd, HWND_TOPMOST, x, y, w, h, SWP_FRAMECHANGED);
                            }
                            InvalidateRect(m_hWnd, nullptr, TRUE);
                        } else if (ctrl && shift) {
                            // Ctrl+Shift+Delete — clear history, keep favorites
                            ClearHistoryKeepFavorites();
                            BuildDisplayList();
                            {
                                int x, y, w, h;
                                GetHistoryWindowBounds(g_hHistOwner ? g_hHistOwner : m_hWnd, x, y, w, h);
                                SetWindowPos(m_hWnd, HWND_TOPMOST, x, y, w, h, SWP_FRAMECHANGED);
                            }
                            InvalidateRect(m_hWnd, nullptr, TRUE);
                        } else if (!ctrl && !shift && !alt) {
                            // Plain Delete — remove hovered entry; Ctrl+Z restores it
                            if (g_hoverRow >= 0 && g_hoverRow < navMax) {
                                const std::wstring &path = g_displayList[g_hoverRow].path;
                                bool wasFav = g_displayList[g_hoverRow].isFavorite;

                                // Find the index in folderHistory for undo
                                auto &hist = historyFoldersManager.folderHistory;
                                int histIdx = -1;
                                for (int i = 0; i < static_cast<int>(hist.size()); ++i) {
                                    if (hist[i] == path) { histIdx = i; break; }
                                }

                                // Save undo state
                                g_lastDeletedPath        = path;
                                g_lastDeletedIndex       = histIdx;
                                g_lastDeletedWasFavorite = wasFav;

                                // Remove from history
                                if (histIdx >= 0)
                                    hist.erase(hist.begin() + histIdx);
                                historyFoldersManager.RewriteHistoryToDisk();

                                // Remove from favorites
                                if (wasFav) {
                                    historyFoldersManager.favorites.erase(path);
                                    historyFoldersManager.RewriteFavoritesToDisk();
                                }

                                BuildDisplayList();
                                int newMax = static_cast<int>(g_displayList.size());
                                if (g_hoverRow >= newMax) g_hoverRow = newMax - 1;
                                int x, y, w, h;
                                GetHistoryWindowBounds(g_hHistOwner ? g_hHistOwner : m_hWnd, x, y, w, h);
                                SetWindowPos(m_hWnd, HWND_TOPMOST, x, y, w, h, SWP_FRAMECHANGED);
                                InvalidateRect(m_hWnd, nullptr, TRUE);
                            }
                        }
                        return 0;

                    default:
                        if (g_hHistOwner)
                            return SendMessageW(g_hHistOwner, message, wParam, lParam);
                        break;
                }
                break;
            }

            case WM_CLOSE:
                ShowWindow(m_hWnd, SW_HIDE);
                return 0;
        }

        return DefWindowProcW(m_hWnd, message, wParam, lParam);
    }

    // ---------------------------------------------------------------------------
    // Public API
    // ---------------------------------------------------------------------------
    void HistoryListWnd::Init(HINSTANCE hInstance, HWND hParent, int8_t /*position*/) {
        Init(hInstance, hParent);
    }

    void HistoryListWnd::Init(HINSTANCE hInstance, HWND hParent) {
        g_hHistOwner = hParent;

        WNDCLASSW wc{};
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = IPanelWindow::WindowRouter;
        wc.hInstance = hInstance;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = L"QIV_HistoryWindow";
        RegisterClassW(&wc);

        BuildDisplayList(); // use whatever is already in RAM (loaded by UIManager::Init)
        int x, y, w, h;
        GetHistoryWindowBounds(hParent, x, y, w, h);

        CreateWindowExW(
                WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
                wc.lpszClassName,
                L"Folder History",
                WS_POPUP,
                x, y, w, h,
                hParent, nullptr, hInstance,
                this);

        if (!m_hWnd) return;

        SetLayeredWindowAttributes(m_hWnd, 0, Constants::THUMBNAIL_PANEL_WINDOW_OPACITY, LWA_ALPHA);
        DWORD corner = app.cornerPreference;
        DwmSetWindowAttribute(m_hWnd, Constants::DWMWA_WINDOW_CORNER_PREFERENCES, &corner, sizeof(corner));
        ShowWindow(m_hWnd, SW_HIDE);
    }

    void HistoryListWnd::Show() {
        if (!m_hWnd) return;
        g_showFullHistory = false; // Always start with limited view
        BuildDisplayList();
        int x, y, w, h;
        GetHistoryWindowBounds(g_hHistOwner ? g_hHistOwner : m_hWnd, x, y, w, h);
        SetWindowPos(m_hWnd, HWND_TOPMOST, x, y, w, h, SWP_FRAMECHANGED);
        g_hoverRow = 0;
        g_scrollOffsetY = 0;
        ShowWindow(m_hWnd, SW_SHOW);
        SetForegroundWindow(m_hWnd);
        InvalidateRect(m_hWnd, nullptr, TRUE);
    }

    void HistoryListWnd::Toggle() {
        if (!m_hWnd) return;
        IsWindowVisible(m_hWnd) ? Hide() : Show();
    }

    void HistoryListWnd::ToggleHistoryWindow() {
        Toggle();
    }

    void HistoryListWnd::PushFolderHistory(const std::wstring &folderPath) {
        UI::PushFolderHistory(folderPath);
    }

    const std::vector<std::wstring> &HistoryListWnd::GetFolderHistory() {
        return UI::GetFolderHistory();
    }
} // namespace UI
