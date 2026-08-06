// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

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

            // Toggle, but when OPENING actually put the panel in front.
            //
            // Show() alone is not enough and the reason is not obvious: every
            // panel here is WS_EX_TOPMOST, so they all sit in the same z-band,
            // and re-showing one that is already visible leaves it wherever it
            // was inside that band — usually behind whichever panel you pressed
            // the button on, which is exactly where you cannot see it. The
            // explicit HWND_TOPMOST re-assert is what reorders it WITHIN the
            // band; SetForegroundWindow alone does not, because the window never
            // lost activation to begin with.
            //
            // This is what every cross-panel button wants — the pairs that open
            // each other (Server Log ↔ Remote Commands, Local Server ↔ Server
            // Clients) all behave identically because they all come through
            // here. It began as a file static in RemoteLogWnd.cpp and moved the
            // moment a second panel needed it.
            void ToggleToFront() {
                if (IsVisible()) { Hide(); return; }
                Show();
                if (m_hWnd) {
                    SetWindowPos(m_hWnd, HWND_TOPMOST, 0, 0, 0, 0,
                                 SWP_NOMOVE | SWP_NOSIZE);
                    SetForegroundWindow(m_hWnd);
                }
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

            // Called when the local-hide key (Esc) is pressed, BEFORE the base
            // hides the panel. Return true to consume it (panel stays open — e.g.
            // a filter input cleared its text, the same action as its ✕ button);
            // return false to let the base hide the panel. Default: do not consume.
            virtual bool OnLocalHide() { return false; }

            void ShowCenterOverParent() {
                if (!m_hParent || !m_hWnd) return;
                RECT pr, wr;
                GetWindowRect(m_hParent, &pr);
                GetWindowRect(m_hWnd, &wr);
                int px = pr.left + ((pr.right - pr.left) - (wr.right - wr.left)) / 2;
                int py = pr.top + ((pr.bottom - pr.top) - (wr.bottom - wr.top)) / 2;
                SetWindowPos(m_hWnd, nullptr, px, py, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                IPanelWindow::Show();
            }

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

                        // Ctrl+W always hides the panel outright.
                        if (isCtrlDown && wParam == Shortcuts::SC_APP_HIDE_ALT) {
                            pThis->Hide();
                            return 0; // Message handled, do not pass to child
                        }
                        // Plain Esc first offers the panel a chance to consume it
                        // (e.g. clear a filter input — same action as the ✕ button).
                        // Hide only if the panel declines.
                        if (wParam == Shortcuts::IPANNEL_WINDOW_LOCAL_HIDE) {
                            if (!pThis->OnLocalHide()) pThis->Hide();
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
