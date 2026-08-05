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
#include "UI/CustomControls/InputBox.h"
#include "UI/CustomControls/ScrollView.h"
#include "RemoteMirror.h"   // CmdReply

// =============================================================================
// RemoteCmdWnd (Ctrl+F10) — pick a command, give it a value, send it.
//
// The manual counterpart to mirroring. F11 forwards what you DO; this sends what
// you TYPE — including the commands that have no key at all (`msgRemote`,
// `OpenFile`, `JumpToImage`, `SlideshowSetInterval`, `EnableRemoteLog`).
//
// Every command has ONE wire name, spelled exactly like its Command.h
// enumerator. What this panel lists is what goes on the socket.
//
//   go to a numbered image in the       image NUMBER, 1-based — the same
//   current playlist                    number the overlay shows
//   Filter [jump          ]        JumpToImage  <value>
//   ┌ Commands   takes a value ┐   [ 12                              ]
//   │ FlipH                   ░│
//   │▸JumpToImage             ▓│   [Send] [Clear] [Log] [Close]
//   │ NextImage               ▓│   ☐ also run it here
//   │ …                       ░│   Send to  (2 of 3 connected)
//   │                    ░│        ┌──────────────────────────────┐
//   │                    ░│        │ ☑ Monitor2   127.0.0.1:7777  │
//   └─────────────────────┘        └──────────────────────────────┘
//   Sent `JumpToImage 12` to 2 instance(s)               Log  6
//   ┌─────────────────────────────────────────────────────────────┐
//   │ 6 ← Wall-Left    OK JumpToImage=12/238 IMG_0012.jpg 0.6 ms ░│  newest
//   │ 5 ← Monitor2     OK JumpToImage=12/238 IMG_0012.jpg 0.4 ms ░│
//   │ 4 → 2 instances  JumpToImage 12                            ░│  the send
//   │ 3 ← Monitor2     OK ZoomTo=150                     0.3 ms  ░│
//   └─────────────────────────────────────────────────────────────┘
//
// TWO DESCRIPTIONS ON ONE LINE, over the two boxes they belong to: what the
// highlighted command DOES over the filter/list column, what its VALUE means over
// the value box. The panel's old title and key-hint line sat here and were text
// you read once. This also split CommandEntry::desc from ::valueDesc — while they
// shared one field, a payload row's only text described its value, so the panel
// had nothing to say about what the command did.
//
// THE LIST SHOWS NAMES, nothing else. The description used to share the row and was
// clipped to a few words by the column, which made it a teaser rather than an
// explanation — and it is stated in full above the box for whichever row is
// highlighted. One sentence, one place.
//
// Instead the NAME carries the one thing worth knowing before picking a row:
// whether the command takes a value, as a second blue (PanelColors::PATH_ALT
// against PATH), keyed by the header. That decides whether the Value box has to be
// filled in, which is the question a colour can answer and a clipped sentence
// could not.
//
// A filter still matches DESCRIPTIONS as well as names, even though they are no
// longer drawn in the list: typing "wallpaper" should find the six wallpaper
// commands whatever they happen to be called.
//
// BROWSE FIRST. The command list is PERMANENT and scrollable, not a popup you
// have to summon: nobody remembers ninety wire names, and a list that only
// appears on a chord is a list most people never find. The filter box narrows
// it; the detail pane on the right explains whatever is highlighted.
//
// The list is built from Remote::CommandTable() — the same table the wire parser
// accepts — so a name shown here cannot come back "unknown command", and there
// is exactly one row per command because the table is now one row per command.
//
// WHERE IT GOES: the ☑ rows in this panel's own SEND TO box — the connected
// instances, with a tick each, under the buttons. Its OWN selection, deliberately
// not the Ctrl+F11 Control ticks: those decide where KEYSTROKES go, and lining one
// screen up by hand should not change what F11 drives afterwards. Two questions
// that look alike and are not the same one.
//
// Only CONNECTED instances are listed — a listed-but-idle row cannot answer, so a
// tick beside it would promise a delivery the send cannot make; F10 is where rows
// get connected. Everything connected starts ticked (the state held is the
// EXCLUSIONS), so an instance that connects while this panel is open is included
// rather than silently left out of a command the user thinks went everywhere.
// All / None sit on the box's header line.
//
// The ☐ also-run-here box adds THIS instance, off by default because the usual
// reason to type a command here is to do something to the other screens.
//
// KEYS
//   type         narrows the list (focus starts in the filter box)
//   ↑ ↓ PgUp/Dn  move the highlight
//   Tab          filter ⇄ value
//   Enter        in the filter: take the highlighted command — to the value box
//                if it needs one, otherwise send. In the value box: send.
//   Esc          clear the filter, then close
//
// ALL THREE LISTS SCROLL — commands, recipients, replies — with the drawn bars
// this app uses everywhere else; native scrollbars are non-client and render as
// white gutters on a dark panel. The wheel goes to whichever the pointer is over.
//
// The recipients list is re-read when Remote::Mirror reports a connection change
// (WM_QIV_REMOTE_TARGETS_CHANGED), not at paint time: this panel repaints on
// mouse-move to track a hovered row, and rebuilding a vector of strings per repaint
// to show three rows that change once a minute is work for nothing. Told rather
// than polled — that notification now holds several subscribers, so this panel and
// the F10 console can both be open and both current.
//
// THE BOTTOM BOX IS A LOG, not a reply list. It holds the whole session — every
// line SENT and every answer, numbered, in the order things actually happened,
// exactly like the Ctrl+F12 wire log but scoped to what this panel sent.
//
// It used to hold the replies to the last send only, cleared on every press. A
// typed session is a conversation — send, read, adjust, send again — and the
// question you have is what the PREVIOUS answer was, which the clear threw away.
// Only the Clear button empties it now, and that restarts the numbering at 1 —
// Clear means "fresh session", so the first line of the new one is line 1. Bounded
// at LOG_MAX_ROWS, oldest first, and the header says when it has started dropping.
//
// NEWEST FIRST — at the TOP, which is the opposite of the Ctrl+F12 wire log and is
// meant to be. That log is a transcript read forwards to follow an exchange; this
// one answers "what did that just say?", and that answer is always the newest line.
// At the top it is there the instant it lands, with nothing to scroll and no tail to
// follow. Each exchange therefore reads upwards — the send, then its answers above
// it — because the alternative is a newest-first log with one section that is not.
//
// Scrolled into the history, the reading position HOLDS: everything shifts down a
// row when one arrives, so the offset is nudged by the same row. Drifting by a row
// per message while somebody reads an answer from three sends ago is the thing logs
// get wrong.
//
// REPLIES arrive one per target, asynchronously (WM_QIV_REMOTE_CMD_REPLY):
// sending happens on a sender thread and waiting for an answer on the UI thread
// would freeze the viewer doing the asking. So the SENT row goes in first and the
// answers land under it as they come — which is also why the log survives a close
// and reopen, while the "waiting for n" counter does not.
// =============================================================================

