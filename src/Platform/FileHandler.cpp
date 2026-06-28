#include "FileHandler.h"
#include "../AppState.h"
#include "RegistrySetup.h"
#include "Constants.h"
#include <commdlg.h>
#include <filesystem>
#include <ranges>
#include <vector>
#include "WorkerThread.h"

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

    // Build the filter buffer correctly to avoid dialog failure
    std::vector<wchar_t> filterBuffer;

    // 1. All Supported Images
    std::wstring desc = L"All Supported Images";
    filterBuffer.insert(filterBuffer.end(), desc.begin(), desc.end());
    filterBuffer.push_back(L'\0');

    std::wstring exts;
    for (size_t i = 0; i < Constants::Registry::SUPPORTED_EXTENSIONS_COUNT; ++i) {
        exts += L"*" + std::wstring(Constants::Registry::SUPPORTED_EXTENSIONS[i]);
        if (i < Constants::Registry::SUPPORTED_EXTENSIONS_COUNT - 1) exts += L";";
    }
    filterBuffer.insert(filterBuffer.end(), exts.begin(), exts.end());
    filterBuffer.push_back(L'\0');

    // 2. All Files
    std::wstring allFilesDesc = L"All Files (*.*)";
    filterBuffer.insert(filterBuffer.end(), allFilesDesc.begin(), allFilesDesc.end());
    filterBuffer.push_back(L'\0');
    filterBuffer.insert(filterBuffer.end(), L"*.*", L"*.*" + 3);
    filterBuffer.push_back(L'\0');
    filterBuffer.push_back(L'\0'); // Double null terminator

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

void LoadImageIndex(HWND hWnd, int index) {
    g_decoderWorker.ClearQueue();
    g_ioWorker.ClearQueue();

    if (index < 0 || index >= static_cast<int>(g_app.playlist.size())) return;

    if (g_app.currentIndex != index) g_app.viewport = ViewportState{};
    g_app.currentIndex = index;
    g_app.wantedIndex.store(index, std::memory_order_release);

    const std::wstring &currentPath = g_app.playlist[index];
    SetWindowTextW(hWnd, (currentPath.substr(currentPath.find_last_of(L"\\/") + 1) + L" - QuickImageViewer").c_str());

    if (g_app.renderer && SUCCEEDED(g_app.renderer->LoadBitmap(nullptr, 0, 0, currentPath))) {
        InvalidateRect(hWnd, nullptr, FALSE);
        SetTimer(hWnd, 1001, Constants::PRELOAD_TIMER_COUNTDOWN, nullptr);
        return;
    }

    g_ioWorker.PushTask([currentPath, index, hWnd]() {
        if (g_app.wantedIndex.load(std::memory_order_acquire) != index) return;
        if (g_app.renderer && SUCCEEDED(g_app.renderer->PreloadBitmap(currentPath, index))) {
            PostMessage(hWnd, Constants::WM_QIV_REPAINT, 0, 0);
        }
    });

    SetTimer(hWnd, 1001, Constants::PRELOAD_TIMER_COUNTDOWN, nullptr);
}

void OpenSpecificImage(HWND hWnd, const std::wstring &filePathStr) {
    fs::path filePath(filePathStr);
    if (!fs::exists(filePath) || !fs::is_regular_file(filePath)) return;
    filePath = fs::canonical(filePath);

    if (!g_app.playlist.empty()) {
        if (filePath.parent_path() == fs::path(g_app.playlist[0]).parent_path()) {
            auto it = std::ranges::find(g_app.playlist, filePath.wstring());
            if (it != g_app.playlist.end()) {
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
        LoadImageIndex(hWnd, static_cast<int>(std::distance(g_app.playlist.begin(), it)));
    }
}
