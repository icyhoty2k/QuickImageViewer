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
    std::vector<std::wstring>                            playlist;
    std::unordered_map<std::wstring, int64_t>            fileSizes;
    std::unordered_map<std::wstring, fs::file_time_type> fileTimes;
    std::wstring                                         targetPath; // navigate here after swap; empty = index 0
    uint64_t                                             generation;
};

void OpenInitialImage(HWND hWnd);

void OpenDirectory(HWND hWnd, const std::wstring &dirPathStr);

void OpenSpecificImage(HWND hWnd, const std::wstring &filePath);

bool is_image_ext(const std::wstring &ext);

// Loads the image at the given playlist index, resets viewport if index changed,
// and kicks off async preload. Declared here so AppMain and MouseHandler can call it.
void LoadImageIndex(HWND hWnd, int index);

// Re-sorts the current playlist using app.fileHandlerDefaultSortOrder and
// app.fileHandlerIsReverseSortOrder, then rebuilds the O(1) index map.
// Call after changing either setting at runtime.
void ReSortPlaylistAndRebuildMap(HWND hWnd);

// Maps an EXIF orientation tag value (1-8) to app.viewport rotation + flip.
// Call after app.viewport = ViewportState{} when the bitmap arrives in cache.
void ApplyOrientationToViewport(USHORT orient);

// Called on the UI thread when WM_QIV_SCAN_COMPLETE is received.
// Takes ownership of result and deletes it.
void HandleScanComplete(HWND hWnd, ScanResult *result);

// True while a background directory scan is in progress.
// Read on the UI thread to show/hide the wait cursor.
extern std::atomic<bool> g_scanInProgress;
