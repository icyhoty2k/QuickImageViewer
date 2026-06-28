#include "FileHandler.h"
#include "../AppState.h"
#include "RegistrySetup.h" // For Save/LoadStringSetting
#include "Constants.h"
#include <commdlg.h>
#include <filesystem>
#include <ranges>
#include <cwctype>
#include <vector>

namespace fs = std::filesystem;

bool is_image_ext(const std::wstring &ext) {
    for (size_t i = 0; i < Constants::Registry::SUPPORTED_EXTENSIONS_COUNT; ++i) {
        if (_wcsicmp(ext.c_str(), Constants::Registry::SUPPORTED_EXTENSIONS[i]) == 0) {
            return true;
        }
    }
    return false;
}

void OpenInitialImage(HWND hWnd) {
    g_app.isDialogVisible = true;

    // Build the filter buffer correctly for Windows API
    std::vector<wchar_t> filterBuffer;

    // 1. Description: "All Supported Images"
    std::wstring desc = L"All Supported Images";
    filterBuffer.insert(filterBuffer.end(), desc.begin(), desc.end());
    filterBuffer.push_back(L'\0');

    // 2. Extensions: "*.jpg;*.jpeg;..."
    std::wstring exts;
    for (size_t i = 0; i < Constants::Registry::SUPPORTED_EXTENSIONS_COUNT; ++i) {
        exts += L"*" + std::wstring(Constants::Registry::SUPPORTED_EXTENSIONS[i]);
        if (i < Constants::Registry::SUPPORTED_EXTENSIONS_COUNT - 1) exts += L";";
    }
    filterBuffer.insert(filterBuffer.end(), exts.begin(), exts.end());
    filterBuffer.push_back(L'\0');

    // 3. Description: "All Files (*.*)"
    std::wstring allFilesDesc = L"All Files (*.*)";
    filterBuffer.insert(filterBuffer.end(), allFilesDesc.begin(), allFilesDesc.end());
    filterBuffer.push_back(L'\0');

    // 4. Extension: "*.*"
    std::wstring allFilesExt = L"*.*";
    filterBuffer.insert(filterBuffer.end(), allFilesExt.begin(), allFilesExt.end());
    filterBuffer.push_back(L'\0');

    // Final double null terminator required by API
    filterBuffer.push_back(L'\0');

    wchar_t szFile[MAX_PATH] = {0};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hWnd;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = filterBuffer.data();
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_EXPLORER;

    wchar_t lastDir[MAX_PATH] = {0};
    System::LoadStringSetting(Constants::Registry::LAST_FOLDER, lastDir, MAX_PATH);
    if (lastDir[0] != L'\0') ofn.lpstrInitialDir = lastDir;

    bool success = GetOpenFileNameW(&ofn);
    g_app.isDialogVisible = false;

    if (success) {
        fs::path filePath = fs::canonical(fs::path(szFile));
        System::SaveStringSetting(Constants::Registry::LAST_FOLDER, filePath.parent_path().wstring());

        g_app.playlist = fs::directory_iterator(filePath.parent_path())
                         | std::views::filter([](const auto &e) {
                             return e.is_regular_file() && is_image_ext(e.path().extension().wstring());
                         })
                         | std::views::transform([](const auto &e) {
                             return fs::canonical(e.path()).wstring();
                         })
                         | std::ranges::to<std::vector<std::wstring> >();

        std::ranges::sort(g_app.playlist);

        auto it = std::ranges::find(g_app.playlist, filePath.wstring());
        if (it != g_app.playlist.end()) {
            extern void LoadImageIndex(HWND, int);
            LoadImageIndex(hWnd, static_cast<int>(std::distance(g_app.playlist.begin(), it)));
        }
    } else if (g_app.playlist.empty()) {
        PostQuitMessage(0);
    }
}

void OpenSpecificImage(HWND hWnd, const std::wstring &filePathStr) {
    fs::path filePath(filePathStr);
    if (!fs::exists(filePath) || !fs::is_regular_file(filePath)) return;
    filePath = fs::canonical(filePath);

    if (!g_app.playlist.empty()) {
        if (filePath.parent_path() == fs::path(g_app.playlist[0]).parent_path()) {
            auto it = std::ranges::find(g_app.playlist, filePath.wstring());
            if (it != g_app.playlist.end()) {
                extern void LoadImageIndex(HWND, int);
                LoadImageIndex(hWnd, static_cast<int>(std::distance(g_app.playlist.begin(), it)));
                return;
            }
        }
    }

    g_app.playlist = fs::directory_iterator(filePath.parent_path())
                     | std::views::filter([](const auto &e) {
                         return e.is_regular_file() && is_image_ext(e.path().extension().wstring());
                     })
                     | std::views::transform([](const auto &e) {
                         return fs::canonical(e.path()).wstring();
                     })
                     | std::ranges::to<std::vector<std::wstring> >();

    std::ranges::sort(g_app.playlist);
    auto it = std::ranges::find(g_app.playlist, filePath.wstring());
    if (it != g_app.playlist.end()) {
        extern void LoadImageIndex(HWND, int);
        LoadImageIndex(hWnd, static_cast<int>(std::distance(g_app.playlist.begin(), it)));
    }
}
