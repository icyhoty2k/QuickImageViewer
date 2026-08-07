// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#include "FileHandler.h"
#include "../AppState.h"
#include "../Overlays/OverlayManager.h"
#include "../Persistence/RegistryManager.h"
#include "../Persistence/SessionFile.h"   // qivSession.ini — the resume position
#include "Constants.h"
#include "ConstantsStrings.h" // the placeholder's headings — the title reuses them
#include "../ImageLoadStats.h"
#include <commdlg.h>
#include <shobjidl.h>
#include <filesystem>
#include <numeric>
#include "Platform/CrashHandler.h" // NoteImage / NoteCommand breadcrumbs
#include <ranges>
#include <shlwapi.h>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "UI/UIManager.h"
#include "WorkerThread.h"
#include "../SvgDecoder.h"
#include "../UI/FloatingPanels/HistoryListWnd.h"
// LoadImageIndex is the ONE place every picture change passes through — see the
// mirror/observe block inside it.
#include "Rem_TCP_IP/RemoteInbound.h" // InboundActive / ForwardInFlight
#include "Rem_TCP_IP/RemoteMirror.h"  // forward the new position to driven targets
#include "Rem_TCP_IP/RemoteServer.h"  // …and echo it to observers

namespace fs = std::filesystem;

void sortCurrentPlaylistInOrder();
void UpdateOverlaysForCurrentImage(HWND hWnd); // defined below; used by HandleScanComplete
void UpdateOverlaysForCurrentImage(HWND hWnd);
// ---------------------------------------------------------------------------
// UpdateIoWorkerForPath
// ---------------------------------------------------------------------------
// Starts the IO pool on first use, at Constants::IO_WORKER_THREADS. Nothing
// else: the pool is never resized and the drive is never inspected.
//
// It used to probe the physical device for a seek penalty and pick 1 thread for
// an HDD and 2 for an SSD, caching the answer per volume and restarting the pool
// when the volume changed. All of that is gone — see IO_WORKER_THREADS in
// Constants.h for the reasoning. In short: these threads only read, reading is
// 20-50x cheaper than the decode that follows, and the probe spun up sleeping
// disks to choose between one thread and two.
//
// The volume root is still tracked, because it costs one string compare and
// makes the "nothing to do" case obvious to a reader.
// ---------------------------------------------------------------------------
static std::wstring g_lastVolumeRoot;

