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
    static int  g_hoverRow       = -1;
    static int  g_scrollOffsetY  = 0;
    static bool g_sbDragging     = false;
    static int  g_sbDragStartY   = 0;
    static int  g_sbDragStartOff = 0;
    static bool g_showFullHistory = false;  // Ctrl+Tab toggles full history view
    static HistoryFoldersManager historyFoldersManager;
    static std::vector<RECT> g_rowRects;

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

        void LoadFolderHistoryFromDisk();

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

        if (favSet.count(path) > 0)
            favSet.erase(path);
        else
            favSet.insert(path);

        // Favorite state is stored as a '*' prefix in the file — must rewrite
        historyFoldersManager.RewriteFileToDisk();
    }

    void ClearHistoryKeepFavorites() {
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

        historyFoldersManager.RewriteFileToDisk();
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

        UINT dpi = GetDpiForWindow(hRef);
        int rowH = MulDiv(Constants::History::HISTORY_ROW_HEIGHT, dpi, 96);
        int padding = MulDiv(Constants::History::HISTORY_PADDING, dpi, 96);

        int entries = std::max(1, static_cast<int>(g_displayList.size()));
        int totalH = padding * 2
                     + MulDiv(30, dpi, 96) // title row
                     + MulDiv(8, dpi, 96) // gap below title
                     + entries * rowH;

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

                UINT dpi = GetDpiForWindow(m_hWnd);
                int padding = MulDiv(Constants::History::HISTORY_PADDING, dpi, 96);
                int rowH    = MulDiv(Constants::History::HISTORY_ROW_HEIGHT, dpi, 96);
                int fontSize = MulDiv(Constants::History::HISTORY_FONT_SIZE, dpi, 96);
                int titleSz  = MulDiv(Constants::History::HISTORY_FONT_SIZE + 2, dpi, 96);
                int indexW  = MulDiv(28, dpi, 96);
                int starW   = MulDiv(18, dpi, 96);

                // Scrollbar geometry — computed before any drawing.
                int SB_W = static_cast<int>(
                    MulDiv(Constants::History::SCROLLBAR_THICKNESS, dpi, 96));
                int totalContentH = padding * 2
                                    + MulDiv(30, dpi, 96)   // title row
                                    + MulDiv(8,  dpi, 96)   // gap below title/sep
                                    + static_cast<int>(g_displayList.size()) * rowH;
                int windowH   = rc.bottom - rc.top;
                int maxScroll = std::max(0, totalContentH - windowH);
                g_scrollOffsetY = std::clamp(g_scrollOffsetY, 0, maxScroll);
                bool needsScrollbar = (maxScroll > 0);

                // Background — use active color if this panel is active
                COLORREF bgColor = RGB(
                    static_cast<int>(Constants::ThumbnailPanel::PANEL_BACKGROUND_R * 255),
                    static_cast<int>(Constants::ThumbnailPanel::PANEL_BACKGROUND_G * 255),
                    static_cast<int>(Constants::ThumbnailPanel::PANEL_BACKGROUND_B * 255));
                if (UI::g_activePanelHwnd == m_hWnd) {
                    bgColor = RGB(
                        static_cast<int>(Constants::ThumbnailPanel::PANEL_ACTIVE_R * 255),
                        static_cast<int>(Constants::ThumbnailPanel::PANEL_ACTIVE_G * 255),
                        static_cast<int>(Constants::ThumbnailPanel::PANEL_ACTIVE_B * 255));
                }
                HBRUSH hBg = CreateSolidBrush(bgColor);
                FillRect(hdc, &rc, hBg);
                DeleteObject(hBg);
                SetBkMode(hdc, TRANSPARENT);

                HFONT hTitleFont = CreateFontW(
                        titleSz, 0, 0, 0, FW_BOLD,   FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
                        CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
                HFONT hBodyFont = CreateFontW(
                        fontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
                        CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");

                // Title (fixed — not scrolled)
                SelectObject(hdc, hTitleFont);
                SetTextColor(hdc, RGB(100, 200, 255));
                int totalSaved = static_cast<int>(historyFoldersManager.folderHistory.size());
                int totalShown = static_cast<int>(g_displayList.size());
                std::wstring title = L"Folder History  (showing "
                                     + std::to_wstring(totalShown) + L" of "
                                     + std::to_wstring(totalSaved) + L" saved)  \x2605=Space";
                RECT titleRect = {
                    rc.left + padding, rc.top + padding,
                    rc.right - padding, rc.top + padding + titleSz + 4
                };
                DrawTextW(hdc, title.c_str(), -1, &titleRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

                // Separator (fixed)
                int sepY = titleRect.bottom + MulDiv(4, dpi, 96);
                HPEN hPen    = CreatePen(PS_SOLID, 1, RGB(50, 50, 50));
                HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
                MoveToEx(hdc, rc.left + padding, sepY, nullptr);
                LineTo(hdc, rc.right - padding, sepY);
                SelectObject(hdc, hOldPen);
                DeleteObject(hPen);

                // Rows (scrolled) — clipped to the area below the separator.
                SelectObject(hdc, hBodyFont);
                g_rowRects.clear();
                int rowsTop = sepY + MulDiv(6, dpi, 96);

                SaveDC(hdc);
                IntersectClipRect(hdc, rc.left, rowsTop, rc.right, rc.bottom);

                if (g_displayList.empty()) {
                    int y = rowsTop - g_scrollOffsetY;
                    SetTextColor(hdc, RGB(100, 100, 100));
                    RECT emptyRect = {rc.left + padding, y, rc.right - padding, y + rowH};
                    DrawTextW(hdc, L"No folders visited yet.", -1, &emptyRect,
                              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                } else {
                    for (int i = 0; i < static_cast<int>(g_displayList.size()); ++i) {
                        int rowTop    = rowsTop - g_scrollOffsetY + i * rowH;
                        int rowBottom = rowTop + rowH;

                        const DisplayEntry &entry = g_displayList[i];
                        // Always push — index must match g_displayList for hit-testing.
                        g_rowRects.push_back({rc.left, rowTop, rc.right, rowBottom});

                        // Skip drawing rows outside the visible area.
                        if (rowBottom <= rowsTop || rowTop >= rc.bottom) continue;

                        RECT rowRect = {rc.left, rowTop, rc.right, rowBottom};

                        // Hover background
                        if (i == g_hoverRow) {
                            HBRUSH hHover = CreateSolidBrush(
                                    entry.isFavorite ? RGB(50, 50, 10) : RGB(40, 60, 80));
                            FillRect(hdc, &rowRect, hHover);
                            DeleteObject(hHover);
                        }

                        // Row index number
                        SetTextColor(hdc, RGB(255, 204, 0));
                        std::wstring idxStr = std::to_wstring(i + 1);
                        RECT idxRect = {
                            rc.left + padding, rowTop,
                            rc.left + padding + indexW, rowBottom
                        };
                        DrawTextW(hdc, idxStr.c_str(), -1, &idxRect,
                                  DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

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

                        // Path text
                        COLORREF pathColor = entry.isFavorite
                                ? (i == g_hoverRow ? RGB(255, 255, 160) : RGB(255, 240, 120))
                                : (i == g_hoverRow ? RGB(255, 255, 255) : RGB(200, 200, 200));
                        SetTextColor(hdc, pathColor);
                        RECT pathRect = {
                            rc.left + padding + indexW + starW + MulDiv(10, dpi, 96), rowTop,
                            rc.right - padding - (needsScrollbar ? SB_W + 2 : 0), rowBottom
                        };
                        DrawTextW(hdc, entry.path.c_str(), -1, &pathRect,
                                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                    }
                }

                RestoreDC(hdc, -1);

                // Scrollbar (drawn on top, not clipped)
                if (needsScrollbar) {
                    int sbX = rc.right - SB_W;

                    HBRUSH hTrack = CreateSolidBrush(RGB(Constants::History::SCROLLBAR_TRACK_R,
                                                         Constants::History::SCROLLBAR_TRACK_G,
                                                         Constants::History::SCROLLBAR_TRACK_B));
                    RECT sbTrack  = {sbX, 0, rc.right, rc.bottom};
                    FillRect(hdc, &sbTrack, hTrack);
                    DeleteObject(hTrack);

                    float visibleFrac = static_cast<float>(windowH) / static_cast<float>(totalContentH);
                    int minThumb = MulDiv(static_cast<int>(Constants::History::SCROLLBAR_MIN_THUMB), dpi, 96);
                    int thumbLen = std::max(minThumb, static_cast<int>(windowH * visibleFrac));
                    int thumbOff = static_cast<int>(
                        static_cast<float>(g_scrollOffsetY) / static_cast<float>(maxScroll)
                        * static_cast<float>(windowH - thumbLen));
                    thumbOff = std::clamp(thumbOff, 0, windowH - thumbLen);

                    HBRUSH hThumb = CreateSolidBrush(RGB(Constants::History::SCROLLBAR_THUMB_R,
                                                         Constants::History::SCROLLBAR_THUMB_G,
                                                         Constants::History::SCROLLBAR_THUMB_B));
                    RECT sbThumb  = {sbX, thumbOff, rc.right, thumbOff + thumbLen};
                    FillRect(hdc, &sbThumb, hThumb);
                    DeleteObject(hThumb);
                }

                SelectObject(hdc, GetStockObject(SYSTEM_FONT));
                DeleteObject(hTitleFont);
                DeleteObject(hBodyFont);
                EndPaint(m_hWnd, &ps);
                return 0;
            }

            case WM_LBUTTONDOWN: {
                int mx = GET_X_LPARAM(lParam);
                int my = GET_Y_LPARAM(lParam);
                RECT rc2{};
                GetClientRect(m_hWnd, &rc2);
                UINT dpi2 = GetDpiForWindow(m_hWnd);
                int sbW = MulDiv(Constants::History::SCROLLBAR_THICKNESS, dpi2, 96);
                if (mx >= rc2.right - sbW) {
                    int  rowH2    = MulDiv(Constants::History::HISTORY_ROW_HEIGHT, dpi2, 96);
                    int  padding2 = MulDiv(Constants::History::HISTORY_PADDING, dpi2, 96);
                    int  totalH2  = padding2 * 2
                                    + MulDiv(30, dpi2, 96)
                                    + MulDiv(8,  dpi2, 96)
                                    + static_cast<int>(g_displayList.size()) * rowH2;
                    int  winH2    = rc2.bottom - rc2.top;
                    int  maxScr2  = std::max(0, totalH2 - winH2);
                    if (maxScr2 > 0) {
                        g_sbDragging     = true;
                        g_sbDragStartY   = my;
                        g_sbDragStartOff = g_scrollOffsetY;
                        SetCapture(m_hWnd);
                        return 0;
                    }
                }
                return 0;
            }

            case WM_MOUSEWHEEL: {
                UINT dpi   = GetDpiForWindow(m_hWnd);
                int  rowH  = MulDiv(Constants::History::HISTORY_ROW_HEIGHT, dpi, 96);
                int  delta = GET_WHEEL_DELTA_WPARAM(wParam);
                g_scrollOffsetY -= (delta / WHEEL_DELTA) * rowH;
                g_scrollOffsetY  = std::max(0, g_scrollOffsetY);
                InvalidateRect(m_hWnd, nullptr, FALSE);
                return 0;
            }

            case WM_MOUSEMOVE: {
                int mx = GET_X_LPARAM(lParam);
                int my = GET_Y_LPARAM(lParam);

                // Check if hovering over scrollbar
                RECT rcSb{};
                GetClientRect(m_hWnd, &rcSb);
                UINT dpiSbHover = GetDpiForWindow(m_hWnd);
                int sbWHover = MulDiv(Constants::History::SCROLLBAR_THICKNESS, dpiSbHover, 96);
                if (mx >= rcSb.right - sbWHover) {
                    UINT dpiSb     = GetDpiForWindow(m_hWnd);
                    int  rowHSb    = MulDiv(Constants::History::HISTORY_ROW_HEIGHT, dpiSb, 96);
                    int  paddingSb = MulDiv(Constants::History::HISTORY_PADDING, dpiSb, 96);
                    int  totalHSb  = paddingSb * 2
                                     + MulDiv(30, dpiSb, 96)
                                     + MulDiv(8,  dpiSb, 96)
                                     + static_cast<int>(g_displayList.size()) * rowHSb;
                    int  winHSb    = rcSb.bottom - rcSb.top;
                    int  maxScrSb  = std::max(0, totalHSb - winHSb);
                    SetCursor(LoadCursor(nullptr, (maxScrSb > 0) ? IDC_HAND : IDC_ARROW));
                } else {
                    SetCursor(LoadCursor(nullptr, IDC_ARROW));
                }

                if (g_sbDragging) {
                    RECT rc3{};
                    GetClientRect(m_hWnd, &rc3);
                    UINT dpi3     = GetDpiForWindow(m_hWnd);
                    int  rowH3    = MulDiv(Constants::History::HISTORY_ROW_HEIGHT, dpi3, 96);
                    int  padding3 = MulDiv(Constants::History::HISTORY_PADDING, dpi3, 96);
                    int  totalH3  = padding3 * 2
                                    + MulDiv(30, dpi3, 96)
                                    + MulDiv(8,  dpi3, 96)
                                    + static_cast<int>(g_displayList.size()) * rowH3;
                    int winH3   = rc3.bottom - rc3.top;
                    int maxScr3 = std::max(0, totalH3 - winH3);
                    if (maxScr3 > 0) {
                        float visF    = static_cast<float>(winH3) / static_cast<float>(totalH3);
                        int   minThumbD = MulDiv(static_cast<int>(Constants::History::SCROLLBAR_MIN_THUMB), dpi3, 96);
                        int   thumbL  = std::max(minThumbD, static_cast<int>(winH3 * visF));
                        int   scrollPx = winH3 - thumbL;
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
                if (newHover != g_hoverRow) {
                    g_hoverRow = newHover;
                    SetCursor(LoadCursor(nullptr, (g_hoverRow >= 0) ? IDC_HAND : IDC_ARROW));
                    InvalidateRect(m_hWnd, nullptr, FALSE);
                }
                return 0;
            }

            case WM_LBUTTONUP: {
                if (g_sbDragging) {
                    g_sbDragging = false;
                    ReleaseCapture();
                    return 0;
                }
                int mx = GET_X_LPARAM(lParam);
                int my = GET_Y_LPARAM(lParam);
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
                // Check Ctrl+Tab first (toggle full history view)
                if (wParam == VK_TAB && (GetKeyState(VK_CONTROL) & 0x8000)) {
                    g_showFullHistory = !g_showFullHistory;
                    g_scrollOffsetY = 0; // Reset scroll when toggling
                    BuildDisplayList();
                    g_hoverRow = 0;
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
                            InvalidateRect(m_hWnd, nullptr, FALSE);
                        }
                        return 0;

                    case VK_DOWN:
                        if (navMax > 0) {
                            g_hoverRow = (g_hoverRow < navMax - 1) ? g_hoverRow + 1 : 0;
                            InvalidateRect(m_hWnd, nullptr, FALSE);
                        }
                        return 0;

                    case VK_RETURN: {
                        if (g_hoverRow >= 0 && g_hoverRow < navMax) {
                            std::wstring folder = g_displayList[g_hoverRow].path;
                            bool shiftHeld = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                            if (shiftHeld) {
                                // Shift+Enter — spawn a DirWnd for this folder.
                                // Pass m_hWnd so focus is returned here after spawn.
                                uiManager.SpawnDirWndForFolder(folder, m_hWnd);
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

                    case Shortcuts::HISTORY_CLEAR_ALL_HISTORY_BUT_NOT_FAVORITES: // Delete
                        ClearHistoryKeepFavorites();
                        BuildDisplayList();
                        {
                            int x, y, w, h;
                            GetHistoryWindowBounds(g_hHistOwner ? g_hHistOwner : m_hWnd, x, y, w, h);
                            SetWindowPos(m_hWnd, HWND_TOPMOST, x, y, w, h, SWP_FRAMECHANGED);
                        }
                        InvalidateRect(m_hWnd, nullptr, TRUE);
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
