#include "RemoteWnd.h"
#include "RemoteSettings.h"
#include "RemoteServer.h"
#include "RemoteClient.h"
#include "RemoteCrypto.h"

#include "AppState.h"
#include "Dedicated/DedicatedSettings.h" // SettingsFilePath — what Save writes to
#include "Platform/Constants.h"
#include "Platform/ConstantsStrings.h"
#include "UI/ThemedDialog.h"

#include <algorithm>
#include <thread>
#include <windowsx.h>

extern AppState app;

namespace UI {

namespace RT  = Constants::RemoteTcpIp;
namespace PC  = Constants::Dedicated::PanelColors;

namespace {
    constexpr int PANEL_W  = 700;
    constexpr int PANEL_H  = 640;
    constexpr int PAD      = 14;
    constexpr int ROW_H    = 42;
    constexpr int HDR_H    = 30;
    constexpr int BTN_H    = 34;
    constexpr int BTN_GAP  = 8;
    constexpr int TITLE_H  = 44;
    constexpr int FOOTER_H = 46; // two lines: status, then last async result

    // Posted by the peer worker thread. LPARAM = new std::wstring*, owned and
    // deleted by the handler. WM_APP rather than WM_USER: WM_USER+N is already
    // spoken for by the app's own WM_QIV_* messages, and a panel's private
    // message space must not collide with them.
    constexpr UINT WM_REMOTE_ASYNC_RESULT = WM_APP + 1;

    enum ButtonId {
        BTN_START = 1, BTN_STOP, BTN_CHECK,
        BTN_SEND, BTN_SAVE
    };

    enum RowId {
        R_NONE = 0,
        // Server
        R_ENABLE, R_NAME, R_BIND, R_PORT, R_ALLOW, R_BLOCK, R_PASSWORD, R_MAXCONN,
        // Peer
        R_PEER_HOST, R_PEER_PORT, R_PEER_PASSWORD, R_PEER_COMMAND,
    };

    bool BgIsDark(COLORREF bg) {
        const int lum = (GetRValue(bg) * 299 + GetGValue(bg) * 587 + GetBValue(bg) * 114) / 1000;
        return lum < 128;
    }

    std::wstring OnOff(bool b) { return b ? L"On" : L"Off"; }

