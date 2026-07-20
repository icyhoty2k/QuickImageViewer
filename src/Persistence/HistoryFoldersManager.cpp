#include "HistoryFoldersManager.h"

#include <windows.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include "../AppState.h"
#include "RegistryManager.h"
#include "../Platform/WriteQueue.h"

namespace fs = std::filesystem;

extern AppState app;

// Returns the filename to use on disk, prepending the dedicated-mode prefix when needed.
// Both history and favorites go through this so neither instance ever touches the other's files.
static std::wstring PrefixedFileName(const std::wstring &baseName) {
    if (app.isDedicated)
        return std::wstring(Constants::DedicatedMode::DEDICATED_MODE_GLOBAL_PREFIX) + baseName;
    return baseName;
}

// ---------------------------------------------------------------------------
// Backup helpers  (file-scope, not exposed in the header)
// ---------------------------------------------------------------------------

// Returns (creating if needed) the QivBackup folder next to the executable.
static fs::path GetBackupDir() {
    std::wstring exePath = Persistence::Registry::GetExePathW();
    fs::path exeDir = exePath.empty()
                          ? fs::current_path()
                          : fs::path(exePath).parent_path();

    // Strip leading slash from the constant (it is stored as L"/QivBackup")
    std::wstring folderName = Constants::History::HISTORY_FAVORITES_BACKUP_FOLDER;
    if (!folderName.empty() && (folderName.front() == L'/' || folderName.front() == L'\\'))
        folderName = folderName.substr(1);

    fs::path backupDir = exeDir / folderName;
    if (!fs::exists(backupDir))
        fs::create_directories(backupDir);

    return backupDir;
}

// Builds a dated backup file path:  QivBackup/qivHistory_dd.MM.YYYY_HH.MM.SS.bak
static fs::path MakeBackupPath(const fs::path &backupDir,
                               const std::wstring &origFileName,
                               const SYSTEMTIME &st) {
    wchar_t suffix[64];
    swprintf_s(suffix, L"_%02d.%02d.%04d_%02d.%02d.%02d",
               st.wDay, st.wMonth, st.wYear,
               st.wHour, st.wMinute, st.wSecond);

    std::wstring stem = fs::path(origFileName).stem().wstring();
    std::wstring ext = Constants::History::HISTORY_FAVORITES_BACKUP_EXTENSION;
    return backupDir / (stem + suffix + ext);
}

