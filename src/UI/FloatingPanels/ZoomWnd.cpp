#include "ZoomWnd.h"
#include "../../AppState.h"
#include "../../Common/Converters.h"
#include "../../Platform/Constants.h"
#include "../../Platform/ConstantsStrings.h"
#include "../../Overlays/OverlayManager.h"
#include "CustomControls/InputBox.h"
#include <algorithm>
#include <cmath>
#include <cwchar>

extern AppState app;

namespace UI {
    void ZoomWnd::Init(HINSTANCE hInstance, HWND hParent) {
        const int w = static_cast<int>(Constants::ZoomPanel::WINDOW_WIDTH * app.dpiScale);
        const int h = static_cast<int>(Constants::ZoomPanel::WINDOW_HEIGHT * app.dpiScale);
        InitFloating(hInstance, hParent, L"QivZoomWndClass", L"Zoom to", w, h);

        m_inputBox.SetMaxLength(Constants::ZoomPanel::INPUT_MAX_CHARS);
        m_inputBox.OnChanged = [this](const std::wstring& t) {
            // Hard-clamp to the buffer: InputBox enforces SetMaxLength, but a
            // longer string here would trip wcsncpy_s's invalid-parameter handler.
            const int cap = static_cast<int>(_countof(m_input)) - 1;
            int len = std::min(static_cast<int>(t.size()), cap);
            wcsncpy_s(m_input, t.c_str(), static_cast<size_t>(len));
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

        // Parse STRICTLY. The WM_CHAR filter only guards typing — Ctrl+V and the
        // right-click Paste in InputBox insert any printable character, so text
        // like "abc" would reach _wtof(), return 0.0 and silently trigger the
        // "0 = reset" branch. wcstod + an end-pointer check rejects that.
        wchar_t *end = nullptr;
        const double parsed = wcstod(m_input, &end);
        const bool valid = (end != nullptr && *end == L'\0' && end != m_input)
                           && std::isfinite(parsed);
        if (!valid) {
            m_outOfRange = true;
            InvalidateRect(m_hWnd, nullptr, FALSE);
            return;
        }
        const float percent = static_cast<float>(parsed);

        // 0 restores the original/default zoom for the current view mode.
        // Must match Command::ZoomReset exactly — that also recenters, otherwise
        // the image stays panned off-centre at 100%.
        if (percent == 0.0f) {
            Hide();
            app.viewport.zoom    = 1.0f;
            app.viewport.offsetX = 0.0f;
            app.viewport.offsetY = 0.0f;
            g_overlayManager.PostCenterMessage(m_hParent, Constants::Messages::ZOOM_RESET_MESSAGE);
            InvalidateRect(m_hParent, nullptr, FALSE);
            return;
        }

        // Range check in PERCENT space on both ends (ZOOM_MIN is a ratio, so it
        // is scaled up; ZOOM_MAX is already the panel's percent ceiling). This
        // matches the label and the out-of-range hint exactly.
        const float mult = percent / 100.0f;
        if (percent < Constants::ZoomPanel::ZOOM_MIN * 100.0f || percent > Constants::ZoomPanel::ZOOM_MAX) {
            m_outOfRange = true;
            InvalidateRect(m_hWnd, nullptr, FALSE);
            return;
        }
        // Scale the current zoom by the ratio of desired effective zoom to current
        // effective zoom.  This avoids the float rounding in `baseScale * (X/baseScale)`.
        // GetRealZoom() mirrors the renderer's GetRenderSize(), so this works in
        // every view mode, not just FitToView.
        const float currentEffective = app.GetRealZoom(m_hParent);

        // The overlay only ever shows a ROUNDED percent (e.g. 150.34% prints as
        // "150%").  Typing back the number the overlay shows must be a no-op —
        // otherwise the image visibly shrinks/grows by the rounding remainder.
        // Below 1% the rounded display is "0%" for every value, so the snap would
        // swallow real changes — only apply it where the display is meaningful.
        const bool alreadyThere =
            percent >= 1.0f &&
            Converters::toZoomInt(currentEffective) == static_cast<int>(std::lround(percent));

        if (!alreadyThere && currentEffective > 0.0f) {
            app.viewport.zoom *= mult / currentEffective;
            // Clamp to the engine limits, matching keyboard/mouse zoom commands.
            app.viewport.zoom = std::clamp(app.viewport.zoom,
                                           Constants::ZoomPanel::ZOOM_MIN,
                                           Constants::ZoomPanel::ZOOM_MAX);
            // Zooming to a smaller percent shrinks the legal pan range — drop any
            // stale offset so the image cannot end up parked against a black gap.
            ClampViewportOffset(m_hParent);
        }
        Hide();
        // Report the zoom that was ACTUALLY applied (post-clamp), not the raw
        // keystrokes — "0150" or a clamped 99999% would otherwise lie.
        wchar_t applied[32];
        swprintf_s(applied, 32, L"%d%%", Converters::toZoomInt(app.GetRealZoom(m_hParent)));
        g_overlayManager.PostCenterMessage(m_hParent,
            std::wstring(Constants::Messages::ZOOM_TO_PREFIX) + applied);
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

                const int pad = static_cast<int>(Constants::ZoomPanel::PADDING * app.dpiScale);
                const int gap = static_cast<int>(Constants::ZoomPanel::GAP * app.dpiScale);
                const int fs = static_cast<int>(Constants::ZoomPanel::FONT_SIZE * app.dpiScale);
                const int fsIn = static_cast<int>(Constants::ZoomPanel::FONT_SIZE_INPUT * app.dpiScale);

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
                SetTextColor(hdc, Constants::Theme::ThemedGray(Constants::ZoomPanel::LABEL_TEXT_GRAY, app.themeFactor));
                RECT labelRect = {pad, pad, rc.right - pad, pad + fs + Constants::ZoomPanel::LABEL_EXTRA_HEIGHT};
                // Build the label dynamically with the actual min/max percent limits.
                {
                    wchar_t label[128];
                    swprintf_s(label, 128, Constants::Messages::ZOOM_TO_INPUT_HINT_FMT,
                               static_cast<double>(Constants::ZoomPanel::ZOOM_MIN * 100.0f),
                               static_cast<double>(Constants::ZoomPanel::ZOOM_MAX));
                    DrawTextW(hdc, label, -1, &labelRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                }

                // Input box
                const int boxTop = labelRect.bottom + gap;
                const int boxH = static_cast<int>(Constants::ZoomPanel::INPUT_BOX_HEIGHT * app.dpiScale);
                RECT boxRect = {pad, boxTop, rc.right - pad, boxTop + boxH};

                m_inputBox.Draw(hdc, m_hFontInput, boxRect, pad / 2,
                                GetFocus() == m_hWnd, m_outOfRange);
                SelectObject(hdc, m_hFont);

                // Hint line
                RECT hintRect = {
                    pad, boxRect.bottom + gap,
                    rc.right - pad, boxRect.bottom + gap + fs + Constants::ZoomPanel::HINT_EXTRA_HEIGHT
                };
                SetTextColor(hdc, Constants::Theme::ThemedGray(Constants::ZoomPanel::HINT_TEXT_GRAY, app.themeFactor));
                // Build the hint dynamically so the displayed range reflects
                // the zoom constants in percent space.
                std::wstring hint = m_outOfRange
                    ? [&]() {
                        wchar_t buf[128];
                        swprintf_s(buf, 128, Constants::Messages::ZOOM_OUT_OF_RANGE_FMT,
                                   static_cast<double>(Constants::ZoomPanel::ZOOM_MIN * 100.0f),
                                   static_cast<double>(Constants::ZoomPanel::ZOOM_MAX));
                        return std::wstring(buf);
                      }()
                    : Constants::Messages::ZOOM_ENTER_HINT;
                DrawTextW(hdc, hint.c_str(), -1, &hintRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

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

            case WM_LBUTTONUP:
                // Ends a drag-select. Without this the box only drops m_dragging
                // on the next WM_MOUSEMOVE, so a press-release-move gesture keeps
                // extending the selection after the button is already up.
                if (m_inputBox.RouteMouse(WM_LBUTTONUP, wParam, lParam, m_hWnd) == InputResult::ConsumedRepaint)
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