#pragma once
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <vector>
#include <string>
#include <unordered_map>
#include <filesystem>
#include "Renderer/IRenderer.h"
#include <memory>

namespace fs = std::filesystem;

#include "Constants.h"
#include "SlideshowTransitions.h"

struct SlideshowState {
    bool running = false;
    bool paused = false;
    bool shuffle = Constants::Slideshow::IS_SHUFFLE;
    bool loop = Constants::Slideshow::IS_LOOP;
    int intervalMs = Constants::Slideshow::IS_INTERVAL_MS;
    int cursorHideMs = Constants::Slideshow::CURSOR_HIDE_MS;
    bool cursorHidden = false;

    bool savedOverlayVisible = true; // overlay visibility saved at slideshow start, restored at stop

    std::vector<int> shuffleOrder; // permutation of playlist indices
    int shufflePos = 0; // current position within shuffleOrder

    SlideshowTransitionState transition;
};

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
    bool isKeepInBackground = Constants::IS_KEEP_IN_BACKGROUND;
    bool isEnableRunOnStartup = Constants::IS_ENABLE_RUN_ON_STARTUP;

    bool isDarkThemed = Constants::IS_APP_DARK_THEME;
    DWORD cornerPreference = Constants::APP_CORNER_PREFERENCES;
    float themeFactor = Constants::Theme::DEFAULT_THEME_FACTOR; // runtime 0=dark … 1=light
    DWORD backdropType = Constants::APP_BACKDROP_TYPE_DEFAULT; // 0=None,1=Mica,2=Acrylic,3=MicaAlt
    bool hasActiveEffects = false; // if any effects are used then true else false and just skip and display image
    bool effectPreviewEnabled = false; //
    int hardwareThreads = 1; //used to save cpu cores/threads query once on startup and use it
    int lastImageBeforeToggleFirstLastImageInCurrentFolder = 0; // used for saving the image index when using jump to first/last
    int fileHandlerDefaultSortOrder = Constants::FileHandler::FILE_HANDLER_DEFAULT_SORT_ORDER;
    bool fileHandlerIsReverseSortOrder = Constants::FileHandler::FILE_HANDLER_SORT_TYPE_IS_REVERSE;
    std::atomic<int> wantedIndex{-1};
    // Hash of the currently-wanted main image's path. The MAIN decode is guarded
    // by this (path identity) instead of wantedIndex, so it survives the folder
    // re-sort that runs after the initial 1-file load (F2 open) — the same file
    // gets a new index there, which would otherwise cancel its in-flight decode.
    std::atomic<size_t> wantedPathHash{0};
    Microsoft::WRL::ComPtr<IWICImagingFactory> wicFactory;
    std::unique_ptr<IImageRenderer> renderer;
    HBITMAP hDIB = nullptr;
    Constants::ViewModes::ViewMode viewMode = Constants::ViewModes::defaultViewMode;
    float dpiScale = 1.0f;
    bool isRmbDown = false;
    bool showOverlayInfoText = Constants::Overlay::DEFAULT_SHOW_OVERLAY;
    bool thumbnailPanelWheelWrapAround = Constants::THUMBNAIL_PANEL_WHEEL_WRAP_AROUND;
    bool thumbnailEffectsEnabled = Constants::ThumbnailPanel::ThumbnailEffects::EFFECTS_MASTER_ENABLED;
    bool historyFullModeEnabled  = Constants::History::HISTORY_SHOW_FULL_HISTORY;
    bool openDirWndOnStart       = Constants::IS_OPEN_DIRWND_ON_START;
    bool overlayShowBackground   = Constants::Overlay::IS_OVERLAY_SHOW_BACKGROUND;
    int  caretStyle              = Constants::InputBox::CARET_STYLE; // 0 = bar, 1 = underscore
    float zoomClickMultiplier    = Constants::ZOOM_CLICK; // left-click zoom (1 = off .. 10)
    bool swapMouseButtons        = Constants::IS_SWAP_MOUSE_BUTTONS;
    bool contextMenuEnabled      = Constants::IS_CONTEXT_MENU_ENABLED; // main-window right-click context menu
    bool ctrlCEnabled            = Constants::IS_CTRL_C_ENABLED;
    bool thumbCopyEnabled        = Constants::IS_THUMB_COPY_ENABLED;
    bool thumbMoveEnabled        = Constants::IS_THUMB_MOVE_ENABLED;
    bool thumbDeleteEnabled      = Constants::IS_THUMB_DELETE_ENABLED;
    bool thumbPasteEnabled       = Constants::IS_THUMB_PASTE_ENABLED;
    bool invertWheelDirection    = Constants::IS_MOUSE_VERTICAL_REVERSE_SCROLL_DIRECTION;
    bool invertWheelDirectionH   = Constants::IS_MOUSE_HORIZONTAL_REVERSE_SCROLL_DIRECTION;
    int  vramCacheCount          = Constants::IS_VRAM_CACHE_IMAGES_COUNT;
    int  baseWidth               = Constants::IS_BASE_WIDTH;
    int  baseHeight              = Constants::IS_BASE_HEIGHT;
    bool startInFullscreen       = false;
    int  historyMaxDirs          = Constants::History::IS_HISTORY_MAX_DIRS_TO_SHOW;
    int  historyMaxFavs          = Constants::History::IS_HISTORY_MAX_FAVORITES_TO_SHOW;
    int  dirThumbCacheMB         = Constants::IS_DIR_THUMB_CACHE_BUDGET_MB;
    int  preloadLookaside        = Constants::IS_PRELOAD_LOOKASIDE_COUNT;
    int  msgCenterDisplayMs      = static_cast<int>(Constants::Overlay::IS_MSG_CENTER_DISPLAY_MS);
    int  historyMaxDirsSave      = Constants::History::IS_HISTORY_MAX_DIRS_TO_SAVE;
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
    std::unordered_map<std::wstring, int64_t> playlistFileSizes; // path → file size bytes, from scan
    std::unordered_map<std::wstring, fs::file_time_type> playlistFileTimes; // path → last_write_time, from scan
    int currentIndex = -1;
    int previousImageIndex = -1; // E — toggle between last and current image
    ViewportState viewport;

    // Window dragging (RMB)
    bool isWindowDragging = false;
    POINT lastWindowMouse = {0, 0};

    // Right-click context menu gating: rmbDownPt is the screen point of the last
    // WM_RBUTTONDOWN; rmbConsumed goes true the moment the RMB gesture becomes a
    // drag or a combo (RMB+wheel / RMB+LMB). On WM_RBUTTONUP a still-false
    // rmbConsumed means it was a pure click → raise the context menu.
    POINT rmbDownPt = {0, 0};
    bool  rmbConsumed = false;

    // Middle mouse panning
    bool isMidDragging = false;
    bool hasMidMoved = false;
    POINT lastMidMouse = {0, 0};

    // Left click temp zoom + saved state
    bool lmbDidZoom = false; // true when LMB press actually applied the 3x zoom
    float savedZoom = 1.0f;
    float savedOffsetX = 0.0f;
    float savedOffsetY = 0.0f;

    // Fullscreen
    bool isFullscreen = false;
    RECT savedWindowRect = {0, 0, 0, 0};

    // Autosize toggle (Ctrl+Space)
    bool isAutosized = false;

    bool isDialogVisible = false;
    bool isLocked = false; // -lock:      KIOSK mode — blocks all keyboard and mouse input
    bool isDedicated = false; // -dedicated: no registry writes, separate history file
    bool isAlwaysOnTop = false; // Ctrl+T:    window stays above all others

    // Slideshow
    SlideshowState slideshow;

    // Persistent main-window overlay shown when the current directory becomes
    // unavailable.  Cleared when the user opens a new folder successfully.
    enum class FolderOverlayState { None, Missing, Empty };

    FolderOverlayState folderOverlay = FolderOverlayState::None;
    std::wstring folderOverlayPath;
    // Client-area rect of the path line in the overlay — written by the
    // renderer each frame it draws, hit-tested by MouseHandler for
    // click-to-open-in-Explorer. Zero rect = nothing clickable.
    D2D1_RECT_F folderOverlayPathRect = {};

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
        activeEffectsList.clear();
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

        int targetW = (int) (baseWidth  * dpiScale);
        int targetH = (int) (baseHeight * dpiScale);

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
    void WakeUpAndApplyEffects(HWND hWnd) {
        UpdateRendererColorEffects(hWnd);

        if (hasActiveEffects) {
            effectPreviewEnabled = true;
            renderer->UpdateColorEffects();
            InvalidateRect(hWnd, nullptr, FALSE);
        }

        // Rebuild the BOT_LEFT effects overlay text.
        // Forward-declared here to avoid a circular include with OverlayManager.h.
        extern void QIV_UpdateEffectsOverlay();
        QIV_UpdateEffectsOverlay();
    }

    void WakeUpAndApplyEffects(HWND hWnd, bool &effectToggle) {
        // 1. Flip the specific effect state FIRST
        effectToggle = !effectToggle;

        WakeUpAndApplyEffects(hWnd);
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

    // Instead of multiple booleans, use a vector to track the active order
    std::vector<std::wstring> activeEffectsList;

    // Helper to toggle an effect chronologically
    void ToggleEffectChronological(const std::wstring &effectName) {
        auto it = std::find(activeEffectsList.begin(), activeEffectsList.end(), effectName);
        if (it != activeEffectsList.end()) {
            // It's already on, so remove it
            activeEffectsList.erase(it);
        } else {
            // It's off, add it to the bottom of the list
            activeEffectsList.push_back(effectName);
        }
    }

    float GetRealZoom(HWND hWnd) const {
        if (imgWidth <= 0 || imgHeight <= 0) return 1.0f;

        RECT rc;
        GetClientRect(hWnd, &rc);
        float winW = (float) (rc.right - rc.left);
        float winH = (float) (rc.bottom - rc.top);

        // 1. Calculate the "Fit" scale (the scale at which zoom 1.0 fits the window)
        float fitScale = std::min(winW / (float) imgWidth, winH / (float) imgHeight);

        // 2. The "Real" zoom is the current zoom (which is a multiplier of fitScale)
        // divided by the fitScale to normalize it to the image's pixel size.
        // Or, more simply: (Current Rendered Size) / (Native Image Size)
        float renderW = (float) imgWidth * fitScale * viewport.zoom;
        return renderW / (float) imgWidth;
    }
};


// Global state shared across files
extern AppState app;
