#include "ThemedDialog.h"
#include <dwmapi.h>
#include <uxtheme.h>

#include "Shortcuts.h"
#include "../AppState.h"
#include "../Platform/ConstantsTheme.h"

namespace UI {
    // ── statics ──────────────────────────────────────────────────────────────────
    HWND ThemedDialog::s_hwnd = nullptr;
    HWND ThemedDialog::s_hwndText = nullptr;
    HWND ThemedDialog::s_hwndBtn1 = nullptr;
    HWND ThemedDialog::s_hwndBtn2 = nullptr;
    HWND ThemedDialog::s_hOwner = nullptr;
    HFONT ThemedDialog::s_font = nullptr;
    HBRUSH ThemedDialog::s_bgBrush = nullptr;
    int ThemedDialog::s_result = 0;
    bool ThemedDialog::s_running = false;

    static constexpr wchar_t kClass[] = L"QIV_ThemedDialog";

    // ── WndProc ──────────────────────────────────────────────────────────────────
    LRESULT CALLBACK ThemedDialog::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (msg == WM_XBUTTONDOWN) {
            if (s_hOwner) {
                SendMessageW(s_hOwner, WM_XBUTTONDOWN, wParam, lParam);
            }
            return 0;
        }

        switch (msg) {
            case WM_COMMAND:
                switch (LOWORD(wParam)) {
                    case IDOK: s_result = 1;
                        s_running = false;
                        return 0;
                    case IDCANCEL: s_result = 0;
                        s_running = false;
                        return 0;
                }
                break;
            case WM_CLOSE:
                s_result = 0;
                s_running = false;
                return 0;
            case WM_ERASEBKGND: {
                RECT rc;
                GetClientRect(hwnd, &rc);
                FillRect(reinterpret_cast<HDC>(wParam), &rc, s_bgBrush);
                return 1;
            }
            case WM_CTLCOLORSTATIC: {
                HDC hdc = reinterpret_cast<HDC>(wParam);
                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, app.isDarkThemed
                                      ? RGB(220, 220, 220)
                                      : GetSysColor(COLOR_WINDOWTEXT));
                return reinterpret_cast<LRESULT>(s_bgBrush);
            }
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    // ── ApplyTheme ───────────────────────────────────────────────────────────────
    void ThemedDialog::ApplyTheme() {
        // Background color follows the same formula as panel backgrounds
        COLORREF bgColor;

        if (app.isDarkThemed) {
            // Only use your custom dark math when the app is actually in dark mode
            bgColor = Constants::Theme::ThemedGray(
                    Constants::Theme::Panel::BACKGROUND_INACTIVE * 2, app.themeFactor);
        } else {
            // Use the native system dialog color for light mode.
            // This will perfectly match the OS title bar and standard UI controls.
            bgColor = GetSysColor(COLOR_3DFACE);
        }
        if (s_bgBrush) {
            DeleteObject(s_bgBrush);
            s_bgBrush = nullptr;
        }
        s_bgBrush = CreateSolidBrush(bgColor);

        BOOL dark = app.isDarkThemed ? TRUE : FALSE;
        DwmSetWindowAttribute(s_hwnd, Constants::DWMWA_DARK_MODE, &dark, sizeof(dark));
        DwmSetWindowAttribute(s_hwnd, Constants::DWMWA_WINDOW_CORNER_PREFERENCES, &app.cornerPreference, sizeof(app.cornerPreference));
        // DWMWA_CAPTION_COLOR_ATTR intentionally not set — DWMWA_DARK_MODE already
        // gives the native dark title bar; overriding the caption color makes it
        // indistinguishable from the body.

        // Push the dark/light visual style onto the button controls so they render
        // correctly — SetPreferredAppMode covers menus but not child controls.
        const wchar_t *btnTheme = app.isDarkThemed ? L"DarkMode_Explorer" : L"";
        if (s_hwndBtn1) SetWindowTheme(s_hwndBtn1, btnTheme, nullptr);
        if (s_hwndBtn2) SetWindowTheme(s_hwndBtn2, btnTheme, nullptr);
    }

