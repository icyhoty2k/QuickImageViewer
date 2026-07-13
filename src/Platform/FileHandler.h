#pragma once
#include <windows.h>
#include <string>

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
