// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#include "JumpToWnd.h"
#include "UI/GdiPool.h" // pooled brushes and pens — never DeleteObject them
#include "../../AppState.h"
#include "../../Platform/Constants.h"
#include "../../Platform/FileHandler.h"
#include "CustomControls/InputBox.h"

extern AppState app;

namespace UI {
    void JumpToWnd::Init(HINSTANCE hInstance, HWND hParent) {
        const int w = static_cast<int>(340.0f * app.dpiScale);
        const int h = static_cast<int>(140.0f * app.dpiScale);
        InitFloating(hInstance, hParent, L"QivJumpToWndClass", L"Jump to Image", w, h);

        m_inputBox.SetMaxLength(6);
        m_inputBox.OnChanged = [this](const std::wstring& t) {
            int len = static_cast<int>(t.size());
            wcsncpy_s(m_input, t.c_str(), len);
            m_input[len] = L'\0';
            m_inputLen   = len;
            m_outOfRange = false;
            InvalidateRect(m_hWnd, nullptr, FALSE);
        };
    }

    void JumpToWnd::Init(HINSTANCE hInstance, HWND hParent, int8_t /*position*/) {
        Init(hInstance, hParent);
    }

    void JumpToWnd::Show() {
        if (!m_hWnd) return;
        m_inputBox.Reset();
        m_input[0]   = L'\0';
        m_inputLen   = 0;
        m_outOfRange = false;
        m_total = static_cast<int>(app.playlist.size());

        ShowCenterOverParent();
        InvalidateRect(m_hWnd, nullptr, FALSE);
    }

    void JumpToWnd::CommitJump() {
        if (m_inputLen == 0) {
            Hide();
            return;
        }
        int number = _wtoi(m_input);
        if (m_total <= 0 || number < 1 || number > m_total) {
            m_outOfRange = true;
            InvalidateRect(m_hWnd, nullptr, FALSE);
            return;
        }
        Hide();
        LoadImageIndex(m_hParent, number - 1);
        InvalidateRect(m_hParent, nullptr, FALSE);
    }

    // Esc: if the number box has text, clear it (same as the ✕ button) and keep
    // the panel open. Empty box → return false so the base hides the panel.
    bool JumpToWnd::OnLocalHide() {
        switch (m_inputBox.RouteKey(VK_ESCAPE, m_hWnd)) {
            case InputResult::RequestClear:
                InvalidateRect(m_hWnd, nullptr, FALSE);
                return true;
            default:
                return false; // RequestClose (empty) → base hides the panel
        }
    }

    bool JumpToWnd::OnKeyDown(WPARAM vk, bool ctrl, bool /*shift*/, bool alt) {
        (void)ctrl; (void)alt; // modifiers read via GetKeyState inside RouteKey

        // Host-specific key first: Enter commits the jump.
        if (vk == VK_RETURN) {
            CommitJump();
            return true;
        }

        // Everything else → the text box (editing, Ctrl+A/C/X/V, forward policy).
        switch (m_inputBox.RouteKey(vk, m_hWnd)) {
            case InputResult::Ignored:         return false; // forward to app pipeline
            case InputResult::RequestClose:    return false; // (Esc arrives via OnLocalHide)
            case InputResult::RequestClear:    InvalidateRect(m_hWnd, nullptr, FALSE); return true;
            case InputResult::ConsumedRepaint: InvalidateRect(m_hWnd, nullptr, FALSE); return true;
            case InputResult::Consumed:        return true;
        }
        return true;
    }

    void JumpToWnd::EnsureBackBuffer(HDC refDC, int w, int h) {
        if (m_bbDC && w == m_bbW && h == m_bbH) return;
        DestroyBackBuffer();
        m_bbDC = CreateCompatibleDC(refDC);
        m_bbBmp = CreateCompatibleBitmap(refDC, w, h);
        m_bbBmpOld = static_cast<HBITMAP>(SelectObject(m_bbDC, m_bbBmp));
        m_bbW = w;
        m_bbH = h;
    }

    void JumpToWnd::DestroyBackBuffer() {
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

    LRESULT JumpToWnd::HandlePanelMessage(UINT message, WPARAM wParam, LPARAM lParam) {
        if (message == WM_ERASEBKGND) return 1;
        switch (message) {
            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC screenDC = BeginPaint(m_hWnd, &ps);
                RECT rc;
                GetClientRect(m_hWnd, &rc);
                EnsureBackBuffer(screenDC, rc.right, rc.bottom);
                HDC hdc = m_bbDC;

                FillRect(hdc, &rc, UI::Gdi::Brush(GetBgColor()));
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

                // Label: "Image number  ( 1 – N )"
                wchar_t label[80];
                if (m_total > 0)
                    swprintf_s(label, L"Image number  ( 1 – %d )", m_total);
                else
                    swprintf_s(label, L"No images loaded");

                SetTextColor(hdc, Constants::Theme::ThemedGray(0.9020f, app.themeFactor));
                RECT labelRect = {pad, pad, rc.right - pad, pad + fs + 6};
                DrawTextW(hdc, label, -1, &labelRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

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
                                          ? L"Out of range — type a new number"
                                          : L"Enter = jump  •  Esc = cancel";
                DrawTextW(hdc, hint, -1, &hintRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

                SelectObject(hdc, hOldFont);
                BitBlt(screenDC, 0, 0, rc.right, rc.bottom, hdc, 0, 0, SRCCOPY);
                EndPaint(m_hWnd, &ps);
                return 0;
            }

            case WM_CHAR: {
                wchar_t ch = static_cast<wchar_t>(wParam);
                if (ch == Constants::PANEL_SWITCH_TO_FIND_CHAR && m_inputBox.IsEmpty()) {
                    Hide();
                    PostMessageW(m_hParent, Constants::WM_QIV_SWITCH_TO_FIND, 0, 0);
                    return 0;
                }
                // Allow digits and backspace; InputBox enforces the max-length of 6.
                if ((ch >= L'0' && ch <= L'9') || ch == L'\b') {
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
                // Ends a drag-select. Without it the box only drops m_dragging on
                // the next WM_MOUSEMOVE, so moving after release keeps extending
                // the selection.
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
