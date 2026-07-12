#pragma once
#include <string>
#include <vector>
#include <unordered_set>

#include "../Platform/Constants.h"

// HistoryFoldersManager — in-memory store for folder history + favorites.
//
// TWO-FILE FORMAT (separate files, minimal NVMe writes):
//
//   qivHistory.txt   — one folder path per line, no prefix, append-only.
//                      Order: oldest at top, newest at bottom.
//                      In RAM, folderHistory is reversed to MRU (index 0 = most recent).
//                      Capped at HISTORY_MAX_DIRS_TO_SAVE entries.
//
//   qivFavorites.txt — one folder path per line, no prefix, max HISTORY_MAX_FAVORITES_TO_SHOW entries.
//                      Rewritten only when a favorite is toggled.
//
// WRITE STRATEGY — NVMe-friendly:
//   PushFolderHistory   → AppendNewFolderToDisk()   (history only, single line append)
//   ToggleFavorite      → RewriteFavoritesToDisk()  (favorites only, small file ≤10 lines)
//   ClearHistory        → RewriteHistoryToDisk()    (history only, does NOT touch favorites)
//   ClearFavorites      → RewriteFavoritesToDisk()  (favorites only, does NOT touch history)

struct HistoryFoldersManager {
    // MRU-ordered list of all folder paths (index 0 = most recently visited).
    // Holds up to HISTORY_MAX_DIRS_TO_SAVE entries.
    std::vector<std::wstring> folderHistory;

    // Set of paths that are marked as favorites (for O(1) lookup).
    std::unordered_set<std::wstring> favorites;

    std::wstring historyFileName   = Constants::History::HISTORY_FILE_NAME;
    std::wstring favoritesFileName = Constants::History::FAVORITES_FILE_NAME;
    std::wstring historyFolderName; // optional subfolder next to the exe

    // Returns the full path to qivHistory.txt next to the executable.
    std::wstring GetFilePath() const;

    // Returns the full path to qivFavorites.txt next to the executable.
    std::wstring GetFavoritesFilePath() const;

    // Reads both files and populates folderHistory + favorites.
    // Call once at startup before any push.
    void LoadHistoryFromDisk();

    // Appends 'folderPath' to qivHistory.txt only (genuinely new entries only).
    // Does NOT rewrite the whole file.
    void AppendNewFolderToDisk(const std::wstring &folderPath) const;

    // Rewrites qivHistory.txt only (used by ClearHistoryKeepFavorites).
    // Does NOT touch qivFavorites.txt.
    void RewriteHistoryToDisk() const;

    // Rewrites qivFavorites.txt only (used by ToggleFavorite and ClearFavorites).
    // Does NOT touch qivHistory.txt.
    void RewriteFavoritesToDisk() const;

    // Backs up qivHistory.txt to QivBackup/ before a clear operation.
    // Call BEFORE modifying RAM or rewriting the file.
    void BackupHistoryToDisk() const;

    // Backs up qivFavorites.txt to QivBackup/ before a clear operation.
    // Call BEFORE modifying RAM or rewriting the file.
    void BackupFavoritesToDisk() const;
};
