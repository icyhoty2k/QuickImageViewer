#include "ZoomWnd.h"
#include "../../AppState.h"
#include "../../Platform/Constants.h"
#include "../../Platform/ConstantsStrings.h"
#include "../../Overlays/OverlayManager.h"
#include "CustomControls/InputBox.h"
#include <cmath>

extern AppState app;

namespace UI {
    void ZoomWnd::Init(HINSTANCE hInstance, HWND hParent) {
        const int w = static_cast<int>(340.0f * app.dpiScale);
        const int h = static_cast<int>(140.0f * app.dpiScale);
        InitFloating(hInstance, hParent, L"QivZoomWndClass", L"Zoom to", w, h);

        m_inputBox.SetMaxLength(10);
        m_inputBox.OnChanged = [this](const std::wstring& t) {
            int len = static_cast<int>(t.size());
            wcsncpy_s(m_input, t.c_str(), len);
            m_input[len] = L'\0';
            m_inputLen   = len;
            m_outOfRange = false;
            InvalidateRect(m_hWnd, nullptr, FALSE);
        };
    }

    void ZoomWnd::Init(HINSTANCE hInstance, HWND hParent, int8_t /*position*/) {
        Init(hInstance, hParent);
    }

    void ZoomWnd::Show() {
        if (!m_hWnd) return;
        m_inputBox.Reset();
        m_input[0]   = L'\0';
        m_inputLen   = 0;
        m_outOfRange = false;

        ShowCenterOverParent();
        InvalidateRect(m_hWnd, nullptr, FALSE);
    }

    void ZoomWnd::CommitZoom() {
        if (m_inputLen == 0) {
            Hide();
            return;
        }
        float percent = static_cast<float>(_wtof(m_input));
        if (percent < 0.1f || percent > 99999.0f) {
            m_outOfRange = true;
            InvalidateRect(m_hWnd, nullptr, FALSE);
            return;
        }
        Hide();
        app.viewport.zoom = percent / 100.0f;
        g_overlayManager.PostCenterMessage(m_hParent,
            std::wstring(Constants::Messages::ZOOM_TO_PREFIX) + L" " + m_input + L"%");
        InvalidateRect(m_hParent, nullptr, FALSE);
    }

    bool ZoomWnd::OnLocalHide() {
        switch (m_inputBox.RouteKey(VK_ESCAPE, m_hWnd)) {
            case InputResult::RequestClear:
                InvalidateRect(m_hWnd, nullptr, FALSE);
                return true;
            default:
                return false;
        }
    }

    bool ZoomWnd::OnKeyDown(WPARAM vk, bool ctrl, bool /*shift*/, bool alt) {
        (void)ctrl; (void)alt;

        if (vk == VK_RETURN) {
            CommitZoom();
            return true;
        }

        switch (m_inputBox.RouteKey(vk, m_hWnd)) {
            case InputResult::Ignored:         return false;
            case InputResult::RequestClose:    return false;
            case InputResult::RequestClear:    InvalidateRect(m_hWnd, nullptr, FALSE); return true;
            case InputResult::ConsumedRepaint: InvalidateRect(m_hWnd, nullptr, FALSE); return true;
            case InputResult::Consumed:        return true;
        }
        return true;
    }

    void ZoomWnd::EnsureBackBuffer(HDC refDC, int w, int h) {
        if (m_bbDC && w == m_bbW && h == m_bbH) return;
        DestroyBackBuffer();
        m_bbDC = CreateCompatibleDC(refDC);
        m_bbBmp = CreateCompatibleBitmap(refDC, w, h);
        m_bbBmpOld = static_cast<HBITMAP>(SelectObject(m_bbDC, m_bbBmp));
        m_bbW = w;
        m_bbH = h;
    }

    void ZoomWnd::DestroyBackBuffer() {
        if (m_bbDC) {
            if (m_bbBmpOld) SelectObject(m_bbDC, m_bbBmpOld);
            DeleteDC(m_bbDC);
            m_bbDC = nullptr;
        }
        if (m_bbBmp) {
            DeleteObject(m_bbBmp);
            m_bbBmp = nullptr;
        }
        m_bbBmpOld = nullptr;
        m_bbW = m_bbH = 0;
    }

