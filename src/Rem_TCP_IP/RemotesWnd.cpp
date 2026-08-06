// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#include "RemotesWnd.h"
#include "RemoteMirror.h"
#include "RemotesFile.h"
#include "UI/LinkText.h"
#include "RemoteTls.h"   // RequiredForAddress — is a pin needed for this host?
#include "RemoteExec.h"   // BuildSyncPayload
#include "RemoteProtocol.h" // FormatEndpoint / StripAddressBrackets — IPv6 literals
#include "RemoteSettings.h" // SameHost — one address has many spellings

#include "AppState.h"
#include "Platform/Constants.h"
#include "Platform/ConstantsStrings.h"
#include "UI/ThemedDialog.h"
#include "UI/ThemedTooltip.h" // hover help — the glyph controls are not guessable
#include "UI/GdiPool.h" // pooled brushes and pens — never DeleteObject them

#include <algorithm>
#include <cwctype>    // towlower — fingerprint normalisation
#include <shobjidl.h> // IFileOpenDialog — Add from file
#include <windowsx.h>

extern AppState app;

namespace UI {

namespace RT = Constants::RemoteTcpIp;
namespace PC = Constants::Dedicated::PanelColors;

namespace {
    // Wide enough for eight buttons in ONE row without truncating any of them,
    // and the extra width goes to the address column, which is the one that
    // actually needs it.
    // Widened for the Identify button, which needs a real column of its own —
    // it is a press, not a glyph, and it sits beside the Link button.
    constexpr int PANEL_W  = 990;
    constexpr int PANEL_H  = 620;
    constexpr int PAD      = 14;
    constexpr int ROW_H    = 34;
    constexpr int FIELD_H  = 40;
    constexpr int HDR_H    = 26;
    constexpr int BTN_H    = 34;
    constexpr int BTN_GAP  = 8;
    constexpr int TITLE_H  = 46;
    constexpr int FOOTER_H = 30;

    // How long a dot stays amber after a start/stop before the panel re-polls
    // that row. Long enough for a launched instance to bind its port, short
    // enough that the console does not look stuck.
    constexpr UINT_PTR TIMER_PENDING    = 1;
    constexpr UINT     PENDING_DELAY_MS = 2500;

    enum ButtonId { BTN_SAVE = 1, BTN_NEW, BTN_IMPORT,
                    BTN_CONNECT_ALL, BTN_DISCONNECT_ALL,
                    BTN_POLL, BTN_SYNC_ALL, BTN_REMOVE,
                    // The read-only latch: BTN_EDIT unlocks the form, BTN_CANCEL
                    // locks it again without saving. See the note in Rebuild.
                    BTN_EDIT, BTN_CANCEL };

    bool BgIsDark(COLORREF bg) {
        const int lum = (GetRValue(bg) * 299 + GetGValue(bg) * 587 + GetBValue(bg) * 114) / 1000;
        return lum < 128;
    }

    // Lag reads as a number a human compares at a glance, not a raw count.
    // Loopback is sub-millisecond, so "0 ms" would look like a failed
    // measurement rather than a fast one.
    std::wstring FormatLag(long long us) {
        if (us < 0)    return L"—";
        if (us < 1000) return L"<1 ms";
        wchar_t b[32];
        swprintf_s(b, L"%.1f ms", static_cast<double>(us) / 1000.0);
        return b;
    }

