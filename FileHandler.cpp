#include "FileHandler.h"
#include "AppState.h"
#include <commdlg.h>
#include <filesystem>
#include <ranges>
#include <cwctype>

namespace fs = std::filesystem;

constexpr auto is_image_ext = [](const std::wstring& ext) -> bool {
    std::wstring l_ext = ext | std::views::transform([](wchar_t c) { return (wchar_t)std::towlower(c); }) 
                             | std::ranges::to<std::wstring>();
    return l_ext == L".jpg" || l_ext == L".jpeg" || l_ext == L".png" || l_ext == L".bmp" || l_ext == L".webp";
};

void OpenInitialImage(HWND hWnd) {
    wchar_t szFile[MAX_PATH] = { 0 };
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hWnd;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile);
    ofn.lpstrFilter = L"Images\0*.jpg;*.jpeg;*.png;*.bmp;*.webp\0";
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;

    if (GetOpenFileNameW(&ofn)) {
        fs::path filePath(szFile);
        g_app.playlist = fs::directory_iterator(filePath.parent_path()) 
            | std::views::filter([](const auto& e) { return e.is_regular_file() && is_image_ext(e.path().extension().wstring()); })
            | std::views::transform([](const auto& e) { return e.path().wstring(); })
            | std::ranges::to<std::vector<std::wstring>>();
            
        auto it = std::ranges::find(g_app.playlist, filePath.wstring());
        if (it != g_app.playlist.end()) {
            extern void LoadImageIndex(HWND, int); // Forward declare from WicDecoder
            LoadImageIndex(hWnd, static_cast<int>(std::distance(g_app.playlist.begin(), it)));
        }
    } else {
        PostQuitMessage(0); 
    }
}