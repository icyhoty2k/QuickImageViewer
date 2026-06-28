#pragma once
#include <windows.h>
#include <string>

void OpenInitialImage(HWND hWnd);
void OpenSpecificImage(HWND hWnd, const std::wstring& filePath); // <-- ADD THIS