    std::wstring OrUnset(const std::wstring &s) { return s.empty() ? L"(not set)" : s; }
}

// =============================================================================
// Init / Show
// =============================================================================
void RemotesWnd::Init(HINSTANCE hInstance, HWND hParent) {
    const float s = app.dpiScale;
    InitFloating(hInstance, hParent, L"qIVRemotesWnd", L"Remote Servers",
                 static_cast<int>(PANEL_W * s), static_cast<int>(PANEL_H * s));
    if (GetHwnd()) {
        SetWindowLongPtrW(GetHwnd(), GWL_EXSTYLE,
                          GetWindowLongPtrW(GetHwnd(), GWL_EXSTYLE) | WS_EX_LAYERED);
        SetLayeredWindowAttributes(GetHwnd(), 0,
                                   Constants::Dedicated::PANEL_OPACITY, LWA_ALPHA);
    }
    m_edit.SetMaxLength(260);
    BuildFields();
    Rebuild();
}

void RemotesWnd::Init(HINSTANCE hInstance, HWND hParent, int8_t) { Init(hInstance, hParent); }

void RemotesWnd::Show() {
    // The popup is owned by this window and is destroyed with it when the panel
    // closes, so what was last explained means nothing on reopening.
    m_tipOwner = nullptr;

    // Where this list came from. Same reasoning as the F9 panel: a populated
    // table and an empty one look the same whether they were read from disk or
    // are simply a session's worth of typing, and only the file makes them
    // survive a restart.
    //
    // Set before Rebuild so a row-derived footer (a target that is down) still
    // wins — that is the more urgent thing to say.
    if (Remote::RemotesFileExists()) {
        m_status     = Constants::Messages::REMOTE_PANEL_READ_FROM;
        m_statusPath = Remote::RemotesFilePath();
    } else {
        m_status = Constants::Messages::REMOTES_PANEL_NO_FILE;
        m_statusPath.clear();
    }

    BuildFields();
    Rebuild();
    ShowCenterOverParent();
    // Subscribe: a target coming up or dropping while this sits open now says
    // so, instead of waiting for the next click to force a repaint.
    Remote::Mirror::AddPanelNotify(GetHwnd());
    // Opening the console is the moment you want to know what is up, so poll
    // immediately rather than showing a table of dashes until F5 is pressed.
    DoPollAll();
    Repaint();
}

void RemotesWnd::Hide() {
    // Unsubscribe FIRST, so no sender thread can post to a window on its way
    // out. A closed console costs them one atomic load.
    Remote::Mirror::RemovePanelNotify(GetHwnd());
    FloatingPanelWnd::Hide();
}

// =============================================================================
// Startup auto-connect
// =============================================================================
void RemotesWnd::AutoConnectAll(HWND hOwner) {
    Remote::Mirror::SetOwner(hOwner);

    // EVERY saved row is loaded; only the ones marked AutoConnect are dialled.
    //
    // Loading only the auto-connecting ones was a bug with an obvious symptom
    // and a confusing cause: a remote added yesterday was still in
    // qivRemoteServers.ini, but the console came up empty, because the console shows
    // Remote::Mirror's targets and nothing had put the row there. The list is a
    // record of what exists; connecting is a separate question about each one.
    for (const Remote::RemoteEntry &e : Remote::LoadRemotes()) {
        // Returns immediately either way — a connection is made on the target's
        // own thread, so a screen that is switched off does not delay startup.
        (void) Remote::Mirror::AddTarget(e.name, e.host, e.port, e.password,
                                         e.exePath, e.pin, e.autoConnect);
    }
}

// =============================================================================
// The New-connection form
// =============================================================================
void RemotesWnd::BuildFields() {
    m_fields.clear();
    auto add = [&](FieldId id, const wchar_t *label, std::wstring value,
                   const wchar_t *desc, bool secret = false) {
        Field f;
        f.id = id; f.label = label; f.value = std::move(value);
        f.desc = desc; f.secret = secret;
        m_fields.push_back(std::move(f));
    };

    add(F_HOST, L"Address", OrUnset(m_newHost),
        L"Address or host name of the instance to drive. 127.0.0.1 for another copy on this machine.");
    add(F_PORT, L"Port",
        m_newPort == 0 ? std::wstring(L"(not set)") : std::to_wstring(m_newPort),
        L"The port that instance is listening on — its PortNo, not this one's.");
    add(F_PASSWORD, L"Password",
        m_newPassword.empty() ? L"(none)"
                              : (Remote::IsStoredSecret(m_newPassword) ? L"(imported)" : L"(set)"),
        L"Its password, if it has one. Answers the challenge; never sent as text. "
        L"Add from file brings this across, so there is nothing to type.", true);
    // Shown as a truncated prefix rather than 64 hex digits: the field is wide
    // enough for a name, not a digest, and the first bytes are what anyone
    // actually compares against the server's F9 panel.
    add(F_PIN, L"TLS fingerprint",
        m_newPin.empty()
            ? std::wstring(Remote::Tls::RequiredForAddress(m_newHost) ? L"(required)"
                                                                      : L"(not needed)")
            : m_newPin.substr(0, 16) + L"…",
        L"SHA-256 of that instance's certificate — read it off its F9 panel. Required "
        L"for any address other than loopback; Add from file fills it in when the "
        L"target's folder is reachable.");
    add(F_NAME, L"Name", m_newName.empty() ? L"(required)" : m_newName,
        L"REQUIRED. Identifies this remote — in the list, in messages about it, and when "
        L"matching it up again later. Add from file fills it in.");
    add(F_EXE, L"Exe to launch", OrUnset(m_newExe),
        L"Optional. Lets the ● start this instance when it is down — only possible on this machine.");
}

void RemotesWnd::EditField(int fieldIndex) {
    if (fieldIndex < 0 || fieldIndex >= static_cast<int>(m_fields.size())) return;
    // The latch, checked at the ONE place every field edit starts — keyboard and
    // mouse both land here, so neither can get in behind the other's back.
    if (m_formLocked) return;

    if (m_fields[fieldIndex].id == F_PORT) {
        const int v = DialogPromptInt(L"Target Port", L"Port (1 - 65535):",
                                      m_newPort ? m_newPort : RT::PORT_DEFAULT,
                                      RT::PORT_MIN, RT::PORT_MAX, RT::PORT_DEFAULT);
        if (v >= 0) m_newPort = v;
        BuildFields();
        Repaint();
        return;
    }
    BeginTextEdit(fieldIndex);
}

void RemotesWnd::BeginTextEdit(int fieldIndex) {
    m_editingField = fieldIndex;
    const Field &f = m_fields[fieldIndex];

    // A secret is never seeded with what is held: pre-filling a placeholder
    // would let an accidental Enter overwrite the real value with literal text.
    std::wstring seed;
    if (!f.secret) {
        switch (f.id) {
            case F_HOST: seed = m_newHost; break;
            case F_NAME: seed = m_newName; break;
            case F_EXE:  seed = m_newExe;  break;
            // Seeded with the FULL value, not the truncated display form —
            // editing a field must never silently shorten what it holds.
            case F_PIN:  seed = m_newPin;  break;
            default: break;
        }
    }
    m_edit.SetText(seed);
    Repaint();
}

void RemotesWnd::CommitTextEdit() {
    if (m_editingField < 0) return;
    const std::wstring text = m_edit.GetText();

    switch (m_fields[m_editingField].id) {
        // Brackets stripped for the same reason the pin below drops colons: the
        // panel PRINTS an IPv6 literal bracketed, so pasting one back in is the
        // ordinary case, and getaddrinfo rejects the brackets.
        case F_HOST:     m_newHost     = Remote::StripAddressBrackets(text); break;
        case F_PASSWORD: m_newPassword = text; break;
        case F_NAME:     m_newName     = text; break;
        case F_EXE:      m_newExe      = text; break;
        // Normalised on entry: a fingerprint is compared case-insensitively but
        // stored lower-case, and pasting from a tool that prints colons or
        // spaces between bytes is the ordinary case rather than an error.
        case F_PIN: {
            std::wstring clean;
            for (wchar_t c : text) {
                if (c == L':' || c == L' ' || c == L'-') continue;
                clean += static_cast<wchar_t>(::towlower(c));
            }
            m_newPin = clean;
            break;
        }
        default: break;
    }

    m_editingField = -1;
    BuildFields();
    Repaint();
}

void RemotesWnd::CancelTextEdit() {
    m_editingField = -1;
    Repaint();
}

// =============================================================================
// Model
// =============================================================================
void RemotesWnd::Rebuild() {
    // Preserve the dots that are mid-action: Targets() reports what the sender
    // threads know, and a row waiting for a launched instance to bind its port
    // is legitimately neither up nor down yet.
    std::vector<int> pending;
    for (const RowView &r : m_rows)
        if (r.dot == DotState::Pending) pending.push_back(r.id);

    m_rows.clear();
    for (const Remote::Mirror::TargetView &t : Remote::Mirror::Targets()) {
        RowView r;
        // The mirror's own handle, not the row position — removing a target
        // does not renumber the rest, so a position would start addressing the
        // wrong screen after any removal.
        r.id          = t.id;
        r.name        = t.name;
        r.host        = t.host;
        r.port        = t.port;
        r.exePath     = t.exePath;
        r.sameMachine = t.sameMachine;
        r.connecting  = t.connecting;
        r.lagUs       = t.lagUs;
        r.observing   = t.observing;
        r.lastError   = t.lastError;
        r.down        = t.down;
        r.dot = t.connected      ? DotState::Up
              : t.connecting     ? DotState::Down   // trying, and not getting there
                                 : DotState::Idle;  // listed, nobody asked

        // A saved list outlives the instances it names: an exe gets moved,
        // renamed or deleted and the row keeps pointing at where it used to be.
        // Checked here so the row can SAY so, rather than the start button
        // failing with a Win32 error code when it is eventually pressed.
        r.exeMissing = !r.exePath.empty() &&
                       GetFileAttributesW(r.exePath.c_str()) == INVALID_FILE_ATTRIBUTES;

        if (std::find(pending.begin(), pending.end(), r.id) != pending.end() &&
            r.dot == DotState::Down)
            r.dot = DotState::Pending;

        m_rows.push_back(std::move(r));
    }

    const bool haveRows = !m_rows.empty();
    const bool editing  = m_editingRowId != 0;

    // Connecting ONE row is the Connect/Disconnect button IN that row, not a
    // toolbar button — a toolbar button acting on "the selected row" made it
    // ambiguous which row a press referred to, when the same click that selects
    // a row also loads it into the form. These two act on everything, which
    // needs no selection at all.
    m_buttons.clear();

    // THE FORM IS READ-ONLY UNTIL YOU SAY OTHERWISE.
    //
    // Clicking a row loads it into the form, and the fields used to be live the
    // moment it landed there — so a stray click on an address, one keystroke,
    // and a working remote was quietly repointed at nothing. The form is a
    // VIEW of the selected row by default; editing is a decision.
    //
    // So the first pair of buttons swaps between two modes rather than sitting
    // there permanently: Update/New to get in, Save/Cancel to get out. Two
    // buttons either way, in the same two positions, so nothing moves.
    if (m_formLocked) {
        m_buttons.push_back({L"Update", BTN_EDIT, {}, haveRows,
            L"Unlock the form so the selected remote can be corrected in place.\n"
            L"Until then the fields are read-only — a stray click on an address is "
            L"otherwise one keystroke away from repointing a working remote."});
        m_buttons.push_back({L"New", BTN_NEW, {}, true,
            L"Clear the form, unlock it, and start describing a new remote."});
    } else {
        m_buttons.push_back({editing ? L"Save changes" : L"Save new", BTN_SAVE, {}, true,
            editing ? L"Apply the form to the remote being edited, and lock it again."
                    : L"Record the form as a new remote, and lock it again.\nSaving does "
                      L"not connect — that is the Connect button in its row, or Connect all."});
        m_buttons.push_back({L"Cancel", BTN_CANCEL, {}, true,
            L"Discard the changes and lock the form again.\nThe remote is left exactly as "
            L"it was."});
    }
    m_buttons.push_back({L"Add from file…", BTN_IMPORT, {}, true,
        L"Read another instance's qivLocalServer.ini — or its .exe, and the file beside it "
        L"is found automatically.\nBrings across its port, name, exe and credentials, so "
        L"there is nothing to type."});
    m_buttons.push_back({L"Connect all", BTN_CONNECT_ALL, {}, haveRows,
        L"Start dialling every remote in the list.\nEach keeps retrying on its own if it is "
        L"not answering yet."});
    m_buttons.push_back({L"Disconnect all", BTN_DISCONNECT_ALL, {}, haveRows,
        L"Drop every connection. The rows stay in the list.\nEnds the session, so the "
        L"restrictions on delete, Find and disk-order sort lift."});
    m_buttons.push_back({L"Poll (F5)", BTN_POLL, {}, haveRows,
        L"Ping every connected remote and fill in the Lag column.\nLag is the round trip on "
        L"the link in use — on this machine it mostly reports how busy that viewer is."});
    m_buttons.push_back({L"Sync all", BTN_SYNC_ALL, {}, haveRows,
        L"Push THIS viewer's look onto every remote: sort order, view mode, rotation, flips, "
        L"colour adjustments and the effect list in order.\n"
        L"Mirroring sends toggles, and a toggle applied to a different starting state "
        L"inverts instead of matching — this replaces their state outright.\n"
        L"One-directional and immediate. Nothing comes back, and there is no undo."});
    m_buttons.push_back({L"Remove", BTN_REMOVE, {}, haveRows,
        L"Delete the selected remote from the list and from qivRemoteServers.ini.\nThe instance "
        L"itself is not affected."});

    if (m_selectedRow >= static_cast<int>(m_rows.size()))
        m_selectedRow = std::max(0, static_cast<int>(m_rows.size()) - 1);
}

void RemotesWnd::PersistRows() {
    // Password and autoConnect are PRESERVED from the file, not invented here.
    // The panel never displays a password, and it must not decide on the user's
    // behalf that a remote should be reconnected at startup — that flag is
    // hand-set by someone who wants a screen wall to come up already joined,
    // and saving the list must not silently turn it on for everyone else.
    std::vector<Remote::RemoteEntry> stored = Remote::LoadRemotes();
    std::vector<Remote::RemoteEntry> out;

    for (const RowView &r : m_rows) {
        Remote::RemoteEntry e;
        e.name    = r.name;
        e.host    = r.host;
        e.port    = r.port;
        e.exePath = r.exePath;

        // Matched by NAME, because that is the identity — a row whose port or
        // folder was corrected keeps its password and its autoConnect, where
        // matching on host+port would silently drop both.
        for (const Remote::RemoteEntry &s : stored) {
            if (_wcsicmp(s.name.c_str(), r.name.c_str()) == 0) {
                e.password    = s.password;
                e.autoConnect = s.autoConnect;
                break;
            }
        }
        out.push_back(std::move(e));
    }
    Remote::SaveRemotes(out);
}

// =============================================================================
// Actions
// =============================================================================
void RemotesWnd::FillFormFromRow(int row) {
    if (row < 0 || row >= static_cast<int>(m_rows.size())) return;
    if (m_editingField >= 0) CancelTextEdit();

    const RowView &r = m_rows[row];
    m_editingRowId = r.id;
    m_newHost      = r.host;
    m_newPort      = r.port;
    m_newName      = r.name;
    m_newExe       = r.exePath;
    m_newPin.clear();

    // The credential is not carried in the row — the panel never displays one —
    // so it comes back out of the file, matched by name. Without this, saving an
    // edited row would silently drop the password it was connecting with.
    m_newPassword.clear();
    for (const Remote::RemoteEntry &e : Remote::LoadRemotes()) {
        if (_wcsicmp(e.name.c_str(), r.name.c_str()) == 0) {
            m_newPassword = e.password;
            m_newPin      = e.pin;   // same reason: not carried in the row view
            break;
        }
    }

    BuildFields();
    Rebuild();
    // Loading a row does NOT unlock the form. Selecting is looking; editing is a
    // decision, and it is the Update button.
    m_status = m_formLocked
                   ? (L"Showing \"" + r.name + L"\" — press Update to change it")
                   : (L"Editing \"" + r.name + L"\" — Save changes, or Cancel");
    Repaint();
}

void RemotesWnd::DoBeginEdit() {
    if (m_rows.empty()) return;

    // Whatever row is selected, loaded fresh: the form may still be showing a
    // row that was clicked several selections ago if nothing reloaded it.
    FillFormFromRow(m_selectedRow);

    m_formLocked = false;
    Rebuild();   // Update/New become Save changes/Cancel
    m_status = L"Editing \"" + m_newName + L"\" — Save changes, or Cancel";
    Repaint();
}

void RemotesWnd::DoCancelEdit() {
    if (m_editingField >= 0) CancelTextEdit();

    m_formLocked = true;

    // Reloaded from the target list rather than merely locked, so a half-typed
    // address does not sit there looking like the saved value.
    if (m_editingRowId != 0) {
        for (size_t i = 0; i < m_rows.size(); ++i) {
            if (m_rows[i].id != m_editingRowId) continue;
            FillFormFromRow(static_cast<int>(i));
            break;
        }
    } else {
        m_newHost = L"127.0.0.1";
        m_newPort = 0;
        m_newName.clear();
        m_newPassword.clear();
        m_newExe.clear();
        m_newPin.clear();
        BuildFields();
    }

    Rebuild();
    m_status = L"Nothing was changed.";
    Repaint();
}

void RemotesWnd::DoNewEntry() {
    if (m_editingField >= 0) CancelTextEdit();
    m_editingRowId = 0;
    m_newHost      = L"127.0.0.1";
    m_newPort      = 0;
    m_newName.clear();
    m_newPassword.clear();
    m_newExe.clear();
    m_newPin.clear();
    // New is the other way IN to editing — there is nothing to describe a new
    // remote with while the form is read-only.
    m_formLocked = false;
    BuildFields();
    Rebuild();
    m_status = L"New remote — fill in address, port and name, then Save new";
    Repaint();
}

void RemotesWnd::DoSaveEntry() {
    if (m_editingField >= 0) CommitTextEdit();

    if (m_newHost.empty() || m_newPort == 0) {
        DialogMessage(L"Fill in the address and port first.", L"Remote Servers");
        return;
    }

    // A name is REQUIRED, because it is the identity: the console shows it, a
    // failure message names it, and it is what makes a row the same row after
    // its port or folder changes. It used to be optional only because a
    // successful connection could borrow the target's own name out of its
    // banner — and saving no longer requires connecting.
    if (m_newName.empty()) {
        DialogMessage(L"Give this remote a Name.\r\n\r\nNames identify a remote — in the "
                      L"list, in every message about it, and when matching it up again "
                      L"after its port or folder changes.\r\n\r\nAdd from file… fills it in "
                      L"from the instance's own settings.", L"Remote Servers");
        return;
    }

    // Two separate constraints, two separate messages — they mean different
    // mistakes and have different fixes. The row being EDITED is skipped, or it
    // would collide with itself.
    for (const RowView &r : m_rows) {
        if (r.id == m_editingRowId) continue;

        // Same instance twice: it would receive every mirrored command once per
        // row, and the console would show two dots for one screen.
        // SameHost, not string equality: "fe80::1" and "fe80:0:0:0:0:0:0:1" are
        // one machine and would otherwise both be admitted, giving one instance
        // two rows — each receiving every mirrored command once.
        if (Remote::SameHost(r.host, m_newHost) && r.port == m_newPort) {
            DialogMessage(Remote::FormatEndpoint(m_newHost, m_newPort) +
                          L" is already in the list, as \"" + r.name + L"\".",
                          L"Remote Servers");
            return;
        }
        // Same name twice: the name is the identity, so every message that
        // named it would be ambiguous.
        if (_wcsicmp(r.name.c_str(), m_newName.c_str()) == 0) {
            DialogMessage(L"There is already a remote called \"" + r.name +
                          L"\" (" + Remote::FormatEndpoint(r.host, r.port) +
                          L").\r\n\r\nNames identify a remote, so they have to be "
                          L"distinct — give this one a different name.", L"Remote Servers");
            return;
        }
    }

    // A pin is REQUIRED off loopback. Refused here rather than at connect time:
    // the row would otherwise save cleanly, sit in the list looking correct, and
    // fail every attempt with an error about a fingerprint — at a moment when
    // the panel that could fix it is not open.
    if (Remote::Tls::RequiredForAddress(m_newHost) && m_newPin.empty()) {
        DialogMessage(m_newHost + L" is not loopback, so that instance speaks TLS and "
                      L"this row needs its certificate fingerprint.\r\n\r\n"
                      L"Read it from that instance's F9 panel and paste it into the "
                      L"TLS fingerprint field — or use Add from file if you can reach "
                      L"its folder, which fills it in for you.",
                      L"Remote Servers");
        return;
    }

    Remote::RemoteEntry e;
    e.name     = m_newName;
    e.host     = m_newHost;
    e.port     = m_newPort;
    e.password = m_newPassword;
    e.exePath  = m_newExe;
    e.pin      = m_newPin;
    // Recorded, not dialled — on this launch or the next. Reconnecting at
    // startup is a deliberate choice for a screen wall, set in the file rather
    // than assumed for everything that was ever added.
    e.autoConnect = false;

    // The name being replaced, so the file row can be found even when the name
    // itself is what changed.
    std::wstring oldName;
    bool wasConnecting = false;
    for (const RowView &r : m_rows) {
        if (r.id == m_editingRowId) { oldName = r.name; wasConnecting = r.connecting; break; }
    }

    std::vector<Remote::RemoteEntry> list = Remote::LoadRemotes();
    if (!oldName.empty()) {
        bool replaced = false;
        for (Remote::RemoteEntry &s : list) {
            if (_wcsicmp(s.name.c_str(), oldName.c_str()) == 0) {
                // autoConnect is the file's to keep — it says what should happen
                // at startup, which is not something this form asks about.
                e.autoConnect = s.autoConnect;
                s = e;
                replaced = true;
                break;
            }
        }
        if (!replaced) list.push_back(e);

        // The live target is rebuilt rather than mutated: its address, port or
        // credential may all have changed, and its sender thread holds an open
        // connection to whatever it used to be.
        Remote::Mirror::RemoveTarget(m_editingRowId);
    } else {
        list.push_back(e);
    }
    Remote::SaveRemotes(list);

    // Reconnected only if it was connected before the edit — saving a change is
    // not a request to start driving something that was sitting idle.
    (void) Remote::Mirror::AddTarget(e.name, e.host, e.port, e.password,
                                     e.exePath, e.pin, wasConnecting);

    const bool wasEdit = !oldName.empty();
    m_editingRowId = 0;
    // The form is cleared: it described a remote that now exists as a row, and
    // leaving it filled in invites adding the same one twice.
    m_newName.clear();
    m_newPassword.clear();
    m_newExe.clear();

    // Locked again. A save is the end of an edit, so the fields go back to being
    // read-only without a second press — leaving them live would put the
    // accidental-keystroke hole straight back.
    m_formLocked = true;

    BuildFields();
    Rebuild();
    m_status = wasEdit ? (L"Updated \"" + e.name + L"\"")
                       : (L"Saved \"" + e.name + L"\" — press Connect to reach it");
    Repaint();
}

void RemotesWnd::DoToggleConnect(int row) {
    if (row < 0 || row >= static_cast<int>(m_rows.size())) return;
    const RowView &r = m_rows[row];

    // The press does what its LABEL says, and the label comes from the dot:
    //   Up      → "Disconnect"  → drop it
    //   Pending → "Connecting…" → the press cancels the dial in progress
    //   Idle/Down → "Connect"   → dial, or retry now
    // Toggling the wish flag instead would leave a row that failed to connect
    // showing "Disconnect", and the press would then silently ABANDON the
    // retry the user thought they were asking for. SetConnecting(id, true) on a
    // target that already wants to connect notifies its condition variable, so
    // a second press cuts the reconnect back-off short — a real retry, not a
    // no-op.
    const bool on = (r.dot != DotState::Up && r.dot != DotState::Pending);
    const std::wstring name = r.name; // Rebuild() invalidates the row
    Remote::Mirror::SetConnecting(r.id, on);

    // The outcome arrives on the sender thread; the poll timer brings it into
    // view. Saying what was ASKED for, rather than claiming a result that has
    // not happened yet.
    m_status = on ? (L"Connecting to " + name + L"…")
                  : (L"Disconnected from " + name);
    Rebuild();
    if (on) SetTimer(GetHwnd(), TIMER_PENDING, PENDING_DELAY_MS, nullptr);
    Repaint();
}

void RemotesWnd::DoIdentify(int row) {
    if (row < 0 || row >= static_cast<int>(m_rows.size())) return;
    const RowView &r = m_rows[row];

    // The row's OWN name, so the screen answers the question the button asks:
    // which of you is this? Falls back to the address, which is still unique and
    // still tells the two apart.
    const std::wstring who = r.name.empty()
                                 ? Remote::FormatEndpoint(r.host, r.port)
                                 : r.name;

    // SendTo, not a broadcast: every target gets a DIFFERENT text — its own
    // name — so there is nothing here to fan out.
    Remote::Mirror::SendTo(r.id, L"msgRemote " + who);

    m_status = L"Told " + who + L" to show its name on screen";
    Repaint();
}

void RemotesWnd::DoConnectAll(bool on) {
    Remote::Mirror::SetConnectingAll(on);
    m_status = on ? (L"Connecting to " + std::to_wstring(m_rows.size()) + L" remote(s)…")
                  : L"Disconnected from every remote";
    Rebuild();
    if (on) SetTimer(GetHwnd(), TIMER_PENDING, PENDING_DELAY_MS, nullptr);
    Repaint();
}

void RemotesWnd::DoImportFromFile() {
    if (m_editingField >= 0) CommitTextEdit();

    // Either half identifies the instance: pick its exe and the listener file
    // is found in the same folder by its fixed name, or pick qivLocalServer.ini
    // directly — which is also the way in for a copy started with -config.
    PushTopmostOff();
    std::wstring chosen;
    {
        IFileOpenDialog *pfd = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                       CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd))) && pfd) {
            COMDLG_FILTERSPEC filters[] = {
                {L"qIV instance (*.ini; *.exe)",   L"*.ini;*.exe"},
                {L"Listener file (qivLocalServer.ini)", L"qivLocalServer.ini"},
                {L"Program (*.exe)",               L"*.exe"},
            };
            pfd->SetFileTypes(ARRAYSIZE(filters), filters);
            DWORD opts = 0;
            pfd->GetOptions(&opts);
            pfd->SetOptions(opts | FOS_FILEMUSTEXIST);
            if (SUCCEEDED(pfd->Show(GetHwnd()))) {
                IShellItem *psi = nullptr;
                if (SUCCEEDED(pfd->GetResult(&psi)) && psi) {
                    PWSTR path = nullptr;
                    if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                        chosen = path;
                        CoTaskMemFree(path);
                    }
                    psi->Release();
                }
            }
            pfd->Release();
        }
    }
    PopTopmost();
    if (chosen.empty()) return;

    Remote::RemoteEntry e;
    std::wstring problem, warning;
    if (!Remote::ImportFromInstanceFile(chosen, e, problem, warning)) {
        DialogMessage(problem, L"Add from file");
        return;
    }

    // Warnings describe something at the OTHER end that will stop the
    // connection completing — a disabled listener, an AllowList that excludes
    // this machine. Both are invisible from outside: the attempt would simply
    // time out. Offered as a choice rather than a refusal, because the user may
    // be about to go and fix it, or may want the row recorded either way.
    if (!warning.empty()) {
        if (!DialogConfirm(L"Imported " + e.name + L" — but it will not answer yet.\r\n\r\n" +
                           warning + L"\r\n\r\nFill the form in anyway?",
                           L"Add from file"))
            return;
    }

    // An import describes a NEW remote, so it leaves any row being edited alone
    // rather than quietly overwriting it.
    m_editingRowId = 0;
    m_newHost      = e.host;
    m_newPort      = e.port;
    m_newName      = e.name;
    m_newExe       = e.exePath;
    m_newPassword  = e.password; // may be a "secret:" field — never displayed

    BuildFields();
    m_status = e.password.empty()
                   ? (L"Imported " + e.name + L" — press Connect && Save")
                   : (L"Imported " + e.name + L" with its credentials — press Connect && Save");
    Repaint();
}

