#pragma once
#include <windows.h>

namespace UI {
    // Single cached dialog window.  Created once at startup, reused for every
    // confirmation prompt and informational message.  Automatically matches the
    // app's dark-mode, corner preference, caption color, and per-monitor DPI.
    class ThemedDialog {
    public:
        // Call once from wWinMain after the main window exists.
        static void Init(HWND hMainWnd);

        // Modal confirmation — returns true when the user clicks Yes, false for No / Escape / X.
        static bool Confirm(HWND hOwner, const wchar_t *text, const wchar_t *caption);

        // Modal informational message — single OK button.
        static void Message(HWND hOwner, const wchar_t *text, const wchar_t *caption);

    private:
        static int  RunModal(HWND hOwner, const wchar_t *text, const wchar_t *caption, bool confirm);
        static void ApplyTheme();
        static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

        static HWND   s_hwnd;
        static HWND   s_hwndText;
        static HWND   s_hwndBtn1;   // Yes / OK  (IDOK)
        static HWND   s_hwndBtn2;   // No        (IDCANCEL) — hidden in message mode
        static HFONT  s_font;
        static HBRUSH s_bgBrush;    // client-area fill, rebuilt on each ApplyTheme()
        static int    s_result;
        static bool   s_running;
    };
}
