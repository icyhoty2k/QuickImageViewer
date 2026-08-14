// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#pragma once

#include "ConstantsTheme.h"
#include "ConstantsIcons.h" // Constants::Icon + the QIV_ICON_* literal macros

// ConstantsStrings.h
// Central repository for all user-visible text used in QIV overlays.
// Keep strings here so they have one place to change for localization.
//
// NO GLYPH IS SPELLED OUT IN THIS FILE. A message that opens with a warning
// triangle pastes QIV_ICON_WARNING in front of its literal — same compile-time
// string, one definition of the icon, in ConstantsIcons.h.

// Internal compile-time string fragments
// Do not use directly outside this header.
#define STR_THUMBNAIL_STRIP         L"Thumbnail strip"
#define STR_CACHE_WINDOW            L"VRAM strip"
#define STR_SEPARATOR               L": "
#define STR_STATE_ON STR_SEPARATOR  L"ON"
#define STR_STATE_OFF STR_SEPARATOR L"OFF"


namespace Constants::Messages {
    // ── MID_CENTER: state-change notifications ──────────────────────────────
    // These are posted via PostCenterMessage and auto-hide after the timer.
    //Inform user has jumped to firs last image in current folder
    constexpr const wchar_t *TOGGLE_FIRST_IMAGE_IN_FOLDER = L"First image: ";
    constexpr const wchar_t *TOGGLE_LAST_IMAGE_IN_FOLDER = L"Last image: ";
    constexpr const wchar_t *GO_TO_LAST_IMAGE_BEFORE_TOGGLE = L"Previous image: ";
    constexpr const wchar_t *CACHE_WINDOW_VISIBLE_MSG = STR_CACHE_WINDOW  STR_STATE_ON;
    constexpr const wchar_t *CACHE_WINDOW_HIDDEN_MSG = STR_CACHE_WINDOW  STR_STATE_OFF;
    constexpr const wchar_t *CACHE_WINDOW_CLEAR_CACHE_MSG = L"Cache cleared!";
    constexpr const wchar_t *DIR_WINDOW_VISIBLE_MSG = STR_THUMBNAIL_STRIP  STR_STATE_ON;
    constexpr const wchar_t *DIR_WINDOW_HIDDEN_MSG = STR_THUMBNAIL_STRIP  STR_STATE_OFF;


    // Overlay master toggle (N / I / Ctrl+0)
    constexpr const wchar_t *INFO_PANELS_ON = L"Info Panels" STR_STATE_ON;
    constexpr const wchar_t *INFO_PANELS_OFF = L"Info Panels" STR_STATE_OFF;

    // Overlay background toggle (P)
    // ` (grave) — bypasses the whole effect chain without discarding it, so the
    // user can compare edited against original. It was the ONLY toggle in the
    // app that changed the picture and said nothing, which made a mis-hit on a
    // key next to 1 look like the image had been damaged.
    constexpr const wchar_t *EFFECT_PREVIEW_ON  = L"Effects" STR_STATE_ON;
    constexpr const wchar_t *EFFECT_PREVIEW_OFF = L"Effects" STR_STATE_OFF;

    constexpr const wchar_t *OVERLAY_BG_ON = L"Overlay BG" STR_STATE_ON;
    constexpr const wchar_t *OVERLAY_BG_OFF = L"Overlay BG" STR_STATE_OFF;

    // Overlay layout cycle (O)
    constexpr const wchar_t *LAYOUT_GRID = L"Layout: Grid";
    constexpr const wchar_t *LAYOUT_STACKED = L"Layout: Stacked";
    constexpr const wchar_t *LAYOUT_SUMMARY = L"Layout: Summary";

    // Reset / effects
    constexpr const wchar_t *RESET_TO_DEFAULTS = L"Reset to Defaults";
    constexpr const wchar_t *ALL_EFFECTS_RESET = L"All Effects Reset";
    // F5 Refresh/Reload current dir
    constexpr const wchar_t *RELOAD_CURRENT_DIR_MSG = L"Refreshed";
    // F5 inside the History panel — re-reads both .txt files and re-scans every
    // folder. Says how many, because the scan is asynchronous and the row markers
    // land a moment later; without it the key looks like it did nothing.
    constexpr const wchar_t *HISTORY_REFRESHED_MSG = L"History refreshed — rescanning ";
    // Empty-dir placeholder shown in DirWnd / SpawnedDirWnd when the folder has no images
    constexpr const wchar_t *EMPTY_DIR_NO_IMAGES = L"No Images:";
    // Placeholder shown when the directory itself has been deleted
    constexpr const wchar_t *EMPTY_DIR_MISSING = QIV_ICON_WARNING L"  Directory Missing";
    // Heading shown when the file is there but nothing can decode it — an
    // unknown format, or a known one this build was not compiled with.
    constexpr const wchar_t *FORMAT_UNSUPPORTED = L"Format not supported:";
    // Line 2 of the placeholder, on every one of its states. A constant, so it
    // is built into the cached layout once and never rebuilt; clicking it opens
    // the same chooser F2 does, which is the way OUT of every state this
    // placeholder reports.
    //
    // No trailing ellipsis. The usual menu convention — "…" means this opens a
    // dialog — reads as TRUNCATION here, because the line sits directly above a
    // long path that really can wrap and really can look cut off.
    constexpr const wchar_t *OVERLAY_OPEN_PROMPT = L"Open a file or folder";
    // Keyboard equivalents, appended after each clickable line and left OUTSIDE
    // the link styling — the hint tells you the key, it is not itself a target.
    // Someone who reads them once stops needing the mouse here at all.
    constexpr const wchar_t *OVERLAY_OPEN_PROMPT_HINT = L"   (F2)";
    constexpr const wchar_t *OVERLAY_PATH_HINT        = L"   (L)";
    // Line 1 of the placeholder, on its own above the heading: "qiv v2.201.0.272".
    //
    // A line of its own rather than sharing one with the heading: this screen is
    // the one that gets photographed and pasted into a bug report, and it is
    // exactly the state where there is no image window whose title bar could
    // carry the version. On its own line it survives a crop.
    // The full product name, not the short "qIV": the one person who reads this
    // line is reading it off a screenshot in a bug report, and it has to name
    // the application to someone who may only know it by its full name.
    constexpr const wchar_t *OVERLAY_APP_LINE = L"Quick Image Viewer v";
    // Placeholder shown in CacheWnd when the VRAM thumbnail cache is empty
    constexpr const wchar_t *EMPTY_CACHE = L"Thumbnail Cache Empty";

    // Q — toggle last/current dir
    constexpr const wchar_t *TOGGLE_DIR_NO_PREV = L"No previous folder";
    constexpr const wchar_t *TOGGLE_DIR_CHANGED = QIV_ICON_ARROW_RIGHT L" "; // prefix — append folder name
    constexpr const wchar_t *TOGGLE_DIR_MISSING = QIV_ICON_WARNING L" Previous folder no longer exists";

    // E — toggle last/current image
    constexpr const wchar_t *TOGGLE_IMAGE_NO_PREV = L"No previous image";
    constexpr const wchar_t *TOGGLE_IMAGE_CHANGED = QIV_ICON_ARROW_RIGHT L" "; // prefix — append filename
    constexpr const wchar_t *TOGGLE_IMAGE_MISSING = QIV_ICON_WARNING L" Previous image no longer exists";

    // Runtime theme factor  (Ctrl+Alt+Shift+Numpad+/-/0)
    constexpr const wchar_t *THEME_FACTOR_PREFIX = L"Theme: ";
    constexpr const wchar_t *THEME_FACTOR_RESET_MSG = L"Theme: Reset";

