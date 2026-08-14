// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#pragma once
#include <windows.h>
#include "Platform/Constants.h"

// =============================================================================
// AppMenuIds — the menu's numbering, and the ONE place it is written down.
//
// WHY THIS FILE EXISTS. The ids used to live in two files: the builder declared
// constants like TRAY_ID_OVERLAY_OFF_BASE = 67 with the comment "values must
// match the cases in TrayHandler::DispatchCommand", and the switch in that other
// file spelled 67 again as a bare literal. Nothing checked the agreement. Move
// one band and the menu silently starts doing something else.
//
// Now both halves include this header, so a band is defined once and the
// arithmetic that decodes it sits beside the constant that anchors it.
//
// -----------------------------------------------------------------------------
// TWO DISJOINT RANGES, ONE MENU.
//
//   < VIEWER_BASE   settings and application state, handled by DispatchSetting
//                   in this folder: toggles, numeric prompts, overlays, sort,
//                   export / import / restore / backup.
//
//   >= VIEWER_BASE  viewer actions. Every one maps to a Command and runs through
//                   InputManager::ExecuteCommand, so a menu pick is identical to
//                   the keyboard shortcut — including the mirror gate, which a
//                   private implementation here would bypass.
//
// Keeping them disjoint is what lets one menu dispatch to the right half without
// either side knowing the other's numbering.
// =============================================================================

namespace UI::AppMenu::Ids {

    constexpr int VIEWER_BASE = 1000;

    // ── Viewer ids (>= VIEWER_BASE) ─────────────────────────────────────────
    enum MenuId : int {
        ID_BROWSE = VIEWER_BASE,  // open-file dialog                (F2)
        ID_HISTORY,               // toggle HistoryListWnd           (Tab)
        ID_THUMB_STRIP,           // toggle DirWnd thumbnail strip   (F6)
        ID_PREV_FOLDER,           // switch to previous folder       (Q)
        ID_PREV_IMAGE,            // switch to previously viewed img (E)
        ID_STATS,                 // toggle StatsWnd                 (K)
        ID_METADATA,              // toggle image info / metadata    (M)
        ID_COPY,                  // copy image to clipboard         (Ctrl+C)
        ID_COPY_PATH,             // copy full path as text          (Ctrl+Shift+C)
        ID_OPEN_WITH,             // Windows "Open with" chooser     (Ctrl+Shift+O)
        ID_SAVE_AS,               // save image as…                  (Ctrl+S)
        ID_EXPLORER,              // reveal current file in Explorer (L)
        ID_NEXT_MONITOR,          // move the window to the next monitor (Ctrl+M)
        ID_HELP,                  // help / shortcuts window         (F1)
        ID_CLOSE_APP,             // hide to tray if kept in bg      (Esc)
        ID_CLOSE_PANELS,          // hide all panels + spawned DirWnds
        ID_RESTORE_PANELS,        // re-open the panels that hid
        ID_HARD_QUIT,             // full process exit               (Ctrl+Q)
        ID_SS_TOGGLE,             // slideshow start / stop          (Ctrl+F1)
        ID_SS_INTERVAL,           // prompt for slide duration
        ID_SS_LOOP,               // loop toggle                     (R)
        ID_SS_SHUFFLE,            // playlist shuffle toggle         (S)
        ID_DEDICATED_PANEL,       // open the Dedicated panel        (F8)
        ID_REMOTE_PANEL,          // open the Local Server panel     (F9)
        ID_REMOTES_CONSOLE,       // open the Remote Servers console (F10)
        ID_REMOTES_CONTROL,       // open the Mirroring panel       (Ctrl+F11)
        ID_REMOTE_CMD,            // open the Send Command panel     (Ctrl+F10)
        ID_REMOTE_LOG,            // open the RemoteLog panel        (Ctrl+F12)
        ID_REMOTE_BEACON,         // announce this server on the network, CHECKABLE
        ID_REMOTE_LOG_FILE,       // write the TCP/IP wire log to logs\, CHECKABLE
        ID_APP_LOG_FILE,          // write the General app log to logs\, CHECKABLE
        ID_OPEN_LOG_DIR,          // open logs\ in Explorer
        // RemoteActivation submenu — the two mirroring switches, CHECKABLE.
        // They are the only settings in this app with no visible resting state:
        // F11 and F12 report themselves on an overlay that fades, so the only
        // way to check whether mirroring is on was to press the key and read
        // what it said — which also changed it. A menu that merely SHOWS them
        // is the fix; being able to set them from there too is the bonus.
        ID_REMOTE_MIRROR,         // F11 — forward my commands
        ID_REMOTE_EXEC_HERE,      // F12 — while mirroring, also execute locally
        // The rest of Remote Bindings: the acts, beside the two switches. Menu
        // entries for keys that already exist — every one resolves to the same
        // Command its shortcut does, so there is no second implementation to
        // drift.
        ID_REMOTE_SYNC_NOW,       // stamp this viewer's look on the controlled ones
        ID_REMOTE_PUSH_POS,       // Ctrl+Enter       — position, ticked screens
        ID_REMOTE_PUSH_POS_ALL,   // Ctrl+Shift+Enter — position, every connected
        ID_REMOTE_STREAM_OUT,     // Alt+Enter        — bytes out
        ID_REMOTE_STREAM_IN,      // Ctrl+Alt+Enter   — bytes in

