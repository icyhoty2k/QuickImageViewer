#include "Command.h"
#include "../AppState.h"
#include "../Overlays/OverlayManager.h"
#include "../Platform/Constants.h"
#include "../Platform/ConstantsStrings.h"
#include "../Platform/FileHandler.h"
#include "../Persistence/RegistryManager.h"
#include "../UI/ThumbnailPanels/CacheWnd.h"
#include "../UI/ThumbnailPanels/DirWnd.h"
#include "../UI/FloatingPanels/HistoryListWnd.h"
#include "../UI/FloatingPanels/HelpWnd.h"
#include "../UI/FloatingPanels/StatsWnd.h"
#include "../UI/ThemedDialog.h"
#include "../CMDArgs.h"
#include <algorithm>
#include <filesystem>
#include <numeric>
#include <random>
#include <cmath>
#include <commdlg.h>
#include <shlobj_core.h>
#include <shtypes.h>
#include "AppCommands.h"
#include "UIManager.h"

// These two functions live in AppMain.cpp.
// Declared here (not in a header) to keep them package-private.


extern AppState app;

// Snaps the window to a half of the work area on its current monitor.
// zone: 0=left  1=right  2=top  3=bottom
static void SnapWindowToZone(HWND hWnd, int zone) {
    HMONITOR hMon = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {sizeof(mi)};
    if (!GetMonitorInfo(hMon, &mi)) return;
    const RECT &wa = mi.rcWork;
    int halfW = (wa.right - wa.left) / 2;
    int halfH = (wa.bottom - wa.top) / 2;
    RECT t;
    switch (zone) {
        case 0: t = {wa.left, wa.top, wa.left + halfW, wa.bottom};
            break; // left half
        case 1: t = {wa.left + halfW, wa.top, wa.right, wa.bottom};
            break; // right half
        case 2: t = {wa.left, wa.top, wa.right, wa.top + halfH};
            break; // top half
        case 3: t = {wa.left, wa.top + halfH, wa.right, wa.bottom};
            break; // bottom half
        case 4: t = {wa.left, wa.top, wa.left + halfW, wa.top + halfH};
            break; // top-left quarter
        case 5: t = {wa.left + halfW, wa.top, wa.right, wa.top + halfH};
            break; // top-right quarter
        case 6: t = {wa.left, wa.top + halfH, wa.left + halfW, wa.bottom};
            break; // bottom-left quarter
        case 7: t = {wa.left + halfW, wa.top + halfH, wa.right, wa.bottom};
            break; // bottom-right quarter
        default: return;
    }
    SetWindowPos(hWnd, nullptr, t.left, t.top,
                 t.right - t.left, t.bottom - t.top,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    InvalidateRect(hWnd, nullptr, FALSE);
}

// Clamps viewport offsets to the maximum range the renderer allows for the
// current view mode and zoom — mirrors the identical logic in MouseHandler.
static void ClampViewportOffset(HWND hWnd) {
    if (app.imgWidth <= 0 || app.imgHeight <= 0) return;
    RECT rc;
    GetClientRect(hWnd, &rc);
    float winW = static_cast<float>(rc.right - rc.left);
    float winH = static_cast<float>(rc.bottom - rc.top);
    float imgW = static_cast<float>(app.imgWidth);
    float imgH = static_cast<float>(app.imgHeight);
    float ratioX = winW / imgW, ratioY = winH / imgH;
    float renderW, renderH;
    switch (app.viewMode) {
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
            renderW = imgW;
            renderH = imgH;
            break;
        case Constants::ViewModes::ViewMode::FitToView_PreserveAspectRatio:
        default:
            renderW = imgW * std::min(ratioX, ratioY);
            renderH = imgH * std::min(ratioX, ratioY);
            break;
    }
    const float z = (app.viewport.zoom <= 0.0f) ? 1.0f : app.viewport.zoom;
    renderW *= z;
    renderH *= z;
    float maxOffX = std::max(0.0f, (renderW - winW) / 2.0f);
    float maxOffY = std::max(0.0f, (renderH - winH) / 2.0f);
    app.viewport.offsetX = std::clamp(app.viewport.offsetX, -maxOffX, maxOffX);
    app.viewport.offsetY = std::clamp(app.viewport.offsetY, -maxOffY, maxOffY);
}

// =============================================================================
// handleKeyboard — public entry point called from WM_KEYDOWN
// =============================================================================
void InputManager::handleKeyboard(HWND hWnd, WPARAM wParam, LPARAM lParam) {
    Command cmd = ResolveKeyboardKeys(static_cast<UINT>(wParam), lParam);
    if (cmd != Command::None) {
        ExecuteCommand(hWnd, cmd);
    }
}

// =============================================================================
// ExecuteCommand — runs a fully-resolved Command and applies all side effects.
// Call from any input path (keyboard, mouse click, tray) to guarantee identical
// behavior regardless of how the action was triggered.
// =============================================================================
void InputManager::ExecuteCommand(HWND hWnd, Command cmd) {
    // Direct transition pick — handled ahead of the switch because it is a
    // contiguous RANGE of commands (one per TransitionType), not discrete cases.
    static_assert(static_cast<int>(Command::SetTransitionLast) -
                  static_cast<int>(Command::SetTransitionFirst) + 1 ==
                  Constants::Slideshow::TRANSITION_COUNT,
                  "SetTransition command range must cover every TransitionType");
    if (cmd >= Command::SetTransitionFirst && cmd <= Command::SetTransitionLast) {
        const int t = static_cast<int>(cmd) - static_cast<int>(Command::SetTransitionFirst);
        auto &tr = app.slideshow.transition;

        // The same menu row does two jobs, decided by the current source:
        //   LIST → tick / untick membership of the custom pool
        //   else → pick THE transition, which implies source NONE
        if (tr.source == Constants::Slideshow::TransitionSource::LIST) {
            tr.listMask ^= (1u << t);
            Persistence::Registry::SaveSetting(Constants::Registry::SLIDESHOW_TRANS_LIST,
                                               static_cast<DWORD>(tr.listMask));
            const bool on = (tr.listMask & (1u << t)) != 0u;
            g_overlayManager.PostCenterMessage(hWnd,
                std::wstring(Constants::Messages::TRANSITION_NAMES[t]) +
                (on ? Constants::Messages::STATE_ON_SUFFIX
                    : Constants::Messages::STATE_OFF_SUFFIX));
            if (tr.listMask == 0u)
                g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::TRANSITION_LIST_EMPTY);
            return;
        }

        tr.type = static_cast<TransitionType>(t);
        tr.source = Constants::Slideshow::TransitionSource::NONE;
        Persistence::Registry::SaveSetting(Constants::Registry::SLIDESHOW_TRANSITION,
                                           static_cast<DWORD>(t));
        Persistence::Registry::SaveSetting(Constants::Registry::SLIDESHOW_TRANS_SOURCE,
                                           static_cast<DWORD>(Constants::Slideshow::TransitionSource::NONE));
        g_overlayManager.PostCenterMessage(hWnd,
            std::wstring(Constants::Messages::TRANSITION_PREFIX) +
            Constants::Messages::TRANSITION_NAMES[t]);
        return;
    }

    switch (cmd) {
        // -----------------------------------------------------------------------
        // Navigation
        // -----------------------------------------------------------------------
        case Command::NextImage:
            if (!app.playlist.empty()) {
                int size = static_cast<int>(app.playlist.size());
                LoadImageIndex(hWnd, (app.currentIndex + 1) % size);
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            break;

        case Command::PrevImage:
            if (!app.playlist.empty()) {
                int size = static_cast<int>(app.playlist.size());
                LoadImageIndex(hWnd, (app.currentIndex - 1 + size) % size);
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            break;

        case Command::ToggleLastDir: {
            const auto &history = UI::GetFolderHistory();
            if (history.size() < 2) {
                g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::TOGGLE_DIR_NO_PREV);
            } else {
                std::wstring prevDir = history[1];
                std::error_code ec;
                if (!std::filesystem::is_directory(prevDir, ec) || ec) {
                    g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::TOGGLE_DIR_MISSING);
                    break;
                }
                OpenDirectory(hWnd, prevDir);
                g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::TOGGLE_DIR_CHANGED + prevDir);
            }
            break;
        }

        case Command::ToggleLastImage: {
            if (app.previousImageIndex < 0 ||
                app.previousImageIndex >= static_cast<int>(app.playlist.size())) {
                g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::TOGGLE_IMAGE_NO_PREV);
                break;
            }
            int target = app.previousImageIndex;
            const std::wstring &path = app.playlist[target];
            std::error_code ec;
            if (!std::filesystem::is_regular_file(path, ec) || ec) {
                g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::TOGGLE_IMAGE_MISSING);
                app.previousImageIndex = -1;
                break;
            }
            LoadImageIndex(hWnd, target);
            size_t slash = path.find_last_of(L"\\/");
            std::wstring name = (slash == std::wstring::npos) ? path : path.substr(slash + 1);
            g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::TOGGLE_IMAGE_CHANGED + name);
            break;
        }

        case Command::ShowInExplorer:
            if (!app.playlist.empty() && app.currentIndex >= 0) {
                const std::wstring &path = app.playlist[app.currentIndex];
                PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(path.c_str());
                if (pidl) {
                    SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
                    ILFree(pidl);
                }
            }
            break;

        // -----------------------------------------------------------------------
        // View modes
        // -----------------------------------------------------------------------
        case Command::ViewMode1:
        case Command::ViewMode2:
        case Command::ViewMode3:
        case Command::ViewMode4:
        case Command::ViewMode5: {
            int modeNum = static_cast<int>(cmd) - static_cast<int>(Command::ViewMode1) + 1;
            app.viewMode = static_cast<Constants::ViewModes::ViewMode>(modeNum);
            Persistence::Registry::SaveSetting(Constants::Registry::VIEW_MODE,
                                               static_cast<DWORD>(modeNum));
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }

        // -----------------------------------------------------------------------
        // Zoom
        // -----------------------------------------------------------------------
        case Command::ZoomIn:
            app.viewport.zoom *= Constants::ZOOM_STEP;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;

        case Command::ZoomOut:
            app.viewport.zoom /= Constants::ZOOM_STEP;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;

        case Command::ZoomReset:
            app.viewport.zoom = 1.0f;
            app.viewport.offsetX = 0.0f;
            app.viewport.offsetY = 0.0f;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;

        // -----------------------------------------------------------------------
        // Transform
        // -----------------------------------------------------------------------
        case Command::RotateCW:
            app.viewport.rotation = (app.viewport.rotation + 90) % 360;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;

        case Command::RotateCCW:
            app.viewport.rotation = (app.viewport.rotation - 90 + 360) % 360;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;

        case Command::FlipH:
            app.viewport.flippedH = !app.viewport.flippedH;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;

        case Command::ToggleThumbnailWrapAround:
            app.thumbnailPanelWheelWrapAround = !app.thumbnailPanelWheelWrapAround;
            g_overlayManager.PostCenterMessage(hWnd,
                                               app.thumbnailPanelWheelWrapAround
                                                   ? Constants::Messages::THUMB_STRIP_WRAP_ON
                                                   : Constants::Messages::THUMB_STRIP_WRAP_OFF);
            break;

        case Command::ToggleThumbnailEffects:
            app.thumbnailEffectsEnabled = !app.thumbnailEffectsEnabled;
            Persistence::Registry::SaveSetting(Constants::Registry::THUMBNAIL_EFFECTS,
                                               static_cast<DWORD>(app.thumbnailEffectsEnabled));
            g_overlayManager.PostCenterMessage(hWnd,
                                               app.thumbnailEffectsEnabled
                                                   ? Constants::Messages::THUMB_EFFECTS_ON
                                                   : Constants::Messages::THUMB_EFFECTS_OFF);
            uiManager.RepaintAllPanels();
            break;

        case Command::FlipV:
            app.viewport.flippedV = !app.viewport.flippedV;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;

        // -----------------------------------------------------------------------
        // Fullscreen
        // -----------------------------------------------------------------------
        case Command::ToggleFullscreen:
            AppCommands::ToggleFullscreen(hWnd);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;

        // -----------------------------------------------------------------------
        // Panels / overlays
        // -----------------------------------------------------------------------
        case Command::ToggleHelp:
            uiManager.Toggle(uiManager.getHelpWindow());
            break;

        case Command::ShowInfo:
            uiManager.Toggle(uiManager.getInfoWindow());
            break;

        case Command::JumpToImage:
            uiManager.ToggleJumpToWindow();
            break;

        case Command::FindImage:
            uiManager.ToggleFindWindow();
            break;

        case Command::ToggleStats:
            uiManager.Toggle(uiManager.getStatsWindow());
            break;

        case Command::OpenFile:
            OpenInitialImage(hWnd);
            break;
        case Command::ReloadCurrentDir:
            ReloadCurrentDirectory(hWnd);
            g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::RELOAD_CURRENT_DIR_MSG);
            break;


        case Command::ToggleCache: {
            UI::CacheWnd &cacheWnd = uiManager.getCacheWindow();
            const bool cacheWasVisible = cacheWnd.IsVisible();
            if (!cacheWasVisible) {
                const UI::PanelLayout &layout = uiManager.GetLayout();
                if (!layout.occupied(Constants::CACHE_WINDOW_POSITION))
                    cacheWnd.SetPosition(Constants::CACHE_WINDOW_POSITION);
            }
            uiManager.Toggle(cacheWnd);
            g_overlayManager.PostCenterMessage(hWnd, cacheWasVisible
                                                         ? Constants::Messages::CACHE_WINDOW_HIDDEN_MSG
                                                         : Constants::Messages::CACHE_WINDOW_VISIBLE_MSG);
            break;
        }

        case Command::ClearCache:
            uiManager.getCacheWindow().ClearThumbnailCache();
            g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::CACHE_WINDOW_CLEAR_CACHE_MSG);

            break;

        case Command::ToggleDir: {
            UI::DirWnd &dirWnd = uiManager.getDirWindow();
            const bool dirWasVisible = dirWnd.IsVisible();
            if (!dirWasVisible) {
                const UI::PanelLayout &layout = uiManager.GetLayout();
                if (!layout.occupied(Constants::CURRENT_DIR_WINDOW_POSITION))
                    dirWnd.SetPosition(Constants::CURRENT_DIR_WINDOW_POSITION);
            }
            uiManager.Toggle(dirWnd);
            g_overlayManager.PostCenterMessage(hWnd, dirWasVisible
                                                         ? Constants::Messages::DIR_WINDOW_HIDDEN_MSG
                                                         : Constants::Messages::DIR_WINDOW_VISIBLE_MSG);
            break;
        }

        case Command::ToggleHistory:
            uiManager.Toggle(uiManager.getHistoryListWindow());
            break;

        case Command::ToggleHistoryFull:
            UI::ToggleHistoryFull();
            break;

        // ── Cycle overlay layout mode (O) ────────────────────────────────────
        case Command::CycleOverlayLayout: {
            int &mode = Constants::Overlay::OVERLAY_LAYOUT_MODE;
            mode = (mode + 1) % 3;
            g_overlayManager.OnLayoutModeChanged(hWnd);
            const wchar_t *labels[] = {Constants::Messages::LAYOUT_GRID, Constants::Messages::LAYOUT_STACKED, Constants::Messages::LAYOUT_SUMMARY};
            g_overlayManager.PostCenterMessage(hWnd, labels[mode]);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }

        // ── Toggle overlay background (P) ─────────────────────────────────────
        case Command::ToggleOverlayBackground: {
            app.overlayShowBackground = !app.overlayShowBackground;
            Persistence::Registry::SaveSetting(Constants::Registry::OVERLAY_SHOW_BG,
                                               static_cast<DWORD>(app.overlayShowBackground));
            g_overlayManager.PostCenterMessage(hWnd,
                                               app.overlayShowBackground ? Constants::Messages::OVERLAY_BG_ON : Constants::Messages::OVERLAY_BG_OFF);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }

        // ── Master overlay toggle (N / I / Ctrl+0) ───────────────────────────
        case Command::ToggleOverlay: {
            app.showOverlayInfoText = !app.showOverlayInfoText;
            Persistence::Registry::SaveSetting(Constants::Registry::OVERLAY_VISIBLE,
                                               static_cast<DWORD>(app.showOverlayInfoText));
            g_overlayManager.SetAllVisible(app.showOverlayInfoText);
            // Always post the state change to center-center — it survives the hide
            // because MID_CENTER is independently controlled by PostCenterMessage.
            g_overlayManager.PostCenterMessage(hWnd,
                                               app.showOverlayInfoText ? Constants::Messages::INFO_PANELS_ON : Constants::Messages::INFO_PANELS_OFF);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }

        // ── Per-slot visibility toggles (Ctrl+1..9) ──────────────────────────
        case Command::ToggleOverlaySlot1: {
            bool now = !g_overlayManager.IsSlotVisible(OverlayManager::TOP_LEFT);
            g_overlayManager.SetSlotVisible(OverlayManager::TOP_LEFT, now);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }
        case Command::ToggleOverlaySlot2: {
            bool now = !g_overlayManager.IsSlotVisible(OverlayManager::TOP_CENTER);
            g_overlayManager.SetSlotVisible(OverlayManager::TOP_CENTER, now);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }
        case Command::ToggleOverlaySlot3: {
            bool now = !g_overlayManager.IsSlotVisible(OverlayManager::TOP_RIGHT);
            g_overlayManager.SetSlotVisible(OverlayManager::TOP_RIGHT, now);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }
        case Command::ToggleOverlaySlot4: {
            bool now = !g_overlayManager.IsSlotVisible(OverlayManager::MID_LEFT);
            g_overlayManager.SetSlotVisible(OverlayManager::MID_LEFT, now);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }
        case Command::ToggleOverlaySlot5: {
            // MID_CENTER — independent toggle; no center message for its own toggle
            bool now = !g_overlayManager.IsSlotVisible(OverlayManager::MID_CENTER);
            g_overlayManager.SetSlotVisible(OverlayManager::MID_CENTER, now);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }
        case Command::ToggleOverlaySlot6: {
            bool now = !g_overlayManager.IsSlotVisible(OverlayManager::MID_RIGHT);
            g_overlayManager.SetSlotVisible(OverlayManager::MID_RIGHT, now);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }
        case Command::ToggleOverlaySlot7: {
            bool now = !g_overlayManager.IsSlotVisible(OverlayManager::BOT_LEFT);
            g_overlayManager.SetSlotVisible(OverlayManager::BOT_LEFT, now);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }
        case Command::ToggleOverlaySlot8: {
            bool now = !g_overlayManager.IsSlotVisible(OverlayManager::BOT_CENTER);
            g_overlayManager.SetSlotVisible(OverlayManager::BOT_CENTER, now);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }
        case Command::ToggleOverlaySlot9: {
            bool now = !g_overlayManager.IsSlotVisible(OverlayManager::BOT_RIGHT);
            g_overlayManager.SetSlotVisible(OverlayManager::BOT_RIGHT, now);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }

        // ── Per-slot compact-mode toggles (Ctrl+Alt+1..9) ────────────────────
        case Command::CompactOverlaySlot1:
            g_overlayManager.ToggleCompactMode(OverlayManager::TOP_LEFT);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        case Command::CompactOverlaySlot2:
            g_overlayManager.ToggleCompactMode(OverlayManager::TOP_CENTER);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        case Command::CompactOverlaySlot3:
            g_overlayManager.ToggleCompactMode(OverlayManager::TOP_RIGHT);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        case Command::CompactOverlaySlot4:
            g_overlayManager.ToggleCompactMode(OverlayManager::MID_LEFT);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        case Command::CompactOverlaySlot5:
            // no-op: MID_CENTER is always single-line
            break;
        case Command::CompactOverlaySlot6:
            g_overlayManager.ToggleCompactMode(OverlayManager::MID_RIGHT);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        case Command::CompactOverlaySlot7:
            g_overlayManager.ToggleCompactMode(OverlayManager::BOT_LEFT);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        case Command::CompactOverlaySlot8:
            g_overlayManager.ToggleCompactMode(OverlayManager::BOT_CENTER);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        case Command::CompactOverlaySlot9:
            g_overlayManager.ToggleCompactMode(OverlayManager::BOT_RIGHT);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        // -----------------------------------------------------------------------
        // App control
        // -----------------------------------------------------------------------
        case Command::HideToTray: {
            uiManager.HideAllPanelWindows();
            if (!app.isKeepInBackground || app.GetInstanceCount() > 1) {
                AppCommands::RemoveTrayIcon(hWnd);
                DestroyWindow(hWnd);
            } else {
                AppCommands::AddTrayIcon(hWnd);
                ShowWindow(hWnd, SW_HIDE);
            }
            break;
        }
        case Command::NewWindow: {
            std::wstring exePath = Persistence::Registry::GetExePathW();
            if (!exePath.empty()) {
                SetEnvironmentVariableW(L"QIV_NEW_INSTANCE", L"1");
                ShellExecuteW(nullptr, L"open", exePath.c_str(), nullptr, nullptr, SW_SHOW);
                SetEnvironmentVariableW(L"QIV_NEW_INSTANCE", nullptr);
            }
            break;
        }

        case Command::CloseAllPanels:
            uiManager.HideAllPanelWindows();
            InvalidateRect(hWnd, nullptr, FALSE);
            break;

        case Command::RestoreAllPanels:
            uiManager.RestoreAllPanels();
            InvalidateRect(hWnd, nullptr, FALSE);
            break;

        case Command::ToggleAllPanels:
            if (uiManager.AnyPanelVisible())
                uiManager.HideAllPanelWindows();
            else
                uiManager.RestoreAllPanels();
            InvalidateRect(hWnd, nullptr, FALSE);
            break;

        case Command::HardQuit:
            AppCommands::RemoveTrayIcon(hWnd);
            DestroyWindow(hWnd);
            break;

        case Command::ResetAll:
            AppCommands::ResetWindowLayoutAndEffects(hWnd);
            g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::RESET_TO_DEFAULTS);
            break;

        // -----------------------------------------------------------------------
        // Color effect toggles
        // -----------------------------------------------------------------------
        case Command::ToggleEffectPreview:
            app.effectPreviewEnabled = !app.effectPreviewEnabled;
            app.UpdateRendererColorEffects(hWnd);
            g_overlayManager.UpdateEffects();
            break;

        case Command::ToggleGrayscale:
            app.ToggleEffectChronological(Constants::Strings::EFFECT_GRAYSCALE);
            app.WakeUpAndApplyEffects(hWnd, app.effectGrayscale);
            break;

        case Command::ToggleInvert:
            app.ToggleEffectChronological(Constants::Strings::EFFECT_INVERT);
            app.WakeUpAndApplyEffects(hWnd, app.effectInvert);
            break;

        case Command::ToggleSepia:
            app.ToggleEffectChronological(Constants::Strings::EFFECT_SEPIA);
            app.WakeUpAndApplyEffects(hWnd, app.effectSepia);
            break;

        case Command::ToggleSolarize:
            app.ToggleEffectChronological(Constants::Strings::EFFECT_SOLARIZE);
            app.WakeUpAndApplyEffects(hWnd, app.effectSolarize);
            break;

        case Command::ToggleOutline:
            app.ToggleEffectChronological(Constants::Strings::EFFECT_OUTLINE);
            app.WakeUpAndApplyEffects(hWnd, app.effectOutline);
            break;

        case Command::ToggleThreshold:
            app.ToggleEffectChronological(Constants::Strings::EFFECT_THRESHOLD);
            app.WakeUpAndApplyEffects(hWnd, app.effectThreshold);
            break;


        // -----------------------------------------------------------------------
        // Continuous adjustments
        // -----------------------------------------------------------------------
        case Command::GammaUp:
            app.gamma = std::min(Constants::MAX_GAMMA, app.gamma + Constants::GAMMA_STEP);
            app.WakeUpAndApplyEffects(hWnd);
            break;

        case Command::GammaDown:
            app.gamma = std::max(Constants::MIN_GAMMA, app.gamma - Constants::GAMMA_STEP);
            app.WakeUpAndApplyEffects(hWnd);
            break;

        case Command::BrightnessUp:
            app.brightness = std::clamp(
                    app.brightness + Constants::COLOR_ADJUST_STEP,
                    -Constants::MIN_MAX_BRIGHTNESS, Constants::MIN_MAX_BRIGHTNESS);
            app.WakeUpAndApplyEffects(hWnd);
            break;

        case Command::BrightnessDown:
            app.brightness = std::clamp(
                    app.brightness - Constants::COLOR_ADJUST_STEP,
                    -Constants::MIN_MAX_BRIGHTNESS, Constants::MIN_MAX_BRIGHTNESS);
            app.WakeUpAndApplyEffects(hWnd);
            break;

        case Command::ContrastUp:
            app.contrast = std::clamp(
                    app.contrast + Constants::COLOR_ADJUST_STEP,
                    0.0f, Constants::MIN_MAX_CONTRAST);
            app.WakeUpAndApplyEffects(hWnd);
            break;

        case Command::ContrastDown:
            app.contrast = std::clamp(
                    app.contrast - Constants::COLOR_ADJUST_STEP,
                    0.0f, Constants::MIN_MAX_CONTRAST);
            app.WakeUpAndApplyEffects(hWnd);
            break;

        case Command::SaturationUp:
            app.saturation = std::min(
                    Constants::MIN_MAX_SATURATION, app.saturation + Constants::COLOR_ADJUST_STEP);
            app.WakeUpAndApplyEffects(hWnd);
            break;

        case Command::SaturationDown:
            app.saturation = std::max(0.0f, app.saturation - Constants::COLOR_ADJUST_STEP);
            app.WakeUpAndApplyEffects(hWnd);
            break;

        // -----------------------------------------------------------------------
        // Save / reset
        // -----------------------------------------------------------------------
        case Command::ResetEffects:
            app.ResetEffects();
            app.UpdateRendererColorEffects(hWnd);
            g_overlayManager.UpdateEffects();
            g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::ALL_EFFECTS_RESET);
            break;

        case Command::SaveImage: {
            AppCommands::SaveImageToDisk(hWnd);
            break;
        }

        case Command::CopyToClipboard:
            AppCommands::CopyImageToClipboard(hWnd);
            break;

        // ── Dedicated instances ──────────────────────────────────────────────
        case Command::ToggleDedicatedPanel:
            uiManager.Toggle(uiManager.getDedicatedWindow());
            break;

        case Command::ToggleDedicated:
            // Runtime flag only — it selects the registry/history namespace, so
            // flipping it mid-session affects subsequent reads/writes and any
            // cmdArgs generated from here. Existing instances are untouched.
            app.isDedicated = !app.isDedicated;
            g_overlayManager.PostCenterMessage(hWnd,
                std::wstring(L"Dedicated") +
                (app.isDedicated ? Constants::Messages::STATE_ON_SUFFIX
                                 : Constants::Messages::STATE_OFF_SUFFIX));
            break;

        case Command::CmdArgsExport:
            ExportCmdArgsFile(hWnd);
            break;

        case Command::CmdArgsImport:
            ImportCmdArgsFile(hWnd);
            break;

        case Command::CmdArgsGenerateShortcut:
            CreateCmdArgsShortcut(hWnd);
            break;

        case Command::CmdArgsTest:
            TestCmdArgsFile(hWnd);
            break;

        // Wallpaper styles — the enum order mirrors Constants::Wallpaper::FILL..SPAN,
        // so the offset is the position index. Same trick as ViewMode1..5 above.
        case Command::SetWallpaperFill:
        case Command::SetWallpaperFit:
        case Command::SetWallpaperStretch:
        case Command::SetWallpaperTile:
        case Command::SetWallpaperCenter:
        case Command::SetWallpaperSpan:
            AppCommands::SetDesktopWallpaper(hWnd,
                static_cast<int>(cmd) - static_cast<int>(Command::SetWallpaperFill));
            break;
        case Command::ToggleFirstLastImageInCurrentFolder: {
            if (app.playlist.empty()) return;

            int total = static_cast<int>(app.playlist.size() - 1);
            int distToStart = app.currentIndex;
            int distToEnd = total - app.currentIndex;

            // Use the further endpoint as the target
            int targetIndex = (distToStart <= distToEnd) ? total : 0;
            LoadImageIndex(hWnd, targetIndex);

            g_overlayManager.PostCenterMessage(hWnd, std::wstring(
                                                       (targetIndex == 0)
                                                           ? Constants::Messages::TOGGLE_FIRST_IMAGE_IN_FOLDER + std::to_wstring(1)
                                                           : Constants::Messages::TOGGLE_LAST_IMAGE_IN_FOLDER + std::to_wstring(targetIndex + 1)));
            if (distToStart != 0 && distToStart != total) app.lastImageBeforeToggleFirstLastImageInCurrentFolder = distToStart;
            break;
        }
        case Command::GoToLastImageInCurrentFolder: {
            if (app.playlist.empty() || app.currentIndex == app.lastImageBeforeToggleFirstLastImageInCurrentFolder) return;
            LoadImageIndex(hWnd, app.lastImageBeforeToggleFirstLastImageInCurrentFolder);
            g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::GO_TO_LAST_IMAGE_BEFORE_TOGGLE
                                                     + std::to_wstring(app.lastImageBeforeToggleFirstLastImageInCurrentFolder + 1));
            break;
        }

        // -----------------------------------------------------------------------
        // Runtime theme factor  (Ctrl+Alt+Shift+Numpad+/-/0)
        // -----------------------------------------------------------------------
        case Command::ThemeFactorUp:
            AppCommands::changeAppThemeFactor(hWnd, app.themeFactor + Constants::Theme::THEME_FACTOR_STEP);
            Persistence::Registry::SaveSetting(Constants::Registry::THEME_FACTOR,
                                               static_cast<DWORD>(std::round(app.themeFactor * 100.0f)));
            g_overlayManager.PostCenterMessage(hWnd,
                                               Constants::Messages::THEME_FACTOR_PREFIX +
                                               std::to_wstring(static_cast<int>(std::round(app.themeFactor * 100))) + L"%");
            break;

        case Command::ThemeFactorDown:
            AppCommands::changeAppThemeFactor(hWnd, app.themeFactor - Constants::Theme::THEME_FACTOR_STEP);
            Persistence::Registry::SaveSetting(Constants::Registry::THEME_FACTOR,
                                               static_cast<DWORD>(std::round(app.themeFactor * 100.0f)));
            g_overlayManager.PostCenterMessage(hWnd,
                                               Constants::Messages::THEME_FACTOR_PREFIX +
                                               std::to_wstring(static_cast<int>(std::round(app.themeFactor * 100))) + L"%");
            break;

        case Command::ThemeFactorReset:
            AppCommands::changeAppThemeFactor(hWnd, Constants::Theme::DEFAULT_THEME_FACTOR);
            Persistence::Registry::SaveSetting(Constants::Registry::THEME_FACTOR,
                                               static_cast<DWORD>(std::round(app.themeFactor * 100.0f)));
            g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::THEME_FACTOR_RESET_MSG);
            break;

        // -----------------------------------------------------------------------
        // Window chrome  (Ctrl+Shift+Numpad* / Numpad/)
        // -----------------------------------------------------------------------
        case Command::ToggleCornerPreference:
            AppCommands::changeAppCornerPreference(hWnd,
                                                   app.cornerPreference == DWMWCP_ROUND ? DWMWCP_DONOTROUND : DWMWCP_ROUND);
            g_overlayManager.PostCenterMessage(hWnd,
                                               app.cornerPreference == DWMWCP_ROUND
                                                   ? Constants::Messages::CORNER_ROUND
                                                   : Constants::Messages::CORNER_SQUARE);
            break;

        case Command::CycleBackdropType:
            AppCommands::changeAppBackdropType(hWnd, (app.backdropType + 1) % 4);
            {
                constexpr const wchar_t *labels[] = {
                    Constants::Messages::BACKDROP_NONE,
                    Constants::Messages::BACKDROP_MICA,
                    Constants::Messages::BACKDROP_ACRYLIC,
                    Constants::Messages::BACKDROP_MICA_ALT
                };
                g_overlayManager.PostCenterMessage(hWnd, labels[app.backdropType]);
            }
            break;

        case Command::ToggleAlwaysOnTop:
            app.isAlwaysOnTop = !app.isAlwaysOnTop;
            SetWindowPos(hWnd,
                         app.isAlwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST,
                         0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            uiManager.ApplyAlwaysOnTop(app.isAlwaysOnTop);
            g_overlayManager.PostCenterMessage(hWnd,
                                               app.isAlwaysOnTop
                                                   ? Constants::Messages::ALWAYS_ON_TOP_ON
                                                   : Constants::Messages::ALWAYS_ON_TOP_OFF);
            break;

        // -----------------------------------------------------------------------
        // Sort order  (Ctrl+Alt+Shift+0/6/7/8/9)
        // First press:  sets the sort mode ascending (reverse = false)
        // Second press: toggles to descending (reverse = true), third press back, etc.
        // -----------------------------------------------------------------------
        case Command::SortByName: {
            if (app.fileHandlerDefaultSortOrder == 0)
                app.fileHandlerIsReverseSortOrder = !app.fileHandlerIsReverseSortOrder;
            else {
                app.fileHandlerDefaultSortOrder = 0;
                app.fileHandlerIsReverseSortOrder = false;
            }
            Persistence::Registry::SaveSetting(Constants::Registry::SORT_ORDER,   static_cast<DWORD>(app.fileHandlerDefaultSortOrder));
            Persistence::Registry::SaveSetting(Constants::Registry::SORT_REVERSE, static_cast<DWORD>(app.fileHandlerIsReverseSortOrder));
            ReSortPlaylistAndRebuildMap(hWnd);
            g_overlayManager.PostCenterMessage(hWnd, app.fileHandlerIsReverseSortOrder
                                                         ? Constants::Messages::SORT_BY_NAME_REV
                                                         : Constants::Messages::SORT_BY_NAME);
            break;
        }

        case Command::SortByDate: {
            if (app.fileHandlerDefaultSortOrder == 1)
                app.fileHandlerIsReverseSortOrder = !app.fileHandlerIsReverseSortOrder;
            else {
                app.fileHandlerDefaultSortOrder = 1;
                app.fileHandlerIsReverseSortOrder = false;
            }
            Persistence::Registry::SaveSetting(Constants::Registry::SORT_ORDER,   static_cast<DWORD>(app.fileHandlerDefaultSortOrder));
            Persistence::Registry::SaveSetting(Constants::Registry::SORT_REVERSE, static_cast<DWORD>(app.fileHandlerIsReverseSortOrder));
            ReSortPlaylistAndRebuildMap(hWnd);
            g_overlayManager.PostCenterMessage(hWnd, app.fileHandlerIsReverseSortOrder
                                                         ? Constants::Messages::SORT_BY_DATE_REV
                                                         : Constants::Messages::SORT_BY_DATE);
            break;
        }

        case Command::SortBySize: {
            if (app.fileHandlerDefaultSortOrder == 2)
                app.fileHandlerIsReverseSortOrder = !app.fileHandlerIsReverseSortOrder;
            else {
                app.fileHandlerDefaultSortOrder = 2;
                app.fileHandlerIsReverseSortOrder = false;
            }
            Persistence::Registry::SaveSetting(Constants::Registry::SORT_ORDER,   static_cast<DWORD>(app.fileHandlerDefaultSortOrder));
            Persistence::Registry::SaveSetting(Constants::Registry::SORT_REVERSE, static_cast<DWORD>(app.fileHandlerIsReverseSortOrder));
            ReSortPlaylistAndRebuildMap(hWnd);
            g_overlayManager.PostCenterMessage(hWnd, app.fileHandlerIsReverseSortOrder
                                                         ? Constants::Messages::SORT_BY_SIZE_REV
                                                         : Constants::Messages::SORT_BY_SIZE);
            break;
        }

        case Command::SortByType: {
            if (app.fileHandlerDefaultSortOrder == 3)
                app.fileHandlerIsReverseSortOrder = !app.fileHandlerIsReverseSortOrder;
            else {
                app.fileHandlerDefaultSortOrder = 3;
                app.fileHandlerIsReverseSortOrder = false;
            }
            Persistence::Registry::SaveSetting(Constants::Registry::SORT_ORDER,   static_cast<DWORD>(app.fileHandlerDefaultSortOrder));
            Persistence::Registry::SaveSetting(Constants::Registry::SORT_REVERSE, static_cast<DWORD>(app.fileHandlerIsReverseSortOrder));
            ReSortPlaylistAndRebuildMap(hWnd);
            g_overlayManager.PostCenterMessage(hWnd, app.fileHandlerIsReverseSortOrder
                                                         ? Constants::Messages::SORT_BY_TYPE_REV
                                                         : Constants::Messages::SORT_BY_TYPE);
            break;
        }

        case Command::SortByDisk: {
            // Disk order has no meaningful reverse — pressing again is a no-op toggle
            app.fileHandlerDefaultSortOrder = 4;
            app.fileHandlerIsReverseSortOrder = false;
            Persistence::Registry::SaveSetting(Constants::Registry::SORT_ORDER,   static_cast<DWORD>(app.fileHandlerDefaultSortOrder));
            Persistence::Registry::SaveSetting(Constants::Registry::SORT_REVERSE, static_cast<DWORD>(app.fileHandlerIsReverseSortOrder));
            ReSortPlaylistAndRebuildMap(hWnd);
            g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::SORT_BY_DISK);
            break;
        }

        case Command::SlideshowToggle: {
            bool wasRunning = app.slideshow.running;
            AppCommands::toggleSlideshow(hWnd);
            if (!wasRunning) {
                std::wstring msg = std::wstring(Constants::Messages::SLIDESHOW_PLAYING)
                                   + L"  " + std::to_wstring(app.slideshow.intervalMs / 1000) + L"s"
                                   + (app.slideshow.loop ? L"  Loop" : L"")
                                   + (app.slideshow.shuffle ? L"  Shuffle" : L"");
                g_overlayManager.PostCenterMessage(hWnd, msg);
            } else {
                g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::SLIDESHOW_STOPPED);
            }
            break;
        }

        case Command::SlideshowPauseResume: {
            bool wasPaused = app.slideshow.paused;
            AppCommands::pauseResumeSlideshow(hWnd);
            g_overlayManager.PostCenterMessage(hWnd,
                                               wasPaused
                                                   ? Constants::Messages::SLIDESHOW_PLAYING
                                                   : Constants::Messages::SLIDESHOW_PAUSED);
            break;
        }

        case Command::SlideshowToggleLoop:
            app.slideshow.loop = !app.slideshow.loop;
            Persistence::Registry::SaveSetting(Constants::Registry::SLIDESHOW_LOOP,
                                               static_cast<DWORD>(app.slideshow.loop));
            g_overlayManager.PostCenterMessage(hWnd,
                                               app.slideshow.loop
                                                   ? Constants::Messages::SLIDESHOW_LOOP_ON
                                                   : Constants::Messages::SLIDESHOW_LOOP_OFF);
            break;

        case Command::SlideshowSetInterval: {
            int v = UI::ThemedDialog::PromptInt(hWnd, L"Slideshow Interval",
                                                L"Time between slides in ms (100 – 60000):",
                                                app.slideshow.intervalMs, 100, 60000,
                                                Constants::Slideshow::IS_INTERVAL_MS);
            if (v >= 0) {
                app.slideshow.intervalMs = v;
                Persistence::Registry::SaveSetting(Constants::Registry::SLIDESHOW_INTERVAL_MS,
                                                   static_cast<DWORD>(v));
                g_overlayManager.PostCenterMessage(hWnd,
                    std::wstring(Constants::Messages::SLIDESHOW_INTERVAL_PREFIX) +
                    std::to_wstring(v) + L" ms");
            }
            break;
        }

        case Command::SetTransitionSourceNone:
        case Command::SetTransitionSourceAll:
        case Command::SetTransitionSourceList: {
            const int src = static_cast<int>(cmd) -
                            static_cast<int>(Command::SetTransitionSourceFirst);
            app.slideshow.transition.source = src;
            app.slideshow.transition.seqIndex = 0; // restart the walk
            Persistence::Registry::SaveSetting(Constants::Registry::SLIDESHOW_TRANS_SOURCE,
                                               static_cast<DWORD>(src));
            g_overlayManager.PostCenterMessage(hWnd,
                std::wstring(Constants::Messages::TRANSITION_SOURCE_PREFIX) +
                Constants::Messages::TRANSITION_SOURCE_NAMES[src]);
            if (src == Constants::Slideshow::TransitionSource::LIST &&
                app.slideshow.transition.listMask == 0u)
                g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::TRANSITION_LIST_EMPTY);
            break;
        }

        case Command::SetTransitionOrderSequential:
        case Command::SetTransitionOrderRandom: {
            const int ord = static_cast<int>(cmd) -
                            static_cast<int>(Command::SetTransitionOrderFirst);
            app.slideshow.transition.order = ord;
            app.slideshow.transition.seqIndex = 0;
            Persistence::Registry::SaveSetting(Constants::Registry::SLIDESHOW_TRANS_ORDER,
                                               static_cast<DWORD>(ord));
            g_overlayManager.PostCenterMessage(hWnd,
                std::wstring(Constants::Messages::TRANSITION_ORDER_PREFIX) +
                Constants::Messages::TRANSITION_ORDER_NAMES[ord]);
            break;
        }


        case Command::SlideshowToggleShuffle: {
            app.slideshow.shuffle = !app.slideshow.shuffle;
            Persistence::Registry::SaveSetting(Constants::Registry::SLIDESHOW_SHUFFLE,
                                               static_cast<DWORD>(app.slideshow.shuffle));
            if (app.slideshow.shuffle && !app.playlist.empty()) {
                int n = static_cast<int>(app.playlist.size());
                app.slideshow.shuffleOrder.resize(n);
                std::iota(app.slideshow.shuffleOrder.begin(), app.slideshow.shuffleOrder.end(), 0);
                std::shuffle(app.slideshow.shuffleOrder.begin(), app.slideshow.shuffleOrder.end(),
                             std::mt19937{std::random_device{}()});
                app.slideshow.shufflePos = 0;
            } else {
                app.slideshow.shuffleOrder.clear();
            }
            g_overlayManager.PostCenterMessage(hWnd,
                                               app.slideshow.shuffle
                                                   ? Constants::Messages::SLIDESHOW_SHUFFLE_ON
                                                   : Constants::Messages::SLIDESHOW_SHUFFLE_OFF);
            break;
        }

        case Command::SlideshowCycleTransition: {
            // Every type is implemented now, so this is a plain wrap-around cycle.
            const int next = (static_cast<int>(app.slideshow.transition.type) + 1) %
                             Constants::Slideshow::TRANSITION_COUNT;
            app.slideshow.transition.type = static_cast<TransitionType>(next);
            Persistence::Registry::SaveSetting(Constants::Registry::SLIDESHOW_TRANSITION,
                                               static_cast<DWORD>(next));
            g_overlayManager.PostCenterMessage(hWnd,
                std::wstring(Constants::Messages::TRANSITION_PREFIX) +
                Constants::Messages::TRANSITION_NAMES[next]);
            break;
        }

        // -----------------------------------------------------------------------
        // Keyboard snap to screen half  (Alt+W/A/S/D)
        // -----------------------------------------------------------------------
        case Command::SnapLeft:
            SnapWindowToZone(hWnd, 0);
            g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::SNAP_LEFT);
            break;
        case Command::SnapRight:
            SnapWindowToZone(hWnd, 1);
            g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::SNAP_RIGHT);
            break;
        case Command::SnapTop:
            SnapWindowToZone(hWnd, 2);
            g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::SNAP_TOP);
            break;
        case Command::SnapBottom:
            SnapWindowToZone(hWnd, 3);
            g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::SNAP_BOTTOM);
            break;

        // -----------------------------------------------------------------------
        // Keyboard snap to screen quarter  (Alt+Q/E/Z/C)
        // -----------------------------------------------------------------------
        case Command::SnapTopLeft:
            SnapWindowToZone(hWnd, 4);
            g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::SNAP_TOP_LEFT);
            break;
        case Command::SnapTopRight:
            SnapWindowToZone(hWnd, 5);
            g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::SNAP_TOP_RIGHT);
            break;
        case Command::SnapBottomLeft:
            SnapWindowToZone(hWnd, 6);
            g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::SNAP_BOTTOM_LEFT);
            break;
        case Command::SnapBottomRight:
            SnapWindowToZone(hWnd, 7);
            g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::SNAP_BOTTOM_RIGHT);
            break;

        // -----------------------------------------------------------------------
        // Viewport pan  (W/A/S/D — DPI-scaled step)
        // Offsets match the mouse-drag convention: positive offsetX → see left side.
        // -----------------------------------------------------------------------
        case Command::PanUp: {
            float step = Constants::KEYBOARD_PAN_STEP * app.dpiScale;
            app.viewport.offsetY += step;
            ClampViewportOffset(hWnd);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }
        case Command::PanDown: {
            float step = Constants::KEYBOARD_PAN_STEP * app.dpiScale;
            app.viewport.offsetY -= step;
            ClampViewportOffset(hWnd);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }
        case Command::PanLeft: {
            float step = Constants::KEYBOARD_PAN_STEP * app.dpiScale;
            app.viewport.offsetX += step;
            ClampViewportOffset(hWnd);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }
        case Command::PanRight: {
            float step = Constants::KEYBOARD_PAN_STEP * app.dpiScale;
            app.viewport.offsetX -= step;
            ClampViewportOffset(hWnd);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }

        // -----------------------------------------------------------------------
        // Window move  (Shift+W/A/S/D — DPI-scaled step)
        // -----------------------------------------------------------------------
        case Command::MoveWindowUp: {
            int step = static_cast<int>(Constants::KEYBOARD_WINDOW_MOVE_STEP * app.dpiScale);
            RECT rc;
            GetWindowRect(hWnd, &rc);
            SetWindowPos(hWnd, nullptr, rc.left, rc.top - step, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            break;
        }
        case Command::MoveWindowDown: {
            int step = static_cast<int>(Constants::KEYBOARD_WINDOW_MOVE_STEP * app.dpiScale);
            RECT rc;
            GetWindowRect(hWnd, &rc);
            SetWindowPos(hWnd, nullptr, rc.left, rc.top + step, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            break;
        }
        case Command::MoveWindowLeft: {
            int step = static_cast<int>(Constants::KEYBOARD_WINDOW_MOVE_STEP * app.dpiScale);
            RECT rc;
            GetWindowRect(hWnd, &rc);
            SetWindowPos(hWnd, nullptr, rc.left - step, rc.top, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            break;
        }
        case Command::MoveWindowRight: {
            int step = static_cast<int>(Constants::KEYBOARD_WINDOW_MOVE_STEP * app.dpiScale);
            RECT rc;
            GetWindowRect(hWnd, &rc);
            SetWindowPos(hWnd, nullptr, rc.left + step, rc.top, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            break;
        }

        // -----------------------------------------------------------------------
        // Window resize from center  (Shift+Numpad+/- and Shift++/-)
        // -----------------------------------------------------------------------
        case Command::ResizeWindowLarger: {
            int step = static_cast<int>(Constants::KEYBOARD_WINDOW_RESIZE_STEP * app.dpiScale);
            RECT rc;
            GetWindowRect(hWnd, &rc);
            SetWindowPos(hWnd, nullptr,
                         rc.left - step, rc.top - step,
                         (rc.right - rc.left) + 2 * step, (rc.bottom - rc.top) + 2 * step,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            break;
        }
        case Command::ResizeWindowSmaller: {
            int step = static_cast<int>(Constants::KEYBOARD_WINDOW_RESIZE_STEP * app.dpiScale);
            RECT rc;
            GetWindowRect(hWnd, &rc);
            int newW = std::max(static_cast<int>(rc.right - rc.left) - 2 * step, 100);
            int newH = std::max(static_cast<int>(rc.bottom - rc.top) - 2 * step, 100);
            SetWindowPos(hWnd, nullptr,
                         rc.left + step, rc.top + step,
                         newW, newH,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            break;
        }

        // -----------------------------------------------------------------------
        // Autosize viewer to fill available screen space  (Ctrl+Space)
        // Starts from the work area (excludes taskbar), then subtracts the rect
        // of every visible thumbnail panel (F3 CacheWnd, F6 DirWnd, spawned
        // DirWnds) that is docked at a screen edge.
        // -----------------------------------------------------------------------
        case Command::AutosizeToWorkArea: {
            if (app.isAutosized) {
                HMONITOR hMon2 = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
                MONITORINFO mi2 = {sizeof(mi2)};
                if (!GetMonitorInfo(hMon2, &mi2)) break;
                int tw = (int)(app.baseWidth  * app.dpiScale);
                int th = (int)(app.baseHeight * app.dpiScale);
                int mw = mi2.rcMonitor.right  - mi2.rcMonitor.left;
                int mh = mi2.rcMonitor.bottom - mi2.rcMonitor.top;
                SetWindowPos(hWnd, nullptr,
                             mi2.rcMonitor.left + (mw - tw) / 2,
                             mi2.rcMonitor.top  + (mh - th) / 2,
                             tw, th,
                             SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
                InvalidateRect(hWnd, nullptr, FALSE);
                app.isAutosized = false;
                g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::AUTOSIZE_RESTORE);
                break;
            }

            HMONITOR hMon = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi = {sizeof(mi)};
            if (!GetMonitorInfo(hMon, &mi)) break;

            RECT r = mi.rcWork;

            const UI::PanelLayout &layout = uiManager.GetLayout();
            // positions: 1=top, 2=right, 3=bottom, 4=left
            for (int8_t pos = 1; pos <= 4; ++pos) {
                const UI::SlotInfo *slot = layout.getSlot(pos);
                if (!slot || slot->isEmpty() || !slot->panel) continue;
                HWND ph = slot->panel->GetHwnd();
                if (!ph || !IsWindowVisible(ph)) continue;
                RECT pr;
                if (!GetWindowRect(ph, &pr)) continue;
                switch (pos) {
                    case 1: r.top    = std::max(r.top,    pr.bottom); break;
                    case 2: r.right  = std::min(r.right,  pr.left);   break;
                    case 3: r.bottom = std::min(r.bottom, pr.top);    break;
                    case 4: r.left   = std::max(r.left,   pr.right);  break;
                }
            }

            if (r.right > r.left && r.bottom > r.top) {
                SetWindowPos(hWnd, nullptr, r.left, r.top,
                             r.right - r.left, r.bottom - r.top,
                             SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
                InvalidateRect(hWnd, nullptr, FALSE);
                app.isAutosized = true;
            }
            g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::AUTOSIZE_TO_WORK_AREA);
            break;
        }

        default:
            break;
    }
}
