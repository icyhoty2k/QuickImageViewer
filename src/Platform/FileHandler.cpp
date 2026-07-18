#include "FileHandler.h"
#include "../AppState.h"
#include "../Overlays/OverlayManager.h"
#include "../Persistence/RegistryManager.h"
#include "Constants.h"
#include "../ImageLoadStats.h"
#include <commdlg.h>
#include <shobjidl.h>
#include <filesystem>
#include <numeric>
#include <ranges>
#include <shlwapi.h>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "UI/UIManager.h"
#include "WorkerThread.h"
#include "DriveInfo.h"
#include "../SvgDecoder.h"
#include "../UI/FloatingPanels/HistoryListWnd.h"

namespace fs = std::filesystem;

void sortCurrentPlaylistInOrder();

// ---------------------------------------------------------------------------
// UpdateIoWorkerForPath
// ---------------------------------------------------------------------------
// Called on every folder/image open. Compares the new path's volume root
// against the last known one. Skips detection entirely when browsing within
// the same volume (the common case). When the volume changes, re-detects
// the drive type and restarts the IO worker only if the optimal thread count
// has actually changed (HDD↔NVMe). ClearQueue before Stop so the join is
// instant — in-flight tasks for the old folder are abandoned safely.
// ---------------------------------------------------------------------------
static std::wstring g_lastVolumeRoot;
static std::unordered_map<std::wstring, size_t> g_driveThreadCache; // volume root → optimal thread count

static void UpdateIoWorkerForPath(const std::wstring &folderPath) {
    wchar_t volRoot[MAX_PATH] = {};
    GetVolumePathNameW(folderPath.c_str(), volRoot, MAX_PATH);
    std::wstring newRoot(volRoot);

    // Look up cached result; detect only on first visit to each volume
    auto it = g_driveThreadCache.find(newRoot);
    if (it == g_driveThreadCache.end()) {
        size_t optimal = DriveInfo::GetOptimalIoThreadCount(folderPath);
        g_driveThreadCache[newRoot] = optimal;
        it = g_driveThreadCache.find(newRoot);
    }
    size_t optimal = it->second;

    if (!g_ioWorker.IsStarted()) {
        g_ioWorker.Start(optimal);
        g_lastVolumeRoot = newRoot;
        return;
    }

    if (newRoot == g_lastVolumeRoot) return; // same volume, nothing to do

    if (optimal != static_cast<size_t>(g_ioWorker.getThreadCount())) {
        g_ioWorker.ClearQueue();
        g_ioWorker.Stop();
        g_ioWorker.Start(optimal);
    }
    g_lastVolumeRoot = newRoot;
}

bool is_image_ext(const std::wstring &ext) {
    // O(1) lookup instead of a linear scan — this fires once per directory
    // entry on every folder open. Set is built lowercased on first use.
    static const std::unordered_set<std::wstring> extSet = [] {
        std::unordered_set<std::wstring> s;
        s.reserve(Constants::Registry::SUPPORTED_EXTENSIONS_COUNT);
        for (size_t i = 0; i < Constants::Registry::SUPPORTED_EXTENSIONS_COUNT; ++i) {
            std::wstring e = Constants::Registry::SUPPORTED_EXTENSIONS[i];
            for (auto &c: e) c = towlower(c);
            s.insert(std::move(e));
        }
        return s;
    }();

    std::wstring lo = ext;
    for (auto &c: lo) c = towlower(c);
    return extSet.contains(lo);
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

// ---------------------------------------------------------------------------
// SortPlaylistByKey
// ---------------------------------------------------------------------------
// Shared helper: computes one sort key per file up front (O(N) filesystem
// calls), then sorts an index array and reorders the playlist. Calling the
// filesystem inside the comparator would cost O(N log N) syscalls instead.
// ---------------------------------------------------------------------------
template<typename Key, typename GetKey>
static void SortPlaylistByKey(std::vector<std::wstring> &playlist, bool ascending, GetKey getKey) {
    std::vector<Key> keys;
    keys.reserve(playlist.size());
    for (const auto &p: playlist)
        keys.push_back(getKey(p));

    std::vector<size_t> idx(playlist.size());
    std::iota(idx.begin(), idx.end(), size_t{0});

    std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) {
        return ascending ? (keys[a] < keys[b]) : (keys[a] > keys[b]);
    });

    std::vector<std::wstring> sorted;
    sorted.reserve(playlist.size());
    for (size_t i: idx)
        sorted.push_back(std::move(playlist[i]));
    playlist = std::move(sorted);
}

