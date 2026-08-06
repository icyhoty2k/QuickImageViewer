// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#pragma once

// Version and product identity — FILE_DESC, PROD_NAME, COPYRIGHT, and the
// numbers you edit to bump a release — all live in Common/Version.h.
#include "Common/Version.h"

#include <iterator>
#include <cstdint>   // uint32_t — Slideshow::TRANSITION_LIST_DEFAULT_MASK
#include "../resources/resource.h"

namespace Constants {
    constexpr const wchar_t *BASE_NAME = L"QuickImageViewer";
    constexpr const wchar_t *APP_CREATOR = L"Ivan Hristov Yanev";

    constexpr const wchar_t *APP_HELP_FOOTER = L"" COPYRIGHT;
    constexpr const wchar_t *APP_TASKBAR_NAME = L"" FILE_DESC;
    // major.minor.patch.build — e.g. 2.80.0.123
    // Defined in Version.cpp, the one TU that sees the generated build number.
    // Deliberately not constexpr: making it so would drag BuildNumber.h in here
    // and rebuild the world on every build. Every use is a runtime string.
    extern const wchar_t *const APP_VERSION;
    constexpr const wchar_t *APP_NAME = BASE_NAME;
    constexpr const wchar_t *WINDOW_CLASS_NAME = BASE_NAME;
    constexpr bool IS_ENABLE_RUN_ON_STARTUP = true; // enable or disable run on startup reg value add/delete
    constexpr bool IS_KEEP_IN_BACKGROUND = true; // enable or disable run on startup reg value add/delete
    constexpr bool IS_OPEN_DIRWND_ON_START = false; // open F6 DirWnd automatically when the app starts

    // Viewport lock (Y). When ON, zoom and pan survive an image change instead of
    // being reset — so flipping through same-framed shots keeps the same detail
    // on screen at the same magnification. Rotation and flips are NOT carried:
    // ApplyOrientationToViewport rewrites them from each file's EXIF tag, and
    // suppressing that would show portrait shots sideways in a mixed folder.
    // Never default this ON — a locked viewport on a fresh install looks like the
    // app failed to fit the image.
    constexpr bool IS_LOCK_VIEWPORT = false;
    // Prefix applied to every registry value name and every data file name
    // when the app is running in dedicated (-dedicated) mode.
    // Guarantees that a dedicated instance and a normal instance never share
    // any persistent state even when running side-by-side on the same machine.
    namespace DedicatedMode {
        constexpr const wchar_t *DEDICATED_MODE_GLOBAL_PREFIX = L"qivDedicated_";
    }

    // =========================================================================
    // CLICKABLE LINKS — app-wide appearance, single source of truth.
    // *** Change these TWO values to restyle every clickable link in the app ***
    // Consumers: RendererD2D overlay path, HelpWnd footer, StatsWnd link rows,
    // HistoryListWnd link fonts.
    // =========================================================================

    namespace Links {
        constexpr COLORREF COLOR = RGB(100, 180, 255); // light blue
        constexpr bool UNDERLINE = true;

        // Derived float channels (0-1) for D2D / DWrite consumers — do not edit.
        // Explicit masking instead of GetR/G/BValue: the macros cast COLORREF→BYTE
        // which triggers C4310 (constant truncation) on /W4.
        constexpr float COLOR_R_F = static_cast<float>(COLOR & 0xFF) / 255.0f;
        constexpr float COLOR_G_F = static_cast<float>((COLOR >> 8) & 0xFF) / 255.0f;
        constexpr float COLOR_B_F = static_cast<float>((COLOR >> 16) & 0xFF) / 255.0f;
    }


    //Mouse
    constexpr bool IS_MOUSE_VERTICAL_REVERSE_SCROLL_DIRECTION = false;
    constexpr bool IS_MOUSE_HORIZONTAL_REVERSE_SCROLL_DIRECTION = false;
    // How many WHEEL_DELTA ticks (each tick = 120) must accumulate before a
    // horizontal scroll triggers a folder change. 1 = every tick, 2 = every
    // other tick, 3 = three ticks, etc. Raise to reduce sensitivity.
    constexpr int MOUSE_HSCROLL_FOLDER_TICKS = 3;




    constexpr float COLOR_ADJUST_STEP = 0.1f; // step for brightness contrast and saturation
    constexpr float DEFAULT_SATURATION = 1.0f; // the default i dont want change when not using it i want original picture
    constexpr float DEFAULT_BRIGHTNESS = 0.0f; // the default i dont want change when not using it i want original picture
    constexpr float DEFAULT_CONTRAST = 1.0f; // the default i dont want change when not using it i want original picture
    constexpr float MIN_MAX_SATURATION = 7.0f;
    constexpr float MIN_MAX_BRIGHTNESS = 1.0f;
    constexpr float MIN_MAX_CONTRAST = 7.0f;

    // ----- Gamma (ImageEffects: SC_COLOR_GAMMA_UP / SC_COLOR_GAMMA_DOWN) -----
    constexpr float DEFAULT_GAMMA = 1.0f; // neutral, no curve applied
    constexpr float GAMMA_STEP = 0.1f;
    constexpr float MIN_GAMMA = 0.1f; // avoid 0 or negative exponents
    constexpr float MAX_GAMMA = 4.0f;

    // ----- Solarize / Threshold (ImageEffects toggles) -----
    constexpr float SOLARIZE_THRESHOLD = 0.5f; // values above this get inverted
    constexpr float BW_THRESHOLD_LEVEL = 0.5f; // black/white cutover point

    // ----- Outline (D2D EdgeDetection) -----
    constexpr float OUTLINE_STRENGTH = 0.5f;
    constexpr float OUTLINE_BLUR_RADIUS = 0.0f;
    static constexpr float ZOOM_CLICK = 3.0f; //  left click zoom multiplier
    static constexpr float ZOOM_CLICK_MIN  = 1.0f;  // minimum (1 = off)
    static constexpr float ZOOM_CLICK_MAX  = 100.0f; // maximum
    static constexpr int OPACITY_STEP = 10; // 0 to 100
    static constexpr int KEYBOARD_PAN_STEP = 30; // W/A/S/D viewport pan step (DPI-scaled in executor)
    static constexpr int KEYBOARD_WINDOW_MOVE_STEP = 20; // Shift+W/A/S/D window move step (DPI-scaled in executor)
    static constexpr int KEYBOARD_WINDOW_RESIZE_STEP = 20; // Shift+Numpad+/- / Shift++/- resize per side, DPI-scaled
    static constexpr int WINDOW_SNAP_DISTANCE = 24; // px from screen edge to trigger drag-end snap
    static constexpr int CONTEXT_MENU_DRAG_TOLERANCE = 4; // px the cursor may move while RMB is down and still count as a "click" (raises the right-click context menu)


    // Custom window messages
    constexpr UINT WM_QIV_CENTER_MSG_HIDE = WM_USER + 10; // Posted by WM_TIMER to hide center msg


    constexpr int IS_BASE_WIDTH  = 1200;
    constexpr int IS_BASE_HEIGHT = 800;

    constexpr bool IS_SWAP_MOUSE_BUTTONS = true;
    constexpr bool IS_CONTEXT_MENU_ENABLED = true; // main-window right-click context menu on/off

    // KIOSK lock — every keyboard and mouse message to the main window is
    // swallowed, so a screen on a wall cannot be driven by a passer-by. Persisted
    // like any other toggle, which is what lets a dedicated screen ship locked
    // from its .ini. The ONLY way back out is the tray icon: TrackPopupMenu runs
    // its own message loop, so the tray menu still works while the window is
    // deaf. Never default this on — a locked app with no tray icon is a brick.
    constexpr bool IS_KIOSK_LOCK_ENABLED = false;

    // Window stays above all others. Ctrl+T toggles it; a dedicated screen sets
    // it in its config so nothing can cover the display.
    constexpr bool IS_ALWAYS_ON_TOP = false;

    // Announce this instance's Local Server on the network, so a phone or another
    // qIV can FIND it instead of being told an address to type.
    //
    // DEFAULTS OFF, and that is not timidity. This app's whole posture is "no
    // cloud, no account, nothing leaves your network" — a machine that starts
    // advertising itself the first time somebody enables the server would be a
    // change to that posture made on the user's behalf. Discovery is opt-in, from
    // the TCP/IP menu, once.
    //
    // DISCOVERY IS NOT ACCESS. The beacon carries the instance name and the port,
    // never a password and never a file. Everything that decides who may actually
    // connect — the AllowList, the password, TLS — is untouched by it, and a
    // beacon nobody may connect to is merely a name on a list.
    constexpr bool IS_REMOTE_BEACON_ENABLED = false;

    // Hold off the screensaver and display sleep while the main window is
    // visible. An unattended screen is useless once Windows blanks it, and no
    // input ever arrives to wake it — least of all with the kiosk lock on.
    // Off by default: the main app has no business overriding a laptop's power
    // plan unless asked.
    constexpr bool IS_KEEP_DISPLAY_AWAKE = false;

    // Desktop wallpaper fit styles — the 6 native Windows options. These indices
    // map 1:1 onto DESKTOP_WALLPAPER_POSITION inside AppCommands.cpp, and index
    // Constants::Messages::WALLPAPER_NAMES for the labels.
    // =========================================================================
    // DEDICATED INSTANCES  (src/Dedicated/*)
    // A dedicated instance is a named, isolated copy of the app — its own mutex,
    // window class, registry namespace and history file — typically parked
    // fullscreen on one monitor running a slideshow. It may additionally pull
    // from a SECOND playlist of "promotions" that is never merged into the
    // normal image playlist.
    // =========================================================================
    // =========================================================================
    // Scrollbar geometry — ONE set, for every scrolled surface in the app.
    //
    // There were three thicknesses: 12 in the Dedicated panel, a hardcoded 10 in
    // Help and Exif, and 6 in the History list. Nothing chose those numbers
    // together, so the same control was a different size depending on which
    // window it was in — most visible at high DPI, where a 6-DIP strip is a hard
    // target and a 12-DIP one is comfortable.
    //
    // DIPs at 96 DPI. Scaled at draw time through UI::ScrollBarThicknessPx, which
    // is the only thing that should read these.
    // =========================================================================
    namespace Scrollbar {
        constexpr int THICKNESS = 10; // strip width / height
        constexpr int MIN_THUMB = 14; // shortest the thumb may get, either axis
    }

    namespace Dedicated {
        // ── F8 panel appearance — single place to restyle the whole window ──
        // Opacity of the Dedicated panel, 0 = invisible .. 255 = opaque.
        constexpr BYTE PANEL_OPACITY = 244;

        // Value colours, chosen so a setting's STATE is readable at a glance
        // without reading the text: green/red for on/off, amber for numbers,
        // blue for paths, violet for multi-choice.
        namespace PanelColors {
            constexpr COLORREF ON       = RGB(126, 211, 133); // toggle enabled
            constexpr COLORREF OFF      = RGB(214, 122, 122); // toggle disabled
            constexpr COLORREF NUMBER   = RGB(232, 190, 110); // numeric value
            constexpr COLORREF PATH     = RGB(120, 186, 244); // folder / file path
            // A SECOND blue, for one list holding two classes of the same kind of
            // item. The Ctrl+F10 command list uses the pair: PATH for a command that
            // takes no value, this for one that does. Both blue because they are both
            // commands; far enough apart in hue to separate at a glance in a list of
            // ninety.
            constexpr COLORREF PATH_ALT = RGB(126, 224, 236); // cyan-leaning blue
            constexpr COLORREF CHOICE   = RGB(196, 160, 240); // one of several options
            constexpr COLORREF TEXT     = RGB(230, 230, 230); // free text value
            constexpr COLORREF HEADER   = RGB(120, 190, 250); // section heading
            constexpr COLORREF STRIPE   = RGB(70, 120, 180);  // heading accent bar
            constexpr COLORREF BTN_MAIN = RGB(58, 104, 158);  // build actions
            constexpr COLORREF BTN_ALT  = RGB(70, 92, 74);    // deploy actions
            constexpr COLORREF WARN     = RGB(222, 148, 96);  // unset / needs attention

            constexpr COLORREF SCROLL_TRACK     = RGB(48, 48, 52);
            constexpr COLORREF SCROLL_THUMB     = RGB(96, 104, 118);
            constexpr COLORREF SCROLL_THUMB_HOT = RGB(132, 156, 196);
        }

        // Scrollbar geometry is Constants::Scrollbar, reached through
        // UI::ScrollBarThicknessPx / ScrollBarMinThumbPx. The PANEL_SCROLLBAR_W
        // and PANEL_SCROLL_MIN_H aliases that briefly bridged the two are gone —
        // every call site reads the shared names directly.

        // Promotion weighting. A promo's weight is its relative chance of being
        // drawn; 65535 is ~65535× more likely than 1.
        constexpr int PROMO_WEIGHT_MIN     = 1;
        constexpr int PROMO_WEIGHT_MAX     = 65535;
        constexpr int PROMO_WEIGHT_DEFAULT = 1;