    // Window chrome toggles  (Ctrl+Shift+Numpad*)
    constexpr const wchar_t *CORNER_ROUND = L"Corners: Round";
    constexpr const wchar_t *CORNER_SQUARE = L"Corners: Square";

    // Backdrop cycle  (Ctrl+Shift+Numpad/)
    constexpr const wchar_t *BACKDROP_NONE = L"Backdrop: None";
    constexpr const wchar_t *BACKDROP_MICA = L"Backdrop: Mica";
    constexpr const wchar_t *BACKDROP_ACRYLIC = L"Backdrop: Acrylic";
    constexpr const wchar_t *BACKDROP_MICA_ALT = L"Backdrop: MicaAlt";

    // Ctrl+F1 / Space / R / S — Slideshow
    constexpr const wchar_t *SLIDESHOW_PLAYING = QIV_ICON_PLAY L" Slideshow"; // prefix; interval/loop/shuffle appended dynamically
    constexpr const wchar_t *SLIDESHOW_PAUSED = QIV_ICON_PAUSE L" Slideshow Paused";
    constexpr const wchar_t *SLIDESHOW_STOPPED = QIV_ICON_STOP L" Slideshow Stopped";
    constexpr const wchar_t *SLIDESHOW_LOOP_ON = L"Loop" STR_STATE_ON;
    constexpr const wchar_t *SLIDESHOW_LOOP_OFF = L"Loop" STR_STATE_OFF;
    constexpr const wchar_t *SLIDESHOW_SHUFFLE_ON = L"Shuffle" STR_STATE_ON;
    constexpr const wchar_t *SLIDESHOW_SHUFFLE_OFF = L"Shuffle" STR_STATE_OFF;
    // Indexed by TransitionType — single source for the tray submenu, the context
    // menu submenu and the overlay message. Order MUST match the enum in
    // SlideshowTransitions.h; size MUST match Constants::Slideshow::TRANSITION_COUNT.
    constexpr const wchar_t *TRANSITION_NAMES[] = {
        L"Cut", L"Fade", L"Dissolve", L"Ripple",
        L"Slide Left", L"Zoom Out", L"Slide Up", L"Zoom In",
        L"Slide Right", L"Slide Down", L"Soft Zoom", L"Spin",
        L"Spin Zoom", L"Drift Left", L"Drift Up", L"Flicker",
        L"Bounce", L"Swing", L"Slam", L"Iris", L"Slide Diagonal"
    };
    constexpr const wchar_t *TRANSITION_PREFIX = L"Transition: "; // append a NAMES entry
    // Indexed by Constants::Slideshow::TransitionSource::NONE..LIST.
    constexpr const wchar_t *TRANSITION_SOURCE_NAMES[] = {
        L"None (use selected)", L"All", L"List (ticked only)"
    };
    // Indexed by Constants::Slideshow::TransitionOrder::SEQUENTIAL..RANDOM.
    constexpr const wchar_t *TRANSITION_ORDER_NAMES[] = { L"Sequential", L"Random" };
    constexpr const wchar_t *TRANSITION_SOURCE_PREFIX = L"Transitions: ";
    constexpr const wchar_t *TRANSITION_ORDER_PREFIX  = L"Transition Order: ";
    constexpr const wchar_t *TRANSITION_LIST_EMPTY    = QIV_ICON_WARNING L" Transition list is empty";
    // Runtime-appendable forms of the STR_STATE_* macros, for messages whose
    // subject is only known at run time.
    constexpr const wchar_t *STATE_ON_SUFFIX  = STR_STATE_ON;
    constexpr const wchar_t *STATE_OFF_SUFFIX = STR_STATE_OFF;
    constexpr const wchar_t *SLIDESHOW_INTERVAL_PREFIX = L"Interval: "; // append "<n> ms"

    // Ctrl+T — always on top
    constexpr const wchar_t *ALWAYS_ON_TOP_ON = L"Always on Top" STR_STATE_ON;
    constexpr const wchar_t *ALWAYS_ON_TOP_OFF = L"Always on Top" STR_STATE_OFF;

    // F11 / F12 — mirroring to other instances. The ON message names the target
    // count: "Mirroring: On" with nothing connected looks identical to a broken
    // feature, and the number is the fastest way to see which it is.
    constexpr const wchar_t *MIRROR_ON_PREFIX  = L"Mirroring" STR_STATE_ON L" " QIV_ICON_ARROW_RIGHT L" "; // append "<n> target(s)"
    constexpr const wchar_t *MIRROR_OFF        = L"Mirroring" STR_STATE_OFF;
    constexpr const wchar_t *MIRROR_NO_TARGETS = L"Mirroring" STR_STATE_ON L" — no targets (F10 to connect)";
    // The picker (two or more instances connected) closed with nothing ticked.
    // Mirroring to no one is mirroring off, so say that rather than claim it is
    // on and then forward nothing.
    constexpr const wchar_t *MIRROR_NONE_PICKED = L"Mirroring" STR_STATE_OFF L" — no instance chosen";
    // F12 is meaningless while F11 is off, and a keypress that appears to do
    // nothing reads as a bug. Say why instead.
    constexpr const wchar_t *MIRROR_LOCAL_ON   = L"Mirror: execute here too" STR_STATE_ON;
    constexpr const wchar_t *MIRROR_LOCAL_OFF  = L"Mirror: remote only (this screen stays put)";
    constexpr const wchar_t *MIRROR_LOCAL_IDLE = L"Mirroring is off (F11 to start)";

    // The folder-tree walk (Alt+Up / Down / Left / Right) refusing to step.
    // Each says WHICH edge was hit — "nothing happened" on a navigation key is
    // indistinguishable from a key that did not register.
    // Where the walk LANDED. Needed precisely because a successful step is the
    // case with no other feedback: stepping into a folder that has pictures just
    // changes the picture, and nothing on screen says which folder it came from.
    // The empty and missing folders announce themselves through the placeholder;
    // this is for the ones that work.
    // One per DIRECTION rather than a single folder icon: the arrow says which
    // way the step went, which the folder name alone cannot. Landing on "2026"
    // reads very differently depending on whether you went up into it or across
    // to it. Icon then two spaces, matching EMPTY_DIR_MISSING above.
    //
    // The variation selector on each of the four, and the font-fallback finding
    // behind it, are explained where the glyphs are defined — see the folder-walk
    // block in ConstantsIcons.h. It matters here only that these must stay paired
    // with D2D1_DRAW_TEXT_OPTIONS_NONE at the centre-message draw call.
    constexpr const wchar_t *FOLDER_WALK_UP      = QIV_ICON_WALK_UP   L"  "; // + folder name
    constexpr const wchar_t *FOLDER_WALK_DOWN    = QIV_ICON_WALK_DOWN L"  ";
    constexpr const wchar_t *FOLDER_WALK_PREV    = QIV_ICON_WALK_PREV L"  ";
    constexpr const wchar_t *FOLDER_WALK_NEXT    = QIV_ICON_WALK_NEXT L"  ";

    // The wrap setting being toggled from the tray.
    constexpr const wchar_t *FOLDER_WALK_WRAP_ON  = L"Folder walk wraps around" STR_STATE_ON;
    constexpr const wchar_t *FOLDER_WALK_WRAP_OFF = L"Folder walk stops at the first and last folder";

