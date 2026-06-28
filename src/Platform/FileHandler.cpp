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

// Unified image extension check using the constant array
constexpr auto is_image_ext = [](const std::wstring &ext) -> bool {
    std::wstring l_ext = ext | std::views::transform([](wchar_t c) {
                             return (wchar_t) std::towlower(c);
                         })
                         | std::ranges::to<std::wstring>();

    for (size_t i = 0; i < Constants::Registry::SUPPORTED_EXTENSIONS_COUNT; ++i) {
        if (l_ext == Constants::Registry::SUPPORTED_EXTENSIONS[i]) return true;
    }
    return false;
};

void OpenInitialImage(HWND hWnd) {
    g_app.isDialogVisible = true;

    // Dynamically build the filter from Constants
    std::wstring filter = L"All Supported Images\0";
    for (size_t i = 0; i < Constants::Registry::SUPPORTED_EXTENSIONS_COUNT; ++i) {
        filter += L"*" + std::wstring(Constants::Registry::SUPPORTED_EXTENSIONS[i]) + (i == Constants::Registry::SUPPORTED_EXTENSIONS_COUNT - 1 ? L"" : L";");
    }
    filter += L"\0All Files (*.*)\0*.*\0";

    wchar_t szFile[MAX_PATH] = {0};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hWnd;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile);
    ofn.lpstrFilter = filter.c_str();
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;

    // Load last folder via our System helper
    wchar_t lastDir[MAX_PATH] = {0};
    System::LoadStringSetting(Constants::Registry::LAST_FOLDER, lastDir, MAX_PATH);
    if (lastDir[0] != L'\0') ofn.lpstrInitialDir = lastDir;

    bool success = GetOpenFileNameW(&ofn);
    g_app.isDialogVisible = false;

    if (success) {
        fs::path filePath = fs::canonical(fs::path(szFile));

        // Save last folder via our System helper
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