namespace UI {

class RemoteCmdWnd : public FloatingPanelWnd {
    public:
        void Init(HINSTANCE hInstance, HWND hParent) override;
        void Init(HINSTANCE hInstance, HWND hParent, int8_t position) override;
        void Show() override;
        // Overridden only to unsubscribe from Remote::Mirror's connection-change
        // notification — see AddPanelNotify.
        void Hide() override;

        ~RemoteCmdWnd() {
            if (m_hFontBody)  DeleteObject(m_hFontBody);
            if (m_hFontBold)  DeleteObject(m_hFontBold);
            if (m_hFontSmall) DeleteObject(m_hFontSmall);
            DestroyBackBuffer();
        }

    protected:
        LRESULT HandlePanelMessage(UINT message, WPARAM wParam, LPARAM lParam) override;
        bool    OnKeyDown(WPARAM vk, bool ctrl, bool shift, bool alt) override;
        bool    OnLocalHide() override;

    private:
        enum class Focus { Filter, Value };

        // One scrollable region. Two of them here (commands, replies) and the
        // arithmetic is identical, so it lives in one place rather than twice —
        // the drawn bar, the clamp, and the drag all read the same fields.
        // ScrollView moved to UI/CustomControls/ScrollView.h when Server Clients
        // needed the same thing — see the note in that header. Unqualified here
        // because this class is in namespace UI.
        using ScrollView = UI::ScrollView;

        struct Command {
            std::wstring name;
            std::wstring desc;       // may be empty — see CommandEntry::desc
            // What the VALUE means. Shown over the Value box, while `desc` is shown
            // over the command box — two questions, two places, so neither answer
            // has to serve as the other.
            std::wstring valueDesc;
            bool         needsValue = false;
        };