    constexpr const wchar_t *FOLDER_WALK_NOWHERE    = L"No folder open to walk from";
    constexpr const wchar_t *FOLDER_WALK_NO_PARENT  = L"Already at the top — no parent folder";
    constexpr const wchar_t *FOLDER_WALK_NO_CHILD   = L"No subfolders here";
    // TWO DISTINCT FAILURES, two distinct texts. They shared one message and it
    // cost a debugging round: "cannot step sideways" is true of both "the parent
    // holds no subfolders" and "this folder is not among the ones it holds", and
    // those have nothing to do with each other.
    constexpr const wchar_t *FOLDER_WALK_PARENT_EMPTY = L"The parent folder has no subfolders to step through";
    constexpr const wchar_t *FOLDER_WALK_NOT_A_CHILD  = L"This folder was not found among its parent's subfolders";
    constexpr const wchar_t *FOLDER_WALK_AT_END     = L"No further folder in that direction";

    // A file this app has no decoder for. Named with its extension, because
    // "unsupported" on its own invites a bug report while ".txt" does not.
    constexpr const wchar_t *OPEN_NOT_AN_IMAGE = L"Not a supported image format " QIV_ICON_MIDDLE_DOT L" ";  // + ext

    // A multi-item drop where some of it could not come along. Only shown for
    // what is genuinely unreachable — a second folder, or a file from elsewhere.
    // Files dropped from the SAME folder as the one that opened are already in
    // the playlist, so they are never counted here and never mentioned.
    constexpr const wchar_t *DROP_EXTRAS_PREFIX  = L"Opened the first item " QIV_ICON_MIDDLE_DOT L" ";  // + count
    constexpr const wchar_t *DROP_EXTRAS_SUFFIX  = L" other(s) not opened";

    // Ctrl+Enter — pushing this viewer's picture at the instances under Control.
    // Every outcome says which it was: a push that reaches nothing must not look
    // the same as one that reached three screens.
    constexpr const wchar_t *PUSH_SENT_PREFIX = L"Pushed image " QIV_ICON_ARROW_RIGHT L" ";      // + count
    constexpr const wchar_t *PUSH_SENT_SUFFIX = L" instance(s)";
    // Sync now. Says what travelled, because this one sends the whole LOOK and
    // a push sends a place — the two must not read alike on an overlay that
    // fades before you can check which key you pressed.
    constexpr const wchar_t *SYNC_SENT_PREFIX =
        L"Synced folder " QIV_ICON_MIDDLE_DOT L" image " QIV_ICON_MIDDLE_DOT L" view " QIV_ICON_ARROW_RIGHT L" ";
    constexpr const wchar_t *PUSH_NO_IMAGE    = L"Nothing to push — no image loaded";
    // Alt+Enter. Worded differently from the Ctrl+Enter line on purpose: the two
    // do visibly different things at the far end, and an identical message would
    // make a mis-pressed modifier impossible to notice.
    constexpr const wchar_t *PUSH_ONCE_PREFIX = L"Image streamed " QIV_ICON_ARROW_RIGHT L" ";  // + count
    // Ctrl+Alt+Enter — asking one instance for the picture it is displaying.
    constexpr const wchar_t *STREAM_IN_ASKING_PREFIX = L"Asking ";  // + target name
    constexpr const wchar_t *STREAM_IN_ASKING_SUFFIX = L" for its image…";
    constexpr const wchar_t *STREAM_IN_NO_TARGET =
        L"Nobody to ask — tick an instance under Control (Ctrl+F11)";
    // It answered, and it is showing nothing. An answer, not a failure.
    constexpr const wchar_t *STREAM_IN_EMPTY = L"That instance is showing no image";
    constexpr const wchar_t *STREAM_IN_FAILED = L"Could not fetch that instance's image";

    // Refusals from the transfer itself, shared by both directions so the two ends
    // describe the same fault with the same words.
    constexpr const wchar_t *STREAM_ERR_UNREADABLE = L"cannot read that image file";
    // The far end predates image streaming. Refused with a reason rather than tried:
    // its line buffer is 4 KB, so it answers a chunk by dropping the connection, and
    // "the link died" is a much worse explanation than this one.
    constexpr const wchar_t *STREAM_ERR_PEER_TOO_OLD =
        L"that instance is too old to receive an image — update it (protocol v2)";
    constexpr const wchar_t *STREAM_ERR_TOO_LARGE  = L"image is too large to stream";
    constexpr const wchar_t *STREAM_ERR_NO_TRANSFER =
        L"no transfer in progress — send StreamImageBegin first";
    constexpr const wchar_t *STREAM_ERR_TRUNCATED =
        L"transfer incomplete — fewer bytes arrived than were declared";
    constexpr const wchar_t *PUSH_NO_TARGETS  =
        L"Nothing to push to — tick an instance under Control (Ctrl+F11)";
    // Every ticked instance is on another machine, where a folder path and a
    // playlist index both mean nothing. Named rather than silently skipped.
    constexpr const wchar_t *PUSH_ONLY_REMOTE =
        L"Not pushed — the ticked instances are on another machine";
    constexpr const wchar_t *PUSH_SKIPPED_PREFIX = L" (";                // + count
    constexpr const wchar_t *PUSH_SKIPPED_SUFFIX = L" skipped: another machine)";

    // Shown when connecting forces the sort order off disk order — see
    // LeaveDiskOrderForSession.
    constexpr const wchar_t *REMOTE_SORT_LEFT_DISK_ORDER =
        L"Sort: by Name — disk order cannot be shared between instances";

    // A command refused because a connection is live. The reason is appended
    // from the SESSION_BLOCKED table, so this is only the opening.
    constexpr const wchar_t *REMOTE_BLOCKED_PREFIX = L"Blocked while connected: ";

    // Ctrl+Alt+S — the listener toggled from the keyboard, with no panel open to
    // show the result. The endpoint is appended to the first, because "started"
    // alone does not say which address and port it came up on, and that is the
    // whole answer somebody needs before pointing a phone at it.
    //
    // The failure form is a CENTER MESSAGE, not a dialog: this is a keystroke,
    // and WhyCannotStart already returns a sentence saying what is wrong.
    constexpr const wchar_t *SERVER_STARTED_PREFIX = L"Local server started on ";
    constexpr const wchar_t *SERVER_STOPPED        = L"Local server stopped";
    constexpr const wchar_t *SERVER_START_FAILED   = L"Local server could not start — ";
    // Shown when the lock is turned ON so the operator knows the window went
    // deaf on purpose, and that the tray is the way back.
    constexpr const wchar_t *KIOSK_LOCK_ON  = L"Kiosk Lock" STR_STATE_ON L" — unlock from the tray icon";
    constexpr const wchar_t *KIOSK_LOCK_OFF = L"Kiosk Lock" STR_STATE_OFF;
    constexpr const wchar_t *KEEP_DISPLAY_AWAKE_ON  = L"Keep Display Awake" STR_STATE_ON;
    constexpr const wchar_t *KEEP_DISPLAY_AWAKE_OFF = L"Keep Display Awake" STR_STATE_OFF;

    // Alt+W/A/S/D — keyboard snap to screen half
    constexpr const wchar_t *SNAP_LEFT = L"Snap: Left Half";
    constexpr const wchar_t *SNAP_RIGHT = L"Snap: Right Half";
    constexpr const wchar_t *SNAP_TOP = L"Snap: Top Half";
    constexpr const wchar_t *SNAP_BOTTOM = L"Snap: Bottom Half";

