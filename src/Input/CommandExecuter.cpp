#include "Command.h"
#include "../AppState.h"
#include "../Overlays/OverlayManager.h"
#include "../Platform/Constants.h"
#include "../Platform/ConstantsStrings.h"
#include "../Platform/FileHandler.h"
#include "../UI/CacheWnd.h"
#include "../UI/DirWnd.h"
#include "../UI/HistoryListWnd.h"
#include "../UI/HelpWnd.h"
#include <algorithm>
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

// =============================================================================
// handleKeyboard — public entry point called from WM_KEYDOWN
// =============================================================================
void InputManager::handleKeyboard(HWND hWnd, WPARAM wParam) {
    Command cmd = ResolveKeyboardKeys(static_cast<UINT>(wParam));
    if (cmd != Command::None) {
        ExecuteKeyboardShortcutCommand(hWnd, cmd);
    }
}

// =============================================================================
// ExecuteKeyboardShortcutCommand — Stage 2: Command → side effects
// =============================================================================
void InputManager::ExecuteKeyboardShortcutCommand(HWND hWnd, Command cmd) {
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
                OpenDirectory(hWnd, prevDir);
                g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::TOGGLE_DIR_CHANGED + prevDir);
            }
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
            app.viewMode = static_cast<Constants::ViewModes::ViewMode>(1);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        case Command::ViewMode2:
            app.viewMode = static_cast<Constants::ViewModes::ViewMode>(2);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        case Command::ViewMode3:
            app.viewMode = static_cast<Constants::ViewModes::ViewMode>(3);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        case Command::ViewMode4:
            app.viewMode = static_cast<Constants::ViewModes::ViewMode>(4);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        case Command::ViewMode5:
            app.viewMode = static_cast<Constants::ViewModes::ViewMode>(5);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;

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

        case Command::OpenFile:
            OpenInitialImage(hWnd);
            break;

        case Command::ToggleCache:
            uiManager.Toggle(uiManager.getCacheWindow());
            uiManager.getCacheWindow().IsVisible() ? g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::CACHE_WINDOW_VISIBLE_MSG) : g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::CACHE_WINDOW_HIDDEN_MSG);
            
            break;

        case Command::ClearCache:
            uiManager.getCacheWindow().ClearThumbnailCache();
            g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::CACHE_WINDOW_CLEAR_CACHE_MSG);

            break;

        case Command::ToggleDir: {
            UI::DirWnd &dirWnd = uiManager.getDirWindow();
            // When showing F5 DirWnd, try its default position (top) if available
            if (!dirWnd.IsVisible()) {
                // Check if default position is free
                const UI::PanelLayout &layout = uiManager.GetLayout();
                if (!layout.occupied(Constants::CURRENT_DIR_WINDOW_POSITION)) {
                    dirWnd.SetPosition(Constants::CURRENT_DIR_WINDOW_POSITION);
                }
                // Otherwise Show() will find a free position naturally
            }
            uiManager.Toggle(dirWnd);
            break;
        }

        case Command::ToggleHistory:
            uiManager.Toggle(uiManager.getHistoryListWindow());
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
            Constants::Overlay::OVERLAY_SHOW_BACKGROUND = !Constants::Overlay::OVERLAY_SHOW_BACKGROUND;
            g_overlayManager.PostCenterMessage(hWnd,
                                               Constants::Overlay::OVERLAY_SHOW_BACKGROUND ? Constants::Messages::OVERLAY_BG_ON : Constants::Messages::OVERLAY_BG_OFF);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }

        // ── Master overlay toggle (N / I / Ctrl+0) ───────────────────────────
        case Command::ToggleOverlay: {
            app.showOverlayInfoText = !app.showOverlayInfoText;
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
            if (app.GetInstanceCount() <= 1) {
                //Tray icon on close
                AppCommands::AddTrayIcon(hWnd);
                ShowWindow(hWnd, SW_HIDE);
            } else {
                DestroyWindow(hWnd);
            }
            break;
        }
        case Command::NewWindow: {
            wchar_t exePath[MAX_PATH];
            GetModuleFileNameW(nullptr, exePath, MAX_PATH);
            SetEnvironmentVariableW(L"QIV_NEW_INSTANCE", L"1");
            ShellExecuteW(nullptr, L"open", exePath, nullptr, nullptr, SW_SHOW);
            SetEnvironmentVariableW(L"QIV_NEW_INSTANCE", nullptr);
            break;
        }

        case Command::HardQuit:
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
            g_overlayManager.PostCenterMessage(hWnd,
                Constants::Messages::THEME_FACTOR_PREFIX +
                std::to_wstring(static_cast<int>(std::round(app.themeFactor * 100))) + L"%");
            break;

        case Command::ThemeFactorDown:
            AppCommands::changeAppThemeFactor(hWnd, app.themeFactor - Constants::Theme::THEME_FACTOR_STEP);
            g_overlayManager.PostCenterMessage(hWnd,
                Constants::Messages::THEME_FACTOR_PREFIX +
                std::to_wstring(static_cast<int>(std::round(app.themeFactor * 100))) + L"%");
            break;

        case Command::ThemeFactorReset:
            AppCommands::changeAppThemeFactor(hWnd, Constants::Theme::THEME_FACTOR);
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

        // -----------------------------------------------------------------------
        // Sort order  (Ctrl+Alt+Shift+0/6/7/8/9)
        // First press:  sets the sort mode ascending (reverse = false)
        // Second press: toggles to descending (reverse = true), third press back, etc.
        // -----------------------------------------------------------------------
        case Command::SortByName: {
            if (app.fileHandlerDefaultSortOrder == 0)
                app.fileHandlerIsReverseSortOrder = !app.fileHandlerIsReverseSortOrder;
            else { app.fileHandlerDefaultSortOrder = 0; app.fileHandlerIsReverseSortOrder = false; }
            ReSortPlaylistAndRebuildMap(hWnd);
            g_overlayManager.PostCenterMessage(hWnd, app.fileHandlerIsReverseSortOrder
                ? Constants::Messages::SORT_BY_NAME_REV : Constants::Messages::SORT_BY_NAME);
            break;
        }

        case Command::SortByDate: {
            if (app.fileHandlerDefaultSortOrder == 1)
                app.fileHandlerIsReverseSortOrder = !app.fileHandlerIsReverseSortOrder;
            else { app.fileHandlerDefaultSortOrder = 1; app.fileHandlerIsReverseSortOrder = false; }
            ReSortPlaylistAndRebuildMap(hWnd);
            g_overlayManager.PostCenterMessage(hWnd, app.fileHandlerIsReverseSortOrder
                ? Constants::Messages::SORT_BY_DATE_REV : Constants::Messages::SORT_BY_DATE);
            break;
        }

        case Command::SortBySize: {
            if (app.fileHandlerDefaultSortOrder == 2)
                app.fileHandlerIsReverseSortOrder = !app.fileHandlerIsReverseSortOrder;
            else { app.fileHandlerDefaultSortOrder = 2; app.fileHandlerIsReverseSortOrder = false; }
            ReSortPlaylistAndRebuildMap(hWnd);
            g_overlayManager.PostCenterMessage(hWnd, app.fileHandlerIsReverseSortOrder
                ? Constants::Messages::SORT_BY_SIZE_REV : Constants::Messages::SORT_BY_SIZE);
            break;
        }

        case Command::SortByType: {
            if (app.fileHandlerDefaultSortOrder == 3)
                app.fileHandlerIsReverseSortOrder = !app.fileHandlerIsReverseSortOrder;
            else { app.fileHandlerDefaultSortOrder = 3; app.fileHandlerIsReverseSortOrder = false; }
            ReSortPlaylistAndRebuildMap(hWnd);
            g_overlayManager.PostCenterMessage(hWnd, app.fileHandlerIsReverseSortOrder
                ? Constants::Messages::SORT_BY_TYPE_REV : Constants::Messages::SORT_BY_TYPE);
            break;
        }

        case Command::SortByDisk: {
            // Disk order has no meaningful reverse — pressing again is a no-op toggle
            app.fileHandlerDefaultSortOrder = 4;
            app.fileHandlerIsReverseSortOrder = false;
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
                    + (app.slideshow.loop    ? L"  Loop"    : L"")
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
                wasPaused ? Constants::Messages::SLIDESHOW_PLAYING
                          : Constants::Messages::SLIDESHOW_PAUSED);
            break;
        }

        case Command::SlideshowToggleLoop:
            app.slideshow.loop = !app.slideshow.loop;
            g_overlayManager.PostCenterMessage(hWnd,
                app.slideshow.loop ? Constants::Messages::SLIDESHOW_LOOP_ON
                                   : Constants::Messages::SLIDESHOW_LOOP_OFF);
            break;

        case Command::SlideshowToggleShuffle: {
            app.slideshow.shuffle = !app.slideshow.shuffle;
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
                app.slideshow.shuffle ? Constants::Messages::SLIDESHOW_SHUFFLE_ON
                                      : Constants::Messages::SLIDESHOW_SHUFFLE_OFF);
            break;
        }

        case Command::SlideshowCycleTransition: {
            // Cycle through implemented types only (Dissolve/Ripple are stubs)
            auto& t = app.slideshow.transition.type;
            const wchar_t* msg = Constants::Messages::TRANSITION_CUT;
            switch (t) {
                case TransitionType::Cut:      t = TransitionType::Fade;  msg = Constants::Messages::TRANSITION_FADE;  break;
                case TransitionType::Fade:     t = TransitionType::Push;  msg = Constants::Messages::TRANSITION_PUSH;  break;
                case TransitionType::Push:     t = TransitionType::Zoom;  msg = Constants::Messages::TRANSITION_ZOOM;  break;
                case TransitionType::Zoom:     t = TransitionType::Cut;   msg = Constants::Messages::TRANSITION_CUT;   break;
                case TransitionType::Dissolve: t = TransitionType::Cut;   msg = Constants::Messages::TRANSITION_CUT;   break;
                case TransitionType::Ripple:   t = TransitionType::Cut;   msg = Constants::Messages::TRANSITION_CUT;   break;
            }
            g_overlayManager.PostCenterMessage(hWnd, msg);
            break;
        }

        default:
            break;
    }
}
