#include "HistoryFoldersManager.h"

#include <windows.h>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

// -------------------------------------------------------------------------
// Locates the text file alongside the portable executable
// -------------------------------------------------------------------------
std::wstring HistoryFoldersManager::GetQIVStateFilePath() const {
    wchar_t buffer[MAX_PATH];
    DWORD size = GetModuleFileNameW(nullptr, buffer, MAX_PATH);

    if (size == 0 || size == MAX_PATH) {
        // Fallback to relative current working directory if the Win32 API fails
        return historyFileName;
    }

    fs::path exePath(buffer);
    fs::path appDir = exePath.parent_path();

    // If a subfolder is defined in the constants, append it and ensure it exists
    if (!historyFolderName.empty()) {
        appDir /= historyFolderName;
        if (!fs::exists(appDir)) {
            fs::create_directories(appDir);
        }
    }

    return (appDir / historyFileName).wstring();
}

// -------------------------------------------------------------------------
// Dumps this struct's current data to the text file
// -------------------------------------------------------------------------
void HistoryFoldersManager::SaveHistoryToDisk() const {
    std::wstring filePath = GetQIVStateFilePath();

    // std::ios::trunc overwrites the old file with the complete current state
    std::wofstream file(filePath, std::ios::out | std::ios::trunc);

    if (file.is_open()) {
        // Line 1 is strictly for the last picture
        file << lastPicture << L"\n";

        // Line 2 and beyond are the history paths
        for (const auto &record: folderHistory) {
            file << record << L"\n";
        }

        file.close();
    }
}

// -------------------------------------------------------------------------
// Reads the text file from disk and populates this struct's variables
// -------------------------------------------------------------------------
void HistoryFoldersManager::LoadHistoryFromDisk() {
    std::wstring filePath = GetQIVStateFilePath();

    // Clear existing memory state in case this is called multiple times
    lastPicture.clear();
    folderHistory.clear();

    std::wifstream file(filePath);
    if (file.is_open()) {
        std::wstring line;

        // Parse Line 1: Last Picture
        if (std::getline(file, line)) {
            // Strip potential carriage return if saved on mixed line endings
            if (!line.empty() && line.back() == L'\r') {
                line.pop_back();
            }
            lastPicture = line;
        }

        // Parse Line 2+: Folder History
        while (std::getline(file, line)) {
            if (!line.empty() && line.back() == L'\r') {
                line.pop_back();
            }
            if (!line.empty()) {
                folderHistory.push_back(line);
            }
        }

        file.close();
    }
}