        ID_REMOTE_CLIENTS,        // open the My Clients panel      (Ctrl+F9)

        // Contiguous blocks. Each resolves to its Command by offset, so a block
        // never needs one case per member — see CommandForId.
        ID_WALLPAPER_FIRST = VIEWER_BASE + 100,
        ID_WALLPAPER_LAST  = ID_WALLPAPER_FIRST + Constants::Wallpaper::COUNT - 1,

        ID_TRANSITION_FIRST = VIEWER_BASE + 200,
        ID_TRANSITION_LAST  = ID_TRANSITION_FIRST + Constants::Slideshow::TRANSITION_COUNT - 1,

        ID_TRANS_SRC_FIRST = VIEWER_BASE + 300,
        ID_TRANS_SRC_LAST  = ID_TRANS_SRC_FIRST +
                             Constants::Slideshow::TransitionSource::COUNT - 1,

        ID_TRANS_ORD_FIRST = VIEWER_BASE + 400,
        ID_TRANS_ORD_LAST  = ID_TRANS_ORD_FIRST +
                             Constants::Slideshow::TransitionOrder::COUNT - 1,

        // In the VIEWER space on purpose: picking a view mode has side effects
        // beyond assigning the field (it re-clamps the pan offset), and a second
        // copy in the settings space had already drifted out of sync once —
        // picking "Fit to View" from the menu after panning left the image
        // off-centre where the 1 key centred it.
        ID_VIEW_MODE_FIRST = VIEWER_BASE + 500,
        ID_VIEW_MODE_LAST  = ID_VIEW_MODE_FIRST + 4,
    };

    // The scalar run above auto-numbers from VIEWER_BASE, so every id added to it
    // walks one step closer to the first band. Adding the hundredth would land ON
    // ID_WALLPAPER_FIRST and turn a menu click into a wallpaper mode — silently,
    // because both sides are just ints. Fail the build instead.
    static_assert(ID_REMOTE_CLIENTS < ID_WALLPAPER_FIRST,
                  "the scalar viewer ids have grown into the wallpaper band");