    // Alt+Q/E/Z/C — keyboard snap to screen quarter
    constexpr const wchar_t *SNAP_TOP_LEFT = L"Snap: Top-Left Quarter";
    constexpr const wchar_t *SNAP_TOP_RIGHT = L"Snap: Top-Right Quarter";
    constexpr const wchar_t *SNAP_BOTTOM_LEFT = L"Snap: Bottom-Left Quarter";
    constexpr const wchar_t *SNAP_BOTTOM_RIGHT = L"Snap: Bottom-Right Quarter";

    // Ctrl+Space — fill available screen space (work area minus visible panels) / restore
    constexpr const wchar_t *AUTOSIZE_TO_WORK_AREA = L"Fit to Screen";
    constexpr const wchar_t *AUTOSIZE_RESTORE = L"Default Size";

    // Ctrl+M — move the window to the next monitor. The single-monitor case is
    // reported rather than ignored: a key that does nothing silently reads as
    // broken, and "there is nowhere to go" is the actual answer.
    constexpr const wchar_t *MONITOR_MOVED_PREFIX = L"Monitor ";
    constexpr const wchar_t *MONITOR_ONLY_ONE = L"Only One Monitor";

    // Network announcement (TCP/IP menu → Announce on network).
    //
    // THREE outcomes, not two. Off and announcing are obvious; the third is
    // "asked for, but nothing is published" — the server is stopped, or bound to
    // loopback. Reporting that as ON would be a lie the user only discovers when
    // a phone fails to find them, which is the worst moment to find out.
    constexpr const wchar_t *BEACON_PREFIX  = L"Network: ";
    constexpr const wchar_t *BEACON_ON      = L"Announcing";
    constexpr const wchar_t *BEACON_OFF     = L"Not announcing";
    constexpr const wchar_t *BEACON_PENDING = L"Will announce";

    // Server Log to file (TCP/IP menu). The folder is appended to the ON line,
    // because "it is logging somewhere" is not an answer anybody can act on.
    //
    constexpr const wchar_t *GENERAL_LOG_ON  = L"General log: ON";
    constexpr const wchar_t *GENERAL_LOG_OFF = L"General log: OFF";

    constexpr const wchar_t *LOG_FILE_ON  = L"Server log to file: ON";
    constexpr const wchar_t *LOG_FILE_OFF = L"Server log to file: OFF";
    // Said on the ON line, because the folder appears immediately and the FILE
    // does not — and "the folder is empty" reads as a broken setting unless
    // something explains that a file is written when there is something to write.
    constexpr const wchar_t *LOG_FILE_WAITING =
        L"A file is written on the first exchange with a client.";

    // Sort order  (Ctrl+Alt+Shift+0/6/7/8/9)  — press once: ascending, press again: descending
    constexpr const wchar_t *SORT_BY_NAME = L"Sort: Name (A" QIV_ICON_ARROW_RIGHT L"Z)";
    constexpr const wchar_t *SORT_BY_NAME_REV = L"Sort: Name (Z" QIV_ICON_ARROW_RIGHT L"A)";
    constexpr const wchar_t *SORT_BY_DATE = L"Sort: Date (Newest)";
    constexpr const wchar_t *SORT_BY_DATE_REV = L"Sort: Date (Oldest)";
    constexpr const wchar_t *SORT_BY_SIZE = L"Sort: Size (Largest)";
    constexpr const wchar_t *SORT_BY_SIZE_REV = L"Sort: Size (Smallest)";
    constexpr const wchar_t *SORT_BY_TYPE = L"Sort: Extension (A" QIV_ICON_ARROW_RIGHT L"Z)";
    constexpr const wchar_t *SORT_BY_TYPE_REV = L"Sort: Extension (Z" QIV_ICON_ARROW_RIGHT L"A)";
    constexpr const wchar_t *SORT_BY_DISK = L"Sort: Disk Order";

    // Spawned DirWnd messages



    constexpr const wchar_t *SPAWN_DIR_TOP =  STR_THUMBNAIL_STRIP STR_SEPARATOR L"Top";
    constexpr const wchar_t *SPAWN_DIR_LEFT = STR_THUMBNAIL_STRIP STR_SEPARATOR L"Left";
    constexpr const wchar_t *SPAWN_DIR_RIGHT = STR_THUMBNAIL_STRIP STR_SEPARATOR L"Right";
    constexpr const wchar_t *SPAWN_DIR_BOTTOM = STR_THUMBNAIL_STRIP STR_SEPARATOR L"Bottom";
    constexpr const wchar_t *SPAWN_DIR_CLOSED = STR_THUMBNAIL_STRIP STR_SEPARATOR L"Closed";
    constexpr const wchar_t *SPAWN_DIR_NO_SPACE = L"No free positions for " STR_THUMBNAIL_STRIP;
    constexpr const wchar_t *COPIED_TO_CLIPBOARD = L"Copied to Clipboard";
    // Ctrl+C / Ctrl+X on a thumbnail strip's SELECTION, as opposed to the image
    // on screen above. Prefixes — the caller appends the single file's name, or
    // "<n> files", the way the delete confirmation already names one file and
    // counts several.
    constexpr const wchar_t *COPIED_FILES_PREFIX = L"Copied to Clipboard: ";
    constexpr const wchar_t *CUT_FILES_PREFIX = L"Cut to Clipboard: ";
    constexpr const wchar_t *CLIPBOARD_FILES_COUNT = L" files";
    // OpenClipboard fails while another process holds the clipboard open, which
    // is ordinary and transient — a Ctrl+C that lands in that window copied
    // NOTHING. Without this the keystroke is indistinguishable from a successful
    // one until the paste comes up empty or, worse, pastes what was there before.
    constexpr const wchar_t *CLIPBOARD_UNAVAILABLE =
            QIV_ICON_WARNING L" Clipboard is in use by another app";
    // Ctrl+Shift+C — the path went to the clipboard, the NAME goes on screen.
    // Prefix; the caller appends the file name.
    constexpr const wchar_t *COPIED_PATH_PREFIX = L"Path copied: ";
    // ── "The image on screen has no usable file" ──────────────────────────────
    // Shared by every command that has to name the current image on disk —
    // Ctrl+Shift+C and Ctrl+Shift+O today. Worded without naming the action so
    // one string can serve both: what failed is the same fact each time, and
    // two near-identical sentences per command is how a vocabulary drifts.
    //
    // Kept separate from CLIPBOARD_UNAVAILABLE above, and from each other: "no
    // image", "the image is streamed" and "the clipboard is busy" are three
    // unrelated causes with three unrelated fixes, and one string for several is
    // the quieter version of saying nothing.
    constexpr const wchar_t *NO_IMAGE_ON_SCREEN =
            QIV_ICON_WARNING L" No image open";
    // A picture pushed here over the network arrived as BYTES, not as a name. It
    // lives in a temp file this process deletes at the next change of picture,
    // so there is no path worth handing out and no file worth opening.
    constexpr const wchar_t *IMAGE_IS_STREAMED =
            QIV_ICON_WARNING L" Streamed image — it has no file on this machine";
    // Ctrl+Shift+O — SHOpenWithDialog itself refused. Rare, and distinct from
    // the two above: there IS a file, Windows just would not raise the chooser.
    constexpr const wchar_t *OPEN_WITH_FAILED =
            QIV_ICON_WARNING L" Could not open the 'Open with' chooser";
    // Reveal-in-Explorer on a file that has been deleted since the panel or the
    // playlist last saw it. Its own sentence because the alternative is not an
    // error at all — Explorer silently opening the wrong folder — and "it is not
    // there any more" is the one thing that explains what the user is looking at.
    constexpr const wchar_t *REVEAL_FILE_GONE =
            QIV_ICON_WARNING L" That file is no longer there";

