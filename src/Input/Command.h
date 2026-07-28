#pragma once
#include <windows.h>
#include <string>

// =============================================================================
// Command.h  —  All discrete actions in QIV.
//
// Flow: WM_KEYDOWN → InputManager::handleKeyboard()
//           Stage 1: ResolveKeyboardKeys()          → Command
//           Stage 2: ExecuteCommand()                 → side effects
//
// EVERY input path resolves to a Command and goes through ExecuteCommand —
// keyboard, mouse, tray, context menu, panel clicks and the TCP/IP socket
// alike. Nothing applies a side effect by calling LoadImageIndex or an
// AppCommands helper directly.
//
// That is not tidiness for its own sake: ExecuteCommand is where mirroring to
// another instance is gated, so an input path that skips it is an input path
// that silently does not mirror. Keeping the sink universal makes the gate
// correct by construction rather than by remembering.
//
// ORDER IS NOT STABLE. Inserting an enumerator renumbers everything after it,
// so nothing may persist or transmit a Command's ordinal — the wire protocol
// uses NAMES for exactly this reason (see RemoteProtocol.h). The only ordering
// contracts are the contiguous RANGES marked below.
// =============================================================================

enum class Command {
    None,

    // --- Navigation ---
    NextImage,
    PrevImage,
    ToggleFirstLastImageInCurrentFolder,
    GoToLastImageInCurrentFolder,
    GoToFirstImage, // Home — unconditional jump to index 0
    GoToLastImage,  // End  — unconditional jump to the final index
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
    ZoomTo,

    // --- Transform ---
    RotateCW,
    RotateCCW,
    FlipH,
    FlipV,
    ToggleThumbnailWrapAround, // B — toggle thumbnail strip wheel wrap-around
    ToggleThumbnailEffects, // U — toggle thumbnail strip visual effects (rounded corners, glow, hover scale)
    ToggleViewportLock, // Y — keep zoom + pan across image changes (rotation/flips still auto-orient)

    // --- Fullscreen ---
    ToggleFullscreen,

    // --- Panels / Overlays ---
    ToggleHelp,
    OpenFile,
    ReloadCurrentDir,
    ToggleCache,
    ClearCache,
    ToggleDir,
    ToggleHistory,
    ToggleHistoryFull,
    // Master toggle  (I / Ctrl+0)
    ToggleOverlay,
    // Cycle layout mode 0→1→2→0  (O)
    CycleOverlayLayout,
    // Toggle overlay text background on/off  (P)
    ToggleOverlayBackground,

    // Per-slot state cycle  (Ctrl+1 .. Ctrl+9)
    // Each press walks the slot through Compact → Full → Off, the same three
    // states the Overlays submenu offers. Compact mode has no shortcut of its
    // own — it is one position of this cycle.
    ToggleOverlaySlot1, // TOP_LEFT
    ToggleOverlaySlot2, // TOP_CENTER
    ToggleOverlaySlot3, // TOP_RIGHT
    ToggleOverlaySlot4, // MID_LEFT
    ToggleOverlaySlot5, // MID_CENTER  (center-center message — On / Off only)
    ToggleOverlaySlot6, // MID_RIGHT
    ToggleOverlaySlot7, // BOT_LEFT
    ToggleOverlaySlot8, // BOT_CENTER
    ToggleOverlaySlot9, // BOT_RIGHT

    // --- App control ---
    HideToTray,
    NewWindow,
    CloseAllPanels, // hide every floating panel + spawned DirWnd (main window stays)
    RestoreAllPanels, // re-open exactly the panels the last CloseAllPanels hid
    ToggleAllPanels, // N — close all if any visible, else restore the last-hidden set
    HardQuit,
    ResetAll,
    // Middle-mouse click: window size/position/opacity + zoom/pan back to
    // defaults. Deliberately NOT ResetAll — that also clears every image effect,
    // which the middle click never did and must not start doing.
    ResetWindowLayout,

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

