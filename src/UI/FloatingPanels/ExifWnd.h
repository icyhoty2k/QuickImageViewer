// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <set>
#include "FloatingPanelWnd.h"
#include "UI/CustomControls/ScrollView.h"

namespace UI {
    class ExifWnd : public FloatingPanelWnd {
    public:
        void Init(HINSTANCE hInstance, HWND hParent) override;
        void Init(HINSTANCE hInstance, HWND hParent, int8_t position) override;
        void Show() override;
        ~ExifWnd() {
            if (m_hFontNorm) { DeleteObject(m_hFontNorm); m_hFontNorm = nullptr; }
            if (m_hFontBold) { DeleteObject(m_hFontBold); m_hFontBold = nullptr; }
            DestroyBackBuffer();
        }
        // Called when the displayed image changes while the window is open.
        // Queues EXIF reading on the IO thread — zero UI-thread cost.
        void Refresh();

    protected:
        bool    OnKeyDown(WPARAM vk, bool ctrl, bool shift, bool alt) override;
        LRESULT HandlePanelMessage(UINT message, WPARAM wParam, LPARAM lParam) override;

    private:
        struct ExifRow {
            std::wstring label;
            std::wstring value;
            bool isSection = false;
            std::wstring action; // non-empty: URL opened on plain click instead of copy
        };

        struct ExifResult {
            std::vector<ExifRow> rows;
        };

        // Builds EXIF rows for the given image.
        // Safe to call from any COM-initialized thread.
        static ExifResult GatherExifData(const std::wstring& path, int imgW, int imgH);

        std::vector<ExifRow> m_rows;
        HBITMAP  m_thumbBitmap     = nullptr;
        int      m_thumbW          = 0;
        int      m_thumbH          = 0;
        // Scroll state. The BASE drives it — wheels, drag, paging and cursor all
        // live in FloatingPanelWnd now, reached through the two overrides below.
        // This panel used to carry all four, and its drag never took capture:
        // releasing outside the window left it dragging until the next click.
        // contentH of 0 doubles as the "needs re-measuring" sentinel.
        UI::ScrollView m_view;

        UI::ScrollView *ScrollViewAt(POINT) override { return &m_view; }
        int ScrollLinePx(const UI::ScrollView &) const override;
        bool  m_moving             = false;
        POINT m_moveStartCursor    = {};
        RECT  m_moveStartRect      = {};
        int   m_anchorRow          = -1;
        std::set<int> m_selectedRows;

        // Cached GDI fonts — recreated only when DPI changes
        HFONT m_hFontNorm    = nullptr;
        HFONT m_hFontBold    = nullptr;
        UINT  m_cachedFontDpi = 0;

        void EnsureBackBuffer(HDC refDC, int w, int h);
        void DestroyBackBuffer();
        HDC     m_bbDC     = nullptr;
        HBITMAP m_bbBmp    = nullptr;
        HBITMAP m_bbBmpOld = nullptr;
        int     m_bbW      = 0;
        int     m_bbH      = 0;
    };
} // namespace UI