        // Weight is read from a "#<n>" suffix on the file stem, e.g.
        // "summer-sale#500.jpg". Mirrors the existing -monitorNum#N convention.
        constexpr wchar_t PROMO_WEIGHT_MARKER = L'#';

        // WHEN a promotion appears. TWO triggers exist — an image counter and a
        // wall-clock timer — each configured as a (from, to) pair:
        //
        //   (0, *)    OFF. A missing/zero `from` always disables the trigger,
        //             whatever `to` says. This is deliberate: a half-filled pair
        //             like (0, 90) is a config mistake, and collapsing it to OFF
        //             is safer than inventing a 0..90 range that fires nonstop.
        //   (5, 0)    STRICT — exactly every 5. A zero `to` means "no upper
        //             bound given", so the cadence is fixed at `from`.
        //   (5, 15)   RANDOM — re-rolled between 5 and 15 after every promotion.
        //   (15, 5)   tolerated: `to` below `from` falls back to STRICT `from`.
        //
        // The two triggers run INDEPENDENTLY: each keeps its own countdown, and
        // whichever comes due fires a promotion and re-arms ONLY itself. Both
        // off = promotions disabled, so there is no separate on/off flag.
        // IDENTITY COMES FROM THE EXE FILE, which is what makes collisions
        // impossible: the settings file is the exe's own path with the extension
        // swapped for .ini —
        //     D:\Screens\qIV_dedicated_Lobby.exe
        //     D:\Screens\qIV_dedicated_Lobby.ini
        // Two copies cannot share one name in one folder, so two instances can
        // never resolve to the same settings file, mutex or window class.
        constexpr const wchar_t *SETTINGS_FILE_EXT = L".ini";

        // A dedicated instance has NO history and NO favorites — it is an
        // appliance showing fixed content, not a browser. Instead it keeps two
        // plain folder lists beside the exe, named after it:
        //     imageLists_<exeName>.qim      folders holding the images
        //     promotionList_<exeName>.qpr   folders holding the promotions
        // The actual names are recorded in the .ini so they can be renamed or
        // shared between instances; missing files are generated on startup.
        constexpr const wchar_t *IMAGE_LIST_PREFIX = L"imageLists_";
        constexpr const wchar_t *PROMO_LIST_PREFIX = L"promotionList_";

        // Distinct extensions, not a shared .txt: the file dialogs then filter
        // to exactly one kind, so an image list can never be picked where a
        // promotion list is meant. Both are plain UTF-16 text inside.
        constexpr const wchar_t *IMAGE_LIST_EXT = L".qim"; // qIV image list
        constexpr const wchar_t *PROMO_LIST_EXT = L".qpr"; // qIV promotion list

        // An exe whose FILE NAME contains this substring triggers dedicated
        // mode: with no .ini beside it, a default one is generated rather than
        // falling back to the registry.
        //
        // MUST BE LOWER CASE — the comparison lower-cases the exe name and then
        // searches for this verbatim, so matching is case-insensitive:
        //   qIV_Dedicated_Lobby.exe   ✓
        //   MyDEDICATEDscreen.exe     ✓
        constexpr const wchar_t *EXE_DEDICATED_MARKER = L"dedicated";

        constexpr int PROMO_DISABLED = 0;

        constexpr int PROMO_IMAGES_MAX  = 65535; // fits a 16-bit counter; plenty for "every N images"
        constexpr int PROMO_SECONDS_MAX = 86400; // one day

        // Default: paced by images (every 3-10), timer off.
        constexpr int PROMO_IMAGES_EVERY_DEFAULT   = 3;
        constexpr int PROMO_IMAGES_UPTO_DEFAULT    = 10;
        constexpr int PROMO_SECONDS_EVERY_DEFAULT  = PROMO_DISABLED;
        constexpr int PROMO_SECONDS_UPTO_DEFAULT   = PROMO_DISABLED;

        // How the next promotion is chosen from the promo playlist.
        namespace PromoOrder {
            constexpr int SEQUENTIAL = 0; // folder order, one after another
            constexpr int WEIGHTED   = 1; // weighted random by priority
            constexpr int COUNT      = 2;
        }
    }

    // Command-line-arguments file + generated shortcut (context menu › CmdArgs).
    namespace CmdArgsFile {
        constexpr const wchar_t *EXPORT_NAME   = L"qIVcmdArgs.txt";
        constexpr const wchar_t *SHORTCUT_NAME = L"QuickImageViewer.lnk";
        constexpr const wchar_t *FILE_HEADER   = L"# QuickImageViewer command-line arguments";
    }

    namespace Wallpaper {
        constexpr int FILL    = 0;
        constexpr int FIT     = 1;
        constexpr int STRETCH = 2;
        constexpr int TILE    = 3;
        constexpr int CENTER  = 4;
        constexpr int SPAN    = 5;
        constexpr int COUNT   = 6;
    }
    // Start maximized/borderless on launch. Was the only persisted setting with
    // no named default — load and Restore Defaults each hardcoded their own.
    constexpr bool IS_START_FULLSCREEN     = false;
    constexpr bool IS_CTRL_C_ENABLED       = true;
    constexpr bool IS_THUMB_COPY_ENABLED   = true;
    constexpr bool IS_THUMB_MOVE_ENABLED   = true;
    constexpr bool IS_THUMB_DELETE_ENABLED = true;
    constexpr bool IS_THUMB_PASTE_ENABLED  = true;
    // =========================================================================
    // EXIF WINDOW EMBEDDED THUMBNAIL PREVIEW
    // =========================================================================
    constexpr int EXIF_THUMB_DISPLAY_SIZE = 100; // logical px — max bounding box for embedded thumb
    constexpr int EXIF_THUMB_COL_GAP = 8; // logical px — gap between text column and thumbnail

    // =========================================================================
    // CACHE WINDOW AND CURRENT DIR WINDOW
    // =========================================================================
    constexpr int THUMBNAIL_PANEL_WINDOW_THICKNESS = 120;
    constexpr float THUMBNAIL_PANEL_THUMB_WIDTH = 128.0f; // 128 * 2x DPI = 256px physical → exact Windows 256px thumbnail cache bucket
    constexpr float THUMBNAIL_PANEL_THUMB_HEIGHT = 80.0f;
    constexpr float THUMBNAIL_PANEL_THUMB_SPACING = 18.0f;
    constexpr float THUMBNAIL_PANEL_THUMB_MARGIN = 20.0f;
    constexpr BYTE THUMBNAIL_PANEL_WINDOW_OPACITY = 210;
    constexpr float THUMBNAIL_PANEL_WINDOW_MOUSE_WHEEL_SPEED = 120.0f;
    // Shift multiplies a wheel step, in EVERY scrolling surface — the strips and
    // every list panel through UI::WheelBoost. One number, because a modifier
    // that accelerates by different amounts depending on which window is under
    // the cursor is worse than no accelerator at all.
    //
    // Deliberately NOT the other convention, where Shift+wheel means "scroll
    // sideways": this app has a second wheel for that, and the horizontal
    // wheel is unambiguous where a modifier is not.
    constexpr int WHEEL_SHIFT_ACCELERATOR = 3;
    constexpr int8_t THUMBNAIL_PANEL_WINDOW_MOUSE_WHEEL_DIRECTION = 1; // 1 is forward -1 is reverse
    constexpr bool THUMBNAIL_PANEL_WHEEL_WRAP_AROUND = false; // wrap strip from last→first and first→last on wheel overflow
    constexpr bool THUMBNAIL_PANEL_WHEEL_WRAP_OVERLAY = true; // show center overlay message on strip wrap-around
    // Pixel gap between a panel edge and the taskbar (visual breathing room)
    constexpr int THUMBNAIL_PANEL_TASKBAR_BOTTOM_GAP_HORIZONTAL_PANEL = 6;
    constexpr int THUMBNAIL_PANEL_NEIGHBOUR_GAP_VERTICAL_PANEL = 2; // gap between vertical and horizontal panels
    //   position 0 : centered floating panel (80 % wide, thumb-height tall)
    //   position 1 : top edge strip (full width)
    //   position 2 : right edge strip (full height)
    //   position 3 : bottom edge strip (full width)
    //   position 4 : left edge strip (full height)
    //cache window
    constexpr int8_t CACHE_WINDOW_POSITION = 3;
    //current dir window (F5 — always independent, always top strip)
    constexpr int8_t CURRENT_DIR_WINDOW_POSITION = 1;

    // =========================================================================
    // Spawned DirWnd instances (from HistoryWnd Shift+Enter)
    // =========================================================================
    // Maximum number of DirWnd instances that can be spawned from history.
    // Slot assignment (0-based spawn index → panel position):
    //   spawn 0 → left  strip (position 4)
    //   spawn 1 → right strip (position 2)
    //   spawn 2 → center floating (position 0)
    //   spawn 3 → wraps back to 0 (oldest slot reused)
    constexpr int DIR_WND_MAX_INSTANCES = 4; // top, left, right, bottom
    constexpr int8_t DIR_WND_SPAWN_POSITIONS[DIR_WND_MAX_INSTANCES] = {1, 4, 2, 3};


    namespace ThumbnailPanel {
        // Geometry & Layout (non-color)
        constexpr float SELECTION_BORDER_THICKNESS = 3.0f;
        constexpr float HOVER_THICKNESS = 1.0f;
        constexpr float SCROLLBAR_THICKNESS = 8.0f; // px width of the strip
        constexpr float SCROLLBAR_MIN_THUMB = 20.0f; // minimum thumb length in px
        // Opacity (0.0–1.0) applied to a thumbnail that has been cut to the clipboard.
        constexpr float THUMBNAIL_CUT_OPACITY = 0.35f;

        // =====================================================================
        // Visual Effects (U key toggles master at runtime)
        // Each bool can be independently disabled here; all are guarded by
        // AppState::thumbnailEffectsEnabled (the runtime master switch).
        // =====================================================================
        namespace ThumbnailEffects {
            constexpr bool EFFECTS_MASTER_ENABLED = false; // AppState init default
            constexpr bool EFFECT_ROUNDED_CORNERS = true; // overdraw corner bites with bg color
            constexpr bool EFFECT_GLOW_BORDER = true; // accent-color border on selected thumb
            constexpr bool EFFECT_HOVER_SCALE = true; // scale hovered thumb by HOVER_SCALE_FACTOR

            constexpr float CORNER_RADIUS = 6.0f; // logical px — DPI-scaled at runtime
            constexpr float HOVER_SCALE_FACTOR = 1.05f; // uniform enlarge on hover
            // Glow border: LightGreen — same hue as the classic selection border
            constexpr float GLOW_COLOR_R = 0.565f;
            constexpr float GLOW_COLOR_G = 0.933f;
            constexpr float GLOW_COLOR_B = 0.565f;
            constexpr float GLOW_COLOR_A = 1.0f;
            // Fake glow: three rounded strokes at decreasing opacity — reads as a
            // soft halo with zero GPU-blur cost. All values logical px, DPI-scaled.
            constexpr float GLOW_OUTER_OFFSET = 5.0f; // rect inflation of outermost pass
            constexpr float GLOW_OUTER_STROKE = 6.0f;
            constexpr float GLOW_OUTER_OPACITY = 0.12f;
            constexpr float GLOW_MID_OFFSET = 2.0f;
            constexpr float GLOW_MID_STROKE = 3.0f;
            constexpr float GLOW_MID_OPACITY = 0.30f;
            constexpr float GLOW_CORE_STROKE = 2.0f; // crisp innermost border
            constexpr float GLOW_CORE_OPACITY = 0.95f;
        }

        // Scrollbar position enums
        enum class ScrollbarSide { LEFT = 0, RIGHT = 1 };

        enum class ScrollbarEdge { TOP = 0, BOTTOM = 1 };

        // Per-position scrollbar placement
        // Position mapping: 0=center, 1=top, 2=right, 3=bottom, 4=left
        constexpr ScrollbarSide SCROLLBAR_POS_RIGHT_PANEL = ScrollbarSide::LEFT; // right panel (pos 2), scrollbar on left
        constexpr ScrollbarSide SCROLLBAR_POS_LEFT_PANEL = ScrollbarSide::RIGHT; // left panel (pos 4), scrollbar on right
        constexpr ScrollbarEdge SCROLLBAR_POS_TOP_PANEL = ScrollbarEdge::BOTTOM; // top panel (pos 1), scrollbar at bottom
        constexpr ScrollbarEdge SCROLLBAR_POS_BOTTOM_PANEL = ScrollbarEdge::BOTTOM; // bottom panel (pos 3), scrollbar at bottom

        // Note: All colors moved to ConstantsTheme.h
    }