// Sort by File Date — reads from playlistFileTimes (populated during scan, zero extra syscalls)
static void SortPlaylistByDate(std::vector<std::wstring> &playlist, bool reverse) {
    SortPlaylistByKey<fs::file_time_type>(playlist, reverse, [](const std::wstring &p) {
        auto it = app.playlistFileTimes.find(p);
        return (it != app.playlistFileTimes.end()) ? it->second : fs::file_time_type{};
    });
}

// Sort by File Size — reads from playlistFileSizes (populated during scan, zero extra syscalls)
static void SortPlaylistBySize(std::vector<std::wstring> &playlist, bool reverse) {
    SortPlaylistByKey<int64_t>(playlist, reverse, [](const std::wstring &p) {
        auto it = app.playlistFileSizes.find(p);
        return (it != app.playlistFileSizes.end()) ? it->second : int64_t{0};
    });
}

// Sort by Extension (Type)
static void SortPlaylistByType(std::vector<std::wstring> &playlist, bool reverse) {
    SortPlaylistByKey<std::wstring>(playlist, !reverse, [](const std::wstring &p) {
        return fs::path(p).extension().wstring();
    });
}

//=====================================end sorting ===========================

// ---------------------------------------------------------------------------
// Background scan support
// ---------------------------------------------------------------------------

// Incremented every time a new folder scan is launched. Background threads
// compare against this before posting results to discard stale scans.
static std::atomic<uint64_t> g_scanGeneration{0};

// True while a background directory scan is running. Read on UI thread to show wait cursor.
std::atomic<bool> g_scanInProgress{false};

// Sort a ScanResult's playlist using its own size/time maps (runs on background thread).
static void SortStandalonePlaylist(ScanResult &sr, int sortOrder, bool reverse) {
    switch (sortOrder) {
        case 0: SortPlaylistAlphabetically(sr.playlist, reverse);
            break;
        case 1:
            SortPlaylistByKey<fs::file_time_type>(sr.playlist, reverse, [&](const std::wstring &p) {
                auto it = sr.fileTimes.find(p);
                return it != sr.fileTimes.end() ? it->second : fs::file_time_type{};
            });
            break;
        case 2:
            SortPlaylistByKey<int64_t>(sr.playlist, reverse, [&](const std::wstring &p) {
                auto it = sr.fileSizes.find(p);
                return it != sr.fileSizes.end() ? it->second : int64_t{0};
            });
            break;
        case 3:
            SortPlaylistByKey<std::wstring>(sr.playlist, !reverse, [](const std::wstring &p) {
                return fs::path(p).extension().wstring();
            });
            break;
        case 4: SortPlaylistByDiskOrder(sr.playlist);
            break;
    }
}

// Spawns a detached background thread that scans dirPath and posts
// WM_QIV_SCAN_COMPLETE to hWnd when done. targetPath = file to navigate to
// after swap (empty = use index 0). gen must match g_scanGeneration on arrival
// or the result is discarded.
static void LaunchBackgroundScan(HWND hWnd, std::wstring dir,
                                 std::wstring targetPath, uint64_t gen,
                                 int sortOrder, bool reverse,
                                 bool updatePrimaryDirWnd = true) {
    g_scanInProgress.store(true, std::memory_order_relaxed);
    std::thread([hWnd, dir = std::move(dir), targetPath = std::move(targetPath),
                gen, sortOrder, reverse, updatePrimaryDirWnd]() {
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

                auto *result = new ScanResult();
                result->generation = gen;
                result->targetPath = targetPath;
                result->scannedDir = dir;
                result->updatePrimaryDirWnd = updatePrimaryDirWnd;

                std::error_code ec;
                for (const auto &entry: fs::directory_iterator(dir, ec)) {
                    if (g_scanGeneration.load(std::memory_order_relaxed) != gen) {
                        delete result;
                        return;
                    }
                    if (!entry.is_regular_file()) continue;
                    if (!is_image_ext(entry.path().extension().wstring())) continue;
                    std::wstring p = entry.path().wstring();
                    result->fileSizes[p] = static_cast<int64_t>(entry.file_size());
                    result->fileTimes[p] = entry.last_write_time(ec);
                    result->playlist.push_back(std::move(p));
                }

                if (g_scanGeneration.load(std::memory_order_relaxed) != gen) {
                    g_scanInProgress.store(false, std::memory_order_relaxed);
                    delete result;
                    return;
                }
                SortStandalonePlaylist(*result, sortOrder, reverse);

                if (g_scanGeneration.load(std::memory_order_relaxed) != gen) {
                    g_scanInProgress.store(false, std::memory_order_relaxed);
                    delete result;
                    return;
                }

                // Clear the flag BEFORE posting — UI thread may process the message
                // before this thread's next instruction, and the cursor must reset.
                g_scanInProgress.store(false, std::memory_order_release);
                PostMessageW(hWnd, Constants::WM_QIV_SCAN_COMPLETE, 0,
                             reinterpret_cast<LPARAM>(result));
            }).detach();
}

