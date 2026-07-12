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
    ToggleFirstLastImageInCurrentFolder,
    GoToLastImageInCurrentFolder,
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
    // Master toggle  (N / I / Ctrl+0)
    ToggleOverlay,
    // Cycle layout mode 0→1→2→0  (O)
    CycleOverlayLayout,
    // Toggle overlay text background on/off  (P)
    ToggleOverlayBackground,

    // Per-slot visibility toggles  (Ctrl+1 .. Ctrl+9)
    ToggleOverlaySlot1, // TOP_LEFT
    ToggleOverlaySlot2, // TOP_CENTER
    ToggleOverlaySlot3, // TOP_RIGHT
    ToggleOverlaySlot4, // MID_LEFT
    ToggleOverlaySlot5, // MID_CENTER  (center-center message)
    ToggleOverlaySlot6, // MID_RIGHT
    ToggleOverlaySlot7, // BOT_LEFT
    ToggleOverlaySlot8, // BOT_CENTER
    ToggleOverlaySlot9, // BOT_RIGHT

    // Per-slot compact-mode toggle  (Ctrl+Alt+1 .. Ctrl+Alt+9)
    // Switches between 1-line (compact) and 2-line (full) display for that slot
    CompactOverlaySlot1,
    CompactOverlaySlot2,
    CompactOverlaySlot3,
    CompactOverlaySlot4,
    CompactOverlaySlot5,
    CompactOverlaySlot6,
    CompactOverlaySlot7,
    CompactOverlaySlot8,
    CompactOverlaySlot9,

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

    // --- Runtime theme ---
    ThemeFactorUp,   // Ctrl+Alt+Shift+Numpad+
    ThemeFactorDown, // Ctrl+Alt+Shift+Numpad-
    ThemeFactorReset,// Ctrl+Alt+Shift+Numpad0

    // --- Window chrome ---
    ToggleCornerPreference, // Ctrl+Shift+Numpad*   round ↔ square
    CycleBackdropType,      // Ctrl+Shift+Numpad/   None→Mica→Acrylic→MicaAlt→None
};

class InputManager {
    public:
        static void handleKeyboard(HWND hWnd, WPARAM wParam);

    private:
        static Command ResolveKeyboardKeys(UINT key);

        static void ExecuteKeyboardShortcutCommand(HWND hWnd, Command cmd);
};