    // ── Init ─────────────────────────────────────────────────────────────────────
    void ThemedDialog::Init(HWND hMainWnd) {
        auto hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hMainWnd, GWLP_HINSTANCE));

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = WndProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr; // painted manually in WM_ERASEBKGND
        wc.lpszClassName = kClass;
        RegisterClassExW(&wc);

        s_hwnd = CreateWindowExW(
                WS_EX_DLGMODALFRAME,
                kClass, L"",
                WS_POPUP | WS_CAPTION | WS_SYSMENU,
                0, 0, 1, 1, // sized properly in RunModal
                hMainWnd, nullptr, hInst, nullptr);

        ApplyTheme();

        s_hwndText = CreateWindowExW(0, L"STATIC", L"",
                                     WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX | SS_EDITCONTROL,
                                     0, 0, 0, 0, s_hwnd, nullptr, hInst, nullptr);
        SetWindowTheme(s_hwndText, L"", L"");
        s_hwndBtn1 = CreateWindowExW(0, L"BUTTON", L"OK",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                     0, 0, 0, 0, s_hwnd, reinterpret_cast<HMENU>(IDOK), hInst, nullptr);

        s_hwndBtn2 = CreateWindowExW(0, L"BUTTON", L"No",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                     0, 0, 0, 0, s_hwnd, reinterpret_cast<HMENU>(IDCANCEL), hInst, nullptr);
    }

    // ── RunModal ─────────────────────────────────────────────────────────────────
    int ThemedDialog::RunModal(HWND hOwner, const wchar_t *text, const wchar_t *caption, bool confirm) {
        ApplyTheme();
        SetWindowTextW(s_hwnd, caption);
        SetWindowTextW(s_hwndText, text);
        s_hOwner = hOwner; // <--- Capture the owner here
        UINT dpi = GetDpiForWindow(hOwner ? hOwner : GetDesktopWindow());
        auto sc = [dpi](int logical) {
            return MulDiv(logical, dpi, 96);
        };

        // Desired CLIENT dimensions (logical px at 96 DPI)
        const int CW = 400, CH = 160, M = 16, BW = 80, BH = 28, BG = 8;
        int cw = sc(CW), ch = sc(CH);
        int m = sc(M), bw = sc(BW), bh = sc(BH), bg = sc(BG);

        // Expand to WINDOW size that yields exactly cw×ch client area
        RECT wr = {0, 0, cw, ch};
        AdjustWindowRectExForDpi(&wr,
                                 static_cast<DWORD>(GetWindowLongW(s_hwnd, GWL_STYLE)),
                                 FALSE,
                                 static_cast<DWORD>(GetWindowLongW(s_hwnd, GWL_EXSTYLE)),
                                 dpi);
        const int winW = wr.right - wr.left;
        const int winH = wr.bottom - wr.top;
        SetWindowPos(s_hwnd, nullptr, 0, 0, winW, winH, SWP_NOMOVE | SWP_NOZORDER);

        // All control positions are CLIENT coordinates using cw/ch
        SetWindowPos(s_hwndText, nullptr, m, m, cw - 2 * m, ch - sc(60), SWP_NOZORDER);

        const int by = ch - m - bh;
        if (confirm) {
            ShowWindow(s_hwndBtn2, SW_SHOW);
            SetWindowTextW(s_hwndBtn1, L"Yes");
            SetWindowTextW(s_hwndBtn2, L"No");
            SetWindowPos(s_hwndBtn2, nullptr, cw - m - bw, by, bw, bh, SWP_NOZORDER);
            SetWindowPos(s_hwndBtn1, nullptr, cw - m - bw - bg - bw, by, bw, bh, SWP_NOZORDER);
        } else {
            ShowWindow(s_hwndBtn2, SW_HIDE);
            SetWindowTextW(s_hwndBtn1, L"OK");
            SetWindowPos(s_hwndBtn1, nullptr, cw - m - bw, by, bw, bh, SWP_NOZORDER);
        }

        // DPI-correct system dialog font
        if (s_font) {
            DeleteObject(s_font);
            s_font = nullptr;
        }
        NONCLIENTMETRICSW ncm{};
        ncm.cbSize = sizeof(ncm);
        SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi);
        s_font = CreateFontIndirectW(&ncm.lfMessageFont);
        SendMessageW(s_hwndText, WM_SETFONT, reinterpret_cast<WPARAM>(s_font), TRUE);
        SendMessageW(s_hwndBtn1, WM_SETFONT, reinterpret_cast<WPARAM>(s_font), TRUE);
        SendMessageW(s_hwndBtn2, WM_SETFONT, reinterpret_cast<WPARAM>(s_font), TRUE);

        // Center on owner
        if (hOwner) {
            RECT r;
            GetWindowRect(hOwner, &r);
            int cx = r.left + (r.right - r.left - winW) / 2;
            int cy = r.top + (r.bottom - r.top - winH) / 2;
            SetWindowPos(s_hwnd, HWND_TOP, cx, cy, 0, 0, SWP_NOSIZE);
        }

        s_result = 0;
        s_running = true;
        if (hOwner) EnableWindow(hOwner, FALSE);
        ShowWindow(s_hwnd, SW_SHOW);
        SetForegroundWindow(s_hwnd);

        MSG msg;
        while (s_running) {
            if (GetMessageW(&msg, nullptr, 0, 0) <= 0) break;

            // Intercept Right Shift specifically using the scan code
            if (msg.message == WM_KEYDOWN || msg.message == WM_KEYUP) {
                BYTE scanCode = (msg.lParam >> 16) & 0xFF;

                if (msg.wParam == VK_SHIFT && scanCode == Shortcuts::SC_RIGHT_SHIFT_SCANCODE) {
                    if (s_hOwner) {
                        // Pass the message exactly as it is to the owner
                        SendMessageW(s_hOwner, msg.message, msg.wParam, msg.lParam);
                    }
                    // 'continue' prevents IsDialogMessageW from seeing this key
                    continue;
                }
            }

            if (!IsDialogMessageW(s_hwnd, &msg)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }

        ShowWindow(s_hwnd, SW_HIDE);
        if (hOwner) {
            EnableWindow(hOwner, TRUE);
            SetForegroundWindow(hOwner);
        }
        return s_result;
    }

    // ── Public API ───────────────────────────────────────────────────────────────
    bool ThemedDialog::Confirm(HWND hOwner, const wchar_t *text, const wchar_t *caption) {
        return RunModal(hOwner, text, caption, true) == 1;
    }

    void ThemedDialog::Message(HWND hOwner, const wchar_t *text, const wchar_t *caption) {
        RunModal(hOwner, text, caption, false);
    }
} // namespace UI
