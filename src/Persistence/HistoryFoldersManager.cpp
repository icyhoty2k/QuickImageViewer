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
// LoadHistoryFromDisk
//   Reads the whole file once at startup.
//   Lines starting with '*' are favorites; the '*' is stripped for the path.
//   folderHistory is built in file order then reversed so index 0 = most
//   recent (newest line = bottom of file = MRU).
//   Caps at HISTORY_MAX_DIRS_TO_SAVE.
// ---------------------------------------------------------------------------
void HistoryFoldersManager::LoadHistoryFromDisk() {
    folderHistory.clear();
    favorites.clear();

    std::wstring filePath = GetFilePath();
    std::wifstream file(filePath);
    if (!file.is_open())
        return;

    std::wstring line;
    while (std::getline(file, line)) {
        // Strip CR for files written on Windows with \r\n
        if (!line.empty() && line.back() == L'\r')
            line.pop_back();
        if (line.empty())
            continue;

        bool isFavorite = (line.front() == static_cast<wchar_t>(Constants::History::HISTORY_FAVORITES_MARK));
        std::wstring path = isFavorite ? line.substr(1) : line;

        if (path.empty())
            continue;

        // Skip duplicates that may exist in a hand-edited file
        bool alreadyKnown = false;
        for (const auto &entry : folderHistory) {
            if (entry == path) { alreadyKnown = true; break; }
        }
        if (alreadyKnown)
            continue;

        if (static_cast<int>(folderHistory.size()) >= Constants::History::HISTORY_MAX_DIRS_TO_SAVE)
            break;

        folderHistory.push_back(path);
        if (isFavorite)
            favorites.insert(path);
    }

    file.close();

    // File is oldest-first; reverse so index 0 = most recently visited
    std::reverse(folderHistory.begin(), folderHistory.end());
}

// ---------------------------------------------------------------------------
// AppendNewFolderToDisk
//   Appends a single line to the file. The caller guarantees this path is
//   not already in folderHistory (i.e. it is genuinely new).
//   Favorites are prepended with '*' if applicable (new entries won't be
//   favorites yet, but the function checks anyway for correctness).
// ---------------------------------------------------------------------------
void HistoryFoldersManager::AppendNewFolderToDisk(const std::wstring &folderPath) const {
    std::wstring filePath = GetFilePath();
    std::wofstream file(filePath, std::ios::out | std::ios::app);
    if (!file.is_open())
        return;

    bool isFav = (favorites.count(folderPath) > 0);
    if (isFav)
        file << static_cast<wchar_t>(Constants::History::HISTORY_FAVORITES_MARK);
    file << folderPath << L"\n";
    file.close();
}

// ---------------------------------------------------------------------------
// RewriteFileToDisk
//   Writes the entire current state to disk.
//   Used when toggling a favorite or clearing history (both need to change
//   existing lines, which append-only cannot do).
//   File order: oldest first (reverse of folderHistory MRU order), so that
//   new appends always go to the bottom and LoadHistoryFromDisk reverses
//   correctly next time.
// ---------------------------------------------------------------------------
void HistoryFoldersManager::RewriteFileToDisk() const {
    std::wstring filePath = GetFilePath();
    std::wofstream file(filePath, std::ios::out | std::ios::trunc);
    if (!file.is_open())
        return;

    // Write oldest first (index size-1 down to 0)
    for (int i = static_cast<int>(folderHistory.size()) - 1; i >= 0; --i) {
        const std::wstring &path = folderHistory[i];
        bool isFav = (favorites.count(path) > 0);
        if (isFav)
            file << static_cast<wchar_t>(Constants::History::HISTORY_FAVORITES_MARK);
        file << path << L"\n";
    }

    file.close();
}
