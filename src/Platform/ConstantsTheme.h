// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#pragma once
#include <windows.h>
#include <dwmapi.h>
#include <d2d1.h>

namespace Constants {
    // =========================================================================
    // APP THEME & WINDOW CHROME  —  single source of truth
    // =========================================================================
    // true = dark title bar / tray menu; false = light (system default)
    constexpr bool IS_APP_DARK_THEME = true;

    // DWM corner preference for the main window.
    // Re-exported here; callers only need ConstantsTheme.h.
    constexpr DWORD DWMWA_WINDOW_CORNER_PREFERENCES = DWMWA_WINDOW_CORNER_PREFERENCE;

    // Corner style applied at window creation (and restored after fullscreen exit).
    // 0 DWMWCP_DEFAULT    — let Windows decide
    // 1 DWMWCP_DONOTROUND — square corners
    // 2 DWMWCP_ROUND      — standard rounded corners
    // 3 DWMWCP_ROUNDSMALL — slightly rounded corners
    constexpr DWORD APP_CORNER_PREFERENCES = DWMWCP_ROUND;

    // DWM attribute ID for dark title bar (DWMWA_USE_IMMERSIVE_DARK_MODE).
    // Named constant may be absent in older SDKs; value 20 is stable since Win10 19H1.
    constexpr DWORD DWMWA_DARK_MODE = 20;

    // DWM attribute ID for custom caption/title-bar background color (DWMWA_CAPTION_COLOR).
    // Available on Windows 11+. Ignored silently on older versions.
    constexpr DWORD DWMWA_CAPTION_COLOR_ATTR = 35;

    // DWM attribute ID for system backdrop type (Mica, Acrylic, Tabbed).
    // Values: 0=None, 1=MainWindow(Mica), 2=TransientWindow(Acrylic), 3=TabbedWindow(MicaAlt)
    // Available on Windows 11 22H2+. Ignored silently on older versions.
    constexpr DWORD DWMWA_SYSTEMBACKDROP_TYPE_ATTR = 38;

    // Initial backdrop type — 0 = None (no Mica/Acrylic effect).
    constexpr DWORD APP_BACKDROP_TYPE_DEFAULT = 0;

    namespace Theme {
        // =====================================================================
        // MASTER THEME CONTROL
        // =====================================================================
        // 0.0f = Dark Theme (original, exact), 1.0f = Inverted Light Theme, 0.5f = Mix
        // This is the INIT VALUE only — runtime state lives in app.themeFactor.
        constexpr float DEFAULT_THEME_FACTOR = 0.0f;

        // Legal range for app.themeFactor. Enforced at BOTH ends of persistence:
        // the runtime setter (AppCommands::changeAppThemeFactor) and the registry
        // load, so a hand-edited or corrupt stored value cannot reach the renderer.
        constexpr float THEME_FACTOR_MIN = 0.0f;
        constexpr float THEME_FACTOR_MAX = 1.0f;

        // The factor is a 0..1 ratio but is STORED as a whole percent (0..100),
        // because the registry only holds DWORDs. Every conversion must go
        // through this scale — casting the ratio straight to DWORD truncates it
        // to 0 for every value below 1.0.
        constexpr float THEME_FACTOR_STORE_SCALE = 100.0f;

        // Step size for runtime THEME_FACTOR adjustment (Ctrl+Alt+Shift+Numpad+/-).
        constexpr float THEME_FACTOR_STEP = 0.05f; // smallest step must be 0.01 dont go smaller than this because i seve in registry:static_cast<DWORD>(std::round(app.themeFactor * 100.0f)));

        // Formula applied inline to every channel:
        //   Final = Base + THEME_FACTOR * (1.0f - 2.0f * Base)
        // At 0.0f the math cancels out and every color is EXACTLY its base value.
        //
        // Colored (non-gray) elements store all three channels (_R/_G/_B) so the
        // original hue is preserved; each channel inverts independently.
        // Alpha values are never themed — they stay constant.

        // =====================================================================
        // BACKGROUND COLORS
        // =====================================================================
        namespace Background {
            // Main viewer background — RendererD2D m_clearColor / RendererGDI brush (0.08 gray = 20,20,20)
            constexpr float MAIN_WINDOW = 0.08f + DEFAULT_THEME_FACTOR * (1.0f - 2.0f * 0.08f);
        }