static void UpdateIoWorkerForPath(const std::wstring &folderPath) {
    wchar_t volRoot[MAX_PATH] = {};
    GetVolumePathNameW(folderPath.c_str(), volRoot, MAX_PATH);
    const std::wstring newRoot(volRoot);

    if (!g_ioWorker.IsStarted())
        g_ioWorker.Start(Constants::IO_WORKER_THREADS);

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

// Adaptive scan-size estimate — the last scan's image count, used to pre-size
// the next scan's containers. Atomic: written by worker/UI scan threads, read
// at the next scan start. Relaxed is fine — it is a sizing hint, not a guard.
static std::atomic<size_t> g_lastDirScanCount{Constants::FileHandler::DIR_SCAN_RESERVE_FLOOR};

size_t DirScanReserveHint() {
    size_t n = g_lastDirScanCount.load(std::memory_order_relaxed);
    if (n < Constants::FileHandler::DIR_SCAN_RESERVE_FLOOR) n = Constants::FileHandler::DIR_SCAN_RESERVE_FLOOR;
    if (n > Constants::FileHandler::DIR_SCAN_RESERVE_CAP)   n = Constants::FileHandler::DIR_SCAN_RESERVE_CAP;
    return n;
}

void RecordDirScanCount(size_t n) {
    g_lastDirScanCount.store(n, std::memory_order_relaxed);
}

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

// ---------------------------------------------------------------------------
// SortScanResultInAppOrder
// ---------------------------------------------------------------------------
// Applies the app's CURRENT sort order + reverse flag to a list the caller
// built itself, keyed off the caller's own size/time maps — app.playlistFileSizes
// and app.playlistFileTimes describe the folder the viewer is showing, which is
// not the caller's folder.
//
// A PANEL THAT BUILDS ITS OWN FILE LIST MUST CALL THIS. SpawnedDirWnd used a
// plain std::sort, which is ordinal ("img10" before "img2") while every list the
// scan produces is natural-ordered by StrCmpLogicalW — and completely different
// again when the user sorts by date, size or type. The first click in such a
// panel is not in app.playlistIndexMap, so it goes through OpenSpecificImage,
// which rescans the folder; the scan comes back app-ordered, the panel adopts it
// in OnFolderRefreshed, and every thumbnail moves to a different slot. The
// selection sync then scrolls to wherever the clicked file ended up, which reads
// as the strip scrolling on its own.
// ---------------------------------------------------------------------------
void SortScanResultInAppOrder(ScanResult &sr) {
    SortStandalonePlaylist(sr, app.fileHandlerDefaultSortOrder,
                           app.fileHandlerIsReverseSortOrder);
}

// ---------------------------------------------------------------------------
// SortPathsInAppOrder
// ---------------------------------------------------------------------------
// Same ordering rule for a caller that holds only paths. Date and size are the
// only two orders that need a key the path itself does not carry, so the stat
// loop runs for those two and is skipped entirely for name, type and disk
// order. One stat per file is acceptable here because every caller is a user
// action — the sort order changed, or a panel rescanned its folder — never a
// frame path.
// ---------------------------------------------------------------------------
void SortPathsInAppOrder(std::vector<std::wstring> &paths) {
    if (paths.size() < 2) return;

    ScanResult sr;
    sr.playlist = std::move(paths);

    const int order = app.fileHandlerDefaultSortOrder;
    if (order == 1 || order == 2) {
        for (const auto &p: sr.playlist) {
            std::error_code ec;
            if (order == 1) {
                const auto t = fs::last_write_time(fs::path(p), ec);
                sr.fileTimes[p] = ec ? fs::file_time_type{} : t;
            } else {
                const auto s = fs::file_size(fs::path(p), ec);
                sr.fileSizes[p] = ec ? int64_t{0} : static_cast<int64_t>(s);
            }
        }
    }

    SortScanResultInAppOrder(sr);
    paths = std::move(sr.playlist);
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

                // Calculated speculation: pre-size all three lockstep containers
                // to the previous scan's image count (one allocation covers the
                // common folder; they still grow for larger ones).
                const size_t reserveHint = DirScanReserveHint();
                result->playlist.reserve(reserveHint);
                result->fileSizes.reserve(reserveHint);
                result->fileTimes.reserve(reserveHint);

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

                // Feed the actual count back into the adaptive estimate.
                RecordDirScanCount(result->playlist.size());

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

// THE one place the main window's title is written.
//
// It used to be written in exactly one place too — but that place was inside
// the image-load path, so it only ever ran when an image loaded. Every route
// that ends with NO image left the previous title standing, and the taskbar
// went on naming a picture that was no longer on screen: an empty folder, a
// folder deleted underneath us, a file nothing could decode, and a startup that
// found nothing at all. Four ways to end up lying about what is displayed.
//
// So the title is derived from the STATE rather than written by whoever last
// changed it, and every one of those routes calls this.
//
// The wording is the placeholder's own — the window says the same thing the
// screen says, in the same words, because they answer the same question.
void UpdateWindowTitle(HWND hWnd) {
    namespace M = Constants::Messages;

    std::wstring title;

    // The PLACEHOLDER STATE is asked first, not the playlist.
    //
    // Unsupported is the reason for the order: that file IS in the playlist and
    // IS the current index — it just cannot be drawn. Asking the playlist first
    // would name it as though it were on screen, which is the one thing the
    // window must not claim while the viewport says "Format not supported".
    const wchar_t *reason = nullptr;
    switch (app.folderOverlay) {
        case AppState::FolderOverlayState::Missing:
            reason = M::EMPTY_DIR_MISSING;
            break;
        case AppState::FolderOverlayState::Unsupported:
            reason = M::FORMAT_UNSUPPORTED;
            break;
        case AppState::FolderOverlayState::Empty:
            reason = M::EMPTY_DIR_NO_IMAGES;
            break;
        default:
            break; // nothing is being reported — fall through to the image
    }

    if (reason) {
        title = reason;
        // Same trim as the placeholder: these constants end in ':' because
        // elsewhere they introduce the line under them. Here something else
        // follows, separated its own way.
        while (!title.empty() && (title.back() == L':' || title.back() == L' '))
            title.pop_back();

        // The folder for the two folder states, the FILE for Unsupported —
        // folderOverlayPath already holds whichever one applies, so this needs
        // no branch of its own. The leaf only: a taskbar button is a few
        // characters wide and a full path would be truncated away to nothing.
        const std::wstring &subject = app.folderOverlayPath;
        if (!subject.empty()) {
            std::wstring leaf = subject.substr(subject.find_last_of(L"\\/") + 1);
            if (leaf.empty()) leaf = subject; // a drive root — "D:\" has no leaf
            title += L" - ";
            title += leaf;
        }
    } else if (!app.playlist.empty() &&
               app.currentIndex >= 0 &&
               app.currentIndex < static_cast<int>(app.playlist.size())) {
        // An image is on screen: it names itself, as it always has.
        const std::wstring &path = app.playlist[app.currentIndex];
        title = path.substr(path.find_last_of(L"\\/") + 1);
    }

    if (title.empty()) {
        SetWindowTextW(hWnd, Constants::APP_NAME);
        return;
    }

    title += L" - ";
    title += Constants::APP_NAME;
    SetWindowTextW(hWnd, title.c_str());
}

void SetFolderOverlay(HWND hWnd, AppState::FolderOverlayState state,
                      const std::wstring &path) {
    // Self-assignment is deliberate at one call site: the renderer's last-ditch
    // guard passes app.folderOverlayPath back in to mean "keep whatever folder
    // was last known". std::wstring handles it, and spelling it at the call
    // site beats a second overload that means the same thing.
    app.folderOverlay     = state;
    app.folderOverlayPath = path;
    UpdateWindowTitle(hWnd);
}

void ClearFolderOverlay(HWND hWnd) {
    app.folderOverlay = AppState::FolderOverlayState::None;
    // Cleared with the state, always. It belongs to whichever state set it, and
    // a leftover from an Unsupported file would otherwise be shown as the
    // second line of the next Empty folder.
    app.folderOverlayPath.clear();
    UpdateWindowTitle(hWnd);
}

// Called on the UI thread from the WM_QIV_SCAN_COMPLETE handler.
void HandleScanComplete(HWND hWnd, ScanResult *result) {
    SetCursor(Constants::Cursors::CURR_DEFAULT);

    if (result->generation != g_scanGeneration.load(std::memory_order_relaxed)) {
        delete result;
        return;
    }

    // Path currently displayed (before the playlist swap below). If the scan's
    // target is this same image, we take a flicker-free light path at the end —
    // updating index/overlay/panels without resetting the viewport or re-rendering
    // the image that is already on screen.
    std::wstring prevPath;
    if (app.currentIndex >= 0 && app.currentIndex < static_cast<int>(app.playlist.size()))
        prevPath = app.playlist[app.currentIndex];

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
            SetFolderOverlay(hWnd, AppState::FolderOverlayState::Missing, dir);
        } else {
            // Directory exists but contains no supported images.
            UI::NotifyFolderContentsChanged(dir);
            SetFolderOverlay(hWnd, AppState::FolderOverlayState::Empty, dir);
        }

        // The viewer is now showing this folder even though the playlist is empty.
        // Tell the History panel explicitly — it cannot infer it from the playlist,
        // which was just cleared above, and without this its green "you are here"
        // row stays stuck on whatever folder was open before.
        UI::NotifyCurrentFolder(dir);

        InvalidateRect(hWnd, nullptr, FALSE);
        delete result;
        return;
    }

    // Prune VRAM cache entries only for files confirmed deleted from disk.
    // Files that exist on disk but aren't in the new playlist (different folder,
    // rename, etc.) are kept so CacheWnd shows a true cross-folder history.
    if (result->playlist != app.playlist && app.renderer) {
        std::unordered_set<std::wstring> newSet;
        newSet.reserve(result->playlist.size());
        newSet.insert(result->playlist.begin(), result->playlist.end());
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

    // Pre-stage the final index so panels rebuild their highlight at the correct
    // slot right away (refreshing with the stale interim index painted the
    // selection on thumbnail 0 for one frame — visible flicker + jump). The real
    // image load happens AFTER the refresh below: LoadImageIndex runs a
    // synchronous selection sync against the panel's thumbnails, and calling it
    // before the refresh made that sync run against the OLD folder's items —
    // path match failed and one frame painted with no/wrong selection.
    // currentIndex is restored first so LoadImageIndex's `currentIndex != index`
    // guard (viewport reset + previousImageIndex tracking) behaves unchanged;
    // nothing paints between here and the load, so the swap is invisible.
    const int preRefreshIndex = app.currentIndex;
    app.currentIndex = targetIdx;

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
    ClearFolderOverlay(hWnd);
    UI::NotifyFolderContentsChanged(scannedDir);
    // Same binding as the empty-scan path above: the viewer has settled on this
    // folder, so the History panel's green row must follow it.
    UI::NotifyCurrentFolder(scannedDir);

    // If the scan's target is the image already on screen (the common F2/click/
    // drag-drop case: the 1-file playlist decoded and displayed it, now the full
    // folder just arrived), take a FLICKER-FREE light path — adopt the final index
    // and refresh the overlay count (1/1 → N/M) + panel selection, WITHOUT resetting
    // the viewport or re-rendering the already-displayed bitmap. A full LoadImageIndex
    // here would reset the viewport and redraw the same image (redundant + flickery).
    const bool sameImage = (!prevPath.empty() &&
                            targetIdx >= 0 && targetIdx < static_cast<int>(app.playlist.size()) &&
                            app.playlist[targetIdx] == prevPath);

    if (sameImage) {
        app.currentIndex = targetIdx;
        app.wantedIndex.store(targetIdx, std::memory_order_release);
        // wantedPathHash is unchanged (same file → same hash), so an in-flight
        // decode (if the scan won the race) still completes and displays.
        UpdateOverlaysForCurrentImage(hWnd);
        uiManager.getActiveDirWnd().SyncDirSelectionRectangle();
        InvalidateRect(hWnd, nullptr, FALSE);
    } else {
        // Different target — do a normal load (decode/display the new image).
        // LoadImageIndex's internal SyncDirSelectionRectangle snaps the selection
        // rectangle and scroll to the correct (possibly re-sorted) slot.
        app.currentIndex = preRefreshIndex;
        LoadImageIndex(hWnd, targetIdx);
        InvalidateRect(hWnd, nullptr, FALSE);
    }
}

// Cancelling this chooser never quits the application.
//
// It used to: PostQuitMessage(0) whenever the dialog closed with an empty
// playlist, from the days when an empty playlist meant a black window and
// there was nothing to stay open FOR. That is no longer true — the caller at
// startup turns on the Missing/Empty overlay right after this returns, and F2
// reaches this same function from a window that is already showing it. Both
// leave the user looking at a heading, a prompt and a clickable path.
//
// Keeping the quit made it strictly harmful in both: at startup it posted
// WM_QUIT that the loop pulled before the overlay was ever painted, and from
// F2 in an empty folder it closed the application on Cancel.
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

    // Where to start browsing.
    //
    // The folder on screen first, then the one from the last exit. There used to
    // be a separate "LastFolder" setting for this, written only when a file was
    // picked THROUGH THIS DIALOG — so arriving anywhere by drag-and-drop, the
    // history panel or the command line left it pointing somewhere the user had
    // not been in a long time. It held the same kind of value as the session
    // folder and answered the same question worse, so it is gone.
    std::wstring lastFolder;
    if (!app.playlist.empty())
        lastFolder = fs::path(app.playlist[0]).parent_path().wstring();
    if (lastFolder.empty())
        lastFolder = Persistence::Session::LoadLastFolder();

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

    // Cancelled — nothing to open, and nothing to do about it.
    if (FAILED(hr)) {
        pfd->Release();
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

    // Closed with OK but no selection — same as a cancel.
    if (filePath.empty()) {
        return;
    }

    // absolute + lexically_normal, not canonical — a path picked through a
    // junction must stay spelled the way the user reached it. Existence is
    // checked by the is_directory / is_regular_file tests that follow.
    fs::path selectedPath;
    try {
        selectedPath = fs::absolute(filePath).lexically_normal();
        if (!fs::exists(selectedPath)) return;
    } catch (...) {
        return;
    }

    // User typed a folder path in the filename field.
    // With an error_code: the throwing overload raises filesystem_error for
    // anything that is not a plain "no such file" — a share that dropped, a
    // denied parent — and this runs inside the window procedure, where an
    // escaping exception crosses a kernel callback boundary and the behaviour
    // stops being defined. Everything else in this file already takes an `ec`.
    std::error_code dirEc;
    if (fs::is_directory(selectedPath, dirEc) && !dirEc) {
        OpenDirectory(hWnd, selectedPath.wstring());
        return;
    }

    // No separate last-folder write here any more. Opening this file builds the
    // playlist below, and the folder it came from is recorded once at exit like
    // every other way of arriving at a folder.

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

void ReclampLockedViewport(HWND hWnd) {
    // A carried-over zoom/pan was legal for the PREVIOUS image, not this one:
    // the effective-zoom limits depend on the fit scale (so on the image's
    // dimensions), and the legal pan range shrinks with the rendered size.
    // Runs only once the bitmap is in and app.imgWidth/imgHeight are current.
    //
    // Gated on the lock deliberately. Clamping an unlocked viewport would not be
    // a no-op: a freshly reset zoom of 1.0 whose fit scale sits below ZOOM_MIN
    // would get bumped off 1.0, changing how every oversized image opens.
    if (!app.lockViewport) return;
    ClampZoomToLimits(hWnd);
    ClampViewportOffset(hWnd);
}

// =============================================================================
// One-shot interjected image — see FileHandler.h and AppState::Interjection.
// =============================================================================
bool ShowInterjectedImage(HWND hWnd) {
    if (app.interject.path.empty() || !app.renderer) return false;

    // Cache probe keyed by PATH. On a hit this becomes the active bitmap and
    // Render draws it — no playlist entry involved, so nothing else moves.
    if (FAILED(app.renderer->LoadBitmap(nullptr, 0, 0, app.interject.path))) {
        WarmInterjectedImage();
        return false;   // caller leaves it queued and tries again
    }

    app.interject.showing = true;
    app.interject.queued  = false;

    // Shown clean and full-frame, ignoring whatever pan/zoom the images were
    // using: it is a message dropped between two slides, not part of the walk.
    app.viewport = ViewportState{};
    // An interjection is one frame. Whatever the previous image had running must
    // not keep firing over it.
    KillTimer(hWnd, Constants::Slideshow::GIF_TIMER_ID);
    InvalidateRect(hWnd, nullptr, FALSE);

    // TELL THE WATCHERS. This is a picture change like any other, and until now
    // it was the ONE that announced nothing:
    //
    //   • The playlist index does not move, so the ImageChanged emitted further
    //     up this file — which is keyed to an index change — never fires.
    //   • The observer echo in ExecuteCommand is skipped for anything that came
    //     from the wire, so `StreamImageShow` does not echo either.
    //   • With a slideshow running this does not even happen at command time: it
    //     happens later, on the slide-boundary timer, with no command in flight
    //     at all.
    //
    // The result was a client that pushed a picture, saw it appear on the far
    // screen, and had no way to learn that it had — its preview showed whatever
    // was underneath. Announced here, at the moment it actually goes up, which
    // is the only place that is true for all three paths above.
    //
    // SENT TO EVERY OBSERVER INCLUDING THE ONE THAT PUSHED IT. This is an
    // announcement, not a command echo — it instructs nothing, and a client that
    // wants the picture asks for it — so there is no bounce to guard against.
    // The sender is precisely who needs it most.
    //
    // The index is the playlist's, unchanged and meaningless for a one-shot; the
    // NAME is what identifies it, which is what a remote client keys on anyway.
    if (Remote::HasObservers()) {
        const std::wstring name =
            app.interject.path.substr(app.interject.path.find_last_of(L"\\/") + 1);
        Remote::EmitToObservers(
            L"ImageChanged " + std::to_wstring(app.currentIndex + 1) + L"/" +
                std::to_wstring(app.playlist.size()) + L" " + name,
            Remote::CONN_NONE);
    }

    return true;
}

bool ArmInterjection(HWND hWnd, const std::wstring &path, bool immediate,
                     bool ownsTempFile) {
    // Replaces whatever was queued or showing — and, if that was a streamed temp
    // file, deletes it. Only the newest one-shot means anything; a queue of them
    // would deliver adverts minutes after they were sent.
    ClearInterjection();

    app.interject.path         = path;
    app.interject.queued       = true;
    app.interject.immediate    = immediate;
    app.interject.ownsTempFile = ownsTempFile;

    // A running slideshow owns the MOMENT: the timer puts it up at the next slide
    // boundary so the picture on screen is not cut short. Decoded ahead here, or
    // the timer would find it undecoded and show it one interval late.
    if (!immediate && app.slideshow.running && !app.slideshow.paused) {
        WarmInterjectedImage();
        return false;
    }
    return ShowInterjectedImage(hWnd);
}

void WarmInterjectedImage() {
    if (app.interject.path.empty() || !app.renderer) return;
    // Under the NEIGHBOUR guard (index-based), exactly like a promotion preload:
    // the MAIN decode guard is path-hash based and would cancel this at once,
    // since an interjection is deliberately never the "wanted" playlist image.
    (void) app.renderer->PreloadBitmap(app.interject.path, app.currentIndex,
                                       app.currentIndex);
}

void ClearInterjection() {
    if (app.interject.path.empty() && !app.interject.showing &&
        !app.interject.queued)
        return;

    // Evicted, not merely forgotten. A one-shot advert that stayed in the VRAM
    // cache would go on occupying it — and would then be served instantly to a
    // later probe for the same path, which is not what "one-shot" means.
    if (app.renderer && !app.interject.path.empty())
        app.renderer->RemoveFromCache(app.interject.path);

    // A STREAMED image lives in a temp file this process wrote. Deleted with it —
    // the alternative is a temp folder that grows by one file per advert. Done
    // after the cache eviction, so nothing still holds the handle.
    if (app.interject.ownsTempFile && !app.interject.path.empty()) {
        std::error_code ec;
        std::filesystem::remove(std::filesystem::path(app.interject.path), ec);
        // A failure here is not worth reporting: the file is in the temp folder,
        // it is named after this process, and Windows clears it eventually.
    }

    app.interject = AppState::Interjection{};
}

void LoadImageIndex(HWND hWnd, int index) {
    if (index < 0 || index >= static_cast<int>(app.playlist.size())) return;

    // ANY change of picture retires an interjection — the slideshow's next tick,
    // a wheel notch, a thumbnail click, an inbound `JumpToImage`. This is the one
    // place every image change passes through, which is why "shown once" needs no
    // timer and no bookkeeping of its own. A queued-but-unshown one is dropped
    // too: it was meant for the moment it arrived.
    if (app.interject.showing || app.interject.queued) ClearInterjection();

    if (app.currentIndex != index) {
        // Viewport lock (Y) carries zoom + pan to the next image so a flip
        // through same-framed shots stays on the same detail. Rotation and the
        // two flips are deliberately NOT carried: ApplyOrientationToViewport
        // rewrites them from the new file's EXIF tag below, so holding them here
        // would fight that and show portrait shots sideways in a mixed folder.
        const float keepZoom = app.viewport.zoom;
        const float keepOffX = app.viewport.offsetX;
        const float keepOffY = app.viewport.offsetY;

        app.viewport = ViewportState{};

        if (app.lockViewport) {
            app.viewport.zoom    = keepZoom;
            app.viewport.offsetX = keepOffX;
            app.viewport.offsetY = keepOffY;
        }

        if (app.currentIndex >= 0)
            app.previousImageIndex = app.currentIndex;
    }

    app.currentIndex = index;
    app.wantedIndex.store(index, std::memory_order_release);

    // Kill any running GIF animation from the previous image.
    KillTimer(hWnd, Constants::Slideshow::GIF_TIMER_ID);

    const std::wstring &currentPath = app.playlist[index];

    // =========================================================================
    // MIRROR / OBSERVE — the one place that catches EVERY change of picture.
    //
    // Here rather than in ExecuteCommand because most image changes are not
    // commands at all: a thumbnail click, a Find hit, a JumpTo, a drag-drop, and
    // above all the slideshow timer, which advances by calling this function
    // directly. An observer bound to a slave running a slideshow would otherwise
    // sit frozen on whichever frame was up when it bound.
    //
    // Sent as a 1-BASED INDEX, matching `goto`. Not a path: producing one would
    // mean a filesystem round trip on the UI thread for every thumbnail click,
    // and an index stays meaningful because sort order is itself mirrored, so
    // both ends order their playlists the same way. The file NAME travels in the
    // reply instead, where it costs nothing and lets the sender detect the case
    // sort parity cannot cover — the two ends holding different FILE SETS.
    //
    // Skipped entirely for an inbound command: the far end changing picture
    // because we told it to must not tell us to change picture.
    // =========================================================================
    // EVERY TEST BEFORE ANY WORK. This function runs on every image change —
    // each wheel notch, each slideshow tick — so in a viewer that is driving
    // nothing and watched by nobody (which is the normal case) the whole block
    // must cost three loads and no allocation. Building the `goto` line first
    // and asking afterwards would put two heap allocations on that path forever.
    //
    // Driving: suppressed while a navigation COMMAND is being forwarded — the
    // gate in ExecuteCommand already sent `next`, and each target applies that
    // to its own playlist; sending our index on top would override the result
    // with a position from a different list.
    const bool tellTargets = app.passCommandToRemote && Remote::Mirror::HasLiveTargets() &&
                             !Remote::InboundActive() && !Remote::ForwardInFlight();
    const bool tellObservers = Remote::HasObservers() && !Remote::InboundActive();

    if (tellTargets || tellObservers) {
        const std::wstring line = L"JumpToImage " + std::to_wstring(index + 1);

        // Same-machine targets only — BroadcastPosition drops the rest, because
        // an index means nothing against a playlist of different files.
        if (tellTargets) {
            Remote::Mirror::BroadcastPosition(
                line, currentPath.substr(currentPath.find_last_of(L"\\/") + 1));
        }
        // positional: a `goto` reaches same-machine observers only.
        if (tellObservers) {
            Remote::EmitToObservers(line, Remote::CONN_NONE, /*positional=*/true);

            // …and the machine-independent half of the same announcement.
            //
            // The `goto` above is dropped for every observer that does not share
            // this folder, correctly: an index names a different picture over
            // there. But that left an off-machine watcher — the phone app — with
            // NO event at all for a picture change, and a running slideshow
            // changes picture without issuing a command, so nothing else was
            // ever emitted either. Its preview simply froze on whatever frame
            // was up when it bound.
            //
            // So this names WHAT is being shown rather than WHERE it sits, and
            // goes to every observer. It instructs nothing; a client that wants
            // the picture asks for it (SendDisplayedImage).
            //
            // Built inside the `tellObservers` branch, after the flags have
            // already been tested — a viewer nobody is watching still allocates
            // nothing on this path, which is the whole point of the ordering
            // above.
            Remote::EmitToObservers(
                L"ImageChanged " + std::to_wstring(index + 1) + L"/" +
                    std::to_wstring(app.playlist.size()) + L" " +
                    currentPath.substr(currentPath.find_last_of(L"\\/") + 1),
                Remote::CONN_NONE);
        }
    }
    // Path-identity guard for the main decode — same file keeps the same hash
    // across a folder re-sort, so its in-flight decode is not cancelled when the
    // index changes (fixes the blank-on-startup race after an F2 open).
    app.wantedPathHash.store(std::hash<std::wstring>{}(currentPath), std::memory_order_release);

    // Recorded for a post-mortem: a decoder fault names a function, not a file,
    // and "which image was it" is the first question. One bounded copy per image
    // change, next to a hash and a window-title update that already cost more.
    Platform::Crash::NoteImage(currentPath.c_str());

    // A different image is being loaded, so whatever the placeholder was
    // reporting is a verdict on a DIFFERENT file and no longer applies.
    //
    // It was only ever cleared by a folder scan (HandleScanComplete) and by
    // OpenDirectory — never by plain navigation. So arrowing onto a file the
    // decoder refused, then arrowing back to a good one, left Unsupported set:
    // the placeholder went on drawing over a picture that had loaded perfectly,
    // still naming the broken file. Every route out of that folder cleared it,
    // which is why it survived — you had to come back within the same folder to
    // see it.
    //
    // Cleared HERE rather than on decode success, because this same call writes
    // the title: doing it later would have the window announce the previous
    // image's failure over the new one.
    //
    // app.currentIndex was set above, so the title this produces names the new
    // image — which is also what makes switching from a broken file, or from an
    // empty folder, to a real image update the taskbar. The app name is no
    // longer spelled out here either: it lived as a literal in this one line
    // while Constants::APP_NAME held the same string.
    ClearFolderOverlay(hWnd);

    // =========================================================================
    // START THE DECODE FIRST. Queue the async read/decode BELOW, then do the
    // synchronous overlay + panel paints — so the decoder worker reads and
    // decodes in parallel with that UI work, shaving it off click-to-screen.
    // All three open paths (F2 dialog, drag-drop, shell/CLI) funnel through here.
    // =========================================================================
    bool cacheHit = false;

    // -------------------------------------------------------------------------
    // SVG path: load bytes on IO thread, then WM_QIV_SVG_READY hands them to
    // PreloadSvgFromBytes which rasterizes + uploads on the decoder worker.
    // -------------------------------------------------------------------------
    if (SvgDecoder::IsSvgPath(currentPath)) {
        if (app.renderer) app.renderer->ClearActiveImage();

        g_ioWorker.PushTask([currentPath, index, hWnd]() {
            // Path-identity guard (survives the post-open folder re-sort), same as raster.
            if (app.wantedPathHash.load(std::memory_order_acquire) != std::hash<std::wstring>{}(currentPath))
                return;

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
    } else if (app.renderer) {
        // -------------------------------------------------------------------------
        // Raster path (fully async). A VRAM cache hit finalizes inline so the paint
        // below shows the new bitmap directly (no flash of the old image); a miss
        // queues the decode now, and WM_QIV_REPAINT swaps the image in when it lands.
        // -------------------------------------------------------------------------
        ImageLoadStats::g_loadStartUs.store(ImageLoadStats::NowUs(), std::memory_order_relaxed);

        if (SUCCEEDED(app.renderer->LoadBitmap(nullptr, 0, 0, currentPath))) {
            cacheHit = true;
            ImageLoadStats::g_lastLoadUs.store(
                    ImageLoadStats::NowUs() -
                    ImageLoadStats::g_loadStartUs.load(std::memory_order_relaxed),
                    std::memory_order_relaxed);
            // Orientation stored in the cache entry, applied after the viewport reset.
            ApplyOrientationToViewport(app.renderer->GetCachedOrientation(currentPath));
            // LoadBitmap has set imgWidth/imgHeight — safe to re-clamp a locked
            // viewport against the new image's dimensions.
            ReclampLockedViewport(hWnd);
            // Rewire the effect graph to the new bitmap before it paints below.
            app.UpdateRendererColorEffects(hWnd);
            if (app.renderer->IsAnimatedGif())
                SetTimer(hWnd, Constants::Slideshow::GIF_TIMER_ID,
                         app.renderer->GetCurrentGifDelay(), nullptr);
        } else {
            (void) app.renderer->PreloadBitmap(currentPath, index);
        }
    }

    // =========================================================================
    // UI-thread work, overlapping the worker decode queued above. Overlay text
    // (filename/index) shows instantly; UpdateWindow forces a synchronous render
    // (Present(0,0) ~1-2ms). On a cache hit this paints the new bitmap directly;
    // on a miss it paints the blank/old frame and WM_QIV_REPAINT swaps in the
    // image the moment the decode completes.
    // =========================================================================
    UpdateOverlaysForCurrentImage(hWnd);
    InvalidateRect(hWnd, nullptr, FALSE);
    UpdateWindow(hWnd);
    uiManager.getActiveDirWnd().SyncDirSelectionRectangle();
    if (cacheHit) {
        uiManager.RefreshInfoWindowIfVisible();
        uiManager.RefreshStatsWindowIfVisible();
    }

    SetTimer(hWnd, Constants::LOOKASIDE_TIMER_ID,
             Constants::PRELOAD_TIMER_COUNTDOWN, nullptr);
}

void OpenDirectory(HWND hWnd, const std::wstring &dirPathStr) {
    // Deliberately NOT canonical(): that resolves junctions and directory
    // symlinks, so opening D:\12_Wallpapers\... (a junction) recorded
    // E:\12_Wallpapers\... instead. Everything downstream then disagreed with the
    // row the user actually picked — history recorded the target, the History
    // panel greened the target's row, and the folder-walk cursor sat on the row
    // that had been chosen. Keep the caller's spelling; absolute + lexically_normal
    // still cleans up relatives and any ".." without touching the link.
    std::error_code ec;
    fs::path dirPath = fs::absolute(fs::path(dirPathStr), ec).lexically_normal();
    if (ec) return;
    // canonical used to double as the existence check — now explicit.
    if (!fs::is_directory(dirPath, ec) || ec) return;

    // Valid directory confirmed — dismiss any Missing/Empty overlay immediately.
    ClearFolderOverlay(hWnd);

    // If we are already in this directory, just jump to the first image.
    // Paths are no longer resolved, so string comparison would miss "D:\x is the
    // same folder as E:\x". SameRealFolder answers that from the History panel's
    // cached link info — the same data that drives the 🔗 row marker — and does no
    // filesystem work for ordinary non-aliased paths.
    if (!app.playlist.empty()) {
        const std::wstring curParent =
                fs::path(app.playlist[0]).parent_path().wstring();
        if (UI::SameRealFolder(dirPath.wstring(), curParent)) {
            UI::PushFolderHistory(dirPath.wstring());
            // The active panel may be a spawned DirWnd still showing a different
            // folder — retarget it so history navigation always lands in the
            // panel the user selected, even when the main viewer is already here.
            UI::ThumbnailPanelWnd &activePanel = uiManager.getActiveDirWnd();
            if (&activePanel != &uiManager.getDirWindow()) {
                bool samefolder = false;
                const std::wstring panelFolder = activePanel.GetPanelFolder();
                if (!panelFolder.empty()) {
                    std::error_code fec;
                    samefolder = fs::equivalent(fs::path(panelFolder), dirPath, fec) && !fec;
                }
                if (!samefolder) {
                    activePanel.ClearDirThumbnailCache();
                    static_cast<UI::SpawnedDirWnd &>(activePanel).LoadFolder(dirPath.wstring());
                    activePanel.UpdateDirView();
                }
            }
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
        UI::ThumbnailPanelWnd &activePanel = uiManager.getActiveDirWnd();
        const bool activeIsPrimary = (&activePanel == &uiManager.getDirWindow());
        activePanel.ClearDirThumbnailCache();
        if (!activeIsPrimary) {
            // Retarget the active spawned panel to the new (empty) folder so the
            // empty-scan notify matches it and shows the placeholder there.
            static_cast<UI::SpawnedDirWnd &>(activePanel).LoadFolder(dirPath.wstring());
            activePanel.UpdateDirView();
        }
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
    UI::ThumbnailPanelWnd &activePanel = uiManager.getActiveDirWnd();
    const bool activeIsPrimary = (&activePanel == &uiManager.getDirWindow());
    activePanel.ClearDirThumbnailCache();
    if (activeIsPrimary) {
        uiManager.getDirWindow().SetPlaylistCopy(app.playlist);
    } else {
        // The active panel is a spawned DirWnd — the user navigates history INTO
        // that panel. Retarget it to the new folder now: LoadFolder installs the
        // folder's files (name-sorted) immediately, and — critically — updates
        // m_folderPath so the panel's OnFolderRefreshed accepts the sorted scan
        // result when it arrives. Without this the panel still points at its old
        // folder, rejects the result, and nothing updates anywhere.
        static_cast<UI::SpawnedDirWnd &>(activePanel).LoadFolder(dirPath.wstring());
        activePanel.UpdateDirView();
    }

    LoadImageIndex(hWnd, 0);
    app.previousImageIndex = -1;

    // Background: full scan + sort. targetPath empty → navigate to index 0 after sort.
    // When a spawned panel is active it adopts the result via LoadFolder above;
    // the primary DirWnd keeps its own folder (activeIsPrimary = false skips it).
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


// =============================================================================
// OpenStartupTarget — what to show when launched with no file or folder.
//
// The file chooser used to be the unconditional answer, which meant a modal
// dialog on every plain launch. It is now the LAST resort: the app resumes
// where it was left, and only asks when it genuinely has nowhere to go.
// =============================================================================
void OpenStartupTarget(HWND hWnd) {
    std::error_code ec;

    // EVERY tier below only counts if it actually produced something to look at.
    //
    // Existing is not the same as usable: a remembered folder can still be
    // there and be empty, or hold nothing this build can decode, and a
    // remembered image can be a zero-byte leftover. Checking the playlist after
    // each attempt is what turns "the path resolves" into "there is an image on
    // screen" — otherwise a deleted-out folder opens qIV to a blank window with
    // no hint of what to do next.
    //
    // Safe to test synchronously: OpenDirectory seeds the playlist with the
    // first image it finds BEFORE handing the rest to the background scan, so
    // an empty playlist here means the folder genuinely had nothing.

    // 1. The image on screen at the last exit — qivSession.ini, not a setting.
    const std::wstring last = Persistence::Session::LoadLastImage();
    if (!last.empty() && fs::is_regular_file(last, ec) && !ec) {
        OpenSpecificImage(hWnd, last);
        if (!app.playlist.empty()) return;
    }

    // 2. The folder that was open at the last exit. Sits ahead of the history
    //    because it is the more specific answer: history's newest entry is
    //    normally the same folder, but it is not there at all for someone who
    //    turned history off, and the image above records nothing when the folder
    //    held no openable image.
    const std::wstring lastFolder = Persistence::Session::LoadLastFolder();
    if (!lastFolder.empty()) {
        ec.clear();
        if (fs::is_directory(lastFolder, ec) && !ec) {
            OpenDirectory(hWnd, lastFolder);
            if (!app.playlist.empty()) return;
        }
    }

    // 3. History, most-recent first. Skips folders that have since been removed
    //    or unmounted, rather than giving up on the first dead entry.
    //    Favorites need no separate pass: a favorite is a folder that is also in
    //    history, so it is already covered here.
    for (const std::wstring &folder : UI::GetFolderHistory()) {
        ec.clear();
        if (folder.empty() || !fs::is_directory(folder, ec) || ec) continue;
        OpenDirectory(hWnd, folder);
        if (!app.playlist.empty()) return;
        // Present but empty — keep walking back through history rather than
        // stopping on it. One emptied folder should not cost the user the
        // dozen still-good ones behind it.
    }

    // 4. Nothing usable — a fresh install, history gone or corrupt, or every
    //    remembered place has since been emptied or deleted. Ask, rather than
    //    sit there blank: this is the same chooser F2 opens.
    OpenInitialImage(hWnd);

    // 5. THE BLACK-SCREEN GUARD.
    //
    // The chooser above is modal, so by here the user has either opened
    // something or dismissed it. Dismissing it used to leave a live window with
    // no playlist, no folder and folderOverlay still None — which renders as a
    // plain black rectangle with no text, no hint and nothing to click. It looks
    // exactly like a broken renderer, and that is how it was reported.
    //
    // The Missing/Empty overlay already draws the two lines wanted here — a
    // heading and the folder path, the path clickable to open it in Explorer.
    // It simply was never switched on for this case, because every existing
    // caller sets it from a folder SCAN that came back empty, and here there was
    // no scan at all.
    if (app.playlist.empty()) {
        // Name the folder we last tried, so the second line is something the
        // user recognises and can click. Empty when there is nothing to name —
        // a first run — and the renderer then draws the heading alone.
        const std::wstring &blame = lastFolder;

        ec.clear();
        const bool missing = !blame.empty() && (!fs::is_directory(blame, ec) || ec);

        SetFolderOverlay(hWnd,
                         missing ? AppState::FolderOverlayState::Missing
                                 : AppState::FolderOverlayState::Empty,
                         blame);
        InvalidateRect(hWnd, nullptr, FALSE);
    }
}

void OpenSpecificImage(HWND hWnd, const std::wstring &filePathStr) {
    fs::path filePath(filePathStr);
    {
        // Same rule as the dialog path above, and this one is reached from a
        // drag-drop and from a remote `open` — both of which hand over a path
        // this process never validated. The throwing overloads turn a dropped
        // network share into an exception inside the window procedure.
        std::error_code ec;
        if (!fs::exists(filePath, ec) || ec) return;
        if (!fs::is_regular_file(filePath, ec) || ec) return;
    }
    // Same reason as OpenDirectory: canonical() would rewrite a path that arrived
    // through a junction into the target's spelling, and the folder recorded in
    // history would not be the one the user dropped or double-clicked.
    // Scoped: the rest of this function declares its own `ec` per operation, and
    // a long-lived one here would shadow them (C4456).
    {
        std::error_code ec;
        filePath = fs::absolute(filePath, ec).lexically_normal();
        if (ec) return;
    }

    if (!app.playlist.empty()) {
        if (UI::SameRealFolder(filePath.parent_path().wstring(),
                               fs::path(app.playlist[0]).parent_path().wstring())) {
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

    // EVERY PANEL THAT KEEPS ITS OWN LIST HAS TO RE-SORT TOO. app.playlist is
    // only the viewer's sequence; F6 holds a copy and each spawned panel holds
    // a list for a folder the viewer may not even be showing. Without this they
    // kept the order they were built with until something rescanned their
    // folder, so changing the sort order visibly reordered one strip and left
    // the others alone.
    uiManager.NotifySortOrderChanged();

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