    // Desktop wallpaper — NAMES is indexed by Constants::Wallpaper::FILL..SPAN and
    // is the single source for both the submenu labels and the overlay message.
    constexpr const wchar_t *WALLPAPER_NAMES[] = {
        L"Fill", L"Fit", L"Stretch", L"Tile", L"Center", L"Span"
    };
    constexpr const wchar_t *WALLPAPER_SET = L"Wallpaper: "; // prefix — append the style name
    constexpr const wchar_t *WALLPAPER_FAILED = QIV_ICON_WARNING L" Wallpaper could not be applied";
    // Folder walking — used by ALL three walkers (horizontal wheel, PageUp/Down,
    // Insert/Delete) so the centre message never depends on how you moved.
    // Format is "<arrow>  <kind> <n>/<total> <folder name>", where <n> is the row
    // number the History panel shows for that folder, so overlay and panel always
    // agree. The kind is chosen from the row itself: starred rows get ★, the
    // rest 📁.
    //
    // THE ARROW IS THE SAME ONE THE FOLDER-TREE WALK USES, deliberately. Alt+Up,
    // Alt+Down and the two siblings announce with ⬆️ ⬇️ ⬅️ ➡️, and this walk
    // used to lead with the kind icon alone — identical whichever way the wheel
    // was turned. Two ways of moving between folders spoke two vocabularies, and
    // the one that reported no direction was the one driven by a gesture that
    // has nothing else to say which way it went.
    //
    // Kind is kept BESIDE the arrow rather than replaced by it: it says whether
    // the row is starred, which the arrow cannot, and unifying by deletion would
    // swap one missing fact for another.
    constexpr const wchar_t *WALK_ARROW_PREV = Constants::Icon::WALK_PREV;
    constexpr const wchar_t *WALK_ARROW_NEXT = Constants::Icon::WALK_NEXT;
    // 📜, NOT 📁 — see Icon::HISTORY. A row in this walk is a folder you have
    // BEEN to, and the folder glyph is what the folder-tree walk is about, so
    // the two features read as the same thing while doing different ones. It is
    // the mark the History panel's own title bar and the help section carry.
    constexpr const wchar_t *WALK_HISTORY_FOLDER = Constants::Icon::HISTORY;
    constexpr const wchar_t *WALK_FAVORITE_FOLDER = Constants::Icon::FAVORITES_MARK;
    // Shown when a walk stepped over more than one dead folder to reach its
    // destination — append the count. A single skip names the folder instead.
    // Text only: the caller prepends the warning icon at run time, because the
    // count sits between the icon and this word.
    constexpr const wchar_t *WALK_SKIPPED = L" skipped ";
    // History panel footer total, and its hover popup.
    // Footer reads "<size>/<files>" plus "/<n>" only when folders were excluded.
    // The popup spells all three out, then names what was left out and why it
    // could be: an alias whose target is already counted elsewhere in the list.
    // Shown centred in the History panel while the background folder sweep is
    // still running. The panel stays fully usable — the scan does not block it,
    // and closing the panel does not stop the scan.
    constexpr const wchar_t *HISTORY_SCANNING = L"Loading ...";

    // The word for a folder, used everywhere in this popup so it can be changed
    // in one place (dirs / folders / directories).
    constexpr const wchar_t *WORD_DIRS = L"dirs";

    constexpr const wchar_t *TOTAL_HEADER = L"TOTAL:";
    constexpr const wchar_t *TOTAL_SIZE_LABEL = L"Size: ";
    constexpr const wchar_t *TOTAL_FILES_LABEL = L"Files: ";
    constexpr const wchar_t *TOTAL_DIRS_LABEL = L"Dirs: ";
    constexpr const wchar_t *TOTAL_SEPARATOR = L"===========";
    constexpr const wchar_t *TOTAL_EXCLUDED_SUFFIX = L" excluded:"; // "<n> dirs excluded:"
    constexpr const wchar_t *EXCLUDED_BULLET = L"* ";               // before each number
    // Group headings under the excluded count. Each names WHY those folders
    // contribute nothing, then lists them numbered from 1.
    constexpr const wchar_t *EXCLUDED_DUPLICATES = L"already counted (symlinks of a listed folder):";
    constexpr const wchar_t *EXCLUDED_MISSING = L"missing ";  // + WORD_DIRS + ":"
    constexpr const wchar_t *EXCLUDED_EMPTY = L"empty ";      // + WORD_DIRS + ":"

    // History panel — row badge labels. One line each in the hover popup that
    // appears when a row carries more than one badge and they have to share a slot.
    constexpr const wchar_t *BADGE_MISSING = L"Folder not found";
    constexpr const wchar_t *BADGE_EMPTY = L"No images in folder";
    constexpr const wchar_t *BADGE_FAVORITE = L"Favorite";

    // History panel — symlink glyph hover popup. Line 1 is the kind of link,
    // line 2 is the resolved destination (filled in at runtime).
    constexpr const wchar_t *LINK_KIND_JUNCTION = L"Directory junction  (mklink /J)";
    constexpr const wchar_t *LINK_KIND_SYMLINK = L"Directory symlink  (mklink /D)";
    // Path resolves elsewhere but no component is a reparse point — the drive
    // letter itself is redirected (subst, or a mapped network drive).
    constexpr const wchar_t *LINK_KIND_MAPPED = L"Mapped path  (subst / network drive)";
    constexpr const wchar_t *LINK_TARGET_UNKNOWN = L"target could not be resolved";

    constexpr const wchar_t *WALK_NO_HISTORY_FOLDERS = L"No other history folders";
    constexpr const wchar_t *WALK_NO_FAVORITE_FOLDERS = L"No favorite folders";
    constexpr const wchar_t *FOLDER_DEAD_MISSING = QIV_ICON_WARNING L" Folder not found";
    constexpr const wchar_t *FOLDER_DEAD_EMPTY = QIV_ICON_WARNING L" No images in folder";
    constexpr const wchar_t *FOLDER_DELETED_NOTIFY = QIV_ICON_WARNING L" Folder deleted";

    // Thumbnail strip wrap-around
    constexpr const wchar_t *THUMB_STRIP_WRAP_TO_START = QIV_ICON_WRAP_START L" Start";
    constexpr const wchar_t *THUMB_STRIP_WRAP_TO_END = QIV_ICON_WRAP_END L" End";
    constexpr const wchar_t *THUMB_STRIP_WRAP_ON = STR_THUMBNAIL_STRIP L" Wrap" STR_STATE_ON;
    constexpr const wchar_t *THUMB_STRIP_WRAP_OFF = STR_THUMBNAIL_STRIP L" Wrap" STR_STATE_OFF;

