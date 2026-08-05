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

// =============================================================================
// RemoteWnd (F9, "Local Server") — configure and control the TCP/IP listener
// THIS instance runs. Named Local Server on screen because that is what it is
// from where the user is standing: "Remote Server" was the same words F10 uses
// for the machines at the other end, and the two read as each other.
//
// Lives in src/Rem_TCP_IP rather than UI/FloatingPanels because everything
// remote-related belongs together, the same way src/Dedicated keeps its own
// window beside its own settings.
//
// THIS PANEL DESCRIBES THIS INSTANCE, AND NOTHING ELSE: the listener it runs.
// Autostart, identity, bind address, port, allow/block lists, password, connection
// cap, and Start / Stop / Save.
//
// It used to carry a second section for connecting OUT to another instance, and
// that was the reason it read badly — one window answering both "what am I" and
// "what am I talking to", with a form for each. Everything about driving other
// instances now lives in Remote Servers (F10): the address list, their status,
// and the form that adds one.
//
// The division is: F9 is what others connect TO, F10 is what this connects to.
//
// WHO is connected right now — and kicking, timed-kicking or banning them —
// lives in My Clients (Ctrl+F9, RemoteClientsWnd.h). It was briefly here and
// did not belong: this panel is a FORM, opened to change values and save them,
// and a list that moves while you are typing in the field above it fights that.
//
// Nothing here talks to a socket except Start and Stop, both of which are local
// operations on this instance's own listener and return immediately.
//
// Full design record: docs/REMOTE_TCP_IP_SPEC.md
// =============================================================================

namespace UI {

class RemoteWnd : public FloatingPanelWnd {
    public:
        void Init(HINSTANCE hInstance, HWND hParent) override;
        void Init(HINSTANCE hInstance, HWND hParent, int8_t position) override;
        void Show() override;

        ~RemoteWnd() {
            if (m_hFontBody)  DeleteObject(m_hFontBody);
            if (m_hFontBold)  DeleteObject(m_hFontBold);
            if (m_hFontSmall) DeleteObject(m_hFontSmall);
            if (m_hFontLink)  DeleteObject(m_hFontLink);
            DestroyBackBuffer();
        }

    protected:
        LRESULT HandlePanelMessage(UINT message, WPARAM wParam, LPARAM lParam) override;
        bool    OnKeyDown(WPARAM vk, bool ctrl, bool shift, bool alt) override;
        bool    OnLocalHide() override;

    private:
        enum class Kind { Header, Text, Secret, Number, Toggle, Choice, ReadOnly };

        struct Row {
            Kind           kind = Kind::Text;
            std::wstring   label;
            std::wstring   value;
            const wchar_t *desc = L"";
            int            id   = 0;
            RECT           rect{};
        };

        struct Button {
            std::wstring label;
            int          id = 0;
            RECT         rect{};
            bool         enabled = true;
            int          row = 0;
        };

        // --- Actions ---------------------------------------------------------
        void DoStart();
        void DoStop();
        void DoSaveToIni();        // seeded generation — may create the .ini

        // --- Model -----------------------------------------------------------
        void BuildRows();
        void EditRow(int rowIndex);
        void BeginTextEdit(int rowIndex);
        void CommitTextEdit();
        void CancelTextEdit();
        void PullFromConfig();  // Remote::Config() → m_rows
        void PushToConfig();    // m_rows → Remote::Config()

        std::wstring StatusLine() const;

        // --- Dialog helpers (panel is topmost; dialogs are not) --------------
        void DialogMessage(const std::wstring &text, const wchar_t *caption);
        bool DialogConfirm(const std::wstring &text, const wchar_t *caption);
        int  DialogPromptInt(const wchar_t *caption, const wchar_t *label,
                             int cur, int lo, int hi, int def);
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
        int m_selected  = 0;
        int m_hotRow    = -1;
        int m_hotButton = -1;

        // Plaintext only while the user is typing it. Hashed on save and cleared;
        // the panel never redisplays a password it has already stored.
        std::wstring m_newPassword;

        std::wstring m_lastResult; // last action's outcome, shown in the footer

        // The file the last Save wrote. Non-empty means the footer carries a
        // clickable path — the app's link convention (Constants::Links), same as
        // the Help footer and the History rows.
        std::wstring m_savedPath;
        RECT         m_savedLinkRect{}; // hit box, recomputed on every paint
        bool         m_savedLinkHot = false;

        void RevealSavedFile();

        // The TLS fingerprint on the footer's third line, as a link that COPIES
        // rather than opens. 64 hex characters is not a value anyone retypes,
        // and it has to reach a phone or another machine somehow.
        RECT         m_fpLinkRect{};      // hit box, recomputed on every paint
        bool         m_fpLinkHot = false;
        bool         m_fpCopied  = false; // shows the confirmation until reopened

        InputBox m_edit;
        int      m_editingRow = -1;

        HFONT m_hFontBody  = nullptr;
        HFONT m_hFontBold  = nullptr;
        HFONT m_hFontSmall = nullptr;
        HFONT m_hFontLink  = nullptr; // small + underline, per Constants::Links
        int   m_cachedFontDpi = 0;

        HDC     m_bbDC     = nullptr;
        HBITMAP m_bbBmp    = nullptr;
        HBITMAP m_bbBmpOld = nullptr;
        int     m_bbW = 0, m_bbH = 0;
};

} // namespace UI