    // --- File operations on the thumbnail panel's selection -----------------
    // Commands so that ONE gate can refuse them (see SESSION_BLOCKED in
    // RemoteProtocol.cpp) rather than four scattered checks that a fifth call
    // site could forget.
    //
    // All four are in NEVER_REMOTE, and a static_assert proves they have no row
    // in the protocol table — adding one fails the build. They alter the folder,
    // and a command that can delete files must not be one authentication bug
    // away from a socket.
    FileCopySelection,
    FileMoveSelection,
    FileDeleteSelection,
    FilePasteIntoFolder,

    // --- Navigation ---
    ToggleLastDir, // Q — switch between current and previous folder
    ToggleLastImage, // E — switch between current and previously viewed image

    // Walk the History panel's list without reordering it. The two pairs cover
    // disjoint halves of that one list: History* visits only non-starred rows,
    // Favorite* only starred rows.
    PrevHistoryFolder, // PageUp
    NextHistoryFolder, // PageDown
    NextFavoriteFolder, // Insert
    PrevFavoriteFolder, // Delete

    // The horizontal wheel walks EVERY row the panel shows — favorites and
    // non-favorites alike (WalkScope::All). That is a third scope, not either
    // half above, so it needs its own pair rather than reusing one of them.
    PrevHistoryFolderAll,
    NextHistoryFolderAll,

    // --- Runtime theme ---
    ThemeFactorUp, // Ctrl+Alt+Shift+Numpad+
    ThemeFactorDown, // Ctrl+Alt+Shift+Numpad-
    ThemeFactorReset, // Ctrl+Alt+Shift+Numpad0

    // --- Window chrome ---
    ToggleCornerPreference, // Ctrl+Shift+Numpad*   round ↔ square
    CycleBackdropType, // Ctrl+Shift+Numpad/   None→Mica→Acrylic→MicaAlt→None
    ToggleAlwaysOnTop, // Ctrl+T               always on top on/off
    OpacityUp,   // Shift+Wheel up   — window alpha, OPACITY_STEP per notch
    OpacityDown, // Shift+Wheel down

    // --- Sort order ---
    SortByName, // Ctrl+Alt+Shift+0
    SortByDate, // Ctrl+Alt+Shift+9
    SortBySize, // Ctrl+Alt+Shift+8
    SortByType, // Ctrl+Alt+Shift+7
    SortByDisk, // Ctrl+Alt+Shift+6

    // --- Slideshow ---
    SlideshowToggle, // Ctrl+F1 — start / stop
    SlideshowPauseResume, // Space   — pause / resume (slideshow only)
    SlideshowToggleLoop, // R       — toggle loop/repeat (slideshow only)
    SlideshowToggleShuffle, // S       — toggle shuffle (slideshow only)
    SlideshowCycleTransition, // T       — cycle transition type (slideshow only)

    // --- Viewport pan (W/A/S/D, no modifier) ---
    PanLeft,
    PanRight,
    PanUp,
    PanDown,

    // --- Window move (Shift+W/A/S/D) ---
    MoveWindowLeft,
    MoveWindowRight,
    MoveWindowUp,
    MoveWindowDown,

    // --- Keyboard snap to screen half (Alt+W/A/S/D) ---
    SnapLeft,
    SnapRight,
    SnapTop,
    SnapBottom,

    // --- Keyboard snap to screen quarter (Alt+Q/E/Z/C) ---
    SnapTopLeft,
    SnapTopRight,
    SnapBottomLeft,
    SnapBottomRight,

    // --- Window resize (Shift+Numpad+/- and Shift++/-) ---
    ResizeWindowLarger,
    ResizeWindowSmaller,

    // --- Autosize viewer to fill available space (Ctrl+Space) ---
    AutosizeToWorkArea,

    // --- Image info / EXIF panel (M) ---
    ShowInfo,

    // --- Jump to image by number (J) ---
    JumpToImage,

    // --- Find image by name (Ctrl+F) ---
    FindImage,

    // --- Statistics window (K) ---
    ToggleStats,

    // --- Clipboard ---
    CopyToClipboard, // Ctrl+C

    // --- Dedicated instances (src/Dedicated) ---
    ToggleDedicatedPanel,   // F8 — the Dedicated configuration panel
    ToggleRemotePanel,      // F9 — the Remote Server panel (this instance's listener)
    ToggleDedicated,        // -dedicated: separate registry/history namespace
    CmdArgsExport,          // current settings → a cmdArgs .txt
    CmdArgsImport,          // read a cmdArgs .txt and apply it
    CmdArgsGenerateShortcut,// desktop .lnk carrying those switches
    CmdArgsTest,            // validate a cmdArgs .txt, report in a dialog