    // =========================================================================
    // Cache optimization
    // =========================================================================
    constexpr const int VRAM_CACHE_THUMBS_THREADS_COUNT = 4; // fallback if processor has less than 8 thread otherwise dynamic thread count / 2
    constexpr const int VRAM_CACHE_DECODER_THREADS_COUNT = 2;
    constexpr int IS_VRAM_CACHE_IMAGES_COUNT = 20;

    // --- IO worker threads -----------------------------------------------------
    //
    // ONE NUMBER FOR EVERY DRIVE. qIV used to probe the physical device for a
    // seek penalty and pick 1 for an HDD, 2 for an SSD. That was dropped, and the
    // reasoning is worth keeping because it looks like a downgrade and is not:
    //
    //   * These threads only READ. The task is CreateFile / GetFileSize /
    //     ReadFile / CloseHandle, and the bytes then go to the decoder pool.
    //   * A 5 MB photo reads from NVMe in 1-3 ms and decodes in 30-80 ms. Decode
    //     is 20-50x the cost, so the IO threads are idle most of the time and
    //     widening that pipe buys nothing. Four would be no faster than two.
    //   * The probe opened \\.\PhysicalDriveN, which SPINS UP a sleeping disk —
    //     noise, wear and power on a drive the user never asked to touch — to
    //     decide between one thread and two.
    //   * It was also the most bug-prone code on the startup path: a detached
    //     thread writing file-scope statics, which is a use-after-free at exit.
    //
    // TWO, not one: on an SSD the second thread overlaps one file's open latency
    // with the previous file's read. On an HDD two threads interleave and cost a
    // few extra seeks per image — a real but modest loss, and qIV preloads
    // neighbours anyway, so the head is moving either way.
    //
    // Tune here if a measurement ever says otherwise. A folder on a slow HDD with
    // Next held down is the test that would settle it.
    constexpr size_t IO_WORKER_THREADS = 2;

    // --- Animated GIF budget ---------------------------------------------------
    //
    // Every frame of an animation is uploaded as a full-canvas PBGRA bitmap, so a
    // frame costs width × height × 4 bytes of VRAM regardless of how little of it
    // actually changed. A 1080p frame is ~8 MB; a 500-frame animation is therefore
    // about 4 GB, and the image cache above counts IMAGES, not bytes — so a single
    // pathological GIF can exhaust VRAM while the cache believes it is holding one
    // entry out of twenty.
    //
    // Reaching either limit TRUNCATES the animation: it loops over the frames that
    // were decoded rather than refusing to show the file. A partial animation is a
    // far better outcome than a failed allocation, and for the files this actually
    // catches — multi-thousand-frame novelty GIFs — nobody watches to the end.
    constexpr size_t GIF_MAX_DECODED_BYTES = 256ull * 1024 * 1024; // 256 MB of frames
    constexpr size_t GIF_MAX_FRAMES        = 600;                  // second, cheaper guard
    // VRAM budget for the dir-panel thumbnail cache.
    // Each entry is CACHE_THUMB_WIDTH * CACHE_THUMB_HEIGHT * 4 bytes ≈ 37 KB
    // after scaling.  512 MB holds ~14 000 thumbnails — far more than any
    // realistic folder.  Increase if you open folders with tens of thousands
    // of images; decrease on low-VRAM cards.
    // Shell thumbnail retrieval flags (SIIGBF — int bitmask from shobjidl.h):
    //   0x00000000  SIIGBF_RESIZETOFIT    — fit within requested SIZE, generate+cache if needed (default)
    //   0x00000001  SIIGBF_BIGGERSIZEOK   — allow returning a larger bitmap than requested
    //   0x00000008  SIIGBF_THUMBNAILONLY  — only thumbnail, no icon fallback
    //   0x00000010  SIIGBF_INCACHEONLY    — return cached entry only, never generate
    // Default: let Windows generate and persistently cache thumbnails on first access.
    constexpr int SHELL_THUMB_FLAGS = 0x00000000;

    // Maximum path buffer size for Win32 file dialogs (OPENFILENAMEW documented max).
    // Use everywhere a wchar_t buffer receives a user-selected or drag-dropped path.
    constexpr DWORD MAX_FILE_PATH = 32767;

    constexpr int IS_DIR_THUMB_CACHE_BUDGET_MB = 512;
    constexpr int IS_PRELOAD_LOOKASIDE_COUNT = 1;
    constexpr const int PRELOAD_TIMER_COUNTDOWN = 60; // {ms} this is used to delay preloading if user scrolls very fast
    //==========================Cache optimization====================================
    //end Saveable options

    // Custom window messages
    constexpr UINT WM_QIV_PENDING_UPLOADS = WM_USER + 1; // Posted by background decoder thread
    constexpr UINT WM_QIV_REPAINT = WM_USER + 2; // Signal to UI thread that bitmap is ready
    constexpr UINT WM_QIV_SVG_READY = WM_USER + 3; // Posted by IO thread when SVG bytes are loaded
    constexpr UINT WM_QIV_OPEN_FILE = WM_USER + 4; // Posted by DropTarget/WM_COPYDATA; LPARAM = new std::wstring*
    constexpr UINT WM_QIV_SWITCH_TO_FIND = WM_USER + 5; // FindWnd  ← PANEL_SWITCH_TO_FIND_CHAR typed in JumpToWnd
    constexpr UINT WM_QIV_SWITCH_TO_JUMP = WM_USER + 6; // JumpToWnd ← PANEL_SWITCH_TO_JUMP_CHAR typed in FindWnd
    constexpr UINT WM_QIV_SCAN_COMPLETE = WM_USER + 7; // Background dir scan done; LPARAM = new ScanResult*
    constexpr UINT WM_QIV_HISTORY_VALIDATED = WM_USER + 8; // Background history folder scan done; WPARAM = generation, LPARAM = new std::vector<DirScanResult>*
    constexpr UINT WM_QIV_DIR_CHANGED = WM_USER + 9; // Posted by DirWatcher thread when a file-system change is detected
    // WM_USER+10 is WM_QIV_CENTER_MSG_HIDE, declared near the top of this file.
    // Posted by a remote client thread; LPARAM = new std::shared_ptr<Remote::RemoteCall>*,
    // which the UI thread deletes after executing. See src/Rem_TCP_IP/RemoteServer.h.
    constexpr UINT WM_QIV_REMOTE_COMMAND = WM_USER + 11;
    // Posted by the listener thread when it stops on its own (bind failed, socket
    // died). Lets the panel drop back to "stopped" without polling. WPARAM = a
    // Constants::RemoteTcpIp::ERR_* code, 0 for a clean stop.
    constexpr UINT WM_QIV_REMOTE_STOPPED = WM_USER + 12;
    // Posted by a MIRROR sender thread when an observed target pushed an EVENT
    // line. LPARAM = new std::wstring*, owned and deleted by the handler.
    // Executed on the UI thread under the inbound guard, so an observer never
    // re-forwards what it was shown.
    constexpr UINT WM_QIV_REMOTE_EVENT = WM_USER + 13;
    // Posted by a MIRROR sender thread when a target's reply named a DIFFERENT
    // file than the one we landed on — the two playlists have diverged, so the
    // index we sent meant something else there. WPARAM = target id. The UI
    // thread answers by building a `sync` (it alone may read app.playlist) and
    // resending. See RemoteMirror.h on why indices are sent at all.
    constexpr UINT WM_QIV_REMOTE_DESYNC = WM_USER + 14;
    // Posted to the RemoteLog PANEL (not the main window) when an entry lands.
    // Producers are a mirror sender thread and the inbound UI path, so this is
    // the only safe way to tell a window from the first of those.
    //
    // COALESCED at the source: RemoteLog holds a "already posted" flag and skips
    // the post while one is outstanding, so a burst of mirrored keystrokes costs
    // one message and one rebuild, not one of each per keystroke. No payload —
    // the handler re-snapshots, because by the time it runs there may be more.
    constexpr UINT WM_QIV_REMOTE_LOG_ADDED = WM_USER + 15;
    // Posted to the F10 Remote Servers console when a target's CONNECTION state
    // changes — connected/disconnected, or the reason it is down. Posted from a
    // sender thread, which is why it is a message and not a call.
    //
    // Connection state ONLY, deliberately not lag: lag is rewritten on every
    // mirrored keystroke, and that console's rebuild stats each row's exe to
    // fill the "exe missing" column. A notification per keystroke would put a
    // filesystem call per target on the mirror path.
    //
    // Coalesced at the source, same as WM_QIV_REMOTE_LOG_ADDED. No payload —
    // a subscriber re-reads the whole target list, which is cheap and cannot go
    // out of step with itself.
    //
    // SEVERAL SUBSCRIBERS, each with its own coalescing gate: the F10 console and
    // the Ctrl+F10 Send Command panel both list connected instances, and while this
    // notification took a single HWND the second one to open stole the first one's
    // messages. See Remote::Mirror::AddPanelNotify.
    constexpr UINT WM_QIV_REMOTE_TARGETS_CHANGED = WM_USER + 16;
    // One reply to a HAND-SENT command (Ctrl+F10). LPARAM = new
    // Remote::Mirror::CmdReply*, owned and deleted by the handler.
    //
    // Mirrored keystrokes deliberately have no such message — nobody reads the
    // answer to "next image" and posting one per keystroke per target would be a
    // message storm. A command somebody typed is the opposite: the answer is the
    // entire reason it was sent.
    constexpr UINT WM_QIV_REMOTE_CMD_REPLY = WM_USER + 17;
    // Ctrl+Alt+Enter — the answer to "what are you showing?". Posted by a MIRROR
    // sender thread after it asked one target `QueryState` and rebuilt the full
    // path from the folder and name it reported. LPARAM = new std::wstring*, owned
    // and deleted by the handler; an EMPTY string means that instance is showing
    // nothing, which is an answer and not a failure.
    //
    // A message rather than a return value because the ask happens on the sender
    // thread — the UI thread never waits on a socket — and only the UI thread may
    // touch the interjection state the picture is then displayed through.
    constexpr UINT WM_QIV_REMOTE_PULLED = WM_USER + 18;

    // The listener started, stopped, or gained/lost a client — repaint the
    // overlay's server indicator. No payload; the handler reads the live figures.
    //
    // Posted rather than polled on a timer: the count changes only on connect
    // and disconnect, which is rare, and a viewer nobody is driving should not
    // wake up once a second to discover nothing happened.
    constexpr UINT WM_QIV_REMOTE_CLIENTS = WM_USER + 19;

    // ---------------------------------------------------------------------------
    // Directory watcher (ReadDirectoryChangesW / FindFirstChangeNotification)
    // ---------------------------------------------------------------------------
    constexpr bool WATCH_DIR_FOR_CHANGES = true; // master on/off switch
    constexpr UINT DIR_WATCHER_DEBOUNCE_MS = 400; // quiet period before auto-refresh fires
    constexpr UINT_PTR DIR_WATCHER_TIMER_ID = 1008; // main-window dir-change debounce tick
    constexpr UINT_PTR PANEL_DIR_WATCHER_TIMER_ID = 1009; // per-panel dir-change debounce tick

    // First-character panel-switch triggers
    constexpr wchar_t PANEL_SWITCH_TO_JUMP_CHAR = L'#'; // type this in FindWnd  to open JumpToWnd
    constexpr wchar_t PANEL_SWITCH_TO_FIND_CHAR = L'@'; // type this in JumpToWnd to open FindWnd
    // =============================================================================

    // =========================================================================
    // REMOTE CONTROL over TCP/IP  (src/Rem_TCP_IP)
    //
    // The listener is OFF unless explicitly switched on — either by command-line
    // switches or by qivLocalServer.ini beside the exe. There is no third way
    // in, and no default that opens a socket. A viewer that has never been
    // configured for remote control never binds a port.
    //
    // Full design record: docs/REMOTE_TCP_IP_SPEC.md
    // =========================================================================
    namespace RemoteTcpIp {
        // What just happened to the client list, carried in WM_QIV_REMOTE_CLIENTS'
        // wParam so the overlay can colour its blink by the event.
        //
        // ONLY THE SOCKET THREAD KNOWS THIS. By the time the UI thread handles
        // the message the connection is gone, and the count alone cannot say
        // whether a client meant to leave — which is the entire distinction
        // between "somebody finished" and "a screen vanished".
        enum class ClientEvent : unsigned {
            Other = 0,   // the listener started or stopped — not an arrival
            Joined,      // a client authenticated and is on the list
            LeftClean,   // it sent `bye` first
            LeftAbrupt,  // the socket ended without one: reset, crash, out of range
            Ejected,     // WE ended it — kicked, timed-kicked or banned
        };

        // --- The listener's own file ---
        //
        // ITS OWN FILE, not a section of the instance .ini, for the reason set
        // out in Persistence/IniFile.h: the exe-derived .ini existing is what
        // makes the whole application file-backed, so parking the listener
        // configuration there meant configuring a port could silently move every
        // unrelated setting off the registry. A fixed name is invisible to that
        // check, and the listener configuration is then just a file — deletable,
        // hand-editable, and copyable between machines on its own.
        constexpr const wchar_t *LOCAL_SERVER_FILE_NAME = L"qivLocalServer.ini";
        constexpr const wchar_t *LOCAL_SERVER_FILE_HEADER =
            L"Local remote-control server (F9). Delete this file to reset it.";