// Called on the UI thread from the WM_QIV_SCAN_COMPLETE handler.
void HandleScanComplete(HWND hWnd, ScanResult *result) {
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));

    if (result->generation != g_scanGeneration.load(std::memory_order_relaxed)) {
        delete result;
        return;
    }

    // Empty scan: folder has no images (or the directory itself was deleted).
    // Notify all visible panels so they can show the appropriate placeholder,
    // then clear the stale playlist and blank the main viewport regardless —
    // navigating with a stale playlist would try to open files that no longer exist.
    if (result->playlist.empty()) {
        std::wstring dir = result->scannedDir;

        // Prune only the files confirmed gone from disk — keeps images from other
        // dirs that happen to share the VRAM cache intact.
        if (app.renderer) {
            for (const auto &path: app.playlist) {
                std::error_code ec;
                if (!fs::exists(fs::path(path), ec) || ec)
                    app.renderer->RemoveFromCache(path);
            }
        }

        // Clear app state BEFORE notifying panels.
        // DirWnd::GetSourceItems() auto-populates from app.playlist when its own
        // playlist is empty — if we notify first, it immediately refills from the
        // still-live app.playlist and shows old thumbnails instead of the placeholder.
        app.playlist.clear();
        app.playlistFileSizes.clear();
        app.playlistFileTimes.clear();
        app.playlistIndexMap.clear();
        app.currentIndex = -1;
        app.previousImageIndex = -1;
        if (app.renderer) app.renderer->ClearActiveImage();

        // Now notify all visible panels — app state is zeroed so GetSourceItems()
        // correctly returns empty and every panel shows its empty placeholder.
        uiManager.NotifyFolderRefreshed(dir, {});

        // Hidden DirWnd is not in a slot and missed the notify — sync directly.
        {
            UI::ThumbnailPanelWnd &dirPanel = uiManager.getDirWindow();
            if (!dirPanel.IsPanelVisible())
                dirPanel.OnFolderRefreshed(dir, {});
        }

        // CacheWnd needs a second kick since its UpdateView guard checks app.playlist.
        uiManager.getCacheWindow().UpdateCacheView();

        std::error_code ec;
        if (!fs::is_directory(fs::path(dir), ec) || ec) {
            // Directory itself is gone (deleted, moved, renamed).
            UI::InvalidateHistoryFolderStatus(dir);
            app.folderOverlay = AppState::FolderOverlayState::Missing;
            app.folderOverlayPath = dir;
        } else {
            // Directory exists but contains no supported images.
            UI::NotifyFolderContentsChanged(dir);
            app.folderOverlay = AppState::FolderOverlayState::Empty;
            app.folderOverlayPath = dir;
        }

        InvalidateRect(hWnd, nullptr, FALSE);
        delete result;
        return;
    }

    // Prune VRAM cache entries only for files confirmed deleted from disk.
    // Files that exist on disk but aren't in the new playlist (different folder,
    // rename, etc.) are kept so CacheWnd shows a true cross-folder history.
    if (result->playlist != app.playlist && app.renderer) {
        std::unordered_set<std::wstring> newSet(result->playlist.begin(), result->playlist.end());
        for (const auto &path: app.playlist) {
            if (newSet.find(path) == newSet.end()) {
                std::error_code ec;
                if (!fs::exists(fs::path(path), ec) || ec)
                    app.renderer->RemoveFromCache(path);
            }
        }
    }

    app.playlist = std::move(result->playlist);
    app.playlistFileSizes = std::move(result->fileSizes);
    app.playlistFileTimes = std::move(result->fileTimes);

    app.playlistIndexMap.clear();
    app.playlistIndexMap.reserve(app.playlist.size());
    for (int i = 0; i < static_cast<int>(app.playlist.size()); ++i)
        app.playlistIndexMap[app.playlist[i]] = i;

    int targetIdx = 0;
    if (!result->targetPath.empty()) {
        auto it = app.playlistIndexMap.find(result->targetPath);
        if (it != app.playlistIndexMap.end())
            targetIdx = it->second;
    }

    std::wstring scannedDir = result->scannedDir;
    const bool updatePrimaryDir = result->updatePrimaryDirWnd;
    delete result;

    // Refresh every visible panel that cares about this folder — DirWnd,
    // SpawnedDirWnd instances watching the same dir, and CacheWnd.
    // Pass updatePrimaryDir = false when the scan was triggered by a SpawnedDirWnd
    // click so F6 DirWnd keeps its own folder's thumbnails.
    uiManager.NotifyFolderRefreshed(scannedDir, app.playlist, updatePrimaryDir);

    // A hidden DirWnd is not in any layout slot and missed the notify above.
    // Only sync it when the scan belongs to the primary context; a SpawnedDirWnd
    // click must not overwrite F6's folder while it's hidden.
    UI::DirWnd &dirWnd = uiManager.getDirWindow();
    if (updatePrimaryDir && !dirWnd.IsPanelVisible())
        dirWnd.SetPlaylistCopy(app.playlist);

    // Folder has images — dismiss any Missing/Empty renderer overlay.
    app.folderOverlay = AppState::FolderOverlayState::None;
    app.folderOverlayPath.clear();
    UI::NotifyFolderContentsChanged(scannedDir);

    LoadImageIndex(hWnd, targetIdx);
    InvalidateRect(hWnd, nullptr, FALSE);
}

