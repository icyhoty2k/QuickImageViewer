#include "FloatingPanelWnd.h"
#include "../../Platform/Constants.h"
#include "../../AppState.h"
#include <dwmapi.h>

namespace UI {
    void FloatingPanelWnd::InitFloating(HINSTANCE hInstance, HWND hParent,
                                        LPCWSTR className, LPCWSTR title,
                                        int pixelW, int pixelH,
                                        UINT classStyle) {
        m_hParent = hParent;

        WNDCLASSW wc = {};
        wc.style = classStyle;
        wc.lpfnWndProc = IPanelWindow::WindowRouter;
        wc.hInstance = hInstance;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = className;
        RegisterClassW(&wc); // silently fails if already registered — that is fine

        CreateWindowExW(
                WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED,
                className, title,
                WS_POPUP | WS_CAPTION | WS_BORDER,
                0, 0, pixelW, pixelH,
                hParent, nullptr, hInstance, this);

        if (!m_hWnd) return;

        SetLayeredWindowAttributes(m_hWnd, 0, Constants::THUMBNAIL_PANEL_WINDOW_OPACITY, LWA_ALPHA);

        BOOL darkMode = app.isDarkThemed ? TRUE : FALSE;
        DwmSetWindowAttribute(m_hWnd, Constants::DWMWA_DARK_MODE, &darkMode, sizeof(darkMode));

        DWORD corner = app.cornerPreference;
        DwmSetWindowAttribute(m_hWnd, Constants::DWMWA_WINDOW_CORNER_PREFERENCES, &corner, sizeof(corner));

        COLORREF capColor = Constants::Theme::ThemedGray(
            Constants::Theme::Panel::BACKGROUND_INACTIVE, app.themeFactor);
        DwmSetWindowAttribute(m_hWnd, Constants::DWMWA_CAPTION_COLOR_ATTR, &capColor, sizeof(capColor));

        SetWindowPos(m_hWnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }

    COLORREF FloatingPanelWnd::GetBgColor() const {
        const float base = (GetFocus() == m_hWnd)
                               ? Constants::Theme::Panel::BACKGROUND_ACTIVE
                               : Constants::Theme::Panel::BACKGROUND_INACTIVE;
        return Constants::Theme::ThemedGray(base, app.themeFactor);
    }

    LRESULT FloatingPanelWnd::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
        if (message == WM_XBUTTONDOWN) {
            if (m_hParent) {
                // Forward the event to the parent so MouseHandler can process it
                SendMessageW(m_hParent, WM_XBUTTONDOWN, wParam, lParam);
                return 0;
            }
        }
        if (message == WM_SETFOCUS) {
            InvalidateRect(m_hWnd, nullptr, FALSE);
            OnSetFocus();
            return 0;
        }
        if (message == WM_KILLFOCUS) {
            InvalidateRect(m_hWnd, nullptr, FALSE);
            OnKillFocus();
            return 0;
        }
        if (message == WM_KEYDOWN) {
            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
            if (!OnKeyDown(wParam, ctrl, shift, alt))
                PostMessageW(m_hParent, WM_KEYDOWN, wParam, lParam);
            return 0;
        }
        return HandlePanelMessage(message, wParam, lParam);
    }
} // namespace UI
