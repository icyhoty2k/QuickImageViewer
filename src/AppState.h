#pragma once
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <vector>
#include <string>
#include <unordered_map>
#include "Renderer/IRenderer.h"
#include <memory>

#include "Constants.h"

struct ViewportState {
    int rotation = 0; // 0, 90, 180, 270
    bool flippedH = false; //horizontal Flip
    bool flippedV = false; //vertical Flip
    // Initial opacity: Fully opaque
    float zoom = 1.0f;
    float offsetX = 0.0f;
    float offsetY = 0.0f;
    bool isDragging = false;
    POINT lastMouse = {0, 0};
};

struct AppState {
    // --- THE MASTER BYPASS SWITCH ---
    // When false, the renderer completely ignores the GPU effect graph
    // and draws the raw ID2D1Bitmap natively.
    bool hasActiveEffects = false; // if any effects are used then true else false and just skip and display image
    bool effectPreviewEnabled = false; // `

    std::atomic<int> wantedIndex{-1};
    Microsoft::WRL::ComPtr<IWICImagingFactory> wicFactory;
    std::unique_ptr<IImageRenderer> renderer;
    HBITMAP hDIB = nullptr;
    Constants::ViewModes::ViewMode viewMode = Constants::ViewModes::defaultViewMode;
    float dpiScale = 1.0f;
    bool isRmbDown = false;
    bool showOverlayInfoText = Constants::DEFAULT_SHOW_OVERLAY;
    BYTE opacity = 255;
    float saturation = Constants::DEFAULT_SATURATION;
    float brightness = Constants::DEFAULT_BRIGHTNESS;
    float contrast = Constants::DEFAULT_CONTRAST;
    float gamma = Constants::DEFAULT_GAMMA;

    // Toggleable color effects — see Shortcuts::ImageEffects (Shortcuts.h is the source of truth)
    bool effectGrayscale = false; // Delete
    bool effectInvert = false; // Insert
    bool effectSepia = false; // Home
    bool effectSolarize = false; // End
    bool effectOutline = false; // Page Up
    bool effectThreshold = false; // Page Down
    int screenW = 0;
    int screenH = 0;
    int imgWidth = 0;
    int imgHeight = 0;
    std::vector<std::wstring> playlist;
    std::unordered_map<std::wstring, int> playlistIndexMap; // path → index, rebuilt with playlist
    int currentIndex = -1;
    ViewportState viewport;

    // Window dragging (RMB)
    bool isWindowDragging = false;
    POINT lastWindowMouse = {0, 0};

    // Middle mouse panning
    bool isMidDragging = false;
    bool hasMidMoved = false;
    POINT lastMidMouse = {0, 0};

    // Left click temp zoom + saved state
    float savedZoom = 1.0f;
    float savedOffsetX = 0.0f;
    float savedOffsetY = 0.0f;

    // Fullscreen
    bool isFullscreen = false;
    RECT savedWindowRect = {0, 0, 0, 0};

    bool isDialogVisible = false;

    // Helper to count active instances of this specific class
    int GetInstanceCount() const {
        int count = 0;
        EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
            wchar_t className[256];
            if (GetClassNameW(hwnd, className, 256)) {
                if (wcscmp(className, Constants::WINDOW_CLASS_NAME) == 0) {
                    (*(int *) lParam)++;
                }
            }
            return TRUE;
        }, (LPARAM) &count);
        return count;
    }

    void ResetEffects() {
        saturation = Constants::DEFAULT_SATURATION;
        brightness = Constants::DEFAULT_BRIGHTNESS;
        contrast = Constants::DEFAULT_CONTRAST;
        gamma = Constants::DEFAULT_GAMMA;

        effectGrayscale = false;
        effectInvert = false;
        effectSepia = false;
        effectSolarize = false;
        effectOutline = false;
        effectThreshold = false;

        // Flip the bypass switch so the renderer stops drawing the effect graph
        // Explicitly clear the active state flag
        hasActiveEffects = false;
        effectPreviewEnabled = false;
    }

    void ResetWindowState(HWND hWnd) {
        // --- Viewport / window ---
        viewport.zoom = 1.0f;
        viewport.offsetX = 0.0f;
        viewport.offsetY = 0.0f;
        viewport.rotation = 0;
        viewport.flippedH = false;
        viewport.flippedV = false;

        opacity = 255;
        SetLayeredWindowAttributes(hWnd, 0, opacity, LWA_ALPHA);

        int targetW = (int) (Constants::BASE_WIDTH * dpiScale);
        int targetH = (int) (Constants::BASE_HEIGHT * dpiScale);

        HMONITOR hMonitor = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = {sizeof(mi)};
        if (GetMonitorInfo(hMonitor, &mi)) {
            int monitorW = mi.rcMonitor.right - mi.rcMonitor.left;
            int monitorH = mi.rcMonitor.bottom - mi.rcMonitor.top;

            SetWindowPos(hWnd, NULL,
                         mi.rcMonitor.left + (monitorW - targetW) / 2,
                         mi.rcMonitor.top + (monitorH - targetH) / 2,
                         targetW, targetH,
                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }
    }

    // HELPER: Auto-wakes the preview toggle when a user adjusts any effect
    void WakeUpAndApplyEffects(HWND hWnd, bool &effectToggle) {
        // 1. Flip the specific effect state FIRST
        effectToggle = !effectToggle;

        // 2. Now that the state is flipped, we can evaluate hasActiveEffects
        //    and trigger the renderer.
        UpdateRendererColorEffects(hWnd);

        // 3. If the user turned something on, ensure the preview is visible
        if (hasActiveEffects) {
            effectPreviewEnabled = true;
            renderer->UpdateColorEffects();
            InvalidateRect(hWnd, nullptr, FALSE);
        }
    }

    void UpdateRendererColorEffects(HWND hWnd) {
        // 1. Evaluate if any value deviates from the strict defaults
        hasActiveEffects =
                std::abs(saturation - Constants::DEFAULT_SATURATION) > 0.001f ||
                std::abs(contrast - Constants::DEFAULT_CONTRAST) > 0.001f ||
                std::abs(brightness - Constants::DEFAULT_BRIGHTNESS) > 0.001f ||
                std::abs(gamma - Constants::DEFAULT_GAMMA) > 0.001f ||
                effectGrayscale || effectInvert || effectSepia ||
                effectSolarize || effectThreshold || effectOutline;

        // 2. Forward the update to the active renderer
        if (renderer) {
            renderer->UpdateColorEffects();
        }

        InvalidateRect(hWnd, nullptr, FALSE);
    }
};


// Global state shared across files
extern AppState g_app;