        // =====================================================================
        // CHILD PANEL COLORS (DirWnd, CacheWnd, SpawnedDirWnd, HistoryWnd bg)
        // =====================================================================
        namespace Panel {
            // Inactive panel background — 0.08 gray, alpha 0.75
            constexpr float BACKGROUND_INACTIVE = 0.08f + DEFAULT_THEME_FACTOR * (1.0f - 2.0f * 0.08f); // 0.08 * times 255=approx 20-> it like 20 , 20 ,20
            constexpr float BACKGROUND_INACTIVE_ALPHA = 0.75f; // not themed

            // Active (focused) panel background — 0.02 gray, alpha 0.90
            constexpr float BACKGROUND_ACTIVE = 0.02f + DEFAULT_THEME_FACTOR * (1.0f - 2.0f * 0.02f);
            constexpr float BACKGROUND_ACTIVE_ALPHA = 0.90f; // not themed

            // D2D scrollbar strip — track near-black 0.12 @ 70%, thumb mid-grey 0.65 @ 85%
            constexpr float SCROLLBAR_TRACK = 0.12f + DEFAULT_THEME_FACTOR * (1.0f - 2.0f * 0.12f);
            constexpr float SCROLLBAR_TRACK_ALPHA = 0.70f; // not themed
            constexpr float SCROLLBAR_THUMB = 0.65f + DEFAULT_THEME_FACTOR * (1.0f - 2.0f * 0.65f);
            constexpr float SCROLLBAR_THUMB_ALPHA = 0.85f; // not themed
        }

        // =====================================================================
        // HISTORY PANEL COLORS (GDI)
        // =====================================================================
        namespace HistoryPanel {
            // Scrollbar colours are Theme::Scrollbar now — one palette for every
            // scrolled surface. This panel's darker pair is gone with them.

            // Accent color used for the history file-size value in the header
            constexpr COLORREF SIZE_HIGHLIGHT = RGB(240, 50, 50);

            // Path row — three segments: drive letter, middle path, final folder name.
            // Non-favorite rows (dark mode):
            constexpr COLORREF PATH_DRIVE = RGB(100, 185, 205); // muted teal
            constexpr COLORREF PATH_DRIVE_HOVER = RGB(140, 215, 235);
            constexpr COLORREF PATH_MIDDLE = RGB(200, 200, 200); // light gray
            constexpr COLORREF PATH_FOLDER = RGB(232, 215, 170); // warm tan — clearly distinct from gray middle
            constexpr COLORREF PATH_FOLDER_HOVER = RGB(255, 248, 210);
            // Non-favorite rows (light mode — isDarkThemed == false):
            constexpr COLORREF PATH_DRIVE_LIGHT  = RGB(30, 110, 150);  // deep teal
            constexpr COLORREF PATH_MIDDLE_LIGHT = RGB(60, 60, 60);    // dark gray
            constexpr COLORREF PATH_FOLDER_LIGHT = RGB(100, 75, 20);   // dark tan/brown
            // Favorite rows:
            constexpr COLORREF PATH_DRIVE_FAV = RGB(195, 165, 70); // amber/gold
            constexpr COLORREF PATH_DRIVE_FAV_HOVER = RGB(215, 195, 105);
            constexpr COLORREF PATH_FOLDER_FAV = RGB(255, 248, 152); // bright warm yellow
            constexpr COLORREF PATH_FOLDER_FAV_HOVER = RGB(255, 255, 195);
            // Current folder row text colors — "you are here" green, 3 distinct shades:
            constexpr COLORREF PATH_DRIVE_CURRENT = RGB(80, 195, 115); // mid-green for drive letter
            constexpr COLORREF PATH_MIDDLE_CURRENT = RGB(100, 160, 120); // muted green for middle path
            constexpr COLORREF PATH_FOLDER_CURRENT = RGB(160, 230, 165); // bright pale-green for folder name
            // Missing folder row text colors — folder does not exist on disk:
            constexpr COLORREF PATH_DEAD_DRIVE = RGB(210, 70, 70); // red for drive / index / warning glyph
            constexpr COLORREF PATH_DEAD_MIDDLE = RGB(160, 60, 60); // darker red for middle path
            constexpr COLORREF PATH_DEAD_FOLDER = RGB(230, 100, 100); // lighter red for folder name
            // Empty folder row text colors — folder exists but contains no supported images:
            constexpr COLORREF PATH_EMPTY_DRIVE = RGB(200, 130, 50); // orange for drive / index / empty glyph
            constexpr COLORREF PATH_EMPTY_MIDDLE = RGB(150, 100, 40); // darker orange for middle path
            constexpr COLORREF PATH_EMPTY_FOLDER = RGB(220, 165, 85); // lighter orange for folder name

