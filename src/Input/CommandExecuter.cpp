#include "Command.h"
#include "../AppState.h"
#include "../Platform/Constants.h"
#include "../Platform/FileHandler.h"
#include "../UI/CacheWindow.h"
#include "../UI/DirWindow.h"
#include "../UI/HistoryWindow.h"
#include "../UI/HelpWindow.h"
#include <algorithm>
#include <commdlg.h>
#include <shlobj_core.h>
#include <shtypes.h>
#include "AppCommands.h"

// These two functions live in AppMain.cpp.
// Declared here (not in a header) to keep them package-private.


extern AppState g_app;

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
            if (!g_app.playlist.empty()) {
                int size = static_cast<int>(g_app.playlist.size());
                LoadImageIndex(hWnd, (g_app.currentIndex + 1) % size);
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            break;

        case Command::PrevImage:
            if (!g_app.playlist.empty()) {
                int size = static_cast<int>(g_app.playlist.size());
                LoadImageIndex(hWnd, (g_app.currentIndex - 1 + size) % size);
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            break;

        case Command::ShowInExplorer:
            if (!g_app.playlist.empty() && g_app.currentIndex >= 0) {
                const std::wstring &path = g_app.playlist[g_app.currentIndex];
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
            g_app.viewMode = static_cast<Constants::ViewModes::ViewMode>(1);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        case Command::ViewMode2:
            g_app.viewMode = static_cast<Constants::ViewModes::ViewMode>(2);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        case Command::ViewMode3:
            g_app.viewMode = static_cast<Constants::ViewModes::ViewMode>(3);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        case Command::ViewMode4:
            g_app.viewMode = static_cast<Constants::ViewModes::ViewMode>(4);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        case Command::ViewMode5:
            g_app.viewMode = static_cast<Constants::ViewModes::ViewMode>(5);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;

        // -----------------------------------------------------------------------
        // Zoom
        // -----------------------------------------------------------------------
        case Command::ZoomIn:
            g_app.viewport.zoom *= Constants::ZOOM_STEP;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;

        case Command::ZoomOut:
            g_app.viewport.zoom /= Constants::ZOOM_STEP;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;

        case Command::ZoomReset:
            g_app.viewport.zoom = 1.0f;
            g_app.viewport.offsetX = 0.0f;
            g_app.viewport.offsetY = 0.0f;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;

        // -----------------------------------------------------------------------
        // Transform
        // -----------------------------------------------------------------------
        case Command::RotateCW:
            g_app.viewport.rotation = (g_app.viewport.rotation + 90) % 360;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;

        case Command::RotateCCW:
            g_app.viewport.rotation = (g_app.viewport.rotation - 90 + 360) % 360;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;

        case Command::FlipH:
            g_app.viewport.flippedH = !g_app.viewport.flippedH;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;

        case Command::FlipV:
            g_app.viewport.flippedV = !g_app.viewport.flippedV;
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
            UI::ToggleHelpWindow();
            break;

        case Command::OpenFile:
            OpenInitialImage(hWnd);
            break;

        case Command::ToggleCache:
            UI::ToggleCacheWindow();
            break;

        case Command::ClearCache:
            UI::ClearThumbnailCache();
            break;

        case Command::ToggleDir:
            UI::ToggleDirWindow();
            break;

        case Command::ToggleHistory:
            UI::ToggleHistoryWindow();
            break;

        case Command::ToggleOverlay:
            g_app.showOverlayInfoText = !g_app.showOverlayInfoText;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;

        // -----------------------------------------------------------------------
        // App control
        // -----------------------------------------------------------------------
        case Command::HideToTray:
            if (g_app.GetInstanceCount() <= 1) {
                ShowWindow(hWnd, SW_HIDE);
            } else {
                PostQuitMessage(0);
            }
            break;

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
            break;

        // -----------------------------------------------------------------------
        // Color effect toggles
        // -----------------------------------------------------------------------
        case Command::ToggleEffectPreview:
            g_app.effectPreviewEnabled = !g_app.effectPreviewEnabled;
            g_app.UpdateRendererColorEffects(hWnd);
            break;

        case Command::ToggleGrayscale:
            g_app.WakeUpAndApplyEffects(hWnd, g_app.effectGrayscale);
            break;

        case Command::ToggleInvert:
            g_app.WakeUpAndApplyEffects(hWnd, g_app.effectInvert);
            break;

        case Command::ToggleSepia:
            g_app.WakeUpAndApplyEffects(hWnd, g_app.effectSepia);
            break;

        case Command::ToggleSolarize:
            g_app.WakeUpAndApplyEffects(hWnd, g_app.effectSolarize);
            break;

        case Command::ToggleOutline:
            g_app.WakeUpAndApplyEffects(hWnd, g_app.effectOutline);
            break;

        case Command::ToggleThreshold:
            g_app.WakeUpAndApplyEffects(hWnd, g_app.effectThreshold);
            break;

        // -----------------------------------------------------------------------
        // Continuous adjustments
        // -----------------------------------------------------------------------
        case Command::GammaUp:
            g_app.gamma = std::min(Constants::MAX_GAMMA, g_app.gamma + Constants::GAMMA_STEP);
            g_app.WakeUpAndApplyEffects(hWnd);
            break;

        case Command::GammaDown:
            g_app.gamma = std::max(Constants::MIN_GAMMA, g_app.gamma - Constants::GAMMA_STEP);
            g_app.WakeUpAndApplyEffects(hWnd);
            break;

        case Command::BrightnessUp:
            g_app.brightness = std::clamp(
                    g_app.brightness + Constants::COLOR_ADJUST_STEP,
                    -Constants::MIN_MAX_BRIGHTNESS, Constants::MIN_MAX_BRIGHTNESS);
            g_app.WakeUpAndApplyEffects(hWnd);
            break;

        case Command::BrightnessDown:
            g_app.brightness = std::clamp(
                    g_app.brightness - Constants::COLOR_ADJUST_STEP,
                    -Constants::MIN_MAX_BRIGHTNESS, Constants::MIN_MAX_BRIGHTNESS);
            g_app.WakeUpAndApplyEffects(hWnd);
            break;

        case Command::ContrastUp:
            g_app.contrast = std::clamp(
                    g_app.contrast + Constants::COLOR_ADJUST_STEP,
                    0.0f, Constants::MIN_MAX_CONTRAST);
            g_app.WakeUpAndApplyEffects(hWnd);
            break;

        case Command::ContrastDown:
            g_app.contrast = std::clamp(
                    g_app.contrast - Constants::COLOR_ADJUST_STEP,
                    0.0f, Constants::MIN_MAX_CONTRAST);
            g_app.WakeUpAndApplyEffects(hWnd);
            break;

        case Command::SaturationUp:
            g_app.saturation = std::min(
                    Constants::MIN_MAX_SATURATION, g_app.saturation + Constants::COLOR_ADJUST_STEP);
            g_app.WakeUpAndApplyEffects(hWnd);
            break;

        case Command::SaturationDown:
            g_app.saturation = std::max(0.0f, g_app.saturation - Constants::COLOR_ADJUST_STEP);
            g_app.WakeUpAndApplyEffects(hWnd);
            break;

        // -----------------------------------------------------------------------
        // Save / reset
        // -----------------------------------------------------------------------
        case Command::ResetEffects:
            g_app.ResetEffects();
            g_app.UpdateRendererColorEffects(hWnd);
            break;

        case Command::SaveImage: {
            AppCommands::SaveImageToDisk(hWnd);
            break;
        }

        default:
            break;
    }
}
