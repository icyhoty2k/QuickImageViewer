// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#include "RemoteClientsWnd.h"
#include "RemoteSettings.h"   // BlockScope — what a ban or a timed kick writes
#include "RemoteProtocol.h"   // FormatEndpoint — brackets IPv6 literals

#include "AppState.h"
#include "Dedicated/DedicatedSettings.h" // PanelColors / PANEL_OPACITY only
#include "Platform/Constants.h"
#include "Platform/ConstantsStrings.h"
#include "UI/ThemedDialog.h"
// Safe from a .cpp despite UIManager.h including this panel's header — the guard
// has already fired, so there is no cycle. Same as RemoteWnd.cpp, whose button
// points back here.
#include "UI/UIManager.h"  // the Local Server button opens F9
#include "UI/GdiPool.h" // brushes and pens are pooled — never DeleteObject them

#include <algorithm>
#include <windowsx.h>

extern AppState app;

namespace UI {

namespace RT = Constants::RemoteTcpIp;
namespace PC = Constants::Dedicated::PanelColors;

namespace {
    constexpr int PANEL_W = 820;
    constexpr int PANEL_H = 560;
    constexpr int PAD     = 14;
    constexpr int ROW_H   = 42;
    constexpr int HDR_H   = 30;
    constexpr int BTN_H   = 34;
    constexpr int BTN_GAP = 8;
    constexpr int TITLE_H = 44;
    // Two lines: the listener's status, then the last action's outcome.
    constexpr int FOOTER_H = 46;

    // How often the list re-reads the server while the panel is on screen. Fast
    // enough that a client arriving feels immediate, slow enough that it is
    // nothing — two vector copies and a repaint of a window nobody is typing
    // into. Stopped on hide, so a closed panel costs zero.
    constexpr UINT TIMER_REFRESH    = 1;
    constexpr UINT REFRESH_INTERVAL = 1000;

    // BTN_SERVER opens F9 — the counterpart of the My Clients button there.
    // The two panels are one subject split in half, and the crossing is constant:
    // ban an address here, widen the AllowList there.
    enum ButtonId { BTN_KICK = 1, BTN_TIMED, BTN_BAN, BTN_LIFT, BTN_SERVER };

    bool BgIsDark(COLORREF bg) {
        const int lum = (GetRValue(bg) * 299 + GetGValue(bg) * 587 + GetBValue(bg) * 114) / 1000;
        return lum < 128;
    }

    // "3s", "4m", "2h" — one unit. This column answers "is that the client I
    // just started" and never needs to be finer than that.
    std::wstring Elapsed(long long ms) {
        if (ms < 0) ms = 0;
        const long long sec = ms / 1000;
        if (sec < 60)   return std::to_wstring(sec) + L"s";
        if (sec < 3600) return std::to_wstring(sec / 60) + L"m";
        return std::to_wstring(sec / 3600) + L"h";
    }

    long long NowTicks() { return static_cast<long long>(GetTickCount64()); }

    // NO DEVICE GLYPH. The platform is known — `agent` carries it and the
    // description line names it in words — but no icon was settled on, and a
    // placeholder pictogram is worse than none: it would be the one mark in this
    // list derived from something the PEER claims rather than something this
    // machine observed. The scope glyph below is the opposite, which is why it
    // earns its place.