            // "Loading ..." shown while the background folder sweep runs. Amber,
            // and deliberately not a status colour: it describes the PANEL being
            // busy, not anything about a folder.
            constexpr COLORREF SCANNING_TEXT = RGB(235, 200, 120);
            // Point size added to the normal row font for that message, so it
            // reads as an overlay rather than another row.
            constexpr int SCANNING_FONT_BOOST = 6;

            // Symlink / junction rows — the path is a reparse point, so it is a
            // second name for a directory that physically lives somewhere else.
            // Violet: deliberately outside the teal/gold/green/red/orange already
            // in use, so "this row is an alias" reads at a glance and never gets
            // confused with a status. Only the drive segment and the glyph are
            // tinted — the rest of the row keeps its normal / favorite / current
            // colours, because being a link says nothing about whether the folder
            // is starred, open, or alive.
            constexpr COLORREF PATH_SYMLINK_DRIVE = RGB(175, 145, 235); // violet drive letter
            constexpr COLORREF PATH_SYMLINK_DRIVE_HOVER = RGB(205, 180, 255);
            // Hover row background for a link row (matches the tint above).
            constexpr COLORREF ROW_HOVER_SYMLINK = RGB(45, 35, 70);
        }

        // =====================================================================
        // EXIF / INFO WINDOW COLORS
        // =====================================================================
        namespace ExifWindow {
            // Label text (EXIF field names) — yellow (255,220,0)
            constexpr float LABEL_R = 1.0000f + DEFAULT_THEME_FACTOR * (1.0f - 2.0f * 1.0000f);
            constexpr float LABEL_G = 0.8627f + DEFAULT_THEME_FACTOR * (1.0f - 2.0f * 0.8627f);
            constexpr float LABEL_B = 0.0000f + DEFAULT_THEME_FACTOR * (1.0f - 2.0f * 0.0000f);

            // Value text — near-white (230,230,230)
            constexpr float VALUE = 0.9020f + DEFAULT_THEME_FACTOR * (1.0f - 2.0f * 0.9020f);

            // Section header — cyan (100,200,255)
            constexpr float SECTION_R = 0.3922f + DEFAULT_THEME_FACTOR * (1.0f - 2.0f * 0.3922f);
            constexpr float SECTION_G = 0.7843f + DEFAULT_THEME_FACTOR * (1.0f - 2.0f * 0.7843f);
            constexpr float SECTION_B = 1.0000f + DEFAULT_THEME_FACTOR * (1.0f - 2.0f * 1.0000f);

            // SECTION HEADER BACKGROUND — the stripe behind "FILE", "CAMERA" and
            // the rest. It was named SCROLLBAR_TRACK_* and the section painter
            // borrowed it; the scrollbar's own colours are Theme::Scrollbar now,
            // so the name is what it has always actually been used for.
            // (50,50,55)
            constexpr float SECTION_BG_R = 0.1961f + DEFAULT_THEME_FACTOR * (1.0f - 2.0f * 0.1961f);
            constexpr float SECTION_BG_G = 0.1961f + DEFAULT_THEME_FACTOR * (1.0f - 2.0f * 0.1961f);
            constexpr float SECTION_BG_B = 0.2157f + DEFAULT_THEME_FACTOR * (1.0f - 2.0f * 0.2157f);
        }

        // =====================================================================
        // HELP WINDOW COLORS
        // =====================================================================
        namespace HelpWindow {
            // Background (20,20,22)
            constexpr float BACKGROUND_R = 0.0784f + DEFAULT_THEME_FACTOR * (1.0f - 2.0f * 0.0784f);
            constexpr float BACKGROUND_G = 0.0784f + DEFAULT_THEME_FACTOR * (1.0f - 2.0f * 0.0784f);
            constexpr float BACKGROUND_B = 0.0863f + DEFAULT_THEME_FACTOR * (1.0f - 2.0f * 0.0863f);

            // Title — cyan (100,200,255)
            constexpr float TITLE_R = 0.3922f + DEFAULT_THEME_FACTOR * (1.0f - 2.0f * 0.3922f);
            constexpr float TITLE_G = 0.7843f + DEFAULT_THEME_FACTOR * (1.0f - 2.0f * 0.7843f);
            constexpr float TITLE_B = 1.0000f + DEFAULT_THEME_FACTOR * (1.0f - 2.0f * 1.0000f);