    // ── Settings ids (< VIEWER_BASE) ────────────────────────────────────────
    // Historically bare numbers on both sides of a file boundary. Named here so
    // the builder and the dispatcher refer to the same thing by the same name.
    //
    // Two groups, kept apart on purpose:
    //
    //   1 .. SCALAR_LAST      one id per item. Order is historical and there is
    //                         no meaning in the gaps — items have come and gone.
    //   OVERLAY_BASE ..       the overlay BANDS: runs of contiguous ids decoded
    //                         by arithmetic rather than by one case each.
    //
    // The split exists because a band and a scalar cannot safely share a number.
    // They did: Lock Viewport was 67, and so was the first id of the overlay
    // "Off" band. Nothing broke, because the band is decoded before the switch
    // and only a slot submenu ever emitted those ids — but the two were one
    // reordering apart from silently becoming each other, and the compiler could
    // not see it. Now the bands start well above every scalar, and the
    // static_asserts at the bottom of this file enforce it.
    enum SettingsId : int {
        SET_RESTORE_WINDOW   = 1,
        SET_SHOW_HELP        = 2,
        SET_EXIT             = 3,
        SET_KEEP_IN_BG       = 4,
        SET_RUN_ON_STARTUP   = 5,
        SET_THUMB_EFFECTS    = 6,
        SET_HISTORY_FULL     = 7,
        SET_OVERLAY_MASTER   = 8,
        SET_OPEN_DIRWND      = 9,
        SET_EXPORT           = 10,
        SET_IMPORT           = 11,
        SET_RESTORE_DEFAULTS = 12,
        SET_OVERLAY_BG       = 13,
        SET_SWAP_MOUSE       = 14,
        SET_WHEEL_INVERT     = 15,
        SET_WHEEL_INVERT_H   = 16,
        SET_VRAM_CACHE       = 17,
        SET_WINDOW_WIDTH     = 23,
        SET_WINDOW_HEIGHT    = 24,
        SET_START_FULLSCREEN = 25,
        SET_HISTORY_MAX_DIRS = 26,
        SET_HISTORY_MAX_FAVS = 27,
        SET_DIR_THUMB_CACHE  = 28,
        SET_PRELOAD_LOOKASIDE= 29,
        SET_MSG_DURATION     = 30,
        SET_HISTORY_SAVE_MAX = 31,
        // 32-40 were the gap between the scalars and SET_BACKUP.
        SET_HISTORY_ENABLED     = 32,
        SET_HISTORY_IMAGES_ONLY = 33,
        SET_HISTORY_CLEAR       = 34,
        SET_HISTORY_REMOVE_BAD  = 35,
        SET_HISTORY_DEDUPE      = 36,
        SET_HISTORY_OPEN_FILE   = 37,
        SET_HISTORY_FAVS_SHOWN  = 38,
        SET_BACKUP_LOGS         = 39,
        SET_BACKUP_REMOTE       = 40,
        // NOT 43 — that is SET_SORT_FIRST, and 43-48 is the contiguous sort band
        // decoded by arithmetic. 41/42 are the history backup pair, so this one
        // lands past the thumbnail-operation run instead of renumbering anything.
        SET_RESTORE_REMOTE      = 54,
        SET_HISTORY_CLEAR_FAVS  = 55,
        SET_HISTORY_CLEAR_BOTH  = 56,
        SET_BACKUP           = 41,
        SET_RESTORE_BACKUP   = 42,

        // Sort order: contiguous, one per order, then the reverse toggle.
        SET_SORT_FIRST       = 43,   // Name
        SET_SORT_LAST        = 47,   // Disk Order
        SET_SORT_REVERSE     = 48,

        SET_CTRL_C           = 49,
        SET_THUMB_COPY       = 50,
        SET_THUMB_MOVE       = 51,
        SET_THUMB_DELETE     = 52,
        SET_THUMB_PASTE      = 53,
        SET_CARET_BAR        = 60,
        SET_CARET_UNDERSCORE = 61,
        SET_ZOOM_CLICK       = 62,
        SET_CONTEXT_MENU     = 63,
        SET_KIOSK_LOCK       = 64,
        SET_ALWAYS_ON_TOP    = 65,
        SET_KEEP_AWAKE       = 66,
        // Moved off 67, which the overlay "Off" band also claimed.
        SET_LOCK_VIEWPORT    = 18,
        SET_REMEMBER_WIN_POS = 19,
        // 20-22 were the only gap left in the low band; 67 is spoken for by the
        // overlay "Off" row, so this takes the next free low id rather than
        // starting a new range for one item.
        SET_FOLDER_WALK_WRAP = 20,

        // "Location = Registry / File". Reports where settings actually live
        // and opens it — regedit at the key, or Explorer with the .ini selected.
        SET_LOCATION         = 68,

        // Highest scalar id. The bands below must start above it — asserted.
        SET_SCALAR_LAST      = SET_LOCATION,

        // ── Overlay bands ───────────────────────────────────────────────────
        // Based well clear of the scalars, with room to grow on both sides.
        // Everything from here to OVERLAY_BAND_LAST is decoded by arithmetic.
        SET_OVERLAY_BASE = 200,

        // Slot state: three contiguous runs of nine, one entry per slot.
        //   200-208 Off,  209-217 Full (visible, not compact),  218-226 Compact
        SET_OVERLAY_OFF_BASE     = SET_OVERLAY_BASE,
        SET_OVERLAY_FULL_BASE    = SET_OVERLAY_OFF_BASE     + 9,
        SET_OVERLAY_COMPACT_BASE = SET_OVERLAY_FULL_BASE    + 9,

