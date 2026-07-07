#pragma once
#include <windows.h>

// =============================================================================
// Command.h  —  All discrete keyboard actions in QIV.
//
// Flow: WM_KEYDOWN → InputManager::handleKeyboard()
//           Stage 1: ResolveKeyboardKeys()          → Command
//           Stage 2: ExecuteKeyboardShortcutCommand() → side effects
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
    ToggleDir,
    ToggleHistory,
    ToggleOverlay, // N / I        — master: all slots on/off
    ToggleOverlayTopRight, // Ctrl+Alt+1   — index / filename slot
    ToggleOverlayTopCenter, // Ctrl+Alt+2   — zoom slot
    ToggleOverlayBotRight, // Ctrl+Alt+3   — dims / file size slot
    ToggleOverlayBotLeft, // Ctrl+Alt+4   — effects slot

    // --- App control ---
    HideToTray,
    NewWindow,
    HardQuit,
    ResetAll,

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

    // --- Save / reset ---
    ResetEffects,
    SaveImage,
};

class InputManager {
    public:
        static void handleKeyboard(HWND hWnd, WPARAM wParam);

    private:
        static Command ResolveKeyboardKeys(UINT key);

        static void ExecuteKeyboardShortcutCommand(HWND hWnd, Command cmd);
};