            // Subtitle / footer text — gray (140,140,140)
            constexpr float SUBTITLE = 0.5490f + DEFAULT_THEME_FACTOR * (1.0f - 2.0f * 0.5490f);

            // Section header — cyan (100,200,255)
            constexpr float SECTION_CYAN_R = 0.3922f + DEFAULT_THEME_FACTOR * (1.0f - 2.0f * 0.3922f);
            constexpr float SECTION_CYAN_G = 0.7843f + DEFAULT_THEME_FACTOR * (1.0f - 2.0f * 0.7843f);
            constexpr float SECTION_CYAN_B = 1.0000f + DEFAULT_THEME_FACTOR * (1.0f - 2.0f * 1.0000f);

            // Section header — orange (255,160,80)
            constexpr float SECTION_ORANGE_R = 1.0000f + DEFAULT_THEME_FACTOR * (1.0f - 2.0f * 1.0000f);
            constexpr float SECTION_ORANGE_G = 0.6275f + DEFAULT_THEME_FACTOR * (1.0f - 2.0f * 0.6275f);
            constexpr float SECTION_ORANGE_B = 0.3137f + DEFAULT_THEME_FACTOR * (1.0f - 2.0f * 0.3137f);

            // Section header — purple (200,120,255)
            constexpr float SECTION_PURPLE_R = 0.7843f + DEFAULT_THEME_FACTOR * (1.0f - 2.0f * 0.7843f);
            constexpr float SECTION_PURPLE_G = 0.4706f + DEFAULT_THEME_FACTOR * (1.0f - 2.0f * 0.4706f);
            constexpr float SECTION_PURPLE_B = 1.0000f + DEFAULT_THEME_FACTOR * (1.0f - 2.0f * 1.0000f);

            // Section header — green (80,220,120) — Advanced / Power User
            constexpr float SECTION_GREEN_R = 0.3137f + DEFAULT_THEME_FACTOR * (1.0f - 2.0f * 0.3137f);
            constexpr float SECTION_GREEN_G = 0.8627f + DEFAULT_THEME_FACTOR * (1.0f - 2.0f * 0.8627f);
            constexpr float SECTION_GREEN_B = 0.4706f + DEFAULT_THEME_FACTOR * (1.0f - 2.0f * 0.4706f);

            // Shortcut key text — yellow (255,220,0)
            constexpr float SHORTCUT_KEY_R = 1.0000f + DEFAULT_THEME_FACTOR * (1.0f - 2.0f * 1.0000f);
            constexpr float SHORTCUT_KEY_G = 0.8627f + DEFAULT_THEME_FACTOR * (1.0f - 2.0f * 0.8627f);
            constexpr float SHORTCUT_KEY_B = 0.0000f + DEFAULT_THEME_FACTOR * (1.0f - 2.0f * 0.0000f);

            // Description text — near-white gray (230,230,230)
            constexpr float DESCRIPTION = 0.9020f + DEFAULT_THEME_FACTOR * (1.0f - 2.0f * 0.9020f);

            // Link colors: see Constants::Links in Constants.h — the app-wide
            // single source of truth for clickable link color + underline.

            // Scrollbar colours are Theme::Scrollbar now — this panel's copy was
            // byte-identical to ExifWindow's, which is what made one shared set
            // the obvious answer.
        }

        // =====================================================================
        // MARKER COLORS  —  single source of truth for status/indicator colors
        // =====================================================================
        namespace Markers {
            constexpr COLORREF OK        = RGB(80,  200, 80);
            constexpr COLORREF INFO      = RGB(60,  140, 230);
            constexpr COLORREF WARNING   = RGB(240, 180, 60);
            constexpr COLORREF ERR       = RGB(255, 80,  80);
            constexpr COLORREF CRITICAL  = RGB(60,  15,  15);
            constexpr COLORREF FAVORITES = RGB(255, 200, 50);
            constexpr COLORREF SYMLINK   = RGB(190, 160, 255); // reparse point / junction glyph
            // Badge-stack glyph. Deliberately a NEUTRAL silver and not borrowed
            // from any status colour: the stack is a container, not a state, so
            // tinting it red/gold/violet would claim one of the badges underneath
            // it is the important one. Grey says "look inside" and nothing more.
            constexpr COLORREF BADGE_STACK = RGB(195, 200, 210);
        }