void OpenInitialImage(HWND hWnd) {
    app.isDialogVisible = true;

    // Event sink: implements IFileDialogEvents + IFileDialogControlEvents so we
    // can receive the "Open Folder" custom button click. Stack-allocated — the
    // dialog holds no reference past pfd->Show(), so lifetime is safe.
    struct DlgEvents : IFileDialogEvents, IFileDialogControlEvents {
        IFileDialog *dlg = nullptr;
        std::wstring folderPath; // set by OnButtonClicked when "Open Folder" is clicked

        ULONG   STDMETHODCALLTYPE AddRef()  override { return 2; }
        ULONG   STDMETHODCALLTYPE Release() override { return 1; }
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
            if (riid == IID_IUnknown || riid == IID_IFileDialogEvents)
                { *ppv = static_cast<IFileDialogEvents *>(this); return S_OK; }
            if (riid == IID_IFileDialogControlEvents)
                { *ppv = static_cast<IFileDialogControlEvents *>(this); return S_OK; }
            *ppv = nullptr; return E_NOINTERFACE;
        }
        // IFileDialogEvents — all no-ops
        HRESULT STDMETHODCALLTYPE OnFileOk(IFileDialog *)                                                      override { return S_OK; }
        HRESULT STDMETHODCALLTYPE OnFolderChanging(IFileDialog *, IShellItem *)                                override { return S_OK; }
        HRESULT STDMETHODCALLTYPE OnFolderChange(IFileDialog *)                                                override { return S_OK; }
        HRESULT STDMETHODCALLTYPE OnSelectionChange(IFileDialog *)                                             override { return S_OK; }
        HRESULT STDMETHODCALLTYPE OnShareViolation(IFileDialog *, IShellItem *, FDE_SHAREVIOLATION_RESPONSE *) override { return S_OK; }
        HRESULT STDMETHODCALLTYPE OnTypeChange(IFileDialog *)                                                  override { return S_OK; }
        HRESULT STDMETHODCALLTYPE OnOverwrite(IFileDialog *, IShellItem *, FDE_OVERWRITE_RESPONSE *)           override { return S_OK; }
        // IFileDialogControlEvents
        HRESULT STDMETHODCALLTYPE OnItemSelected(IFileDialogCustomize *, DWORD, DWORD) override { return S_OK; }
        HRESULT STDMETHODCALLTYPE OnCheckButtonToggled(IFileDialogCustomize *, DWORD, BOOL) override { return S_OK; }
        HRESULT STDMETHODCALLTYPE OnControlActivating(IFileDialogCustomize *, DWORD) override { return S_OK; }
        HRESULT STDMETHODCALLTYPE OnButtonClicked(IFileDialogCustomize *, DWORD id) override {
            if (id == 1 && dlg) {
                IShellItem *psi = nullptr;
                if (SUCCEEDED(dlg->GetFolder(&psi))) {
                    PWSTR path = nullptr;
                    if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                        folderPath = path;
                        CoTaskMemFree(path);
                    }
                    psi->Release();
                }
                dlg->Close(S_OK);
            }
            return S_OK;
        }
    };

    IFileOpenDialog *pfd = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd)))) {
        app.isDialogVisible = false;
        if (app.playlist.empty()) PostQuitMessage(0);
        return;
    }

    // File type filters
    std::wstring extList;
    for (size_t i = 0; i < Constants::Registry::SUPPORTED_EXTENSIONS_COUNT; ++i) {
        extList += L"*";
        extList += Constants::Registry::SUPPORTED_EXTENSIONS[i];
        if (i + 1 < Constants::Registry::SUPPORTED_EXTENSIONS_COUNT)
            extList += L";";
    }
    COMDLG_FILTERSPEC filters[] = {
        { L"All Supported Images", extList.c_str() },
        { L"All Files",            L"*.*"           }
    };
    pfd->SetFileTypes(ARRAYSIZE(filters), filters);
    pfd->SetFileTypeIndex(1);

    // Restore last-used folder
    std::wstring lastFolder = Persistence::Registry::LoadStringSetting(Constants::Registry::LAST_FOLDER);
    if (!lastFolder.empty()) {
        IShellItem *psi = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(lastFolder.c_str(), nullptr, IID_PPV_ARGS(&psi)))) {
            pfd->SetFolder(psi);
            psi->Release();
        }
    }

    // Add "Open Folder" button and register event sink
    DlgEvents events;
    events.dlg = pfd;
    DWORD cookie = 0;
    IFileDialogCustomize *pfdc = nullptr;
    if (SUCCEEDED(pfd->QueryInterface(IID_PPV_ARGS(&pfdc)))) {
        pfdc->AddPushButton(1, L"Open Folder");
        pfd->Advise(&events, &cookie);
        pfdc->Release();
    }

    HRESULT hr = pfd->Show(hWnd);

    if (cookie) pfd->Unadvise(cookie);
    app.isDialogVisible = false;

    // "Open Folder" button was clicked — open the directory shown in the dialog
    if (!events.folderPath.empty()) {
        pfd->Release();
        OpenDirectory(hWnd, events.folderPath);
        return;
    }

    if (FAILED(hr)) {
        pfd->Release();
        if (app.playlist.empty()) PostQuitMessage(0);
        return;
    }

    // Normal file selection
    std::wstring filePath;
    {
        IShellItem *psi = nullptr;
        if (SUCCEEDED(pfd->GetResult(&psi))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                filePath = path;
                CoTaskMemFree(path);
            }
            psi->Release();
        }
    }
    pfd->Release();

    if (filePath.empty()) {
        if (app.playlist.empty()) PostQuitMessage(0);
        return;
    }

    fs::path selectedPath;
    try {
        selectedPath = fs::canonical(filePath);
    } catch (...) {
        return;
    }

    // User typed a folder path in the filename field
    if (fs::is_directory(selectedPath)) {
        OpenDirectory(hWnd, selectedPath.wstring());
        return;
    }

    Persistence::Registry::SaveStringSetting(
            Constants::Registry::LAST_FOLDER,
            selectedPath.parent_path().wstring());

    // Immediate: 1-file playlist so the selected image loads right now.
    uint64_t gen = ++g_scanGeneration;
    std::wstring target = selectedPath.wstring();
    {
        std::error_code ec;
        app.playlist = {target};
        app.playlistFileSizes = {{target, static_cast<int64_t>(fs::file_size(selectedPath, ec))}};
        app.playlistFileTimes = {{target, fs::last_write_time(selectedPath, ec)}};
        app.playlistIndexMap = {{target, 0}};
    }
    app.previousImageIndex = -1;

    UpdateIoWorkerForPath(selectedPath.parent_path().wstring());
    UI::PushFolderHistory(selectedPath.parent_path().wstring());
    uiManager.getActiveDirWnd().ClearDirThumbnailCache();
    uiManager.getDirWindow().SetPlaylistCopy(app.playlist); // F2 always targets F5

    LoadImageIndex(hWnd, 0);
    app.previousImageIndex = -1; // don't allow E-toggle into the old playlist

    // Background: full directory scan + sort.
    LaunchBackgroundScan(hWnd, selectedPath.parent_path().wstring(), target, gen,
                         app.fileHandlerDefaultSortOrder, app.fileHandlerIsReverseSortOrder);
}

