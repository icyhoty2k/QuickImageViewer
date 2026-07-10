#include "FileHandler.h"
#include "../AppState.h"
#include "../Overlays/OverlayManager.h"
#include "RegistrySetup.h"
#include "Constants.h"
#include <commdlg.h>
#include <filesystem>
#include <ranges>
#include <shlwapi.h>
#include <vector>
#include "UI/UIManager.h"
#include "WorkerThread.h"
#include "DriveInfo.h"
#include "../SvgDecoder.h"
#include "../UI/HistoryListWnd.h"

namespace fs = std::filesystem;

void sortCurrentPlaylistInOrder();

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

// Sort by "Natural" filename order (same as Windows Explorer)
static void SortPlaylistAlphabetically(std::vector<std::wstring> &playlist, bool reverse) {
    std::ranges::sort(playlist, [reverse](const std::wstring &a, const std::wstring &b) {
        int cmp = StrCmpLogicalW(a.c_str(), b.c_str());
        return reverse ? (cmp > 0) : (cmp < 0);
    });
}

// Sort by File Date
static void SortPlaylistByDate(std::vector<std::wstring> &playlist, bool reverse) {
    std::ranges::sort(playlist, [reverse](const std::wstring &a, const std::wstring &b) {
        return reverse
                   ? (fs::last_write_time(a) < fs::last_write_time(b))
                   : (fs::last_write_time(a) > fs::last_write_time(b));
    });
}

// Sort by File Size
static void SortPlaylistBySize(std::vector<std::wstring> &playlist, bool reverse) {
    std::ranges::sort(playlist, [reverse](const std::wstring &a, const std::wstring &b) {
        return reverse
                   ? (fs::file_size(a) < fs::file_size(b))
                   : (fs::file_size(a) > fs::file_size(b));
    });
}

// Sort by Extension (Type)
static void SortPlaylistByType(std::vector<std::wstring> &playlist, bool reverse) {
    std::ranges::sort(playlist, [reverse](const std::wstring &a, const std::wstring &b) {
        auto extA = fs::path(a).extension();
        auto extB = fs::path(b).extension();
        return reverse ? (extA > extB) : (extA < extB);
    });
}

//=====================================end sorting ===========================

void OpenInitialImage(HWND hWnd) {
    app.isDialogVisible = true;

    // Helper for building the filter string
    std::vector<wchar_t> filterBuffer;
    auto AppendFilterString = [&](std::wstring_view text) {
        filterBuffer.insert(filterBuffer.end(), text.begin(), text.end());
        filterBuffer.push_back(L'\0');
    };

    // Build supported extensions
    std::wstring extensions;
    for (size_t i = 0; i < Constants::Registry::SUPPORTED_EXTENSIONS_COUNT; ++i) {
        extensions += L"*";
        extensions += Constants::Registry::SUPPORTED_EXTENSIONS[i];
        if (i + 1 < Constants::Registry::SUPPORTED_EXTENSIONS_COUNT)
            extensions += L";";
    }

    // Filter entries
    AppendFilterString(L"All Supported Images");
    AppendFilterString(extensions);
    AppendFilterString(L"All Files (*.*)");
    AppendFilterString(L"*.*");

    // End of filter list (double null)
    filterBuffer.push_back(L'\0');

    wchar_t fileName[MAX_PATH] = {};

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hWnd;
    ofn.lpstrFilter = filterBuffer.data();
    ofn.lpstrFile = fileName;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;

    wchar_t lastFolder[MAX_PATH] = {};
    System::LoadStringSetting(Constants::Registry::LAST_FOLDER,
                              lastFolder,
                              MAX_PATH);

    if (lastFolder[0] != L'\0')
        ofn.lpstrInitialDir = lastFolder;

    const BOOL result = GetOpenFileNameW(&ofn);

    app.isDialogVisible = false;

    if (!result) {
        if (app.playlist.empty())
            PostQuitMessage(0);
        return;
    }

    std::filesystem::path selectedPath;

    try {
        selectedPath = std::filesystem::canonical(fileName);
    } catch (...) {
        return;
    }

    System::SaveStringSetting(
            Constants::Registry::LAST_FOLDER,
            selectedPath.parent_path().wstring());

    app.playlist.clear();

    try {
        for (const auto &entry: std::filesystem::directory_iterator(selectedPath.parent_path())) {
            if (!entry.is_regular_file())
                continue;

            if (!is_image_ext(entry.path().extension().wstring()))
                continue;

            app.playlist.push_back(
                    std::filesystem::canonical(entry.path()).wstring());
        }
    } catch (...) {
        return;
    }

    EnsureIoWorkerStarted(selectedPath.parent_path().wstring());
    // sort
    sortCurrentPlaylistInOrder();

    // Rebuild the O(1) path → index lookup map
    app.playlistIndexMap.clear();
    app.playlistIndexMap.reserve(app.playlist.size());
    for (int i = 0; i < static_cast<int>(app.playlist.size()); ++i) {
        app.playlistIndexMap[app.playlist[i]] = i;
    }

    // Record this folder in the history panel
    UI::PushFolderHistory(selectedPath.parent_path().wstring());

    // New folder — drop stale dir window thumbnails before navigating
    uiManager.getDirWindow().ClearDirThumbnailCache();


    auto it = std::ranges::find(app.playlist, selectedPath.wstring());
    if (it != app.playlist.end()) {
        LoadImageIndex(hWnd, static_cast<int>(std::distance(app.playlist.begin(), it)));
        uiManager.getDirWindow().UpdateDirView();
    }
}

