#pragma once
#include <windows.h>
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

// ---------------------------------------------------------------------------
// PATH HYGIENE
//
// qivHistory.txt and qivFavorites.txt are plain text sitting next to the exe —
// a user can and will open them in Notepad. Everything read back is therefore
// UNTRUSTED INPUT: blank lines, stray indentation, quoted paths pasted from
// Explorer's "Copy as path", forward slashes, trailing backslashes, the same
// folder listed twice in different case, outright garbage. None of it may reach
// the panel, the walk, or the filesystem calls unchecked.
//
// Every path entering either list goes through NormalizeFolderPath() first, and
// every comparison between two paths goes through the case-insensitive helpers
// below — Windows folders are case-insensitive, so plain operator== on wstring
// is the wrong test and silently produces duplicate rows.
// ---------------------------------------------------------------------------
namespace HistoryPath {

    // Cleans one raw line (from disk) or one raw path (from the app) into the
    // canonical form stored in the lists:
    //   • trims surrounding whitespace and a wrapping pair of double quotes
    //   • converts '/' to '\' and collapses repeated separators
    //   • drops a trailing separator, except on a drive root ("D:\")
    //   • rejects anything that cannot be an absolute folder path: empty, not
    //     drive-qualified and not UNC, containing characters illegal in Win32
    //     paths (< > " | ? *) or control characters, or absurdly long
    // Returns false when the input is unusable; 'out' is then untouched.
    bool Normalize(const std::wstring &raw, std::wstring &out);

    // Case-insensitive equality / hashing, so "D:\Pics" and "d:\pics" are one
    // folder everywhere: dedupe, favorite lookups, status cache, walk anchors.
    bool Equal(const std::wstring &a, const std::wstring &b);

    struct HashCI {
        size_t operator()(const std::wstring &s) const;
    };
    struct EqualCI {
        bool operator()(const std::wstring &a, const std::wstring &b) const { return Equal(a, b); }
    };

} // namespace HistoryPath

// A set of folder paths that treats case-different spellings as one entry while
// preserving whatever casing was first stored, for display.
using FolderPathSet = std::unordered_set<std::wstring, HistoryPath::HashCI, HistoryPath::EqualCI>;

struct HistoryFoldersManager {
    // MRU-ordered list of all folder paths (index 0 = most recently visited).
    // Holds up to HISTORY_MAX_DIRS_TO_SAVE entries. Normalized, duplicate-free.
    std::vector<std::wstring> folderHistory;

    // Set of paths that are marked as favorites (for O(1) lookup).
    FolderPathSet favorites;

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

    // Merges disk into the current in-memory list without destroying MRU order:
    //   - Appends to disk any memory entries absent from the file (no duplicates).
    //   - Appends to memory any disk entries absent from RAM (picked up from external edits).
    // Favorites are fully reloaded (no ordering concern there).
    void MergeHistoryFromDisk();

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
