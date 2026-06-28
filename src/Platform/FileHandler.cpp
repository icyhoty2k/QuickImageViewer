#include "FileHandler.h"
#include "../AppState.h"
#include "RegistrySetup.h"
#include "Constants.h"
#include <commdlg.h>
#include <filesystem>
#include <ranges>
#include <vector>
#include "WorkerThread.h"
#include "DriveInfo.h"

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// EnsureIoWorkerStarted
// ---------------------------------------------------------------------------
// Called once per folder open with the folder path. Detects the drive type
// and starts g_ioWorker with the right thread count on first call.
// Subsequent calls are no-ops (IsStarted() guard).
// ---------------------------------------------------------------------------
static void EnsureIoWorkerStarted(const std::wstring &folderPath) {
    if (!g_ioWorker.IsStarted()) {
        size_t optimal = DriveInfo::GetOptimalIoThreadCount(folderPath);
        g_ioWorker.Start(optimal);
    }
}

bool is_image_ext(const std::wstring &ext) {
    for (size_t i = 0; i < Constants::Registry::SUPPORTED_EXTENSIONS_COUNT; ++i) {
        if (_wcsicmp(ext.c_str(), Constants::Registry::SUPPORTED_EXTENSIONS[i]) == 0) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// GetDiskOffset
// ---------------------------------------------------------------------------
// Returns the logical cluster number (LCN) of the first extent of a file
// using FSCTL_GET_RETRIEVAL_POINTERS. This is the physical position of the
// file's first data cluster on the disk platter.
//
// On SSDs or network paths the ioctl may fail — in that case we return
// UINT64_MAX so the file sorts to the end and the fallback is alphabetical
// order for those entries (harmless on SSD anyway).
//
// Requires SE_MANAGE_VOLUME_NAME privilege on some older Windows versions,
// but on Windows 10/11 with NTFS opening with FILE_FLAG_NO_BUFFERING is
// sufficient for the ioctl without elevation.
// ---------------------------------------------------------------------------
static UINT64 GetDiskOffset(const std::wstring &path) {
    HANDLE hFile = CreateFileW(
            path.c_str(),
            FILE_READ_ATTRIBUTES, // minimal access — no data read
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_NO_BUFFERING, // required for retrieval pointer ioctl
            nullptr
            );

    if (hFile == INVALID_HANDLE_VALUE)
        return UINT64_MAX;

    // FSCTL_GET_RETRIEVAL_POINTERS needs a starting VCN of 0
    STARTING_VCN_INPUT_BUFFER startVcn{};
    startVcn.StartingVcn.QuadPart = 0;

    // Buffer sized for one extent (we only need the first one)
    struct {
        RETRIEVAL_POINTERS_BUFFER header;
        LARGE_INTEGER extraLcn; // room for at least one extent
    } rpBuf{};

    DWORD bytesReturned = 0;
    DeviceIoControl(
            hFile,
            FSCTL_GET_RETRIEVAL_POINTERS,
            &startVcn, sizeof(startVcn),
            &rpBuf, sizeof(rpBuf),
            &bytesReturned,
            nullptr
            );
    // ERROR_MORE_DATA is fine — we only need Extents[0]

    CloseHandle(hFile);

    if (rpBuf.header.ExtentCount < 1)
        return UINT64_MAX;

    // Lcn of the first extent = physical position on disk
    LONGLONG lcn = rpBuf.header.Extents[0].Lcn.QuadPart;
    return (lcn < 0) ? UINT64_MAX : static_cast<UINT64>(lcn);
}

// ---------------------------------------------------------------------------
// SortPlaylistByDiskOrder
// ---------------------------------------------------------------------------
// Sorts the playlist so files are visited in ascending physical disk offset
// order. This minimises HDD head seeks when navigating sequentially.
// On SSDs or non-NTFS volumes the ioctl returns UINT64_MAX for all files,
// so the sort is stable and the existing order (alphabetical) is preserved.
// ---------------------------------------------------------------------------
static void SortPlaylistByDiskOrder(std::vector<std::wstring> &playlist) {
    // Gather offsets once up front — one CreateFile per image, cheap
    std::vector<UINT64> offsets;
    offsets.reserve(playlist.size());
    for (const auto &p: playlist)
        offsets.push_back(GetDiskOffset(p));

    // Build an index array and sort that, then reorder both vectors together
    std::vector<size_t> idx(playlist.size());
    for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;

    std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) {
        return offsets[a] < offsets[b];
    });

    std::vector<std::wstring> sorted;
    sorted.reserve(playlist.size());
    for (size_t i: idx)
        sorted.push_back(std::move(playlist[i]));

    playlist = std::move(sorted);
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

        // Start IO worker with correct thread count for this drive type (HDD=1, SSD/NVMe=2)
        EnsureIoWorkerStarted(filePath.parent_path().wstring());

        // Sort by physical disk position to minimise HDD head seeks
        SortPlaylistByDiskOrder(g_app.playlist);

        auto it = std::ranges::find(g_app.playlist, filePath.wstring());
        if (it != g_app.playlist.end()) {
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

    // Start IO worker with correct thread count for this drive type (HDD=1, SSD/NVMe=2)
    EnsureIoWorkerStarted(filePath.parent_path().wstring());

    // Sort by physical disk position to minimise HDD head seeks
    SortPlaylistByDiskOrder(g_app.playlist);

    auto it = std::ranges::find(g_app.playlist, filePath.wstring());
    if (it != g_app.playlist.end()) {
        LoadImageIndex(hWnd, static_cast<int>(std::distance(g_app.playlist.begin(), it)));
    }
}