// Called from every code path where an image finishes loading (cache hit,
// WM_QIV_REPAINT, WM_QIV_SVG_READY). Keeps overlay content in sync.
void UpdateOverlaysForCurrentImage(HWND hWnd) {
    if (app.playlist.empty() || app.currentIndex < 0) return;
    const std::wstring &path = app.playlist[app.currentIndex];
    std::wstring fileName = path.substr(path.find_last_of(L"\\/") + 1);

    // Get file size from scan-time cache — no extra syscall per navigation.
    int64_t fileSizeBytes = 0;
    {
        auto sIt = app.playlistFileSizes.find(path);
        if (sIt != app.playlistFileSizes.end())
            fileSizeBytes = sIt->second;
    }

    g_overlayManager.UpdateInfo(app.currentIndex,
                                static_cast<int>(app.playlist.size()),
                                fileName);
    g_overlayManager.UpdateDims(app.imgWidth, app.imgHeight, fileSizeBytes);
    g_overlayManager.UpdateZoom(app.viewport.zoom, hWnd);
    g_overlayManager.UpdateEffects();
}

void ApplyOrientationToViewport(USHORT orient) {
    switch (orient) {
        case 2: app.viewport.flippedH = true;
            break;
        case 3: app.viewport.rotation = 180;
            break;
        case 4: app.viewport.flippedV = true;
            break;
        case 5: app.viewport.rotation = 90;
            app.viewport.flippedH = true;
            break;
        case 6: app.viewport.rotation = 90;
            break;
        case 7: app.viewport.rotation = 270;
            app.viewport.flippedH = true;
            break;
        case 8: app.viewport.rotation = 270;
            break;
        default: break; // 1 = normal
    }
}

