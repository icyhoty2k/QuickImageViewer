#pragma once
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <vector>
#include <string>

struct ViewportState {
    float zoom = 1.0f;
    float offsetX = 0.0f;
    float offsetY = 0.0f;
    bool isDragging = false;
    POINT lastMouse = { 0, 0 };
};

struct AppState {
    Microsoft::WRL::ComPtr<IWICImagingFactory> wicFactory;
    HBITMAP hDIB = nullptr;
    int imgWidth = 0;
    int imgHeight = 0;
    std::vector<std::wstring> playlist;
    int currentIndex = -1;
    ViewportState viewport;
    bool isWindowDragging = false;
    POINT lastWindowMouse = {0, 0};
    bool isFullscreen = false;
    RECT savedWindowRect = {0, 0, 0, 0};
};

// Global state shared across files
extern AppState g_app;