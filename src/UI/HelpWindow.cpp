#include "HelpWindow.h"
#include "../Platform/Constants.h"
#include "../Platform/Shortcuts.h"
#include <string>
#include <dwmapi.h>
#include <algorithm>

namespace UI {
    HWND g_hHelpWnd = nullptr;
    std::wstring fullTitle = std::wstring(Constants::BASE_NAME) + L" v" + Constants::APP_VERSION;

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
                int padding = MulDiv(20, dpi, 96);
                int fontSize = MulDiv(16, dpi, 96);

                HFONT hFont = CreateFontW(fontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                          DEFAULT_CHARSET, OUT_OUTLINE_PRECIS,
                                          CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                          VARIABLE_PITCH, L"Segoe UI");
                HFONT hOldFont = (HFONT) SelectObject(hdc, hFont);
                SetBkMode(hdc, TRANSPARENT);

                int y = padding + MulDiv(20, dpi, 96);

                // Standard shortcut line helper
                auto DrawLine = [&](const std::wstring &key, const std::wstring &desc) {
                    RECT lineRect = {rc.left + padding, y, rc.right - padding, y + fontSize + 5};
                    SetTextColor(hdc, RGB(255, 204, 0));
                    DrawTextW(hdc, key.c_str(), -1, &lineRect, DT_LEFT);
                    SetTextColor(hdc, RGB(220, 220, 220));
                    lineRect.left += MulDiv(150, dpi, 96);
                    DrawTextW(hdc, desc.c_str(), -1, &lineRect, DT_LEFT);
                    y += fontSize + 3;
                };

                // Specialized helper for System Integration (Two-tone coloring)
                auto DrawSystemLine = [&](const std::wstring &label, const std::wstring &desc) {
                    RECT labelRect = {rc.left + padding, y, rc.right - padding, y + fontSize + 2};
                    SetTextColor(hdc, RGB(100, 200, 255)); // Blue Label
                    DrawTextW(hdc, label.c_str(), -1, &labelRect, DT_LEFT);

                    RECT descRect = {rc.left + padding + MulDiv(100, dpi, 96), y, rc.right - padding, y + fontSize + 5};
                    SetTextColor(hdc, RGB(200, 200, 200)); // Grey Description
                    DrawTextW(hdc, desc.c_str(), -1, &descRect, DT_LEFT);
                    y += fontSize + 3;
                };

                // Draw Header
                SetTextColor(hdc, RGB(100, 200, 255));
                DrawTextW(hdc, fullTitle.c_str(), -1, &rc, DT_CENTER);
                y += fontSize / 2;

                SetTextColor(hdc, RGB(100, 200, 255));
                RECT sysRect2 = {rc.left + padding, y, rc.right - padding, rc.bottom};
                DrawTextW(hdc, L"Keyboard and mouse shortcuts :", -1, &sysRect2, DT_LEFT);
                y += fontSize;

                // Render shortcuts
                DrawLine(L"Left/Right", L": Previous / Next image in folder");
                DrawLine(L"Space/Shift+Space", L": Next / Previous image in folder");
                DrawLine(L"Wheel Scroll", L": Next / Previous image in folder");
                DrawLine(L"E / Tab", L": Open file location in Windows Explorer");
                DrawLine(L"R / Ctrl+R", L": Rotate image 90° Clockwise / Counter");
                DrawLine(L"H", L": Flip image Horizontally");
                DrawLine(L"V", L": Flip image Vertically");
                y += fontSize / 2;
                DrawLine(L"Shift + Wheel", L": Adjust Window/Image Opacity");
                y += fontSize / 2;
                DrawLine(L"Up/Down", L": Zoom image In / Out");
                DrawLine(L"Ctrl + Wheel", L": Zoom image In / Out");
                DrawLine(L"Numpad 0", L": Reset zoom and opacity and center image");
                DrawLine(L"Right Click", L": 2x Quick zoom and toggle pan");
                y += fontSize / 2;
                DrawLine(L"F11 / Enter", L": Toggle Fullscreen mode");
                DrawLine(L"Left Click Drag", L": Pan image view");
                DrawLine(L"Middle Drag", L": Resize window dimensions");
                DrawLine(L"Middle Click", L": Reset window and center image");
                y += fontSize / 2; // Add a small spacer
                DrawLine(L"RMB + Wheel", L": Zoom In / Out");
                DrawLine(L"Horizontal Wheel", L": Adjust Window Opacity");
                DrawLine(L"RMB + Horizontal Wheel", L": Resize Window from Center");
                DrawLine(L"RMB + Left Click", L": Open current image location in Explorer");
                DrawLine(L"N", L": Toggle overlay info (Index/Filename)"); // NEW
                y += fontSize / 2; // Add a small spacer
                DrawLine(L"Ctrl+N", L": Open new viewer instance");
                DrawLine(L"Esc / Ctrl+W", L": Hide window to background");
                DrawLine(L"Ctrl+Q", L": Quit application process and kill background process");
                DrawLine(L"F1", L": Toggle help menu");
                DrawLine(L"ESC", L": Hide help menu");

