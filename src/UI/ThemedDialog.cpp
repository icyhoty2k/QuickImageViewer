#include "ThemedDialog.h"
#include <climits>
#include <cfloat>
#include <dwmapi.h>
#include <uxtheme.h>

#include "Shortcuts.h"
#include "../Common/Converters.h"
#include "../AppState.h"
#include "../Platform/ConstantsTheme.h"
#include "../Platform/Constants.h"

namespace UI {

    static constexpr wchar_t kClass[]   = L"QIV_ThemedDialog";
    static constexpr int ID_RESET_DEFAULT = 103;

    // ── statics ───────────────────────────────────────────────────────────────────
    int    ThemedDialog::s_result       = 0;
    bool   ThemedDialog::s_running      = false;
    HWND   ThemedDialog::s_hOwner       = nullptr;
    HFONT  ThemedDialog::s_font         = nullptr;
    HBRUSH ThemedDialog::s_bgBrush      = nullptr;
    HBRUSH ThemedDialog::s_editBrush    = nullptr;
    HBRUSH ThemedDialog::s_errorBrush   = nullptr;
    int    ThemedDialog::s_intMin       = INT_MIN;
    int    ThemedDialog::s_intMax       = INT_MAX;
    int    ThemedDialog::s_defaultValue = 0;

    float  ThemedDialog::s_floatMin     = -FLT_MAX;
    float  ThemedDialog::s_floatMax     = FLT_MAX;
    float  ThemedDialog::s_floatDefault = 0.0f;
    bool   ThemedDialog::s_isFloat      = false;
    bool   ThemedDialog::s_isFloatError = false;
    bool   ThemedDialog::s_isIntError   = false;
    std::wstring ThemedDialog::s_floatLabel;
    std::wstring ThemedDialog::s_intLabel;

    HWND ThemedDialog::s_hwndMsg   = nullptr;
    HWND ThemedDialog::s_textMsg   = nullptr;
    HWND ThemedDialog::s_btnMsgOk  = nullptr;

    HWND ThemedDialog::s_hwndCfm   = nullptr;
    HWND ThemedDialog::s_textCfm   = nullptr;
    HWND ThemedDialog::s_btnCfmYes = nullptr;
    HWND ThemedDialog::s_btnCfmNo  = nullptr;

    HWND ThemedDialog::s_hwndInt      = nullptr;
    HWND ThemedDialog::s_textInt      = nullptr;
    HWND ThemedDialog::s_editInt      = nullptr;
    HWND ThemedDialog::s_btnIntOk     = nullptr;
    HWND ThemedDialog::s_btnIntCancel = nullptr;
    HWND ThemedDialog::s_btnIntReset  = nullptr;

