#include "RemoteWnd.h"
#include "RemoteSettings.h"
#include "RemoteServer.h"
#include "RemoteCrypto.h"

#include "AppState.h"
#include "Dedicated/DedicatedSettings.h" // SettingsFilePath — what Save writes to
#include "Platform/Constants.h"
#include "Platform/ConstantsStrings.h"
#include "UI/ThemedDialog.h"
#include "UI/GdiPool.h" // brushes and pens are pooled — never DeleteObject them

#include <algorithm>
#include <shlobj_core.h> // ILCreateFromPathW / SHOpenFolderAndSelectItems
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

    enum ButtonId { BTN_START = 1, BTN_STOP, BTN_SAVE };

    enum RowId {
        R_NONE = 0,
        R_ENABLE, R_NAME, R_BIND, R_PORT, R_ALLOW, R_BLOCK, R_PASSWORD, R_MAXCONN,
    };

    bool BgIsDark(COLORREF bg) {
        const int lum = (GetRValue(bg) * 299 + GetGValue(bg) * 587 + GetBValue(bg) * 114) / 1000;
        return lum < 128;
    }

    std::wstring OnOff(bool b) { return b ? L"On" : L"Off"; }

    // Lighten a colour by a fixed amount per channel.
    //
    // Explicit masking rather than GetRValue/GetGValue/GetBValue: those macros
    // cast COLORREF to BYTE, which on a constexpr argument trips C4310
    // (constant truncation) under /W4. Constants.h works around the same thing
    // for the link colour's float channels.
    COLORREF Brighten(COLORREF c, int by) {
        const int r = std::min(255, static_cast<int>( c        & 0xFF) + by);
        const int g = std::min(255, static_cast<int>((c >>  8) & 0xFF) + by);
        const int b = std::min(255, static_cast<int>((c >> 16) & 0xFF) + by);
        return RGB(r, g, b);
    }

    std::wstring OrUnset(const std::wstring &s) { return s.empty() ? L"(not set)" : s; }
}

// =============================================================================
// Init / Show
// =============================================================================
void RemoteWnd::Init(HINSTANCE hInstance, HWND hParent) {
    const float s = app.dpiScale;
    InitFloating(hInstance, hParent, L"qIVRemoteWnd", L"Local Server",
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
        L"REQUIRED. How this instance identifies itself — a driving instance records "
        L"this as the row's identity, and names have to be distinct.", R_NAME);
    add(Kind::Choice, L"Bind address", c.bindAddress,
        L"127.0.0.1 = this machine only, no firewall prompt.  0.0.0.0 = every network interface.", R_BIND);
    add(Kind::Number, L"Port",
        c.port == RT::PORT_UNSET ? std::wstring(L"(not set)") : std::to_wstring(c.port),
        L"The port to listen on. Required — there is no default.", R_PORT);
    add(Kind::Text, L"AllowList", OrUnset(Remote::JoinList(c.allowList)),
        L"IPs allowed to connect. EMPTY DENIES EVERYONE, including this machine. Starts as "
        L"127.0.0.1. Trailing * matches a subnet: 192.168.1.*", R_ALLOW);
    add(Kind::Text, L"BlackList", OrUnset(Remote::JoinList(c.blackList)),
        L"IPs always refused. Checked first — deny always beats allow.", R_BLOCK);
    add(Kind::Secret, L"Password", c.passwordHash.empty() ? L"(none)" : L"(set)",
        L"Stored hashed, never in plain text. Sent as a challenge-response, so it never crosses the wire.", R_PASSWORD);
    add(Kind::Number, L"Max connections", std::to_wstring(c.maxConnections),
        L"Simultaneous clients, 1-99. Further callers are told the limit was reached.", R_MAXCONN);

    m_buttons.clear();
    const bool running = Remote::IsRunning();
    m_buttons.push_back({L"Start",       BTN_START, {}, !running, 0});
    m_buttons.push_back({L"Stop",        BTN_STOP,  {},  running, 0});
    m_buttons.push_back({L"Save to INI", BTN_SAVE,  {}, true,     0});
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
        DialogMessage(err.empty() ? L"Could not start." : err, L"Local Server");
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

void RemoteWnd::DoSaveToIni() {
    PushToConfig();

    // Saving a nameless configuration is allowed — half-finished settings are a
    // normal state to leave a panel in — but it will not start, and finding that
    // out later from a status line is worse than being told now.
    if (Remote::Config().name.empty()) {
        DialogMessage(L"This instance has no Name.\r\n\r\nIt will save, but the listener "
                      L"will not start without one: whoever drives this instance "
                      L"identifies it by name, and two unnamed instances would be "
                      L"indistinguishable.",
                      L"Local Server");
    }

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
                L"Local Server"))
            return;
    }

    // An ordinary overwrite still asks. This writes every field in the panel to
    // disk, including a password that was just retyped, and there is no undo —
    // the previous contents are gone. The creation case above asks a bigger
    // question and has already been answered by the time we get here.
    if (!willCreate) {
        if (!DialogConfirm(L"Write these settings to\r\n\r\n    " +
                           Dedicated::SettingsFilePath() +
                           L"\r\n\r\nreplacing what is in the [" +
                           std::wstring(RT::INI_SECTION) + L"] section?",
                           L"Local Server"))
            return;
    }

    bool created = false;
    Remote::SaveToIniSeeded(created);

    m_savedPath  = Dedicated::SettingsFilePath();
    m_lastResult = created ? L"Created " : L"Saved to ";
    BuildRows();
    Repaint();
}

