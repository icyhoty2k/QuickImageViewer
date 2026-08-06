// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

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
    // Hide if shown, show if hidden — one command, so a REMOTE caller that
    // cannot see the screen still has a way back. HideToTray is one-way from a
    // socket: once the window is gone the only restore is the tray icon, and a
    // phone cannot click it. This always takes the keep-alive path (tray icon +
    // SW_HIDE) regardless of app.isKeepInBackground, because destroying the
    // window would take the listener with it and strand the caller.
    ToggleAppVisibility,
    // Move the window to the NEXT monitor, wrapping at the end. Monitors are
    // ordered left-to-right, then top-to-bottom, by their virtual-desktop
    // coordinates — the order the eye reads them in, not the order Windows
    // happens to enumerate them.
    MoveToNextMonitor,
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
    ToggleRemotePanel,      // F9 — the Local Server panel (this instance's listener)
    // Ctrl+F9 — My Clients: who is connected to THIS instance's listener,
    // and kick / timed kick / ban. Split out of the Local Server panel because
    // that one describes the listener's CONFIGURATION, and a live list of peers
    // is not configuration — it changes while you look at it.
    ToggleRemoteClients,
    ToggleDedicated,        // -dedicated: separate registry/history namespace
    CmdArgsExport,          // current settings → a cmdArgs .txt
    CmdArgsImport,          // read a cmdArgs .txt and apply it
    CmdArgsGenerateShortcut,// desktop .lnk carrying those switches
    CmdArgsTest,            // validate a cmdArgs .txt, report in a dialog

    // --- Mirroring / observing (src/Rem_TCP_IP) -----------------------------
    // Two LOCAL switches, both off at every startup — a viewer that boots
    // forwarding its keystrokes to machines you forgot about would be a trap.
    MirrorToggle,      // F11 — forward my commands to my connected targets
    // Ctrl+F11 — the panel that says WHICH connected instances F11 drives, and
    // carries Sync now. Toggling a window, not a flag: it can be left open while
    // mirroring goes on and off, and it never touches passCommandToRemote —
    // F11 owns that, and two places deciding one bool is how they disagree.
    MirrorPick,        // Ctrl+F11 — opens/closes the selection PANEL
    MirrorLocalToggle, // F12 — when mirroring, also execute here (not just forward)
    // ─── Three ways to put one picture on another instance's screen ──────────
    //
    // They differ in what travels and what it disturbs, and the names say which:
    //
    //   Ctrl+Enter      SendImagePositionToRemotes   a POSITION travels
    //   Alt+Enter       StreamImageToRemotes         the IMAGE BYTES travel, out
    //   Ctrl+Alt+Enter  StreamImageFromRemote        the IMAGE BYTES travel, in
    //
    // ── Ctrl+Enter. Sends a PLACE, not a picture: folder, sort order and index,
    // negotiated against what the far end already has (QueryState). Both ends
    // then hold the same picture from the same file on the same disk, and the far
    // end STAYS there.
    //
    // SAME-MACHINE ONLY, necessarily: a folder path and a playlist index are both
    // read against the far end's own filesystem.
    SendImagePositionToRemotes,
    // ── Ctrl+Shift+Enter. The same push, to EVERY CONNECTED INSTANCE, ignoring
    // the Ctrl+F11 Control ticks.
    //
    // The ticks exist so mirroring and pushing cannot reach different screens,
    // and that is right for the routine case. This is the deliberate exception:
    // lining up a whole wall at once, without first going to a panel to tick
    // rows you are about to untick again. Still same-machine only — the
    // constraint is the payload, not the target list, and a folder path does not
    // survive the trip whoever it is sent to.
    SendImagePositionToAllRemotes,
    // ── "Sync now" — stamps this viewer's whole look (folder, image, view mode,
    // zoom, effects) onto the controlled instances.
    //
    // A Command rather than a private method on the picker, because the picker's
    // button and the menu item must be the same act. Anything that reaches the
    // sender threads without passing ExecuteCommand skips the mirror gate, and a
    // second path that "also syncs" is exactly how two of them drift apart.
    MirrorSyncNow,
    // `QueryState` — READ-ONLY, and the only command in the table that is. Every
    // other one reports its value as a by-product of doing something; this one is
    // asked. The position send above is built on the answer (folder, sort order,
    // playlist length, current position), because whether an index is safe to send
    // depends entirely on whether the far end already holds the same folder in the
    // same order. No local keystroke — a viewer does not need to ask itself.
    QueryState,
    // `QueryHistory` — READ-ONLY, like QueryState. Answers with the folder
    // history this instance has on disk, so a client with no filesystem access
    // to that machine (the phone app) can offer a folder list to jump to and
    // then send `OpenFile <path>`. Without it a remote caller can only walk the
    // history blindly with Next/PrevHistoryFolder, moving the viewer at every
    // step just to discover where it could go.
    QueryHistory,
    // `QueryClients` — READ-ONLY. How many clients this instance's listener
    // currently has, against its cap, and what it is bound to. A caller can
    // otherwise only discover the cap by being refused at it.
    QueryClients,
    // `QueryToggles` — READ-ONLY. Every command's current value in one reply,
    // as `Name=value;Name=value;…`.
    //
    // Exists because a client that has just connected knows NOTHING about the
    // viewer's state, and the protocol otherwise only reveals a value as the
    // by-product of CHANGING it: the phone app's toggle buttons could not be
    // drawn correctly without first pressing all of them. Asking one command
    // at a time would be a dozen round trips over a link that may be a phone
    // on mobile data, so it is one.
    //
    // Built by walking the command TABLE and asking GetCommandValue for each
    // row, rather than from a hand-written list. A list would be a second
    // place to remember, and the failure mode is silent — a new toggle would
    // simply never initialise, on a screen nobody is looking at closely.
    // Aliases are emitted too, each carrying the same value, so a client keyed
    // on either spelling finds its entry.
    QueryToggles,
    // `ImageChanged <n>/<total> <file name>` — a NOTIFICATION, and the only
    // command in the table whose local execution is deliberately nothing.
    //
    // It exists because the picture-change event the viewer already pushes is
    // `JumpToImage <n>`, and an INDEX is meaningless to an observer that does
    // not share the folder — so EmitToObservers marks it positional and drops
    // it for every off-machine watcher. Correct for a qIV following another
    // qIV, and it left the phone app frozen on whatever frame was up when it
    // bound: a running slideshow changes picture without issuing a single
    // command, so no other event ever reached it.
    //
    // This is the machine-independent half of the same announcement. It names
    // what is being shown rather than where it sits, carries no instruction,
    // and a client's correct response is to ask (SendDisplayedImage) — which
    // is exactly what the phone's preview does.
    //
    // In the table so a qIV observer that replays events parses it instead of
    // answering ERR, and so a third-party client can look it up.
    ImageChanged,
    // ── Alt+Enter. Sends the PICTURE ITSELF — the file's bytes, in base64 chunks
    // — to be shown ONCE and change nothing else there: no folder, no sort order,
    // no playlist position. It goes up in place of the current slide and the far
    // end carries on afterwards as though it had not. An advert, in other words.
    //
    // Bytes rather than a path precisely so this works to ANOTHER MACHINE, where a
    // path means nothing. That is the whole reason it is a separate mechanism from
    // Ctrl+Enter rather than a lighter version of it.
    StreamImageToRemotes,
    // ── Ctrl+Alt+Enter. The same transfer in reverse: ask ONE instance for the
    // picture it is displaying and show it here, once. For looking at what a screen
    // in another room is showing without going to it, so it must not depend on
    // sharing a filesystem either.
    StreamImageFromRemote,
    // --- The wire half of the two streaming commands -------------------------
    //
    // A line-based protocol with a bounded line length cannot carry a whole image
    // in one request, so an OUTBOUND stream is framed across several ordinary
    // commands, each its own request and reply:
    //
    //   StreamImageBegin <file name>   start (or restart) an inbound transfer
    //   StreamImageChunk <base64>      append — repeated
    //   StreamImageShow                assemble and display it once
    //
    // The INBOUND direction needs no framing: a reply may be several lines, so
    // `SendDisplayedImage` answers with its body lines followed by its OK.
    StreamImageBegin,
    StreamImageChunk,
    StreamImageShow,
    SendDisplayedImage,
    // `SendDisplayedPreview [maxDim[;quality]]` — the same picture, DOWNSCALED
    // and re-encoded as JPEG.
    //
    // Exists because SendDisplayedImage sends the original file, and for a phone
    // that is 25-40x more bytes than the screen can use: a 6 MB wallpaper became
    // ~8 MB of base64 to draw something shown at about 1080 px, which the phone
    // then had to decode at full size.
    //
    // NOT a replacement. The original is what Save writes, and a re-encoded JPEG
    // is not the file the user asked to keep — so a client displays the preview
    // and asks for the original only when saving.
    SendDisplayedPreview,
    // `ShowImageOnce <full path>` — the same one-shot display, but naming a file
    // the far end can already read. No transfer, so it costs nothing on one
    // machine; useless across two. Kept because it is the honest spelling for a
    // script or a hand-sent line, and because the streaming path assembles into
    // exactly the same mechanism (AppState::Interjection, FileHandler.h).
    ShowImageOnce,
    ToggleRemotesConsole,  // F10 — Remote Servers, the slave management console
    ToggleRemoteCmd,       // Ctrl+F10 — the Send Command panel. LOCAL, like the
                           // other panel toggles: no table row, never mirrored.
    ToggleRemoteLog,       // Ctrl+F12 — open/close the RemoteLog panel. LOCAL:
                           // it has no table row, so it is never mirrored — one
                           // keypress must not open a window on every screen.
    // Announce this instance's Local Server on the network so clients can find
    // it. LOCAL by design: no table row, therefore never mirrored and not
    // reachable over the wire.
    //
    // Deliberate. A remote client being able to switch on the beacon means one
    // connection can make this machine advertise itself to the whole network —
    // a decision about visibility that belongs to the person at the keyboard,
    // not to whoever got in first. It is one tick in the TCP/IP menu.
    ToggleRemoteBeacon,
    // Write the wire log to rotating files under logs\. LOCAL by design, for
    // the same reason as the beacon above and a sharper one: a remote client
    // able to switch this on could make the machine write to its disk
    // indefinitely, which is a decision about somebody else's storage. It is
    // one tick in the TCP/IP menu.
    //
    // Separate from EnableRemoteLog below, which is the RECORDING switch. That
    // one decides whether exchanges are captured at all; this one decides
    // whether captured exchanges also reach a file. Nothing is written while
    // recording is off, and the toggle says so rather than leaving an empty
    // file behind.
    ToggleRemoteLogFile,
    // The General log — what this instance did. LOCAL for the same reason as the
    // one above: a remote client that could switch it on would be deciding to
    // write to somebody else's disk indefinitely.
    ToggleGeneralLog,
    // Open logs\ in Explorer. LOCAL, and firmly so: a remote client able to
    // open a shell window on somebody else's desktop is not a logging feature.
    OpenLogFolder,
    // The RECORDING switch, and a separate command from the panel that shows it.
    // Payload-carrying (`enablelog 0|1`) rather than a toggle, because it is
    // sent to the other instances: a toggle applied to ends that disagree makes
    // them disagree in the opposite direction, and the whole point of turning
    // logging on across a session is that every end records the same exchanges.
    EnableRemoteLog,
    // `msg <text>` — put a centre-screen message on the RECEIVING instance.
    //
    // Exists because a wall of identical viewers is anonymous: two windows side
    // by side give no clue which row in the F10 console drives which screen.
    // The console's Identify button sends each target its own name, and the
    // screen says who it is.
    //
    // Remote-only in practice: it carries a payload, and there is no local
    // keystroke for it — a viewer already knows what it is called.
    msgRemote,

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