// Writes the mandatory first line:
//   BACKUP COMPUTER_NAME, dd.MM.YYYY, HH:MM:SS.ms, Backup Version Schema : 1.0
static void WriteBackupHeader(std::wofstream &f, const SYSTEMTIME &st) {
    wchar_t compName[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD sz = MAX_COMPUTERNAME_LENGTH + 1;
    GetComputerNameW(compName, &sz);

    wchar_t header[512];
    swprintf_s(header,
               L"BACKUP %s, %02d.%02d.%04d, %02d:%02d:%02d.%03d, %s",
               compName,
               st.wDay, st.wMonth, st.wYear,
               st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
               Constants::History::HISTORY_FAVORITES_BACKUP_VERSION);
    f << header << L"\n";
}

// ---------------------------------------------------------------------------
// GetFilePath  —  full path to qivHistory.txt next to the executable
// ---------------------------------------------------------------------------
std::wstring HistoryFoldersManager::GetFilePath() const {
    const std::wstring name = PrefixedFileName(historyFileName);
    std::wstring exePath = Persistence::Registry::GetExePathW();
    if (exePath.empty())
        return name;

    fs::path appDir = fs::path(exePath).parent_path();

    if (!historyFolderName.empty()) {
        appDir /= historyFolderName;
        if (!fs::exists(appDir))
            fs::create_directories(appDir);
    }

    return (appDir / name).wstring();
}

// ---------------------------------------------------------------------------
// GetFavoritesFilePath  —  full path to qivFavorites.txt next to the executable
// ---------------------------------------------------------------------------
std::wstring HistoryFoldersManager::GetFavoritesFilePath() const {
    const std::wstring name = PrefixedFileName(favoritesFileName);
    std::wstring exePath = Persistence::Registry::GetExePathW();
    if (exePath.empty())
        return name;

    fs::path appDir = fs::path(exePath).parent_path();

    if (!historyFolderName.empty()) {
        appDir /= historyFolderName;
        if (!fs::exists(appDir))
            fs::create_directories(appDir);
    }

    return (appDir / name).wstring();
}

// ---------------------------------------------------------------------------
// LoadHistoryFromDisk
//   Reads qivHistory.txt (paths only, no prefix) and qivFavorites.txt
//   (paths only, no prefix) separately. folderHistory is built oldest-first
//   then reversed so index 0 = most recently visited.
//
//   Legacy support: lines starting with '*' in qivHistory.txt are treated as
//   old-format favorites and migrated to the favorites set automatically.
// ---------------------------------------------------------------------------
void HistoryFoldersManager::LoadHistoryFromDisk() {
    folderHistory.clear();
    favorites.clear();

    // --- Load favorites first (small file, O(1) lookup during history load) ---
    {
        std::wifstream fav(GetFavoritesFilePath());
        if (fav.is_open()) {
            std::wstring line;
            while (std::getline(fav, line)) {
                if (!line.empty() && line.back() == L'\r') line.pop_back();
                if (line.empty()) continue;
                favorites.insert(line); // no cap — all favorites must always be loadable
            }
        }
    }

    // --- Load history ---
    {
        std::wifstream file(GetFilePath());
        if (!file.is_open())
            return;

        std::wstring line;
        while (std::getline(file, line)) {
            if (!line.empty() && line.back() == L'\r') line.pop_back();
            if (line.empty()) continue;

            // Legacy: old-format '*' prefix — migrate to favorites set
            if (line.front() == static_cast<wchar_t>(Constants::History::HISTORY_FAVORITES_MARK)) {
                line = line.substr(1);
                if (!line.empty() &&
                    static_cast<int>(favorites.size()) < app.historyMaxFavs)
                    favorites.insert(line);
            }

            if (line.empty()) continue;

            // Skip duplicates (hand-edited files)
            bool alreadyKnown = false;
            for (const auto &entry: folderHistory) {
                if (entry == line) {
                    alreadyKnown = true;
                    break;
                }
            }
            if (alreadyKnown) continue;

            if (static_cast<int>(folderHistory.size()) >= app.historyMaxDirsSave)
                break;

            folderHistory.push_back(line);
        }
    }

    // File is oldest-first; reverse so index 0 = most recently visited
    std::reverse(folderHistory.begin(), folderHistory.end());
}

// ---------------------------------------------------------------------------
// MergeHistoryFromDisk
//   Merges disk state into the current in-memory MRU list without destroying
//   the in-memory order.  Both history and favorites are merged two-way,
//   duplicate-free:
//     Memory → disk : appends entries present in RAM but absent from the file.
//     Disk → memory : adds entries present in the file but absent from RAM.
//
//   Missing files are treated as empty (no crash, no data loss).
//   History uses append-only writes.  Favorites use a full rewrite only when
//   RAM has entries the file does not (the file is small, ≤ historyMaxFavs).
// ---------------------------------------------------------------------------
void HistoryFoldersManager::MergeHistoryFromDisk() {
    // ---- HISTORY --------------------------------------------------------
    std::vector<std::wstring> diskList;
    std::unordered_set<std::wstring> diskSet;
    {
        std::wifstream file(GetFilePath());
        if (file.is_open()) {
            std::wstring line;
            while (std::getline(file, line)) {
                if (!line.empty() && line.back() == L'\r') line.pop_back();
                if (line.empty()) continue;
                // Legacy '*' prefix — treat path as history entry
                if (line.front() == static_cast<wchar_t>(Constants::History::HISTORY_FAVORITES_MARK))
                    line = line.substr(1);
                if (line.empty() || diskSet.count(line)) continue;
                diskList.push_back(line);
                diskSet.insert(line);
            }
        }
        // Missing file → diskList empty; memory entries will recreate it via appends below.
    }
    // Disk file is oldest-first; reverse so newest entries are appended to memory last
    // (they belong at lower priority than anything already in the MRU list).
    std::reverse(diskList.begin(), diskList.end());

    // Memory → disk: entries in RAM that are not on disk
    for (const auto &path : folderHistory) {
        if (!diskSet.count(path))
            AppendNewFolderToDisk(path);
    }

    // Disk → memory: entries on disk that are not in RAM
    {
        std::unordered_set<std::wstring> memSet;
        memSet.reserve(folderHistory.size() + diskList.size());
        memSet.insert(folderHistory.begin(), folderHistory.end());
        for (const auto &path : diskList) {
            if (!memSet.count(path) &&
                static_cast<int>(folderHistory.size()) < app.historyMaxDirsSave) {
                folderHistory.push_back(path);
                memSet.insert(path);
            }
        }
    }

    // ---- FAVORITES ------------------------------------------------------
    std::unordered_set<std::wstring> diskFavSet;
    {
        std::wifstream fav(GetFavoritesFilePath());
        if (fav.is_open()) {
            std::wstring line;
            while (std::getline(fav, line)) {
                if (!line.empty() && line.back() == L'\r') line.pop_back();
                if (!line.empty())
                    diskFavSet.insert(line);
            }
        }
        // Missing file → diskFavSet empty; memory favorites will be rewritten below if any.
    }

    // Disk → memory: disk favorites not in RAM (no cap — all favorites must be visible)
    for (const auto &path : diskFavSet) {
        if (!favorites.count(path))
            favorites.insert(path);
    }

    // Memory → disk: if RAM has favorites the file does not, rewrite (file is small)
    bool memHasNewFavs = false;
    for (const auto &path : favorites) {
        if (!diskFavSet.count(path)) { memHasNewFavs = true; break; }
    }
    if (memHasNewFavs)
        RewriteFavoritesToDisk();
}

// ---------------------------------------------------------------------------
// AppendNewFolderToDisk
//   Appends a single path to qivHistory.txt. No prefix — plain path only.
//   The caller guarantees this path is genuinely new (not already in folderHistory).
// ---------------------------------------------------------------------------
void HistoryFoldersManager::AppendNewFolderToDisk(const std::wstring &folderPath) const {
    std::wstring path = GetFilePath();
    std::wstring entry = folderPath;
    g_writeQueue.PushTask([path = std::move(path), entry = std::move(entry)]() {
        std::wofstream f(path, std::ios::out | std::ios::app);
        if (f.is_open()) f << entry << L"\n";
    });
}

// ---------------------------------------------------------------------------
// RewriteHistoryToDisk
//   Rewrites qivHistory.txt with all current paths (oldest-first, no prefix).
//   Does NOT touch qivFavorites.txt.
//   Used by ClearHistoryKeepFavorites.
// ---------------------------------------------------------------------------
void HistoryFoldersManager::RewriteHistoryToDisk() const {
    std::wstring path = GetFilePath();
    // Snapshot oldest-first (reverse of MRU) at push time so the drain thread
    // writes a consistent view even if folderHistory changes before it wakes.
    std::vector<std::wstring> snap(folderHistory.rbegin(), folderHistory.rend());
    g_writeQueue.PushTask([path = std::move(path), snap = std::move(snap)]() {
        std::wofstream f(path, std::ios::out | std::ios::trunc);
        if (!f.is_open()) return;
        for (const auto &e : snap) f << e << L"\n";
    });
}

// ---------------------------------------------------------------------------
// RewriteFavoritesToDisk
//   Rewrites qivFavorites.txt with up to HISTORY_MAX_FAVORITES_TO_SHOW paths.
//   Does NOT touch qivHistory.txt.
//   Used by ToggleFavorite and ClearFavoritesKeepHistory.
// ---------------------------------------------------------------------------
void HistoryFoldersManager::RewriteFavoritesToDisk() const {
    std::wstring path = GetFavoritesFilePath();
    std::vector<std::wstring> snap(favorites.begin(), favorites.end());
    const int maxFavs = app.historyMaxFavs;
    g_writeQueue.PushTask([path = std::move(path), snap = std::move(snap), maxFavs]() {
        std::wofstream f(path, std::ios::out | std::ios::trunc);
        if (!f.is_open()) return;
        int written = 0;
        for (const auto &e : snap) {
            if (written >= maxFavs) break;
            f << e << L"\n";
            ++written;
        }
    });
}

// ---------------------------------------------------------------------------
// BackupHistoryToDisk
//   Writes a dated snapshot of the current in-RAM history to QivBackup/.
//   Must be called BEFORE clearing RAM and before RewriteHistoryToDisk.
//   Format:  first line = header, then paths oldest-first (same as the live file).
// ---------------------------------------------------------------------------
void HistoryFoldersManager::BackupHistoryToDisk() const {
    // Capture time and data snapshot now so the backup reflects the exact moment
    // the user triggered it, not whenever the drain thread eventually runs.
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::wstring fileName = PrefixedFileName(historyFileName);
    std::vector<std::wstring> snap(folderHistory.rbegin(), folderHistory.rend());
    g_writeQueue.PushTask([st, fileName = std::move(fileName), snap = std::move(snap)]() {
        fs::path backupPath = MakeBackupPath(GetBackupDir(), fileName, st);
        std::wofstream f(backupPath, std::ios::out | std::ios::trunc);
        if (!f.is_open()) return;
        WriteBackupHeader(f, st);
        for (const auto &e : snap) f << e << L"\n";
    });
}

// ---------------------------------------------------------------------------
// BackupFavoritesToDisk
//   Writes a dated snapshot of the current in-RAM favorites to QivBackup/.
//   Must be called BEFORE clearing RAM and before RewriteFavoritesToDisk.
//   Format:  first line = header, then favorite paths.
// ---------------------------------------------------------------------------
void HistoryFoldersManager::BackupFavoritesToDisk() const {
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::wstring fileName = PrefixedFileName(favoritesFileName);
    std::vector<std::wstring> snap(favorites.begin(), favorites.end());
    const int maxFavs = app.historyMaxFavs;
    g_writeQueue.PushTask([st, fileName = std::move(fileName), snap = std::move(snap), maxFavs]() {
        fs::path backupPath = MakeBackupPath(GetBackupDir(), fileName, st);
        std::wofstream f(backupPath, std::ios::out | std::ios::trunc);
        if (!f.is_open()) return;
        WriteBackupHeader(f, st);
        int written = 0;
        for (const auto &e : snap) {
            if (written >= maxFavs) break;
            f << e << L"\n";
            ++written;
        }
    });
}