// Called from every code path where an image finishes loading (cache hit,
// WM_QIV_REPAINT, WM_QIV_SVG_READY). Keeps overlay content in sync.
void UpdateOverlaysForCurrentImage(HWND hWnd) {
    if (app.playlist.empty() || app.currentIndex < 0) return;
    const std::wstring &path = app.playlist[app.currentIndex];
    std::wstring fileName = path.substr(path.find_last_of(L"\\/") + 1);

    // Get file size on disk for the BOT_RIGHT overlay.
    int64_t fileSizeBytes = 0;
    {
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
            LARGE_INTEGER li;
            li.HighPart = static_cast<LONG>(fad.nFileSizeHigh);
            li.LowPart = fad.nFileSizeLow;
            fileSizeBytes = li.QuadPart;
        }
    }

    g_overlayManager.UpdateInfo(app.currentIndex,
                                static_cast<int>(app.playlist.size()),
                                fileName);
    g_overlayManager.UpdateDims(app.imgWidth, app.imgHeight, fileSizeBytes);
    g_overlayManager.UpdateZoom(app.viewport.zoom, hWnd);
    g_overlayManager.UpdateEffects();
}

void LoadImageIndex(HWND hWnd, int index) {
    g_decoderWorker.ClearQueue();
    g_ioWorker.ClearQueue();

    if (index < 0 || index >= static_cast<int>(app.playlist.size())) return;

    if (app.currentIndex != index) {
        app.viewport = ViewportState{};
    }

    app.currentIndex = index;
    app.wantedIndex.store(index, std::memory_order_release);
    uiManager.getDirWindow().SyncDirSelectionRectangle();

    const std::wstring &currentPath = app.playlist[index];
    SetWindowTextW(hWnd, (currentPath.substr(currentPath.find_last_of(L"\\/") + 1) + L" - QuickImageViewer").c_str());

    // =========================================================================
    // IMMEDIATE: Update overlay text INSTANTLY (filename, count, zoom, dims)
    // This happens synchronously, so fast scrolling shows text changes right away
    // =========================================================================
    UpdateOverlaysForCurrentImage(hWnd);
    InvalidateRect(hWnd, nullptr, FALSE);

    // -------------------------------------------------------------------------
    // SVG path: load bytes on IO thread, call LoadSvgFromBytes on UI thread
    // -------------------------------------------------------------------------
    if (SvgDecoder::IsSvgPath(currentPath)) {
        if (app.renderer) app.renderer->ClearActiveImage();

        g_ioWorker.PushTask([currentPath, index, hWnd]() {
            if (app.wantedIndex.load(std::memory_order_acquire) != index) return;

            std::vector<BYTE> svgBytes;
            if (FAILED(SvgDecoder::LoadFile(currentPath, svgBytes))) return;

            struct SvgPayload {
                std::wstring path;
                std::vector<BYTE> bytes;
            };

            auto *payload = new SvgPayload{currentPath, std::move(svgBytes)};

            PostMessageW(hWnd, Constants::WM_QIV_SVG_READY,
                         static_cast<WPARAM>(index),
                         reinterpret_cast<LPARAM>(payload));
        });

        SetTimer(hWnd, 1001, Constants::PRELOAD_TIMER_COUNTDOWN, nullptr);
        return;
    }

    // -------------------------------------------------------------------------
    // Raster path (now fully async)
    // -------------------------------------------------------------------------
    if (app.renderer) {
        if (SUCCEEDED(app.renderer->LoadBitmap(nullptr, 0, 0, currentPath))) {
            // Cache hit: the new bitmap is now active in the renderer.
            // Rewire the effect graph to the new bitmap so the display node
            // is not left pointing at the previous image's effect output.
            app.UpdateRendererColorEffects(hWnd);
            // Note: UpdateOverlaysForCurrentImage() was already called above
        } else {
            (void) app.renderer->PreloadBitmap(currentPath, index);
        }
    }

    SetTimer(hWnd, 1001, Constants::PRELOAD_TIMER_COUNTDOWN, nullptr);
}

