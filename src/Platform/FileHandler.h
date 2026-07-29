#pragma once
#include <windows.h>
#include <atomic>
#include <string>
#include <vector>
#include <unordered_map>
#include <filesystem>

namespace fs = std::filesystem;

// Heap-allocated result from a background directory scan.
// Ownership transfers to the UI thread via WM_QIV_SCAN_COMPLETE (LPARAM).
struct ScanResult {
    std::vector<std::wstring> playlist;
    std::unordered_map<std::wstring, int64_t> fileSizes;
    std::unordered_map<std::wstring, fs::file_time_type> fileTimes;
    std::wstring targetPath; // navigate here after swap; empty = index 0
    std::wstring scannedDir; // always set — valid even when playlist is empty
    uint64_t generation;
    // false when the scan was triggered by a SpawnedDirWnd click — F6 DirWnd
    // must not follow because it tracks a different, user-pinned folder.
    bool updatePrimaryDirWnd = true;
};

void OpenInitialImage(HWND hWnd);

void ReloadCurrentDirectory(HWND hWnd);

void OpenDirectory(HWND hWnd, const std::wstring &dirPathStr);

void OpenSpecificImage(HWND hWnd, const std::wstring &filePath);

// Resolves what to show when the app is launched with no file or folder.
// Tries, in order:
//   1. the image that was on screen at the last exit
//   2. the first folder in history that still exists
//   3. the file chooser, as a last resort
// So the chooser only ever appears on a genuinely fresh or broken install.
void OpenStartupTarget(HWND hWnd);

bool is_image_ext(const std::wstring &ext);

// Loads the image at the given playlist index, resets viewport if index changed,
// and kicks off async preload. Declared here so AppMain and MouseHandler can call it.
void LoadImageIndex(HWND hWnd, int index);

// --- One-shot interjected image (app.interject) ------------------------------
//
// ONE picture to be shown once, from any of the three routes that produce one:
// `ShowImageOnce <path>`, an inbound StreamImage* transfer (Alt+Enter elsewhere),
// or Ctrl+Alt+Enter here fetching what another instance is displaying. These
// functions are the whole mechanism, and they live beside LoadImageIndex because
// that is the function they have to stay honest with.
//
// Nothing here touches app.playlist, app.currentIndex or the sort order. The
// image becomes the renderer's ACTIVE bitmap through the PATH-keyed cache — the
// same trick a Dedicated promotion uses — so the sequence resumes afterwards
// with nothing to restore.

// Makes app.interject.path the active bitmap. Returns false when it is not
// decoded yet: the cache is warmed and the caller leaves the request queued, so
// a slideshow is never stalled waiting for a decode.
bool ShowInterjectedImage(HWND hWnd);

// Arms an interjection for `path` and either shows it or leaves it queued,
// according to the same rule everywhere: a running slideshow gets it at the next
// slide boundary unless `immediate` (a picture the user asked for AT THIS
// keyboard, i.e. a stream-in, which must not appear to do nothing).
//
// `ownsTempFile` says the path is a temp file written from streamed bytes, so
// retiring it deletes the file. Returns true when it is on screen already.
bool ArmInterjection(HWND hWnd, const std::wstring &path, bool immediate,
                     bool ownsTempFile);

// Decode-ahead without displaying. Used when a slideshow is running and the
// image is meant to appear at the NEXT slide boundary rather than now.
void WarmInterjectedImage();

// Forgets the interjection and EVICTS it from the VRAM cache — a one-shot image
// must not sit there afterwards holding memory nobody asked it to hold. Called
// by every path that changes picture, so "shown once" needs no timer.
void ClearInterjection();

// Re-sorts the current playlist using app.fileHandlerDefaultSortOrder and
// app.fileHandlerIsReverseSortOrder, then rebuilds the O(1) index map.
// Call after changing either setting at runtime.
void ReSortPlaylistAndRebuildMap(HWND hWnd);

// Maps an EXIF orientation tag value (1-8) to app.viewport rotation + flip.
// Call after app.viewport = ViewportState{} when the bitmap arrives in cache.
void ApplyOrientationToViewport(USHORT orient);

// Re-clamps a LOCKED viewport (app.lockViewport, the Y key) against the newly
// loaded image's dimensions. No-op when the lock is off. Call at every site that
// finishes bringing a bitmap in — i.e. right after ApplyOrientationToViewport,
// once app.imgWidth/imgHeight belong to the new image.
void ReclampLockedViewport(HWND hWnd);

// Called on the UI thread when WM_QIV_SCAN_COMPLETE is received.
// Takes ownership of result and deletes it.
void HandleScanComplete(HWND hWnd, ScanResult *result);

// True while a background directory scan is in progress.
// Read on the UI thread to show/hide the wait cursor.
extern std::atomic<bool> g_scanInProgress;

// Adaptive reserve hint for directory scans. Seeded by the previous scan's
// image count so per-folder containers (playlist + size/time maps, DirWnd's
// playlist) pre-size to the user's typical folder in a single allocation —
// folders in one collection tend to be similar sizes, so last-count is a strong
// predictor. Clamped to [256, 16384] so the first scan and pathological folders
// both stay bounded. Thread-safe (read on worker + UI threads).
size_t DirScanReserveHint();
void   RecordDirScanCount(size_t n);