void LoadImageIndex(HWND hWnd, int index) {
    if (index < 0 || index >= static_cast<int>(app.playlist.size())) return;

    if (app.currentIndex != index) {
        app.viewport = ViewportState{};
        if (app.currentIndex >= 0)
            app.previousImageIndex = app.currentIndex;
    }

    app.currentIndex = index;
    app.wantedIndex.store(index, std::memory_order_release);

    // Kill any running GIF animation from the previous image.
    KillTimer(hWnd, Constants::Slideshow::GIF_TIMER_ID);

    const std::wstring &currentPath = app.playlist[index];
    SetWindowTextW(hWnd, (currentPath.substr(currentPath.find_last_of(L"\\/") + 1) + L" - QuickImageViewer").c_str());

    // =========================================================================
    // IMMEDIATE: Update overlay text INSTANTLY (filename, count, zoom, dims)
    // UpdateWindow forces a synchronous render so text is never blocked by
    // WM_PAINT being starved behind a flood of WM_MOUSEWHEEL messages.
    // The main renderer uses Present(0,0) so this costs only ~1-2ms.
    // =========================================================================
    UpdateOverlaysForCurrentImage(hWnd);
    InvalidateRect(hWnd, nullptr, FALSE);
    UpdateWindow(hWnd);

    // Scroll the dir panel to track the selection (synchronous via UpdateWindow
    // inside SyncSelectionRectangle; dirWnd also uses Present(0,0) so it's fast).
    uiManager.getActiveDirWnd().SyncDirSelectionRectangle();

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
        // Start timing before cache check — measures wall-clock time the user waits
        // regardless of whether the image comes from VRAM cache or needs a full decode.
        ImageLoadStats::g_loadStartUs.store(ImageLoadStats::NowUs(), std::memory_order_relaxed);

        if (SUCCEEDED(app.renderer->LoadBitmap(nullptr, 0, 0, currentPath))) {
            // Cache hit — record immediately; typically < 1 ms.
            ImageLoadStats::g_lastLoadUs.store(
                    ImageLoadStats::NowUs() -
                    ImageLoadStats::g_loadStartUs.load(std::memory_order_relaxed),
                    std::memory_order_relaxed);
            // Orientation was read during the original decode and stored in the
            // cache entry — apply it now that the viewport has been reset.
            ApplyOrientationToViewport(app.renderer->GetCachedOrientation(currentPath));
            // Rewire the effect graph to the new bitmap so the display node
            // is not left pointing at the previous image's effect output.
            app.UpdateRendererColorEffects(hWnd);
            // Paint the new image — the UpdateWindow earlier drew the old bitmap.
            InvalidateRect(hWnd, nullptr, FALSE);
            uiManager.RefreshInfoWindowIfVisible();
            uiManager.RefreshStatsWindowIfVisible();
            // Sync the dir panel selection only on actual image load, not on every
            // keypress — async loads are handled by WM_QIV_REPAINT + the callback.
            uiManager.getActiveDirWnd().SyncDirSelectionRectangle();
            // Arm GIF timer if this cached image is animated.
            if (app.renderer->IsAnimatedGif())
                SetTimer(hWnd, Constants::Slideshow::GIF_TIMER_ID,
                         app.renderer->GetCurrentGifDelay(), nullptr);
        } else {
            // Cache miss — decoder lambda records the time when decode completes.
            (void) app.renderer->PreloadBitmap(currentPath, index);
        }
    }

    SetTimer(hWnd, 1001, Constants::PRELOAD_TIMER_COUNTDOWN, nullptr);
}

