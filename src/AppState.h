// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#pragma once
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <vector>
#include <string>
#include <unordered_map>
#include <filesystem>
#include <algorithm>
#include "Renderer/IRenderer.h"
#include "Common/Converters.h"   // PercentToRatio — ZOOM_MIN/MAX are percents
#include <memory>

namespace fs = std::filesystem;

#include "Constants.h"
#include "ConstantsTheme.h"
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

// Mirrors the renderer's renderW/renderH calculation. Every call site (renderer,
// mouse handler, AppState::GetRealZoom) must use this so all sizes match exactly.
inline void GetRenderSize(float winW, float winH, float imgW, float imgH,
                          Constants::ViewModes::ViewMode viewMode, float zoom,
                          float &renderW, float &renderH) {
    // A DEGENERATE SIZE IS ANSWERED HERE, not by each caller remembering to.
    //
    // The FitToView branch divides by imgW and imgH. Float division by zero is
    // not a crash — it is an infinity, and `imgW * scale` then makes the result
    // NaN, which propagates into the viewport offsets and the pan clamps and
    // parks the image somewhere no comparison can bring it back from. Silent,
    // and much harder to trace than the divide that caused it.
    //
    // Every one of the twelve call sites currently guards imgW/imgH itself, so
    // this changes nothing today — except that the guarantee now belongs to the
    // function whose header already insists every caller go through it, instead
    // of to twelve places that each have to keep remembering. The one that came
    // closest to needing it is the SVG path, which substitutes the render-target
    // size when the file declares none.
    //
    // Zero out rather than fall back to the window: nothing can be drawn for an
    // image with no area, and a zero rect says that where a window-sized one
    // would claim there is something on screen.
    if (!(imgW > 0.0f) || !(imgH > 0.0f) || !(winW > 0.0f) || !(winH > 0.0f)) {
        renderW = 0.0f;
        renderH = 0.0f;
        return;
    }

    renderW = imgW;
    renderH = imgH;
    switch (viewMode) {
        case Constants::ViewModes::ViewMode::FitToView_PreserveAspectRatio:
        default: {
            const float scale = std::min(winW / imgW, winH / imgH);
            renderW = imgW * scale;
            renderH = imgH * scale;
            break;
        }
        case Constants::ViewModes::ViewMode::FitToWidth_DoNotPreserveAspectRatio:
            renderW = winW;
            renderH = imgH;
            if (renderH > winH) renderH = winH;
            break;
        case Constants::ViewModes::ViewMode::FitToHeight_DoNotPreserveAspectRatio:
            renderH = winH;
            renderW = imgW;
            if (renderW > winW) renderW = winW;
            break;
        case Constants::ViewModes::ViewMode::FitToWindow_DoNotPreserveAspectRatio:
            renderW = winW;
            renderH = winH;
            break;
        case Constants::ViewModes::ViewMode::OriginalImageSize_PreserveAspectRatio:
            break;
    }
    const float z = (zoom <= 0.0f) ? 1.0f : zoom;
    renderW *= z;
    renderH *= z;
}

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
    // Y — carry viewport.zoom/offsetX/offsetY across an image change instead of
    // resetting them in LoadImageIndex. Rotation and flips still reset, because
    // EXIF auto-orientation owns those (see Constants::IS_LOCK_VIEWPORT).
    bool lockViewport            = Constants::IS_LOCK_VIEWPORT;

    // Reopen the main window where it was left. The rect itself is session
    // state, not a setting — it lives in qivSession.ini or the registry with
    // the rest of the session (see Persistence/SessionFile.h). This flag is the
    // ordinary persisted setting that decides whether any of that is consulted.
    bool rememberWindowPosition = Constants::IS_REMEMBER_WINDOW_POSITION;
    bool historyFullModeEnabled  = Constants::History::HISTORY_SHOW_FULL_HISTORY;
    bool openDirWndOnStart       = Constants::IS_OPEN_DIRWND_ON_START;
    bool overlayShowBackground   = Constants::Overlay::IS_OVERLAY_SHOW_BACKGROUND;
    // Overlay layout mode (0 grid / 1 stacked / 2 summary) and the per-slot
    // visibility + compact bitmasks, one bit per OverlayManager::Slot.
    // OverlayManager seeds itself from these in Init() and writes them back on
    // every change, so these three stay the persisted truth.
    int      overlayLayoutMode     = Constants::Overlay::DEFAULT_LAYOUT_MODE;
    unsigned overlaySlotVisibleMask = Constants::Overlay::DEFAULT_SLOT_VISIBLE_MASK;
    unsigned overlaySlotCompactMask = Constants::Overlay::DEFAULT_SLOT_COMPACT_MASK;
    // The two BOT_LEFT readouts. Independent of each other and of the slot's
    // own visible/compact bits. overlayShowEffectsList governs only the overlay
    // text — effectPreviewEnabled still decides whether effects are rendered.
    bool overlayShowDirName     = Constants::Overlay::SHOW_DIR_NAME;
    bool overlayShowEffectsList = Constants::Overlay::SHOW_EFFECTS_LIST;
    // Text style for the eight outer overlay slots. MID_CENTER keeps its own
    // colour and size — it is a transient notice that must stay readable
    // whatever the outer slots are set to. Family is an index into
    // Constants::Overlay::OVERLAY_FONT_FAMILIES.
    int      overlayFontSize   = Constants::Overlay::OVERLAY_FONT_SIZE_DEFAULT;
    COLORREF overlayFontColor  = Constants::Overlay::OVERLAY_FONT_COLOR_DEFAULT;
    int      overlayFontFamily = Constants::Overlay::OVERLAY_FONT_FAMILY_DEFAULT;
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

    // True while a TrackPopupMenu is up (context or tray menu). The slideshow
    // cursor-hide timer honours this so the pointer never vanishes mid-menu.
    bool isContextMenuOpen = false;

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

    // True when -hideMouse hid the cursor at startup. ShowCursor is a COUNTER,
    // so that call must be matched exactly once — stopping the slideshow undoes
    // it, which is how an unattended screen becomes operable again.
    bool cursorHiddenAtStartup = false;

    bool isDialogVisible = false;
    // KIOSK mode — MainAppWndProc swallows every keyboard and mouse message.
    // Persisted (Registry::KIOSK_LOCK), so a dedicated screen can start locked
    // straight from its .ini. Escape hatch is the tray menu, which runs its own
    // message loop and is therefore unaffected.
    bool isLocked = Constants::IS_KIOSK_LOCK_ENABLED; // -lock
    bool isDedicated = false; // -dedicated: no registry writes, separate history file
    // The window class THIS process registered. A dedicated instance uses its
    // own, so instance counting and window lookups never cross instances.
    // Set once at startup from Dedicated::ResolveWindowClassName().
    std::wstring windowClassName = Constants::WINDOW_CLASS_NAME;
    bool isAlwaysOnTop = Constants::IS_ALWAYS_ON_TOP; // Ctrl+T / -awaysOnTop

    // --- Mirroring to other instances (src/Rem_TCP_IP) ----------------------
    // F11: forward every mirrorable command to the connected targets.
    // F12: while mirroring, ALSO execute it here. F11 on + F12 off makes this
    //      viewer a pure remote control — it drives the other screens and its
    //      own display stays put.
    //
    // DELIBERATELY NOT PERSISTED, and so deliberately absent from
    // Constants::Registry and RegistryManager. A viewer that came back from a
    // restart already forwarding keystrokes to machines the user had forgotten
    // about would be the worst kind of surprise. Both start false, every launch.
    //
    // There is no third flag for "am I being observed": that is
    // Remote::HasObservers(), i.e. whether the observer vector is non-empty. A
    // bool beside it could only ever disagree with it.
    bool passCommandToRemote   = false; // F11
    bool resendCommandToCaller = false; // F12

    // Ctrl+F12 — is the remote log recording? Same session-only reasoning as the
    // two above, and the default is the one place that decides it.
    //
    // THE UI-THREAD COPY. The producers are socket and sender threads, which may
    // not read `app` at all, so the value that actually gates recording is an
    // atomic inside Remote::Log — flipped in the same breath as this one. This
    // field exists so the menu, the panel and GetCommandValue have something on
    // the UI thread to read, exactly like every other toggle.
    bool remoteLogEnabled = Constants::RemoteTcpIp::REMOTE_LOG_DEFAULT;

    // Also write the wire log to rotating files under logs\ beside the exe.
    //
    // PERSISTED, unlike the recording switch directly above — see
    // Constants::IS_TCP_IP_LOG for why the two differ. Same UI-thread-copy
    // arrangement: the value the writer actually reads is an atomic inside
    // Remote::Log, flipped in the same breath as this one.
    bool remoteLogToFile = Constants::IS_TCP_IP_LOG;

    // The General log — what this instance did, as opposed to what it said on
    // the wire. Same arrangement as the field above in every respect; the value
    // the writer reads is an atomic inside AppLog, flipped alongside this one.
    //
    // It exists mainly for a DEDICATED instance: started with Windows, showing a
    // locked fullscreen slideshow on one monitor, with nobody in front of it and
    // no panel to look at. See Platform/AppLog.h.
    bool generalLog = Constants::IS_GENERAL_LOG;

    // --- One-shot INTERJECTED image -------------------------------------------
    //
    // On the wire this arrives either as `ShowImageOnce <path>` (one machine) or
    // as a StreamImageBegin/Chunk/Show transfer (any machine, the picture's own
    // bytes) — and from Ctrl+Alt+Enter here, which fetches what another instance
    // is displaying. All three land in this one piece of state.
    //
    // Another instance pressed Alt+Enter and sent one picture to be shown ONCE.
    // It is displayed INSTEAD of the current playlist entry and changes nothing
    // else: playlist, currentIndex, sort order and the async pipeline are all left
    // exactly as they were, and the next image change of any kind drops it. Think
    // advert between two slides — afterwards the sequence carries on as if it had
    // never happened, because as far as the sequence is concerned it did not.
    //
    // Mechanically this is the Dedicated PROMOTION trick (DedicatedInstance.cpp):
    // the path is made the renderer's ACTIVE bitmap through the path-keyed cache,
    // so nothing has to know a playlist entry was not involved.
    //
    // SESSION-ONLY, like the mirror flags — nothing here is persisted. A one-shot
    // image that survived a restart would be neither one-shot nor asked for.
    struct Interjection {
        // Queued but not yet on screen. A running slideshow owns the MOMENT: the
        // timer shows it at the next slide boundary rather than cutting the
        // current slide short. A still viewer shows it as soon as it is decoded.
        bool         queued  = false;
        bool         showing = false;
        std::wstring path;
        // Show it the moment it is decoded, even with a slideshow running.
        //
        // Set for a PULLED image (Ctrl+Alt+Enter here): the user asked for it at
        // this keyboard and is waiting to look at it, so making them wait for a
        // slide boundary would read as the key having done nothing. An interjection
        // ARRIVING from elsewhere is the opposite case — it waits for the boundary
        // so the slide on screen is not cut short.
        bool         immediate = false;
        // The path is a TEMP FILE this process wrote from streamed bytes, so
        // retiring the interjection must delete it. False for `ShowImageOnce`,
        // which names a file that belongs to the user and must be left alone —
        // one flag decides that, rather than every retire site guessing.
        bool         ownsTempFile = false;
    };
    Interjection interject;

    // Blocks the screensaver and display sleep while the main window is visible.
    // Acted on by AppCommands::ApplyDisplayAwake — never by touching
    // SetThreadExecutionState directly, because that call is per-THREAD and
    // arming it from anywhere but the UI thread silently does nothing.
    bool keepDisplayAwake = Constants::IS_KEEP_DISPLAY_AWAKE;

    // Announce the Local Server on the network so clients can discover it.
    //
    // Persisted like any other toggle, but it does NOT by itself mean a beacon is
    // live: the announcement only exists while the server is actually listening
    // and reachable. Advertising a service on a loopback-only bind, or with the
    // server stopped, would put a name on the network pointing at nothing. See
    // Remote::Beacon::Refresh, which is what reconciles this flag with reality.
    bool remoteBeacon = Constants::IS_REMOTE_BEACON_ENABLED;

    // Slideshow
    SlideshowState slideshow;

    // Persistent main-window overlay shown when the current directory becomes
    // unavailable.  Cleared when the user opens a new folder successfully.
    // Unsupported = the file is there and readable, but nothing in this build
    // can decode it. A folder problem and a file problem, reported by the same
    // placeholder because from the user's side they are the same event: the
    // window is empty and they need to know why.
    enum class FolderOverlayState { None, Missing, Empty, Unsupported };

    FolderOverlayState folderOverlay = FolderOverlayState::None;

    // What the placeholder's second line SHOWS, and what clicking it opens.
    // Always a real filesystem path — a folder for Missing/Empty, the file
    // itself for Unsupported (a click opens its folder). Shown as-is: the
    // states used to differ in what they displayed here, and making all three
    // report a plain path is what lets one line of code render any of them.
    std::wstring folderOverlayPath;

    // Client-area rects of the two clickable lines, written by the renderer
    // whenever the layout is built or re-flowed and hit-tested by MouseHandler.
    // A zero rect means that line is not clickable.
    //   Path   = the folder or file, opened in Explorer. Covers its (L) hint.
    //   Action = the constant "Open a file or folder" prompt. Covers its (F2).
    D2D1_RECT_F folderOverlayActionRect = {};
    D2D1_RECT_F folderOverlayPathRect = {};

    // Counts windows of OUR OWN class — not the global one. A dedicated copy
    // registers its own class, so it must not count the main app or another
    // instance as a duplicate of itself (HideToTray destroys rather than hides
    // when the count is > 1).
    // The class name travels through the enum context rather than the global
    // `app`, which is not declared until after this struct.
    int GetInstanceCount() const {
        struct Ctx {
            const wchar_t *cls;
            int            count;
        } ctx{windowClassName.c_str(), 0};

        EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
            auto *c = reinterpret_cast<Ctx *>(lParam);
            wchar_t className[256];
            if (GetClassNameW(hwnd, className, 256) && wcscmp(className, c->cls) == 0)
                ++c->count;
            return TRUE;
        }, reinterpret_cast<LPARAM>(&ctx));
        return ctx.count;
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

    // GEOMETRY ONLY — default size, centred on the monitor the window is
    // nearest to. Split out of ResetWindowState because the display-change
    // rescue needs exactly this and nothing else: a window stranded by an
    // unplugged monitor is a placement problem, and wiping the user's zoom,
    // rotation and effects to fix it would be a second, unrelated surprise.
    void ResetWindowGeometry(HWND hWnd) {
        int targetW = (int) (baseWidth  * dpiScale);
        int targetH = (int) (baseHeight * dpiScale);

        HMONITOR hMonitor = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = {sizeof(mi)};
        if (GetMonitorInfoW(hMonitor, &mi)) {
            int workW = mi.rcWork.right - mi.rcWork.left;
            int workH = mi.rcWork.bottom - mi.rcWork.top;

            SetWindowPos(hWnd, NULL,
                         mi.rcWork.left + (workW - targetW) / 2,
                         mi.rcWork.top  + (workH - targetH) / 2,
                         targetW, targetH,
                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }
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

        ResetWindowGeometry(hWnd);
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

    // The order the user switched effects on. This IS the render order: each
    // effect operates on the result of the ones before it, so enabling Sepia
    // while Desaturate is already on tints the desaturated image, not the
    // original. Stays in lockstep with the effect* booleans above — every
    // toggle goes through ToggleEffectChronological.
    //
    // Nothing here is destructive: the decoded bitmap is only ever the INPUT to
    // the D2D graph, so switching an effect off drops its link and the chain
    // recomputes from the untouched original.
    std::vector<std::wstring> activeEffectsList;

    // Toggles one effect's membership, appending to the END when switching on
    // so the newest effect always sees every earlier one's output.
    void ToggleEffectChronological(const std::wstring &effectName) {
        auto it = std::find(activeEffectsList.begin(), activeEffectsList.end(), effectName);
        if (it != activeEffectsList.end()) {
            activeEffectsList.erase(it); // switching off — pull its link out
        } else {
            activeEffectsList.push_back(effectName); // switching on — apply last
        }
    }

    // Effective on-screen zoom = (rendered width) / (native image width).
    // Uses the SAME GetRenderSize() the renderer uses, so it stays correct for
    // every view mode (the old version hardcoded the FitToView fit-scale and so
    // reported a bogus value in OriginalImageSize / FitToWidth / FitToHeight /
    // FitToWindow — which made "Zoom to N%" resize the image by the wrong factor).
    float GetRealZoom(HWND hWnd) const {
        if (imgWidth <= 0 || imgHeight <= 0) return 1.0f;

        RECT rc;
        GetClientRect(hWnd, &rc);
        const float winW = (float) (rc.right - rc.left);
        const float winH = (float) (rc.bottom - rc.top);
        if (winW <= 0.0f || winH <= 0.0f) return 1.0f;

        float renderW = 0.0f, renderH = 0.0f;
        GetRenderSize(winW, winH, (float) imgWidth, (float) imgHeight,
                      viewMode, viewport.zoom, renderW, renderH);
        return renderW / (float) imgWidth;
    }
};

// Global state shared across files
extern AppState app;

// =============================================================================
//  THE ONLY SUPPORTED WAY TO RAISE OR DISMISS THE BLANK-SCREEN PLACEHOLDER
// =============================================================================
//
// Assigning app.folderOverlay / app.folderOverlayPath directly is a bug. They
// have a third dependant that is not visible from here — the window title, and
// so the taskbar label — and every defect in this area has been the same shape:
// two of the three updated and the third left saying something that was true a
// moment ago.
//
//   * an empty folder left the taskbar naming the last picture
//   * a file the decoder refused did the same
//   * navigating off a refused file left the placeholder drawn OVER the good
//     image that had just loaded, still naming the broken one
//
// One call writes all of it, so there is nothing left to forget. Defined in
// FileHandler.cpp, next to the title function it drives.
//
// Neither repaints: the caller knows whether it is already inside one, and the
// renderer's own last-ditch guard calls this from mid-frame.
void SetFolderOverlay(HWND hWnd, AppState::FolderOverlayState state,
                      const std::wstring &path);

// Dismisses it. Also refreshes the title, which then falls back to naming the
// current image — so the load path needs no second call.
void ClearFolderOverlay(HWND hWnd);

// Clamps app.viewport.zoom so the EFFECTIVE on-screen zoom — the percentage the
// overlay shows — lands inside [ZOOM_MIN, ZOOM_MAX].
//
// viewport.zoom is only a multiplier on top of the view mode's base scale, so
// clamping it directly (the old `std::clamp(zoom, ZOOM_MIN, ZOOM_MAX)`) bounded
// the wrong quantity: in FitToView a 20000px image in a small window already
// starts near 0.05x, so the real zoom ran far outside the constants in one
// direction and stopped far short in the other. Every zoom entry point —
// keyboard, wheel, click-zoom, zoom panel — must go through here.
// Returns which limit was hit, so the caller can tell the user the keypress was
// capped rather than ignored. Only reports a clamp that actually CHANGED the
// zoom — sitting at the limit and zooming the other way returns None.
inline Constants::ZoomClampResult ClampZoomToLimits(HWND hWnd) {
    using Constants::ZoomClampResult;

    if (!(app.viewport.zoom > 0.0f)) app.viewport.zoom = 1.0f; // also catches NaN

    // The limits are percents; viewport.zoom is a ratio. Convert once, here.
    // With no image to scale against, baseScale is 1, so the effective-zoom
    // bounds and the multiplier bounds are the same numbers.
    float loBound = Converters::PercentToRatio(Constants::ZoomPanel::ZOOM_MIN);
    float hiBound = Converters::PercentToRatio(Constants::ZoomPanel::ZOOM_MAX);

    if (app.imgWidth > 0 && app.imgHeight > 0) {
        RECT rc;
        if (!GetClientRect(hWnd, &rc)) return ZoomClampResult::None;
        const float winW = static_cast<float>(rc.right - rc.left);
        const float winH = static_cast<float>(rc.bottom - rc.top);
        if (winW <= 0.0f || winH <= 0.0f) return ZoomClampResult::None;

        // Base scale = what the view mode renders at multiplier 1.0.
        float baseW = 0.0f, baseH = 0.0f;
        GetRenderSize(winW, winH,
                      static_cast<float>(app.imgWidth), static_cast<float>(app.imgHeight),
                      app.viewMode, 1.0f, baseW, baseH);
        const float baseScale = baseW / static_cast<float>(app.imgWidth);
        if (!(baseScale > 0.0f)) return ZoomClampResult::None;

        // effective = baseScale * zoom, so zoom = effective / baseScale.
        loBound /= baseScale;
        hiBound /= baseScale;
    }

    if (app.viewport.zoom > hiBound) {
        app.viewport.zoom = hiBound;
        return ZoomClampResult::ClampedMax;
    }
    if (app.viewport.zoom < loBound) {
        app.viewport.zoom = loBound;
        return ZoomClampResult::ClampedMin;
    }
    return ZoomClampResult::None;
}

// Clamps the pan offsets to the range the CURRENT view mode + zoom allow.
// Must be called after EVERY zoom mutation: shrinking the rendered image
// shrinks the legal offset range, so a stale offset left over from an earlier
// pan would park the image off-centre with a black gap on one side.
// Uses the same GetRenderSize() as the renderer, so it is correct in all five
// view modes.
inline void ClampViewportOffset(HWND hWnd) {
    if (app.imgWidth <= 0 || app.imgHeight <= 0) return;

    RECT rc;
    if (!GetClientRect(hWnd, &rc)) return;
    const float winW = static_cast<float>(rc.right - rc.left);
    const float winH = static_cast<float>(rc.bottom - rc.top);
    if (winW <= 0.0f || winH <= 0.0f) return;

    float renderW = 0.0f, renderH = 0.0f;
    GetRenderSize(winW, winH,
                  static_cast<float>(app.imgWidth), static_cast<float>(app.imgHeight),
                  app.viewMode, app.viewport.zoom, renderW, renderH);

    const float maxOffX = std::max(0.0f, (renderW - winW) / 2.0f);
    const float maxOffY = std::max(0.0f, (renderH - winH) / 2.0f);
    app.viewport.offsetX = std::clamp(app.viewport.offsetX, -maxOffX, maxOffX);
    app.viewport.offsetY = std::clamp(app.viewport.offsetY, -maxOffY, maxOffY);
}