    // Remote control over TCP/IP (src/Rem_TCP_IP). Shown in the remote panel's
    // status line. An enabled server with an empty AllowList refuses every
    // connection — stating that is the difference between a configured refusal
    // and something that merely looks broken.
    // NOTE: there is no "server disabled" reason any more. Autostart describes
    // launch behaviour only, so there is no state in which the user asks the
    // server to start and is told it is not allowed to.
    constexpr const wchar_t *REMOTE_BLOCKED_NO_PORT        = L"No port configured";
    // A name identifies this instance to whoever drives it — see WhyCannotStart.
    constexpr const wchar_t *REMOTE_BLOCKED_NO_NAME        =
        L"No name set — a driving instance identifies this one by name";
    constexpr const wchar_t *REMOTE_WARN_EMPTY_ALLOWLIST   = L"AllowList empty — all connections denied";
    // A HARD STOP, not a warning, and the only rule here that refuses a
    // configuration the user explicitly asked for.
    //
    // An unauthenticated listener on loopback is a local convenience and stays
    // permitted. The same listener on a LAN or public interface is an open door
    // to the folder history, to opening any path, and to reading back whatever
    // is on screen — with no step at which anyone is asked who they are.
    //
    // Refusing to start is the only honest response: a warning on a panel nobody
    // has open does not defend anything, and this is precisely the mistake that
    // is invisible until it has already been exploited.
    constexpr const wchar_t *REMOTE_BLOCKED_NO_PASSWORD    =
        L"No password set — required unless bound to loopback (127.0.0.1)";
    // A stored value this build cannot parse — in practice one written before
    // the move to PBKDF2. It cannot be converted, because converting it needs
    // the plaintext and the stored form deliberately does not keep it.
    //
    // Refusing to start is the only safe reading. The alternative — treating an
    // unparseable password as no password — is how a listener ends up
    // unauthenticated because of a file format change, which is exactly the
    // class of accident that must not be possible.
    constexpr const wchar_t *REMOTE_BLOCKED_BAD_PASSWORD   =
        L"Stored password is in an old or damaged format — set it again";

    // Written into qivRemoteServerBlacklist.ini by the brute-force guard. The
    // count is spliced in from Constants::RemoteTcpIp::AUTH_MAX_FAILURES rather
    // than spelled out, so raising the threshold cannot leave the file claiming
    // a figure that is no longer true.
    //
    // Says what happened in plain words on purpose: this line is read months
    // later by somebody deciding whether it is safe to delete.
    constexpr const wchar_t *BLACKLIST_REASON_AUTH_PREFIX = L"blocked automatically: ";
    constexpr const wchar_t *BLACKLIST_REASON_AUTH_SUFFIX =
        L" failed authentications in 10 minutes";

    // The OTHER way a row gets into that file: somebody pressed Ban in the F9
    // panel. Distinguished from the automatic reason above because the two are
    // undone differently — an automatic block is evidence of an attack, an
    // operator block is a decision, and the person reading the file later needs
    // to know which of the two they are looking at.
    constexpr const wchar_t *BLACKLIST_REASON_OPERATOR =
        L"blocked by hand from the My Clients panel";

    // A TIMED block, which never reaches the file — this shows in the panel's
    // own list only. Says "kick" rather than "block" because that is the button
    // that produced it and the word the operator will be looking for.
    constexpr const wchar_t *BLACKLIST_REASON_TIMED =
        L"timed kick from the My Clients panel";
    // Panel footers reporting WHERE the values on screen came from.
    //
    // Every panel backed by a file says this on open, because a populated panel
    // is otherwise ambiguous — identical whether it was loaded from disk or is
    // showing defaults — and that difference decides whether saving creates a
    // file or overwrites one.
    constexpr const wchar_t *REMOTE_PANEL_READ_FROM = L"Read from ";
    constexpr const wchar_t *REMOTE_PANEL_NO_INI =
        L"No settings file — these are defaults. Save to INI creates one.";
    constexpr const wchar_t *REMOTES_PANEL_NO_FILE =
        L"No qivRemoteServers.ini — added rows are saved to it automatically.";
    // The F8 Dedicated panel says the same thing in its own subtitle
    // ("Editing: <path>" / "New instance — not yet generated"), so it needs
    // nothing here.

    // Overlay top-right server indicator: "<dot> 2/4  86.0%".
    //
    // COLOUR emoji glyphs, so they keep their own colour while the rest of the
    // slot is drawn in the user's chosen overlay colour. The outer slots share
    // one brush; a per-range drawing effect would be the alternative and is far
    // more machinery for one dot.
    //
    // Shown only while the listener is running, so its absence is the "stopped"
    // state and there is nothing to grey out. The COLOUR then carries a second
    // fact that is otherwise invisible until something goes wrong:
    //
    //   GREEN  (U+1F7E2)  TLS — reachable from off this machine, encrypted
    //   ORANGE (U+1F7E0)  plaintext — loopback only, nothing to encrypt against
    //
    // Orange rather than red on purpose: a loopback listener is not a fault or a
    // misconfiguration, it is the normal local case. Red would cry wolf on the
    // setup most people run.
    constexpr const wchar_t *OVERLAY_SERVER_DOT_TLS   = Constants::Icon::DOT_GREEN;
    constexpr const wchar_t *OVERLAY_SERVER_DOT_PLAIN = Constants::Icon::DOT_ORANGE;

    // The dark phase of the connect / disconnect blink.
    //
    // A GLYPH RATHER THAN AN EMPTY STRING, and the same emoji class as the two
    // above so it measures the same width. Blanking it would make the count and
    // the zoom beside it jump left and back three times, which reads as the
    // overlay glitching rather than as the dot blinking.
    constexpr const wchar_t *OVERLAY_SERVER_DOT_OFF   = Constants::Icon::DOT_BLACK;

    // The LIT phase, coloured by WHAT happened. Four outcomes, because they mean
    // four different things to whoever sees them from across a room:
    //
    //   green   somebody arrived
    //   white   somebody left normally — they said goodbye, nothing is wrong.
    //           Absence rather than alarm: the screen went quiet
    //   red     somebody VANISHED — no goodbye: a crash, a reset, a phone out of
    //           range, a cable pulled. The only one worth walking over for
    //   yellow  we ejected them — kicked or banned from this machine
    //
    // The red/blue split is the whole point. Without it a tidy disconnect and a
    // dead screen look identical, so either every departure is alarming or none
    // is — and both make the indicator useless.
    //
    // Yellow for an eject because it was DELIBERATE and done here. Showing the
    // operator red for the thing they just did themselves would teach them to
    // ignore red.
    constexpr const wchar_t *OVERLAY_SERVER_DOT_JOIN   = Constants::Icon::DOT_GREEN;
    constexpr const wchar_t *OVERLAY_SERVER_DOT_LEFT   = Constants::Icon::DOT_WHITE;
    constexpr const wchar_t *OVERLAY_SERVER_DOT_LOST   = Constants::Icon::DOT_RED;
    constexpr const wchar_t *OVERLAY_SERVER_DOT_KICKED = Constants::Icon::DOT_YELLOW;

    // Connect / disconnect blink: how long each phase lasts, and how many blinks.
    //
    // COUNT is blinks, not phases — a blink is dark-then-lit, so the timer runs
    // twice this many times. Stated the way a person would ask for it, with the
    // doubling done where the timer is armed rather than baked into the number.
    //
    // Ending on the LIT phase falls out of that: an even number of phases always
    // settles into the true colour, and a dot left dark would read as "the
    // server stopped".
    //
    // 3 blinks at 250 ms is 1.5 seconds — long enough to catch the eye on a
    // screen nobody is watching, short enough that it is over before anyone
    // walks to it.
    constexpr int  OVERLAY_SERVER_BLINK_MS    = 300;
    constexpr int  OVERLAY_SERVER_BLINK_COUNT = 3;

    // F9 status line. The wording matches the overlay dot's two colours, so the
    // panel and the indicator cannot appear to disagree.
    constexpr const wchar_t *REMOTE_STATUS_TLS   = QIV_ICON_MIDDLE_DOT L" TLS for remote clients";
    constexpr const wchar_t *REMOTE_STATUS_PLAIN = QIV_ICON_MIDDLE_DOT L" loopback only — nothing to encrypt";
    constexpr const wchar_t *REMOTE_STATUS_FINGERPRINT = L"Fingerprint: ";
    constexpr const wchar_t *REMOTE_FINGERPRINT_COPIED = L"— copied";

