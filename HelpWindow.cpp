#include "HelpWindow.h"
#include "Constants.h"
#include <string>
#include <dwmapi.h>

namespace UI {
    HWND g_hHelpWnd = nullptr;
    std::wstring fullTitle = std::wstring(Config::BASE_NAME) + L" v" + Config::APP_VERSION;

    LRESULT CALLBACK HelpWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC hdc = BeginPaint(hWnd, &ps);
                RECT rc;
                GetClientRect(hWnd, &rc);

                // 1. Dark Background
                HBRUSH hBrush = CreateSolidBrush(RGB(24, 24, 24));
                FillRect(hdc, &rc, hBrush);
                DeleteObject(hBrush);

                UINT dpi = GetDpiForWindow(hWnd);
                int padding = MulDiv(25, dpi, 96);
                int fontSize = MulDiv(20, dpi, 96);

                HFONT hFont = CreateFontW(fontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                          DEFAULT_CHARSET, OUT_OUTLINE_PRECIS,
                                          CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                          VARIABLE_PITCH, L"Segoe UI");
                HFONT hOldFont = (HFONT) SelectObject(hdc, hFont);
                SetBkMode(hdc, TRANSPARENT);

                // Lambda to draw a two-tone line: [Key] : [Description]
                int y = padding;
                auto DrawLine = [&](const std::wstring &key, const std::wstring &desc) {
                    RECT lineRect = {rc.left + padding, y, rc.right - padding, y + fontSize + 5};

                    // Draw Key (Gold/Yellow)
                    SetTextColor(hdc, RGB(255, 204, 0));
                    DrawTextW(hdc, key.c_str(), -1, &lineRect, DT_LEFT);

                    // Draw Description (Light Grey)
                    SetTextColor(hdc, RGB(220, 220, 220));
                    lineRect.left += MulDiv(230, dpi, 96); // Constant tab offset
                    DrawTextW(hdc, desc.c_str(), -1, &lineRect, DT_LEFT);

                    y += fontSize + 10;
                };

                // Draw Header
                SetTextColor(hdc, RGB(100, 200, 255));

                SetTextColor(hdc, RGB(100, 200, 255));
                DrawTextW(hdc, fullTitle.c_str(), -1, &rc, DT_CENTER);
                y += fontSize * 2;

                // Render explicit shortcuts
                DrawLine(L"Left/Right", L": Previous / Next image in folder");
                DrawLine(L"Space/Shift+Space", L": Next / Previous image in folder");
                DrawLine(L"Wheel Scroll", L": Next / Previous image in folder");
                DrawLine(L"E / Tab", L": Open file location in Windows Explorer");
                DrawLine(L"R / Ctrl+R", L": Rotate image 90° Clockwise / Counter");
                DrawLine(L"H", L": Flip image Horizontally");
                DrawLine(L"V", L": Flip image Vertically");
                y += fontSize / 2; // Spacer
                // --- NEW SECTION: Opacity ---
                DrawLine(L"Shift + Wheel", L": Adjust Window/Image Opacity");
                y += fontSize / 2; // Spacer
                DrawLine(L"Up/Down", L": Zoom image In / Out");
                DrawLine(L"Ctrl + Wheel", L": Zoom image In / Out");
                DrawLine(L"Numpad 0", L": Reset zoom and opacity and center image");
                DrawLine(L"Right Click", L": 2x Quick zoom and toggle pan");
                y += fontSize / 2; // Spacer
                DrawLine(L"F11 / Enter", L": Toggle Fullscreen mode");
                DrawLine(L"Left Click Drag", L": Pan image view");
                DrawLine(L"Middle Drag", L": Resize window dimensions");
                DrawLine(L"Middle Click", L": Reset window and center image");
                y += fontSize / 2; // Spacer
                DrawLine(L"Ctrl+N", L": Open new viewer instance");
                DrawLine(L"Esc / Ctrl+W", L": Hide window to background");
                DrawLine(L"Ctrl+Q", L": Quit application process");
                DrawLine(L"F1", L": Toggle help menu");
                DrawLine(L"ESC", L": Hide help menu");

                // --- FOOTER: Version and Creator ---
                int footerSize = MulDiv(14, dpi, 96);
                HFONT hFooterFont = CreateFontW(footerSize, 0, 0, 0, FW_LIGHT, FALSE, FALSE, FALSE,
                                                DEFAULT_CHARSET, OUT_OUTLINE_PRECIS,
                                                CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                                VARIABLE_PITCH, L"Segoe UI");
                SelectObject(hdc, hFooterFont);
                SetTextColor(hdc, RGB(120, 120, 120));

                std::wstring footer = std::wstring(Config::APP_CREATOR) + L" | " + Config::APP_HELP_FOOTER;
                RECT footerRect = {rc.left, rc.bottom - padding, rc.right, rc.bottom};
                DrawTextW(hdc, footer.c_str(), -1, &footerRect, DT_CENTER | DT_BOTTOM);

                SelectObject(hdc, hOldFont);
                DeleteObject(hFont);
                DeleteObject(hFooterFont);
                EndPaint(hWnd, &ps);
                return 0;
            }

            case WM_KEYDOWN:
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


    void ToggleHelpWindow() {
        if (!g_hHelpWnd) return;

        if (IsWindowVisible(g_hHelpWnd)) {
            ShowWindow(g_hHelpWnd, SW_HIDE);
        } else {
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

    void InitHelpWindow(HINSTANCE hInstance, HWND hParent) {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc = HelpWndProc;
        wc.hInstance = hInstance;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = L"QIV_HelpWindow";
        RegisterClassW(&wc);

        UINT dpi = GetDpiForWindow(hParent);
        int winW = MulDiv(600, dpi, 96);
        int winH = MulDiv(750, dpi, 96);

        // Removed WS_EX_LAYERED to keep it solid 255
        g_hHelpWnd = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
            wc.lpszClassName,
            Config::APP_TASKBAR_NAME,
            WS_POPUP | WS_CAPTION | WS_BORDER,
            0, 0, winW, winH,
            hParent, nullptr, hInstance, nullptr
        );

        // 1. Force Dark Mode
        BOOL darkMode = TRUE;
        DwmSetWindowAttribute(g_hHelpWnd, 20, &darkMode, sizeof(darkMode));

        // 2. Rounded corners
        DWORD corner = 2;
        DwmSetWindowAttribute(g_hHelpWnd, 33, &corner, sizeof(corner));

        // 3. Caption Color to ensure clean integration
        COLORREF darkColor = RGB(24, 24, 24);
        DwmSetWindowAttribute(g_hHelpWnd, 35, &darkColor, sizeof(darkColor));

        // 4. Force frame recalculation
        SetWindowPos(g_hHelpWnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }
}