        // Documentation written above each key when the file is CREATED. The
        // file is meant to be hand-edited, so it explains itself rather than
        // requiring the manual. Kept beside the key names so the two cannot
        // drift; composed into a file by RemoteSettings::SaveToIni.
        constexpr const wchar_t *DOC_AUTOSTART =
            L"; Start the listener automatically when qIV launches (true/false).\r\n"
            L"; Off by default. The Start button in F9 works either way - this only\r\n"
            L"; decides what happens at launch.\r\n";
        constexpr const wchar_t *DOC_NAME =
            L"; REQUIRED. How this instance identifies itself to whoever drives it.\r\n"
            L"; Names must be distinct - the listener refuses to start without one.\r\n";
        constexpr const wchar_t *DOC_IP_ADDRESS =
            L"; Which local interfaces to listen on - NOT the address you connect to.\r\n"
            L";   127.0.0.1 = this machine only, no firewall prompt, no encryption needed\r\n"
            L";   0.0.0.0   = every interface (LAN / internet)\r\n"
            L"; Anything other than 127.x forces TLS and requires a Password.\r\n";
        constexpr const wchar_t *DOC_PORT_NO =
            L"; TCP port to listen on, 1-65535. No default - 0 means unconfigured\r\n"
            L"; and the listener will not start.\r\n";
        constexpr const wchar_t *DOC_ALLOW_LIST =
            L"; IPs allowed to connect, separated by , or ; (NOT spaces).\r\n"
            L"; EMPTY DENIES EVERYONE - this list fails closed by design.\r\n"
            L"; A trailing * matches a prefix:  192.168.1.*\r\n"
            L"; Blocked addresses live in qivRemoteServerBlacklist.ini and win over this.\r\n";
        constexpr const wchar_t *DOC_PASSWORD =
            L"; PBKDF2-HMAC-SHA256 of the password, as <iterations>$<salt>$<digest>.\r\n"
            L"; Never the password itself, and never sent over the wire - clients prove\r\n"
            L"; they know it by answering a challenge.\r\n"
            L"; Set it in the F9 panel; typing a plain password here does NOT work.\r\n"
            L"; Required unless IpAddress is loopback. Anyone who can read this line can\r\n"
            L"; authenticate, so treat this file as secret.\r\n";
        constexpr const wchar_t *DOC_MAX_CONNS =
            L"; Simultaneous clients, 1-99. Further callers are refused.\r\n";

        // --- .ini section and key names ---
        constexpr const wchar_t *INI_SECTION      = L"REMOTE_TCP_IP";
        // AUTOSTART, not "Enable" — it decides whether the listener comes up on
        // its own at launch, and nothing else. It used to also gate the F9
        // panel's Start button, which made "Enable=false" mean two things at
        // once and produced the genuinely baffling state of pressing Start and
        // being told the server is disabled. Start now always starts.
        constexpr const wchar_t *KEY_AUTOSTART    = L"Autostart";
        constexpr const wchar_t *KEY_NAME         = L"Name";
        constexpr const wchar_t *KEY_IP_ADDRESS   = L"IpAddress";
        constexpr const wchar_t *KEY_PORT_NO      = L"PortNo";
        constexpr const wchar_t *KEY_ALLOW_LIST   = L"AllowList";
        constexpr const wchar_t *KEY_PASSWORD     = L"Password";
        constexpr const wchar_t *KEY_MAX_CONNS    = L"MaxConnections";

        // --- The blacklist, in a file of its own -----------------------------
        //
        // Separate from the listener's configuration because it is the one part
        // the PROGRAM writes: crossing the failed-authentication threshold adds
        // the offending address here, unattended, possibly at three in the
        // morning. Configuration is what the user decides; this is a log of what
        // the machine decided, and mixing the two means a hand-edited settings
        // file racing an automatic writer.
        //
        // LINE-BASED, one entry per line, NOT key=value:
        //
        //     203.0.113.7;2026-08-03 04:11:52;5 failed authentications in 10 min
        //     198.51.100.0/…            <- ignored, not an address literal
        //     192.0.2.44                <- bare address, hand-added, equally valid
        //
        // Bare addresses are accepted so blocking something by hand is one line
        // with nothing to look up. Anything the PROGRAM adds carries all three
        // fields, because an automatic entry that does not say when or why is an
        // entry nobody can safely delete later.
        //
        // Trailing "*" works exactly as it does in the AllowList, and for the
        // same reason — one rule, one implementation (Remote::AddressMatches).
        constexpr const wchar_t *BLACKLIST_FILE_NAME = L"qivRemoteServerBlacklist.ini";
        constexpr const wchar_t *BLACKLIST_FILE_HEADER =
            L"; Blocked addresses, one per line:  <ip>;<date time>;<reason>\r\n"
            L"; The date and reason are optional - a bare IP on its own line works.\r\n"
            L"; Lines starting with ; or # are ignored. Trailing * matches a subnet.\r\n"
            L"; qIV appends to this file automatically when an address fails to\r\n"
            L"; authenticate too often. Delete a line to unblock that address.\r\n";

        // Field separator. Semicolon rather than comma because an address never
        // contains one and a reason plausibly contains a comma.
        constexpr wchar_t BLACKLIST_FIELD_SEP = L';';

        // Also the comment marker, which is why a line is only a COMMENT when
        // the separator is the FIRST character — "1.2.3.4;…" is an entry.
        constexpr wchar_t BLACKLIST_COMMENT_ALT = L'#';

        // Upper bound on entries held in memory, so a file that has grown for
        // years — or been tampered with — cannot cost unbounded memory or an
        // unbounded scan on every accept().
        constexpr size_t BLACKLIST_MAX = 8192;

        // --- Timed blocks (a kick that keeps the peer out for a while) -------
        //
        // Far smaller than the permanent list because these are made BY HAND,
        // one press at a time, and they expire on their own. Thousands of them
        // would mean something had gone wrong rather than that somebody had been
        // busy.
        constexpr size_t TIMED_BLOCK_MAX = 256;

        // What the panel's duration prompt opens on. Ten minutes is long enough
        // to outlast a bot's retry loop and short enough that shutting out a
        // real client by mistake fixes itself before anyone files a complaint.
        constexpr int TIMED_BLOCK_DEFAULT_MIN = 10;
        constexpr int TIMED_BLOCK_MIN_MIN     = 1;
        // A day. Past this, the permanent list is the honest tool — a "timed"
        // block measured in weeks is a ban that forgets itself at the next
        // restart, which is the worst of both.
        constexpr int TIMED_BLOCK_MAX_MIN     = 1440;

        // --- Defaults ---
        // Never autostart by default. A viewer that binds a port because nobody
        // said otherwise is a viewer that surprises its owner — and with the
        // file absent entirely, which is the normal case, nothing here is even
        // read.
        constexpr bool AUTOSTART_DEFAULT = false;

        // The Ctrl+F12 remote log. OFF, and for the same reason: it is a
        // DIAGNOSTIC, and a diagnostic that is always on is a cost everybody
        // pays for the benefit of the one session that needed it.
        //
        // What the cost actually is: with it off, the record point in the sender
        // thread short-circuits on one relaxed atomic load and copies no strings
        // at all. With it on, every exchange copies four wstrings and takes a
        // mutex. Neither is large next to a socket round trip — but "neither is
        // large" is how a viewer built for hours-long slideshows accumulates a
        // 20000-entry deque nobody asked for.
        //
        // Session-only, deliberately not persisted: switching logging on is
        // something you do WHILE looking at a problem, and a viewer that came
        // back from a restart still logging would be recording for nobody.
        constexpr bool REMOTE_LOG_DEFAULT = false;
        // Loopback: reachable only from this machine, and Windows Firewall never
        // prompts. A wall screen opts into 0.0.0.0 (every interface) knowingly.
        //
        // Also the value a new AllowList is seeded with. Seeded only — the accept
        // gate gives it no special status, so it can be removed like any entry.
        constexpr const wchar_t *BIND_ADDRESS_DEFAULT = L"127.0.0.1";
        constexpr const wchar_t *BIND_ADDRESS_ANY     = L"0.0.0.0";
        // Every interface, DUAL STACK. Distinct from 0.0.0.0 because that one is
        // IPv4 only — the socket family follows the literal — and a mobile client
        // on a carrier that hands out no IPv4 can reach this and nothing else.
        // Start() clears IPV6_V6ONLY on it, so one socket serves both families.
        constexpr const wchar_t *BIND_ADDRESS_ANY6    = L"::";

        // 0 means "not configured" — still refused by WhyCannotStart, since a
        // hand-edited file can say PortNo=0.
        //
        // A DEFAULT is now safe, and was not before: nothing binds a socket
        // unless Autostart is on or Start is pressed, so pre-filling the port no
        // longer risks an unconfigured instance opening one. It saves the user
        // filling in a field that has one obvious answer.
        constexpr int PORT_UNSET   = 0;
        constexpr int PORT_DEFAULT = 8770;
        constexpr int PORT_MIN     = 1;
        constexpr int PORT_MAX     = 65535;

        // A name is REQUIRED to start, so defaulting it means F9 opens ready to
        // run rather than with a blank that must be filled before anything works.
        // Distinct names still matter when several instances are driven at once —
        // this is a starting point, not a recommendation to leave it alone.
        constexpr const wchar_t *NAME_DEFAULT = L"qIV";

        // Simultaneous clients. Values outside the range fall back to DEFAULT
        // rather than refusing to start.
        constexpr int MAX_CONNECTIONS_MIN     = 1;
        constexpr int MAX_CONNECTIONS_MAX     = 99;
        // 4: a phone, a second phone or a desktop client, and headroom for a
        // connection still authenticating. 1 was needlessly tight now that the
        // failed-auth delay holds a slot for a second.
        constexpr int MAX_CONNECTIONS_DEFAULT = 4;

        // Separators accepted inside AllowList / BlackList, so a hand-edited
        // file tolerates either convention.
        constexpr const wchar_t *LIST_SEPARATORS = L",;";

        // --- TLS (src/Rem_TCP_IP/RemoteTls.*) --------------------------------
        //
        // Self-signed, pinned by fingerprint. No CA is involved and none could
        // be: this server is reached by IP on a home connection, which no public
        // authority will ever issue for.
        constexpr const wchar_t *TLS_CERT_FILE_NAME = L"qivServerCert.pfx";
        constexpr const wchar_t *TLS_CERT_SUBJECT   = L"CN=QuickImageViewer Remote";

        // Ten years. A self-signed certificate the user pins by hand gains
        // nothing from expiring — expiry exists so a compromised CA-issued
        // identity stops being trusted eventually, and here the trust anchor IS
        // the pin. A short life would only mean the pin breaking on a schedule.
        constexpr int TLS_CERT_VALID_YEARS = 10;

        // RSA-2048. Not 4096: the handshake cost lands on a phone, the key is
        // pinned rather than chained, and nobody is factoring 2048 bits to read
        // somebody's wallpaper.
        constexpr DWORD TLS_KEY_BITS = 2048;

        // Bound on ONE handshake, so a peer that opens a socket and then says
        // nothing cannot hold a client thread forever. The plaintext path has no
        // equivalent because it has nothing to wait for.
        constexpr int TLS_HANDSHAKE_TIMEOUT_MS = 15000;

        // Upper bound on handshake token exchange, as a guard against a peer
        // that keeps a handshake going indefinitely.
        constexpr int TLS_HANDSHAKE_MAX_STEPS = 32;

        // --- Password derivation -------------------------------------------
        //
        // PBKDF2-HMAC-SHA256. The stored value and the shared secret are the
        // SAME number, so whatever produces it is also what an attacker has to
        // run per guess if the .ini ever leaks. A bare SHA-256 costs them one
        // hash — billions per second on a GPU, which makes any password a human
        // chose recoverable in minutes.
        //
        // 210,000 is OWASP's current figure for this PRF. It costs the CLIENT
        // roughly a fifth of a second at connect time and costs the SERVER
        // nothing at all: the server stores the derived value and uses it
        // directly as the HMAC key, so it runs this only when a password is set.
        //
        // WRITTEN INTO the stored value rather than assumed, so raising it later
        // does not invalidate every existing password — a value carrying its own
        // cost parameter can be verified by any build.
        constexpr int PBKDF2_ITERATIONS = 210000;

        // Length of the random salt, in bytes. 16 is the usual floor and its job
        // is uniqueness, not secrecy — it travels in the challenge.
        constexpr size_t PBKDF2_SALT_LEN = 16;

