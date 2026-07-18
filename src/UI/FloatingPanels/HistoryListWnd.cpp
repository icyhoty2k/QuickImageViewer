#include "HistoryListWnd.h"
#include "../../Platform/Constants.h"
#include "../../Platform/ConstantsStrings.h"
#include "../../Platform/FileHandler.h"
#include "../../Persistence/RegistryManager.h"
#include "../../Overlays/OverlayManager.h"
#include "../../AppState.h"
#include "../../Input/Shortcuts.h"
#include "../../Persistence/HistoryFoldersManager.h"
#include "../UIManager.h"
#include <algorithm>
#include <cwctype>
#include <thread>
#include <unordered_map>
#include <windowsx.h>
#include <filesystem>
#include <Constants.h>

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
    // Row remembered across focus loss, restored on refocus. Focus bounces
    // whenever a spawned panel opens/closes (Shift+Enter) — without this the
    // keyboard selection resets every time. -1 = nothing to restore.
    static int g_savedHoverRow = -1;
    static int g_scrollOffsetY = 0;
    static bool g_sbDragging = false;
    static int g_sbDragStartY = 0;
    static int g_sbDragStartOff = 0;
    static bool g_showFullHistory = false; // Ctrl+Tab toggles full history view
    static bool g_headerDragging = false;
    static int g_headerDragStartX = 0; // screen X at drag start
    static int g_headerDragStartY = 0; // screen Y at drag start
    static RECT g_headerDragWindowRect = {}; // window rect at drag start
    static int g_bodyTop = 0; // updated each WM_PAINT — actual top of scrollable body
    static int g_bodyBottom = 0; // updated each WM_PAINT — actual bottom of scrollable body
    static int g_rowH = 0; // updated each WM_PAINT — row height in pixels
    static HistoryFoldersManager historyFoldersManager;

    // Ctrl+Z undo state for single-row Delete
    static std::wstring g_lastDeletedPath;
    static int g_lastDeletedIndex = -1;
    static bool g_lastDeletedWasFavorite = false;

    // ---------------------------------------------------------------------------
    // Folder validity cache.
    // Populated lazily by background validation; also set inline on Enter/click.
    // Unknown = not yet checked → painted as normal (no warning colour).
    // ---------------------------------------------------------------------------
    enum class FolderStatus { Unknown, Valid, Missing, Empty };

    static std::unordered_map<std::wstring, FolderStatus> g_statusCache;

    struct DirSizeInfo { int64_t bytes = 0; int count = 0; };
    static std::unordered_map<std::wstring, DirSizeInfo> g_dirSizeCache;

    // Scan a folder synchronously: count image files and sum their sizes.
    // Updates g_statusCache and g_dirSizeCache for the given path.
    static void ScanDirForHistory(const std::wstring &path) {
        namespace fs = std::filesystem;
        std::error_code ec;
        if (!fs::is_directory(fs::path(path), ec) || ec) {
            g_statusCache[path] = FolderStatus::Missing;
            g_dirSizeCache.erase(path);
            return;
        }
        int64_t totalBytes = 0;
        int count = 0;
        for (auto it = fs::directory_iterator(fs::path(path),
                         fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::directory_iterator(); it.increment(ec)) {
            if (!it->is_regular_file(ec)) { ec.clear(); continue; }
            if (!is_image_ext(it->path().extension().wstring())) continue;
            auto sz = fs::file_size(it->path(), ec);
            if (!ec) totalBytes += static_cast<int64_t>(sz);
            ec.clear();
            ++count;
        }
        g_statusCache[path] = (count > 0) ? FolderStatus::Valid : FolderStatus::Empty;
        g_dirSizeCache[path] = {totalBytes, count};
    }

    // Set whenever the display list changes (push, delete, restore, clear).
    // Background validation only runs when this is true, preventing redundant
    // filesystem work on rapid Tab / Tab / Tab presses.

    static FolderStatus GetFolderStatus(const std::wstring &path) {
        auto it = g_statusCache.find(path);
        if (it != g_statusCache.end() && it->second != FolderStatus::Unknown)
            return it->second;

        namespace fs = std::filesystem;
        std::error_code ec;
        if (!fs::is_directory(fs::path(path), ec) || ec) {
            return g_statusCache[path] = FolderStatus::Missing;
        }
        // Non-throwing iteration: it.increment(ec) instead of range-for, whose
        // operator++() throws filesystem_error on transient I/O errors. This
        // runs on the UI thread (Enter/click handlers) — a throw here would
        // escape the wndproc and terminate the process (0xC0000409).
        for (auto dirIt = fs::directory_iterator(
                     fs::path(path), fs::directory_options::skip_permission_denied, ec);
             !ec && dirIt != fs::directory_iterator(); dirIt.increment(ec)) {
            if (!dirIt->is_regular_file(ec)) {
                ec.clear();
                continue;
            }
            std::wstring ext = dirIt->path().extension().wstring();
            for (auto &c: ext) c = static_cast<wchar_t>(::towlower(c));
            for (size_t i = 0; i < Constants::Registry::SUPPORTED_EXTENSIONS_COUNT; ++i) {
                if (ext == Constants::Registry::SUPPORTED_EXTENSIONS[i])
                    return g_statusCache[path] = FolderStatus::Valid;
            }
        }
        return g_statusCache[path] = FolderStatus::Empty;
    }

    // ---------------------------------------------------------------------------
    // CalcTotalContentH — single formula for virtual scroll height used in
    // GetHistoryWindowBounds, WM_PAINT, WM_LBUTTONDOWN and WM_MOUSEMOVE.
    // Includes header area + rows + footer so window sizing and scroll are consistent.
    // ---------------------------------------------------------------------------
    static int CalcTotalContentH(int nEntries, UINT dpi) {
        int padding = MulDiv(Constants::History::HISTORY_PADDING, dpi, 96);
        int rowH = MulDiv(Constants::History::HISTORY_ROW_HEIGHT, dpi, 96);
        int footerH = MulDiv(Constants::History::HISTORY_FONT_SIZE + 2 + 8, dpi, 96);
        return padding * 2
               + MulDiv(2 * Constants::History::HISTORY_FONT_SIZE + 16, dpi, 96)
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
        // Preserve known statuses — only clear entries that are no longer in the list.
        // Unknown entries get validated lazily by the background timer.
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
        // Mark unchecked entries as Unknown so WM_PAINT shows them in neutral colour.
        bool hasUnknown = false;
        for (const auto &e: g_displayList) {
            if (g_statusCache.find(e.path) == g_statusCache.end()) {
                g_statusCache[e.path] = FolderStatus::Unknown;
                hasUnknown = true;
            }
        }
    }

    // ---------------------------------------------------------------------------
    // Free functions — used by FileHandler, UIManager, CommandExecuter
    // ---------------------------------------------------------------------------

    bool IsFolderValidForViewer(const std::wstring &folderPath) {
        return GetFolderStatus(folderPath) == FolderStatus::Valid;
    }

    void InvalidateHistoryFolderStatus(const std::wstring &path) {
        g_statusCache[path] = FolderStatus::Missing;
        HistoryListWnd &hw = uiManager.getHistoryListWindow();
        HWND hwnd = hw.GetHwnd();
        if (hwnd) InvalidateRect(hwnd, nullptr, FALSE);
    }

    void NotifyFolderContentsChanged(const std::wstring &path) {
        g_statusCache.erase(path); // force fresh check — stale cached status must not persist
        GetFolderStatus(path);     // re-validate now so paint sees Valid/Empty/Missing, not Unknown
        HistoryListWnd &hw = uiManager.getHistoryListWindow();
        HWND hwnd = hw.GetHwnd();
        if (hwnd) InvalidateRect(hwnd, nullptr, FALSE);
    }

    void LoadFolderHistoryFromDisk() {
        historyFoldersManager.LoadHistoryFromDisk();
    }

    void PushFolderHistory(const std::wstring &folderPath) {
        if (folderPath.empty())
            return;

        auto &history = historyFoldersManager.folderHistory;
        auto it = std::find(history.begin(), history.end(), folderPath);

        if (it != history.end()) {
            // Already exists: promote to front (MRU), no file write needed
            std::wstring tmp = *it;
            history.erase(it);
            history.insert(history.begin(), tmp);
        } else {
            // Genuinely new: prepend to RAM list
            history.insert(history.begin(), folderPath);
            if (static_cast<int>(history.size()) > Constants::History::HISTORY_MAX_DIRS_TO_SAVE)
                history.resize(static_cast<size_t>(Constants::History::HISTORY_MAX_DIRS_TO_SAVE));
            historyFoldersManager.AppendNewFolderToDisk(folderPath);
        }

        // Invalidate history window so the current-folder green updates immediately.
        auto &histWnd = uiManager.getHistoryListWindow();
        if (histWnd.IsVisible())
            InvalidateRect(histWnd.GetHwnd(), nullptr, FALSE);
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

    static std::vector<std::wstring> g_navSnap;
    static int g_navSnapVersion = 0;

    void CaptureNavigationSnapshot() {
        if (g_displayList.empty())
            BuildDisplayList();
        g_navSnap.clear();
        g_navSnap.reserve(g_displayList.size());
        for (const auto &e: g_displayList)
            g_navSnap.push_back(e.path);
        ++g_navSnapVersion;
    }

    const std::vector<std::wstring> &GetNavigationSnapshot() {
        return g_navSnap;
    }

    int GetNavigationSnapshotVersion() {
        return g_navSnapVersion;
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
        int totalH = CalcTotalContentH(entries, dpi);

        int minW = MulDiv(Constants::History::HISTORY_MIN_W, dpi, 96);
        int maxW = MulDiv(Constants::History::HISTORY_MAX_W, dpi, 96);
        int minH = MulDiv(Constants::History::HISTORY_MIN_H, dpi, 96);
        int maxH = MulDiv(Constants::History::HISTORY_MAX_H, dpi, 96);

        w = std::clamp(static_cast<int>(monW * 0.30f), minW, maxW);
        h = std::clamp(std::min(totalH, static_cast<int>(monH * 0.80f)), minH, std::min(maxH, static_cast<int>(monH * 0.80f)));
        x = monX + (monW - w) / 2;
        y = monY + (monH - h) / 2;
    }

    void ToggleHistoryFull() {
        auto &histWnd = uiManager.getHistoryListWindow();
        if (!histWnd.GetHwnd()) return;
        if (IsWindowVisible(histWnd.GetHwnd())) {
            if (g_showFullHistory) {
                histWnd.Hide();
            } else {
                g_showFullHistory = true;
                g_scrollOffsetY = 0;
                BuildDisplayList();
                CaptureNavigationSnapshot();
                g_hoverRow = 0;
                InvalidateRect(histWnd.GetHwnd(), nullptr, TRUE);
            }
        } else {
            g_showFullHistory = true;
            BuildDisplayList();
            CaptureNavigationSnapshot();
            int x, y, w, h;
            GetHistoryWindowBounds(g_hHistOwner ? g_hHistOwner : histWnd.GetHwnd(), x, y, w, h);
            SetWindowPos(histWnd.GetHwnd(), HWND_TOPMOST, x, y, w, h, SWP_FRAMECHANGED);
            g_hoverRow = 0;
            g_scrollOffsetY = 0;
            ShowWindow(histWnd.GetHwnd(), SW_SHOW);
            SetForegroundWindow(histWnd.GetHwnd());
            InvalidateRect(histWnd.GetHwnd(), nullptr, TRUE);
        }
    }

    // ---------------------------------------------------------------------------
    // Keyboard handling
    // ---------------------------------------------------------------------------
    bool HistoryListWnd::OnKeyDown(WPARAM vk, bool ctrl, bool shift, bool alt) {
        if (vk == VK_TAB && ctrl) {
            g_showFullHistory = !g_showFullHistory;
            g_scrollOffsetY = 0;
            BuildDisplayList();
            CaptureNavigationSnapshot();
            g_hoverRow = 0;
            InvalidateRect(m_hWnd, nullptr, TRUE);
            return true;
        }

        if (vk == 'Z' && ctrl && !shift && !alt && g_lastDeletedIndex >= 0) {
            auto &hist = historyFoldersManager.folderHistory;
            int insertAt = std::min(g_lastDeletedIndex, static_cast<int>(hist.size()));
            hist.insert(hist.begin() + insertAt, g_lastDeletedPath);
            historyFoldersManager.RewriteHistoryToDisk();
            if (g_lastDeletedWasFavorite) {
                historyFoldersManager.favorites.insert(g_lastDeletedPath);
                historyFoldersManager.RewriteFavoritesToDisk();
            }
            g_lastDeletedIndex = -1;
            BuildDisplayList();
            int newMax = static_cast<int>(g_displayList.size());
            if (g_hoverRow >= newMax) g_hoverRow = newMax - 1;
            int x, y, w, h;
            GetHistoryWindowBounds(g_hHistOwner ? g_hHistOwner : m_hWnd, x, y, w, h);
            SetWindowPos(m_hWnd, HWND_TOPMOST, x, y, w, h, SWP_FRAMECHANGED);
            InvalidateRect(m_hWnd, nullptr, TRUE);
            return true;
        }

        if (vk == VK_F5 && !ctrl && !shift && !alt) {
            // Full refresh: scan every history entry (not just visible rows).
            // Updates missing/empty/valid status AND size/count for each folder.
            // Consumes F5 so it doesn't propagate to the main app.
            for (const auto &path : historyFoldersManager.folderHistory)
                ScanDirForHistory(path);
            InvalidateRect(m_hWnd, nullptr, FALSE);
            return true;
        }

        int navMax = static_cast<int>(g_displayList.size());
        switch (vk) {
            case Shortcuts::SC_PANEL_HISTORY_TOGGLE:
                ToggleHistoryWindow();
                return true;

            case VK_UP:
                if (navMax > 0) {
                    g_hoverRow = (g_hoverRow <= 0) ? navMax - 1 : g_hoverRow - 1;
                    if (g_rowH > 0) {
                        int bodyH = g_bodyBottom - g_bodyTop;
                        if (g_hoverRow == navMax - 1) {
                            g_scrollOffsetY = std::max(0, navMax * g_rowH - bodyH);
                        } else {
                            int rowStart = g_hoverRow * g_rowH;
                            if (rowStart < g_scrollOffsetY)
                                g_scrollOffsetY = rowStart;
                        }
                    }
                    InvalidateRect(m_hWnd, nullptr, FALSE);
                }
                return true;

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
                return true;

            case VK_RETURN: {
                if (g_hoverRow >= 0 && g_hoverRow < navMax) {
                    std::wstring folder = g_displayList[g_hoverRow].path;
                    {
                        FolderStatus fs = GetFolderStatus(folder);
                        if (fs == FolderStatus::Missing) {
                            if (g_hHistOwner)
                                g_overlayManager.PostCenterMessage(g_hHistOwner,
                                    Constants::Messages::FOLDER_DEAD_MISSING);
                            return true;
                        }
                        // Empty folders fall through — both OpenDirectory and
                        // SpawnDirWndForFolder handle them so the user can paste into them.
                    }
                    bool shiftHeld = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                    if (shiftHeld) {
                        // Creating or hiding a spawned panel briefly steals focus,
                        // firing OnKillFocus which resets g_hoverRow. Save and
                        // restore so the selection stays on the row after the call.
                        const int savedRow = g_hoverRow;
                        std::wstring posLabel = uiManager.GetSpawnedDirWndPositionLabel(folder);
                        if (!posLabel.empty()) {
                            std::wstring posName = posLabel.substr(2, posLabel.length() - 3);
                            SlotInfo *slot = uiManager.GetLayout().getSlotByName(posName);
                            if (slot && slot->panel) {
                                slot->panel->Hide();
                            }
                        } else {
                            uiManager.SpawnDirWndForFolder(folder, m_hWnd);
                        }
                        g_hoverRow = savedRow;
                        InvalidateRect(m_hWnd, nullptr, FALSE);
                    } else {
                        ShowWindow(m_hWnd, SW_HIDE);
                        OpenDirectory(g_hHistOwner, folder);
                    }
                }
                return true;
            }

            case Shortcuts::HISTORY_FAVORITES_TOGGLE_KEY:
                if (g_hoverRow >= 0 && g_hoverRow < navMax) {
                    ToggleFavorite(g_hoverRow);
                    BuildDisplayList();
                    int newMax = static_cast<int>(g_displayList.size());
                    if (g_hoverRow >= newMax)
                        g_hoverRow = newMax - 1;
                    int x, y, w, h;
                    GetHistoryWindowBounds(g_hHistOwner ? g_hHistOwner : m_hWnd, x, y, w, h);
                    SetWindowPos(m_hWnd, HWND_TOPMOST, x, y, w, h, SWP_FRAMECHANGED);
                    InvalidateRect(m_hWnd, nullptr, TRUE);
                }
                return true;

            case VK_DELETE:
                if (ctrl && shift && alt) {
                    ClearFavoritesKeepHistory();
                    BuildDisplayList();
                    {
                        int x, y, w, h;
                        GetHistoryWindowBounds(g_hHistOwner ? g_hHistOwner : m_hWnd, x, y, w, h);
                        SetWindowPos(m_hWnd, HWND_TOPMOST, x, y, w, h, SWP_FRAMECHANGED);
                    }
                    InvalidateRect(m_hWnd, nullptr, TRUE);
                } else if (ctrl && shift) {
                    ClearHistoryKeepFavorites();
                    BuildDisplayList();
                    {
                        int x, y, w, h;
                        GetHistoryWindowBounds(g_hHistOwner ? g_hHistOwner : m_hWnd, x, y, w, h);
                        SetWindowPos(m_hWnd, HWND_TOPMOST, x, y, w, h, SWP_FRAMECHANGED);
                    }
                    InvalidateRect(m_hWnd, nullptr, TRUE);
                } else if (!ctrl && !shift && !alt) {
                    if (g_hoverRow >= 0 && g_hoverRow < navMax) {
                        const std::wstring &path = g_displayList[g_hoverRow].path;
                        bool wasFav = g_displayList[g_hoverRow].isFavorite;

                        auto &hist = historyFoldersManager.folderHistory;
                        int histIdx = -1;
                        for (int i = 0; i < static_cast<int>(hist.size()); ++i) {
                            if (hist[i] == path) {
                                histIdx = i;
                                break;
                            }
                        }

                        g_lastDeletedPath = path;
                        g_lastDeletedIndex = histIdx;
                        g_lastDeletedWasFavorite = wasFav;

                        if (histIdx >= 0)
                            hist.erase(hist.begin() + histIdx);
                        historyFoldersManager.RewriteHistoryToDisk();

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
                return true;

            default:
                return false; // forward unhandled keys to main app
        }
    }

    // ---------------------------------------------------------------------------
    // Back-buffer helpers
    // ---------------------------------------------------------------------------
    void HistoryListWnd::EnsureBackBuffer(HDC refDC, int w, int h) {
        if (m_bbDC && w == m_bbW && h == m_bbH) return;
        DestroyBackBuffer();
        m_bbDC = CreateCompatibleDC(refDC);
        m_bbBmp = CreateCompatibleBitmap(refDC, w, h);
        m_bbBmpOld = static_cast<HBITMAP>(SelectObject(m_bbDC, m_bbBmp));
        m_bbW = w;
        m_bbH = h;
    }

    void HistoryListWnd::DestroyBackBuffer() {
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

    // ---------------------------------------------------------------------------
    // Window procedure
    // ---------------------------------------------------------------------------
    LRESULT HistoryListWnd::HandlePanelMessage(UINT message, WPARAM wParam, LPARAM lParam) {
        if (message == WM_ERASEBKGND) return 1;
        switch (message) {
            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC screenDC = BeginPaint(m_hWnd, &ps);
                RECT rc;
                GetClientRect(m_hWnd, &rc);

                EnsureBackBuffer(screenDC, rc.right, rc.bottom);
                HDC hdc = m_bbDC;

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
                HBRUSH hBg = CreateSolidBrush(GetBgColor());
                FillRect(hdc, &rc, hBg);
                DeleteObject(hBg);
                SetBkMode(hdc, TRANSPARENT);

                int listFontSz = MulDiv(Constants::History::HISTORY_LIST_FONT_SIZE, dpi, 96);
                if (static_cast<int>(dpi) != m_cachedFontDpi) {
                    if (m_hFontTitle) {
                        DeleteObject(m_hFontTitle);
                        m_hFontTitle = nullptr;
                    }
                    if (m_hFontBody) {
                        DeleteObject(m_hFontBody);
                        m_hFontBody = nullptr;
                    }
                    if (m_hFontList) {
                        DeleteObject(m_hFontList);
                        m_hFontList = nullptr;
                    }
                    if (m_hFontIndexLink) {
                        DeleteObject(m_hFontIndexLink);
                        m_hFontIndexLink = nullptr;
                    }
                    if (m_hFontLink) {
                        DeleteObject(m_hFontLink);
                        m_hFontLink = nullptr;
                    }
                    m_hFontTitle = CreateFontW(
                            titleSz, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
                    m_hFontBody = CreateFontW(
                            fontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
                    m_hFontList = CreateFontW(
                            listFontSz, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
                    m_hFontIndexLink = CreateFontW(
                            listFontSz, 0, 0, 0, FW_NORMAL, FALSE, Constants::Links::UNDERLINE, FALSE,
                            DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
                    m_hFontLink = CreateFontW(
                            fontSize, 0, 0, 0, FW_NORMAL, FALSE, Constants::Links::UNDERLINE, FALSE,
                            DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
                    m_cachedFontDpi = static_cast<int>(dpi);
                }

                // Push counts into the real title bar
                int totalSaved = static_cast<int>(historyFoldersManager.folderHistory.size());
                int totalShown = static_cast<int>(g_displayList.size());
                int favCount = static_cast<int>(historyFoldersManager.favorites.size());
                {
                    std::wstring caption = L"Folder History  (showing "
                                           + std::to_wstring(totalShown) + L" of "
                                           + std::to_wstring(totalSaved) + L" saved)   \x2605 = Space (toggle fav)   "
                                           + std::to_wstring(favCount) + L" / "
                                           + std::to_wstring(Constants::History::HISTORY_MAX_FAVORITES_TO_SHOW)
                                           + L" favorites";
                    SetWindowTextW(m_hWnd, caption.c_str());
                }

                // Single shortcuts line
                SelectObject(hdc, m_hFontBody);
                int hintTop = rc.top + padding;
                int hintBot = hintTop + MulDiv(fontSize + 2, dpi, 96);
                {
                    constexpr wchar_t SHORTCUTS[] =
                            L"F5 = refresh dirs     Del = delete entry     Ctrl+Z = restore"
                            L"     Ctrl+Tab = full list"
                            L"     Ctrl+Shift+Del = clear history"
                            L"     Ctrl+Alt+Shift+Del = clear favorites";
                    SetTextColor(hdc, RGB(150, 150, 150));
                    RECT scRect = {rc.left + padding, hintTop, rc.right - padding, hintBot};
                    DrawTextW(hdc, SHORTCUTS, -1, &scRect,
                              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                }

                g_exeLinkRect = {};
                SelectObject(hdc, m_hFontTitle);

                // Separator (fixed) — placed after shortcuts line
                int sepY = hintBot + MulDiv(4, dpi, 96);
                HPEN hPen = CreatePen(PS_SOLID, 1, RGB(50, 50, 50));
                HPEN hOldPen = (HPEN) SelectObject(hdc, hPen);
                MoveToEx(hdc, rc.left + padding, sepY, nullptr);
                LineTo(hdc, rc.right - padding, sepY);
                SelectObject(hdc, hOldPen);
                DeleteObject(hPen);

                int footerSepY = rc.bottom - MulDiv(fontSize + 2 + 8, dpi, 96);

                // Rows (scrolled) — clipped to the body area between separator and footer
                SelectObject(hdc, m_hFontList);
                g_rowRects.clear();
                g_indexRects.clear();
                int rowsTop = sepY + MulDiv(6, dpi, 96);
                int bodyBottom = footerSepY;
                g_bodyTop = rowsTop;
                g_bodyBottom = bodyBottom;
                g_rowH = rowH;

                SaveDC(hdc);
                IntersectClipRect(hdc, rc.left, rowsTop, rc.right, bodyBottom);

                // Derive the folder currently open in the main app
                std::wstring currentFolder;
                if (!app.playlist.empty() && app.currentIndex >= 0) {
                    const std::wstring &cur = app.playlist[app.currentIndex];
                    const size_t sep = cur.find_last_of(L"\\/");
                    if (sep != std::wstring::npos)
                        currentFolder = cur.substr(0, sep);
                }

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

                        const bool isCurrent = (!currentFolder.empty() && entry.path == currentFolder);
                        auto _sit = g_statusCache.find(entry.path);
                        const FolderStatus rowStatus = (_sit != g_statusCache.end())
                                                           ? _sit->second
                                                           : FolderStatus::Unknown;
                        const bool isDead = (rowStatus == FolderStatus::Missing ||
                                             rowStatus == FolderStatus::Empty);

                        // Hover background
                        if (i == g_hoverRow) {
                            HBRUSH hHover = CreateSolidBrush(
                                    isDead
                                        ? RGB(60, 20, 20)
                                        : entry.isFavorite
                                              ? RGB(50, 50, 10)
                                              : RGB(40, 60, 80));
                            FillRect(hdc, &rowRect, hHover);
                            DeleteObject(hHover);
                        }

                        // Row index number — red for dead, green for current, blue otherwise
                        SetTextColor(hdc, isDead
                                              ? Constants::Theme::HistoryPanel::PATH_DEAD_DRIVE
                                              : (isCurrent
                                                     ? Constants::Theme::HistoryPanel::PATH_DRIVE_CURRENT
                                                     : RGB(100, 180, 255)));
                        SelectObject(hdc, m_hFontIndexLink);
                        std::wstring idxStr = std::to_wstring(i + 1);
                        RECT idxRect = {
                            rc.left + padding, rowTop,
                            rc.left + padding + indexW, rowBottom
                        };
                        DrawTextW(hdc, idxStr.c_str(), -1, &idxRect,
                                  DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
                        // Store index rect for click detection
                        g_indexRects.push_back(idxRect);
                        SelectObject(hdc, m_hFontList);

                        // Warning glyph for dead folders; star for favorites
                        {
                            RECT slotRect = {
                                rc.left + padding + indexW + MulDiv(4, dpi, 96), rowTop,
                                rc.left + padding + indexW + MulDiv(4, dpi, 96) + starW, rowBottom
                            };
                            if (isDead) {
                                SetTextColor(hdc, Constants::Theme::HistoryPanel::PATH_DEAD_DRIVE);
                                DrawTextW(hdc, L"⚠", -1, &slotRect,
                                          DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                            } else if (entry.isFavorite) {
                                SetTextColor(hdc, RGB(255, 220, 0));
                                DrawTextW(hdc, L"\x2605", -1, &slotRect,
                                          DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                            }
                        }

                        // Path text — three segments: drive, middle, folder
                        const bool isHov = (i == g_hoverRow);
                        const bool isFav = entry.isFavorite;

                        COLORREF driveColor = isDead
                                                  ? Constants::Theme::HistoryPanel::PATH_DEAD_DRIVE
                                                  : (isCurrent
                                                         ? Constants::Theme::HistoryPanel::PATH_DRIVE_CURRENT
                                                         : (isFav
                                                                ? (isHov ? Constants::Theme::HistoryPanel::PATH_DRIVE_FAV_HOVER : Constants::Theme::HistoryPanel::PATH_DRIVE_FAV)
                                                                : (isHov
                                                                       ? Constants::Theme::HistoryPanel::PATH_DRIVE_HOVER
                                                                       : Constants::Theme::HistoryPanel::PATH_DRIVE)));
                        COLORREF middleColor = isDead
                                                   ? Constants::Theme::HistoryPanel::PATH_DEAD_MIDDLE
                                                   : (isCurrent
                                                          ? Constants::Theme::HistoryPanel::PATH_MIDDLE_CURRENT
                                                          : (isFav
                                                                 ? (isHov ? RGB(255, 255, 160) : RGB(255, 240, 120))
                                                                 : (isHov
                                                                        ? RGB(255, 255, 255)
                                                                        : RGB(200, 200, 200))));
                        COLORREF folderColor = isDead
                                                   ? Constants::Theme::HistoryPanel::PATH_DEAD_FOLDER
                                                   : (isCurrent
                                                          ? Constants::Theme::HistoryPanel::PATH_FOLDER_CURRENT
                                                          : (isFav
                                                                 ? (isHov ? Constants::Theme::HistoryPanel::PATH_FOLDER_FAV_HOVER : Constants::Theme::HistoryPanel::PATH_FOLDER_FAV)
                                                                 : (isHov
                                                                        ? Constants::Theme::HistoryPanel::PATH_FOLDER_HOVER
                                                                        : Constants::Theme::HistoryPanel::PATH_FOLDER)));

                        // Split path into (drive, middle, folder)
                        const std::wstring &fp = entry.path;
                        std::wstring segDrive, segMiddle, segFolder;
                        if (fp.size() >= 2 && fp[1] == L':') {
                            segDrive = fp.substr(0, 2);
                            size_t lastSep = fp.find_last_of(L"\\/");
                            if (lastSep != std::wstring::npos && lastSep >= 2) {
                                segMiddle = fp.substr(2, lastSep - 1); // "\rest\of\path\"
                                segFolder = fp.substr(lastSep + 1); // "FolderName"
                            } else {
                                segFolder = fp.substr(2);
                            }
                        } else {
                            segMiddle = fp;
                        }

                        // Append spawned DirWnd position label if this folder has one open
                        std::wstring posLabel = uiManager.GetSpawnedDirWndPositionLabel(fp);
                        segFolder += posLabel;


                        // Size/count column — drawn on the far right, only when a SpawnedDirWnd is open for this folder
                        auto [sizeStr, imgCount] = uiManager.GetSpawnedDirWndSizeInfo(fp);
                        // Fall back to the scanned cache (populated by F5) when no DirWnd is open for this folder
                        if (sizeStr.empty()) {
                            auto cit = g_dirSizeCache.find(fp);
                            if (cit != g_dirSizeCache.end()) {
                                sizeStr  = ThumbnailPanelWnd::FormatDirSize(cit->second.bytes);
                                imgCount = cit->second.count;
                            }
                        }
                        const bool hasSizeInfo = !sizeStr.empty();
                        int sizeColW = hasSizeInfo ? MulDiv(100, dpi, 96) : 0;

                        LONG rowLeft = rc.left + padding + indexW + starW + MulDiv(10, dpi, 96);
                        LONG rowRight = rc.right - padding - (needsScrollbar ? SB_W + 2 : 0) - sizeColW;
                        LONG curX = rowLeft;

                        // 1. Drive letter
                        if (!segDrive.empty() && curX < rowRight) {
                            SetTextColor(hdc, driveColor);
                            SIZE sz = {};
                            GetTextExtentPoint32W(hdc, segDrive.c_str(),
                                                  static_cast<int>(segDrive.size()), &sz);
                            RECT dr = {curX, rowTop, curX + sz.cx, rowBottom};
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
                            RECT mr = {curX, rowTop, midRight, rowBottom};
                            DrawTextW(hdc, segMiddle.c_str(), -1, &mr,
                                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                            curX = (szM.cx < midRight - curX) ? curX + szM.cx : midRight;
                        }

                        // 3. Folder name
                        if (!segFolder.empty() && curX < rowRight) {
                            SetTextColor(hdc, folderColor);
                            RECT fr = {curX, rowTop, rowRight, rowBottom};
                            DrawTextW(hdc, segFolder.c_str(), -1, &fr,
                                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                        }

                        // 4. Size/count column — right-aligned after the path, only when a SpawnedDirWnd is open
                        if (hasSizeInfo) {
                            std::wstring sizeCountStr = sizeStr + L"/" + std::to_wstring(imgCount);
                            LONG colLeft  = rowRight;
                            LONG colRight = rc.right - padding - (needsScrollbar ? SB_W + 2 : 0);
                            SetTextColor(hdc, isDead
                                                  ? Constants::Theme::HistoryPanel::PATH_DEAD_MIDDLE
                                                  : RGB(140, 200, 140));
                            RECT scr = {colLeft, rowTop, colRight, rowBottom};
                            DrawTextW(hdc, sizeCountStr.c_str(), -1, &scr,
                                      DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
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

                // FOOTER — QIV link | [Fkey] Cache | [Fkey] Dir  ···  summary  history-size
                {
                    int footerTop = rc.bottom - MulDiv(fontSize + 2 + 4, dpi, 96);
                    int footerBot = rc.bottom;

                    SelectObject(hdc, m_hFontBody);
                    SetTextColor(hdc, RGB(150, 150, 150));

                    const UI::PanelLayout &layout = uiManager.GetLayout();
                    const UI::SlotInfo *slots[] = {&layout.center, &layout.top, &layout.right, &layout.bottom, &layout.left};
                    LONG rightEdge = rc.right - padding;
                    const int gap   = MulDiv(8, dpi, 96);

                    // RIGHT: summary just left of file size, then file size rightmost
                    const std::wstring &sizeValue = m_cachedSizeStr;
                    SIZE szFileSize = {};
                    GetTextExtentPoint32W(hdc, sizeValue.c_str(),
                                          static_cast<int>(sizeValue.size()), &szFileSize);
                    LONG fileSizeLeft = rightEdge - szFileSize.cx;

                    // Summary total: scanned cache (F5) when available, else live open-DirWnd sum
                    std::wstring summaryStr;
                    {
                        int64_t totalBytes = 0;
                        int     totalCount = 0;
                        if (!g_dirSizeCache.empty()) {
                            for (const auto &[p, info] : g_dirSizeCache) {
                                totalBytes += info.bytes;
                                totalCount += info.count;
                            }
                            summaryStr = ThumbnailPanelWnd::FormatDirSize(totalBytes)
                                         + L"/" + std::to_wstring(totalCount);
                        } else {
                            auto [liveStr, liveCount] = uiManager.GetAllOpenDirWndsSummary();
                            if (!liveStr.empty())
                                summaryStr = liveStr + L"/" + std::to_wstring(liveCount);
                        }
                    }
                    LONG summaryLeft = fileSizeLeft;
                    if (!summaryStr.empty()) {
                        SIZE szSummary = {};
                        GetTextExtentPoint32W(hdc, summaryStr.c_str(),
                                              static_cast<int>(summaryStr.size()), &szSummary);
                        summaryLeft = fileSizeLeft - gap - szSummary.cx;
                        SetTextColor(hdc, RGB(140, 200, 140));
                        RECT summaryRect = {summaryLeft, footerTop, summaryLeft + szSummary.cx, footerBot};
                        DrawTextW(hdc, summaryStr.c_str(), -1, &summaryRect,
                                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                    }
                    SetTextColor(hdc, Constants::Theme::HistoryPanel::SIZE_HIGHLIGHT);
                    RECT sizeRect = {fileSizeLeft, footerTop, rightEdge, footerBot};
                    DrawTextW(hdc, sizeValue.c_str(), -1, &sizeRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

                    // LEFT: QIV link first, then [Fkey] Cache, then [Fkey] Dir
                    {
                        LONG curX     = rc.left + padding;
                        LONG leftBound = summaryLeft - gap;

                        // QIV→dir link — first item, clickable
                        {
                            std::wstring linkText = L"QIV.exe/path="
                                                    + std::filesystem::path(Persistence::Registry::GetExePathW()).parent_path().wstring() + L"\\";
                            SIZE szLink = {};
                            SelectObject(hdc, m_hFontLink);
                            GetTextExtentPoint32W(hdc, linkText.c_str(),
                                                  static_cast<int>(linkText.size()), &szLink);
                            SetTextColor(hdc, RGB(100, 180, 255));
                            g_exeLinkRect = {curX, footerTop, curX + szLink.cx, footerBot};
                            DrawTextW(hdc, linkText.c_str(), -1, &g_exeLinkRect,
                                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                            curX += szLink.cx + gap;
                            SelectObject(hdc, m_hFontBody);
                        }

                        // Cache toggle key
                        std::wstring cacheKeyLabel = L"[" + FKeyLabel(Shortcuts::SC_PANEL_CACHE_TOGGLE) + L"]";
                        SetTextColor(hdc, RGB(100, 180, 255));
                        SelectObject(hdc, m_hFontLink);
                        SIZE szToggle = {};
                        GetTextExtentPoint32W(hdc, cacheKeyLabel.c_str(),
                                              static_cast<int>(cacheKeyLabel.size()), &szToggle);
                        g_cacheIndexRect = {curX, footerTop, curX + szToggle.cx, footerBot};
                        DrawTextW(hdc, cacheKeyLabel.c_str(), -1, &g_cacheIndexRect,
                                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                        curX += szToggle.cx;

                        // Cache status
                        SelectObject(hdc, m_hFontBody);
                        SetTextColor(hdc, RGB(150, 150, 150));
                        bool cacheFound = false;
                        std::wstring cachePosName;
                        for (auto *slot: slots) {
                            if (slot->panel == &uiManager.getCacheWindow() && slot->panel->IsVisible()) {
                                cachePosName = slot->name;
                                cacheFound = true;
                                break;
                            }
                        }
                        std::wstring cacheRest = cacheFound ? (L"Cache->" + cachePosName) : L"Cache->Hidden";
                        SIZE szCacheRest = {};
                        GetTextExtentPoint32W(hdc, cacheRest.c_str(),
                                              static_cast<int>(cacheRest.size()), &szCacheRest);
                        RECT cacheRestRect = {curX, footerTop, curX + szCacheRest.cx, footerBot};
                        DrawTextW(hdc, cacheRest.c_str(), -1, &cacheRestRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                        curX += szCacheRest.cx + gap;

                        // Dir-panel toggle key
                        std::wstring dirKeyLabel = L"[" + FKeyLabel(Shortcuts::SC_PANEL_DIR_TOGGLE) + L"]";
                        SetTextColor(hdc, RGB(100, 180, 255));
                        SelectObject(hdc, m_hFontLink);
                        SIZE szF5 = {};
                        GetTextExtentPoint32W(hdc, dirKeyLabel.c_str(),
                                              static_cast<int>(dirKeyLabel.size()), &szF5);
                        g_f5IndexRect = {curX, footerTop, curX + szF5.cx, footerBot};
                        DrawTextW(hdc, dirKeyLabel.c_str(), -1, &g_f5IndexRect,
                                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                        curX += szF5.cx;

                        // Dir status
                        SelectObject(hdc, m_hFontBody);
                        SetTextColor(hdc, RGB(150, 150, 150));
                        bool f5Found = false;
                        std::wstring f5PosName;
                        int f5HistoryIndex = -1;
                        for (auto *slot: slots) {
                            if (slot->panel == &uiManager.getDirWindow() && slot->panel->IsVisible()) {
                                if (app.currentIndex >= 0 && app.currentIndex < static_cast<int>(app.playlist.size())) {
                                    std::wstring currentImagePath = app.playlist[app.currentIndex];
                                    std::wstring f5Folder = std::filesystem::path(currentImagePath).parent_path().wstring();
                                    const auto &history = historyFoldersManager.folderHistory;
                                    for (int i = 0; i < static_cast<int>(history.size()); ++i) {
                                        if (_wcsicmp(f5Folder.c_str(), history[i].c_str()) == 0) {
                                            f5HistoryIndex = i + 1;
                                            break;
                                        }
                                    }
                                }
                                f5PosName = slot->name;
                                f5Found = true;
                                break;
                            }
                        }
                        std::wstring f5Rest = f5Found
                            ? ((f5HistoryIndex > 0) ? (L"Dir->#" + std::to_wstring(f5HistoryIndex) + L" " + f5PosName) : (L"Dir->? " + f5PosName))
                            : L"Dir->Hidden";
                        SIZE szF5Rest = {};
                        GetTextExtentPoint32W(hdc, f5Rest.c_str(),
                                              static_cast<int>(f5Rest.size()), &szF5Rest);
                        RECT f5RestRect = {curX, footerTop, std::min(curX + szF5Rest.cx, leftBound), footerBot};
                        DrawTextW(hdc, f5Rest.c_str(), -1, &f5RestRect,
                                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                    }
                }

                BitBlt(screenDC, 0, 0, rc.right, rc.bottom, hdc, 0, 0, SRCCOPY);
                EndPaint(m_hWnd, &ps);
                return 0;
            }

            case WM_GETMINMAXINFO: {
                UINT dpiMM = static_cast<UINT>(app.dpiScale * 96.0f);
                auto *mmi = reinterpret_cast<MINMAXINFO *>(lParam);
                mmi->ptMinTrackSize.x = MulDiv(Constants::History::HISTORY_MIN_W, dpiMM, 96);
                mmi->ptMinTrackSize.y = MulDiv(Constants::History::HISTORY_MIN_H, dpiMM, 96);
                mmi->ptMaxTrackSize.x = MulDiv(Constants::History::HISTORY_MAX_W, dpiMM, 96);
                mmi->ptMaxTrackSize.y = MulDiv(Constants::History::HISTORY_MAX_H, dpiMM, 96);
                return 0;
            }

            case WM_NCHITTEST: {
                POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                RECT wrc;
                GetWindowRect(m_hWnd, &wrc);
                const int border = std::max(4, static_cast<int>(6 * app.dpiScale));
                bool top = pt.y < wrc.top + border;
                bool bottom = pt.y >= wrc.bottom - border;
                bool left = pt.x < wrc.left + border;
                bool right = pt.x >= wrc.right - border;
                if (top && left) return HTTOPLEFT;
                if (top && right) return HTTOPRIGHT;
                if (bottom && left) return HTBOTTOMLEFT;
                if (bottom && right) return HTBOTTOMRIGHT;
                if (top) return HTTOP;
                if (bottom) return HTBOTTOM;
                if (left) return HTLEFT;
                if (right) return HTRIGHT;
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
                    POINT ptScreen = {mx, my};
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

                // Windows synthesizes a WM_MOUSEMOVE (cursor stationary) whenever
                // the window stack under the cursor changes — e.g. a spawned panel
                // opening/closing. Acting on it would stomp the keyboard selection
                // with whatever row happens to sit under the resting cursor.
                // Only react when the cursor actually moved.
                static POINT s_lastHoverPos = {LONG_MIN, LONG_MIN};
                if (mx == s_lastHoverPos.x && my == s_lastHoverPos.y)
                    return 0;
                s_lastHoverPos = {mx, my};

                // Handle header dragging to move window
                if (g_headerDragging) {
                    POINT ptScreen = {mx, my};
                    ClientToScreen(m_hWnd, &ptScreen);
                    int newX = g_headerDragWindowRect.left + (ptScreen.x - g_headerDragStartX);
                    int newY = g_headerDragWindowRect.top + (ptScreen.y - g_headerDragStartY);
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
                    POINT pt = {mx, my};
                    if (PtInRect(&g_exeLinkRect, pt)) {
                        SetCursor(LoadCursor(nullptr, IDC_HAND));
                        return 0;
                    }
                }
                if (g_f5IndexRect.right > g_f5IndexRect.left) {
                    POINT pt = {mx, my};
                    if (PtInRect(&g_f5IndexRect, pt)) {
                        SetCursor(LoadCursor(nullptr, IDC_HAND));
                        return 0;
                    }
                }
                // Hand cursor over history row indexes
                for (const auto &idxRect: g_indexRects) {
                    if (idxRect.right > idxRect.left) {
                        POINT pt = {mx, my};
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
                    POINT pt = {mx, my};
                    if (PtInRect(&g_exeLinkRect, pt)) {
                        std::wstring dir = std::filesystem::path(Persistence::Registry::GetExePathW()).parent_path().wstring();
                        ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOW);
                        return 0;
                    }
                }

                // [F5] label click — toggle F5
                if (g_f5IndexRect.right > g_f5IndexRect.left) {
                    POINT pt = {mx, my};
                    if (PtInRect(&g_f5IndexRect, pt)) {
                        uiManager.Toggle(uiManager.getDirWindow());
                        return 0;
                    }
                }

                // [F3] (Cache) label click — toggle Cache
                if (g_cacheIndexRect.right > g_cacheIndexRect.left) {
                    POINT pt = {mx, my};
                    if (PtInRect(&g_cacheIndexRect, pt)) {
                        uiManager.Toggle(uiManager.getCacheWindow());
                        return 0;
                    }
                }

                // History row index click — open in explorer
                for (int i = 0; i < static_cast<int>(g_indexRects.size()); ++i) {
                    const RECT &idxRect = g_indexRects[i];
                    if (idxRect.right > idxRect.left) {
                        POINT pt = {mx, my};
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
                        FolderStatus fs = GetFolderStatus(folder);
                        if (fs == FolderStatus::Missing) {
                            if (g_hHistOwner)
                                g_overlayManager.PostCenterMessage(g_hHistOwner,
                                    Constants::Messages::FOLDER_DEAD_MISSING);
                            return 0;
                        }
                        // Empty folders fall through — OpenDirectory handles them.
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
        BuildDisplayList();
        int x, y, w, h;
        GetHistoryWindowBounds(hParent, x, y, w, h);
        InitFloating(hInstance, hParent, L"QIV_HistoryWindow", L"Folder History",
                     w, h, CS_DBLCLKS);
        if (!m_hWnd) return;
        SetWindowPos(m_hWnd, nullptr, x, y, 0, 0, SWP_NOZORDER | SWP_NOSIZE);
        ShowWindow(m_hWnd, SW_HIDE);
    }

    void HistoryListWnd::OnSetFocus() {
        UI::SetActivePanelWindow(m_hWnd);
        // Restore the selection that OnKillFocus saved when focus bounced away
        // (spawned panel open/close, overlay activation, etc.).
        if (g_hoverRow < 0 && g_savedHoverRow >= 0) {
            int navMax = static_cast<int>(g_displayList.size());
            g_hoverRow = (g_savedHoverRow < navMax)
                             ? g_savedHoverRow
                             : (navMax > 0 ? navMax - 1 : -1);
        }
    }

    void HistoryListWnd::OnKillFocus() {
        if (g_hoverRow >= 0) g_savedHoverRow = g_hoverRow;
        g_hoverRow = -1;
        // InvalidateRect is already called by FloatingPanelWnd before this hook.
    }

    void HistoryListWnd::Show() {
        if (!m_hWnd) return;
        g_showFullHistory = app.historyFullModeEnabled;
        BuildDisplayList();
        CaptureNavigationSnapshot();
        int x, y, w, h;
        GetHistoryWindowBounds(g_hHistOwner ? g_hHistOwner : m_hWnd, x, y, w, h);
        SetWindowPos(m_hWnd, HWND_TOPMOST, x, y, w, h, SWP_FRAMECHANGED);
        g_hoverRow = 0;
        g_savedHoverRow = -1; // fresh open always starts at the top row
        g_scrollOffsetY = 0;

        // Cache history file size so WM_PAINT needs no I/O
        {
            std::error_code ec;
            auto bytes = std::filesystem::file_size(historyFoldersManager.GetFilePath(), ec);
            if (!ec) {
                wchar_t buf[64];
                if (bytes >= 1024ULL * 1024)
                    swprintf_s(buf, L"History - %.3f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
                else if (bytes >= 1024)
                    swprintf_s(buf, L"History - %.3f KB", static_cast<double>(bytes) / 1024.0);
                else
                    swprintf_s(buf, L"History - %llu Bytes", static_cast<unsigned long long>(bytes));
                m_cachedSizeStr = buf;
            } else {
                m_cachedSizeStr = L"History - n/a";
            }
        }

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