    // --- Mirroring / observing (src/Rem_TCP_IP) -----------------------------
    // Two LOCAL switches, both off at every startup — a viewer that boots
    // forwarding its keystrokes to machines you forgot about would be a trap.
    MirrorToggle,      // F11 — forward my commands to my connected targets
    MirrorLocalToggle, // F12 — when mirroring, also execute here (not just forward)
    ToggleRemotesConsole,  // F10 — the slave management console

    // Remote-only, both payload-carrying, both handled headlessly in RemoteExec:
    //   observe 1|0   the CALLER asks to be added to / removed from this
    //                 instance's observer list. There is no local key and no
    //                 AppState flag — the observer vector IS the state.
    //   sync <k=v;…>  adopt the sender's view/effect state wholesale. Exists
    //                 because mirroring forwards TOGGLES, and a toggle applied
    //                 to a different starting state diverges silently.
    Observe,
    Sync,

    // --- Desktop wallpaper (context menu submenu) ---
    // Order must match Constants::Wallpaper::FILL..SPAN.
    SetWallpaperFill,
    SetWallpaperFit,
    SetWallpaperStretch,
    SetWallpaperTile,
    SetWallpaperCenter,
    SetWallpaperSpan,

    // --- Slideshow (context / tray menus) ---
    SlideshowSetInterval, // prompt for the ms between slides
    // Which transitions are in play. Contiguous — offset from
    // SetTransitionSourceFirst is a Constants::Slideshow::TransitionSource value.
    SetTransitionSourceFirst,
    SetTransitionSourceNone = SetTransitionSourceFirst, // the single picked one
    SetTransitionSourceAll,                             // every transition
    SetTransitionSourceList,                            // only the ticked ones

    // How the next one is drawn from that pool. Contiguous — offset from
    // SetTransitionOrderFirst is a Constants::Slideshow::TransitionOrder value.
    SetTransitionOrderFirst,
    SetTransitionOrderSequential = SetTransitionOrderFirst,
    SetTransitionOrderRandom,

    // Direct transition selection — one command per TransitionType, resolved by
    // offset from SetTransitionFirst. This is a RANGE rather than 21 spelled-out
    // enumerators; the span is checked against Constants::Slideshow::TRANSITION_COUNT
    // by a static_assert in CommandExecuter.cpp. Keep it last in this enum.
    SetTransitionFirst,
    SetTransitionLast = SetTransitionFirst + 20,
};

class InputManager {
    public:
        static void handleKeyboard(HWND hWnd, WPARAM wParam, LPARAM lParam);

        // THE sink. Every input path — keyboard, mouse, tray, context menu,
        // panel click, socket — ends here, which is what lets one gate at the
        // top of it mirror the whole application to another instance.
        static void ExecuteCommand(HWND hWnd, Command cmd);

        // Payload form: "do this WITH this value" rather than "open the picker
        // for this". `goto 42` and pressing J are the same command; one carries
        // the number, the other asks for it.
        //
        // The five payload commands (JumpToImage, OpenFile, FindImage, ZoomTo,
        // SlideshowSetInterval) plus Observe/Sync route here. The body lives in
        // RemoteExec, so a value that arrives from a panel and one that arrives
        // from a socket run the SAME code — previously two implementations kept
        // in step by hand.
        static void ExecuteCommand(HWND hWnd, Command cmd, const std::wstring &payload);

        // Current value of whatever `cmd` controls, read straight from `app`
        // (the source of truth), for the "OK <name>=<value>" reply a remote
        // caller gets back. Lives beside ExecuteCommand in CommandExecuter.cpp
        // because it is a SECOND switch over the same enum and the two must be
        // edited together — a command with a case there and none here reports
        // "?" rather than silently reporting nothing.
        static std::wstring GetCommandValue(HWND hWnd, Command cmd);

    private:
        static Command ResolveKeyboardKeys(UINT key, LPARAM lParam);
};