void OpenSpecificImage(HWND hWnd, const std::wstring &filePathStr) {
    fs::path filePath(filePathStr);
    if (!fs::exists(filePath) || !fs::is_regular_file(filePath)) return;
    filePath = fs::canonical(filePath);

    if (!app.playlist.empty()) {
        if (filePath.parent_path() == fs::path(app.playlist[0]).parent_path()) {
            auto it = std::ranges::find(app.playlist, filePath.wstring());
            if (it != app.playlist.end()) {
                // Same folder — no need to rebuild playlist, but still record
                // the folder in history so revisiting via the history panel
                // bumps the entry to the top (PushFolderHistory deduplicates).
                UI::PushFolderHistory(filePath.parent_path().wstring());
                LoadImageIndex(hWnd, static_cast<int>(std::distance(app.playlist.begin(), it)));
                InvalidateRect(hWnd, nullptr, TRUE);
                UpdateWindow(hWnd);
                return;
            }
        }
    }

    app.playlist = fs::directory_iterator(filePath.parent_path())
                   | std::views::filter([](const auto &e) {
                       return e.is_regular_file() && is_image_ext(e.path().extension().wstring());
                   })
                   | std::views::transform([](const auto &e) {
                       return fs::canonical(e.path()).wstring();
                   })
                   | std::ranges::to<std::vector<std::wstring> >();

    // Start IO worker with correct thread count for this drive type (HDD=1, SSD/NVMe=2)
    EnsureIoWorkerStarted(filePath.parent_path().wstring());

    // Sort
    sortCurrentPlaylistInOrder();


    // Rebuild the O(1) path → index lookup map
    app.playlistIndexMap.clear();
    app.playlistIndexMap.reserve(app.playlist.size());
    for (int i = 0; i < static_cast<int>(app.playlist.size()); ++i) {
        app.playlistIndexMap[app.playlist[i]] = i;
    }

    // Record this folder in the history panel
    UI::PushFolderHistory(filePath.parent_path().wstring());

    // New folder — drop stale dir window thumbnails before navigating
    uiManager.getDirWindow().ClearDirThumbnailCache();
    uiManager.getDirWindow().UpdateDirView();

    auto it = std::ranges::find(app.playlist, filePath.wstring());
    if (it != app.playlist.end()) {
        LoadImageIndex(hWnd, static_cast<int>(std::distance(app.playlist.begin(), it)));
        uiManager.getDirWindow().UpdateDirView();
    }
}

void sortCurrentPlaylistInOrder() {
    switch (app.fileHandlerDefaultSortOrder) {
        case 0: {
            SortPlaylistAlphabetically(app.playlist, app.fileHandlerIsReverseSortOrder);
            break;
        }
        case 1: {
            SortPlaylistByDate(app.playlist, app.fileHandlerIsReverseSortOrder);
            break;
        }
        case 2: {
            SortPlaylistBySize(app.playlist, app.fileHandlerIsReverseSortOrder);
            break;
        }
        case 3: {
            SortPlaylistByType(app.playlist, app.fileHandlerIsReverseSortOrder);
            break;
        }
        case 4: {
            SortPlaylistByDiskOrder(app.playlist);
            break;
        }
    }
}