    // SendDisplayedPreview failures. Separate reasons because the remedies
    // differ: no WIC is a broken install, a decode failure is a format this
    // build cannot read, and an encode failure is neither.
    constexpr const wchar_t *PREVIEW_NO_WIC =
        L"imaging component unavailable on this machine";
    constexpr const wchar_t *PREVIEW_CANNOT_DECODE =
        L"cannot decode that image for a preview — ask for the original instead";
    constexpr const wchar_t *PREVIEW_ENCODE_FAILED =
        L"could not encode the preview";

    // --- TLS (src/Rem_TCP_IP/RemoteTls.*) ---------------------------------
    constexpr const wchar_t *REMOTE_TLS_HANDSHAKE_FAILED =
        L"TLS handshake failed";
    // Refusing an unpinned server is the CORRECT outcome, so the text explains
    // rather than apologises: the first connection is the one an attacker has
    // to be present for, and trusting it silently is what pinning exists to
    // prevent.
    constexpr const wchar_t *REMOTE_TLS_NO_PIN =
        L"No fingerprint stored for this server. Compare the one shown against "
        L"the server's F9 panel, then save it to allow this connection.";
    constexpr const wchar_t *REMOTE_TLS_PIN_MISMATCH =
        L"CERTIFICATE MISMATCH — this is not the server you saved. Either its "
        L"certificate was regenerated, or something is impersonating it. "
        L"Connection refused.";
    // A listener that cannot obtain credentials must not run. Falling back to
    // plaintext on an address reachable from off the machine would be the exact
    // failure TLS was added to prevent, arrived at by accident.
    constexpr const wchar_t *REMOTE_BLOCKED_TLS_UNAVAILABLE =
        L"TLS could not be initialised — refusing to listen unencrypted: ";

    constexpr const wchar_t *BLACKLIST_REASON_CMDLINE = L"added with -remoteBlock";
    constexpr const wchar_t *BLACKLIST_REASON_PANEL   = L"added in the F9 panel";

    // Start-up failures. Each names the actual cause: "could not start" with no
    // reason is the least useful thing a status line can say.
    constexpr const wchar_t *REMOTE_ALREADY_RUNNING        = L"Server already running";
    constexpr const wchar_t *REMOTE_WSA_FAILED             = L"Winsock initialisation failed";
    constexpr const wchar_t *REMOTE_BAD_BIND_ADDRESS       = L"Bind address is not a valid IP literal";
    constexpr const wchar_t *REMOTE_SOCKET_FAILED          = L"Could not create socket";
    constexpr const wchar_t *REMOTE_PORT_IN_USE            = L"Port already in use";
    constexpr const wchar_t *REMOTE_BIND_FAILED            = L"Could not bind to that address and port";
    constexpr const wchar_t *REMOTE_LISTEN_FAILED          = L"Socket could not enter listening state";
    // Shown in the center overlay when an enabled listener fails at startup.
    // Deliberately non-fatal: a screen whose port is taken must still show
    // pictures rather than refuse to launch.
    constexpr const wchar_t *REMOTE_START_FAILED_PREFIX     = L"Remote control: ";

    // Client side — connecting to another instance.
    constexpr const wchar_t *REMOTE_CLIENT_NO_HOST           = L"No target address";
    constexpr const wchar_t *REMOTE_CLIENT_BAD_PORT          = L"Target port out of range";
    constexpr const wchar_t *REMOTE_CLIENT_RESOLVE_FAILED    = L"Could not resolve that host";
    constexpr const wchar_t *REMOTE_CLIENT_CONNECT_FAILED    = L"Could not connect";
    constexpr const wchar_t *REMOTE_CLIENT_NO_BANNER         = L"Peer did not identify itself";
    constexpr const wchar_t *REMOTE_CLIENT_PROTOCOL_ERROR    = L"Unexpected reply from peer";
    constexpr const wchar_t *REMOTE_CLIENT_PASSWORD_REQUIRED = L"Peer requires a password";
    constexpr const wchar_t *REMOTE_CLIENT_AUTH_FAILED       = L"Password rejected by peer";
    constexpr const wchar_t *REMOTE_CLIENT_NOT_CONNECTED     = L"Not connected";
    constexpr const wchar_t *REMOTE_CLIENT_SEND_FAILED       = L"Connection lost while sending";
    constexpr const wchar_t *REMOTE_CLIENT_NO_REPLY          = L"No reply from peer";
    // Imported-credential failures. Distinct from a wrong password, because the
    // remedy is different: re-import rather than retype.
    constexpr const wchar_t *REMOTE_CLIENT_BAD_SECRET        = L"Stored credentials are malformed";
    constexpr const wchar_t *REMOTE_CLIENT_SECRET_STALE      = L"Its password changed since this was imported";

    // Viewport lock (Y) — zoom + pan carried across image changes
    constexpr const wchar_t *VIEWPORT_LOCK_ON  = L"Viewport Lock" STR_STATE_ON;
    constexpr const wchar_t *WINDOW_RECOVERED = L"Window was off screen — reset to default size and position";
    constexpr const wchar_t *REMEMBER_WIN_POS_ON  = L"Remember Window Position" STR_STATE_ON;
    constexpr const wchar_t *REMEMBER_WIN_POS_OFF = L"Remember Window Position" STR_STATE_OFF;
    constexpr const wchar_t *VIEWPORT_LOCK_OFF = L"Viewport Lock" STR_STATE_OFF;

    // Thumbnail strip visual effects
    constexpr const wchar_t *THUMB_EFFECTS_ON  = L"Thumbnail Effects" STR_STATE_ON;
    constexpr const wchar_t *THUMB_EFFECTS_OFF = L"Thumbnail Effects" STR_STATE_OFF;

    // Overlay slot names, indexed by OverlayManager::Slot. Single source for
    // every centre message that names a slot, whatever triggered the change.
    constexpr const wchar_t *OVERLAY_SLOT_NAMES[] = {
        L"Top Left", L"Top Center", L"Top Right",
        L"Mid Left", L"Mid Center", L"Mid Right",
        L"Bot Left", L"Bot Center", L"Bot Right"
    };
    constexpr const wchar_t *OVERLAY_PREFIX        = L"Overlay ";
    constexpr const wchar_t *OVERLAY_FONT_PREFIX   = L"Overlay Font: "; // append the family name
    constexpr const wchar_t *OVERLAY_STATE_COMPACT = STR_SEPARATOR L"Compact";
    constexpr const wchar_t *OVERLAY_STATE_FULL    = STR_SEPARATOR L"Full";

    // BOT_LEFT readouts — independent toggles in the Overlays menu
    constexpr const wchar_t *OVERLAY_EFFECTS_LIST_ON  = L"Effects Overlay" STR_STATE_ON;
    constexpr const wchar_t *OVERLAY_EFFECTS_LIST_OFF = L"Effects Overlay" STR_STATE_OFF;
    constexpr const wchar_t *OVERLAY_DIR_NAME_ON      = L"Folder Name" STR_STATE_ON;
    constexpr const wchar_t *OVERLAY_DIR_NAME_OFF     = L"Folder Name" STR_STATE_OFF;
    constexpr const wchar_t *HISTORY_RECORD_ON        = L"Record Folder History" STR_STATE_ON;
    constexpr const wchar_t *HISTORY_RECORD_OFF       = L"Record Folder History" STR_STATE_OFF;
    constexpr const wchar_t *HISTORY_IMAGES_ONLY_ON   = L"History: Only Folders With Images" STR_STATE_ON;
    constexpr const wchar_t *HISTORY_IMAGES_ONLY_OFF  = L"History: Only Folders With Images" STR_STATE_OFF;
    constexpr const wchar_t *OVERLAY_FULL_PATH_ON     = L"Full Path" STR_STATE_ON;
    constexpr const wchar_t *OVERLAY_FULL_PATH_OFF    = L"Full Path" STR_STATE_OFF;