void RemotesWnd::DoPollAll() {
    Remote::Mirror::PingAll();
    // Same beat as the ping: the poll is where the console re-asks the world
    // what is true, and "is that row on this machine?" is one of those answers.
    // Costs nothing here — it raises a flag and returns.
    Remote::Mirror::RefreshSameMachine();
    m_status = L"Polling…";
    // The replies land on the sender threads; the timer brings the answers into
    // view without this function ever waiting for one.
    SetTimer(GetHwnd(), TIMER_PENDING, PENDING_DELAY_MS, nullptr);
    Repaint();
}

void RemotesWnd::DoRemoveTarget(int row) {
    if (row < 0 || row >= static_cast<int>(m_rows.size())) return;

    // COPIED, NOT REFERENCED — the same use-after-free that crashed
    // RemoteClientsWnd::DoKick on 2026-08-06.
    //
    // DialogConfirm runs a modal message loop, so this window keeps receiving
    // messages while the box is up, and TWO of them rebuild m_rows:
    // WM_QIV_REMOTE_TARGETS_CHANGED when a target connects or drops, and
    // TIMER_PENDING, which several actions here arm. A pending timer is
    // GUARANTEED to fire during the wait, so this was more reachable than the
    // crash that was actually observed.
    //
    // The row reference would then point into a freed buffer, and `r.id` below
    // — used to decide what to remove — would be read out of it.
    const auto id        = m_rows[row].id;
    const std::wstring name = m_rows[row].name;
    const std::wstring host = m_rows[row].host;
    const int          port = m_rows[row].port;

    if (!DialogConfirm(L"Remove " + name + L" (" + Remote::FormatEndpoint(host, port) +
                       L") from the list?\r\n\r\n"
                       L"The instance itself is not affected.",
                       L"Remote Servers"))
        return;

    const bool wasEditing = (id == m_editingRowId);

    Remote::Mirror::RemoveTarget(id);
    Rebuild();
    PersistRows();

    // The form was describing the row that no longer exists — saving it would
    // silently re-create what was just removed.
    if (wasEditing) DoNewEntry();
    Repaint();
}