// Reveal rather than open: clicking a path should show you where the file is,
// not launch whatever happens to be associated with .ini on this machine —
// which is a text editor at best and an unknown at worst. Matches the
// reveal-in-Explorer the main window already does for the current image.
void RemoteWnd::RevealSavedFile() {
    if (m_savedPath.empty()) return;
    if (PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(m_savedPath.c_str())) {
        SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
        ILFree(pidl);
    }
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
    // Underline comes from the app-wide link convention rather than a local
    // choice, so every clickable thing in the app looks the same.
    m_hFontLink  = mk(8,  FW_NORMAL, Constants::Links::UNDERLINE ? TRUE : FALSE);
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
        case WM_SETCURSOR: {
            if (LOWORD(lParam) != HTCLIENT) break;
            POINT pt; GetCursorPos(&pt);
            ScreenToClient(GetHwnd(), &pt);
            SetCursor((HitTestButton(pt) >= 0 || HitTestRow(pt) >= 0 ||
                       PtInRect(&m_savedLinkRect, pt))
                          ? Constants::Cursors::CURR_CLICK
                          : Constants::Cursors::CURR_DEFAULT);
            return TRUE;
        }

        case WM_MOUSEMOVE: {
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const int r = HitTestRow(pt);
            const int b = HitTestButton(pt);
            const bool linkHot = PtInRect(&m_savedLinkRect, pt) != FALSE;
            if (r != m_hotRow || b != m_hotButton || linkHot != m_savedLinkHot) {
                m_hotRow = r; m_hotButton = b; m_savedLinkHot = linkHot;
                Repaint();
            }
            return 0;
        }

        case WM_LBUTTONDOWN: {
            SetFocus(GetHwnd());
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};

            // Checked before the rows: the footer link sits below them, but a
            // stale row rect from a previous layout must never win over it.
            if (PtInRect(&m_savedLinkRect, pt)) {
                RevealSavedFile();
                return 0;
            }

            const int b = HitTestButton(pt);
            if (b >= 0) {
                switch (m_buttons[b].id) {
                    case BTN_START: DoStart();     break;
                    case BTN_STOP:  DoStop();      break;
                    case BTN_SAVE:  DoSaveToIni(); break;
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

            FillRect(bb, &rc, Gdi::Brush(bg));
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
            DrawTextW(bb, L"Remote server (TCP/IP) — what other instances connect to", -1, &tr, DT_LEFT | DT_SINGLELINE);

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

                    RECT st{pad, y + hdrH / 4, pad + static_cast<int>(3 * s), y + hdrH * 3 / 4};
                    FillRect(bb, &st, Gdi::Brush(PC::STRIPE));

                    y += hdrH;
                    continue;
                }

                r.rect = {pad, y, W - pad, y + rowH};

                if (static_cast<int>(i) == m_selected || static_cast<int>(i) == m_hotRow)
                    FillRect(bb, &r.rect,
                             Gdi::Brush(static_cast<int>(i) == m_selected ? selBg : hotBg));

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

                // The path the last Save wrote, as a link. Measured off the end
                // of the message above rather than positioned by hand, so the
                // hit box is exactly the text and cannot drift from it when the
                // wording changes.
                m_savedLinkRect = RECT{};
                if (!m_savedPath.empty()) {
                    SIZE pre{};
                    GetTextExtentPoint32W(bb, m_lastResult.c_str(),
                                          static_cast<int>(m_lastResult.size()), &pre);

                    SelectObject(bb, m_hFontLink);
                    SIZE link{};
                    GetTextExtentPoint32W(bb, m_savedPath.c_str(),
                                          static_cast<int>(m_savedPath.size()), &link);

                    const int lx = pad + pre.cx;
                    const int ly = fy + static_cast<int>(20 * s);
                    // Clipped to the panel: a long path must not draw past the
                    // edge, and the hit box must not extend past what is drawn.
                    const int lw = std::min<int>(link.cx, (W - pad) - lx);
                    if (lw > 0) {
                        m_savedLinkRect = {lx, ly, lx + lw, ly + static_cast<int>(18 * s)};
                        SetTextColor(bb, m_savedLinkHot
                                             ? Brighten(Constants::Links::COLOR, 50)
                                             : Constants::Links::COLOR);
                        RECT lr2 = m_savedLinkRect;
                        DrawTextW(bb, m_savedPath.c_str(), -1, &lr2,
                                  DT_LEFT | DT_SINGLELINE | DT_PATH_ELLIPSIS);
                    }
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