    // ── WndProc ───────────────────────────────────────────────────────────────────
    LRESULT CALLBACK ThemedDialog::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (msg == WM_XBUTTONDOWN) {
            if (s_hOwner) SendMessageW(s_hOwner, WM_XBUTTONDOWN, wParam, lParam);
            return 0;
        }
        switch (msg) {
            case WM_COMMAND:
                if (HIWORD(wParam) == EN_UPDATE && reinterpret_cast<HWND>(lParam) == s_editInt) {
                    if (s_isFloatError) {
                        s_isFloatError = false;
                        SetWindowTextW(s_textInt, s_floatLabel.c_str());
                        InvalidateRect(s_editInt, nullptr, TRUE);
                    }
                    if (s_isIntError) {
                        s_isIntError = false;
                        SetWindowTextW(s_textInt, s_intLabel.c_str());
                        InvalidateRect(s_editInt, nullptr, TRUE);
                    }
                }
                switch (LOWORD(wParam)) {
                    case IDOK:
                        if (hwnd == s_hwndInt) {
                            if (s_isFloat) {
                                wchar_t buf[32]{};
                                GetWindowTextW(s_editInt, buf, 32);
                                float v = static_cast<float>(_wtof(buf));
                                if (v < s_floatMin || v > s_floatMax) {
                                    s_isFloatError = true;
                                    wchar_t errBuf[64];
                                    swprintf_s(errBuf, L"Out of range : (%.2f to %.2f)", s_floatMin, s_floatMax);
                                    SetWindowTextW(s_textInt, errBuf);
                                    InvalidateRect(s_editInt, nullptr, TRUE);
                                    return 0;
                                }
                                s_isFloatError = false;
                                SetWindowTextW(s_textInt, s_floatLabel.c_str());
                                s_result = Converters::toZoomInt(v);
                            } else {
                                wchar_t buf[16]{};
                                GetWindowTextW(s_editInt, buf, 16);
                                int v = _wtoi(buf);
                                if (v < s_intMin || v > s_intMax) {
                                    s_isIntError = true;
                                    wchar_t errBuf[64];
                                    swprintf_s(errBuf, L"Out of range : (%d to %d)", s_intMin, s_intMax);
                                    SetWindowTextW(s_textInt, errBuf);
                                    InvalidateRect(s_editInt, nullptr, TRUE);
                                    return 0;
                                }
                                s_isIntError = false;
                                SetWindowTextW(s_textInt, s_intLabel.c_str());
                                s_result = v;
                            }
                        } else {
                            s_result = 1;
                        }
                        s_running = false;
                        return 0;
                    case IDCANCEL:
                        s_result = -1;
                        s_running = false;
                        return 0;
                    case ID_RESET_DEFAULT: {
                        wchar_t buf[16]{};
                        if (s_isFloat)
                            swprintf_s(buf, L"%.2f", s_floatDefault);
                        else
                            swprintf_s(buf, L"%d", s_defaultValue);
                        SetWindowTextW(s_editInt, buf);
                        SendMessageW(s_editInt, EM_SETSEL, 0, -1);
                        return 0;
                    }
                }
                break;
            case WM_CLOSE:
                s_result = -1;
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
                SetTextColor(hdc, app.isDarkThemed ? RGB(220, 220, 220) : GetSysColor(COLOR_WINDOWTEXT));
                return reinterpret_cast<LRESULT>(s_bgBrush);
            }
            case WM_CTLCOLOREDIT: {
                HDC hdc = reinterpret_cast<HDC>(wParam);
                if ((s_isFloatError || s_isIntError) && s_errorBrush) {
                    SetBkColor(hdc, Constants::Theme::Markers::CRITICAL);
                    SetTextColor(hdc, Constants::Theme::Markers::ERR);
                    return reinterpret_cast<LRESULT>(s_errorBrush);
                }
                if (app.isDarkThemed && s_editBrush) {
                    SetBkColor(hdc, RGB(40, 40, 40));
                    SetTextColor(hdc, RGB(220, 220, 220));
                    return reinterpret_cast<LRESULT>(s_editBrush);
                }
                break;
            }
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    // ── ApplyTheme ────────────────────────────────────────────────────────────────
    void ThemedDialog::ApplyTheme() {
        COLORREF bgColor = app.isDarkThemed
            ? Constants::Theme::ThemedGray(Constants::Theme::Panel::BACKGROUND_INACTIVE * 2, app.themeFactor)
            : GetSysColor(COLOR_3DFACE);

        if (s_bgBrush)    { DeleteObject(s_bgBrush);    s_bgBrush    = nullptr; }
        if (s_editBrush)  { DeleteObject(s_editBrush);  s_editBrush  = nullptr; }
        if (s_errorBrush) { DeleteObject(s_errorBrush); s_errorBrush = nullptr; }
        s_bgBrush = CreateSolidBrush(bgColor);
        s_errorBrush = CreateSolidBrush(Constants::Theme::Markers::CRITICAL);
        if (app.isDarkThemed) s_editBrush = CreateSolidBrush(RGB(40, 40, 40));

        BOOL dark = app.isDarkThemed ? TRUE : FALSE;
        const wchar_t *btnTheme = app.isDarkThemed ? L"DarkMode_Explorer" : L"";

        for (HWND w : { s_hwndMsg, s_hwndCfm, s_hwndInt }) {
            if (!w) continue;
            DwmSetWindowAttribute(w, Constants::DWMWA_DARK_MODE, &dark, sizeof(dark));
            DwmSetWindowAttribute(w, Constants::DWMWA_WINDOW_CORNER_PREFERENCES,
                                  &app.cornerPreference, sizeof(app.cornerPreference));
        }
        if (s_btnMsgOk)     SetWindowTheme(s_btnMsgOk,     btnTheme, nullptr);
        if (s_btnCfmYes)    SetWindowTheme(s_btnCfmYes,    btnTheme, nullptr);
        if (s_btnCfmNo)     SetWindowTheme(s_btnCfmNo,     btnTheme, nullptr);
        if (s_btnIntOk)     SetWindowTheme(s_btnIntOk,     btnTheme, nullptr);
        if (s_btnIntCancel) SetWindowTheme(s_btnIntCancel, btnTheme, nullptr);
        if (s_btnIntReset)  SetWindowTheme(s_btnIntReset,  btnTheme, nullptr);
        if (s_editInt)      SetWindowTheme(s_editInt,      btnTheme, nullptr);
    }

