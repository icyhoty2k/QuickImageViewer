#include "HistoryFoldersManager.h"

#include <windows.h>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// GetFilePath  —  full path to qivHistory.txt next to the executable
// ---------------------------------------------------------------------------
std::wstring HistoryFoldersManager::GetFilePath() const {
    wchar_t buffer[MAX_PATH];
    DWORD size = GetModuleFileNameW(nullptr, buffer, MAX_PATH);

    if (size == 0 || size == MAX_PATH)
        return historyFileName; // fallback: relative to CWD

    fs::path appDir = fs::path(buffer).parent_path();

    if (!historyFolderName.empty()) {
        appDir /= historyFolderName;
        if (!fs::exists(appDir))
            fs::create_directories(appDir);
    }

    return (appDir / historyFileName).wstring();
}

// ---------------------------------------------------------------------------
// GetFavoritesFilePath  —  full path to qivFavorites.txt next to the executable
// ---------------------------------------------------------------------------
std::wstring HistoryFoldersManager::GetFavoritesFilePath() const {
    wchar_t buffer[MAX_PATH];
    DWORD size = GetModuleFileNameW(nullptr, buffer, MAX_PATH);

    if (size == 0 || size == MAX_PATH)
        return favoritesFileName;

    fs::path appDir = fs::path(buffer).parent_path();

    if (!historyFolderName.empty()) {
        appDir /= historyFolderName;
        if (!fs::exists(appDir))
            fs::create_directories(appDir);
    }

    return (appDir / favoritesFileName).wstring();
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
            for (const auto &entry : folderHistory) {
                if (entry == line) { alreadyKnown = true; break; }
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
    for (const auto &path : favorites) {
        if (written >= Constants::History::HISTORY_MAX_FAVORITES_TO_SHOW) break;
        file << path << L"\n";
        ++written;
    }
}
