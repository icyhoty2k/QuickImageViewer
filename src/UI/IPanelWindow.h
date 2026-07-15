#pragma once
#include <cstdint>
#include <windows.h>
#include "Shortcuts.h"

// Include your Shortcuts header if you want to use SC_LOCAL_HIDE
// #include "Shortcuts.h"

namespace UI {
    class IPanelWindow {
        public:
            virtual ~IPanelWindow() {
                if (m_hWnd) {
                    // Unhook WindowRouter BEFORE DestroyWindow. By the time this
                    // base destructor runs, the derived object is already gone and
                    // the vtable points at IPanelWindow, where HandleMessage is
                    // pure virtual. DestroyWindow sends WM_DESTROY/WM_NCDESTROY
                    // synchronously; routing them through GWLP_USERDATA would be
                    // a pure virtual call → _purecall → abort (0xC0000409).
                    SetWindowLongPtrW(m_hWnd, GWLP_USERDATA, 0);
                    DestroyWindow(m_hWnd);
                    m_hWnd = nullptr;
                }
            }

            // Pure virtual function ensuring derived classes implement their setup
            virtual void Init(HINSTANCE hInstance, HWND hParent) = 0;

            virtual void Init(HINSTANCE hInstance, HWND hParent, int8_t position) =0;

            virtual void Show() {
                if (m_hWnd) {
                    ShowWindow(m_hWnd, SW_SHOW);
                    SetForegroundWindow(m_hWnd);
                }
            }

            virtual void Hide() {
                if (m_hWnd && IsWindowVisible(m_hWnd)) {
                    ShowWindow(m_hWnd, SW_HIDE);
                }
            }

            virtual void Toggle() {
                IsVisible() ? Hide() : Show();
            }

            bool IsVisible() const {
                return m_hWnd && IsWindowVisible(m_hWnd);
            }

            HWND GetHwnd() const {
                return m_hWnd;
            }

        protected:
            HWND m_hWnd = nullptr;
            HWND m_hParent = nullptr;

            // Derived classes will put their switch(message) logic here
            virtual LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) = 0;

            // The bridge between Win32 and C++
            static LRESULT CALLBACK WindowRouter(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
                IPanelWindow *pThis = nullptr;

                if (message == WM_NCCREATE) {
                    // When the window is created, extract the 'this' pointer passed via CreateWindowExW
                    CREATESTRUCT *pCreate = reinterpret_cast<CREATESTRUCT *>(lParam);
                    pThis = static_cast<IPanelWindow *>(pCreate->lpCreateParams);

                    // Store the pointer inside the window's internal data structure
                    SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));
                    pThis->m_hWnd = hWnd;
                } else {
                    // For all subsequent messages, retrieve the stored pointer
                    pThis = reinterpret_cast<IPanelWindow *>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
                }

                if (pThis) {
                    // --- GLOBAL PANEL SHORTCUTS ---
                    // Intercept keyboard commands before they reach the child class
                    if (message == WM_KEYDOWN) {
                        bool isCtrlDown = (GetKeyState(VK_CONTROL) & 0x8000) != 0;

                        // If you include Shortcuts.h, use wParam == Shortcuts::SC_LOCAL_HIDE instead of VK_ESCAPE
                        if ((isCtrlDown && wParam == Shortcuts::SC_APP_HIDE_ALT) || wParam == Shortcuts::IPANNEL_WINDOW_LOCAL_HIDE) {
                            pThis->Hide();
                            return 0; // Message handled, do not pass to child
                        }
                    }

                    // Route remaining messages to the specific instance's handler
                    return pThis->HandleMessage(message, wParam, lParam);
                }

                return DefWindowProcW(hWnd, message, wParam, lParam);
            }
    };
}
