#pragma once

// ConstantsStrings.h
// Central repository for all user-visible text used in QIV overlays.
// Keep strings here so they have one place to change for localization.

namespace Constants::Messages {
    // ── MID_CENTER: state-change notifications ──────────────────────────────
    // These are posted via PostCenterMessage and auto-hide after the timer.
    //Inform user has jumped to firs last image in current folder
    constexpr const wchar_t *TOGGLE_FIRST_IMAGE_IN_FOLDER = L"First image: ";
    constexpr const wchar_t *TOGGLE_LAST_IMAGE_IN_FOLDER = L"Last image: ";
    constexpr const wchar_t *GO_TO_LAST_IMAGE_BEFORE_TOGGLE = L"Previous image: ";
    constexpr const wchar_t *CACHE_WINDOW_VISIBLE_MSG = L"Cache Window ON ";
    constexpr const wchar_t *CACHE_WINDOW_HIDDEN_MSG = L"Cache Window OFF ";
    constexpr const wchar_t *CACHE_WINDOW_CLEAR_CACHE_MSG = L"Cache cleared ! ";


    // Overlay master toggle (N / I / Ctrl+0)
    constexpr const wchar_t *INFO_PANELS_ON = L"Info Panels: ON";
    constexpr const wchar_t *INFO_PANELS_OFF = L"Info Panels: OFF";

    // Overlay background toggle (P)
    constexpr const wchar_t *OVERLAY_BG_ON = L"Overlay BG: ON";
    constexpr const wchar_t *OVERLAY_BG_OFF = L"Overlay BG: OFF";

    // Overlay layout cycle (O)
    constexpr const wchar_t *LAYOUT_GRID = L"Layout: Grid";
    constexpr const wchar_t *LAYOUT_STACKED = L"Layout: Stacked";
    constexpr const wchar_t *LAYOUT_SUMMARY = L"Layout: Summary";

    // Reset / effects
    constexpr const wchar_t *RESET_TO_DEFAULTS = L"Reset to Defaults";
    constexpr const wchar_t *ALL_EFFECTS_RESET = L"All Effects Reset";

    // Spawned DirWnd messages
    constexpr const wchar_t *SPAWN_DIR_TOP = L"DirWnd Spawned: Top";
    constexpr const wchar_t *SPAWN_DIR_LEFT = L"DirWnd Spawned: Left";
    constexpr const wchar_t *SPAWN_DIR_RIGHT = L"DirWnd Spawned: Right";
    constexpr const wchar_t *SPAWN_DIR_BOTTOM = L"DirWnd Spawned: Bottom";
    constexpr const wchar_t *SPAWN_DIR_CLOSED = L"DirWnd Closed";
    constexpr const wchar_t *SPAWN_DIR_NO_SPACE = L"No free positions for DirWnd";
}

namespace Constants::Strings {
    // ── BOT_LEFT slot: active color-effect labels ───────────────────────────
    // Used in OverlayManager::UpdateEffects() via appendLine()

    // Named colour effects (also fed to ToggleEffectChronological)
    constexpr const wchar_t *EFFECT_GRAYSCALE = L"Grayscale";
    constexpr const wchar_t *EFFECT_INVERT = L"Invert";
    constexpr const wchar_t *EFFECT_SEPIA = L"Sepia";
    constexpr const wchar_t *EFFECT_SOLARIZE = L"Solarize";
    constexpr const wchar_t *EFFECT_OUTLINE = L"Outline";
    constexpr const wchar_t *EFFECT_THRESHOLD = L"Threshold";

    // Continuous-parameter labels (prefix before the numeric value)
    constexpr const wchar_t *LABEL_BRIGHTNESS = L"Brightness: ";
    constexpr const wchar_t *LABEL_CONTRAST = L"Contrast: ";
    constexpr const wchar_t *LABEL_SATURATION = L"Saturation: ";
    constexpr const wchar_t *LABEL_GAMMA = L"Gamma: ";

    // Brightness sign prefix (positive values)
    constexpr const wchar_t *SIGN_POSITIVE = L"+";
}