        // One line of the session log along the bottom — a SENT command or an
        // ANSWER, in the order things actually happened.
        //
        // It used to hold only the replies to the last send, cleared on every press.
        // A typed session is a conversation: you send, read, adjust, send again, and
        // the useful question is what the previous answer was — which the clear threw
        // away. Same shape as the Ctrl+F12 wire log, scoped to what THIS panel sent.
        struct Reply {
            int          seq = 0;
            // Ours going out, or theirs coming back. Decides the arrow and which side
            // of the exchange the row is describing.
            bool         outbound = false;
            std::wstring target;    // the instance, or how many were sent to
            std::wstring text;      // the command line, or the reply
            bool         ok = true;
            long long    deltaUs = -1;
        };

        struct Button {
            std::wstring label;
            int          id = 0;
            RECT         rect{};
            bool         enabled = true;
        };

        // One CONNECTED instance, in the recipients box under the buttons.
        //
        // Snapshotted from Remote::Mirror::Targets() when a connection CHANGES
        // (WM_QIV_REMOTE_TARGETS_CHANGED), not read at paint time: a panel repaints
        // on mouse-move to track a hovered row, and rebuilding a vector of strings
        // per repaint to show three or four rows that change once a minute is work
        // for nothing.
        //
        // Only connected instances appear. A row that is listed but switched off
        // cannot answer a typed command, and offering it a tick would promise
        // something the send cannot keep — F10 is where rows are connected.
        struct TargetRow {
            int          id = 0;      // Mirror's handle, NOT a position
            std::wstring name;
            std::wstring host;
            int          port = 0;
        };

        // --- Actions ---------------------------------------------------------
        void DoSend();
        void DoClear();
        void MoveSelection(int delta);
        // Enter in the filter box: commit the highlighted command. Sends it
        // outright when it takes no value, so the common case is one keypress.
        void TakeHighlighted();

        // --- Model -----------------------------------------------------------
        void BuildCommands();     // once, from Remote::CommandTable()
        void ApplyFilter();       // m_all → m_shown, preserving the highlight
        void BuildButtons();
        const Command *Selected() const;

        // --- Layout / paint ---------------------------------------------------
        void EnsureFonts(HDC dc);
        void EnsureBackBuffer(HDC refDC, int w, int h);
        void DestroyBackBuffer();
        void Repaint();
        void EnsureSelectionVisible();
        int  HitTestButton(POINT pt) const;
        int  HitTestCommandRow(POINT pt) const;
        int  HitTestTargetRow(POINT pt) const;

        // Re-snapshots the connected instances. Rows are keyed by id, and the
        // exclusion set below survives the rebuild, so a target that drops and comes
        // back keeps whatever the user decided about it.
        void RefreshTargets();
        // The ids this send will actually reach, in row order.
        std::vector<int> CheckedTargetIds() const;

        InputBox m_filterBox;
        InputBox m_valueBox;
        Focus    m_focus = Focus::Filter;

        std::vector<Command> m_all;     // every distinct wire command
        std::vector<Command> m_shown;   // after the filter
        int m_selected = 0;             // index into m_shown

        std::vector<Reply>  m_replies;
        std::vector<Button> m_buttons;
        // Reset to 1 by Clear, unlike the Ctrl+F12 wire log, which keeps counting.
        // That log records a whole session and a gap in its numbers is evidence; this
        // one is a scratchpad you empty deliberately, and after emptying it the first
        // line is line 1.
        int  m_nextSeq = 1;
        // Appends one row, trims the oldest past the cap, and follows the tail unless
        // the reader has scrolled up to look at something.
        void AddLogRow(Reply &&row);

        ScrollView m_list;
        ScrollView m_log;
        ScrollView m_targets;

        std::vector<TargetRow> m_targetRows;
        // UNCHECKED ids, not checked ones — so an instance that connects while this
        // panel is open is included by default. The common case is "send to
        // everything that is up"; a set of exclusions makes that the state that
        // needs no upkeep, and a newly arrived screen cannot be silently left out of
        // a command the user thinks went everywhere.
        std::vector<int>       m_untickedIds;
        RECT m_targetsAllRect{}, m_targetsNoneRect{};

        // Drag state used to live here as a Drag enum naming which of the three
        // bars was held. The base owns it now, keyed by the ScrollView pointer
        // itself, so there is nothing to name and nothing to keep in step.
        //
        // Both overrides are the whole of this panel's scroll code. The POINT is
        // load-bearing here and nowhere else in the app: three lists share one
        // window, and the wheel has to move the one under the cursor.
        ScrollView *ScrollViewAt(POINT pt) override;
        int ScrollLinePx(const ScrollView &) const override;

        bool m_alsoLocal = false;
        RECT m_localRect{};
        RECT m_filterRect{}, m_valueRect{};

        std::wstring m_status;
        int          m_awaiting = 0;

        int m_hotButton = -1;

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