    // ── Init ─────────────────────────────────────────────────────────────────────
    void ThemedDialog::Init(HWND hMainWnd) {
        auto hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hMainWnd, GWLP_HINSTANCE));

        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = WndProc;
        wc.hInstance     = hInst;
        wc.hCursor       = Constants::Cursors::CURR_DEFAULT;
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kClass;
        RegisterClassExW(&wc);

        auto makeShell = [&]() -> HWND {
            return CreateWindowExW(WS_EX_DLGMODALFRAME, kClass, L"",
                                   WS_POPUP | WS_CAPTION | WS_SYSMENU,
                                   0, 0, 1, 1, hMainWnd, nullptr, hInst, nullptr);
        };
        auto makeStatic = [&](HWND parent) -> HWND {
            HWND h = CreateWindowExW(0, L"STATIC", L"",
                                     WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX | SS_EDITCONTROL,
                                     0, 0, 0, 0, parent, nullptr, hInst, nullptr);
            SetWindowTheme(h, L"", L"");
            return h;
        };
        auto makeBtn = [&](HWND parent, const wchar_t *label, int id, DWORD extra = 0) -> HWND {
            return CreateWindowExW(0, L"BUTTON", label,
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | extra,
                                   0, 0, 0, 0, parent,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hInst, nullptr);
        };

        // Window 1: Message  [OK]
        s_hwndMsg  = makeShell();
        s_textMsg  = makeStatic(s_hwndMsg);
        s_btnMsgOk = makeBtn(s_hwndMsg, L"OK", IDOK, BS_DEFPUSHBUTTON);

        // Window 2: Confirm  [Yes] [No]
        s_hwndCfm   = makeShell();
        s_textCfm   = makeStatic(s_hwndCfm);
        s_btnCfmYes = makeBtn(s_hwndCfm, L"Yes", IDOK, BS_DEFPUSHBUTTON);
        s_btnCfmNo  = makeBtn(s_hwndCfm, L"No",  IDCANCEL);

        // Window 3: PromptInt  [Reset Default]  [Cancel] [OK]
        s_hwndInt      = makeShell();
        s_textInt      = makeStatic(s_hwndInt);
        s_editInt      = CreateWindowExW(0, L"EDIT", L"",
                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_NUMBER | ES_CENTER,
                                         0, 0, 0, 0, s_hwndInt, nullptr, hInst, nullptr);
        s_btnIntReset  = makeBtn(s_hwndInt, L"Reset Default", ID_RESET_DEFAULT);
        s_btnIntCancel = makeBtn(s_hwndInt, L"Cancel",        IDCANCEL);
        s_btnIntOk     = makeBtn(s_hwndInt, L"OK",            IDOK, BS_DEFPUSHBUTTON);

        ApplyTheme();
    }

    // ── RunModalLoop ──────────────────────────────────────────────────────────────
    void ThemedDialog::RunModalLoop(HWND hwndDlg, HWND hOwner, HWND focusCtrl) {
        if (hOwner) {
            RECT r, wr;
            GetWindowRect(hOwner, &r);
            GetWindowRect(hwndDlg, &wr);
            int dw = wr.right - wr.left, dh = wr.bottom - wr.top;
            SetWindowPos(hwndDlg, HWND_TOP,
                         r.left + (r.right - r.left - dw) / 2,
                         r.top  + (r.bottom - r.top  - dh) / 2,
                         0, 0, SWP_NOSIZE);
        }
        s_result  = 0;
        s_running = true;
        s_hOwner  = hOwner;
        if (hOwner) EnableWindow(hOwner, FALSE);
        ShowWindow(hwndDlg, SW_SHOW);
        SetForegroundWindow(hwndDlg);
        if (focusCtrl) {
            SetFocus(focusCtrl);
            SendMessageW(focusCtrl, EM_SETSEL, 0, -1);
        }

        MSG msg;
        while (s_running) {
            if (GetMessageW(&msg, nullptr, 0, 0) <= 0) break;
            if (msg.message == WM_KEYDOWN || msg.message == WM_KEYUP) {
                BYTE scanCode = static_cast<BYTE>((msg.lParam >> 16) & 0xFF);
                if (msg.wParam == VK_SHIFT && scanCode == Shortcuts::SC_RIGHT_SHIFT_SCANCODE) {
                    if (s_hOwner) SendMessageW(s_hOwner, msg.message, msg.wParam, msg.lParam);
                    continue;
                }
            }
            if (!IsDialogMessageW(hwndDlg, &msg)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }

        if (hOwner) EnableWindow(hOwner, TRUE); // BEFORE SW_HIDE to avoid flicker
        ShowWindow(hwndDlg, SW_HIDE);
        if (hOwner) SetForegroundWindow(hOwner);
    }

    // ── Message ───────────────────────────────────────────────────────────────────
    void ThemedDialog::Message(HWND hOwner, const wchar_t *text, const wchar_t *caption) {
        ApplyTheme();
        SetWindowTextW(s_hwndMsg, caption);
        SetWindowTextW(s_textMsg, text);

        UINT dpi = GetDpiForWindow(hOwner ? hOwner : GetDesktopWindow());
        auto sc = [dpi](int l) { return MulDiv(l, dpi, 96); };

        const int CW = 400, CH = 160, M = 16, BW = 80, BH = 28;
        int cw = sc(CW), ch = sc(CH), m = sc(M), bw = sc(BW), bh = sc(BH);

        RECT wr = {0, 0, cw, ch};
        AdjustWindowRectExForDpi(&wr,
                                 static_cast<DWORD>(GetWindowLongW(s_hwndMsg, GWL_STYLE)), FALSE,
                                 static_cast<DWORD>(GetWindowLongW(s_hwndMsg, GWL_EXSTYLE)), dpi);
        SetWindowPos(s_hwndMsg, nullptr, 0, 0, wr.right - wr.left, wr.bottom - wr.top, SWP_NOMOVE | SWP_NOZORDER);

        SetWindowPos(s_textMsg,  nullptr, m, m, cw - 2 * m, ch - sc(60), SWP_NOZORDER);
        SetWindowPos(s_btnMsgOk, nullptr, cw - m - bw, ch - m - bh, bw, bh, SWP_NOZORDER);

        if (s_font) { DeleteObject(s_font); s_font = nullptr; }
        NONCLIENTMETRICSW ncm{}; ncm.cbSize = sizeof(ncm);
        SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi);
        s_font = CreateFontIndirectW(&ncm.lfMessageFont);
        SendMessageW(s_textMsg,  WM_SETFONT, reinterpret_cast<WPARAM>(s_font), TRUE);
        SendMessageW(s_btnMsgOk, WM_SETFONT, reinterpret_cast<WPARAM>(s_font), TRUE);

        RunModalLoop(s_hwndMsg, hOwner);
    }

    // ── Confirm ───────────────────────────────────────────────────────────────────
    bool ThemedDialog::Confirm(HWND hOwner, const wchar_t *text, const wchar_t *caption) {
        ApplyTheme();
        SetWindowTextW(s_hwndCfm, caption);
        SetWindowTextW(s_textCfm, text);

        UINT dpi = GetDpiForWindow(hOwner ? hOwner : GetDesktopWindow());
        auto sc = [dpi](int l) { return MulDiv(l, dpi, 96); };

        const int CW = 400, CH = 160, M = 16, BW = 80, BH = 28, BG = 8;
        int cw = sc(CW), ch = sc(CH), m = sc(M), bw = sc(BW), bh = sc(BH), bg = sc(BG);

        RECT wr = {0, 0, cw, ch};
        AdjustWindowRectExForDpi(&wr,
                                 static_cast<DWORD>(GetWindowLongW(s_hwndCfm, GWL_STYLE)), FALSE,
                                 static_cast<DWORD>(GetWindowLongW(s_hwndCfm, GWL_EXSTYLE)), dpi);
        SetWindowPos(s_hwndCfm, nullptr, 0, 0, wr.right - wr.left, wr.bottom - wr.top, SWP_NOMOVE | SWP_NOZORDER);

        int by = ch - m - bh;
        SetWindowPos(s_textCfm,   nullptr, m, m, cw - 2 * m, ch - sc(60), SWP_NOZORDER);
        SetWindowPos(s_btnCfmNo,  nullptr, cw - m - bw,           by, bw, bh, SWP_NOZORDER);
        SetWindowPos(s_btnCfmYes, nullptr, cw - m - bw - bg - bw, by, bw, bh, SWP_NOZORDER);

        if (s_font) { DeleteObject(s_font); s_font = nullptr; }
        NONCLIENTMETRICSW ncm{}; ncm.cbSize = sizeof(ncm);
        SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi);
        s_font = CreateFontIndirectW(&ncm.lfMessageFont);
        SendMessageW(s_textCfm,   WM_SETFONT, reinterpret_cast<WPARAM>(s_font), TRUE);
        SendMessageW(s_btnCfmYes, WM_SETFONT, reinterpret_cast<WPARAM>(s_font), TRUE);
        SendMessageW(s_btnCfmNo,  WM_SETFONT, reinterpret_cast<WPARAM>(s_font), TRUE);

        RunModalLoop(s_hwndCfm, hOwner);
        return s_result == 1;
    }

    // ── PromptInt ─────────────────────────────────────────────────────────────────
    int ThemedDialog::PromptInt(HWND hOwner, const wchar_t *caption, const wchar_t *label,
                                int currentValue, int minVal, int maxVal, int defaultValue) {
        s_intMin       = minVal;
        s_intMax       = maxVal;
        s_defaultValue = defaultValue;
        s_isIntError   = false;
        s_intLabel     = label;

        int digits = 1;
        for (int v = maxVal; v >= 10; v /= 10) ++digits;
        SendMessageW(s_editInt, EM_SETLIMITTEXT, static_cast<WPARAM>(digits), 0);

        ApplyTheme();
        SetWindowTextW(s_hwndInt, caption);
        SetWindowTextW(s_textInt, label);

        wchar_t initBuf[16];
        swprintf_s(initBuf, L"%d", currentValue);
        SetWindowTextW(s_editInt, initBuf);

        UINT dpi = GetDpiForWindow(hOwner ? hOwner : GetDesktopWindow());
        auto sc = [dpi](int l) { return MulDiv(l, dpi, 96); };

        if (s_font) { DeleteObject(s_font); s_font = nullptr; }
        NONCLIENTMETRICSW ncm{}; ncm.cbSize = sizeof(ncm);
        SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi);
        s_font = CreateFontIndirectW(&ncm.lfMessageFont);

        int fontH = sc(13);
        {
            HDC hdc = GetDC(s_hwndInt);
            HFONT old = static_cast<HFONT>(SelectObject(hdc, s_font));
            TEXTMETRICW tm{};
            GetTextMetricsW(hdc, &tm);
            fontH = tm.tmHeight;
            SelectObject(hdc, old);
            ReleaseDC(s_hwndInt, hdc);
        }

        const int M = 16, BW = 80, BH = 28, BG = 8, BW3 = 110, LABEL_LINES = 2;
        int m = sc(M), bw = sc(BW), bh = sc(BH), bg = sc(BG), bw3 = sc(BW3);
        int lh = (fontH + sc(4)) * LABEL_LINES;
        int eh = fontH + sc(4) + 2;
        int cw = sc(360);
        int ch = m + lh + sc(10) + eh + sc(12) + bh + m;

        RECT wr = {0, 0, cw, ch};
        AdjustWindowRectExForDpi(&wr,
                                 static_cast<DWORD>(GetWindowLongW(s_hwndInt, GWL_STYLE)), FALSE,
                                 static_cast<DWORD>(GetWindowLongW(s_hwndInt, GWL_EXSTYLE)), dpi);
        SetWindowPos(s_hwndInt, nullptr, 0, 0, wr.right - wr.left, wr.bottom - wr.top, SWP_NOMOVE | SWP_NOZORDER);

        SetWindowPos(s_textInt, nullptr, m, m,              cw - 2 * m, lh, SWP_NOZORDER);
        SetWindowPos(s_editInt, nullptr, m, m + lh + sc(10), cw - 2 * m, eh, SWP_NOZORDER);

        const int by = ch - m - bh;
        // [Reset Default] flush left — [Cancel][OK] flush right
        SetWindowPos(s_btnIntReset,  nullptr, m,                      by, bw3, bh, SWP_NOZORDER);
        SetWindowPos(s_btnIntCancel, nullptr, cw - m - bw,            by, bw,  bh, SWP_NOZORDER);
        SetWindowPos(s_btnIntOk,     nullptr, cw - m - bw - bg - bw,  by, bw,  bh, SWP_NOZORDER);

        SendMessageW(s_textInt,     WM_SETFONT, reinterpret_cast<WPARAM>(s_font), TRUE);
        SendMessageW(s_editInt,     WM_SETFONT, reinterpret_cast<WPARAM>(s_font), TRUE);
        SendMessageW(s_btnIntOk,    WM_SETFONT, reinterpret_cast<WPARAM>(s_font), TRUE);
        SendMessageW(s_btnIntCancel,WM_SETFONT, reinterpret_cast<WPARAM>(s_font), TRUE);
        SendMessageW(s_btnIntReset, WM_SETFONT, reinterpret_cast<WPARAM>(s_font), TRUE);

        RunModalLoop(s_hwndInt, hOwner, s_editInt);
        return s_result;
    }

    // ── PromptFloat ──────────────────────────────────────────────────────────────
    int ThemedDialog::PromptFloat(HWND hOwner, const wchar_t *caption, const wchar_t *label,
                                  float currentValue, float minVal, float maxVal, float defaultValue) {
        s_isFloat = true;
        s_floatMin = minVal;
        s_floatMax = maxVal;
        s_floatDefault = defaultValue;
        s_isFloatError = false;
        s_floatLabel = label;

            if (!s_errorBrush) s_errorBrush = CreateSolidBrush(Constants::Theme::Markers::CRITICAL);

        LONG_PTR style = GetWindowLongPtrW(s_editInt, GWL_STYLE);
        SetWindowLongPtrW(s_editInt, GWL_STYLE, style & ~ES_NUMBER);
        SendMessageW(s_editInt, EM_SETLIMITTEXT, 8, 0);

        ApplyTheme();
        SetWindowTextW(s_hwndInt, caption);
        SetWindowTextW(s_textInt, label);

        wchar_t initBuf[16];
        swprintf_s(initBuf, L"%.2f", currentValue);
        SetWindowTextW(s_editInt, initBuf);

        UINT dpi = GetDpiForWindow(hOwner ? hOwner : GetDesktopWindow());
        auto sc = [dpi](int l) { return MulDiv(l, dpi, 96); };

        if (s_font) { DeleteObject(s_font); s_font = nullptr; }
        NONCLIENTMETRICSW ncm{}; ncm.cbSize = sizeof(ncm);
        SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi);
        s_font = CreateFontIndirectW(&ncm.lfMessageFont);

        int fontH = sc(13);
        {
            HDC hdc = GetDC(s_hwndInt);
            HFONT old = static_cast<HFONT>(SelectObject(hdc, s_font));
            TEXTMETRICW tm{};
            GetTextMetricsW(hdc, &tm);
            fontH = tm.tmHeight;
            SelectObject(hdc, old);
            ReleaseDC(s_hwndInt, hdc);
        }

        const int M = 16, BW = 80, BH = 28, BG = 8, BW3 = 110, LABEL_LINES = 2;
        int m = sc(M), bw = sc(BW), bh = sc(BH), bg = sc(BG), bw3 = sc(BW3);
        int lh = (fontH + sc(4)) * LABEL_LINES;
        int eh = fontH + sc(4) + 2;
        int cw = sc(360);
        int ch = m + lh + sc(10) + eh + sc(12) + bh + m;

        RECT wr = {0, 0, cw, ch};
        AdjustWindowRectExForDpi(&wr,
                                 static_cast<DWORD>(GetWindowLongW(s_hwndInt, GWL_STYLE)), FALSE,
                                 static_cast<DWORD>(GetWindowLongW(s_hwndInt, GWL_EXSTYLE)), dpi);
        SetWindowPos(s_hwndInt, nullptr, 0, 0, wr.right - wr.left, wr.bottom - wr.top, SWP_NOMOVE | SWP_NOZORDER);

        SetWindowPos(s_textInt, nullptr, m, m,              cw - 2 * m, lh, SWP_NOZORDER);
        SetWindowPos(s_editInt, nullptr, m, m + lh + sc(10), cw - 2 * m, eh, SWP_NOZORDER);

        const int by = ch - m - bh;
        SetWindowPos(s_btnIntReset,  nullptr, m,                      by, bw3, bh, SWP_NOZORDER);
        SetWindowPos(s_btnIntCancel, nullptr, cw - m - bw,            by, bw,  bh, SWP_NOZORDER);
        SetWindowPos(s_btnIntOk,     nullptr, cw - m - bw - bg - bw,  by, bw,  bh, SWP_NOZORDER);

        SendMessageW(s_textInt,      WM_SETFONT, reinterpret_cast<WPARAM>(s_font), TRUE);
        SendMessageW(s_editInt,      WM_SETFONT, reinterpret_cast<WPARAM>(s_font), TRUE);
        SendMessageW(s_btnIntOk,     WM_SETFONT, reinterpret_cast<WPARAM>(s_font), TRUE);
        SendMessageW(s_btnIntCancel, WM_SETFONT, reinterpret_cast<WPARAM>(s_font), TRUE);
        SendMessageW(s_btnIntReset,  WM_SETFONT, reinterpret_cast<WPARAM>(s_font), TRUE);

        RunModalLoop(s_hwndInt, hOwner, s_editInt);

        SetWindowLongPtrW(s_editInt, GWL_STYLE, style | ES_NUMBER);
        SendMessageW(s_editInt, EM_SETLIMITTEXT, 10, 0);
        s_isFloat = false;
        return s_result;
    }

} // namespace UI