    // Settings-menu-only toggles (no keyboard shortcut counterpart)
    constexpr const wchar_t *KEEP_IN_BG_ON        = L"Keep in Background" STR_STATE_ON;
    constexpr const wchar_t *KEEP_IN_BG_OFF       = L"Keep in Background" STR_STATE_OFF;
    constexpr const wchar_t *RUN_ON_STARTUP_ON    = L"Run on Startup" STR_STATE_ON;
    constexpr const wchar_t *RUN_ON_STARTUP_OFF   = L"Run on Startup" STR_STATE_OFF;
    constexpr const wchar_t *HISTORY_FULL_ON      = L"History: Full List" STR_STATE_ON;
    constexpr const wchar_t *HISTORY_FULL_OFF     = L"History: Full List" STR_STATE_OFF;
    constexpr const wchar_t *OPEN_THUMB_START_ON  = L"Thumbnail Strip on Start" STR_STATE_ON;
    constexpr const wchar_t *OPEN_THUMB_START_OFF = L"Thumbnail Strip on Start" STR_STATE_OFF;
    constexpr const wchar_t *SWAP_MOUSE_ON        = L"Swap Mouse Buttons" STR_STATE_ON;
    constexpr const wchar_t *SWAP_MOUSE_OFF       = L"Swap Mouse Buttons" STR_STATE_OFF;
    constexpr const wchar_t *WHEEL_INVERT_ON      = L"Invert Scroll" STR_STATE_ON;
    constexpr const wchar_t *WHEEL_INVERT_OFF     = L"Invert Scroll" STR_STATE_OFF;
    constexpr const wchar_t *WHEEL_INVERT_H_ON    = L"Invert Horizontal Scroll" STR_STATE_ON;
    constexpr const wchar_t *WHEEL_INVERT_H_OFF   = L"Invert Horizontal Scroll" STR_STATE_OFF;
    constexpr const wchar_t *START_FULLSCREEN_ON  = L"Start in Fullscreen" STR_STATE_ON;
    constexpr const wchar_t *START_FULLSCREEN_OFF = L"Start in Fullscreen" STR_STATE_OFF;
    constexpr const wchar_t *CTRL_C_COPY_ON       = L"Ctrl+C Copy" STR_STATE_ON;
    constexpr const wchar_t *CTRL_C_COPY_OFF      = L"Ctrl+C Copy" STR_STATE_OFF;
    constexpr const wchar_t *THUMB_COPY_OP_ON     = L"Thumbnail Copy" STR_STATE_ON;
    constexpr const wchar_t *THUMB_COPY_OP_OFF    = L"Thumbnail Copy" STR_STATE_OFF;
    constexpr const wchar_t *THUMB_MOVE_OP_ON     = L"Thumbnail Move" STR_STATE_ON;
    constexpr const wchar_t *THUMB_MOVE_OP_OFF    = L"Thumbnail Move" STR_STATE_OFF;
    constexpr const wchar_t *THUMB_DELETE_OP_ON   = L"Thumbnail Delete" STR_STATE_ON;
    constexpr const wchar_t *THUMB_DELETE_OP_OFF  = L"Thumbnail Delete" STR_STATE_OFF;
    constexpr const wchar_t *THUMB_PASTE_OP_ON    = L"Thumbnail Paste" STR_STATE_ON;
    constexpr const wchar_t *THUMB_PASTE_OP_OFF   = L"Thumbnail Paste" STR_STATE_OFF;

    // View modes (keys 1-5)
    constexpr const wchar_t *VIEW_MODE_PREFIX = L"View: ";
    constexpr const wchar_t *VIEW_MODE_NAMES[] = {
        L"Fit to View",
        L"Fit to Width",
        L"Fit to Height",
        L"Fit to Window",
        L"Original Size"
    };

    // Zoom popup
    constexpr const wchar_t *ZOOM_TO_PREFIX = L"Zoom: ";
    // %s pairs — the bounds arrive pre-formatted from
    // Converters::FormatPercentCompact so their precision follows the value.
    // A fixed "%.1f" here printed ZOOM_MIN = 0.01 as "0.0".
    constexpr const wchar_t *ZOOM_TO_INPUT_HINT_FMT = L"Enter zoom in percent (%s%% – %s%%, 0 = reset)";

    // ZoomWnd — hint line below the input box
    // The out-of-range / invalid messages are built at draw time straight from
    // Constants::ZoomPanel::ZOOM_MIN and ZOOM_MAX — both are already percents.
    constexpr const wchar_t *ZOOM_ENTER_HINT = L"Enter = apply zoom  •  Esc = cancel";
    constexpr const wchar_t *ZOOM_OUT_OF_RANGE_FMT = L"Out of range — type a value between %s%% and %s%%";
    constexpr const wchar_t *ZOOM_RESET_MESSAGE = L"Zoom restored to default";
    // Shown centre-screen when a zoom keypress/wheel tick is capped by
    // ZoomPanel::ZOOM_MIN / ZOOM_MAX — otherwise the input looks ignored.
    constexpr const wchar_t *ZOOM_MIN_REACHED = L"Minimum zoom reached";
    constexpr const wchar_t *ZOOM_MAX_REACHED = L"Maximum zoom reached";
}

namespace Constants::Strings {
    // ── BOT_LEFT slot: active color-effect labels ───────────────────────────
    // Used in OverlayManager::UpdateEffects() via appendLine()

    // Named colour effects. These strings are also the KEYS in
    // app.activeEffectsList, so the renderer matches on them — changing a value
    // changes what RendererD2D::ChainEffectByName() compares against.
    //
    // "Grayscale", on the user's call 2026-08-13: *"desaturate is not accurate
    // enough, Grayscale is more accurate"*. It had been "Desaturate" on the
    // argument below — kept here so the trade is visible rather than rediscovered
    // and quietly reverted:
    //
    //   effects STACK, so a later Sepia tints this one's output and the picture
    //   ends up brown while the label still reads Grayscale. That is the case
    //   "Desaturate" was chosen to describe honestly, by naming the OPERATION
    //   rather than promising a result.
    //
    // Weighed against it: on its own — which is how it is used almost every time,
    // and what Ctrl+Del does in one press — the picture IS gray, and "Desaturate"
    // sends people looking for a slider that does not exist. The common reading
    // wins over the stacked one.
    //
    // The identifier already matched: EFFECT_GRAYSCALE, app.effectGrayscale,
    // Command::ToggleGrayscale. Only the shown text moved.
    //
    // SAFE TO CHANGE, checked rather than assumed: this string is a key in
    // app.activeEffectsList and is compared by RendererD2D::ChainEffectByName and
    // by RemoteExec's effects parser — both through THIS constant, so they move
    // together. It is not persisted (the booleans are), and the Android client
    // never sees it: it sends the COMMAND name ToggleGrayscale and labels its own
    // button "Gray".
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
// Prevent helper macros leaking outside this header
#undef STR_STATE_OFF
#undef STR_STATE_ON
#undef STR_SEPARATOR
#undef STR_CACHE_WINDOW
#undef STR_THUMBNAIL_STRIP
