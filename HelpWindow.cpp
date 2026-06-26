#include "HelpWindow.h"
#include <string>
#include <dwmapi.h>

namespace UI {
    HWND g_hHelpWnd = nullptr;

    LRESULT CALLBACK HelpWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC hdc = BeginPaint(hWnd, &ps);

                RECT rc;
                GetClientRect(hWnd, &rc);

                // Dark background
                HBRUSH hBrush = CreateSolidBrush(RGB(24, 24, 24)); // Sleek dark slate
                FillRect(hdc, &rc, hBrush);
                DeleteObject(hBrush);

                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, RGB(240, 240, 240));

                // Scale font size based on your 4K 200% DPI settings
                UINT dpi = GetDpiForWindow(hWnd);
                int fontSize = MulDiv(18, dpi, 96); 

                HFONT hFont = CreateFontW(fontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                          DEFAULT_CHARSET, OUT_OUTLINE_PRECIS,
                                          CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                          VARIABLE_PITCH, L"Segoe UI");
                HFONT hOldFont = (HFONT)SelectObject(hdc, hFont);

                std::wstring helpText =
                    L" Quick Image Viewer - Shortcuts\n\n"
                    L" [Navigation]\n"
                    L" Left / Right Arrow\t: Previous / Next Image\n"
                    L" Space / Shift+Space\t: Next / Previous Image\n"
                    L" Scroll Wheel\t\t: Next / Previous Image\n\n"
                    L" [Zoom & Pan]\n"
                    L" Up / Down Arrow\t: Zoom In / Out\n"
                    L" Ctrl + Scroll\t\t: Zoom In / Out\n"
                    L" Numpad 0\t\t: Reset Zoom & Center\n"
                    L" Right Mouse Hold\t: 2x Quick Zoom & Pan\n\n"
                    L" [Window Controls]\n"
                    L" F / F11 / Enter\t\t: Toggle Fullscreen\n"
                    L" Left Mouse Drag\t: Move Window\n"
                    L" Middle Mouse Drag\t: Resize Window\n"
                    L" Middle Mouse Click\t: Reset & Center Window\n\n"
                    L" [Application]\n"
                    L" Ctrl + N\t\t: Open New Viewer Window\n"
                    L" Shift + Open File\t: Open in New Instance\n"
                    L" Esc / Ctrl+W / X\t: Hide to Background (Instant Load)\n"
                    L" Ctrl + Q\t\t: Quit Completely\n"
                    L" F1\t\t\t: Toggle this Help Menu\n";

                // Add padding inside the window
                int padding = MulDiv(20, dpi, 96);
                rc.left += padding;
                rc.top += padding;

                DrawTextW(hdc, helpText.c_str(), -1, &rc, DT_LEFT | DT_TOP | DT_EXPANDTABS);

                SelectObject(hdc, hOldFont);
                DeleteObject(hFont);
                EndPaint(hWnd, &ps);
                return 0;
            }

            case WM_KEYDOWN:
                // Let Esc or F1 close the help window easily
                if (wParam == VK_ESCAPE || wParam == VK_F1) {
                    ShowWindow(hWnd, SW_HIDE);
                }
                return 0;

            case WM_CLOSE:
                ShowWindow(hWnd, SW_HIDE);
                return 0;
        }
        return DefWindowProcW(hWnd, message, wParam, lParam);
    }

    void InitHelpWindow(HINSTANCE hInstance, HWND hParent) {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc = HelpWndProc;
        wc.hInstance = hInstance;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = L"QIV_HelpWindow";
        RegisterClassW(&wc);

        // Calculate size based on DPI
        UINT dpi = GetDpiForWindow(hParent);
        int winW = MulDiv(550, dpi, 96);
        int winH = MulDiv(650, dpi, 96);

        // Create as a Tool Window so it doesn't create a second icon on the taskbar
        g_hHelpWnd = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
            wc.lpszClassName,
            L"Shortcuts",
            WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_BORDER,
            0, 0, winW, winH,
            hParent, nullptr, hInstance, nullptr
        );

        // Apply DWM rounded corners to match Windows 11 styling
        DWORD corner = 2; // DWMWCP_ROUND
        DwmSetWindowAttribute(g_hHelpWnd, 33, &corner, sizeof(corner));
    }

    void ToggleHelpWindow() {
        if (!g_hHelpWnd) return;

        if (IsWindowVisible(g_hHelpWnd)) {
            ShowWindow(g_hHelpWnd, SW_HIDE);
        } else {
            // Center the help window directly over the main app window
            HWND hParent = GetWindow(g_hHelpWnd, GW_OWNER);
            if (hParent) {
                RECT rcParent, rcHelp;
                GetWindowRect(hParent, &rcParent);
                GetWindowRect(g_hHelpWnd, &rcHelp);
                int x = rcParent.left + ((rcParent.right - rcParent.left) - (rcHelp.right - rcHelp.left)) / 2;
                int y = rcParent.top + ((rcParent.bottom - rcParent.top) - (rcHelp.bottom - rcHelp.top)) / 2;
                SetWindowPos(g_hHelpWnd, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);
            } else {
                ShowWindow(g_hHelpWnd, SW_SHOW);
            }
            SetForegroundWindow(g_hHelpWnd);
        }
    }
}