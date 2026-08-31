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
#include "FloatingPanelWnd.h"
#include "CustomControls/InputBox.h"

namespace UI {
    class FindWnd : public FloatingPanelWnd {
        public:
            // How many cross-folder hits are offered at once. A human reads this
            // list: one letter can match tens of thousands of names, and the ones
            // past the first screenful are neither read nor useful.
            static constexpr int CROSS_FOLDER_MAX = 200;

    public:
        void Init(HINSTANCE hInstance, HWND hParent) override;
        void Init(HINSTANCE hInstance, HWND hParent, int8_t position) override;
        void Show() override;

        // WHICH FOLDERS THE SEARCH COVERS. False is the folder on screen
        // (Ctrl+F); true is every folder qIV knows (Ctrl+Shift+F).
        //
        // Set BEFORE Show, so the first rebuild already has the right scope.
        void SetSearchEverywhere(bool on) { m_searchEverywhere = on; }
        [[nodiscard]] bool SearchesEverywhere() const { return m_searchEverywhere; }

        // Re-runs the current query in the current scope. For the case where the
        // scope changed while the panel was already open and typing.
        void RefreshMatches();
        ~FindWnd() {
            if (m_hFontNorm) DeleteObject(m_hFontNorm);
            if (m_hFontBold) DeleteObject(m_hFontBold);
            DestroyBackBuffer();
        }

    protected:
        bool    OnKeyDown(WPARAM vk, bool ctrl, bool shift, bool alt) override;
        bool    OnLocalHide() override;
        LRESULT HandlePanelMessage(UINT message, WPARAM wParam, LPARAM lParam) override;

    private:
        static constexpr int MAX_QUERY    = 200;
        static constexpr int VISIBLE_ROWS = 8;

        // False = the folder on screen, true = every folder qIV knows. Not
        // persisted: the scope belongs to the keystroke that opened the panel,
        // so Ctrl+F is always the narrow search and never inherits a wide one
        // from an hour ago.
        bool m_searchEverywhere = false;

        struct MatchResult {
            int          playlistIdx; // -1 for cache-only entries
            std::wstring path;        // full path (always set)
            int          score;
            int          positions[MAX_QUERY];
            int          posCount;
        };

        int m_cachedExtraCount = 0; // entries from VRAM cache not in playlist

        InputBox                 m_inputBox;
        wchar_t                  m_query[MAX_QUERY + 2] = {};
        int                      m_queryLen  = 0;
        std::vector<MatchResult> m_results;
        int                      m_selIdx    = 0;
        int                      m_rowScroll = 0;

        void RebuildMatches();
        void CommitOpen();
        void AdjustScroll();

        HFONT m_hFontNorm    = nullptr;
        HFONT m_hFontBold    = nullptr;
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
