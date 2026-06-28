#pragma once
#include <windows.h>
#include <string>

void OpenInitialImage(HWND hWnd);

void OpenSpecificImage(HWND hWnd, const std::wstring &filePath);

// Loads the image at the given playlist index, resets viewport if index changed,
// and kicks off async preload. Declared here so AppMain and MouseHandler can call it.
void LoadImageIndex(HWND hWnd, int index);
