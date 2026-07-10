#pragma once
#include <string>
#include <vector>
#include <unordered_set>

#include "../Platform/Constants.h"

// HistoryFoldersManager — in-memory store for folder history + favorites.
//
// File format (qivHistory.txt):
//   Each line is one folder path.
//   Lines beginning with '*' are favorites (the '*' is stripped on load).
//   Order in file is append order (oldest at top, newest at bottom).
//   In RAM, folderHistory is kept in MRU order (index 0 = most recent).
//   Favorites are tracked separately; their position in the display
//   is controlled by Constants::History::HISTORY_FAVORITES_POSITION.
//
// Write strategy: APPEND-ONLY for new unique paths.
//   LoadHistoryFromDisk() reads the whole file into RAM on startup.
//   AppendNewFolderToDisk() writes only genuinely new entries.
//   ToggleFavoriteOnDisk() rewrites the file (needed to flip the '*').
//   ClearHistoryOnDisk()   rewrites the file (favorites-only rewrite).

struct HistoryFoldersManager {
    // MRU-ordered list of all folder paths (index 0 = most recently visited).
    // Holds up to HISTORY_MAX_DIRS_TO_SAVE entries.
    std::vector<std::wstring> folderHistory;

    // Set of paths that are marked as favorites (for O(1) lookup).
    std::unordered_set<std::wstring> favorites;

    std::wstring historyFileName = Constants::History::HISTORY_FILE_NAME;
    std::wstring historyFolderName; // optional subfolder next to the exe

    // Returns the full path to qivHistory.txt next to the executable.
    std::wstring GetFilePath() const;

    // Reads the file and populates folderHistory + favorites.
    // Call once at startup before any push.
    void LoadHistoryFromDisk();

    // Appends 'folderPath' to the file only if it is not already known.
    // Does NOT rewrite the whole file.
    void AppendNewFolderToDisk(const std::wstring &folderPath) const;

    // Rewrites the entire file (used by ToggleFavorite and ClearHistory).
    void RewriteFileToDisk() const;
};
