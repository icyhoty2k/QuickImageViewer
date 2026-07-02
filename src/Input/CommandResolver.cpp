#include "Command.h"
#include "Shortcuts.h"

// =============================================================================
// CommandResolver.cpp  —  Stage 1: key + modifiers → Command.
//
// Rules:
//   - Read modifiers once at the top; never call GetKeyState again below.
//   - Every constant must come from Shortcuts.h — no raw VK_ literals.
//   - Order: modifier-sensitive cases first so plain keys fall through cleanly.
// =============================================================================

Command InputManager::ResolveKeyboardKeys(UINT key) {
    bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

    // -------------------------------------------------------------------------
    // View modes  '1'–'5'  (no modifier)
    // -------------------------------------------------------------------------
    if (!ctrl && !shift && key >= Shortcuts::SC_VIEW_MODE_FIRST && key <= Shortcuts::SC_VIEW_MODE_LAST) {
        switch (key) {
            case '1': return Command::ViewMode1;
            case '2': return Command::ViewMode2;
            case '3': return Command::ViewMode3;
            case '4': return Command::ViewMode4;
            case '5': return Command::ViewMode5;
        }
    }

    // -------------------------------------------------------------------------
    // All other keys
    // -------------------------------------------------------------------------
    switch (key) {
        // --- Navigation ---
        case Shortcuts::SC_NAV_NEXT:
        case Shortcuts::SC_NAV_NEXT_A:
            return Command::NextImage;

        case Shortcuts::SC_NAV_PREV:
        case Shortcuts::SC_NAV_PREV_A:
            return Command::PrevImage;

        case Shortcuts::SC_NAV_NEXT_SPACE:
            // Space = next; Shift+Space = prev
            return shift ? Command::PrevImage : Command::NextImage;

        case Shortcuts::SC_NAV_SHOW_IN_EXPLORER: // E
        case Shortcuts::SC_NAV_SHOW_IN_EXPLORER_TAB: // Tab
            return Command::ShowInExplorer;

        // --- Zoom ---
        case Shortcuts::SC_ZOOM_IN_NUMPAD: return Command::ZoomIn;
        case Shortcuts::SC_ZOOM_OUT_NUMPAD: return Command::ZoomOut;
        case Shortcuts::SC_ZOOM_RESET: return Command::ZoomReset;

        // --- Transform ---
        case Shortcuts::SC_TRANSFORM_ROTATE:
            return shift ? Command::RotateCCW : Command::RotateCW;

        case Shortcuts::SC_TRANSFORM_FLIP_H: return Command::FlipH;
        case Shortcuts::SC_TRANSFORM_FLIP_V: return Command::FlipV;

        // --- Fullscreen: F / F11 / Enter / Ctrl+Shift+T ---
        case Shortcuts::SC_PANEL_FULLSCREEN: return Command::ToggleFullscreen; // F11
        case Shortcuts::SC_PANEL_FULLSCREEN_F: return Command::ToggleFullscreen; // F (no modifier)
        case Shortcuts::SC_PANEL_FULLSCREEN_ENTER: return Command::ToggleFullscreen; // Enter

        case Shortcuts::SC_PANEL_FULLSCREEN_T: // T — only with Ctrl+Shift
            if (ctrl && shift) return Command::ToggleFullscreen;
            break;

        // --- Panels ---
        case Shortcuts::SC_PANEL_HELP_TOGGLE: return Command::ToggleHelp; // F1
        case Shortcuts::SC_PANEL_OPEN_FILE: return Command::OpenFile; // F2
        case Shortcuts::SC_PANEL_CACHE_TOGGLE: return Command::ToggleCache; // F3
        case Shortcuts::SC_PANEL_DIR_TOGGLE: return Command::ToggleDir; // F5
        case Shortcuts::SC_PANEL_CACHE_CLEAR: return Command::ClearCache; // F12

        // --- Overlay ---
        // N without Ctrl = overlay toggle; Ctrl+N = new window (handled below)
        case Shortcuts::SC_PANEL_OVERLAY_TOGGLE: // 'N'
            if (!ctrl) return Command::ToggleOverlay;
            return Command::NewWindow; // Ctrl+N

        // --- App control ---
        case Shortcuts::SC_APP_HIDE: return Command::HideToTray; // Esc
        case Shortcuts::SC_APP_HIDE_ALT: // W — only with Ctrl
            if (ctrl) return Command::HideToTray;
            break;

        case Shortcuts::SC_APP_HARD_QUIT: // Q — only with Ctrl
            if (ctrl) return Command::HardQuit;
            break;

        case Shortcuts::SC_APP_RESET_DEFAULTS: // Delete
            if (shift) return Command::ResetAll;
            // plain Delete without shift → grayscale toggle (SC_COLOR_GRAYSCALE reuses VK_DELETE)
            return Command::ToggleGrayscale;

        // --- Color effect toggles ---
        case Shortcuts::ImageEffects::SC_EFFECT_APPLY_TOGGLE: return Command::ToggleEffectPreview; // `
        // SC_COLOR_GRAYSCALE == VK_DELETE — already handled above (plain Delete)
        case Shortcuts::ImageEffects::SC_COLOR_INVERT: return Command::ToggleInvert; // Insert
        case Shortcuts::ImageEffects::SC_COLOR_SEPIA: return Command::ToggleSepia; // Home
        case Shortcuts::ImageEffects::SC_COLOR_SOLARIZE: return Command::ToggleSolarize; // End
        case Shortcuts::ImageEffects::SC_COLOR_OUTLINE: return Command::ToggleOutline; // PgUp
        case Shortcuts::ImageEffects::SC_COLOR_THRESHOLD: return Command::ToggleThreshold; // PgDn

        // --- Continuous adjustments ---
        case Shortcuts::ImageEffects::SC_COLOR_GAMMA_UP: return Command::GammaUp;
        case Shortcuts::ImageEffects::SC_COLOR_GAMMA_DOWN: return Command::GammaDown;
        case Shortcuts::ImageEffects::SC_COLOR_BRIGHTNESS_UP: return Command::BrightnessUp;
        case Shortcuts::ImageEffects::SC_COLOR_BRIGHTNESS_DOWN: return Command::BrightnessDown;
        case Shortcuts::ImageEffects::SC_COLOR_CONTRAST_UP: return Command::ContrastUp;
        case Shortcuts::ImageEffects::SC_COLOR_CONTRAST_DOWN: return Command::ContrastDown;
        case Shortcuts::ImageEffects::SC_COLOR_SAT_UP: return Command::SaturationUp;
        case Shortcuts::ImageEffects::SC_COLOR_SAT_DOWN: return Command::SaturationDown;

        // --- Save / reset ---
        case Shortcuts::ImageEffects::SC_COLOR_RESET_ALL_EFFECTS: return Command::ResetEffects; // Numpad0
        case Shortcuts::ImageEffects::SC_COLOR_SAVE_TO_DISK: // S — only with Ctrl
            if (ctrl) return Command::SaveImage;
            break;
    }

    return Command::None;
}