        // --- Brute-force resistance ----------------------------------------
        //
        // The handshake proves knowledge of a password, and a password is only
        // as strong as the number of guesses an attacker gets. A failed AUTH
        // costs an attacker one TCP connection and nothing else, so without a
        // limit the whole scheme reduces to how fast sockets can be opened.
        //
        // Counted PER PEER ADDRESS rather than globally: one attacker must not
        // be able to lock out every other client by failing on purpose, which a
        // global counter would allow and which is a denial of service dressed
        // up as a security control.
        //
        // The delay is applied BEFORE the failure is reported, on the socket
        // thread, so it costs the attacker wall-clock time on every attempt
        // even before the ban engages. It is small enough to be invisible to a
        // human who mistyped and large enough to make a dictionary run
        // impractical when multiplied by the whole keyspace.
        constexpr int AUTH_FAIL_DELAY_MS = 1000;

        // Failures from one address before it is refused outright. Generous:
        // the case being served is a phone with a saved-but-stale password
        // retrying, not a careful attacker, and five is past any typo.
        constexpr int AUTH_MAX_FAILURES = 5;

        // Failures older than this are forgotten, so an address that fails once
        // a day forever is never blacklisted for it. The counter measures a
        // BURST — five wrong guesses in ten minutes is an attack; five spread
        // over a fortnight is somebody who keeps mistyping.
        constexpr int AUTH_FAIL_WINDOW_MS = 10 * 60 * 1000;   // 10 minutes

        // Upper bound on tracked addresses, so a spoofed-source flood cannot
        // grow the table without limit. At the cap the oldest entry is dropped:
        // an attacker CAN evict their own ban that way, but only by producing
        // enough distinct source addresses that they were never being stopped
        // by an address-keyed rule to begin with.
        constexpr size_t AUTH_TRACK_MAX = 1024;

        // --- Handshake deadline ---------------------------------------------
        //
        // Bounds every read from the moment a connection is accepted until it is
        // authenticated: the TLS handshake, the banner, and the AUTH exchange.
        // LIFTED once the client is in — commands can be minutes apart, and an
        // idle authenticated connection waiting for the next keystroke is the
        // normal state of a mirrored screen, not a fault.
        //
        // WITHOUT THIS THE LISTENER IS TRIVIALLY DENIED. A peer that completes
        // the TCP handshake and then says nothing holds a worker thread blocked
        // in recv() for ever; MAX_CONNECTIONS of those and no real client can get
        // in until the app restarts. It costs an attacker one open socket each,
        // needs no password, and survives every gate above — the AllowList admits
        // the address, and nothing after that ever asks it to speak.
        //
        // Ten seconds because the slowest legitimate case is a phone deriving
        // PBKDF2 at 210,000 iterations over a cellular link, which is well under
        // a second of compute and a few round trips of network.
        constexpr int HANDSHAKE_TIMEOUT_MS = 10000;

        // Bounds a SEND that makes no progress at all, for the whole life of a
        // connection. Deliberately generous: a 32 MB image to a phone on a bad
        // link is slow by design, and this must never cut one short. What it
        // stops is the other thing — a peer that accepts a connection and then
        // never drains its receive window, which without a timeout pins the
        // sending thread indefinitely.
        constexpr int SEND_TIMEOUT_MS = 30000;

        // --- TCP keepalive -------------------------------------------------
        //
        // WHAT THIS IS FOR IS NAT, not a slow peer. An authenticated connection
        // deliberately has NO receive timeout (see the note beside
        // HANDSHAKE_TIMEOUT_MS): a mirrored screen is supposed to sit silent for
        // minutes. On a LAN that is free. Across a home router it is not — a NAT
        // mapping with no traffic through it is discarded after a few minutes,
        // and the discard is SILENT IN BOTH DIRECTIONS. Neither end sees a close,
        // so the server thread stays blocked in recv() on a socket that can never
        // deliver anything again and the driving end's row stays green while the
        // mirror does nothing. That failure has no other detection path, because
        // the whole design of an idle mirror is that nothing is sent.
        //
        // Sixty seconds because typical consumer NAT UDP/TCP idle timeouts start
        // around five minutes; this is comfortably inside the shortest of them
        // while costing one empty segment a minute per connection.
        constexpr DWORD KEEPALIVE_IDLE_MS     = 60000;

        // Gap between probes once one has gone unanswered. Windows fixes the
        // retry COUNT at 10 on Vista and later — it is not settable through
        // SIO_KEEPALIVE_VALS — so this is the only lever on how fast a dead peer
        // is declared dead: 10 s x 10 gives roughly 100 s after the idle period.
        constexpr DWORD KEEPALIVE_INTERVAL_MS = 10000;

        // Longest peer-chosen name kept from "hello <name>". A LABEL for the
        // log and nothing else — capped so a peer cannot pad its name until the
        // address it is shown beside scrolls out of the column, which would let
        // it choose what the log appears to say about where it came from.
        constexpr size_t PEER_NAME_MAX = 40;

        // --- The driving side's target list (src/Rem_TCP_IP/RemotesFile.*) ---
        // Beside the exe, and DELIBERATELY not the exe-derived name: an .ini
        // called qIV.ini next to qIV.exe is what makes the whole app switch from
        // registry-backed to file-backed (Dedicated::DetectStartupMode). This
        // name is invisible to that check, so writing it changes nothing about
        // how the copy persists everything else.
        // Named for what it holds — the list of OTHER instances this one drives
        // (F10). "qivRemotes" read as though it were this instance's own remote
        // settings, which is qivLocalServer.ini and the opposite direction.
        constexpr const wchar_t *REMOTES_FILE_NAME = L"qivRemoteServers.ini";
        constexpr const wchar_t *REMOTES_SECTION   = L"Remotes";

        // Upper bound on rows, so a corrupted file cannot spin the reader. Far
        // beyond any real setup — the use case is a handful of monitors.
        constexpr int REMOTES_MAX = 64;

        // --- Wire protocol -------------------------------------------------
        // Newline-delimited UTF-8 text: "<command> [payload]\n". Deliberately
        // plain, so netcat, curl, telnet and a five-line Python script are all
        // first-class clients with nothing to marshal.
        // Announced in the banner and by the `version` verb, so a client knows what
        // it is talking to before it tries anything.
        //
        // 1 → 2:  image STREAMING (StreamImageBegin/Chunk/Show, SendDisplayedImage),
        //         the read-only QueryState, ShowImageOnce, a MAX_LINE_LEN raised from
        //         4 KB to 256 KB so a chunk fits, and a `help` listing that carries
        //         each command's description in a parseable form.
        //
        // The line limit is why this MATTERS rather than being decoration: a v1
        // instance drops the connection on a line it cannot buffer, so a v2 sender
        // has to check the version and refuse cleanly instead. See RunStreamOut.
        // 2 → 3:  the AUTH challenge gained its ITERATION COUNT — "AUTH <iter>
        //         <salt> <nonce>" — because the password digest moved from a
        //         single SHA-256 to PBKDF2-HMAC-SHA256, and a client cannot
        //         derive the secret without knowing the work factor.
        //
        //         A CLEAN BREAK, with no negotiation: a v2 client sends an
        //         answer derived the old way and is refused, which is the
        //         correct outcome — the whole point of the change is that the
        //         old derivation is too cheap to keep accepting. Existing
        //         stored passwords are in the old format and must be re-entered;
        //         they cannot be upgraded in place, because upgrading needs the
        //         plaintext and the stored form deliberately does not have it.
        //
        // 3 → 4:  TLS. A listener bound to anything other than loopback now
        //         speaks TLS 1.2/1.3 from the first byte — before the banner,
        //         which is application data and belongs inside the tunnel.
        //
        //         NOT NEGOTIATED. Both ends decide from the address, so there is
        //         no offer for an attacker to strip and no plaintext fallback to
        //         force. A v3 client reaching a v4 listener fails the handshake,
        //         which is the correct outcome: its traffic would be readable.
        //
        //         The server is identified by a self-signed certificate that the
        //         client PINS by SHA-256 fingerprint (RemoteTls.h). Loopback is
        //         unchanged and still plaintext — nothing off-machine can reach
        //         it, so there is nothing there to encrypt against.
        //
        // 4 → 5:  the handshake gained a MANDATORY SECOND LINE. After the banner
        //         the server now always sends exactly one more line before it
        //         will read anything: "AUTH <iter> <salt> <nonce>" when it wants
        //         a password, and a bare "OK" when it does not. A successful
        //         AUTH is likewise answered "OK" instead of silence.
        //
        //         WHY IT IS WORTH A VERSION: under v4 "no password" was signalled
        //         by SAYING NOTHING, so a client could only detect it by waiting
        //         for a timeout — 750 ms in the desktop client, 2 s in the
        //         Android one — and guessing from the silence. That is wrong in
        //         both directions. A challenge delayed past the probe by a slow
        //         link read as "no password", and the client then entered command
        //         mode unauthenticated while the server waited for an answer that
        //         never came; every command failed and nothing said why. A server
        //         that DID want a password but whose OK was silent left the
        //         Android client blocking its full 10 s read and reporting
        //         "Read timed out" for a CORRECT password.
        //
        //         A CLEAN BREAK, no negotiation and no probing: both clients now
        //         block for that line and branch on what it says. A v4 server
        //         talking to a v5 client stalls at the handshake instead of
        //         guessing, which is the honest outcome — the two builds ship
        //         together.
        // v6 — 2026-08-06. Adds the `agent` verb: both ends exchange
        //      `k=v;k=v` describing themselves (app, ver, proto, platform, os,
        //      host, name) right after authentication, and the server answers
        //      with its own rather than merely recording the client's.
        //
        //      NOT A CLEAN BREAK, unlike v5. `agent` is optional in both
        //      directions and unknown keys inside it are ignored, so a build
        //      that never sends one is simply a peer whose details are unknown —
        //      which is what every peer was before this. The bump exists so the
        //      banner does not advertise v5 from a build that greets, because a
        //      client cannot otherwise tell the two apart.
        constexpr int PROTOCOL_VERSION = 6;

        // Hard cap on one received line. A socket must never be allowed to grow
        // a buffer without bound just by never sending a newline.
        // Longest line either end will buffer before dropping the connection. An
        // unbounded line is the one input a peer fully controls, so there has to be
        // a ceiling — but it also has to hold one STREAM_CHUNK_BYTES chunk after
        // base64 expansion (see below), which is why this is not 4 KB any more.
        // Still bounded, still per-connection, and 256 KB of accumulator is
        // nothing next to the bitmaps this program already holds.
        constexpr size_t MAX_LINE_LEN = 256 * 1024;

        // Response prefixes. A client only ever needs to look at the first token.
        constexpr const wchar_t *RESP_OK  = L"OK";
        constexpr const wchar_t *RESP_ERR = L"ERR";

        // UNSOLICITED line: an observed instance reporting something it just
        // did, pushed down the connection its observer already holds open. The
        // only line the server ever sends that is not an answer to a request.
        //
        // A third prefix rather than an OK variant, because a client sitting in
        // a request/reply exchange has to be able to tell "this is my answer"
        // from "this happened meanwhile" without tracking state. Existing
        // clients that only branch on OK/ERR simply never see one: nothing is
        // ever pushed to a connection that did not send `observe 1`.
        constexpr const wchar_t *RESP_EVENT = L"EVENT";

        // --- Mirroring (src/Rem_TCP_IP/RemoteMirror.*) ----------------------
        // Per-target send queue depth. Bounded, and the OLDEST is dropped when
        // it overflows: a backlog of keystrokes for a machine that stopped
        // answering would replay minutes of stale navigation when it came back,
        // and a mirrored keystroke only means anything at the moment it is made.
        constexpr size_t MIRROR_QUEUE_MAX = 64;

        // How long an idle sender thread waits on the socket for an unsolicited
        // EVENT before looping to re-check its queue and the stop flag. Short
        // enough that F11 responds immediately, long enough not to spin.
        constexpr int MIRROR_IDLE_POLL_MS = 200;

        // Wait before retrying a target that would not connect. A slave that is
        // switched off must not be hammered, and its thread must not spin.
        constexpr int MIRROR_RECONNECT_MS = 3000;

        // Bound on a single mirrored send, so one wedged target cannot stall its
        // own queue indefinitely. Shorter than the server's REPLY_TIMEOUT_MS
        // because on loopback anything slower is already a fault.
        constexpr int MIRROR_SEND_TIMEOUT_MS = 3000;

        // --- Ctrl+Enter: pushing this viewer's picture at the targets ---------
        // Opening a folder on the far end starts an ASYNC scan there, and its
        // reply comes back the moment the open was accepted — not when the
        // playlist exists. An index sent into that gap lands on the old list or
        // out of range, which is precisely why `sync` never carried one.
        //
        // So the push re-asks `QueryState` until the far end reports the folder
        // it was told to open with a playlist long enough to hold the index. On
        // loopback with a warm folder that is the first answer; a cold folder of
        // a few thousand files is what the budget below is for. Bounded, because
        // "wait until it settles" with no limit is a hung sender thread.
        constexpr int PUSH_SETTLE_TRIES = 40;
        constexpr int PUSH_SETTLE_MS    = 50;   // 40 × 50 ms = 2 s at worst