void OpenDirectory(HWND hWnd, const std::wstring &dirPathStr) {
    // canonical(ec) resolves symlinks and fails if path doesn't exist — one call
    // instead of exists + is_directory + canonical. Then one is_directory check.
    std::error_code ec;
    fs::path dirPath = fs::canonical(fs::path(dirPathStr), ec);
    if (ec) return;
    if (!fs::is_directory(dirPath, ec) || ec) return;

    // Valid directory confirmed — dismiss any Missing/Empty overlay immediately.
    app.folderOverlay = AppState::FolderOverlayState::None;
    app.folderOverlayPath.clear();

    // If we are already in this directory, just jump to the first image.
    // Compare canonical paths to handle junctions and drive-letter case.
    if (!app.playlist.empty()) {
        fs::path curParent = fs::canonical(fs::path(app.playlist[0]).parent_path(), ec);
        if (!ec && dirPath == curParent) {
            UI::PushFolderHistory(dirPath.wstring());
            LoadImageIndex(hWnd, 0);
            InvalidateRect(hWnd, nullptr, TRUE);
            UpdateWindow(hWnd);
            return;
        }
    }

    // Quick synchronous probe: grab the first image found in disk order.
    // entry.file_size() / entry.last_write_time() come free from WIN32_FIND_DATA —
    // no extra syscalls beyond reading the directory itself.
    std::wstring firstFile;
    int64_t firstSize = 0;
    fs::file_time_type firstTime;
    {
        ec.clear();
        for (auto it = fs::directory_iterator(
                     dirPath, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::directory_iterator(); it.increment(ec)) {
            if (!it->is_regular_file(ec)) {
                ec.clear();
                continue;
            }
            if (!is_image_ext(it->path().extension().wstring())) continue;
            firstFile = it->path().wstring();
            firstSize = static_cast<int64_t>(it->file_size(ec));
            ec.clear();
            firstTime = it->last_write_time(ec);
            ec.clear();
            break;
        }
    }
    if (firstFile.empty()) {
        // Empty directory — still navigate to it so history records it, the panel
        // shows the empty-dir placeholder, and F5 can recover when images appear.
        UI::PushFolderHistory(dirPath.wstring());
        UpdateIoWorkerForPath(dirPath.wstring());
        uiManager.getActiveDirWnd().ClearDirThumbnailCache();
        const bool activeIsPrimary = (&uiManager.getActiveDirWnd() == &uiManager.getDirWindow());
        uint64_t emptyGen = ++g_scanGeneration;
        LaunchBackgroundScan(hWnd, dirPath.wstring(), L"", emptyGen,
                             app.fileHandlerDefaultSortOrder, app.fileHandlerIsReverseSortOrder,
                             activeIsPrimary);
        return;
    }

    uint64_t gen = ++g_scanGeneration;
    {
        app.playlist = {firstFile};
        app.playlistFileSizes = {{firstFile, firstSize}};
        app.playlistFileTimes = {{firstFile, firstTime}};
        app.playlistIndexMap = {{firstFile, 0}};
    }
    app.previousImageIndex = -1;

    UpdateIoWorkerForPath(dirPath.wstring());
    UI::PushFolderHistory(dirPath.wstring());
    uiManager.getActiveDirWnd().ClearDirThumbnailCache();
    const bool activeIsPrimary = (&uiManager.getActiveDirWnd() == &uiManager.getDirWindow());
    if (activeIsPrimary)
        uiManager.getDirWindow().SetPlaylistCopy(app.playlist);

    LoadImageIndex(hWnd, 0);
    app.previousImageIndex = -1;

    // Background: full scan + sort. targetPath empty → navigate to index 0 after sort.
    LaunchBackgroundScan(hWnd, dirPath.wstring(), L"", gen,
                         app.fileHandlerDefaultSortOrder, app.fileHandlerIsReverseSortOrder,
                         activeIsPrimary);
}

// Used to reload / refresh current dir with F5
void ReloadCurrentDirectory(HWND hWnd) {
    std::wstring currentImage;
    std::wstring dir;

    if (app.currentIndex >= 0 &&
        app.currentIndex < static_cast<int>(app.playlist.size())) {
        currentImage = app.playlist[app.currentIndex];
        dir = fs::path(currentImage).parent_path().wstring();
    } else if (app.folderOverlay != AppState::FolderOverlayState::None &&
               !app.folderOverlayPath.empty()) {
        // Playlist was cleared because the folder went missing/empty — rescan
        // that folder so the viewer recovers when images (re)appear in it.
        dir = app.folderOverlayPath;
    } else {
        return;
    }

    uint64_t gen = ++g_scanGeneration;
    UpdateIoWorkerForPath(dir);

    const bool activeIsPrimary = (&uiManager.getActiveDirWnd() == &uiManager.getDirWindow());
    LaunchBackgroundScan(
            hWnd,
            dir,
            currentImage,
            gen,
            app.fileHandlerDefaultSortOrder,
            app.fileHandlerIsReverseSortOrder,
            activeIsPrimary);
}


void OpenSpecificImage(HWND hWnd, const std::wstring &filePathStr) {
    fs::path filePath(filePathStr);
    if (!fs::exists(filePath) || !fs::is_regular_file(filePath)) return;
    filePath = fs::canonical(filePath);

    if (!app.playlist.empty()) {
        if (filePath.parent_path() == fs::path(app.playlist[0]).parent_path()) {
            auto mapIt = app.playlistIndexMap.find(filePath.wstring());
            if (mapIt != app.playlistIndexMap.end()) {
                // Same folder — no need to rebuild playlist, but still record
                // the folder in history so revisiting via the history panel
                // bumps the entry to the top (PushFolderHistory deduplicates).
                UI::PushFolderHistory(filePath.parent_path().wstring());
                LoadImageIndex(hWnd, mapIt->second);
                InvalidateRect(hWnd, nullptr, TRUE);
                UpdateWindow(hWnd);
                return;
            }
        }
    }

    // Immediate: 1-file playlist so the target image loads right now.
    uint64_t gen = ++g_scanGeneration;
    std::wstring target = filePath.wstring();
    {
        std::error_code ec;
        app.playlist = {target};
        app.playlistFileSizes = {{target, static_cast<int64_t>(fs::file_size(filePath, ec))}};
        app.playlistFileTimes = {{target, fs::last_write_time(filePath, ec)}};
        app.playlistIndexMap = {{target, 0}};
    }
    app.previousImageIndex = -1;

    UpdateIoWorkerForPath(filePath.parent_path().wstring());
    UI::PushFolderHistory(filePath.parent_path().wstring());

    // If the active dir panel is already displaying the clicked file's folder
    // (e.g. clicking back into the F6 DirWnd after viewing an image from a
    // spawned panel), keep its thumbnails and full item list intact — wiping
    // the cache and collapsing the copy to a 1-file playlist causes a visible
    // flicker while the background scan rebuilds what the panel already shows.
    bool panelShowsDir = false;
    {
        const std::wstring panelFolder = uiManager.getActiveDirWnd().GetPanelFolder();
        if (!panelFolder.empty()) {
            std::error_code ec;
            panelShowsDir = fs::equivalent(fs::path(panelFolder), filePath.parent_path(), ec) && !ec;
        }
    }

    if (!panelShowsDir) {
        uiManager.getActiveDirWnd().ClearDirThumbnailCache();
        if (&uiManager.getActiveDirWnd() == &uiManager.getDirWindow())
            uiManager.getDirWindow().SetPlaylistCopy(app.playlist);
    }

    LoadImageIndex(hWnd, 0);
    app.previousImageIndex = -1;
    if (!panelShowsDir)
        uiManager.getActiveDirWnd().UpdateDirView();

    // Background: full directory scan + sort.
    // If the click came from a SpawnedDirWnd, keep F6 DirWnd showing its own
    // folder — pass false so HandleScanComplete skips the primary DirWnd update.
    const bool activeIsPrimary = (&uiManager.getActiveDirWnd() == &uiManager.getDirWindow());
    LaunchBackgroundScan(hWnd, filePath.parent_path().wstring(), target, gen,
                         app.fileHandlerDefaultSortOrder, app.fileHandlerIsReverseSortOrder,
                         activeIsPrimary);
}

void ReSortPlaylistAndRebuildMap(HWND hWnd) {
    if (app.playlist.empty()) return;

    // Remember which file is currently displayed so we can restore it.
    const std::wstring currentPath = (app.currentIndex >= 0 &&
                                      app.currentIndex < static_cast<int>(app.playlist.size()))
                                         ? app.playlist[app.currentIndex]
                                         : std::wstring{};

    sortCurrentPlaylistInOrder();

    // Rebuild O(1) lookup map.
    app.playlistIndexMap.clear();
    app.playlistIndexMap.reserve(app.playlist.size());
    for (int i = 0; i < static_cast<int>(app.playlist.size()); ++i)
        app.playlistIndexMap[app.playlist[i]] = i;

    // Restore position so the same image stays current.
    if (!currentPath.empty()) {
        auto it = app.playlistIndexMap.find(currentPath);
        if (it != app.playlistIndexMap.end())
            app.currentIndex = it->second;
    }

    // Sync dir panel selection to the new index without reloading the image.
    uiManager.getActiveDirWnd().SyncDirSelectionRectangle();
    uiManager.getActiveDirWnd().UpdateDirView();
    uiManager.getCacheWindow().UpdateCacheView();
    InvalidateRect(hWnd, nullptr, FALSE);
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
