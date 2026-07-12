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

    // Corner style applied at window creation.
    // 0 DWMWCP_DEFAULT    — let Windows decide
    // 1 DWMWCP_DONOTROUND — square corners
    // 2 DWMWCP_ROUND      — standard rounded corners
    // 3 DWMWCP_ROUNDSMALL — slightly rounded corners
    constexpr DWORD APP_CORNER_PREFERENCES = DWMWCP_ROUND;

    namespace Theme {
        // =====================================================================
        // MASTER THEME CONTROL
        // =====================================================================
        // 0.0f = Dark Theme (original, exact), 1.0f = Inverted Light Theme, 0.5f = Mix
        constexpr float THEME_FACTOR = 0.0f;

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

            // Shortcut key text — yellow (255,220,0)
            constexpr float SHORTCUT_KEY_R = 1.0000f + THEME_FACTOR * (1.0f - 2.0f * 1.0000f);
            constexpr float SHORTCUT_KEY_G = 0.8627f + THEME_FACTOR * (1.0f - 2.0f * 0.8627f);
            constexpr float SHORTCUT_KEY_B = 0.0000f + THEME_FACTOR * (1.0f - 2.0f * 0.0000f);

            // Description text — near-white gray (230,230,230)
            constexpr float DESCRIPTION = 0.9020f + THEME_FACTOR * (1.0f - 2.0f * 0.9020f);

            // Facebook link — light blue (100,180,255)
            constexpr float LINK_R = 0.3922f + THEME_FACTOR * (1.0f - 2.0f * 0.3922f);
            constexpr float LINK_G = 0.7059f + THEME_FACTOR * (1.0f - 2.0f * 0.7059f);
            constexpr float LINK_B = 1.0000f + THEME_FACTOR * (1.0f - 2.0f * 1.0000f);

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
    } // namespace Theme
} // namespace Constants
