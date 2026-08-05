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
#include "UI/FloatingPanels/FloatingPanelWnd.h"
#include "UI/CustomControls/ScrollView.h"
#include "RemoteServer.h"      // ClientInfo — the live rows
#include "RemoteBlacklist.h"   // TimedEntry — the second section

// =============================================================================
// RemoteClientsWnd (Ctrl+F9, "Server Clients") — WHO is connected to the
// listener this instance runs, and the three ways to get rid of them.
//
// SPLIT OUT OF THE LOCAL SERVER PANEL (F9) ON PURPOSE. That panel describes the
// listener's CONFIGURATION: a form you open, change and save. This one is a live
// view that moves while you look at it — clients arrive, names appear when they
// say hello, timed blocks count down. Those are two different kinds of window
// and they were fighting each other: the connection list went stale the moment
// you started typing in a settings field, and rebuilding it to keep it fresh
// threw away whatever was half-typed above.
//
// -----------------------------------------------------------------------------
// THE THREE ACTIONS, AND WHY THERE ARE THREE
//
//   Kick        close the connection. It may reconnect immediately.
//   Kick for N  close it AND refuse this peer for N minutes.
//   Ban         blacklist permanently, then close it.
//
// The middle one is the one that answers a bot, and it is the reason this panel
// was worth building. A plain kick is useless against anything automated — it
// reconnects inside a second. A ban is a permanent decision about an address
// that may be a customer tomorrow, written to a file somebody has to remember
// to edit. A timed block outlasts a retry loop and then forgets by itself.
//
// Timed blocks live IN MEMORY ONLY and do not survive a restart — see the timed
// section of RemoteBlacklist.h. That is also the escape hatch when you have shut
// yourself out: restart qIV.
//
// -----------------------------------------------------------------------------
// NOT ON THE WIRE. None of this is reachable by a connected peer, and none of it
// is in the command table. `ban` writes access-control state that is read before
// every accept and survives a restart — the same category as delete/move/save
// that NEVER_REMOTE keeps off the wire. A remotely reachable ban would also let
// any authenticated peer blacklist the operator and lock them out of their own
// listener. See the note over Connections() in RemoteServer.h.
//
// LIVE REFRESH is a timer, not a message. The server has no "something changed"
// broadcast the panels subscribe to, and adding one for a list that is only on
// screen while somebody is looking at it would be machinery for nothing. The
// timer runs only while the panel is visible.
// =============================================================================

namespace UI {

class RemoteClientsWnd : public FloatingPanelWnd {
    public:
        void Init(HINSTANCE hInstance, HWND hParent) override;
        void Init(HINSTANCE hInstance, HWND hParent, int8_t position) override;
        void Show() override;

        ~RemoteClientsWnd() {
            if (m_hFontBody)  DeleteObject(m_hFontBody);
            if (m_hFontBold)  DeleteObject(m_hFontBold);
            if (m_hFontSmall) DeleteObject(m_hFontSmall);
            DestroyBackBuffer();
        }

    protected:
        LRESULT HandlePanelMessage(UINT message, WPARAM wParam, LPARAM lParam) override;
        bool    OnKeyDown(WPARAM vk, bool ctrl, bool shift, bool alt) override;

    private:
        // A row is either a live connection or a live timed block. One list with
        // a kind rather than two lists, because the selection, the hit test and
        // the keyboard walk are identical for both and duplicating them is how
        // the two drift.
        enum class Kind { Header, Client, Block, Note };

        struct Row {
            Kind         kind = Kind::Client;
            std::wstring label;
            std::wstring value;
            std::wstring desc;
            // Index into m_conns or m_blocks, by kind. -1 for headers and notes.
            int          item = -1;
            // Position in CONTENT space — measured from the top of the list, not
            // the top of the window, so it survives scrolling. Assigned by
            // LayoutRows and used both to paint and to scroll the selection into
            // view; `rect` is the on-screen result and is EMPTY for a row that
            // is currently scrolled out, which is what keeps the hit test from
            // finding rows nobody can see.
            int          cy = 0;
            int          ch = 0;
            RECT         rect{};
        };

        struct Button {
            std::wstring label;
            int          id = 0;
            RECT         rect{};
            bool         enabled = true;
        };

        // --- Actions ---------------------------------------------------------
        void DoKick();
        void DoTimedKick();
        void DoBan();
        void DoLiftBlock();

        // --- Model -----------------------------------------------------------
        void BuildRows();
        void SyncButtons();          // enable state follows the selection

        // Assigns every row's cy/ch and sets m_list.contentH. Called from paint,
        // because row heights are DPI-scaled and the scale is only known with a
        // DC in hand.
        void LayoutRows(int rowH, int headerH);

        // Scrolls the selected row into view. Called after every selection move
        // and after a rebuild — a list that refreshes under a selection you
        // cannot see is worse than one that does not refresh.
        void EnsureSelectedVisible();

        // The selected row's item index, or -1 when the selection is not of that
        // kind. The one place row-kind is decoded into an item.
        int  SelectedClient() const;
        int  SelectedBlock() const;

        std::wstring StatusLine() const;

        // --- Dialog helpers (panel is topmost; dialogs are not) --------------
        void DialogMessage(const std::wstring &text);
        bool DialogConfirm(const std::wstring &text);
        int  DialogPromptInt(const wchar_t *label, int cur, int lo, int hi, int def);
        void PushTopmostOff();
        void PopTopmost();

        // --- Paint -----------------------------------------------------------
        void EnsureFonts(HDC dc);
        void EnsureBackBuffer(HDC refDC, int w, int h);
        void DestroyBackBuffer();
        void Repaint();
        int  HitTestRow(POINT pt) const;
        int  HitTestButton(POINT pt) const;

        std::vector<Row>    m_rows;
        std::vector<Button> m_buttons;

        // The snapshots the current rows were built from. Held so a button press
        // maps the selected ROW back to a ConnId without asking the server
        // again — re-querying between the click and the action would let the
        // list shift and act on a different peer than the one on screen.
        std::vector<Remote::ClientInfo>            m_conns;
        std::vector<Remote::Blacklist::TimedEntry> m_blocks;

        int m_selected  = 0;
        int m_hotRow    = -1;
        int m_hotButton = -1;

        ScrollView m_list;

        // The base owns every scroll interaction — both wheels, thumb drag with
        // capture, track paging, the hand cursor. These two overrides are the
        // whole of this panel's involvement.
        ScrollView *ScrollViewAt(POINT) override { return &m_list; }
        int ScrollLinePx(const ScrollView &) const override;

        // The last paint's row height, so the wheel can scroll by rows without a
        // DC. Zero until the first paint — and a window that has never painted
        // has nothing to scroll, which is why the wheel floors it at 1.
        int m_lastRowH = 0;

        // Set when the ROW SET changed, consumed by the next paint.
        //
        // Deferred rather than done in BuildRows because the positions it would
        // need do not exist yet: cy/ch are assigned by LayoutRows, which runs
        // during paint with the DPI-scaled heights. Acting on the PREVIOUS
        // layout's positions after the rows have changed underneath them is how
        // a refresh scrolls somewhere arbitrary.
        bool m_scrollToSelection = false;

        std::wstring m_lastResult;

        HFONT m_hFontBody  = nullptr;
        HFONT m_hFontBold  = nullptr;
        HFONT m_hFontSmall = nullptr;
        int   m_cachedFontDpi = 0;

        HDC     m_bbDC     = nullptr;
        HBITMAP m_bbBmp    = nullptr;
        HBITMAP m_bbBmpOld = nullptr;
        int     m_bbW = 0, m_bbH = 0;
};

} // namespace UI