        // --- Streaming an image over the wire (Alt+Enter / Ctrl+Alt+Enter) -----
        //
        // The picture's own FILE BYTES travel, base64-encoded, so that showing an
        // image on another machine does not depend on it being able to read a path.
        // The far end decodes them with its own WIC, exactly as if it had opened
        // the file — sending raw pixels instead would multiply the transfer by ten
        // or more for no gain.
        //
        // Chunked because the protocol has a bounded line length and an image does
        // not fit in one line. Raw bytes per chunk; base64 makes it 4/3 as long on
        // the wire, so this must stay comfortably under MAX_LINE_LEN together with
        // the command name.
        constexpr size_t STREAM_CHUNK_BYTES = 96 * 1024;   // → ~128 KB per line

        // Refused above this. An unbounded transfer is an unbounded allocation on
        // both ends, and a one-shot advert is not a file-transfer protocol: a
        // 32 MB ceiling covers any camera JPEG or screenshot and stops a 2 GB TIFF
        // from being buffered twice on a viewer that only wanted to show it.
        constexpr size_t STREAM_MAX_BYTES = 32u * 1024u * 1024u;

        // --- Preview transfer (SendDisplayedPreview) -------------------------
        //
        // A phone preview does not need the original file. Sending one means a
        // 6 MB wallpaper becomes ~8 MB of base64 to draw something the phone
        // shows at about 1080 px, and the phone then decodes a bitmap larger
        // than its own screen. Downscaling and re-encoding first is 25-40x less
        // on the wire and far less work at the far end.
        //
        // The ORIGINAL is still available through SendDisplayedImage, which is
        // what Save uses — a preview is for looking at, not for keeping.
        constexpr int PREVIEW_MAX_DIM_DEFAULT = 1440; // longest edge, px
        constexpr int PREVIEW_MAX_DIM_MIN     = 64;
        constexpr int PREVIEW_MAX_DIM_MAX     = 4096; // past a phone screen already

        // JPEG quality, 1-100. 80 is the usual "no visible artefacts at a
        // glance" point and roughly halves the size against 95.
        constexpr int PREVIEW_QUALITY_DEFAULT = 80;
        constexpr int PREVIEW_QUALITY_MIN     = 20;
        constexpr int PREVIEW_QUALITY_MAX     = 100;

        // How much of a line the wire LOG keeps. The log is a diagnostic record,
        // not a byte-for-byte archive, and a stream chunk is ~128 KB of base64 that
        // says nothing a human can read — kept whole it would bloat the store and
        // the file it saves to. Applied inside Log::Add, so every producer is
        // covered by one rule.
        constexpr size_t LOG_LINE_MAX = 200;

        // Error codes carried after ERR. Stable numbers: scripts branch on these,
        // so they may be appended to but never renumbered.
        constexpr int ERR_UNKNOWN_COMMAND   = 1;
        constexpr int ERR_PAYLOAD_REQUIRED  = 2;
        constexpr int ERR_PAYLOAD_REJECTED  = 3;
        constexpr int ERR_LINE_TOO_LONG     = 4;
        constexpr int ERR_BAD_PAYLOAD       = 5;
        constexpr int ERR_NOT_AUTHENTICATED = 6;
        constexpr int ERR_AUTH_FAILED       = 7;
        constexpr int ERR_TOO_MANY_CLIENTS  = 8;
        constexpr int ERR_INTERNAL          = 9;
    }

    namespace Registry {
        // Switch this between HKEY_CURRENT_USER and HKEY_LOCAL_MACHINE
        inline HKEY ROOT_HIVE = HKEY_CURRENT_USER;

        // Base path for application-specific user preferences (HKEY_CURRENT_USER)
        constexpr const wchar_t *ROOT_KEY = L"Software\\QuickImageViewer";
        // Path string to the last directory accessed by the user
        constexpr const wchar_t *LAST_FOLDER = L"LastFolder";

        // --- Settings (Stored under ROOT_KEY) ---
        // --- System Integration (Open With & Startup) ---
        // Registry path to define the shell command for opening associated files
        constexpr const wchar_t *OPEN_WITH_COMMAND = L"Software\\Classes\\Applications\\QuickImageViewer.exe\\shell\\open\\command";
        // Base registry key for the application's file association definition
        constexpr const wchar_t *OPEN_WITH_ROOT = L"Software\\Classes\\Applications\\QuickImageViewer.exe";
        // Key containing a list of supported file extensions (e.g., .jpg, .png)
        constexpr const wchar_t *OPEN_WITH_TYPES = L"Software\\Classes\\Applications\\QuickImageViewer.exe\\SupportedTypes";

        // Windows Auto-start path (Standard location for user-specific startup applications)
        constexpr const wchar_t *RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
        // Value name used for the application's auto-start entry in the Run key
        constexpr const wchar_t *RUN_VALUE_NAME = L"QuickImageViewer";

        constexpr const wchar_t *SUPPORTED_EXTENSIONS[] = {
            L".jpg", L".jpeg", L".jpe", L".png", L".apng", L".webp", L".bmp", L".gif", L".tiff", L".tif",
            L".ico", L".cur", L".heic", L".heif", L".hif", L".heics", L".heifs",
            L".jxr", L".wdp", L".hdp", L".dds", L".jxl",
            L".avif", L".avcs", L".avci", L".avifs",
            L".dng", L".cr2", L".cr3", L".nef", L".arw",
            L".svg", L".tga", L".qoi",
            L".jp2", L".j2k", L".j2c", L".jpf", L".jpx",
            L".hdr", L".exr",
            L".ppm", L".pgm", L".pbm"
        };

        // Helper to get the number of elements
        constexpr size_t SUPPORTED_EXTENSIONS_COUNT = std::size(SUPPORTED_EXTENSIONS);
        constexpr const wchar_t *THEME_FACTOR = L"qivThemeFactor";
        constexpr const wchar_t *KEEP_IN_BACKGROUND = L"qivKeepInBackground";
        constexpr const wchar_t *RUN_ON_STARTUP = L"qivRunOnStartup";
        constexpr const wchar_t *THUMBNAIL_EFFECTS = L"qivThumbnailEffects";
        constexpr const wchar_t *HISTORY_FULL_MODE = L"qivHistoryFullMode";
        constexpr const wchar_t *OVERLAY_VISIBLE = L"qivOverlayVisible";
        constexpr const wchar_t *OVERLAY_SHOW_BG = L"qivOverlayShowBg";
        constexpr const wchar_t *OVERLAY_LAYOUT_MODE  = L"qivOverlayLayoutMode";
        constexpr const wchar_t *OVERLAY_SLOT_VISIBLE = L"qivOverlaySlotVisible"; // bitmask, bit N = slot N
        constexpr const wchar_t *OVERLAY_SLOT_COMPACT = L"qivOverlaySlotCompact"; // bitmask, bit N = slot N
        constexpr const wchar_t *OVERLAY_SHOW_DIR_NAME = L"qivOverlayShowDirName";
        constexpr const wchar_t *OVERLAY_FONT_SIZE     = L"qivOverlayFontSize";
        constexpr const wchar_t *OVERLAY_FONT_COLOR    = L"qivOverlayFontColor";
        constexpr const wchar_t *OVERLAY_FONT_FAMILY   = L"qivOverlayFontFamily"; // index into OVERLAY_FONT_FAMILIES
        constexpr const wchar_t *OPEN_DIRWND_ON_START = L"qivOpenDirWndOnStart";
        constexpr const wchar_t *LOCK_VIEWPORT = L"qivLockViewport";
        constexpr const wchar_t *SWAP_MOUSE_BUTTONS = L"qivSwapMouseButtons";
        constexpr const wchar_t *WHEEL_INVERT   = L"qivWheelInvert";
        constexpr const wchar_t *WHEEL_INVERT_H = L"qivWheelInvertH";
        constexpr const wchar_t *VRAM_CACHE_COUNT = L"qivVramCacheCount";
        constexpr const wchar_t *VIEW_MODE        = L"qivViewMode";
        constexpr const wchar_t *BASE_WIDTH_KEY   = L"qivBaseWidth";
        constexpr const wchar_t *BASE_HEIGHT_KEY  = L"qivBaseHeight";
        constexpr const wchar_t *START_FULLSCREEN      = L"qivStartFullscreen";
        constexpr const wchar_t *HISTORY_MAX_DIRS      = L"qivHistoryMaxDirs";
        constexpr const wchar_t *HISTORY_MAX_FAVS      = L"qivHistoryMaxFavs";
        constexpr const wchar_t *DIR_THUMB_CACHE_MB    = L"qivDirThumbCacheMB";
        constexpr const wchar_t *PRELOAD_LOOKASIDE      = L"qivPreloadLookaside";
        constexpr const wchar_t *MSG_CENTER_MS          = L"qivMsgCenterMs";
        constexpr const wchar_t *HISTORY_MAX_DIRS_SAVE  = L"qivHistoryMaxDirsSave";
        constexpr const wchar_t *SLIDESHOW_INTERVAL_MS  = L"qivSlideshowIntervalMs";
        constexpr const wchar_t *SLIDESHOW_LOOP          = L"qivSlideshowLoop";
        constexpr const wchar_t *SLIDESHOW_SHUFFLE       = L"qivSlideshowShuffle";
        constexpr const wchar_t *SLIDESHOW_TRANSITION    = L"qivSlideshowTransition";
        constexpr const wchar_t *SLIDESHOW_TRANS_SOURCE  = L"qivSlideshowTransSource";
        constexpr const wchar_t *SLIDESHOW_TRANS_ORDER   = L"qivSlideshowTransOrder";
        constexpr const wchar_t *SLIDESHOW_TRANS_LIST    = L"qivSlideshowTransList"; // bitmask
        constexpr const wchar_t *SORT_ORDER              = L"qivSortOrder";
        constexpr const wchar_t *SORT_REVERSE            = L"qivSortReverse";
        constexpr const wchar_t *CTRL_C_ENABLED          = L"qivCtrlCEnabled";
        constexpr const wchar_t *INPUTBOX_CARET_STYLE    = L"qivCaretStyle";
        constexpr const wchar_t *ZOOM_CLICK_MULT         = L"qivZoomClick";
        constexpr const wchar_t *CONTEXT_MENU_ENABLED    = L"qivContextMenu";
        constexpr const wchar_t *KIOSK_LOCK               = L"qivKioskLock";
        constexpr const wchar_t *ALWAYS_ON_TOP            = L"qivAlwaysOnTop";
        constexpr const wchar_t *KEEP_DISPLAY_AWAKE       = L"qivKeepDisplayAwake";
        constexpr const wchar_t *REMOTE_BEACON            = L"qivRemoteBeacon";
        // NOTE: the last-image-on-exit value used to live here. It is SESSION
        // state, not a setting — it changes on every close and is meaningless on
        // another machine — and it now has its own file, Constants::Session.
        // Keeping it here meant rewriting the entire settings store for one line
        // at every exit.
        constexpr const wchar_t *THUMB_COPY_ENABLED      = L"qivThumbCopy";
        constexpr const wchar_t *THUMB_MOVE_ENABLED      = L"qivThumbMove";
        constexpr const wchar_t *THUMB_DELETE_ENABLED    = L"qivThumbDelete";
        constexpr const wchar_t *THUMB_PASTE_ENABLED     = L"qivThumbPaste";
    }

    // =========================================================================
    // SESSION STATE — what this copy was doing, not how it is configured.
    //
    // Its own file because of how often it is written and how little it means:
    // the resume position changes at every exit, and an .ini write rewrites the
    // whole file. Held in the settings store, one line of session state made
    // every close rewrite every setting the application has — pointless write
    // traffic on an SSD, and a needless opportunity to corrupt the settings.
    //
    // Nothing here is worth preserving. Delete the file and the next launch
    // simply opens from history instead.
    // =========================================================================
    namespace Session {
        constexpr const wchar_t *FILE_NAME = L"qivSession.ini";
        constexpr const wchar_t *FILE_HEADER =
            L"Session state (last image viewed). Safe to delete.";
        constexpr const wchar_t *SECTION    = L"Session";

        // Full path of the image on screen at the last exit, reopened on the
        // next launch so the app resumes where it was left instead of prompting.
        constexpr const wchar_t *KEY_LAST_IMAGE = L"LastImage";
    }

    namespace SettingsFile {
        constexpr const wchar_t *EXPORT_PREFIX = L"QIVSettings_";
        constexpr const wchar_t *EXPORT_EXTENSION = L".ini";
        constexpr const wchar_t *EXPORT_FILTER = L"INI Settings (*.ini)\0*.ini\0All Files (*.*)\0*.*\0";
    }

