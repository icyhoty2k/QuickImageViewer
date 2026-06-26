#pragma once
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <vector>
#include <string>
#include "Renderer/IRenderer.h"
#include <memory>

struct ViewportState {

    float zoom    = 1.0f;
    float offsetX = 0.0f;
    float offsetY = 0.0f;
    bool isDragging = false;
    POINT lastMouse = { 0, 0 };
};

struct AppState {
    Microsoft::WRL::ComPtr<IWICImagingFactory> wicFactory;
    std::unique_ptr<IImageRenderer> renderer;
    HBITMAP hDIB = nullptr;
    float dpiScale = 1.0f;
    int screenW = 0;
    int screenH = 0;
    int imgWidth  = 0;
    int imgHeight = 0;
    std::vector<std::wstring> playlist;
    int currentIndex = -1;
    ViewportState viewport;

    // Window dragging (RMB)
    bool isWindowDragging = false;
    POINT lastWindowMouse = {0, 0};

    // Middle mouse panning
    bool isMidDragging = false;
    bool hasMidMoved   = false;
    POINT lastMidMouse = {0, 0};

    // Left click temp zoom + saved state
    float savedZoom    = 1.0f;
    float savedOffsetX = 0.0f;
    float savedOffsetY = 0.0f;

    // Fullscreen
    bool isFullscreen = false;
    RECT savedWindowRect = {0, 0, 0, 0};

    bool isDialogVisible = false;
};

// Global state shared across files
extern AppState g_app;