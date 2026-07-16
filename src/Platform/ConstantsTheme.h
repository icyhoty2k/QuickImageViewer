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
        constexpr float THEME_FACTOR = 0.0f;

        // Step size for runtime THEME_FACTOR adjustment (Ctrl+Alt+Shift+Numpad+/-).
        constexpr float THEME_FACTOR_STEP = 0.05f;

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
            constexpr float MAIN_WINDOW = 0.08f + THEME_FACTOR * (1.0f - 2.0f * 0.08f);
        }

        // =====================================================================
        // CHILD PANEL COLORS (DirWnd, CacheWnd, SpawnedDirWnd, HistoryWnd bg)
        // =====================================================================
        namespace Panel {
            // Inactive panel background — 0.08 gray, alpha 0.75
            constexpr float BACKGROUND_INACTIVE = 0.08f + THEME_FACTOR * (1.0f - 2.0f * 0.08f);
            constexpr float BACKGROUND_INACTIVE_ALPHA = 0.75f; // not themed

            // Active (focused) panel background — 0.02 gray, alpha 0.90
            constexpr float BACKGROUND_ACTIVE = 0.02f + THEME_FACTOR * (1.0f - 2.0f * 0.02f);
            constexpr float BACKGROUND_ACTIVE_ALPHA = 0.90f; // not themed

            // D2D scrollbar strip — track near-black 0.12 @ 70%, thumb mid-grey 0.65 @ 85%
            constexpr float SCROLLBAR_TRACK = 0.12f + THEME_FACTOR * (1.0f - 2.0f * 0.12f);
            constexpr float SCROLLBAR_TRACK_ALPHA = 0.70f; // not themed
            constexpr float SCROLLBAR_THUMB = 0.65f + THEME_FACTOR * (1.0f - 2.0f * 0.65f);
            constexpr float SCROLLBAR_THUMB_ALPHA = 0.85f; // not themed
        }

        // =====================================================================
        // HISTORY PANEL COLORS (GDI)
        // =====================================================================
        namespace HistoryPanel {
            // Scrollbar — track (28,28,28), thumb (110,110,110)
            constexpr float SCROLLBAR_TRACK = 0.1098f + THEME_FACTOR * (1.0f - 2.0f * 0.1098f);
            constexpr float SCROLLBAR_THUMB = 0.4314f + THEME_FACTOR * (1.0f - 2.0f * 0.4314f);
            // Accent color used for the history file-size value in the header
            constexpr COLORREF SIZE_HIGHLIGHT = RGB(240, 50, 50);

            // Path row — three segments: drive letter, middle path, final folder name.
            // Non-favorite rows:
            constexpr COLORREF PATH_DRIVE = RGB(100, 185, 205); // muted teal
            constexpr COLORREF PATH_DRIVE_HOVER = RGB(140, 215, 235);
            constexpr COLORREF PATH_FOLDER = RGB(232, 215, 170); // warm tan — clearly distinct from gray middle
            constexpr COLORREF PATH_FOLDER_HOVER = RGB(255, 248, 210);
            // Favorite rows:
            constexpr COLORREF PATH_DRIVE_FAV = RGB(195, 165, 70); // amber/gold
            constexpr COLORREF PATH_DRIVE_FAV_HOVER = RGB(215, 195, 105);
            constexpr COLORREF PATH_FOLDER_FAV = RGB(255, 248, 152); // bright warm yellow
            constexpr COLORREF PATH_FOLDER_FAV_HOVER = RGB(255, 255, 195);
            // Current folder row text colors — "you are here" green, 3 distinct shades:
            constexpr COLORREF PATH_DRIVE_CURRENT  = RGB(80,  195, 115); // mid-green for drive letter
            constexpr COLORREF PATH_MIDDLE_CURRENT = RGB(100, 160, 120); // muted green for middle path
            constexpr COLORREF PATH_FOLDER_CURRENT = RGB(160, 230, 165); // bright pale-green for folder name
            // Dead folder row text colors — folder missing or contains no images:
            constexpr COLORREF PATH_DEAD_DRIVE  = RGB(210,  70,  70); // red for drive / index / warning glyph
            constexpr COLORREF PATH_DEAD_MIDDLE = RGB(160,  60,  60); // darker red for middle path
            constexpr COLORREF PATH_DEAD_FOLDER = RGB(230, 100, 100); // lighter red for folder name
        }

        // =====================================================================
        // EXIF / INFO WINDOW COLORS
        // =====================================================================
        namespace ExifWindow {
            // Label text (EXIF field names) — yellow (255,220,0)
            constexpr float LABEL_R = 1.0000f + THEME_FACTOR * (1.0f - 2.0f * 1.0000f);
            constexpr float LABEL_G = 0.8627f + THEME_FACTOR * (1.0f - 2.0f * 0.8627f);
            constexpr float LABEL_B = 0.0000f + THEME_FACTOR * (1.0f - 2.0f * 0.0000f);

            // Value text — near-white (230,230,230)
            constexpr float VALUE = 0.9020f + THEME_FACTOR * (1.0f - 2.0f * 0.9020f);

            // Section header — cyan (100,200,255)
            constexpr float SECTION_R = 0.3922f + THEME_FACTOR * (1.0f - 2.0f * 0.3922f);
            constexpr float SECTION_G = 0.7843f + THEME_FACTOR * (1.0f - 2.0f * 0.7843f);
            constexpr float SECTION_B = 1.0000f + THEME_FACTOR * (1.0f - 2.0f * 1.0000f);

            // Scrollbar — track (50,50,55), thumb (150,150,160)
            constexpr float SCROLLBAR_TRACK_R = 0.1961f + THEME_FACTOR * (1.0f - 2.0f * 0.1961f);
            constexpr float SCROLLBAR_TRACK_G = 0.1961f + THEME_FACTOR * (1.0f - 2.0f * 0.1961f);
            constexpr float SCROLLBAR_TRACK_B = 0.2157f + THEME_FACTOR * (1.0f - 2.0f * 0.2157f);
            constexpr float SCROLLBAR_THUMB_R = 0.5882f + THEME_FACTOR * (1.0f - 2.0f * 0.5882f);
            constexpr float SCROLLBAR_THUMB_G = 0.5882f + THEME_FACTOR * (1.0f - 2.0f * 0.5882f);
            constexpr float SCROLLBAR_THUMB_B = 0.6275f + THEME_FACTOR * (1.0f - 2.0f * 0.6275f);
        }

        // =====================================================================
        // HELP WINDOW COLORS
        // =====================================================================
        namespace HelpWindow {
            // Background (20,20,22)
            constexpr float BACKGROUND_R = 0.0784f + THEME_FACTOR * (1.0f - 2.0f * 0.0784f);
            constexpr float BACKGROUND_G = 0.0784f + THEME_FACTOR * (1.0f - 2.0f * 0.0784f);
            constexpr float BACKGROUND_B = 0.0863f + THEME_FACTOR * (1.0f - 2.0f * 0.0863f);

            // Title — cyan (100,200,255)
            constexpr float TITLE_R = 0.3922f + THEME_FACTOR * (1.0f - 2.0f * 0.3922f);
            constexpr float TITLE_G = 0.7843f + THEME_FACTOR * (1.0f - 2.0f * 0.7843f);
            constexpr float TITLE_B = 1.0000f + THEME_FACTOR * (1.0f - 2.0f * 1.0000f);

            // Subtitle / footer text — gray (140,140,140)
            constexpr float SUBTITLE = 0.5490f + THEME_FACTOR * (1.0f - 2.0f * 0.5490f);

            // Section header — cyan (100,200,255)
            constexpr float SECTION_CYAN_R = 0.3922f + THEME_FACTOR * (1.0f - 2.0f * 0.3922f);
            constexpr float SECTION_CYAN_G = 0.7843f + THEME_FACTOR * (1.0f - 2.0f * 0.7843f);
            constexpr float SECTION_CYAN_B = 1.0000f + THEME_FACTOR * (1.0f - 2.0f * 1.0000f);

            // Section header — orange (255,160,80)
            constexpr float SECTION_ORANGE_R = 1.0000f + THEME_FACTOR * (1.0f - 2.0f * 1.0000f);
            constexpr float SECTION_ORANGE_G = 0.6275f + THEME_FACTOR * (1.0f - 2.0f * 0.6275f);
            constexpr float SECTION_ORANGE_B = 0.3137f + THEME_FACTOR * (1.0f - 2.0f * 0.3137f);

            // Section header — purple (200,120,255)
            constexpr float SECTION_PURPLE_R = 0.7843f + THEME_FACTOR * (1.0f - 2.0f * 0.7843f);
            constexpr float SECTION_PURPLE_G = 0.4706f + THEME_FACTOR * (1.0f - 2.0f * 0.4706f);
            constexpr float SECTION_PURPLE_B = 1.0000f + THEME_FACTOR * (1.0f - 2.0f * 1.0000f);

            // Section header — green (80,220,120) — Advanced / Power User
            constexpr float SECTION_GREEN_R = 0.3137f + THEME_FACTOR * (1.0f - 2.0f * 0.3137f);
            constexpr float SECTION_GREEN_G = 0.8627f + THEME_FACTOR * (1.0f - 2.0f * 0.8627f);
            constexpr float SECTION_GREEN_B = 0.4706f + THEME_FACTOR * (1.0f - 2.0f * 0.4706f);

            // Shortcut key text — yellow (255,220,0)
            constexpr float SHORTCUT_KEY_R = 1.0000f + THEME_FACTOR * (1.0f - 2.0f * 1.0000f);
            constexpr float SHORTCUT_KEY_G = 0.8627f + THEME_FACTOR * (1.0f - 2.0f * 0.8627f);
            constexpr float SHORTCUT_KEY_B = 0.0000f + THEME_FACTOR * (1.0f - 2.0f * 0.0000f);

            // Description text — near-white gray (230,230,230)
            constexpr float DESCRIPTION = 0.9020f + THEME_FACTOR * (1.0f - 2.0f * 0.9020f);

            // Link colors: see Constants::Links in Constants.h — the app-wide
            // single source of truth for clickable link color + underline.

            // Scrollbar — track (50,50,55), thumb (150,150,160)
            constexpr float SCROLLBAR_TRACK_R = 0.1961f + THEME_FACTOR * (1.0f - 2.0f * 0.1961f);
            constexpr float SCROLLBAR_TRACK_G = 0.1961f + THEME_FACTOR * (1.0f - 2.0f * 0.1961f);
            constexpr float SCROLLBAR_TRACK_B = 0.2157f + THEME_FACTOR * (1.0f - 2.0f * 0.2157f);
            constexpr float SCROLLBAR_THUMB_R = 0.5882f + THEME_FACTOR * (1.0f - 2.0f * 0.5882f);
            constexpr float SCROLLBAR_THUMB_G = 0.5882f + THEME_FACTOR * (1.0f - 2.0f * 0.5882f);
            constexpr float SCROLLBAR_THUMB_B = 0.6275f + THEME_FACTOR * (1.0f - 2.0f * 0.6275f);
        }

        // =====================================================================
        // RENDERER COLORS
        // =====================================================================
        namespace Renderer {
            // GDI fallback debug/info text — pure green (0,255,0)
            constexpr float TEXT_DEBUG_R = 0.0f + THEME_FACTOR * (1.0f - 2.0f * 0.0f);
            constexpr float TEXT_DEBUG_G = 1.0f + THEME_FACTOR * (1.0f - 2.0f * 1.0f);
            constexpr float TEXT_DEBUG_B = 0.0f + THEME_FACTOR * (1.0f - 2.0f * 0.0f);
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

        // Runtime-themed grayscale GDI color
        inline COLORREF ThemedGray(float base, float factor) {
            return RGB(ToByte(Apply(base, factor)), ToByte(Apply(base, factor)), ToByte(Apply(base, factor)));
        }

        // Runtime-themed RGB GDI color
        inline COLORREF ThemedColor(float r, float g, float b, float factor) {
            return RGB(ToByte(Apply(r, factor)), ToByte(Apply(g, factor)), ToByte(Apply(b, factor)));
        }
    } // namespace Theme
} // namespace Constants
