#pragma once
#include <string>
#include <vector>

#include "Constants.h"

struct HistoryFoldersManager {
    std::wstring lastPicture;
    std::wstring historyFileName = Constants::History::HISTORY_FILE_NAME;
    std::wstring historyFolderName;
    std::vector<std::wstring> folderHistory;

    // Locates the text file alongside the executable
    // 'const' because getting a path doesn't modify the struct's data
    std::wstring GetQIVStateFilePath() const;

    // Dumps this struct's current data to the text file
    // 'const' because saving to disk doesn't modify the struct in memory
    void SaveHistoryToDisk() const;

    // Reads the text file from disk and populates this struct's variables
    void LoadHistoryFromDisk();
};

extern HistoryFoldersManager historyFoldersManager;