    LRESULT ZoomWnd::HandlePanelMessage(UINT message, WPARAM wParam, LPARAM lParam) {
        if (message == WM_ERASEBKGND) return 1;
        switch (message) {
            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC screenDC = BeginPaint(m_hWnd, &ps);
                RECT rc;
                GetClientRect(m_hWnd, &rc);
                EnsureBackBuffer(screenDC, rc.right, rc.bottom);
                HDC hdc = m_bbDC;

                HBRUSH bgBrush = CreateSolidBrush(GetBgColor());
                FillRect(hdc, &rc, bgBrush);
                DeleteObject(bgBrush);
                SetBkMode(hdc, TRANSPARENT);

                const int pad = static_cast<int>(14.0f * app.dpiScale);
                const int gap = static_cast<int>(8.0f * app.dpiScale);
                const int fs = static_cast<int>(13.0f * app.dpiScale);
                const int fsIn = static_cast<int>(16.0f * app.dpiScale);

                const int dpiKey = static_cast<int>(app.dpiScale * 96);
                if (dpiKey != m_cachedFontDpi) {
                    if (m_hFont) {
                        DeleteObject(m_hFont);
                        m_hFont = nullptr;
                    }
                    if (m_hFontInput) {
                        DeleteObject(m_hFontInput);
                        m_hFontInput = nullptr;
                    }
                    m_hFont = CreateFontW(-fs, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                          CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                          DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
                    m_hFontInput = CreateFontW(-fsIn, 0, 0, 0, FW_BOLD, 0, 0, 0,
                                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                                CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                                DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
                    m_cachedFontDpi = dpiKey;
                }
                HFONT hOldFont = static_cast<HFONT>(SelectObject(hdc, m_hFont));

                // Label: "Zoom multiplier ( 0.001 – 99999 )"
                SetTextColor(hdc, Constants::Theme::ThemedGray(0.9020f, app.themeFactor));
                RECT labelRect = {pad, pad, rc.right - pad, pad + fs + 6};
                DrawTextW(hdc, Constants::Messages::ZOOM_TO_INPUT_HINT, -1, &labelRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

                // Input box
                const int boxTop = labelRect.bottom + gap;
                const int boxH = static_cast<int>(34.0f * app.dpiScale);
                RECT boxRect = {pad, boxTop, rc.right - pad, boxTop + boxH};

                m_inputBox.Draw(hdc, m_hFontInput, boxRect, pad / 2,
                                GetFocus() == m_hWnd, m_outOfRange);
                SelectObject(hdc, m_hFont);

                // Hint line
                RECT hintRect = {
                    pad, boxRect.bottom + gap,
                    rc.right - pad, boxRect.bottom + gap + fs + 4
                };
                SetTextColor(hdc, Constants::Theme::ThemedGray(0.50f, app.themeFactor));
                const wchar_t *hint = m_outOfRange
                                          ? L"Out of range — type a value between 0.1% and 99999%"
                                          : L"Enter = apply zoom  •  Esc = cancel";
                DrawTextW(hdc, hint, -1, &hintRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

                SelectObject(hdc, hOldFont);
                BitBlt(screenDC, 0, 0, rc.right, rc.bottom, hdc, 0, 0, SRCCOPY);
                EndPaint(m_hWnd, &ps);
                return 0;
            }

            case WM_CHAR: {
                wchar_t ch = static_cast<wchar_t>(wParam);
                // Allow digits, decimal point, and backspace
                if ((ch >= L'0' && ch <= L'9') || ch == L'.' || ch == L'\b') {
                    if (m_inputBox.RouteChar(ch, m_hWnd) == InputResult::ConsumedRepaint)
                        InvalidateRect(m_hWnd, nullptr, FALSE);
                }
                return 0;
            }

            case WM_LBUTTONDOWN:
                if (m_inputBox.RouteMouse(WM_LBUTTONDOWN, wParam, lParam, m_hWnd) == InputResult::ConsumedRepaint)
                    InvalidateRect(m_hWnd, nullptr, FALSE);
                return 0;

            case WM_RBUTTONUP:
                if (m_inputBox.RouteMouse(WM_RBUTTONUP, wParam, lParam, m_hWnd) == InputResult::ConsumedRepaint)
                    InvalidateRect(m_hWnd, nullptr, FALSE);
                return 0;

            case WM_MOUSEMOVE:
                if (m_inputBox.RouteMouse(WM_MOUSEMOVE, wParam, lParam, m_hWnd) == InputResult::ConsumedRepaint)
                    InvalidateRect(m_hWnd, nullptr, FALSE);
                return 0;

            case WM_MOUSELEAVE:
                if (m_inputBox.RouteMouse(WM_MOUSELEAVE, wParam, lParam, m_hWnd) == InputResult::ConsumedRepaint)
                    InvalidateRect(m_hWnd, nullptr, FALSE);
                return 0;

            default:
                break;
        }
        return DefWindowProcW(m_hWnd, message, wParam, lParam);
    }
} // namespace UI