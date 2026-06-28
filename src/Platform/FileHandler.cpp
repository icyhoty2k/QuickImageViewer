#include "FileHandler.h"
#include "../AppState.h"
#include <commdlg.h>
#include <filesystem>
#include <ranges>
#include <cwctype>
#include <vector>

namespace fs = std::filesystem;

constexpr auto is_image_ext = [](const std::wstring &ext) -> bool {
    std::wstring l_ext = ext | std::views::transform([](wchar_t c) {
                             return (wchar_t) std::towlower(c);
                         })
                         | std::ranges::to<std::wstring>();

    static const std::vector<std::wstring> supported = {
        L".jpg", L".jpeg", L".png", L".bmp", L".webp", L".gif", L".tiff", L".tif",
        L".ico", L".heic", L".heif", L".jxr", L".wdp", L".hdp", L".dds",
        L".dng", L".cr2", L".cr3", L".nef", L".arw"
    };

    return std::ranges::find(supported, l_ext) != supported.end();
};

void OpenInitialImage(HWND hWnd) {
    g_app.isDialogVisible = true; // Block Esc processing

    wchar_t szFile[MAX_PATH] = {0};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hWnd;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile);

    // Comprehensive filter list
    ofn.lpstrFilter = L"All Supported Images\0*.jpg;*.jpeg;*.png;*.bmp;*.webp;*.gif;*.tiff;*.tif;*.ico;*.heic;*.heif;*.jxr;*.wdp;*.hdp;*.dds;*.dng;*.cr2;*.cr3;*.nef;*.arw\0"
            L"JPEG Files (*.jpg;*.jpeg)\0*.jpg;*.jpeg\0"
            L"PNG Files (*.png)\0*.png\0"
            L"WebP Files (*.webp)\0*.webp\0"
            L"All Files (*.*)\0*.*\0";

    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;

    // Load last used folder from Registry
    wchar_t lastDir[MAX_PATH] = {0};
    DWORD lastDirSize = sizeof(lastDir);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\QuickImageViewer", L"LastFolder", RRF_RT_REG_SZ, nullptr, lastDir, &lastDirSize) == ERROR_SUCCESS) {
        ofn.lpstrInitialDir = lastDir;
    }

    bool success = GetOpenFileNameW(&ofn);
    g_app.isDialogVisible = false;

    if (success) {
        fs::path filePath = fs::canonical(fs::path(szFile));

        // Save the successful directory to Registry for next time
        std::wstring folderPath = filePath.parent_path().wstring();
        RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\QuickImageViewer", L"LastFolder", REG_SZ, folderPath.c_str(), static_cast<DWORD>((folderPath.length() + 1) * sizeof(wchar_t)));

        g_app.playlist = fs::directory_iterator(filePath.parent_path())
                         | std::views::filter([](const auto &e) {
                             return e.is_regular_file() && is_image_ext(e.path().extension().wstring());
                         })
                         | std::views::transform([](const auto &e) {
                             return fs::canonical(e.path()).wstring();
                         })
                         | std::ranges::to<std::vector<std::wstring> >();

        // Sort alphabetically so arrow-key navigation matches Explorer order
        std::ranges::sort(g_app.playlist);

        auto it = std::ranges::find(g_app.playlist, filePath.wstring());
        if (it != g_app.playlist.end()) {
            extern void LoadImageIndex(HWND, int);
            LoadImageIndex(hWnd, static_cast<int>(std::distance(g_app.playlist.begin(), it)));
        }
    } else {
        // Only quit if we were at initial startup, not if F2 was pressed
        if (g_app.playlist.empty()) {
            PostQuitMessage(0);
        }
    }
}

void OpenSpecificImage(HWND hWnd, const std::wstring &filePathStr) {
    fs::path filePath(filePathStr);

    if (!fs::exists(filePath) || !fs::is_regular_file(filePath)) return;
    filePath = fs::canonical(filePath);

    // --- NEW: Fast-path for same-directory files ---
    if (!g_app.playlist.empty()) {
        fs::path currentDir = fs::path(g_app.playlist[0]).parent_path();
        if (filePath.parent_path() == currentDir) {
            auto it = std::ranges::find(g_app.playlist, filePath.wstring());
            if (it != g_app.playlist.end()) {
                extern void LoadImageIndex(HWND, int);
                LoadImageIndex(hWnd, static_cast<int>(std::distance(g_app.playlist.begin(), it)));
                return; // Skip rebuild entirely
            }
        }
    }
    // -----------------------------------------------

    // Fallback: Build the playlist from scratch
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
