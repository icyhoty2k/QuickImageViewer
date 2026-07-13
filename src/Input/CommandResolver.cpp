#include "Command.h"
#include "Shortcuts.h"
#include "AppState.h"

extern AppState app;

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
    bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;

    // -------------------------------------------------------------------------
    // Sort order  Ctrl+Alt+Shift+0/6/7/8/9
    // -------------------------------------------------------------------------
    if (ctrl && alt && shift) {
        switch (key) {
            case Shortcuts::SC_SORT_BY_NAME: return Command::SortByName;
            case Shortcuts::SC_SORT_BY_DATE: return Command::SortByDate;
            case Shortcuts::SC_SORT_BY_SIZE: return Command::SortBySize;
            case Shortcuts::SC_SORT_BY_TYPE: return Command::SortByType;
            case Shortcuts::SC_SORT_BY_DISK: return Command::SortByDisk;
        }
    }

    // -------------------------------------------------------------------------
    // Per-slot compact-mode toggles  Ctrl+Alt+1..9  (no shift)
    // -------------------------------------------------------------------------
    if (ctrl && !alt && shift) {
        switch (key) {
            case Shortcuts::SC_OVERLAY_COMPACT_1: return Command::CompactOverlaySlot1;
            case Shortcuts::SC_OVERLAY_COMPACT_2: return Command::CompactOverlaySlot2;
            case Shortcuts::SC_OVERLAY_COMPACT_3: return Command::CompactOverlaySlot3;
            case Shortcuts::SC_OVERLAY_COMPACT_4: return Command::CompactOverlaySlot4;
            case Shortcuts::SC_OVERLAY_COMPACT_5: return Command::CompactOverlaySlot5;
            case Shortcuts::SC_OVERLAY_COMPACT_6: return Command::CompactOverlaySlot6;
            case Shortcuts::SC_OVERLAY_COMPACT_7: return Command::CompactOverlaySlot7;
            case Shortcuts::SC_OVERLAY_COMPACT_8: return Command::CompactOverlaySlot8;
            case Shortcuts::SC_OVERLAY_COMPACT_9: return Command::CompactOverlaySlot9;
        }
    }

    // -------------------------------------------------------------------------
    // Per-slot visibility toggles  Ctrl+1..9  and  Ctrl+0 (master)   (no alt, no shift)
    // -------------------------------------------------------------------------
    if (ctrl && !alt && !shift) {
        switch (key) {
            case Shortcuts::SC_PANEL_OVERLAY_MASTER_CTRL0: return Command::ToggleOverlay;
            case Shortcuts::SC_OVERLAY_SLOT_1: return Command::ToggleOverlaySlot1;
            case Shortcuts::SC_OVERLAY_SLOT_2: return Command::ToggleOverlaySlot2;
            case Shortcuts::SC_OVERLAY_SLOT_3: return Command::ToggleOverlaySlot3;
            case Shortcuts::SC_OVERLAY_SLOT_4: return Command::ToggleOverlaySlot4;
            case Shortcuts::SC_OVERLAY_SLOT_5: return Command::ToggleOverlaySlot5;
            case Shortcuts::SC_OVERLAY_SLOT_6: return Command::ToggleOverlaySlot6;
            case Shortcuts::SC_OVERLAY_SLOT_7: return Command::ToggleOverlaySlot7;
            case Shortcuts::SC_OVERLAY_SLOT_8: return Command::ToggleOverlaySlot8;
            case Shortcuts::SC_OVERLAY_SLOT_9: return Command::ToggleOverlaySlot9;
        }
    }

    // -------------------------------------------------------------------------
    // View modes  '1'–'5'  (no modifier at all)
    // -------------------------------------------------------------------------
    if (!ctrl && !alt && !shift && key >= Shortcuts::SC_VIEW_MODE_FIRST && key <= Shortcuts::SC_VIEW_MODE_LAST) {
        switch (key) {
            case '1': return Command::ViewMode1;
            case '2': return Command::ViewMode2;
            case '3': return Command::ViewMode3;
            case '4': return Command::ViewMode4;
            case '5': return Command::ViewMode5;
        }
    }

    // -------------------------------------------------------------------------
    // Theme factor  Ctrl+Alt+Numpad+/-/0  (no shift — Shift+Numpad0 = VK_INSERT = Invert)
    // -------------------------------------------------------------------------
    if (ctrl && alt && !shift) {
        switch (key) {
            case Shortcuts::SC_THEME_FACTOR_UP:    return Command::ThemeFactorUp;
            case Shortcuts::SC_THEME_FACTOR_DOWN:  return Command::ThemeFactorDown;
            case Shortcuts::SC_THEME_FACTOR_RESET: return Command::ThemeFactorReset;
        }
    }

    // -------------------------------------------------------------------------
    // Window chrome  Ctrl+Shift+Numpad* / Numpad/
    // -------------------------------------------------------------------------
    if (ctrl && shift && !alt) {
        switch (key) {
            case Shortcuts::SC_CORNER_PREFERENCE_TOGGLE: return Command::ToggleCornerPreference;
            case Shortcuts::SC_BACKDROP_TYPE_CYCLE:      return Command::CycleBackdropType;
        }
    }

    // -------------------------------------------------------------------------
    // Q (no modifier)  —  toggle last/current dir
    // -------------------------------------------------------------------------
    if (!ctrl && !alt && !shift && key == Shortcuts::SC_TOGGLE_LAST_DIR)
        return Command::ToggleLastDir;

    // -------------------------------------------------------------------------
    // Ctrl+F1  —  Slideshow start/stop (must precede plain F1 = ToggleHelp in switch)
    // -------------------------------------------------------------------------
    if (ctrl && !alt && !shift && key == Shortcuts::SC_SLIDESHOW_TOGGLE)
        return Command::SlideshowToggle;

    // -------------------------------------------------------------------------
    // Slideshow-only keys (Space / R / S) — only intercepted when slideshow is running
    // -------------------------------------------------------------------------
    if (!ctrl && !alt && !shift && app.slideshow.running) {
        if (key == Shortcuts::SC_SLIDESHOW_PAUSE_RESUME)      return Command::SlideshowPauseResume;
        if (key == Shortcuts::SC_SLIDESHOW_LOOP_TOGGLE)       return Command::SlideshowToggleLoop;
        if (key == Shortcuts::SC_SLIDESHOW_SHUFFLE_TOGGLE)    return Command::SlideshowToggleShuffle;
        if (key == Shortcuts::SC_SLIDESHOW_TRANSITION_CYCLE)  return Command::SlideshowCycleTransition;
    }

    // -------------------------------------------------------------------------
    // All other keys
    // -------------------------------------------------------------------------
    switch (key) {
        // --- Navigation ---
        case Shortcuts::SC_NAV_NEXT: return Command::NextImage;
        case Shortcuts::SC_NAV_PREV: return Command::PrevImage;
        //smart jump to first or last image depending which is further
        case Shortcuts::SC_NAV_TOGGLE_FIRST_LAST_IMAGE_IN_CURR_FOLDER:
            return shift ? Command::GoToLastImageInCurrentFolder : Command::ToggleFirstLastImageInCurrentFolder;

        case Shortcuts::SC_NAV_NEXT_SPACE:
            return shift ? Command::PrevImage : Command::NextImage;

        case Shortcuts::SC_NAV_SHOW_IN_EXPLORER: // 'E'
            if (!ctrl && alt && !shift)   return Command::SnapTopRight;
            return Command::ShowInExplorer;

        // --- Zoom ---
        case Shortcuts::SC_ZOOM_IN_NUMPAD: // VK_ADD  plain=zoom-in  shift=resize-larger
            if (shift) return Command::ResizeWindowLarger;
            return Command::ZoomIn;
        case Shortcuts::SC_ZOOM_OUT_NUMPAD: // VK_SUBTRACT  plain=zoom-out  shift=resize-smaller
            if (shift) return Command::ResizeWindowSmaller;
            return Command::ZoomOut;
        case Shortcuts::SC_ZOOM_RESET: return Command::ZoomReset;

        // --- Transform ---
        case Shortcuts::SC_TRANSFORM_ROTATE:
            return shift ? Command::RotateCCW : Command::RotateCW;

        case Shortcuts::SC_TRANSFORM_FLIP_H: return Command::FlipH;
        case Shortcuts::SC_TRANSFORM_FLIP_V: return Command::FlipV;

        // --- Fullscreen ---
        case Shortcuts::SC_PANEL_FULLSCREEN: return Command::ToggleFullscreen;
        case Shortcuts::SC_PANEL_FULLSCREEN_F: return Command::ToggleFullscreen;
        case Shortcuts::SC_PANEL_FULLSCREEN_ENTER: return Command::ToggleFullscreen;

        case Shortcuts::SC_PANEL_FULLSCREEN_T: // 'T' — shared key
            if (ctrl && shift)          return Command::ToggleFullscreen;
            if (ctrl && !shift && !alt) return Command::ToggleAlwaysOnTop;
            break;

        // --- Panels ---
        case Shortcuts::SC_PANEL_HELP_TOGGLE: return Command::ToggleHelp;
        case Shortcuts::SC_PANEL_OPEN_FILE: return Command::OpenFile;
        case Shortcuts::SC_PANEL_CACHE_TOGGLE: return Command::ToggleCache;
        case Shortcuts::SC_PANEL_DIR_TOGGLE: return Command::ToggleDir;
        case Shortcuts::SC_PANEL_HISTORY_TOGGLE: return Command::ToggleHistory;
        case Shortcuts::SC_PANEL_CACHE_CLEAR: return Command::ClearCache;

        // --- Overlays ---
        case Shortcuts::SC_PANEL_OVERLAY_TOGGLE: // N
            if (!ctrl) return Command::ToggleOverlay;
            return Command::NewWindow; // Ctrl+N

        case Shortcuts::SC_PANEL_OVERLAY_MASTER: // I
            if (!ctrl) return Command::ToggleOverlay;
            break;

        case Shortcuts::SC_OVERLAY_LAYOUT_CYCLE: // O
            if (!ctrl && !alt && !shift) return Command::CycleOverlayLayout;
            break;

        case Shortcuts::SC_OVERLAY_BG_TOGGLE: // P
            if (!ctrl && !alt && !shift) return Command::ToggleOverlayBackground;
            break;

        // --- App control ---
        case Shortcuts::SC_APP_HIDE: return Command::HideToTray;

        case Shortcuts::SC_APP_HIDE_ALT: // 'W'  ctrl=hide  plain=pan-up  shift=move-up  alt=snap-top
            if (ctrl)                      return Command::HideToTray;
            if (!ctrl &&  alt && !shift)   return Command::SnapTop;
            if (!ctrl && !alt && !shift)   return Command::PanUp;
            if (!ctrl && !alt &&  shift)   return Command::MoveWindowUp;
            break;

        case Shortcuts::SC_APP_HARD_QUIT: // 'Q'
            if (ctrl)                     return Command::HardQuit;
            if (!ctrl && alt && !shift)   return Command::SnapTopLeft;
            break;

        case Shortcuts::SC_APP_RESET_DEFAULTS:
            if (shift) return Command::ResetAll;
            return Command::ToggleGrayscale; // plain Delete

        // --- Color effect toggles ---
        case Shortcuts::ImageEffects::SC_EFFECT_APPLY_TOGGLE: return Command::ToggleEffectPreview;
        case Shortcuts::ImageEffects::SC_COLOR_INVERT: return Command::ToggleInvert;
        case Shortcuts::ImageEffects::SC_COLOR_SEPIA: return Command::ToggleSepia;
        case Shortcuts::ImageEffects::SC_COLOR_SOLARIZE: return Command::ToggleSolarize;
        case Shortcuts::ImageEffects::SC_COLOR_OUTLINE: return Command::ToggleOutline;
        case Shortcuts::ImageEffects::SC_COLOR_THRESHOLD: return Command::ToggleThreshold;

        // --- Continuous adjustments ---
        case Shortcuts::ImageEffects::SC_COLOR_GAMMA_UP: // VK_OEM_PLUS  plain=gamma-up  shift=resize-larger
            if (shift) return Command::ResizeWindowLarger;
            return Command::GammaUp;
        case Shortcuts::ImageEffects::SC_COLOR_GAMMA_DOWN: // VK_OEM_MINUS  plain=gamma-down  shift=resize-smaller
            if (shift) return Command::ResizeWindowSmaller;
            return Command::GammaDown;
        case Shortcuts::ImageEffects::SC_COLOR_BRIGHTNESS_UP: return Command::BrightnessUp;
        case Shortcuts::ImageEffects::SC_COLOR_BRIGHTNESS_DOWN: return Command::BrightnessDown;
        case Shortcuts::ImageEffects::SC_COLOR_CONTRAST_UP: return Command::ContrastUp;
        case Shortcuts::ImageEffects::SC_COLOR_CONTRAST_DOWN: return Command::ContrastDown;
        case Shortcuts::ImageEffects::SC_COLOR_SAT_UP: return Command::SaturationUp;
        case Shortcuts::ImageEffects::SC_COLOR_SAT_DOWN: return Command::SaturationDown;


        // --- Save / reset ---
        case Shortcuts::ImageEffects::SC_COLOR_RESET_ALL_EFFECTS: return Command::ResetEffects;

        case Shortcuts::ImageEffects::SC_COLOR_SAVE_TO_DISK: // 'S'  ctrl=save  plain=pan-down  shift=move-down  alt=snap-bottom
            if (ctrl)                      return Command::SaveImage;
            if (!ctrl &&  alt && !shift)   return Command::SnapBottom;
            if (!ctrl && !alt && !shift)   return Command::PanDown;
            if (!ctrl && !alt &&  shift)   return Command::MoveWindowDown;
            break;

        case Shortcuts::SC_PAN_LEFT: // 'A'  plain=pan-left  shift=move-left  alt=snap-left
            if (!ctrl &&  alt && !shift)   return Command::SnapLeft;
            if (!ctrl && !alt && !shift)   return Command::PanLeft;
            if (!ctrl && !alt &&  shift)   return Command::MoveWindowLeft;
            break;

        case Shortcuts::SC_PAN_RIGHT: // 'D'  plain=pan-right  shift=move-right  alt=snap-right
            if (!ctrl &&  alt && !shift)   return Command::SnapRight;
            if (!ctrl && !alt && !shift)   return Command::PanRight;
            if (!ctrl && !alt &&  shift)   return Command::MoveWindowRight;
            break;

        case 'Z':
            if (!ctrl && alt && !shift)    return Command::SnapBottomLeft;
            break;

        case 'C':
            if ( ctrl && !alt && !shift)   return Command::CopyToClipboard;
            if (!ctrl &&  alt && !shift)   return Command::SnapBottomRight;
            break;

        case 'X':
            if (!ctrl && alt && !shift)    return Command::ResetAll;
            break;

        case Shortcuts::SC_SHOW_INFO: // 'M'
            if (!ctrl && !alt && !shift)   return Command::ShowInfo;
            break;
    }

    return Command::None;
}