    std::wstring OrUnset(const std::wstring &s) { return s.empty() ? L"(not set)" : s; }
}

// =============================================================================
// Init / Show
// =============================================================================
void RemoteWnd::Init(HINSTANCE hInstance, HWND hParent) {
    const float s = app.dpiScale;
    InitFloating(hInstance, hParent, L"qIVRemoteWnd", L"Remote Control",
                 static_cast<int>(PANEL_W * s), static_cast<int>(PANEL_H * s));
    if (GetHwnd()) {
        SetWindowLongPtrW(GetHwnd(), GWL_EXSTYLE,
                          GetWindowLongPtrW(GetHwnd(), GWL_EXSTYLE) | WS_EX_LAYERED);
        SetLayeredWindowAttributes(GetHwnd(), 0,
                                   Constants::Dedicated::PANEL_OPACITY, LWA_ALPHA);
    }
    m_edit.SetMaxLength(256);
    BuildRows();
}

void RemoteWnd::Init(HINSTANCE hInstance, HWND hParent, int8_t) { Init(hInstance, hParent); }

void RemoteWnd::Show() {
    // Re-read on every open so the panel always describes what this instance is
    // actually running, not what was last typed here.
    Remote::LoadFromIni();
    BuildRows();
    ShowCenterOverParent();
    Repaint();
}

// =============================================================================
// Model
// =============================================================================
void RemoteWnd::BuildRows() {
    const Remote::Settings &c = Remote::Config();
    m_rows.clear();

    auto add = [&](Kind k, const wchar_t *label, std::wstring value,
                   const wchar_t *desc, int id) {
        Row r; r.kind = k; r.label = label; r.value = std::move(value);
        r.desc = desc; r.id = id;
        m_rows.push_back(std::move(r));
    };

    add(Kind::Header, L"Server", L"", L"", R_NONE);
    add(Kind::Toggle, L"Enable", OnOff(c.enable),
        L"Master switch. Off by default — a viewer nobody configured never opens a socket.", R_ENABLE);
    add(Kind::Text, L"Name", OrUnset(c.name),
        L"How this instance identifies itself to a connecting client.", R_NAME);
    add(Kind::Choice, L"Bind address", c.bindAddress,
        L"127.0.0.1 = this machine only, no firewall prompt.  0.0.0.0 = every network interface.", R_BIND);
    add(Kind::Number, L"Port",
        c.port == RT::PORT_UNSET ? std::wstring(L"(not set)") : std::to_wstring(c.port),
        L"The port to listen on. Required — there is no default.", R_PORT);
    add(Kind::Text, L"AllowList", OrUnset(Remote::JoinList(c.allowList)),
        L"IPs allowed to connect. EMPTY DENIES EVERYONE. Trailing * matches a subnet: 192.168.1.*", R_ALLOW);
    add(Kind::Text, L"BlackList", OrUnset(Remote::JoinList(c.blackList)),
        L"IPs always refused. Checked first — deny always beats allow.", R_BLOCK);
    add(Kind::Secret, L"Password", c.passwordHash.empty() ? L"(none)" : L"(set)",
        L"Stored hashed, never in plain text. Sent as a challenge-response, so it never crosses the wire.", R_PASSWORD);
    add(Kind::Number, L"Max connections", std::to_wstring(c.maxConnections),
        L"Simultaneous clients, 1-99. Further callers are told the limit was reached.", R_MAXCONN);

    add(Kind::Header, L"Connect to another instance", L"", L"", R_NONE);
    add(Kind::Text, L"Target host", OrUnset(m_peerHost),
        L"Address or host name of the qIV instance to drive. Not saved — it describes a neighbour, not this screen.", R_PEER_HOST);
    add(Kind::Number, L"Target port",
        m_peerPort == 0 ? std::wstring(L"(not set)") : std::to_wstring(m_peerPort),
        L"The port that instance is listening on.", R_PEER_PORT);
    add(Kind::Secret, L"Target password", m_peerPassword.empty() ? L"(none)" : L"(set)",
        L"Its password, if it has one. Used to answer the challenge; never sent as text.", R_PEER_PASSWORD);
    add(Kind::Text, L"Command", m_peerCommand,
        L"What to send. Try: ping, next, goto 42, zoom 200, help", R_PEER_COMMAND);

    m_buttons.clear();
    const bool running = Remote::IsRunning();
    m_buttons.push_back({L"Start",            BTN_START, {}, !running, 0});
    m_buttons.push_back({L"Stop",             BTN_STOP,  {},  running, 0});
    m_buttons.push_back({L"Check Connection", BTN_CHECK, {}, true,     0});
    m_buttons.push_back({L"Send Command",     BTN_SEND,  {}, true,     1});
    m_buttons.push_back({L"Save to INI",      BTN_SAVE,  {}, true,     1});
}

std::wstring RemoteWnd::StatusLine() const {
    if (Remote::IsRunning()) {
        std::wstring s = L"Listening on " + Remote::BoundEndpoint() +
                         L"   clients: " + std::to_wstring(Remote::ActiveConnections()) +
                         L"/" + std::to_wstring(Remote::Config().maxConnections);
        // An empty AllowList binds and listens but refuses every caller. Saying
        // only "Listening" there would make a deliberately closed server look
        // like a broken one.
        if (Remote::Config().allowList.empty())
            s += L"   — " + std::wstring(Constants::Messages::REMOTE_WARN_EMPTY_ALLOWLIST);
        return s;
    }
    const std::wstring why = Remote::WhyCannotStart(Remote::Config());
    return why.empty() ? std::wstring(L"Stopped") : L"Stopped — " + why;
}

// =============================================================================
// Actions
// =============================================================================
void RemoteWnd::DoStart() {
    PushToConfig();
    std::wstring err;
    if (!Remote::Start(GetHwnd(), err)) {
        DialogMessage(err.empty() ? L"Could not start." : err, L"Remote Control");
    } else {
        m_lastResult = L"Started on " + Remote::BoundEndpoint();
    }
    BuildRows();
    Repaint();
}

void RemoteWnd::DoStop() {
    Remote::Stop();
    m_lastResult = L"Stopped.";
    BuildRows();
    Repaint();
}

// Both peer actions run here. The lambda-free function-pointer form keeps the
// worker's captured state explicit: it touches only `self`, and everything it
// writes back travels through the posted message rather than through members.
void RemoteWnd::RunAsync(void (*work)(RemoteWnd *self)) {
    std::thread(work, this).detach();
}

void RemoteWnd::DoCheckConnection() {
    PushToConfig();
    if (m_peerHost.empty() || m_peerPort == 0) {
        DialogMessage(L"Set a target host and port first.", L"Remote Control");
        return;
    }
    m_lastResult = L"Checking " + m_peerHost + L":" + std::to_wstring(m_peerPort) + L"…";
    Repaint();

    RunAsync([](RemoteWnd *self) {
        std::wstring info, err;
        const bool ok = Remote::Probe(self->m_peerHost, self->m_peerPort,
                                      self->m_peerPassword, info, err);
        auto *msg = new std::wstring(ok ? L"Reachable — " + info
                                        : L"Unreachable — " + err);
        PostMessageW(self->GetHwnd(), WM_REMOTE_ASYNC_RESULT, 0,
                     reinterpret_cast<LPARAM>(msg));
    });
}

void RemoteWnd::DoSendToPeer() {
    PushToConfig();
    if (m_peerHost.empty() || m_peerPort == 0) {
        DialogMessage(L"Set a target host and port first.", L"Remote Control");
        return;
    }
    if (m_peerCommand.empty()) {
        DialogMessage(L"Type a command to send.", L"Remote Control");
        return;
    }
    m_lastResult = L"Sending…";
    Repaint();

    RunAsync([](RemoteWnd *self) {
        Remote::Client c;
        std::wstring err, reply;
        std::wstring out;
        if (!c.Connect(self->m_peerHost, self->m_peerPort, self->m_peerPassword, err)) {
            out = L"Connect failed — " + err;
        } else if (!c.Send(self->m_peerCommand, reply, err)) {
            out = L"Send failed — " + err;
        } else {
            out = reply;
        }
        c.Disconnect();
        PostMessageW(self->GetHwnd(), WM_REMOTE_ASYNC_RESULT, 0,
                     reinterpret_cast<LPARAM>(new std::wstring(out)));
    });
}

void RemoteWnd::DoSaveToIni() {
    PushToConfig();

    // Creating an .ini where none existed moves the WHOLE APP off the registry
    // onto the file, from the next launch. SaveToIniSeeded makes that lossless by
    // writing every current setting first — but the user still has to know their
    // persistence model just changed, because it affects far more than this panel.
    const bool willCreate = !Remote::IniExists();
    if (willCreate) {
        if (!DialogConfirm(
                L"No settings file exists for this instance yet.\r\n\r\n"
                L"Saving creates one beside the exe. From the next launch qIV will "
                L"read ALL of its settings from that file instead of the registry.\r\n\r\n"
                L"Your current settings are copied into it first, so nothing is lost.\r\n\r\n"
                L"Create it?",
                L"Remote Control"))
            return;
    }

    bool created = false;
    Remote::SaveToIniSeeded(created);
    m_lastResult = created ? L"Created " + Dedicated::SettingsFilePath()
                           : L"Saved to " + Dedicated::SettingsFilePath();
    BuildRows();
    Repaint();
}

// =============================================================================
// Row editing
// =============================================================================
void RemoteWnd::PullFromConfig() { BuildRows(); }

void RemoteWnd::PushToConfig() {
    // The rows are the display; Config() is the truth. Only the fields the panel
    // owns are written back, and the password only when a new one was typed.
    Remote::Settings &c = Remote::Config();
    if (!m_newPassword.empty()) {
        const std::wstring hashed = Remote::Crypto::HashPassword(m_newPassword);
        if (!hashed.empty()) c.passwordHash = hashed;
        m_newPassword.clear(); // never retained beyond the save that consumes it
    }
    Remote::Normalize(c);
}

void RemoteWnd::EditRow(int rowIndex) {
    if (rowIndex < 0 || rowIndex >= static_cast<int>(m_rows.size())) return;
    Row &r = m_rows[rowIndex];
    Remote::Settings &c = Remote::Config();

    switch (r.id) {
        case R_ENABLE:
            c.enable = !c.enable;
            BuildRows();
            Repaint();
            return;

        case R_BIND: {
            // Cycle the two useful literals, then fall through to free text —
            // those two cover almost every case and typing them is error-prone.
            if (c.bindAddress == RT::BIND_ADDRESS_DEFAULT)      c.bindAddress = RT::BIND_ADDRESS_ANY;
            else if (c.bindAddress == RT::BIND_ADDRESS_ANY)     { BeginTextEdit(rowIndex); return; }
            else                                                c.bindAddress = RT::BIND_ADDRESS_DEFAULT;
            BuildRows();
            Repaint();
            return;
        }

        case R_PORT: {
            const int v = DialogPromptInt(L"Listen Port", L"Port (1 - 65535):",
                                          c.port ? c.port : 8770,
                                          RT::PORT_MIN, RT::PORT_MAX, 8770);
            if (v >= 0) c.port = v;
            BuildRows();
            Repaint();
            return;
        }

        case R_MAXCONN: {
            const int v = DialogPromptInt(L"Max Connections",
                                          L"Simultaneous clients (1 - 99):",
                                          c.maxConnections,
                                          RT::MAX_CONNECTIONS_MIN, RT::MAX_CONNECTIONS_MAX,
                                          RT::MAX_CONNECTIONS_DEFAULT);
            if (v >= 0) c.maxConnections = v;
            BuildRows();
            Repaint();
            return;
        }

        case R_PEER_PORT: {
            const int v = DialogPromptInt(L"Target Port", L"Peer port (1 - 65535):",
                                          m_peerPort ? m_peerPort : 8770,
                                          RT::PORT_MIN, RT::PORT_MAX, 8770);
            if (v >= 0) m_peerPort = v;
            BuildRows();
            Repaint();
            return;
        }

        default:
            BeginTextEdit(rowIndex);
            return;
    }
}

void RemoteWnd::BeginTextEdit(int rowIndex) {
    m_editingRow = rowIndex;
    const Row &r = m_rows[rowIndex];

    // A secret is never seeded with what is stored — the panel cannot show a
    // password it only holds as a hash, and pre-filling a placeholder would let
    // an accidental Enter overwrite the real one with literal text.
    //
    // NOTE: InputBox has no masked mode, so a password IS visible while being
    // typed. It is still hashed before storage and never sent as text, so this
    // is a shoulder-surfing exposure only. Adding masking would mean changing a
    // control shared by four other panels — deliberately not done here.
    std::wstring seed;
    if (r.kind != Kind::Secret && r.value != L"(not set)") seed = r.value;

    m_edit.SetText(seed);
    Repaint();
}

void RemoteWnd::CommitTextEdit() {
    if (m_editingRow < 0) return;
    const Row &r = m_rows[m_editingRow];
    const std::wstring text = m_edit.GetText();
    Remote::Settings &c = Remote::Config();

    switch (r.id) {
        case R_NAME:           c.name       = text;                        break;
        case R_BIND:           c.bindAddress = text;                       break;
        case R_ALLOW:          c.allowList  = Remote::ParseList(text);     break;
        case R_BLOCK:          c.blackList  = Remote::ParseList(text);     break;
        // Held as plaintext only until the next save hashes it. Clearing the
        // field is how a password is removed.
        case R_PASSWORD:
            if (text.empty()) { c.passwordHash.clear(); m_newPassword.clear(); }
            else               m_newPassword = text;
            break;
        case R_PEER_HOST:      m_peerHost     = text;                      break;
        case R_PEER_PASSWORD:  m_peerPassword = text;                      break;
        case R_PEER_COMMAND:   m_peerCommand  = text;                      break;
        default: break;
    }

    m_editingRow = -1;
    Remote::Normalize(c);
    BuildRows();
    Repaint();
}

void RemoteWnd::CancelTextEdit() {
    m_editingRow = -1;
    Repaint();
}

// =============================================================================
// Dialog helpers — the panel is topmost, themed dialogs are not, so a dialog
// would open BEHIND it. Drop topmost for the duration and restore after.
// =============================================================================
void RemoteWnd::PushTopmostOff() {
    if (GetHwnd())
        SetWindowPos(GetHwnd(), HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void RemoteWnd::PopTopmost() {
    if (GetHwnd())
        SetWindowPos(GetHwnd(), HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void RemoteWnd::DialogMessage(const std::wstring &text, const wchar_t *caption) {
    PushTopmostOff();
    ThemedDialog::Message(GetHwnd(), text.c_str(), caption);
    PopTopmost();
}

bool RemoteWnd::DialogConfirm(const std::wstring &text, const wchar_t *caption) {
    PushTopmostOff();
    const bool r = ThemedDialog::Confirm(GetHwnd(), text.c_str(), caption);
    PopTopmost();
    return r;
}

int RemoteWnd::DialogPromptInt(const wchar_t *caption, const wchar_t *label,
                               int cur, int lo, int hi, int def) {
    PushTopmostOff();
    const int r = ThemedDialog::PromptInt(GetHwnd(), caption, label, cur, lo, hi, def);
    PopTopmost();
    return r;
}

// =============================================================================
// Paint plumbing
// =============================================================================
void RemoteWnd::EnsureFonts(HDC dc) {
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

void RemoteWnd::EnsureBackBuffer(HDC refDC, int w, int h) {
    if (m_bbDC && m_bbW == w && m_bbH == h) return;
    DestroyBackBuffer();
    m_bbDC = CreateCompatibleDC(refDC);
    m_bbBmp = CreateCompatibleBitmap(refDC, w, h);
    m_bbBmpOld = static_cast<HBITMAP>(SelectObject(m_bbDC, m_bbBmp));
    m_bbW = w; m_bbH = h;
}

void RemoteWnd::DestroyBackBuffer() {
    if (!m_bbDC) return;
    SelectObject(m_bbDC, m_bbBmpOld);
    DeleteObject(m_bbBmp);
    DeleteDC(m_bbDC);
    m_bbDC = nullptr; m_bbBmp = nullptr; m_bbBmpOld = nullptr; m_bbW = m_bbH = 0;
}

void RemoteWnd::Repaint() { if (GetHwnd()) InvalidateRect(GetHwnd(), nullptr, FALSE); }

int RemoteWnd::HitTestRow(POINT pt) const {
    for (size_t i = 0; i < m_rows.size(); ++i)
        if (m_rows[i].kind != Kind::Header && PtInRect(&m_rows[i].rect, pt))
            return static_cast<int>(i);
    return -1;
}

int RemoteWnd::HitTestButton(POINT pt) const {
    for (size_t i = 0; i < m_buttons.size(); ++i)
        if (m_buttons[i].enabled && PtInRect(&m_buttons[i].rect, pt))
            return static_cast<int>(i);
    return -1;
}

// =============================================================================
// Keyboard
// =============================================================================
bool RemoteWnd::OnKeyDown(WPARAM vk, bool, bool, bool) {
    // Enter/Escape are handled BEFORE delegating: InputBox has no notion of
    // committing to a host, so the panel owns those two.
    if (m_editingRow >= 0) {
        if (vk == VK_RETURN) { CommitTextEdit(); return true; }
        if (vk == VK_ESCAPE) { CancelTextEdit(); Repaint(); return true; }
        const InputResult r = m_edit.RouteKey(vk, GetHwnd());
        if (r != InputResult::Ignored) {
            if (r == InputResult::ConsumedRepaint) Repaint();
            return true;
        }
        return false;
    }

    auto step = [&](int dir) {
        int i = m_selected;
        for (int n = 0; n < static_cast<int>(m_rows.size()); ++n) {
            i += dir;
            if (i < 0 || i >= static_cast<int>(m_rows.size())) return;
            if (m_rows[i].kind != Kind::Header) { m_selected = i; Repaint(); return; }
        }
    };

    switch (vk) {
        case VK_UP:   step(-1); return true;
        case VK_DOWN: step(+1); return true;
        case VK_RETURN:
        case VK_SPACE: EditRow(m_selected); return true;
        default: break;
    }
    return false; // unhandled keys go to the app pipeline, per FloatingPanelWnd
}

bool RemoteWnd::OnLocalHide() {
    // Esc while editing abandons the edit rather than closing the panel — the
    // same rule the other panels' filter boxes follow.
    if (m_editingRow >= 0) { CancelTextEdit(); return true; }
    return false;
}

// =============================================================================
// Message handling
// =============================================================================
LRESULT RemoteWnd::HandlePanelMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        // A peer worker finished. It owns nothing but the string it posted.
        case WM_REMOTE_ASYNC_RESULT: {
            auto *msg = reinterpret_cast<std::wstring *>(lParam);
            if (msg) { m_lastResult = *msg; delete msg; }
            Repaint();
            return 0;
        }

        case WM_SETCURSOR: {
            if (LOWORD(lParam) != HTCLIENT) break;
            POINT pt; GetCursorPos(&pt);
            ScreenToClient(GetHwnd(), &pt);
            SetCursor((HitTestButton(pt) >= 0 || HitTestRow(pt) >= 0)
                          ? Constants::Cursors::CURR_CLICK
                          : Constants::Cursors::CURR_DEFAULT);
            return TRUE;
        }

        case WM_MOUSEMOVE: {
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
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
                    case BTN_START: DoStart();           break;
                    case BTN_STOP:  DoStop();            break;
                    case BTN_CHECK: DoCheckConnection(); break;
                    case BTN_SEND:  DoSendToPeer();      break;
                    case BTN_SAVE:  DoSaveToIni();       break;
                    default: break;
                }
                return 0;
            }

            const int r = HitTestRow(pt);
            if (r >= 0) {
                if (m_editingRow >= 0 && m_editingRow != r) CommitTextEdit();
                m_selected = r;
                EditRow(r);
            }
            return 0;
        }

        case WM_CHAR: {
            if (m_editingRow >= 0) {
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

            HBRUSH bgb = CreateSolidBrush(bg); FillRect(bb, &rc, bgb); DeleteObject(bgb);
            SetBkMode(bb, TRANSPARENT);

            const float s      = app.dpiScale;
            const int pad      = static_cast<int>(PAD * s);
            const int rowH     = static_cast<int>(ROW_H * s);
            const int hdrH     = static_cast<int>(HDR_H * s);
            const int btnH     = static_cast<int>(BTN_H * s);
            const int labelW   = static_cast<int>(150 * s);

            // ── Title ────────────────────────────────────────────────────────
            SelectObject(bb, m_hFontBold);
            SetTextColor(bb, fg);
            RECT tr{pad, static_cast<int>(6 * s), W - pad, static_cast<int>(26 * s)};
            DrawTextW(bb, L"Remote control (TCP/IP)", -1, &tr, DT_LEFT | DT_SINGLELINE);

            SelectObject(bb, m_hFontSmall);
            SetTextColor(bb, dim);
            RECT sr{pad, tr.bottom, W - pad, tr.bottom + static_cast<int>(16 * s)};
            DrawTextW(bb, Remote::IniExists()
                              ? L"Settings file present — values persist here"
                              : L"No settings file — Save to INI creates one",
                      -1, &sr, DT_LEFT | DT_SINGLELINE | DT_PATH_ELLIPSIS);

            // ── Buttons ──────────────────────────────────────────────────────
            {
                const int gap = static_cast<int>(BTN_GAP * s);
                SelectObject(bb, m_hFontBody);
                for (int rowIdx = 0; rowIdx <= 1; ++rowIdx) {
                    int count = 0;
                    for (const Button &b2 : m_buttons) if (b2.row == rowIdx) ++count;
                    if (count == 0) continue;

                    const int total = W - pad * 2 - gap * (count - 1);
                    const int bw = total / count;
                    int x = pad;
                    const int y = static_cast<int>(TITLE_H * s) + rowIdx * (btnH + gap);

                    for (Button &btn : m_buttons) {
                        if (btn.row != rowIdx) continue;
                        btn.rect = {x, y, x + bw, y + btnH};
                        const int myIndex = static_cast<int>(&btn - m_buttons.data());
                        COLORREF base = (btn.row == 0) ? PC::BTN_MAIN : PC::BTN_ALT;
                        if (!btn.enabled) base = bg;
                        else if (myIndex == m_hotButton)
                            base = RGB(std::min(255, GetRValue(base) + 40),
                                       std::min(255, GetGValue(base) + 40),
                                       std::min(255, GetBValue(base) + 40));

                        HBRUSH bbr = CreateSolidBrush(base);
                        FillRect(bb, &btn.rect, bbr);
                        DeleteObject(bbr);

                        HPEN pen = CreatePen(PS_SOLID, 1, line);
                        HGDIOBJ op = SelectObject(bb, pen);
                        HGDIOBJ ob = SelectObject(bb, GetStockObject(NULL_BRUSH));
                        Rectangle(bb, btn.rect.left, btn.rect.top, btn.rect.right, btn.rect.bottom);
                        SelectObject(bb, ob); SelectObject(bb, op); DeleteObject(pen);

                        SetTextColor(bb, btn.enabled ? RGB(245,245,245) : dim);
                        RECT lr = btn.rect;
                        DrawTextW(bb, btn.label.c_str(), -1, &lr,
                                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                        x += bw + gap;
                    }
                }
            }

            // ── Rows ─────────────────────────────────────────────────────────
            int y = static_cast<int>(TITLE_H * s) + btnH * 2 +
                    static_cast<int>(BTN_GAP * s) + static_cast<int>(10 * s);

            for (size_t i = 0; i < m_rows.size(); ++i) {
                Row &r = m_rows[i];

                if (r.kind == Kind::Header) {
                    r.rect = {pad, y, W - pad, y + hdrH};
                    SelectObject(bb, m_hFontBold);
                    SetTextColor(bb, PC::HEADER);
                    RECT hr{pad + static_cast<int>(6 * s), y, W - pad, y + hdrH};
                    DrawTextW(bb, r.label.c_str(), -1, &hr,
                              DT_LEFT | DT_VCENTER | DT_SINGLELINE);

                    HBRUSH stripe = CreateSolidBrush(PC::STRIPE);
                    RECT st{pad, y + hdrH / 4, pad + static_cast<int>(3 * s), y + hdrH * 3 / 4};
                    FillRect(bb, &st, stripe);
                    DeleteObject(stripe);

                    y += hdrH;
                    continue;
                }

                r.rect = {pad, y, W - pad, y + rowH};

                if (static_cast<int>(i) == m_selected || static_cast<int>(i) == m_hotRow) {
                    HBRUSH hb = CreateSolidBrush(
                        static_cast<int>(i) == m_selected ? selBg : hotBg);
                    FillRect(bb, &r.rect, hb);
                    DeleteObject(hb);
                }

                SelectObject(bb, m_hFontBody);
                SetTextColor(bb, fg);
                RECT lr{pad + static_cast<int>(6 * s), y, pad + labelW,
                        y + static_cast<int>(22 * s)};
                DrawTextW(bb, r.label.c_str(), -1, &lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

                // Value colour carries the state, so a glance is enough.
                COLORREF vc = PC::TEXT;
                if (r.kind == Kind::Toggle) vc = (r.value == L"On") ? PC::ON : PC::OFF;
                else if (r.kind == Kind::Number) vc = PC::NUMBER;
                else if (r.kind == Kind::Choice) vc = PC::CHOICE;
                if (r.value == L"(not set)" || r.value == L"(none)") vc = PC::WARN;

                RECT vr{pad + labelW, y, W - pad - static_cast<int>(6 * s),
                        y + static_cast<int>(22 * s)};

                if (m_editingRow == static_cast<int>(i)) {
                    m_edit.Draw(bb, m_hFontBody, vr, static_cast<int>(4 * s),
                                GetFocus() == GetHwnd());
                } else {
                    SetTextColor(bb, vc);
                    DrawTextW(bb, r.value.c_str(), -1, &vr,
                              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS);
                }

                SelectObject(bb, m_hFontSmall);
                SetTextColor(bb, dim);
                RECT dr{pad + static_cast<int>(6 * s), y + static_cast<int>(21 * s),
                        W - pad, y + rowH};
                DrawTextW(bb, r.desc, -1, &dr, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

                y += rowH;
            }

            // ── Footer: live status, then the last async result ──────────────
            {
                const int fy = H - static_cast<int>(FOOTER_H * s);
                HPEN pen = CreatePen(PS_SOLID, 1, line);
                HGDIOBJ op = SelectObject(bb, pen);
                MoveToEx(bb, pad, fy - static_cast<int>(4 * s), nullptr);
                LineTo(bb, W - pad, fy - static_cast<int>(4 * s));
                SelectObject(bb, op); DeleteObject(pen);

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
