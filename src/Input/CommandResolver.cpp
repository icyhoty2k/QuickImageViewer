// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

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

Command InputManager::ResolveKeyboardKeys(UINT key, LPARAM lParam) {
    // Extract scan code from bits 16-23
    BYTE scanCode = (lParam >> 16) & 0xFF;

    // Check for Right Shift specifically as a shortcut

    bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    if (key == VK_SHIFT && scanCode == Shortcuts::SC_RIGHT_SHIFT_SCANCODE) {
        return Command::ToggleDir;
    }

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
    // Per-slot state cycle  Ctrl+1..9  and  Ctrl+0 (master)   (no alt, no shift)
    // Ctrl+N walks Compact → Full → Off for that slot. There is no separate
    // compact shortcut — the two were merged into this one tri-state cycle.
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
            case Shortcuts::SC_THEME_FACTOR_UP: return Command::ThemeFactorUp;
            case Shortcuts::SC_THEME_FACTOR_DOWN: return Command::ThemeFactorDown;
            case Shortcuts::SC_THEME_FACTOR_RESET: return Command::ThemeFactorReset;
        }
    }

    // -------------------------------------------------------------------------
    // Window chrome  Ctrl+Shift+Numpad* / Numpad/
    // -------------------------------------------------------------------------
    if (ctrl && shift && !alt) {
        switch (key) {
            case Shortcuts::SC_CORNER_PREFERENCE_TOGGLE: return Command::ToggleCornerPreference;
            case Shortcuts::SC_BACKDROP_TYPE_CYCLE: return Command::CycleBackdropType;
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
        if (key == Shortcuts::SC_SLIDESHOW_PAUSE_RESUME) return Command::SlideshowPauseResume;
        if (key == Shortcuts::SC_SLIDESHOW_LOOP_TOGGLE) return Command::SlideshowToggleLoop;
        if (key == Shortcuts::SC_SLIDESHOW_SHUFFLE_TOGGLE) return Command::SlideshowToggleShuffle;
        if (key == Shortcuts::SC_SLIDESHOW_TRANSITION_CYCLE) return Command::SlideshowCycleTransition;
    }

    // -------------------------------------------------------------------------
    // All other keys
    // -------------------------------------------------------------------------
    switch (key) {
        // --- Navigation ---
        // The four arrows carry the FOLDER-TREE walk under Alt, and the image
        // walk without it. Alt+Up is Explorer's own "up one level", so the pair
        // people already have in their fingers is the pair that works here.
        case Shortcuts::SC_NAV_NEXT:
            if (alt && !ctrl && !shift) return Command::FolderNextSibling;
            return Command::NextImage;
        case Shortcuts::SC_NAV_PREV:
            if (alt && !ctrl && !shift) return Command::FolderPrevSibling;
            return Command::PrevImage;
        case Shortcuts::SC_NAV_FOLDER_UP:
            if (alt && !ctrl && !shift) return Command::FolderUp;
            break;
        case Shortcuts::SC_NAV_FOLDER_DOWN:
            if (alt && !ctrl && !shift) return Command::FolderDown;
            break;
        //smart jump to first or last image depending which is further
        case Shortcuts::SC_NAV_TOGGLE_FIRST_LAST_IMAGE_IN_CURR_FOLDER:
            return shift ? Command::GoToLastImageInCurrentFolder : Command::ToggleFirstLastImageInCurrentFolder;

        case Shortcuts::SC_NAV_NEXT_SPACE:
            if (ctrl && !alt && !shift) return Command::AutosizeToWorkArea;
            return shift ? Command::PrevImage : Command::NextImage;

        case Shortcuts::SC_NAV_SHOW_IN_EXPLORER: // 'L'
            return Command::ShowInExplorer;

        case Shortcuts::SC_TOGGLE_LAST_IMAGE: // 'E'
            if (!ctrl && !alt && !shift) return Command::ToggleLastImage;
            if (!ctrl && alt && !shift) return Command::SnapTopRight;
            break;

        // --- Zoom ---
        case Shortcuts::SC_ZOOM_IN_NUMPAD: // VK_ADD  plain=zoom-in  shift=resize-larger
            if (shift) return Command::ResizeWindowLarger;
            return Command::ZoomIn;
        case Shortcuts::SC_ZOOM_OUT_NUMPAD: // VK_SUBTRACT  plain=zoom-out  shift=resize-smaller
            if (shift) return Command::ResizeWindowSmaller;
            return Command::ZoomOut;
        case Shortcuts::SC_ZOOM_RESET: return Command::ZoomReset;
        case Shortcuts::SC_ZOOM_TO: // '0' plain
            if (!ctrl && !alt && !shift) return Command::ZoomTo;
            break;

        // --- Transform ---
        case Shortcuts::SC_TRANSFORM_ROTATE:
            return shift ? Command::RotateCCW : Command::RotateCW;

        case Shortcuts::SC_TRANSFORM_FLIP_H: return Command::FlipH;
        case Shortcuts::SC_TRANSFORM_FLIP_V: return Command::FlipV;
        case Shortcuts::SC_THUMBNAIL_WRAP_TOGGLE:
            if (!ctrl && !alt && !shift) return Command::ToggleThumbnailWrapAround;
            break;

        case Shortcuts::SC_THUMBNAIL_EFFECTS_TOGGLE:
            if (!ctrl && !alt && !shift) return Command::ToggleThumbnailEffects;
            break;

        case Shortcuts::SC_VIEWPORT_LOCK: // 'Y'
            if (!ctrl && !alt && !shift) return Command::ToggleViewportLock;
            break;

        // --- Mirroring (F11 / F12) ---
        // Ctrl+F11 opens the selection PANEL instead of toggling: same key,
        // because it is the same subject, and the modified form is the one you
        // reach for while mirroring is already on — when plain F11 would switch
        // it off. Ctrl rather than Shift, matching the other panel shortcuts.
        case Shortcuts::SC_MIRROR_TOGGLE:
            return ctrl ? Command::MirrorPick : Command::MirrorToggle;
        // Ctrl+F12 opens the wire log, the same way Ctrl+F11 opens the target
        // selection: the plain key is the toggle, the Ctrl form is its panel.
        case Shortcuts::SC_MIRROR_LOCAL_TOGGLE:
            return ctrl ? Command::ToggleRemoteLog : Command::MirrorLocalToggle;

        // --- Fullscreen ---
        case Shortcuts::SC_PANEL_FULLSCREEN_F: // 'F' — same value as SC_NAV_FIND
            if (ctrl && !alt && !shift) return Command::FindImage; // Ctrl+F → find
            if (!ctrl && !alt && !shift) return Command::ToggleFullscreen; // F → fullscreen
            break;
        // One key, three depths (Shortcuts.h):
        //   Ctrl+Enter        go to MY picture there    — position, ticked screens
        //   Ctrl+Shift+Enter  the same, to EVERY connected instance
        //   Alt+Enter         show MY picture there     — bytes, any machine
        //   Ctrl+Alt+Enter    show ITS picture here     — bytes, any machine
        // Fullscreen keeps plain Enter, and also 'F' and Ctrl+Shift+T.
        case Shortcuts::SC_PANEL_FULLSCREEN_ENTER:
            if (ctrl && alt && !shift) return Command::StreamImageFromRemote;
            if (ctrl && !alt && !shift) return Command::SendImagePositionToRemotes;
            // Shift WIDENS the plain Ctrl form from the ticked screens to every
            // connected one — the same relationship Shift has elsewhere in this
            // resolver, and the reason it reads as the fourth depth of one key
            // rather than a separate binding.
            if (ctrl && shift && !alt) return Command::SendImagePositionToAllRemotes;
            if (alt && !ctrl && !shift) return Command::StreamImageToRemotes;
            if (!ctrl && !alt) return Command::ToggleFullscreen;
            break;

        case Shortcuts::SC_PANEL_FULLSCREEN_T: // 'T' — shared key
            if (ctrl && shift) return Command::ToggleFullscreen;
            if (ctrl && !shift && !alt) return Command::ToggleAlwaysOnTop;
            break;

        // --- Panels ---
        case Shortcuts::SC_PANEL_HELP_TOGGLE: return Command::ToggleHelp;
        case Shortcuts::SC_PANEL_OPEN_FILE: return Command::OpenFile;
        case Shortcuts::SC_APP_RELOAD_CURRENT_DIR: return Command::ReloadCurrentDir;
        // F3 plain — toggle the cache panel;  Ctrl+F3 — clear the cache.
        case Shortcuts::SC_PANEL_CACHE_TOGGLE: // == SC_PANEL_CACHE_CLEAR
            if (ctrl && !alt && !shift) return Command::ClearCache;
            if (!ctrl && !alt && !shift) return Command::ToggleCache;
            break;
        case Shortcuts::SC_PANEL_DIR_TOGGLE: return Command::ToggleDir;
        // F4 / F7 — the move siblings of F3 / F6. Resolved here so every
        // documented key has exactly one owner; the panels keep their own
        // handler for the focused case, exactly as they do for the toggles.
        case Shortcuts::SC_PANEL_CACHE_MOVE: return Command::MoveCacheWnd;
        case Shortcuts::SC_PANEL_DIR_MOVE:   return Command::MoveDirWnd;
        case Shortcuts::SC_PANEL_DEDICATED_TOGGLE: return Command::ToggleDedicatedPanel;
        // Same shape as the three keys below: plain key = the panel about the
        // subject, Ctrl form = the live view of it.
        case Shortcuts::SC_PANEL_REMOTE_TOGGLE:
            return ctrl ? Command::ToggleRemoteClients : Command::ToggleRemotePanel;
        // Ctrl+F10 sends a typed command, the same way Ctrl+F11 picks targets
        // and Ctrl+F12 shows the wire: the plain key is the panel about the
        // subject, the Ctrl form is the thing you DO with it.
        case Shortcuts::SC_PANEL_REMOTES_CONSOLE:
            return ctrl ? Command::ToggleRemoteCmd : Command::ToggleRemotesConsole;
        case Shortcuts::SC_PANEL_HISTORY_TOGGLE:
            if (ctrl) return Command::ToggleHistoryFull;
            return Command::ToggleHistory;

        // --- 'N' — Ctrl+N new window, plain = toggle all panels (close ↔ restore) ---
        case Shortcuts::SC_PANEL_OVERLAY_TOGGLE: // 'N' (== SC_TOGGLE_ALL_PANELS)
            if (ctrl && !alt && !shift) return Command::NewWindow;
            if (!ctrl && !alt && !shift) return Command::ToggleAllPanels;
            break;

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
            if (ctrl) return Command::HideToTray;
            if (!ctrl && alt && !shift) return Command::SnapTop;
            if (!ctrl && !alt && !shift) return Command::PanUp;
            if (!ctrl && !alt && shift) return Command::MoveWindowUp;
            break;

        case Shortcuts::SC_APP_HARD_QUIT: // 'Q'
            if (ctrl) return Command::HardQuit;
            if (!ctrl && alt && !shift) return Command::SnapTopLeft;
            break;

        // --- Color effect toggles ---
        // The six named effects all require ctrl (no alt, no shift). Their plain
        // presses belong to the navigation cluster — see the cases below.
        case Shortcuts::ImageEffects::SC_EFFECT_APPLY_TOGGLE: return Command::ToggleEffectPreview;

        case Shortcuts::SC_APP_RESET_DEFAULTS: // VK_DELETE  shift=reset-all  ctrl=desaturate  plain=prev favorite
            if (shift && !ctrl && !alt) return Command::ResetAll;
            if (ctrl && !alt && !shift) return Command::ToggleGrayscale;
            if (!ctrl && !alt && !shift) return Command::NextFavoriteFolder;
            break;

        case Shortcuts::ImageEffects::SC_COLOR_INVERT_OR_NAVIGATE_FAVS_PREV: // VK_INSERT  ctrl=invert  plain=next favorite
            if (ctrl && !alt && !shift) return Command::ToggleInvert;
            if (!ctrl && !alt && !shift) return Command::PrevFavoriteFolder;
            break;

        case Shortcuts::ImageEffects::SC_COLOR_SEPIA_OR_SC_NAV_FIRST_IMAGE: // VK_HOME  ctrl=sepia  plain=first image
            if (ctrl && !alt && !shift) return Command::ToggleSepia;
            if (!ctrl && !alt && !shift) return Command::GoToFirstImage;
            break;

        case Shortcuts::ImageEffects::SC_COLOR_SOLARIZE_OR_SC_NAV_LAST_IMAGE: // VK_END  ctrl=solarize  plain=last image
            if (ctrl && !alt && !shift) return Command::ToggleSolarize;
            if (!ctrl && !alt && !shift) return Command::GoToLastImage;
            break;

        case Shortcuts::ImageEffects::SC_COLOR_OUTLINE_OR_NAV_PREV_HISTORY_FOLDER: // VK_PRIOR  ctrl=outline  plain=prev history folder
            if (ctrl && !alt && !shift) return Command::ToggleOutline;
            if (!ctrl && !alt && !shift) return Command::PrevHistoryFolder;
            break;

        case Shortcuts::ImageEffects::SC_COLOR_THRESHOLD_OR_NAV_NEXT_HISTORY_FOLDER: // VK_NEXT  ctrl=threshold  plain=next history folder
            if (ctrl && !alt && !shift) return Command::ToggleThreshold;
            if (!ctrl && !alt && !shift) return Command::NextHistoryFolder;
            break;

        // --- Continuous adjustments ---
        // Like the six named effects, all four adjustments require ctrl (no alt,
        // no shift). Gamma keeps its Shift= window-resize meaning on the same keys.
        case Shortcuts::ImageEffects::SC_COLOR_GAMMA_UP: // VK_OEM_PLUS  ctrl=gamma-up  shift=resize-larger
            if (shift && !ctrl && !alt) return Command::ResizeWindowLarger;
            if (ctrl && !alt && !shift) return Command::GammaUp;
            break;
        case Shortcuts::ImageEffects::SC_COLOR_GAMMA_DOWN: // VK_OEM_MINUS  ctrl=gamma-down  shift=resize-smaller
            if (shift && !ctrl && !alt) return Command::ResizeWindowSmaller;
            if (ctrl && !alt && !shift) return Command::GammaDown;
            break;
        case Shortcuts::ImageEffects::SC_COLOR_BRIGHTNESS_UP: // VK_OEM_5  backslash
            if (ctrl && !alt && !shift) return Command::BrightnessUp;
            break;
        case Shortcuts::ImageEffects::SC_COLOR_BRIGHTNESS_DOWN: // VK_OEM_7  apostrophe
            if (ctrl && !alt && !shift) return Command::BrightnessDown;
            break;
        case Shortcuts::ImageEffects::SC_COLOR_CONTRAST_UP: // VK_OEM_2  forward slash
            if (ctrl && !alt && !shift) return Command::ContrastUp;
            break;
        case Shortcuts::ImageEffects::SC_COLOR_CONTRAST_DOWN: // VK_OEM_PERIOD
            if (ctrl && !alt && !shift) return Command::ContrastDown;
            break;
        case Shortcuts::ImageEffects::SC_COLOR_SAT_UP: // VK_OEM_6  ]
            if (ctrl && !alt && !shift) return Command::SaturationUp;
            break;
        case Shortcuts::ImageEffects::SC_COLOR_SAT_DOWN: // VK_OEM_4  [
            if (ctrl && !alt && !shift) return Command::SaturationDown;
            break;


        // --- Save / reset ---
        case Shortcuts::ImageEffects::SC_COLOR_RESET_ALL_EFFECTS: return Command::ResetEffects;

        case Shortcuts::ImageEffects::SC_COLOR_SAVE_TO_DISK: // 'S'  ctrl=save  plain=pan-down  shift=move-down  alt=snap-bottom
            if (ctrl) return Command::SaveImage;
            if (!ctrl && alt && !shift) return Command::SnapBottom;
            if (!ctrl && !alt && !shift) return Command::PanDown;
            if (!ctrl && !alt && shift) return Command::MoveWindowDown;
            break;

        case Shortcuts::SC_PAN_LEFT: // 'A'  ctrl=always-on-top  plain=pan-left  shift=move-left  alt=snap-left
            if (ctrl && !alt && !shift) return Command::ToggleAlwaysOnTop;
            if (!ctrl && alt && !shift) return Command::SnapLeft;
            if (!ctrl && !alt && !shift) return Command::PanLeft;
            if (!ctrl && !alt && shift) return Command::MoveWindowLeft;
            break;

        case Shortcuts::SC_PAN_RIGHT: // 'D'  plain=pan-right  shift=move-right  alt=snap-right
            if (!ctrl && alt && !shift) return Command::SnapRight;
            if (!ctrl && !alt && !shift) return Command::PanRight;
            if (!ctrl && !alt && shift) return Command::MoveWindowRight;
            break;

        case Shortcuts::SC_SNAP_QUARTER_BOTTOM_LEFT: // 'Z'
            if (!ctrl && alt && !shift) return Command::SnapBottomLeft;
            break;

        case 'C':
            if (ctrl && !alt && !shift && app.ctrlCEnabled) return Command::CopyToClipboard;
            if (!ctrl && alt && !shift) return Command::SnapBottomRight;
            break;

        case Shortcuts::SC_WINDOW_RESET_DEFAULTS: // 'X'
            if (!ctrl && alt && !shift) return Command::ResetAll;
            break;

        case Shortcuts::SC_SHOW_INFO: // 'M'  plain=info panel  ctrl=next monitor
            if (!ctrl && !alt && !shift) return Command::ShowInfo;
            if (ctrl && !alt && !shift) return Command::MoveToNextMonitor;
            break;

        case Shortcuts::SC_NAV_JUMP_TO_IMAGE: // 'J'
            if (!ctrl && !alt && !shift) return Command::JumpToImage;
            break;

        case Shortcuts::SC_NAV_JUMP_TO_IMAGE_ALT: // 'G'
            if (ctrl && !alt && !shift) return Command::JumpToImage;
            break;

        case Shortcuts::SC_TOGGLE_STATS: // 'K'
            if (!ctrl && !alt && !shift) return Command::ToggleStats;
            break;
    }

    return Command::None;
}