                y += fontSize + 1;
                SetTextColor(hdc, RGB(100, 200, 255));
                RECT sysRect = {rc.left + padding, y, rc.right - padding, rc.bottom};
                DrawTextW(hdc, L"System Integration & Healing:", -1, &sysRect, DT_LEFT);
                y += fontSize + 10;

                // Render colored System Integration lines
                DrawSystemLine(L"Self-Healing:", L": Auto checks self path on every launch, updates windows registry");
                DrawSystemLine(L"Auto-Start:", L": Enabled (runs hidden in background/cached, faster image show)");
                DrawSystemLine(L"Associations:", L": Auto-checks associations common image formats on every launch");
                DrawSystemLine(L"Cache:", L": Caches last 30 images in VRAM/RAM and preload next/previous image in folder");
                DrawSystemLine(L"Move instructions:", L": Quit all instances Ctrl+Q, check Task Manager/background app/ EndTask");
                DrawSystemLine(L"Relocation:", L": After moving QIV.exe, kill all background instances, run(exec) once to update registry");

                // --- FOOTER: Name and Copyright ---
                int footerSize = MulDiv(14, dpi, 96);
                HFONT hFooterFont = CreateFontW(footerSize, 0, 0, 0, FW_LIGHT, FALSE, FALSE, FALSE,
                                                DEFAULT_CHARSET, OUT_OUTLINE_PRECIS,
                                                CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                                VARIABLE_PITCH, L"Segoe UI");
                SelectObject(hdc, hFooterFont);
                SetTextColor(hdc, RGB(120, 120, 120));

                std::wstring footer = std::wstring(L" ") + Constants::APP_CREATOR + L" | " + Constants::APP_HELP_FOOTER;
                RECT footerRect = {rc.left, rc.bottom - padding, rc.right, rc.bottom};
                DrawTextW(hdc, footer.c_str(), -1, &footerRect, DT_CENTER | DT_BOTTOM);

                SelectObject(hdc, hOldFont);
                DeleteObject(hFont);
                DeleteObject(hFooterFont);
                EndPaint(hWnd, &ps);
                return 0;
            }

            case WM_KEYDOWN:
                if (wParam == Shortcuts::SC_LOCAL_HIDE || wParam == Shortcuts::SC_PANEL_HELP_TOGGLE)
                    ShowWindow(hWnd, SW_HIDE);
                return 0;

            case WM_CLOSE:
                ShowWindow(hWnd, SW_HIDE);
                return 0;
        }
        return DefWindowProcW(hWnd, message, wParam, lParam);
    }

    void ToggleHelpWindow() {
        if (!g_hHelpWnd) return;
        if (IsWindowVisible(g_hHelpWnd)) ShowWindow(g_hHelpWnd, SW_HIDE);
        else {
            HWND hParent = GetWindow(g_hHelpWnd, GW_OWNER);
            if (hParent) {
                RECT rcParent, rcHelp;
                GetWindowRect(hParent, &rcParent);
                GetWindowRect(g_hHelpWnd, &rcHelp);
                int x = rcParent.left + ((rcParent.right - rcParent.left) - (rcHelp.right - rcHelp.left)) / 2;
                int y = rcParent.top + ((rcParent.bottom - rcParent.top) - (rcHelp.bottom - rcHelp.top)) / 2;
                SetWindowPos(g_hHelpWnd, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);
            } else ShowWindow(g_hHelpWnd, SW_SHOW);
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
        int winW = MulDiv(640, dpi, 96);
        int winH = MulDiv(760, dpi, 96);

        g_hHelpWnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, wc.lpszClassName, Constants::APP_TASKBAR_NAME,
                                     WS_POPUP | WS_CAPTION | WS_BORDER, 0, 0, winW, winH, hParent, nullptr, hInstance,
                                     nullptr);

        BOOL darkMode = TRUE;
        DwmSetWindowAttribute(g_hHelpWnd, 20, &darkMode, sizeof(darkMode));
        DWORD corner = 2;
        DwmSetWindowAttribute(g_hHelpWnd, 33, &corner, sizeof(corner));
        COLORREF darkColor = RGB(24, 24, 24);
        DwmSetWindowAttribute(g_hHelpWnd, 35, &darkColor, sizeof(darkColor));
        SetWindowPos(g_hHelpWnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }
}
