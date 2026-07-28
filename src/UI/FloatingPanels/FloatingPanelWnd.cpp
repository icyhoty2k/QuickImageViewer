#include "FloatingPanelWnd.h"
#include "Dedicated/DedicatedInstance.h" // AppIconId — one icon source for the process
#include "../../Platform/Constants.h"
#include "../../AppState.h"
#include <dwmapi.h>
#include <windowsx.h>

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
        wc.hCursor = Constants::Cursors::CURR_DEFAULT;
        // Panels carry the same icon as the app, so a dedicated instance stays
        // visually distinct all the way down to its floating windows.
        wc.hIcon = LoadIcon(hInstance, MAKEINTRESOURCEW(Dedicated::AppIconId()));
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
        // WM_SYSKEYDOWN as well as WM_KEYDOWN, and the reason is not obvious:
        //
        //   • F10 pressed ALONE arrives as WM_SYSKEYDOWN. Windows reserves it as
        //     the menu-activation key, so it never appears as an ordinary
        //     keypress — which meant F10 toggled its panel from the main window
        //     and did nothing at all once that panel had focus.
        //   • Every ALT combination arrives the same way. Alt+W/A/S/D (snap),
        //     Alt+Q/E/Z/C (quarter-snap) and Alt+X (reset window) were therefore
        //     dead in every panel, silently.
        //
        // AppMain's WndProc has always handled both for the main window. Panels
        // forward to it, so they have to recognise the same pair or the two
        // disagree about which keys exist.
        if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) {
            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;

            if (!OnKeyDown(wParam, ctrl, shift, alt)) {
                // Forwarded as the SAME message it arrived as. Re-labelling a
                // WM_SYSKEYDOWN as WM_KEYDOWN would strip the context bit in
                // lParam that says whether Alt was held — and the resolver reads
                // lParam for the Right-Shift scancode, so the original must
                // survive intact.
                PostMessageW(m_hParent, message, wParam, lParam);
            }

            // System keys still go to DefWindowProc, exactly as the main window
            // does it, so Alt+F4 and Alt+Space keep working. Neither the main
            // window nor a panel has a menu bar, so F10's own default behaviour
            // has nothing to open.
            return (message == WM_SYSKEYDOWN)
                       ? DefWindowProcW(m_hWnd, message, wParam, lParam)
                       : 0;
        }
        if (message == WM_MBUTTONUP) {
            if (!OnMButtonUp(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)))
                Hide();
            return 0;
        }
        return HandlePanelMessage(message, wParam, lParam);
    }
} // namespace UI