        // Layout modes follow the slot runs immediately — the decoder relies on
        // that adjacency to tell "a slot" from "a layout" with one comparison.
        SET_LAYOUT_GRID    = SET_OVERLAY_COMPACT_BASE + 9,
        SET_LAYOUT_STACKED = SET_LAYOUT_GRID + 1,
        SET_LAYOUT_SUMMARY = SET_LAYOUT_GRID + 2,

        // ── Overlay scalars ─────────────────────────────────────────────────
        // Outside the arithmetic band on purpose: they dispatch as ordinary
        // cases. The two BOT_LEFT readouts are independent of that slot's own
        // Compact/Full/Off state, and the font settings apply to every slot.
        SET_OVERLAY_EFFECTS_LIST = 230,
        SET_OVERLAY_DIR_NAME     = 231,
        SET_OVERLAY_FONT_SIZE    = 232,
        SET_OVERLAY_FONT_COLOR   = 233,
        // The other spelling of SET_OVERLAY_DIR_NAME's line — exclusive with it.
        SET_OVERLAY_FULL_PATH    = 234,

        // Contiguous, one id per entry in Constants::Overlay::OVERLAY_FONT_FAMILIES.
        // Its own band, checked separately from the slot band above.
        SET_OVERLAY_FONT_FAMILY_BASE = 240,
    };

    constexpr int OVERLAY_SLOT_COUNT = 9;

    // The arithmetic band as one range, so the decoder and any future reader
    // test the same bounds instead of re-deriving them.
    constexpr int OVERLAY_BAND_FIRST = SET_OVERLAY_OFF_BASE;
    constexpr int OVERLAY_BAND_LAST  = SET_LAYOUT_SUMMARY;

    // =========================================================================
    // The layout rules this file depends on, checked by the compiler.
    //
    // Every one of these was previously a fact you had to hold in your head
    // while editing — and the first of them was already violated.
    // =========================================================================

    // No band may reach down into the scalars.
    static_assert(SET_OVERLAY_BASE > SET_SCALAR_LAST,
                  "the overlay band overlaps a scalar settings id");
    static_assert(SET_LOCK_VIEWPORT < SET_OVERLAY_BASE,
                  "Lock Viewport fell back into the overlay band");
    static_assert(SET_REMEMBER_WIN_POS < SET_OVERLAY_BASE,
                  "Remember Window Position fell back into the overlay band");
    static_assert(SET_REMEMBER_WIN_POS != SET_LOCK_VIEWPORT,
                  "Remember Window Position reused an occupied settings id");

    // The three slot runs must be adjacent and nine wide: the decoder recovers
    // the slot with (id - OFF_BASE) % 9 and the state with (id - OFF_BASE) / 9,
    // which is only correct while that holds.
    static_assert(SET_OVERLAY_FULL_BASE    == SET_OVERLAY_OFF_BASE  + OVERLAY_SLOT_COUNT, "");
    static_assert(SET_OVERLAY_COMPACT_BASE == SET_OVERLAY_FULL_BASE + OVERLAY_SLOT_COUNT, "");

    // Layout ids sit immediately after the last slot run — the decoder splits
    // the two on exactly that boundary.
    static_assert(SET_LAYOUT_GRID == SET_OVERLAY_COMPACT_BASE + OVERLAY_SLOT_COUNT,
                  "layout ids must directly follow the compact slot run");

    // The overlay scalars must be clear of the arithmetic band, or a font-size
    // click would be read as a slot.
    static_assert(SET_OVERLAY_EFFECTS_LIST > OVERLAY_BAND_LAST,
                  "an overlay scalar fell inside the arithmetic band");

    // The font-family run must clear the scalars above it and stay below the
    // viewer space, whatever the family list grows to.
    static_assert(SET_OVERLAY_FONT_FAMILY_BASE > SET_OVERLAY_FONT_COLOR, "");
    static_assert(SET_OVERLAY_FONT_FAMILY_BASE +
                      Constants::Overlay::OVERLAY_FONT_FAMILY_COUNT <= VIEWER_BASE,
                  "the font-family band runs into the viewer id space");

    // Sort is its own small run, decoded by subtraction from SET_SORT_FIRST.
    static_assert(SET_SORT_LAST > SET_SORT_FIRST, "");
    static_assert(SET_SORT_REVERSE == SET_SORT_LAST + 1,
                  "Reverse Order must follow the sort run");

} // namespace UI::AppMenu::Ids