        // =====================================================================
        // RENDERER COLORS
        // =====================================================================
        namespace Renderer {
            // GDI fallback debug/info text — pure green (0,255,0)
            constexpr float TEXT_DEBUG_R = 0.0f + DEFAULT_THEME_FACTOR * (1.0f - 2.0f * 0.0f);
            constexpr float TEXT_DEBUG_G = 1.0f + DEFAULT_THEME_FACTOR * (1.0f - 2.0f * 1.0f);
            constexpr float TEXT_DEBUG_B = 0.0f + DEFAULT_THEME_FACTOR * (1.0f - 2.0f * 0.0f);
        }

        // =====================================================================
        // COLOR CONVERSION HELPERS
        // =====================================================================
        // Convert channel float (0-1) to byte with rounding
        inline constexpr BYTE ToByte(float v) {
            return static_cast<BYTE>(v * 255.0f + 0.5f);
        }

        // COLORREF from grayscale value
        inline COLORREF Gray(float gray) {
            const BYTE v = ToByte(gray);
            return RGB(v, v, v);
        }

        // COLORREF from RGB channel floats
        inline COLORREF Color(float r, float g, float b) {
            return RGB(ToByte(r), ToByte(g), ToByte(b));
        }

        // D2D color from grayscale value with optional alpha
        inline D2D1_COLOR_F GrayD2D(float gray, float alpha = 1.0f) {
            return D2D1::ColorF(gray, gray, gray, alpha);
        }

        // =====================================================================
        // RUNTIME THEME HELPERS  —  apply app.themeFactor at the call site
        // =====================================================================
        // Core formula: mirrors compile-time formula, applied at runtime.
        inline float Apply(float base, float factor) {
            return base + factor * (1.0f - 2.0f * base);
        }

        // Runtime-themed grayscale D2D color
        inline D2D1_COLOR_F ThemedGrayD2D(float base, float factor, float alpha = 1.0f) {
            const float v = Apply(base, factor);
            return D2D1::ColorF(v, v, v, alpha);
        }

        // =====================================================================
        // Scrollbar — ONE set, for every scrolled surface in the app.
        //
        // There were four: HelpWindow and ExifWindow had byte-identical values
        // written out twice, HistoryPanel had its own darker pair, and the
        // Dedicated panel used flat COLORREFs that ignored the theme factor
        // entirely — so the same control changed colour depending on which
        // window it was in, and one of them did not follow the theme at all.
        //
        // The Help/Exif values win because two panels already agreed on them.
        // The HOT thumb comes from the Dedicated set, which was the only one
        // that had a hover state worth keeping.
        //
        // Read through UI::ThemeScrollBarColors(app.themeFactor); nothing should
        // reach for these directly.
        namespace Scrollbar {
            // track (50,50,55), thumb (150,150,160), hot thumb (132,156,196)
            constexpr float TRACK_R = 0.1961f + DEFAULT_THEME_FACTOR * (1.0f - 2.0f * 0.1961f);
            constexpr float TRACK_G = 0.1961f + DEFAULT_THEME_FACTOR * (1.0f - 2.0f * 0.1961f);
            constexpr float TRACK_B = 0.2157f + DEFAULT_THEME_FACTOR * (1.0f - 2.0f * 0.2157f);

            constexpr float THUMB_R = 0.5882f + DEFAULT_THEME_FACTOR * (1.0f - 2.0f * 0.5882f);
            constexpr float THUMB_G = 0.5882f + DEFAULT_THEME_FACTOR * (1.0f - 2.0f * 0.5882f);
            constexpr float THUMB_B = 0.6275f + DEFAULT_THEME_FACTOR * (1.0f - 2.0f * 0.6275f);

            constexpr float THUMB_HOT_R = 0.5176f + DEFAULT_THEME_FACTOR * (1.0f - 2.0f * 0.5176f);
            constexpr float THUMB_HOT_G = 0.6118f + DEFAULT_THEME_FACTOR * (1.0f - 2.0f * 0.6118f);
            constexpr float THUMB_HOT_B = 0.7686f + DEFAULT_THEME_FACTOR * (1.0f - 2.0f * 0.7686f);
        }

        // Runtime-themed grayscale GDI color
        inline COLORREF ThemedGray(float base, float factor) {
            return RGB(ToByte(Apply(base, factor)), ToByte(Apply(base, factor)), ToByte(Apply(base, factor)));
        }

