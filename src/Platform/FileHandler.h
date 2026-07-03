#pragma once
#include <windows.h>
#include <string>

void OpenInitialImage(HWND hWnd);

void OpenSpecificImage(HWND hWnd, const std::wstring &filePath);

bool is_image_ext(const std::wstring &ext);

// Loads the image at the given playlist index, resets viewport if index changed,
// and kicks off async preload. Declared here so AppMain and MouseHandler can call it.
void LoadImageIndex(HWND hWnd, int index);
