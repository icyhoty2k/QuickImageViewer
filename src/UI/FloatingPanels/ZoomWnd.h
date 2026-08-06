// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#pragma once
#include <windows.h>
#include "FloatingPanelWnd.h"
#include "CustomControls/InputBox.h"

namespace UI {
    class ZoomWnd : public FloatingPanelWnd {
    public:
        void Init(HINSTANCE hInstance, HWND hParent) override;
        void Init(HINSTANCE hInstance, HWND hParent, int8_t position) override;
        void Show() override;
        ~ZoomWnd() {
            if (m_hFont)      DeleteObject(m_hFont);
            if (m_hFontInput) DeleteObject(m_hFontInput);
            DestroyBackBuffer();
        }

    protected:
        bool    OnKeyDown(WPARAM vk, bool ctrl, bool shift, bool alt) override;
        bool    OnLocalHide() override;
        LRESULT HandlePanelMessage(UINT message, WPARAM wParam, LPARAM lParam) override;

    private:
        InputBox m_inputBox;
        wchar_t  m_input[16] = {};
        int      m_inputLen = 0;
        bool     m_outOfRange = false;

        void CommitZoom();

        HFONT m_hFont        = nullptr;
        HFONT m_hFontInput   = nullptr;
        int   m_cachedFontDpi = 0;

        void EnsureBackBuffer(HDC refDC, int w, int h);
        void DestroyBackBuffer();
        HDC     m_bbDC     = nullptr;
        HBITMAP m_bbBmp    = nullptr;
        HBITMAP m_bbBmpOld = nullptr;
        int     m_bbW      = 0;
        int     m_bbH      = 0;
    };
} // namespace UI