    namespace Backup {
        constexpr const wchar_t *BACKUP_PREFIX = L"qIVBackup_";
    }

    namespace ViewModes {
        enum class ViewMode {
            FitToView_PreserveAspectRatio = 1,
            FitToWidth_DoNotPreserveAspectRatio = 2,
            FitToHeight_DoNotPreserveAspectRatio = 3,
            FitToWindow_DoNotPreserveAspectRatio = 4,
            OriginalImageSize_PreserveAspectRatio = 5
        };

        constexpr ViewMode defaultViewMode = ViewMode::FitToView_PreserveAspectRatio;
    }

    namespace Overlay {
        constexpr bool DEFAULT_SHOW_OVERLAY = true;

        // =====================================================================
        // PER-SLOT STARTING STATE  —  one constant per slot, three values each
        // =====================================================================
        //
        //   0 = OFF      the slot is not drawn
        //   1 = COMPACT  one line
        //   2 = FULL     two lines / the long form
        //
        // These are FIRST-RUN DEFAULTS. Once a slot has been cycled with
        // Ctrl+1..9 the choice is persisted (qivOverlaySlotVisible /
        // qivOverlaySlotCompact) and read from there instead — changing a
        // constant here will not move a slot that has already been set. Clear
        // those two registry values, or Restore Defaults, to pick them up.
        //
        // INDEPENDENT OF THE MASTER TOGGLE (I / Ctrl+0, app.showOverlayInfoText).
        // That switch hides every slot at once and restores them to whatever
        // these produced; it does not change them.
        //
        // Grid positions:
        //   [1] TOP_LEFT    [2] TOP_CENTER    [3] TOP_RIGHT
        //   [4] MID_LEFT    [5] MID_CENTER    [6] MID_RIGHT
        //   [7] BOT_LEFT    [8] BOT_CENTER    [9] BOT_RIGHT
        constexpr int SLOT_OFF     = 0;
        constexpr int SLOT_COMPACT = 1;
        constexpr int SLOT_FULL    = 2;

        constexpr int SLOT_STATE_TOP_LEFT   = SLOT_COMPACT; // index/total + filename
        constexpr int SLOT_STATE_TOP_CENTER = SLOT_COMPACT; // top panel selection
        constexpr int SLOT_STATE_TOP_RIGHT  = SLOT_FULL; // server dot + zoom %
        constexpr int SLOT_STATE_MID_LEFT   = SLOT_COMPACT; // left panel selection
        // MID_CENTER is the centre message queue. It has NO compact form —
        // COMPACT and FULL both mean "on", and only OFF differs.
        constexpr int SLOT_STATE_MID_CENTER = SLOT_COMPACT;
        constexpr int SLOT_STATE_MID_RIGHT  = SLOT_COMPACT; // right panel selection
        constexpr int SLOT_STATE_BOT_LEFT   = SLOT_COMPACT; // effects + folder name
        constexpr int SLOT_STATE_BOT_CENTER = SLOT_COMPACT; // bottom panel selection
        constexpr int SLOT_STATE_BOT_RIGHT  = SLOT_COMPACT; // dimensions / file size

        // Kept because TextOverlay's own member initialiser uses it as the
        // starting value for a slot nobody has wired yet. The per-slot
        // constants above are what actually decide the startup state.
        constexpr const bool IS_COMPACT_OVERLAY_MODE = true; // true → 1-line, false → 2-line
        // P key — toggle semi-transparent background behind all overlay text.
        // Text is always drawn; only the background rect is suppressed when false.
        constexpr bool IS_OVERLAY_SHOW_BACKGROUND = true;
        // Layout mode cycled with O key — live value is app.overlayLayoutMode:
        //   0 — default 3×3 grid
        //   1 — the four corners stacked vertically on top-left
        //   2 — the four corners collapsed into a 2-line summary top-left:
        //         line 1: index / total + filename
        //         line 2: zoom% + WxH / size
        // Modes 1 and 2 re-arrange only the corners; the five non-corner slots
        // keep their grid positions in every mode.
        constexpr int DEFAULT_LAYOUT_MODE = 0;
        constexpr int LAYOUT_MODE_COUNT   = 3;

        // BOT_LEFT carries two independent readouts, in this order:
        //   the active-effects list, then the current folder name LAST.
        // The slot is bottom-anchored and grows upward, so the folder name
        // stays pinned to the bottom while effects stack above it. The two
        // toggles are independent — hiding one never hides the other.
        constexpr bool SHOW_DIR_NAME     = false; // persisted (qivOverlayShowDirName)
        constexpr bool SHOW_EFFECTS_LIST = true;  // session-only, not persisted

        // Per-slot visibility / compact state is persisted as one bit per slot,
        // indexed by OverlayManager::Slot. Nine slots → the low 9 bits.
        constexpr unsigned SLOT_MASK_ALL = 0x1FFu;

        // The nine SLOT_STATE_* constants folded into the two bitmasks the rest
        // of the program uses. Two masks rather than nine values because that is
        // the persisted shape (one DWORD each) and the runtime shape the slots
        // are wired from — this is the one place the two representations meet.
        //
        // The ORDER of these two lists is the Slot enum's order, and it must
        // stay that way: bit N is slot N. A static_assert cannot check that, so
        // it is written once, here, rather than repeated anywhere else.
        constexpr unsigned SlotBit(int state, int index) {
            return state == SLOT_OFF ? 0u : (1u << index);
        }
        constexpr unsigned SlotCompactBit(int state, int index) {
            return state == SLOT_COMPACT ? (1u << index) : 0u;
        }

        constexpr unsigned DEFAULT_SLOT_VISIBLE_MASK =
                SlotBit(SLOT_STATE_TOP_LEFT,   0) | SlotBit(SLOT_STATE_TOP_CENTER, 1) |
                SlotBit(SLOT_STATE_TOP_RIGHT,  2) | SlotBit(SLOT_STATE_MID_LEFT,   3) |
                SlotBit(SLOT_STATE_MID_CENTER, 4) | SlotBit(SLOT_STATE_MID_RIGHT,  5) |
                SlotBit(SLOT_STATE_BOT_LEFT,   6) | SlotBit(SLOT_STATE_BOT_CENTER, 7) |
                SlotBit(SLOT_STATE_BOT_RIGHT,  8);

        constexpr unsigned DEFAULT_SLOT_COMPACT_MASK =
                SlotCompactBit(SLOT_STATE_TOP_LEFT,   0) | SlotCompactBit(SLOT_STATE_TOP_CENTER, 1) |
                SlotCompactBit(SLOT_STATE_TOP_RIGHT,  2) | SlotCompactBit(SLOT_STATE_MID_LEFT,   3) |
                SlotCompactBit(SLOT_STATE_MID_CENTER, 4) | SlotCompactBit(SLOT_STATE_MID_RIGHT,  5) |
                SlotCompactBit(SLOT_STATE_BOT_LEFT,   6) | SlotCompactBit(SLOT_STATE_BOT_CENTER, 7) |
                SlotCompactBit(SLOT_STATE_BOT_RIGHT,  8);

        // =========================================================================
        // Overlay — Center-Center message queue (MID_CENTER slot)
        // =========================================================================
        // How long the center-center notification stays visible before auto-hiding (ms)

        constexpr UINT IS_MSG_CENTER_DISPLAY_MS = 1000;
        // Center-center text color  (R, G, B, A)
        constexpr float MSG_CENTER_COLOR_R = 1.0f;
        constexpr float MSG_CENTER_COLOR_G = 0.85f;
        constexpr float MSG_CENTER_COLOR_B = 0.20f;
        constexpr float MSG_CENTER_COLOR_A = 1.0f;
        constexpr float MSG_BASE_FONT_SIZE = 10.0f;
        // Center-center font size (pt). Other slots use the RendererD2D default size.
        constexpr float MSG_ALL_BUT_CENTER_FONT_SIZE = MSG_BASE_FONT_SIZE * 1.4f;
        constexpr float MSG_CENTER_FONT_SIZE = MSG_BASE_FONT_SIZE * 1.6f;
        constexpr const wchar_t *MSG_ALL_BUT_CENTER_FONT_FAMILY_DEFAULT = L"Segoe UI";

        // =========================================================================
        // Overlay — user-chosen text style for the eight OUTER slots
        // =========================================================================
        // MID_CENTER is deliberately excluded: it is a transient notification with
        // its own colour and size above, and it has to stay legible no matter what
        // the outer slots are set to.
        constexpr int OVERLAY_FONT_SIZE_MIN     = 1;
        constexpr int OVERLAY_FONT_SIZE_MAX     = 92;
        constexpr int OVERLAY_FONT_SIZE_DEFAULT = static_cast<int>(MSG_ALL_BUT_CENTER_FONT_SIZE);
        // LightGreen — the colour RendererD2D has always created its text brush
        // with, kept as the default so nothing changes until the user picks.
        constexpr COLORREF OVERLAY_FONT_COLOR_DEFAULT = RGB(144, 238, 144);
        // Offered in the Overlays ▸ Font submenu. Persisted as an INDEX into this
        // array, so only ever append — inserting or reordering would silently
        // repoint every saved setting.
        constexpr const wchar_t *OVERLAY_FONT_FAMILIES[] = {
            L"Segoe UI", L"Arial", L"Verdana", L"Tahoma", L"Calibri",
            L"Georgia", L"Times New Roman", L"Courier New", L"Consolas",
            L"Comic Sans MS", L"Impact", L"Trebuchet MS"
        };
        constexpr int OVERLAY_FONT_FAMILY_COUNT   = static_cast<int>(std::size(OVERLAY_FONT_FAMILIES));
        constexpr int OVERLAY_FONT_FAMILY_DEFAULT = 0; // Segoe UI — matches the constant above
        constexpr const wchar_t *MSG_CENTER__FONT_FAMILY_DEFAULT = L"Segoe UI";
        constexpr const wchar_t *MSG_ALL_FONT_FAMILY_FALLBACK = L"Arial";
        constexpr const wchar_t *MSG_ALL_FONT_LOCALE = L"en-us";

        // =========================================================================
        // Overlay — per-slot notification panel width / height
        // =========================================================================
        // Width of the center-center message box (pixels)

        constexpr float MSG_CENTER_WIDTH = 420.0f;
        // Height of a single-line center-center message box
        constexpr float MSG_CENTER_HEIGHT = 36.0f;
        // Custom window messages
        constexpr UINT WM_QIV_CENTER_MSG_HIDE = WM_USER + 10; // Posted by WM_TIMER to hide center msg
    }

    namespace History {
        // =========================================================================
        // Folder History (HistoryWindow)
        // =========================================================================
        constexpr bool HISTORY_SHOW_FULL_HISTORY = false; // controls initial behaviour of HistoryWnd full or limited , you can swith with key comb that after you show but this is for inital behaviour
        constexpr const wchar_t *HISTORY_FILE_NAME = L"qivHistory.txt";
        constexpr const wchar_t *FAVORITES_FILE_NAME = L"qivFavorites.txt";

        // Comment markers. A line whose first non-space character is one of
        // these is skipped by the loaders — which is what lets these files carry
        // a header without it appearing as a folder row in the panel.
        constexpr wchar_t COMMENT_MARK_SEMI = L';';
        constexpr wchar_t COMMENT_MARK_HASH = L'#';

        // Written once, when the file is first created. Deliberately NO
        // "Updated" line: these files are APPENDED to on every folder visit, and
        // refreshing a header would mean rewriting the whole file each time —
        // the write amplification that keeping them as plain text avoids. The
        // file's own modified date is the accurate last-write, and the header
        // says so rather than carrying a stamp that would quietly go stale.
        constexpr const wchar_t *HISTORY_FILE_TITLE =
            L"folder history, oldest first (the panel shows it newest first)";
        constexpr const wchar_t *FAVORITES_FILE_TITLE =
            L"favourite folders, one per line";
        //backup name must be the same as the file name only append the currentDate ex: qivHistory_DATE.bak
        //We Backup when we delete history/favorites only then we first backup then delete !
        constexpr const wchar_t *HISTORY_FAVORITES_BACKUP_FOLDER = L"/QivBackup";
        constexpr const wchar_t *HISTORY_FAVORITES_BACKUP_VERSION = L"Backup Version Schema : 1.0";
        //when backing up history or favorites first line must be the date time and the QuickImageViewer backupVersion ,and COMPUTER_NAME ex:BACKUP COMPUTER_NAME, dd.MM.YYYY, HH:MM:SS.ms, HISTORY_FAVORITES_BACKUP_VERSION
        constexpr const wchar_t *HISTORY_FAVORITES_BACKUP_EXTENSION = L".bak";
        //theese are kept im mot recently used order in ram , when addin a new one to file just append to end with no duplicates
        constexpr int IS_HISTORY_MAX_DIRS_TO_SHOW = 10; // how many folders to show in historyWnd
        constexpr int IS_HISTORY_MAX_DIRS_TO_SAVE = 1000; // how many folders to remember/sava in file , just append to end new ones until max is reached excluding duplicates
        constexpr char HISTORY_FAVORITES_MARK = '*'; // mark for favorites appened before the file name
        constexpr int IS_HISTORY_MAX_FAVORITES_TO_SHOW = 10; // how many favorites folders to show in HistoryWnd
        constexpr int HISTORY_FAVORITES_POSITION = 0; // 0 on top , 1 on bottom , 2 don't change position(not pinned)