    // The scope glyph lives in RemoteProtocol now — Servers (F10) shows one too,
    // and the two must never disagree about what counts as "the LAN".
}

// =============================================================================
// Init / Show
// =============================================================================
void RemoteClientsWnd::Init(HINSTANCE hInstance, HWND hParent) {
    const float s = app.dpiScale;
    InitFloating(hInstance, hParent, L"qIVRemoteClientsWnd", L"My Clients",
                 static_cast<int>(PANEL_W * s), static_cast<int>(PANEL_H * s));
    if (GetHwnd()) {
        SetWindowLongPtrW(GetHwnd(), GWL_EXSTYLE,
                          GetWindowLongPtrW(GetHwnd(), GWL_EXSTYLE) | WS_EX_LAYERED);
        SetLayeredWindowAttributes(GetHwnd(), 0,
                                   Constants::Dedicated::PANEL_OPACITY, LWA_ALPHA);
    }
    BuildRows();
}

void RemoteClientsWnd::Init(HINSTANCE hInstance, HWND hParent, int8_t) { Init(hInstance, hParent); }

void RemoteClientsWnd::Show() {
    m_lastResult.clear();
    BuildRows();
    ShowCenterOverParent();

    // The timer starts with the window and stops with it — see WM_SHOWWINDOW.
    // A panel nobody can see must not be waking the UI thread once a second.
    if (GetHwnd()) SetTimer(GetHwnd(), TIMER_REFRESH, REFRESH_INTERVAL, nullptr);
    Repaint();
}

// =============================================================================
// Model
// =============================================================================
void RemoteClientsWnd::BuildRows() {
    m_rows.clear();

    m_conns  = Remote::IsRunning() ? Remote::Connections()
                                   : std::vector<Remote::ClientInfo>{};
    m_blocks = Remote::Blacklist::TimedSnapshot();

    auto header = [&](const wchar_t *text) {
        Row r; r.kind = Kind::Header; r.label = text; m_rows.push_back(std::move(r));
    };
    auto note = [&](std::wstring text) {
        Row r; r.kind = Kind::Note; r.label = std::move(text); m_rows.push_back(std::move(r));
    };

    // ── Connected clients ───────────────────────────────────────────────────
    header(L"Connected");

    if (!Remote::IsRunning()) {
        note(L"The listener is stopped — start it in Local Server (F9).");
    } else if (m_conns.empty()) {
        note(L"Nothing connected.");
    } else {
        const long long now = NowTicks();
        for (size_t i = 0; i < m_conns.size(); ++i) {
            const Remote::ClientInfo &ci = m_conns[i];

            // ADDRESS AS THE LABEL, name as part of the value. The name is the
            // only string on the wire a peer chooses about itself, so it never
            // stands where the identity goes — a peer calling itself
            // "127.0.0.1" must not be able to make this list lie about where it
            // came from. See g_peerNames in RemoteServer.cpp.
            std::wstring value;
            if (!ci.name.empty()) value = ci.name + L" — ";
            value += ci.tls ? L"TLS" : L"plain";
            if (ci.sameMachine) value += L", this machine";
            value += L", connected " + Elapsed(now - ci.sinceMs);

            // WATCHING is the difference between a screen and a script.
            //
            // Everything else in this row describes a socket. This describes what
            // the peer is FOR: a client that sent `Observe 1` is being pushed
            // every action and is showing the pictures, which is a phone on a
            // wall. One that has not is polling, scripting, or idle.
            //
            // It matters when deciding what to kick. Two rows from the same
            // address tell you nothing; "Kitchen tablet — watching" and a bare
            // connection tell you which one is the display somebody is looking at.
            if (ci.observing) value += L", watching";

            Row r;
            r.kind  = Kind::Client;
            // Glyph in front of the ADDRESS, because both describe where the
            // connection came from and both are this machine's own observation.
            // The peer-chosen name stays over in the value, where it cannot be
            // mistaken for identity.
            r.label = std::wstring(Remote::ScopeIcon(ci.address, ci.sameMachine)) + L"  " +
                      Remote::FormatEndpoint(ci.address, ci.port);
            r.value = std::move(value);
            // What the peer said about itself, on the description line rather
            // than in the row: it is useful when you are deciding what something
            // is, and noise when you are scanning the list.
            //
            // "?" for anything it never told us. A blank reads as "nothing";
            // a question mark reads as "it did not say", which is the truth.
            Remote::AgentInfo ag;
            ag.app      = ci.agentApp;
            ag.version  = ci.agentVersion;
            ag.os       = ci.agentOs;
            ag.host     = ci.agentHost;
            ag.platform = ci.platform;

            // CLIENT, always, in this panel: every row here reached us through
            // the listener. Mirroring passes Server for the same reason —
            // this instance dialled those.
            r.desc  = Remote::DescribeAgent(ag, Remote::AgentRole::Client) +
                      L"\r\nKick closes it. Kick for N also refuses this peer for a "
                      L"while. Ban blacklists it permanently.";
            r.item  = static_cast<int>(i);
            m_rows.push_back(std::move(r));
        }
    }

    // ── Timed blocks ────────────────────────────────────────────────────────
    //
    // Shown even when empty is NOT done: a section that is blank in every
    // ordinary session is a section that trains you to skip it, and this one
    // matters exactly when it has something in it.
    if (!m_blocks.empty()) {
        header(L"Timed blocks");
        const long long now = NowTicks();
        for (size_t i = 0; i < m_blocks.size(); ++i) {
            const Remote::Blacklist::TimedEntry &t = m_blocks[i];

            Row r;
            r.kind  = Kind::Block;
            r.label = t.address;
            r.value = L"expires in " + Elapsed(t.untilMs - now);
            r.desc  = L"Refused until then. Lift clears it early; a restart "
                      L"clears every timed block.";
            r.item  = static_cast<int>(i);
            m_rows.push_back(std::move(r));
        }
    }

    // The selection can outlive the rows it pointed at — a client disconnects
    // between two refreshes and the list gets shorter under the cursor. Clamped
    // onto something selectable rather than left dangling, because every action
    // below reads it.
    if (m_selected >= static_cast<int>(m_rows.size()))
        m_selected = static_cast<int>(m_rows.size()) - 1;
    if (m_selected < 0) m_selected = 0;
    while (m_selected < static_cast<int>(m_rows.size()) &&
           (m_rows[m_selected].kind == Kind::Header || m_rows[m_selected].kind == Kind::Note))
        ++m_selected;

    // The rows moved, so wherever the selection now sits has to be brought back
    // into view — by the next paint, once it has laid them out.
    m_scrollToSelection = true;

    m_buttons.clear();
    m_buttons.push_back({L"Kick",         BTN_KICK,   {}, false});
    m_buttons.push_back({L"Kick for…",    BTN_TIMED,  {}, false});
    m_buttons.push_back({L"Ban",          BTN_BAN,    {}, false});
    m_buttons.push_back({L"Lift block",   BTN_LIFT,   {}, false});
    // Always enabled — unlike the four above it does not act on a selection.
    m_buttons.push_back({L"Local Server", BTN_SERVER, {}, true});
    SyncButtons();
}

// One wheel "line" is one row. The base applies the user's Mouse setting and
// the Shift accelerator on top.
int RemoteClientsWnd::ScrollLinePx(const ScrollView &) const {
    return std::max(1, m_lastRowH);
}

void RemoteClientsWnd::SyncButtons() {
    const bool onClient = SelectedClient() >= 0;
    const bool onBlock  = SelectedBlock()  >= 0;
    for (Button &b : m_buttons) {
        switch (b.id) {
            case BTN_KICK:
            case BTN_TIMED:
            case BTN_BAN:  b.enabled = onClient; break;
            case BTN_LIFT: b.enabled = onBlock;  break;
            default: break;
        }
    }
}

void RemoteClientsWnd::LayoutRows(int rowH, int headerH) {
    int y = 0;
    for (Row &r : m_rows) {
        r.ch = (r.kind == Kind::Header || r.kind == Kind::Note) ? headerH : rowH;
        r.cy = y;
        y += r.ch;
    }
    m_list.contentH = y;
    m_lastRowH = rowH;
}

void RemoteClientsWnd::EnsureSelectedVisible() {
    if (m_selected < 0 || m_selected >= static_cast<int>(m_rows.size())) return;
    // Before the first paint there is no layout and no view to scroll within.
    // Doing nothing is right: the first paint lays out and clamps anyway.
    if (m_list.Height() <= 0) return;
    const Row &r = m_rows[static_cast<size_t>(m_selected)];
    m_list.EnsureVisibleY(r.cy, r.ch);
}

int RemoteClientsWnd::SelectedClient() const {
    if (m_selected < 0 || m_selected >= static_cast<int>(m_rows.size())) return -1;
    const Row &r = m_rows[m_selected];
    if (r.kind != Kind::Client) return -1;
    // Bounds-checked against the snapshot rather than trusted from the row. The
    // two are rebuilt together, but a stale row surviving a rebuild would index
    // out of a shorter vector, and that is a crash rather than a wrong answer.
    return (r.item >= 0 && r.item < static_cast<int>(m_conns.size())) ? r.item : -1;
}

int RemoteClientsWnd::SelectedBlock() const {
    if (m_selected < 0 || m_selected >= static_cast<int>(m_rows.size())) return -1;
    const Row &r = m_rows[m_selected];
    if (r.kind != Kind::Block) return -1;
    return (r.item >= 0 && r.item < static_cast<int>(m_blocks.size())) ? r.item : -1;
}

std::wstring RemoteClientsWnd::StatusLine() const {
    if (!Remote::IsRunning()) return L"Listener stopped";

    std::wstring s = L"Listening on " + Remote::BoundEndpoint() +
                     L"   clients: " + std::to_wstring(Remote::ActiveConnections()) +
                     L"/" + std::to_wstring(Remote::Config().maxConnections);
    if (!m_blocks.empty())
        s += L"   timed blocks: " + std::to_wstring(m_blocks.size());
    return s;
}

// =============================================================================
// Actions
// =============================================================================
void RemoteClientsWnd::DoKick() {
    const int idx = SelectedClient();
    if (idx < 0) return;
    const Remote::ClientInfo &ci = m_conns[static_cast<size_t>(idx)];
    const std::wstring where = Remote::FormatEndpoint(ci.address, ci.port);

    if (!DialogConfirm(L"Close the connection from " + where + L"?\r\n\r\n"
                       L"This does not block the address — the same client may "
                       L"reconnect immediately. Use \"Kick for…\" for that."))
        return;

    // False means it ended on its own between the last refresh and the button
    // press. Said out loud rather than ignored: the row vanishing a second later
    // would otherwise look like the kick worked.
    m_lastResult = Remote::KickConnection(ci.id)
                       ? L"Kicked " + where
                       : L"That connection had already closed.";
    BuildRows();
    Repaint();
}

void RemoteClientsWnd::DoTimedKick() {
    const int idx = SelectedClient();
    if (idx < 0) return;
    const Remote::ClientInfo &ci = m_conns[static_cast<size_t>(idx)];
    const std::wstring where = Remote::FormatEndpoint(ci.address, ci.port);

    const int minutes = DialogPromptInt(L"Refuse this peer for how many minutes?",
                                        RT::TIMED_BLOCK_DEFAULT_MIN,
                                        RT::TIMED_BLOCK_MIN_MIN,
                                        RT::TIMED_BLOCK_MAX_MIN,
                                        RT::TIMED_BLOCK_DEFAULT_MIN);
    if (minutes <= 0) return;   // cancelled

    std::wstring written;
    if (!Remote::TimedKickConnection(ci.id, minutes, written)) {
        m_lastResult = L"That connection had already closed — nothing was blocked.";
    } else {
        m_lastResult = L"Kicked " + where + L" — " + written + L" refused for " +
                       std::to_wstring(minutes) + L" min";
        // Named when it is WIDER than the row, because a /64 is not what the
        // list showed and blocking one silently would be the panel doing
        // something broader than it appeared to offer.
        if (written != ci.address)
            m_lastResult += L" (the whole /64)";
    }
    BuildRows();
    Repaint();
}

void RemoteClientsWnd::DoBan() {
    const int idx = SelectedClient();
    if (idx < 0) return;
    const Remote::ClientInfo &ci = m_conns[static_cast<size_t>(idx)];

    // WHAT WILL ACTUALLY BE WRITTEN, computed before the question is asked. For
    // IPv6 that is a /64 rather than the address on the row.
    const std::wstring scope = Remote::BlockScope(ci.address);
    const bool         wider = (scope != ci.address);

    std::wstring text = L"Block " + scope + L" permanently and close this "
                        L"connection?\r\n\r\n";
    if (wider)
        text += L"That is the whole /64 this client's address belongs to, not "
                L"just " + ci.address + L". A single IPv6 address is not worth "
                L"blocking — the peer has billions of others in that same "
                L"prefix.\r\n\r\n";
    text += L"Written to qivRemoteServerBlacklist.ini and survives a restart. "
            L"Undo it by editing or deleting that file. For something temporary, "
            L"use \"Kick for…\" instead.";

    if (!DialogConfirm(text)) return;

    std::wstring written;
    m_lastResult = Remote::BanConnection(ci.id, written)
                       ? L"Blocked " + written + L" permanently"
                       : L"That connection had already closed — nothing was blocked.";
    BuildRows();
    Repaint();
}

void RemoteClientsWnd::DoLiftBlock() {
    const int idx = SelectedBlock();
    if (idx < 0) return;
    const std::wstring addr = m_blocks[static_cast<size_t>(idx)].address;

    m_lastResult = Remote::Blacklist::ClearTimed(addr)
                       ? L"Lifted the block on " + addr
                       : L"That block had already expired.";
    BuildRows();
    Repaint();
}

// =============================================================================
// Dialog helpers — the panel is topmost, dialogs are not
// =============================================================================
void RemoteClientsWnd::PushTopmostOff() {
    if (GetHwnd())
        SetWindowPos(GetHwnd(), HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void RemoteClientsWnd::PopTopmost() {
    if (GetHwnd())
        SetWindowPos(GetHwnd(), HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void RemoteClientsWnd::DialogMessage(const std::wstring &text) {
    PushTopmostOff();
    ThemedDialog::Message(GetHwnd(), text.c_str(), L"My Clients");
    PopTopmost();
}

bool RemoteClientsWnd::DialogConfirm(const std::wstring &text) {
    PushTopmostOff();
    const bool r = ThemedDialog::Confirm(GetHwnd(), text.c_str(), L"My Clients");
    PopTopmost();
    return r;
}

int RemoteClientsWnd::DialogPromptInt(const wchar_t *label, int cur, int lo, int hi, int def) {
    PushTopmostOff();
    const int r = ThemedDialog::PromptInt(GetHwnd(), L"My Clients", label, cur, lo, hi, def);
    PopTopmost();
    return r;
}

// =============================================================================
// Paint plumbing
// =============================================================================
void RemoteClientsWnd::EnsureFonts(HDC dc) {
    const int dpi = GetDeviceCaps(dc, LOGPIXELSY);
    if (m_hFontBody && dpi == m_cachedFontDpi) return;
    if (m_hFontBody)  DeleteObject(m_hFontBody);
    if (m_hFontBold)  DeleteObject(m_hFontBold);
    if (m_hFontSmall) DeleteObject(m_hFontSmall);
    m_cachedFontDpi = dpi;
    auto mk = [&](int pt, int w) {
        return CreateFontW(-MulDiv(pt, dpi, 72), 0, 0, 0, w, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
    };
    m_hFontBody  = mk(10, FW_NORMAL);
    m_hFontBold  = mk(11, FW_SEMIBOLD);
    m_hFontSmall = mk(8,  FW_NORMAL);
}

void RemoteClientsWnd::EnsureBackBuffer(HDC refDC, int w, int h) {
    if (m_bbDC && m_bbW == w && m_bbH == h) return;
    DestroyBackBuffer();
    m_bbDC = CreateCompatibleDC(refDC);
    m_bbBmp = CreateCompatibleBitmap(refDC, w, h);
    m_bbBmpOld = static_cast<HBITMAP>(SelectObject(m_bbDC, m_bbBmp));
    m_bbW = w; m_bbH = h;
}

void RemoteClientsWnd::DestroyBackBuffer() {
    if (!m_bbDC) return;
    SelectObject(m_bbDC, m_bbBmpOld);
    DeleteObject(m_bbBmp);
    DeleteDC(m_bbDC);
    m_bbDC = nullptr; m_bbBmp = nullptr; m_bbBmpOld = nullptr; m_bbW = m_bbH = 0;
}

void RemoteClientsWnd::Repaint() {
    // The buttons follow the SELECTION, and the selection moves without
    // rebuilding rows — arrow keys and a click both just repaint. Done here
    // because it is the one call every one of those paths already makes, so a
    // new way to move the selection cannot forget it and leave four buttons
    // lying about what they will act on.
    SyncButtons();
    if (GetHwnd()) InvalidateRect(GetHwnd(), nullptr, FALSE);
}

int RemoteClientsWnd::HitTestRow(POINT pt) const {
    for (size_t i = 0; i < m_rows.size(); ++i)
        if (m_rows[i].kind != Kind::Header && m_rows[i].kind != Kind::Note &&
            PtInRect(&m_rows[i].rect, pt))
            return static_cast<int>(i);
    return -1;
}

int RemoteClientsWnd::HitTestButton(POINT pt) const {
    for (size_t i = 0; i < m_buttons.size(); ++i)
        if (m_buttons[i].enabled && PtInRect(&m_buttons[i].rect, pt))
            return static_cast<int>(i);
    return -1;
}

// =============================================================================
// Keyboard
// =============================================================================
bool RemoteClientsWnd::OnKeyDown(WPARAM vk, bool, bool, bool) {
    auto step = [&](int dir) {
        int i = m_selected;
        for (int n = 0; n < static_cast<int>(m_rows.size()); ++n) {
            i += dir;
            if (i < 0 || i >= static_cast<int>(m_rows.size())) return;
            if (m_rows[i].kind != Kind::Header && m_rows[i].kind != Kind::Note) {
                m_selected = i;
                // The selection must never walk off screen — that is the whole
                // difference between a list you can drive from the keyboard and
                // one you can only drive with the wheel.
                EnsureSelectedVisible();
                Repaint();
                return;
            }
        }
    };

    switch (vk) {
        case VK_UP:   step(-1); return true;
        case VK_DOWN: step(+1); return true;
        // Page and end move the VIEW, not the selection — the list is short
        // enough that hunting for a row is a scroll, not a walk.
        case VK_PRIOR: m_list.ScrollBy(0, -m_list.Height()); Repaint(); return true;
        case VK_NEXT:  m_list.ScrollBy(0,  m_list.Height()); Repaint(); return true;
        case VK_HOME:  m_list.scrollY = 0;                Repaint(); return true;
        case VK_END:   m_list.scrollY = m_list.MaxScrollY(); Repaint(); return true;
        // DELETE kicks, because it is the destructive key and a plain kick is
        // the reversible one of the three. The other two are deliberate enough
        // to want a button.
        case VK_DELETE:
            if (SelectedClient() >= 0) { DoKick(); return true; }
            if (SelectedBlock()  >= 0) { DoLiftBlock(); return true; }
            return true;
        case VK_F5: BuildRows(); Repaint(); return true;
        default: break;
    }
    return false; // unhandled keys go to the app pipeline, per FloatingPanelWnd
}

// =============================================================================
// Message handling
// =============================================================================
LRESULT RemoteClientsWnd::HandlePanelMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_SETCURSOR: {
            if (LOWORD(lParam) != HTCLIENT) break;
            POINT pt; GetCursorPos(&pt);
            ScreenToClient(GetHwnd(), &pt);
            // The bars are not tested here — the base already answered for them
            // and only passes this on when the cursor is somewhere else.
            SetCursor((HitTestButton(pt) >= 0 || HitTestRow(pt) >= 0)
                          ? Constants::Cursors::CURR_CLICK
                          : Constants::Cursors::CURR_DEFAULT);
            return TRUE;
        }

        case WM_TIMER: {
            if (wParam != TIMER_REFRESH) break;
            // The whole reason this panel has a timer: connections arrive and
            // leave on socket threads, timed blocks count down, and neither
            // sends the UI anything. Rebuilding is two vector copies.
            BuildRows();
            Repaint();
            return 0;
        }

        case WM_SHOWWINDOW: {
            // Stop the timer when the panel goes away, start it when it comes
            // back. A hidden panel polling the server once a second would be
            // pure waste in a viewer that never opens this window.
            if (GetHwnd()) {
                if (wParam) SetTimer(GetHwnd(), TIMER_REFRESH, REFRESH_INTERVAL, nullptr);
                else        KillTimer(GetHwnd(), TIMER_REFRESH);
            }
            break;
        }

        // No wheel, drag, paging or bar-cursor cases here: FloatingPanelWnd
        // handles all of it against ScrollViewAt() and consumes the message
        // before this panel is asked. What remains below is the panel's own
        // business — its rows and its buttons.
        case WM_MOUSEMOVE: {
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};

            // The thumb's own hot state is the base's — it maintains it on every
            // move and consumes the ones over a bar, so testing it here would
            // both duplicate that and never see the interesting case.
            const int r = HitTestRow(pt);
            const int b = HitTestButton(pt);
            if (r != m_hotRow || b != m_hotButton) {
                m_hotRow = r; m_hotButton = b;
                Repaint();
            }
            return 0;
        }

        case WM_LBUTTONDOWN: {
            SetFocus(GetHwnd());
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};

            const int b = HitTestButton(pt);
            if (b >= 0) {
                switch (m_buttons[b].id) {
                    case BTN_KICK:   DoKick();      break;
                    case BTN_TIMED:  DoTimedKick(); break;
                    case BTN_BAN:    DoBan();       break;
                    case BTN_LIFT:   DoLiftBlock(); break;
                    case BTN_SERVER: uiManager.getRemoteWindow().ToggleToFront(); break;
                    default: break;
                }
                return 0;
            }

            const int r = HitTestRow(pt);
            if (r >= 0) { m_selected = r; Repaint(); }
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

            const float s    = app.dpiScale;
            const int pad    = static_cast<int>(PAD * s);
            const int rowH   = static_cast<int>(ROW_H * s);
            const int hdrH   = static_cast<int>(HDR_H * s);
            const int btnH   = static_cast<int>(BTN_H * s);
            const int labelW = static_cast<int>(210 * s);

            // ── Title ────────────────────────────────────────────────────────
            SelectObject(bb, m_hFontBold);
            SetTextColor(bb, fg);
            RECT tr{pad, static_cast<int>(6 * s), W - pad, static_cast<int>(26 * s)};
            // COUNTS IN THE TITLE, matching Servers and Mirroring. The footer
            // already reports clients/max, but that line is also where errors and
            // action results land — a total belongs where it cannot be replaced.
            int watchingCount = 0;
            for (const Remote::ClientInfo &c : m_conns)
                if (c.observing) ++watchingCount;

            const std::wstring clientsTitle =
                L"\U0001F64B Clients — who connected to this instance   \x00B7   " +
                std::to_wstring(m_conns.size()) + L" connected, " +
                std::to_wstring(watchingCount) + L" watching";

            DrawTextW(bb, clientsTitle.c_str(),
                      -1, &tr, DT_LEFT | DT_SINGLELINE);

            // ── Buttons ──────────────────────────────────────────────────────
            {
                const int gap   = static_cast<int>(BTN_GAP * s);
                const int count = static_cast<int>(m_buttons.size());
                const int total = W - pad * 2 - gap * (count - 1);
                const int bw    = total / count;
                int       x     = pad;
                const int y     = static_cast<int>(TITLE_H * s);

                SelectObject(bb, m_hFontBody);
                for (int i = 0; i < count; ++i) {
                    Button &btn = m_buttons[static_cast<size_t>(i)];
                    btn.rect = {x, y, x + bw, y + btnH};

                    // Ban is the one that writes a permanent decision to a file,
                    // so it does not share the others' colour. Nothing here
                    // stops a mis-click — the confirm does that — but a button
                    // that looks like its neighbours invites the mis-click.
                    COLORREF base = (btn.id == BTN_BAN) ? PC::BTN_ALT : PC::BTN_MAIN;
                    if (!btn.enabled) base = bg;
                    else if (i == m_hotButton)
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

            // ── Rows ─────────────────────────────────────────────────────────
            const int listTop = static_cast<int>(TITLE_H * s) + btnH +
                                static_cast<int>(12 * s);
            const int listBot = H - static_cast<int>(FOOTER_H * s);
            const int sbW     = UI::ScrollBarThicknessPx(s);

            LayoutRows(rowH, hdrH);

            // Which bars, where the rows go, and the clamp — one call. The bar's
            // width comes out of the row area, so a row never runs under a thumb,
            // and a shorter list cannot leave a stale offset behind (clients
            // disconnect on their own, and an offset past the new end paints an
            // empty list that reads as a bug).
            m_list.Layout(RECT{pad, listTop, W - pad, listBot}, sbW);

            // The deferred scroll from the last rebuild, now that there are
            // positions to scroll to. Consumed, not left set: the wheel must be
            // able to scroll away from the selection afterwards.
            if (m_scrollToSelection) {
                m_scrollToSelection = false;
                EnsureSelectedVisible();
            }

            {
                const int saved = SaveDC(bb);
                IntersectClipRect(bb, m_list.view.left, m_list.view.top,
                                  m_list.view.right, m_list.view.bottom);

                for (size_t i = 0; i < m_rows.size(); ++i) {
                    Row &r = m_rows[i];

                    // Content space → screen space. One subtraction, in one
                    // place, so nothing below has to know about scrolling.
                    const int y = m_list.view.top + r.cy - m_list.scrollY;

                    // Entirely outside: not drawn, and its rect CLEARED so the
                    // hit test cannot find a row that is not on screen. The
                    // clip would hide it either way; an uncleared rect would
                    // still be clickable.
                    if (y + r.ch <= m_list.view.top || y >= m_list.view.bottom) {
                        r.rect = RECT{};
                        continue;
                    }

                    const int rowRight = m_list.view.right;

                    if (r.kind == Kind::Header) {
                        r.rect = {pad, y, rowRight, y + hdrH};
                        SelectObject(bb, m_hFontBold);
                        SetTextColor(bb, PC::HEADER);
                        RECT hr{pad + static_cast<int>(6 * s), y, rowRight, y + hdrH};
                        DrawTextW(bb, r.label.c_str(), -1, &hr,
                                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);

                        RECT st{pad, y + hdrH / 4, pad + static_cast<int>(3 * s),
                                y + hdrH * 3 / 4};
                        FillRect(bb, &st, Gdi::Brush(PC::STRIPE));
                        continue;
                    }

                    if (r.kind == Kind::Note) {
                        r.rect = {pad, y, rowRight, y + hdrH};
                        SelectObject(bb, m_hFontBody);
                        SetTextColor(bb, dim);
                        RECT nr{pad + static_cast<int>(6 * s), y, rowRight, y + hdrH};
                        DrawTextW(bb, r.label.c_str(), -1, &nr,
                                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                        continue;
                    }

                    r.rect = {pad, y, rowRight, y + rowH};

                    if (static_cast<int>(i) == m_selected || static_cast<int>(i) == m_hotRow)
                        FillRect(bb, &r.rect,
                                 Gdi::Brush(static_cast<int>(i) == m_selected ? selBg : hotBg));

                    SelectObject(bb, m_hFontBody);
                    SetTextColor(bb, fg);
                    RECT lr{pad + static_cast<int>(6 * s), y, pad + labelW,
                            y + static_cast<int>(22 * s)};
                    DrawTextW(bb, r.label.c_str(), -1, &lr,
                              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS);

                    // A blocked address reads as a warning, a connected one as
                    // ordinary text — the two lists are next to each other and
                    // the colour is what tells them apart at a glance.
                    SetTextColor(bb, r.kind == Kind::Block ? PC::WARN : PC::TEXT);
                    RECT vr{pad + labelW, y, rowRight - static_cast<int>(6 * s),
                            y + static_cast<int>(22 * s)};
                    DrawTextW(bb, r.value.c_str(), -1, &vr,
                              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

                    SelectObject(bb, m_hFontSmall);
                    SetTextColor(bb, dim);
                    RECT dr{pad + static_cast<int>(6 * s), y + static_cast<int>(21 * s),
                            rowRight, y + rowH};
                    DrawTextW(bb, r.desc.c_str(), -1, &dr,
                              DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
                }

                RestoreDC(bb, saved);
            }

            // Whichever bars Layout decided on. No "if it needs one" here — that
            // question was answered above and is not asked twice.
            // The drag flag lives in the base now; the thumb's own hot state is
            // enough to keep it highlighted while it is being dragged.
            DrawBars(bb, m_list, s,
                     ThemeScrollBarColors(app.themeFactor));

            // ── Footer ───────────────────────────────────────────────────────
            {
                const int fy = H - static_cast<int>(FOOTER_H * s);
                HGDIOBJ op = SelectObject(bb, Gdi::Pen(line));
                MoveToEx(bb, pad, fy - static_cast<int>(4 * s), nullptr);
                LineTo(bb, W - pad, fy - static_cast<int>(4 * s));
                SelectObject(bb, op);

                SelectObject(bb, m_hFontBody);
                SetTextColor(bb, Remote::IsRunning() ? PC::ON : dim);
                RECT st{pad, fy, W - pad, fy + static_cast<int>(20 * s)};
                const std::wstring status = StatusLine();
                DrawTextW(bb, status.c_str(), -1, &st,
                          DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

                SelectObject(bb, m_hFontSmall);
                SetTextColor(bb, dim);
                RECT rr{pad, fy + static_cast<int>(20 * s), W - pad,
                        fy + static_cast<int>(38 * s)};
                DrawTextW(bb, m_lastResult.c_str(), -1, &rr,
                          DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
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
