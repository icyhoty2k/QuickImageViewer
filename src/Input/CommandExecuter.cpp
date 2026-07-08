#include "Command.h"
#include "../AppState.h"
#include "../Overlays/OverlayManager.h"
#include "../Platform/Constants.h"
#include "../Platform/FileHandler.h"
#include "../UI/CacheWnd.h"
#include "../UI/DirWnd.h"
#include "../UI/HistoryListWnd.h"
#include "../UI/HelpWnd.h"
#include <algorithm>
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
            break;

        case Command::ClearCache:
            uiManager.getCacheWindow().ClearThumbnailCache();

            break;

        case Command::ToggleDir:
            uiManager.Toggle(uiManager.getDirWindow());
            break;

        case Command::ToggleHistory:
            uiManager.Toggle(uiManager.getHistoryListWindow());
            break;

        // ── Master overlay toggle (N / I / Ctrl+0) ───────────────────────────
        case Command::ToggleOverlay: {
            app.showOverlayInfoText = !app.showOverlayInfoText;
            g_overlayManager.SetAllVisible(app.showOverlayInfoText);
            // Always post the state change to center-center — it survives the hide
            // because MID_CENTER is independently controlled by PostCenterMessage.
            g_overlayManager.PostCenterMessage(hWnd,
                                               app.showOverlayInfoText ? L"Info Panels: ON" : L"Info Panels: OFF");
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
            g_overlayManager.PostCenterMessage(hWnd, L"Reset to Defaults");
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
            app.ToggleEffectChronological(L"Grayscale");
            app.WakeUpAndApplyEffects(hWnd, app.effectGrayscale);
            break;

        case Command::ToggleInvert:
            app.ToggleEffectChronological(L"Invert");
            app.WakeUpAndApplyEffects(hWnd, app.effectInvert);
            break;

        case Command::ToggleSepia:
            app.ToggleEffectChronological(L"Sepia");
            app.WakeUpAndApplyEffects(hWnd, app.effectSepia);
            break;

        case Command::ToggleSolarize:
            app.ToggleEffectChronological(L"Solarize");
            app.WakeUpAndApplyEffects(hWnd, app.effectSolarize);
            break;

        case Command::ToggleOutline:
            app.ToggleEffectChronological(L"Outline");
            app.WakeUpAndApplyEffects(hWnd, app.effectOutline);
            break;

        case Command::ToggleThreshold:
            app.ToggleEffectChronological(L"Threshold");
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
            g_overlayManager.PostCenterMessage(hWnd, L"All Effects Reset");
            break;

        case Command::SaveImage: {
            AppCommands::SaveImageToDisk(hWnd);
            break;
        }

        default:
            break;
    }
}
