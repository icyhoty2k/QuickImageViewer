#pragma once
#include <windows.h>

// =============================================================================
// Command.h  —  All discrete keyboard actions in QIV.
//
// Flow: WM_KEYDOWN → InputManager::handleKeyboard()
//           Stage 1: ResolveKeyboardKeys()   → Command
//           Stage 2: ExecuteKeyboardShortcutCommand()  → side effects
// =============================================================================

enum class Command {
    None,

    // --- Navigation ---
    NextImage,
    PrevImage,
    ShowInExplorer,

    // --- View modes (1-5) ---
    ViewMode1,
    ViewMode2,
    ViewMode3,
    ViewMode4,
    ViewMode5,

    // --- Zoom ---
    ZoomIn,
    ZoomOut,
    ZoomReset,

    // --- Transform ---
    RotateCW,
    RotateCCW,
    FlipH,
    FlipV,

    // --- Fullscreen ---
    ToggleFullscreen,

    // --- Panels / Overlays ---
    ToggleHelp,
    OpenFile,
    ToggleCache,
    ClearCache,
    ToggleDir, // F5 — current-folder image browser
    ToggleOverlay,

    // --- App control ---
    HideToTray,
    NewWindow,
    HardQuit,
    ResetAll, // Shift+Delete — window layout + all effects

    // --- Color effects (toggles) ---
    ToggleGrayscale,
    ToggleInvert,
    ToggleSepia,
    ToggleSolarize,
    ToggleOutline,
    ToggleThreshold,
    ToggleEffectPreview,

    // --- Color adjustments ---
    GammaUp,
    GammaDown,
    BrightnessUp,
    BrightnessDown,
    ContrastUp,
    ContrastDown,
    SaturationUp,
    SaturationDown,

    // --- Save ---
    ResetEffects, // Numpad0 — effects only, leave window alone
    SaveImage, // Ctrl+S
};

class InputManager {
    public:
        // Single public entry point — call from WM_KEYDOWN
        static void handleKeyboard(HWND hWnd, WPARAM wParam);

    private:
        static Command ResolveKeyboardKeys(UINT key);

        static void ExecuteKeyboardShortcutCommand(HWND hWnd, Command cmd);
};
