#include "HistoryFoldersManager.h"

#include <windows.h>
#include <filesystem>
#include <fstream>
#include "../AppState.h"
#include "RegistryManager.h"

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
                if (static_cast<int>(favorites.size()) >= Constants::History::HISTORY_MAX_FAVORITES_TO_SHOW)
                    break;
                favorites.insert(line);
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
                    static_cast<int>(favorites.size()) < Constants::History::HISTORY_MAX_FAVORITES_TO_SHOW)
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

            if (static_cast<int>(folderHistory.size()) >= Constants::History::HISTORY_MAX_DIRS_TO_SAVE)
                break;

            folderHistory.push_back(line);
        }
    }

    // File is oldest-first; reverse so index 0 = most recently visited
    std::reverse(folderHistory.begin(), folderHistory.end());
}

// ---------------------------------------------------------------------------
// AppendNewFolderToDisk
//   Appends a single path to qivHistory.txt. No prefix — plain path only.
//   The caller guarantees this path is genuinely new (not already in folderHistory).
// ---------------------------------------------------------------------------
void HistoryFoldersManager::AppendNewFolderToDisk(const std::wstring &folderPath) const {
    std::wofstream file(GetFilePath(), std::ios::out | std::ios::app);
    if (!file.is_open())
        return;
    file << folderPath << L"\n";
}

// ---------------------------------------------------------------------------
// RewriteHistoryToDisk
//   Rewrites qivHistory.txt with all current paths (oldest-first, no prefix).
//   Does NOT touch qivFavorites.txt.
//   Used by ClearHistoryKeepFavorites.
// ---------------------------------------------------------------------------
void HistoryFoldersManager::RewriteHistoryToDisk() const {
    std::wofstream file(GetFilePath(), std::ios::out | std::ios::trunc);
    if (!file.is_open())
        return;

    // Write oldest-first (reverse of MRU order) so that new appends stay at bottom
    for (int i = static_cast<int>(folderHistory.size()) - 1; i >= 0; --i)
        file << folderHistory[i] << L"\n";
}

// ---------------------------------------------------------------------------
// RewriteFavoritesToDisk
//   Rewrites qivFavorites.txt with up to HISTORY_MAX_FAVORITES_TO_SHOW paths.
//   Does NOT touch qivHistory.txt.
//   Used by ToggleFavorite and ClearFavoritesKeepHistory.
// ---------------------------------------------------------------------------
void HistoryFoldersManager::RewriteFavoritesToDisk() const {
    std::wofstream file(GetFavoritesFilePath(), std::ios::out | std::ios::trunc);
    if (!file.is_open())
        return;

    int written = 0;
    for (const auto &path: favorites) {
        if (written >= Constants::History::HISTORY_MAX_FAVORITES_TO_SHOW) break;
        file << path << L"\n";
        ++written;
    }
}

// ---------------------------------------------------------------------------
// BackupHistoryToDisk
//   Writes a dated snapshot of the current in-RAM history to QivBackup/.
//   Must be called BEFORE clearing RAM and before RewriteHistoryToDisk.
//   Format:  first line = header, then paths oldest-first (same as the live file).
// ---------------------------------------------------------------------------
void HistoryFoldersManager::BackupHistoryToDisk() const {
    SYSTEMTIME st;
    GetLocalTime(&st);

    fs::path backupPath = MakeBackupPath(GetBackupDir(), PrefixedFileName(historyFileName), st);
    std::wofstream f(backupPath, std::ios::out | std::ios::trunc);
    if (!f.is_open())
        return;

    WriteBackupHeader(f, st);

    // Oldest-first — same order as the live qivHistory.txt
    for (int i = static_cast<int>(folderHistory.size()) - 1; i >= 0; --i)
        f << folderHistory[i] << L"\n";
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

    fs::path backupPath = MakeBackupPath(GetBackupDir(), PrefixedFileName(favoritesFileName), st);
    std::wofstream f(backupPath, std::ios::out | std::ios::trunc);
    if (!f.is_open())
        return;

    WriteBackupHeader(f, st);

    int written = 0;
    for (const auto &path: favorites) {
        if (written >= Constants::History::HISTORY_MAX_FAVORITES_TO_SHOW) break;
        f << path << L"\n";
        ++written;
    }
}