        // Runtime-themed RGB GDI color
        inline COLORREF ThemedColor(float r, float g, float b, float factor) {
            return RGB(ToByte(Apply(r, factor)), ToByte(Apply(g, factor)), ToByte(Apply(b, factor)));
        }
    }
namespace ThemeIcons {
    // ── Status / indicators ──────────────────────────────────────────
    constexpr const wchar_t* ICON_FAVORITES_MARK  = L"\x2605";        // ★
    constexpr const wchar_t* ICON_SYMLINK_MARK     = L"\U0001F517";   // 🔗
    constexpr const wchar_t* ICON_WARNING          = L"\x26A0";       // ⚠
    constexpr const wchar_t* ICON_EMPTY            = L"\x2205";       // ∅
    constexpr const wchar_t* ICON_CLOSE            = L"\x2715";       // ✕
    constexpr const wchar_t* ICON_CHECK            = L"\x2714";       // ✔
    constexpr const wchar_t* ICON_FOLDER_ARROW     = L"\x25B8";       // ▸
    // Shown when a row has MORE than one badge and only one slot to show them in;
    // hovering it lists them all. Two joined squares — the layered look says
    // "several things stacked here" without borrowing any badge's own meaning.
    constexpr const wchar_t* ICON_BADGE_STACK      = L"\x29C9";       // ⧉

    // ── Directional arrows ───────────────────────────────────────────
    constexpr const wchar_t* ICON_ARROW_RIGHT      = L"\x2192";       // →
    constexpr const wchar_t* ICON_ARROW_DOWN       = L"\x2193";       // ↓
    constexpr const wchar_t* ICON_ARROWS_UP_DOWN   = L"\x2191\x2193"; // ↑↓

    // ── Media playback ───────────────────────────────────────────────
    constexpr const wchar_t* ICON_PLAY             = L"\x25B6";       // ▶
    constexpr const wchar_t* ICON_PAUSE            = L"\x23F8";       // ⏸
    constexpr const wchar_t* ICON_STOP             = L"\x25A0";       // ■

    // ── Wrap navigation ──────────────────────────────────────────────
    constexpr const wchar_t* ICON_WRAP_START       = L"\x21A9";       // ↩
    constexpr const wchar_t* ICON_WRAP_END         = L"\x21AA";       // ↪

    // ── Objects / folders ────────────────────────────────────────────
    constexpr const wchar_t* ICON_FOLDER           = L"\U0001F4C1";   // 📁

    // ── HelpWnd section emoji ────────────────────────────────────────
    constexpr const wchar_t* ICON_SECTION_COMPASS    = L"\U0001F9ED";     // 🧭
    constexpr const wchar_t* ICON_SECTION_MAGNIFIER  = L"\U0001F50D";     // 🔍
    constexpr const wchar_t* ICON_SECTION_MOUSE      = L"\U0001F5B1\xFE0F";// 🖱️
    constexpr const wchar_t* ICON_SECTION_WINDOW     = L"\U0001FA9F";     // 🪟
    constexpr const wchar_t* ICON_SECTION_TOOLBOX    = L"\U0001F9F0";     // 🧰
    constexpr const wchar_t* ICON_SECTION_PICTURE    = L"\U0001F5BC\xFE0F";// 🖼️
    constexpr const wchar_t* ICON_SECTION_SCROLL     = L"\U0001F4DC";     // 📜
    constexpr const wchar_t* ICON_SECTION_PLAY       = L"\x25B6\xFE0F";   // ▶️
    constexpr const wchar_t* ICON_SECTION_INFO       = L"\x2139\xFE0F";   // ℹ️
    constexpr const wchar_t* ICON_SECTION_PALETTE    = L"\U0001F3A8";     // 🎨
    constexpr const wchar_t* ICON_SECTION_FLOPPY     = L"\U0001F4BE";     // 💾
    constexpr const wchar_t* ICON_SECTION_GEAR       = L"\x2699\xFE0F";   // ⚙️
    constexpr const wchar_t* ICON_SECTION_BELL       = L"\U0001F514";     // 🔔
    constexpr const wchar_t* ICON_SECTION_DESKTOP    = L"\U0001F5A5\xFE0F";// 🖥️
    constexpr const wchar_t* ICON_SECTION_KEYBOARD   = L"\x2328\xFE0F";   // ⌨️
    constexpr const wchar_t* ICON_SECTION_ANTENNA    = L"\U0001F4E1";     // 📡
    }
} 