        constexpr int HISTORY_ROW_HEIGHT = 28; // px at 96 DPI per history row
        constexpr int HISTORY_PADDING = 16; // px at 96 DPI inner padding
        constexpr int HISTORY_FONT_SIZE = 14; // pt at 96 DPI — header / hint lines
        constexpr int HISTORY_LIST_FONT_SIZE = 16; // pt at 96 DPI — list item text (tune independently)
        constexpr int HISTORY_FILTER_ROW_H = 24; // px at 96 DPI — filter input row below the footer
        // Scrollbar geometry and colours are Constants::Scrollbar and
        // Constants::Theme::Scrollbar — one set for every scrolled surface.
        // This panel's own 6px width and 16px thumb are gone with them.

        // How long the startup folder sweep waits before touching the disk.
        // It runs at background I/O priority anyway, but holding off entirely
        // until the first image is decoded and on screen keeps app launch as fast
        // as it was before the sweep existed. Only applies to the startup kick-off
        // — opening the panel or pressing F5 scans immediately.
        constexpr DWORD HISTORY_SCAN_STARTUP_DELAY_MS = 3000;

        // Window size limits (px at 96 DPI — DPI-scaled at runtime)
        constexpr int HISTORY_MIN_W = 690; // minimum panel width
        constexpr int HISTORY_MAX_W = IS_BASE_WIDTH  - 120; // maximum panel width
        constexpr int HISTORY_MIN_H = 620; // minimum panel height
        constexpr int HISTORY_MAX_H = IS_BASE_HEIGHT - 60; // maximum panel height (also capped to 80% of monitor)
    }

    // =========================================================================
    // Zoom Panel (F2 zoom-to dialog) — geometry constants
    // =========================================================================
    namespace ZoomPanel {
        constexpr float WINDOW_WIDTH = 340.0f;
        constexpr float WINDOW_HEIGHT = 140.0f;
        constexpr float PADDING = 14.0f;
        constexpr float GAP = 8.0f;
        constexpr float FONT_SIZE = 13.0f;
        constexpr float FONT_SIZE_INPUT = 16.0f;
        constexpr float INPUT_BOX_HEIGHT = 34.0f;
        constexpr int LABEL_EXTRA_HEIGHT = 6;     // extra px below label text
        constexpr int HINT_EXTRA_HEIGHT = 4;      // extra px below hint text
        // Base gray values passed to Constants::Theme::ThemedGray at draw time
        constexpr float LABEL_TEXT_GRAY = 0.9020f; // label: "Enter zoom multiplier..."
        constexpr float HINT_TEXT_GRAY = 0.50f;    // hint: "Enter = apply zoom..."
        // Zoom limits, in PERCENT — exactly the number the overlay displays and
        // the zoom panel accepts. 0.1 means 0.1%, 99999 means 99999%.
        //
        // They bound the EFFECTIVE on-screen zoom, NOT the raw app.viewport.zoom
        // multiplier. Effective zoom is (baseScale * viewport.zoom) and baseScale
        // depends on the view mode, so clamping the multiplier alone lets the real
        // zoom drift far past these bounds. Always clamp through
        // ClampZoomToLimits() in AppState.h, never with a bare std::clamp on
        // viewport.zoom.
        //
        // Unit changes go through Converters::PercentToRatio / RatioToPercent.
        constexpr float ZOOM_STEP = 1.1f;     // ratio factor per +/- key or wheel tick
        constexpr float ZOOM_MIN  = 0.01f;     // percent — smallest allowed zoom
        constexpr float ZOOM_MAX  = 99'999.0f; // percent — largest allowed zoom single quote is number separator " ' " like in java "_"
        constexpr int   INPUT_MAX_CHARS = 10; // must be < ZoomWnd::m_input capacity
    }

    // Outcome of ClampZoomToLimits() (AppState.h). Lives here rather than in
    // AppState.h so OverlayManager.h can consume it without pulling in AppState.
    // Callers use it to tell the user WHY a zoom keypress did nothing.
    enum class ZoomClampResult {
        None,       // zoom was already inside the limits — nothing was capped
        ClampedMin, // hit ZOOM_MIN, cannot zoom out further
        ClampedMax  // hit ZOOM_MAX, cannot zoom in further
    };

    // =========================================================================
    // InputBox — shared single-line text control (Find / JumpTo / History / Help)
    // =========================================================================
    namespace InputBox {
        // Text-caret geometry (px at 96 DPI, DPI-scaled at draw time).
        //   CARET_STYLE 0 = vertical bar (|) sized to the text height.
        //   CARET_STYLE 1 = underscore (_) along the text baseline.
        constexpr int   CARET_STYLE          = 1;    // 0 = vertical bar, 1 = underscore
        constexpr int   CARET_PADDING_HEIGHT = 0;    // bar: top/bottom inset from text height
                                                     // underscore: lift off the baseline
        constexpr float CARET_THICKNESS      = 1.0f; // bar: width / underscore: height (float:
                                                     // e.g. 1.5 reads as in-between on high-DPI)
        constexpr int   CARET_GAP            = 0;    // horizontal space between caret and text
    }

    namespace Cursors {
          inline const HCURSOR CURR_ZOOM =  LoadCursorW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDC_ZOOM_CURSOR_L));
          inline const HCURSOR CURR_GRAB =  LoadCursorW(GetModuleHandleW(nullptr),  MAKEINTRESOURCEW(IDC_GRAB_CURSOR_L));
          inline const HCURSOR CURR_DEFAULT =  LoadCursorW(nullptr, IDC_ARROW);
          inline const HCURSOR CURR_CLICK =  LoadCursorW(nullptr, IDC_HAND);
          inline const HCURSOR CURR_APPSTARTING =  LoadCursorW(nullptr, IDC_APPSTARTING);
    }

    namespace FileHandler {
        constexpr const int FILE_HANDLER_DEFAULT_SORT_ORDER = 0; // 0 name, 1 date, 2 size,3 extension(type), 4 performance mode - SortPlaylistByDiskOrder
        constexpr const bool FILE_HANDLER_SORT_TYPE_IS_REVERSE = false;

        // Adaptive directory-scan reserve hint (DirScanReserveHint). The hint is
        // the previous scan's image count, clamped to this range, used to pre-size
        // the playlist + size/time maps in one allocation.
        //   FLOOR — cover small folders without realloc churn.
        //   CAP   — bound the worst-case over-reserve after a one-off huge folder.
        //           Folders larger than CAP still load fine; the vector just grows
        //           past the reserve (a few extra reallocs). Raise CAP to match the
        //           largest folder you regularly open.
        constexpr size_t DIR_SCAN_RESERVE_FLOOR = 256;
        constexpr size_t DIR_SCAN_RESERVE_CAP   = 16384;
    }

    // =========================================================================
    // Slideshow  (Ctrl+F1 — play / pause / stop cycle)
    // Init-only constants; runtime state lives in AppState::SlideshowState.
    // =========================================================================
    namespace Slideshow {
        // WM_TIMER wParam IDs (1001/1002 are taken by lookaside/center-msg)
        constexpr UINT_PTR TIMER_ID = 1003; // slide-advance tick
        constexpr UINT_PTR CURSOR_TIMER_ID = 1004; // cursor-hide inactivity tick
        constexpr UINT_PTR TRANSITION_TIMER_ID = 1005; // transition animation tick
        constexpr UINT_PTR GIF_TIMER_ID = 1006; // animated GIF frame-advance tick


        constexpr int IS_INTERVAL_MS = 5000; // ms between auto-advances

        // The accepted range for that interval.
        //
        // These bounds already existed — written out as bare 100 and 60000 in
        // four separate places: the keyboard prompt, the numeric settings entry,
        // the remote SlideshowSetInterval handler and the mirroring Sync
        // payload. Four copies of a rule is four chances for one of them to
        // drift, and the remote handler's error message quotes the range to the
        // client, so a drift there is a protocol answer that lies.
        constexpr int INTERVAL_MIN_MS = 100;   // below this the advance outruns the decode
        constexpr int INTERVAL_MAX_MS = 60000; // a minute per slide; longer is not a slideshow
        constexpr bool IS_LOOP = true; // wrap to first image at end
        constexpr bool IS_SHUFFLE = false; // random order
        constexpr int CURSOR_HIDE_MS = 3000; // ms of inactivity before hiding cursor (0 = never)
        constexpr int TRANSITION_TICK_MS = 16; // animation tick interval ~60 fps
        constexpr int TRANSITION_DURATION_MS = 800; // default transition length ms

        // Number of TransitionType members — keep in sync with the enum in
        // SlideshowTransitions.h (a static_assert-free contract used for menu
        // building, cycling and registry clamping).
        constexpr int TRANSITION_COUNT = 21;

        // Default TRANS_LIST bitmask: every animated type ticked, bit 0 (Cut)
        // cleared — a "transition list" containing Cut would mean "no transition".
        // Used as the registry default AND by Restore Defaults; keep it here so
        // the two cannot drift apart.
        constexpr uint32_t TRANSITION_LIST_DEFAULT_MASK = 0xFFFFFFFEu;

        // Transition selection is two independent axes:
        //   SOURCE — which transitions are in play
        //   ORDER  — how the next one is drawn from that pool (ignored for NONE)
        // Both index the matching *_NAMES arrays in ConstantsStrings.h.
        namespace TransitionSource {
            constexpr int NONE  = 0; // always the single transition the user picked
            constexpr int ALL   = 1; // every entry in the menu
            constexpr int LIST  = 2; // only the entries ticked in the menu
            constexpr int COUNT = 3;
        }
        namespace TransitionOrder {
            constexpr int SEQUENTIAL = 0; // menu order, one per slide, then repeat
            constexpr int RANDOM     = 1; // random pick from the pool every slide
            constexpr int COUNT      = 2;
        }

        // Dissolve — alpha ramp is quantised into this many discrete steps so it
        // reads as a grainy staircase instead of a smooth Fade.
        constexpr int DISSOLVE_STEPS = 14;

        // Ripple — damped zoom oscillation around 1.0.
        constexpr float RIPPLE_AMPLITUDE = 0.12f; // peak zoom deviation at progress 0
        constexpr float RIPPLE_WAVES     = 3.0f;  // full oscillations across the transition

        // ZoomIn — starting zoom that grows to 1.0 (ZoomOut is the 2× → 1× inverse).
        constexpr float ZOOM_IN_START = 0.4f;

        // ── Tunables for the extended transition set ─────────────────────────
        constexpr float SOFT_ZOOM_START  = 1.5f; // SoftZoom starting zoom
        constexpr int   SPIN_DEGREES     = 360;  // Spin full turn
        constexpr int   SPIN_ZOOM_DEGREES = 180; // SpinZoom half turn
        constexpr float SPIN_ZOOM_START  = 0.3f; // SpinZoom starting zoom
        constexpr float DRIFT_FRACTION   = 0.12f;// Drift* travel as a fraction of the window
        constexpr float FLICKER_CYCLES   = 6.0f; // Flicker strobes across the transition
        constexpr float BOUNCE_START     = 0.6f; // Bounce starting zoom
        constexpr float BOUNCE_OVERSHOOT = 0.18f;// Bounce peak above 1.0
        constexpr int   SWING_DEGREES    = 14;   // Swing peak tilt
        constexpr float SWING_WAVES      = 2.5f; // Swing oscillations
        constexpr float SLAM_START       = 3.0f; // Slam starting zoom
        constexpr float IRIS_START       = 0.02f;// Iris starting zoom (near a point)
    }

    // =========================================================================
    // Save dialog formats  (Ctrl+S)
    // Add a row here to expose a new format in the save dialog.
    // SaveCurrentImageWithEffects() detects format from the chosen file extension.
    // =========================================================================
    namespace Save {
        struct Format {
            const wchar_t *description; // label shown in the dialog filter list
            const wchar_t *pattern; // file mask(s), e.g. L"*.jpg;*.jpeg"
            const wchar_t *ext; // extension auto-appended when user omits it
        };

        constexpr Format FORMATS[] = {
            {L"PNG Image", L"*.png", L"png"},
            {L"JPEG Image", L"*.jpg;*.jpeg", L"jpg"},
            {L"BMP Image", L"*.bmp", L"bmp"},
            {L"TIFF Image", L"*.tif;*.tiff", L"tif"},
            {L"GIF Image", L"*.gif", L"gif"},
        };
        constexpr wchar_t DEFAULT_EXT[] = L"png"; // used when no filter is selected
    }
}