void RemotesWnd::DoStartTarget(int row) {
    if (row < 0 || row >= static_cast<int>(m_rows.size())) return;
    RowView &r = m_rows[row];

    // CreateProcess starts a process on THIS machine. Nothing here can launch
    // one elsewhere without an agent already running there, so for a remote row
    // the button has nothing it can do — say so rather than starting a second
    // copy of the viewer here, which is what the naive version would do.
    if (!r.sameMachine) {
        DialogMessage(r.name + L" is on another machine.\r\n\r\n"
                      L"It can only be started at that machine — nothing here can "
                      L"launch a process on it. Stopping it works, because that "
                      L"travels down the connection it already has open.",
                      L"Remote Servers");
        return;
    }

    if (r.exePath.empty()) {
        DialogMessage(L"No exe recorded for this remote, so there is nothing to "
                      L"launch.\r\n\r\nRemove it and add it again with the exe "
                      L"path filled in to enable the start button.", L"Remote Servers");
        return;
    }

    // Re-checked at the moment of use, not only when the list was built: a
    // console can sit open for a long time, and the file may have gone in
    // between.
    if (GetFileAttributesW(r.exePath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        DialogMessage(L"That exe is no longer there:\r\n\r\n    " + r.exePath +
                      L"\r\n\r\nIt has been moved, renamed or deleted. Remove this "
                      L"row and add it again from the instance's current location.",
                      L"Remote Servers");
        return;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    // Non-const buffer: CreateProcessW may write to the command-line argument.
    std::wstring cmd = L"\"" + r.exePath + L"\"";
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                                   0, nullptr, nullptr, &si, &pi);
    if (!ok) {
        m_status = L"Launch failed (" + std::to_wstring(GetLastError()) + L") — " + r.exePath;
        Repaint();
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    // Amber until the re-poll: the instance needs a moment to bind its port, and
    // a dot that stayed red would read as a click that failed.
    r.dot    = DotState::Pending;
    m_status = L"Launched " + r.name + L" — waiting for it to listen…";
    SetTimer(GetHwnd(), TIMER_PENDING, PENDING_DELAY_MS, nullptr);
    Repaint();
}

void RemotesWnd::DoStopTarget(int row) {
    if (row < 0 || row >= static_cast<int>(m_rows.size())) return;
    RowView &r = m_rows[row];

    if (!DialogConfirm(L"Shut down " + r.name + L"?\r\n\r\n"
                       L"It exits immediately without saving its session.",
                       L"Remote Servers"))
        return;

    // The existing HardQuit, down the connection already open. Note this is a
    // command the MIRROR deny-list refuses to fan out — one Ctrl+Q must never
    // take every screen down at once — but a deliberate per-row button is
    // exactly the case that deny-list exists to leave available.
    Remote::Mirror::SendTo(r.id, L"HardQuit");
    r.dot    = DotState::Pending;
    m_status = L"Stopping " + r.name + L"…";
    SetTimer(GetHwnd(), TIMER_PENDING, PENDING_DELAY_MS, nullptr);
    Repaint();
}

void RemotesWnd::DoSyncAll() {
    // One push of this instance's whole view state. Mirroring forwards toggles,
    // and a toggle applied to a different starting state diverges — this is the
    // cure rather than the prevention, which is the cheaper trade for a set of
    // screens that normally start clean.
    //
    // Two spellings: the folder travels only to instances that share this
    // filesystem. A drive letter means nothing on another machine.
    Remote::Mirror::BroadcastSync(L"Sync " + Remote::BuildSyncPayload(true),
                                  L"Sync " + Remote::BuildSyncPayload(false));
    m_status = L"Pushed folder, image and view state to " +
               std::to_wstring(m_rows.size()) + L" remote(s)";
    Repaint();
}

// =============================================================================
// Dialog helpers — the panel is topmost, themed dialogs are not, so a dialog
// would open BEHIND it. Drop topmost for the duration and restore after.
// =============================================================================
void RemotesWnd::PushTopmostOff() {
    if (GetHwnd())
        SetWindowPos(GetHwnd(), HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void RemotesWnd::PopTopmost() {
    if (GetHwnd())
        SetWindowPos(GetHwnd(), HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void RemotesWnd::DialogMessage(const std::wstring &text, const wchar_t *caption) {
    PushTopmostOff();
    ThemedDialog::Message(GetHwnd(), text.c_str(), caption);
    PopTopmost();
}

bool RemotesWnd::DialogConfirm(const std::wstring &text, const wchar_t *caption) {
    PushTopmostOff();
    const bool r = ThemedDialog::Confirm(GetHwnd(), text.c_str(), caption);
    PopTopmost();
    return r;
}

int RemotesWnd::DialogPromptInt(const wchar_t *caption, const wchar_t *label,
                                int cur, int lo, int hi, int def) {
    PushTopmostOff();
    const int r = ThemedDialog::PromptInt(GetHwnd(), caption, label, cur, lo, hi, def);
    PopTopmost();
    return r;
}

// =============================================================================
// Paint plumbing
// =============================================================================
void RemotesWnd::EnsureFonts(HDC dc) {
    const int dpi = GetDeviceCaps(dc, LOGPIXELSY);
    if (m_hFontBody && dpi == m_cachedFontDpi) return;
    if (m_hFontBody)  DeleteObject(m_hFontBody);
    if (m_hFontBold)  DeleteObject(m_hFontBold);
    if (m_hFontSmall) DeleteObject(m_hFontSmall);
    if (m_hFontLink)  DeleteObject(m_hFontLink);
    m_cachedFontDpi = dpi;
    auto mk = [&](int pt, int w, BOOL underline = FALSE) {
        return CreateFontW(-MulDiv(pt, dpi, 72), 0, 0, 0, w, FALSE, underline, FALSE,
                           DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
    };
    m_hFontBody  = mk(10, FW_NORMAL);
    m_hFontBold  = mk(11, FW_SEMIBOLD);
    m_hFontSmall = mk(8,  FW_NORMAL);
    m_hFontLink  = mk(8,  FW_NORMAL, Constants::Links::UNDERLINE ? TRUE : FALSE);
}

void RemotesWnd::EnsureBackBuffer(HDC refDC, int w, int h) {
    if (m_bbDC && m_bbW == w && m_bbH == h) return;
    DestroyBackBuffer();
    m_bbDC     = CreateCompatibleDC(refDC);
    m_bbBmp    = CreateCompatibleBitmap(refDC, w, h);
    m_bbBmpOld = static_cast<HBITMAP>(SelectObject(m_bbDC, m_bbBmp));
    m_bbW = w; m_bbH = h;
}

void RemotesWnd::DestroyBackBuffer() {
    if (!m_bbDC) return;
    SelectObject(m_bbDC, m_bbBmpOld);
    DeleteObject(m_bbBmp);
    DeleteDC(m_bbDC);
    m_bbDC = nullptr; m_bbBmp = nullptr; m_bbBmpOld = nullptr; m_bbW = m_bbH = 0;
}

void RemotesWnd::Repaint() { if (GetHwnd()) InvalidateRect(GetHwnd(), nullptr, FALSE); }

void RemotesWnd::ShowTipAt(const RECT &clientRect, const wchar_t *text) {
    if (!GetHwnd() || !text || !*text) return;

    RECT anchor = clientRect;
    MapWindowPoints(GetHwnd(), nullptr, reinterpret_cast<POINT *>(&anchor), 2);

    // Below-right of the control rather than under the cursor, so the popup
    // never sits on top of the thing it is describing.
    POINT at{anchor.left, anchor.bottom + static_cast<int>(4 * app.dpiScale)};
    ThemedTooltip::Show(GetHwnd(), text, at, anchor);
}

void RemotesWnd::UpdateTip(POINT pt) {
    // ThemedTooltip dismisses itself when the cursor leaves the anchor rect, so
    // it can be gone without this panel being told. Forget what was shown in
    // that case, or moving back onto the same control would show nothing.
    if (!ThemedTooltip::IsVisible()) m_tipOwner = nullptr;

    // One pass over everything hoverable. The identity of the control is used to
    // suppress re-showing the same popup on every pixel of movement inside it —
    // ThemedTooltip would happily move the window each time otherwise.
    const void *owner = nullptr;
    const wchar_t *text = nullptr;
    RECT rect{};

    if (const int b = HitTestButton(pt); b >= 0) {
        owner = &m_buttons[b]; text = m_buttons[b].tip; rect = m_buttons[b].rect;
    } else if (const int d = HitTestDot(pt); d >= 0) {
        owner = &m_rows[d].dotRect;
        rect  = m_rows[d].dotRect;
        text  = m_rows[d].dot == DotState::Up
                    ? L"Running. Click to shut this instance down.\nIt exits immediately "
                      L"without saving its session."
                    : L"Click to launch this instance's exe.\nOnly possible for an instance "
                      L"on this machine, and only if an exe was recorded for it.";
    } else if (const int id = HitTestIdentify(pt); id >= 0) {
        owner = &m_rows[id].idRect;
        rect  = m_rows[id].idRect;
        text  = L"Make that screen say its own name, in the middle of its window.\n"
                L"Two identical viewers side by side are otherwise anonymous — this "
                L"is how you tell which row drives which.";
    } else if (const int l = HitTestLink(pt); l >= 0) {
        owner = &m_rows[l].linkRect;
        rect  = m_rows[l].linkRect;
        text  = m_rows[l].dot == DotState::Up
                    ? L"Connected. Click to disconnect.\nThe row stays in the list."
                : m_rows[l].dot == DotState::Pending
                    ? L"Dialling. Click to give up on it.\nNothing is connected yet."
                : m_rows[l].connecting
                    ? L"Asked for, but NOT connected — the other end is not answering.\n"
                      L"Click to retry now instead of waiting for the next attempt."
                    : L"Listed but not dialled. Click to connect.\nBeing in the list does "
                      L"not mean being connected to.";
    } else if (const int f = HitTestField(pt); f >= 0) {
        owner = &m_fields[f]; text = m_fields[f].desc; rect = m_fields[f].rect;
    }

    if (!text || !*text) {
        if (m_tipOwner) { ThemedTooltip::Hide(); m_tipOwner = nullptr; }
        return;
    }
    if (owner == m_tipOwner) return;

    m_tipOwner = owner;
    ShowTipAt(rect, text);
}

int RemotesWnd::HitTestField(POINT pt) const {
    for (size_t i = 0; i < m_fields.size(); ++i)
        if (PtInRect(&m_fields[i].rect, pt)) return static_cast<int>(i);
    return -1;
}

int RemotesWnd::HitTestRow(POINT pt) const {
    for (size_t i = 0; i < m_rows.size(); ++i)
        if (PtInRect(&m_rows[i].rect, pt)) return static_cast<int>(i);
    return -1;
}

int RemotesWnd::HitTestDot(POINT pt) const {
    for (size_t i = 0; i < m_rows.size(); ++i)
        if (PtInRect(&m_rows[i].dotRect, pt)) return static_cast<int>(i);
    return -1;
}

int RemotesWnd::HitTestIdentify(POINT pt) const {
    for (size_t i = 0; i < m_rows.size(); ++i)
        // Connected rows only — the button is greyed otherwise, and a hit test
        // that disagreed with the drawing would fire an invisible button.
        if (m_rows[i].dot == DotState::Up && PtInRect(&m_rows[i].idRect, pt))
            return static_cast<int>(i);
    return -1;
}

int RemotesWnd::HitTestLink(POINT pt) const {
    for (size_t i = 0; i < m_rows.size(); ++i)
        if (PtInRect(&m_rows[i].linkRect, pt)) return static_cast<int>(i);
    return -1;
}

int RemotesWnd::HitTestButton(POINT pt) const {
    for (size_t i = 0; i < m_buttons.size(); ++i)
        if (m_buttons[i].enabled && PtInRect(&m_buttons[i].rect, pt))
            return static_cast<int>(i);
    return -1;
}

// =============================================================================
// Keyboard
// =============================================================================
bool RemotesWnd::OnKeyDown(WPARAM vk, bool, bool, bool) {
    // Enter/Escape are handled BEFORE delegating: InputBox has no notion of
    // committing to a host, so the panel owns those two.
    if (m_editingField >= 0) {
        if (vk == VK_RETURN) { CommitTextEdit(); return true; }
        if (vk == VK_ESCAPE) { CancelTextEdit(); return true; }
        const InputResult r = m_edit.RouteKey(vk, GetHwnd());
        if (r != InputResult::Ignored) {
            if (r == InputResult::ConsumedRepaint) Repaint();
            return true;
        }
        return false;
    }

    switch (vk) {
        // Arrow keys load the row too, so keyboard and mouse browsing behave the
        // same — landing on a row means looking at it.
        case VK_UP:
            if (m_selectedRow > 0) { --m_selectedRow; FillFormFromRow(m_selectedRow); }
            return true;
        case VK_DOWN:
            if (m_selectedRow + 1 < static_cast<int>(m_rows.size())) {
                ++m_selectedRow;
                FillFormFromRow(m_selectedRow);
            }
            return true;
        case VK_F5:
            DoPollAll();
            return true;
        case VK_DELETE:
            DoRemoveTarget(m_selectedRow);
            return true;
        case VK_RETURN:
            // Enter on a row does the dot's job: bring it up, or take it down.
            if (m_selectedRow < static_cast<int>(m_rows.size())) {
                if (m_rows[m_selectedRow].dot == DotState::Up) DoStopTarget(m_selectedRow);
                else                                           DoStartTarget(m_selectedRow);
            }
            return true;
        default:
            break;
    }
    return false; // unhandled keys go to the app pipeline, per FloatingPanelWnd
}

bool RemotesWnd::OnLocalHide() {
    // Esc while editing abandons the edit rather than closing the panel — the
    // same rule the other panels' input fields follow.
    if (m_editingField >= 0) { CancelTextEdit(); return true; }
    // Then Esc again leaves edit MODE, before a third one closes the panel. Two
    // escapes to back all the way out of a form is the shape every dialog has.
    if (!m_formLocked) { DoCancelEdit(); return true; }
    return false;
}

// =============================================================================
// Message handling
// =============================================================================
LRESULT RemotesWnd::HandlePanelMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        // A target connected, dropped, or changed why it is down. The dot, the
        // Lag column and the Connect/Disconnect button all read from that state,
        // so the whole list is re-read — it is a handful of atomic loads plus
        // one exe stat per row, and it cannot go out of step with itself.
        case Constants::WM_QIV_REMOTE_TARGETS_CHANGED:
            // BEFORE the rebuild. Clearing afterwards would drop a change that
            // landed during it: that change would find the gate still closed,
            // skip its post, and the console would sit stale again — which is
            // the bug this whole path exists to fix.
            Remote::Mirror::ClearPanelNotifyPending(GetHwnd());
            Rebuild();
            Repaint();
            return 0;

        // Belt and braces alongside Hide(): a window can also go away without
        // being hidden first.
        case WM_DESTROY:
            Remote::Mirror::RemovePanelNotify(GetHwnd());
            break;

        case WM_TIMER:
            if (wParam == TIMER_PENDING) {
                KillTimer(GetHwnd(), TIMER_PENDING);
                // Whatever was launched or stopped has had its moment; ask the
                // sender threads what actually happened.
                Remote::Mirror::PingAll();
                Rebuild();
                Repaint();
            }
            return 0;

        case WM_SETCURSOR: {
            if (LOWORD(lParam) != HTCLIENT) break;
            POINT pt; GetCursorPos(&pt);
            ScreenToClient(GetHwnd(), &pt);
            // A locked field is not clickable, so it must not offer the hand —
            // the cursor is the first thing that says whether something can be
            // pressed, and a hand over a dead control is a lie.
            const bool hot = HitTestButton(pt) >= 0 || HitTestDot(pt) >= 0 ||
                             HitTestIdentify(pt) >= 0 ||
                             HitTestLink(pt) >= 0 || HitTestRow(pt) >= 0 ||
                             PtInRect(&m_statusLinkRect, pt) ||
                             (!m_formLocked && HitTestField(pt) >= 0);
            SetCursor(hot ? Constants::Cursors::CURR_CLICK
                          : Constants::Cursors::CURR_DEFAULT);
            return TRUE;
        }

        case WM_MOUSEMOVE: {
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const int r = HitTestRow(pt);
            const int b = HitTestButton(pt);
            const bool linkHot = PtInRect(&m_statusLinkRect, pt) != FALSE;
            if (r != m_hotRow || b != m_hotButton || linkHot != m_statusLinkHot) {
                m_hotRow = r; m_hotButton = b; m_statusLinkHot = linkHot;
                Repaint();
            }
            UpdateTip(pt);
            return 0;
        }

        case WM_LBUTTONDOWN: {
            SetFocus(GetHwnd());
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};

            // Before the buttons: the footer link sits below every control, so
            // nothing else can claim this point, and testing it first keeps the
            // ordering obvious rather than accidental.
            if (PtInRect(&m_statusLinkRect, pt)) {
                UI::Link::Reveal(m_statusPath);
                return 0;
            }

            const int b = HitTestButton(pt);
            if (b >= 0) {
                switch (m_buttons[b].id) {
                    case BTN_SAVE:           DoSaveEntry();               break;
                    case BTN_EDIT:           DoBeginEdit();               break;
                    case BTN_CANCEL:         DoCancelEdit();              break;
                    case BTN_NEW:            DoNewEntry();                break;
                    case BTN_IMPORT:         DoImportFromFile();          break;
                    case BTN_CONNECT_ALL:    DoConnectAll(true);          break;
                    case BTN_DISCONNECT_ALL: DoConnectAll(false);         break;
                    case BTN_POLL:           DoPollAll();                 break;
                    case BTN_SYNC_ALL:       DoSyncAll();                 break;
                    case BTN_REMOVE:         DoRemoveTarget(m_selectedRow); break;
                    default: break;
                }
                return 0;
            }

            const int f = HitTestField(pt);
            if (f >= 0) {
                if (m_formLocked) {
                    // Say why nothing happened. A field that simply ignores a
                    // click is indistinguishable from one that is broken.
                    m_status = L"The form is read-only — press Update to change this "
                               L"remote, or New to add one.";
                    Repaint();
                    return 0;
                }
                if (m_editingField >= 0 && m_editingField != f) CommitTextEdit();
                m_selectedField = f;
                EditField(f);
                return 0;
            }

            // The three action cells are checked before the row, so clicking any
            // of them performs its action rather than merely selecting the line
            // and loading it into the form.
            const int dot = HitTestDot(pt);
            if (dot >= 0) {
                m_selectedRow = dot;
                if (m_rows[dot].dot == DotState::Up) DoStopTarget(dot);
                else                                 DoStartTarget(dot);
                return 0;
            }

            const int ident = HitTestIdentify(pt);
            if (ident >= 0) {
                m_selectedRow = ident;
                DoIdentify(ident);
                return 0;
            }

            const int link = HitTestLink(pt);
            if (link >= 0) {
                m_selectedRow = link;
                DoToggleConnect(link);
                return 0;
            }


            // Clicking a row selects it AND loads it into the form above, so a
            // saved remote is corrected in place instead of being removed and
            // retyped. The dot and the eye were tested first, so this is a click
            // on the row proper.
            const int row = HitTestRow(pt);
            if (row >= 0) {
                m_selectedRow = row;
                FillFormFromRow(row);
            }
            return 0;
        }

        case WM_CHAR: {
            if (m_editingField >= 0) {
                m_edit.RouteChar(static_cast<wchar_t>(wParam), GetHwnd());
                Repaint();
            }
            return 0;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(GetHwnd(), &ps);
            RECT rc; GetClientRect(GetHwnd(), &rc);
            const int W = rc.right - rc.left, H = rc.bottom - rc.top;

            EnsureFonts(dc);
            EnsureBackBuffer(dc, W, H);
            HDC bb = m_bbDC;

            const COLORREF bg    = GetBgColor();
            const bool     dark  = BgIsDark(bg);
            const COLORREF fg    = dark ? RGB(235,235,235) : RGB(24,24,24);
            const COLORREF dim   = dark ? RGB(150,150,150) : RGB(110,110,110);
            const COLORREF selBg = dark ? RGB(58,86,132)   : RGB(203,222,250);
            const COLORREF hotBg = dark ? RGB(48,48,52)    : RGB(232,232,236);
            const COLORREF line  = dark ? RGB(64,64,64)    : RGB(220,220,220);

            FillRect(bb, &rc, Gdi::Brush(bg));
            SetBkMode(bb, TRANSPARENT);

            const float s   = app.dpiScale;
            const int pad   = static_cast<int>(PAD * s);
            const int rowH  = static_cast<int>(ROW_H * s);
            const int fldH  = static_cast<int>(FIELD_H * s);
            const int hdrH  = static_cast<int>(HDR_H * s);
            const int btnH  = static_cast<int>(BTN_H * s);
            const int labelW = static_cast<int>(130 * s);

            // ── Title ────────────────────────────────────────────────────────
            SelectObject(bb, m_hFontBold);
            SetTextColor(bb, fg);
            RECT tr{pad, static_cast<int>(6 * s), W - pad, static_cast<int>(26 * s)};
            // COUNTS IN THE TITLE. "Servers" alone does not say whether the list
            // is empty because nothing is saved or because nothing answered, and
            // those need opposite actions from the user.
            int upCount = 0;
            for (const RowView &rv : m_rows)
                if (rv.dot == DotState::Up) ++upCount;

            // Named for what it is rather than `title`: a later local by that
            // name lives in the row loop below, and /W4 is right that one
            // shadowing the other is a trap waiting for whoever edits next.
            const std::wstring serversTitle =
                L"\U0001F4E1 Servers — the instances this copy can connect to   \x00B7   " +
                std::to_wstring(m_rows.size()) + L" saved, " +
                std::to_wstring(upCount) + L" connected";

            DrawTextW(bb, serversTitle.c_str(), -1, &tr,
                      DT_LEFT | DT_SINGLELINE);

            SelectObject(bb, m_hFontSmall);
            SetTextColor(bb, dim);
            RECT sr{pad, tr.bottom, W - pad, tr.bottom + static_cast<int>(16 * s)};
            {
                // The mirror selection has its own panel (Ctrl+F11), but this
                // console is where the targets themselves are managed — so it
                // names the selection while it is narrowed, and stays silent
                // while every connected row is following along, which is the
                // ordinary case and needs no explaining.
                std::wstring mirror = app.passCommandToRemote ? L"ON" : L"off";
                if (app.passCommandToRemote && Remote::Mirror::HasLiveTargets())
                    mirror += L" → " + Remote::Mirror::SelectionSummary();

                const std::wstring sub =
                    std::wstring(L"F11 mirror ") + mirror +
                    L"   ·   F12 execute here " + (app.resendCommandToCaller ? L"ON" : L"off") +
                    L"   ·   Ctrl+F11 picks which & watches · ● starts/stops the "
                    L"program · Identify names the screen · F5 polls";
                DrawTextW(bb, sub.c_str(), -1, &sr, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            }

            int y = static_cast<int>(TITLE_H * s);

            // ── New connection ───────────────────────────────────────────────
            SelectObject(bb, m_hFontBold);
            SetTextColor(bb, PC::HEADER);
            {
                // The heading says which mode the form is in, because the fields
                // themselves look nearly the same either way and "why can I not
                // type here" is the question this whole latch invites.
                const wchar_t *title = m_formLocked
                                           ? L"Connection details  (read-only)"
                                       : m_editingRowId != 0
                                           ? L"Editing connection"
                                           : L"New connection";
                RECT hr{pad + static_cast<int>(6 * s), y, W - pad, y + hdrH};
                DrawTextW(bb, title, -1, &hr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                RECT st{pad, y + hdrH / 4, pad + static_cast<int>(3 * s), y + hdrH * 3 / 4};
                // The stripe goes the accent colour while the form is live, so
                // "these fields will accept typing" reads from across the panel.
                FillRect(bb, &st, Gdi::Brush(m_formLocked ? PC::STRIPE : PC::ON));
            }
            y += hdrH;

            for (size_t i = 0; i < m_fields.size(); ++i) {
                Field &f = m_fields[i];
                f.rect = {pad, y, W - pad, y + fldH};

                if (static_cast<int>(i) == m_selectedField) {
                    FillRect(bb, &f.rect, Gdi::Brush(selBg));
                }

                SelectObject(bb, m_hFontBody);
                SetTextColor(bb, fg);
                RECT lr{pad + static_cast<int>(6 * s), y, pad + labelW,
                        y + static_cast<int>(22 * s)};
                DrawTextW(bb, f.label.c_str(), -1, &lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

                RECT vr{pad + labelW, y, W - pad - static_cast<int>(6 * s),
                        y + static_cast<int>(22 * s)};

                if (m_editingField == static_cast<int>(i)) {
                    m_edit.Draw(bb, m_hFontBody, vr, static_cast<int>(4 * s),
                                GetFocus() == GetHwnd());
                } else {
                    COLORREF vc = PC::TEXT;
                    if (f.id == F_PORT) vc = PC::NUMBER;
                    else if (f.id == F_HOST || f.id == F_EXE) vc = PC::PATH;
                    if (f.value == L"(not set)" || f.value == L"(none)" ||
                        f.value == L"(from its banner)") vc = PC::WARN;
                    // Dimmed while locked — the same language every disabled
                    // control in this panel already speaks.
                    if (m_formLocked) vc = dim;
                    SetTextColor(bb, vc);
                    DrawTextW(bb, f.value.c_str(), -1, &vr,
                              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS);
                }

                SelectObject(bb, m_hFontSmall);
                SetTextColor(bb, dim);
                RECT dr{pad + static_cast<int>(6 * s), y + static_cast<int>(21 * s),
                        W - pad, y + fldH};
                DrawTextW(bb, f.desc, -1, &dr, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

                y += fldH;
            }

            // ── Buttons ──────────────────────────────────────────────────────
            y += static_cast<int>(6 * s);
            {
                const int gap   = static_cast<int>(BTN_GAP * s);
                const int count = static_cast<int>(m_buttons.size());
                const int total = W - pad * 2 - gap * (count - 1);
                const int bw    = total / count;
                int x = pad;

                SelectObject(bb, m_hFontBody);
                for (Button &btn : m_buttons) {
                    btn.rect = {x, y, x + bw, y + btnH};
                    const int myIndex = static_cast<int>(&btn - m_buttons.data());
                    // Save is the form's affirmative action and gets the accent;
                    // everything else acts on the list and stays uniform.
                    COLORREF base = (btn.id == BTN_SAVE) ? PC::BTN_ALT : PC::BTN_MAIN;
                    if (!btn.enabled) base = bg;
                    else if (myIndex == m_hotButton)
                        base = RGB(std::min(255, GetRValue(base) + 40),
                                   std::min(255, GetGValue(base) + 40),
                                   std::min(255, GetBValue(base) + 40));

                    FillRect(bb, &btn.rect, Gdi::Brush(base));

                    HGDIOBJ op = SelectObject(bb, Gdi::Pen(line));
                    HGDIOBJ ob = SelectObject(bb, GetStockObject(NULL_BRUSH));
                    Rectangle(bb, btn.rect.left, btn.rect.top, btn.rect.right, btn.rect.bottom);
                    SelectObject(bb, ob); SelectObject(bb, op);

                    SetTextColor(bb, btn.enabled ? RGB(245,245,245) : dim);
                    RECT lr = btn.rect;
                    DrawTextW(bb, btn.label.c_str(), -1, &lr,
                              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    x += bw + gap;
                }
            }
            y += btnH + static_cast<int>(12 * s);

            // ── Remotes ──────────────────────────────────────────────────────
            const int cNum  = pad + static_cast<int>(6   * s);
            const int cName = pad + static_cast<int>(46  * s);
            const int cHost = pad + static_cast<int>(250 * s);
            const int cPort = pad + static_cast<int>(420 * s);
            const int cLag  = pad + static_cast<int>(490 * s);
            const int cDot  = pad + static_cast<int>(645 * s);
            // No Watch column here any more — the eye moved to Ctrl+F11, where
            // the rest of the "who drives whom" questions live. This console is
            // about which instances EXIST and how to reach them.
            //
            // Two real buttons, so both need a span rather than an anchor.
            // Identify sits BEFORE Link: it is the question you ask first —
            // which screen is this row? — and Link is the one that changes
            // something, which belongs at the end of the row.
            // Identify is sized to the WORD; Link stays wider because it has to
            // hold "Disconnect" and "Connecting…" without the label changing the
            // button's width as you press it.
            const int cIdL  = pad + static_cast<int>(700 * s);
            const int cIdR  = pad + static_cast<int>(776 * s);
            const int cLinkL = pad + static_cast<int>(798 * s);
            const int cLinkR = pad + static_cast<int>(944 * s);

            SelectObject(bb, m_hFontSmall);
            SetTextColor(bb, PC::HEADER);
            {
                auto hdr = [&](int x, const wchar_t *t) {
                    RECT r{x, y, W - pad, y + hdrH};
                    DrawTextW(bb, t, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                };
                // The last three columns draw CENTRED marks (two circles and a
                // button), so their headers must be centred on the same axis —
                // a left-aligned header over a centred glyph reads as belonging
                // to the column beside it.
                auto hdrC = [&](int cx, const wchar_t *t) {
                    const int half = static_cast<int>(50 * s);
                    RECT r{cx - half, y, cx + half, y + hdrH};
                    DrawTextW(bb, t, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                };
                hdr(cNum, L"#"); hdr(cName, L"Name"); hdr(cHost, L"Address");
                hdr(cPort, L"Port"); hdr(cLag, L"Lag");
                hdrC(cDot, L"Up");
                hdrC((cIdL + cIdR) / 2, L"Identify");
                hdrC((cLinkL + cLinkR) / 2, L"Link");
            }
            y += hdrH;

            {
                HGDIOBJ ohp = SelectObject(bb, Gdi::Pen(line));
                MoveToEx(bb, pad, y, nullptr);
                LineTo(bb, W - pad, y);
                SelectObject(bb, ohp);
            }

            if (m_rows.empty()) {
                SelectObject(bb, m_hFontBody);
                SetTextColor(bb, dim);
                RECT er{pad, y + static_cast<int>(16 * s), W - pad,
                        y + static_cast<int>(70 * s)};
                DrawTextW(bb,
                          L"No remotes yet. Fill in the address, port and name above and "
                          L"press Save — or Add from file… to read them out of that "
                          L"instance's own settings. Connect is a separate press.",
                          -1, &er, DT_LEFT | DT_WORDBREAK);
            }

            for (size_t i = 0; i < m_rows.size(); ++i) {
                RowView &r = m_rows[i];
                r.rect = {pad, y, W - pad, y + rowH};

                if (static_cast<int>(i) == m_selectedRow || static_cast<int>(i) == m_hotRow)
                    FillRect(bb, &r.rect,
                             Gdi::Brush(static_cast<int>(i) == m_selectedRow ? selBg : hotBg));

                SelectObject(bb, m_hFontBody);
                auto cell = [&](int x, const std::wstring &t, COLORREF c) {
                    SetTextColor(bb, c);
                    RECT cr{x, y, W - pad, y + rowH};
                    DrawTextW(bb, t.c_str(), -1, &cr,
                              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                };

                cell(cNum,  std::to_wstring(i + 1), dim);
                cell(cName, r.name,                 PC::TEXT);
                // A remote row is marked, because it behaves differently: it
                // gets the portable command set, its start button cannot work,
                // and it will show different pictures than this screen.
                cell(cHost, r.sameMachine ? r.host : (r.host + L"  (remote)"),
                     r.sameMachine ? PC::PATH : PC::CHOICE);
                cell(cPort, std::to_wstring(r.port), PC::NUMBER);

                // Connected: the round trip. Not connected: WHY not, in a word.
                // A dash tells you nothing you could not already see from the
                // dot, and this is the column with room for it.
                if (r.dot == DotState::Up) {
                    cell(cLag, FormatLag(r.lagUs), PC::NUMBER);
                } else if (r.dot == DotState::Pending) {
                    cell(cLag, L"…", PC::WARN);
                } else if (r.dot == DotState::Idle) {
                    cell(cLag, L"not connected", dim);
                } else {
                    cell(cLag, Remote::Mirror::DownLabel(r.down),
                         r.down == Remote::Mirror::Down::Offline ? dim : PC::WARN);
                }

                // ● — the state and the button in one. Amber means an action is
                // in flight, so a click is acknowledged before the re-poll
                // confirms it.
                const int dotR  = static_cast<int>(6 * s);
                const int dotCY = y + rowH / 2;
                r.dotRect = {cDot - dotR * 2, dotCY - dotR * 2,
                             cDot + dotR * 2, dotCY + dotR * 2};
                {
                    const COLORREF dotColour = (r.dot == DotState::Up)      ? PC::ON
                                             : (r.dot == DotState::Pending) ? PC::WARN
                                             : (r.dot == DotState::Idle)    ? dim
                                                                            : PC::OFF;
                    HGDIOBJ ob2 = SelectObject(bb, Gdi::Brush(dotColour));
                    HGDIOBJ op2 = SelectObject(bb, Gdi::Pen(line));
                    Ellipse(bb, cDot - dotR, dotCY - dotR, cDot + dotR, dotCY + dotR);
                    SelectObject(bb, ob2); SelectObject(bb, op2);
                }

                // Identify — make THIS screen say who it is. Sends the row's own
                // name as a centre-screen message to that instance, which is the
                // only way to tell two identical viewers apart without walking
                // over to them.
                //
                // Only meaningful down a live connection, so it is greyed when
                // the row is not connected — a press that queues a message for a
                // screen that is not listening looks like a broken button.
                {
                    const int vin = static_cast<int>(4 * s);
                    r.idRect = {cIdL, y + vin, cIdR, y + rowH - vin};

                    const bool live = (r.dot == DotState::Up);
                    COLORREF base = live ? PC::BTN_MAIN : bg;
                    if (live && static_cast<int>(i) == m_hotRow)
                        base = RGB(std::min(255, GetRValue(base) + 40),
                                   std::min(255, GetGValue(base) + 40),
                                   std::min(255, GetBValue(base) + 40));
                    FillRect(bb, &r.idRect, Gdi::Brush(base));

                    HGDIOBJ op4 = SelectObject(bb, Gdi::Pen(line));
                    HGDIOBJ ob4 = SelectObject(bb, GetStockObject(NULL_BRUSH));
                    Rectangle(bb, r.idRect.left, r.idRect.top,
                              r.idRect.right, r.idRect.bottom);
                    SelectObject(bb, ob4); SelectObject(bb, op4);

                    SelectObject(bb, m_hFontSmall);
                    SetTextColor(bb, live ? RGB(245, 245, 245) : dim);
                    RECT ir = r.idRect;
                    DrawTextW(bb, L"Identify", -1, &ir,
                              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    SelectObject(bb, m_hFontBody);
                }

                // Link — connect / disconnect THIS row. Last column, and drawn
                // as a real button (fill + border) so it reads as the one thing
                // in the row you press, rather than as another status word.
                // The label states what the press DOES, not what the row is.
                {
                    const int vin = static_cast<int>(4 * s);
                    r.linkRect = {cLinkL, y + vin, cLinkR, y + rowH - vin};

                    // Label follows the LINK, not the wish. Asking to connect to
                    // a machine that is off leaves wantConnect true while
                    // nothing is connected — saying "Disconnect" there claims a
                    // session that does not exist. Only a live round trip earns
                    // that word; a dial in progress says so; everything else
                    // offers the retry.
                    const wchar_t *lbl = (r.dot == DotState::Up)      ? L"Disconnect"
                                       : (r.dot == DotState::Pending) ? L"Connecting…"
                                                                      : L"Connect";
                    COLORREF base = (r.dot == DotState::Up) ? PC::BTN_ALT : PC::BTN_MAIN;
                    if (static_cast<int>(i) == m_hotRow)
                        base = RGB(std::min(255, GetRValue(base) + 40),
                                   std::min(255, GetGValue(base) + 40),
                                   std::min(255, GetBValue(base) + 40));
                    FillRect(bb, &r.linkRect, Gdi::Brush(base));

                    HGDIOBJ op3 = SelectObject(bb, Gdi::Pen(line));
                    HGDIOBJ ob3 = SelectObject(bb, GetStockObject(NULL_BRUSH));
                    Rectangle(bb, r.linkRect.left, r.linkRect.top,
                              r.linkRect.right, r.linkRect.bottom);
                    SelectObject(bb, ob3); SelectObject(bb, op3);

                    SelectObject(bb, m_hFontSmall);
                    SetTextColor(bb, RGB(245, 245, 245));
                    RECT lkr = r.linkRect;
                    DrawTextW(bb, lbl, -1, &lkr,
                              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    SelectObject(bb, m_hFontBody);
                }

                y += rowH;
            }

            // ── Footer ───────────────────────────────────────────────────────
            {
                const int fy = H - static_cast<int>(FOOTER_H * s);
                HGDIOBJ op = SelectObject(bb, Gdi::Pen(line));
                MoveToEx(bb, pad, fy - static_cast<int>(4 * s), nullptr);
                LineTo(bb, W - pad, fy - static_cast<int>(4 * s));
                SelectObject(bb, op);

                // A row that is down knows WHY, and what to do about it. The
                // list column has room for one word; this has room for the
                // sentence that makes the word actionable.
                std::wstring foot = m_status;
                if (foot.empty() && m_selectedRow < static_cast<int>(m_rows.size())) {
                    const RowView &sel = m_rows[m_selectedRow];
                    if (sel.exeMissing) {
                        foot = L"Exe not found — " + sel.exePath +
                               L"   ·   moved or deleted, so the dot cannot start it";
                    } else if (sel.dot == DotState::Down) {
                        foot = Remote::Mirror::DownRemedy(sel.down);
                        if (foot.empty()) foot = sel.lastError;
                    }
                }

                SelectObject(bb, m_hFontSmall);
                SetTextColor(bb, dim);
                RECT frc{pad, fy, W - pad, fy + static_cast<int>(20 * s)};
                DrawTextW(bb, foot.c_str(), -1, &frc,
                          DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

                // The source file, as a link, measured off the end of the label
                // so the hit box cannot drift from the glyphs. Only when the
                // footer is still showing the label — a row-derived message
                // (a target that is down) replaced it and owns the line.
                m_statusLinkRect = RECT{};
                if (!m_statusPath.empty() && foot == m_status) {
                    const int lx = pad + UI::Link::MeasureIn(bb, m_hFontSmall, m_status);
                    m_statusLinkRect = UI::Link::Draw(bb, m_hFontLink, lx, fy, W - pad,
                                                      m_statusPath, m_statusLinkHot, s);
                }
            }

            BitBlt(dc, 0, 0, W, H, bb, 0, 0, SRCCOPY);
            EndPaint(GetHwnd(), &ps);
            return 0;
        }

        case WM_ERASEBKGND:
            return 1; // fully repainted from the back buffer

        default:
            break;
    }
    return DefWindowProcW(GetHwnd(), message, wParam, lParam);
}

} // namespace UI
