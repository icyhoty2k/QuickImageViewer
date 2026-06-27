#include "FileHandler.h"
#include "AppState.h"
#include <commdlg.h>
#include <filesystem>
#include <ranges>
#include <cwctype>

namespace fs = std::filesystem;

constexpr auto is_image_ext = [](const std::wstring &ext) -> bool {
    std::wstring l_ext = ext | std::views::transform([](wchar_t c) { return (wchar_t) std::towlower(c); })
                         | std::ranges::to<std::wstring>();
    return l_ext == L".jpg" || l_ext == L".jpeg" || l_ext == L".png" || l_ext == L".bmp" || l_ext == L".webp" || l_ext == L".gif" || l_ext == L".tiff";
};

void OpenInitialImage(HWND hWnd) {
    g_app.isDialogVisible = true; // Block Esc processing

    wchar_t szFile[MAX_PATH] = {0};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hWnd;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile);
    ofn.lpstrFilter = L"Images\0*.jpg;*.jpeg;*.png;*.bmp;*.webp;*.gif;*.tiff\0";
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;

    bool success = GetOpenFileNameW(&ofn);
    g_app.isDialogVisible = false; // Always clear the flag immediately after the dialog closes

    if (success) {
        fs::path filePath(szFile);
        g_app.playlist = fs::directory_iterator(filePath.parent_path())
                         | std::views::filter([](const auto &e) { return e.is_regular_file() && is_image_ext(e.path().extension().wstring()); })
                         | std::views::transform([](const auto &e) { return e.path().wstring(); })
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
        // You can check if the playlist is empty to decide:
        if (g_app.playlist.empty()) {
            PostQuitMessage(0);
        }
    }
}

void OpenSpecificImage(HWND hWnd, const std::wstring &filePathStr) {
    fs::path filePath(filePathStr);

    // Safety check: ensure the file actually exists before processing
    if (!fs::exists(filePath) || !fs::is_regular_file(filePath)) {
        return;
    }

    // Build the playlist from the directory of the injected file
    g_app.playlist = fs::directory_iterator(filePath.parent_path())
                     | std::views::filter([](const auto &e) { return e.is_regular_file() && is_image_ext(e.path().extension().wstring()); })
                     | std::views::transform([](const auto &e) { return e.path().wstring(); })
                     | std::ranges::to<std::vector<std::wstring> >();

    // Sort alphabetically so arrow-key navigation matches Explorer order
    std::ranges::sort(g_app.playlist);

    // Find the image in the playlist and load it
    auto it = std::ranges::find(g_app.playlist, filePath.wstring());
    if (it != g_app.playlist.end()) {
        extern void LoadImageIndex(HWND, int);
        LoadImageIndex(hWnd, static_cast<int>(std::distance(g_app.playlist.begin(), it)));
    }
}
