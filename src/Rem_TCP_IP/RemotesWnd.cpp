#include "RemotesWnd.h"
#include "RemoteMirror.h"
#include "RemotesFile.h"
#include "RemoteExec.h"   // BuildSyncPayload

#include "AppState.h"
#include "Platform/Constants.h"
#include "Platform/ConstantsStrings.h"
#include "UI/ThemedDialog.h"

#include <algorithm>
#include <windowsx.h>

extern AppState app;

namespace UI {

namespace RT = Constants::RemoteTcpIp;
namespace PC = Constants::Dedicated::PanelColors;

namespace {
    constexpr int PANEL_W  = 760;
    constexpr int PANEL_H  = 520;
    constexpr int PAD      = 14;
    constexpr int ROW_H    = 34;
    constexpr int HDR_H    = 26;
    constexpr int BTN_H    = 34;
    constexpr int BTN_GAP  = 8;
    constexpr int TITLE_H  = 44;
    constexpr int FOOTER_H = 30;

    // How long a dot stays amber after a start/stop before the panel re-polls
    // that row. Long enough for a launched instance to bind its port, short
    // enough that the console does not look stuck.
    constexpr UINT_PTR TIMER_PENDING    = 1;
    constexpr UINT     PENDING_DELAY_MS = 2500;

    enum ButtonId { BTN_ADD = 1, BTN_POLL, BTN_SYNC_ALL, BTN_SAVE, BTN_REMOVE };

    bool BgIsDark(COLORREF bg) {
        const int lum = (GetRValue(bg) * 299 + GetGValue(bg) * 587 + GetBValue(bg) * 114) / 1000;
        return lum < 128;
    }

    // Lag reads as a number a human compares at a glance, not a raw count.
    // Everything here is loopback, so sub-millisecond is the normal case and
    // "0 ms" would look like a failed measurement rather than a fast one.
    std::wstring FormatLag(long long us) {
        if (us < 0)    return L"—";
        if (us < 1000) return L"<1 ms";
        wchar_t b[32];
        swprintf_s(b, L"%.1f ms", static_cast<double>(us) / 1000.0);
        return b;
    }
}

// =============================================================================
// Init / Show
// =============================================================================
void RemotesWnd::Init(HINSTANCE hInstance, HWND hParent) {
    const float s = app.dpiScale;
    InitFloating(hInstance, hParent, L"qIVRemotesWnd", L"Remotes",
                 static_cast<int>(PANEL_W * s), static_cast<int>(PANEL_H * s));
    if (GetHwnd()) {
        SetWindowLongPtrW(GetHwnd(), GWL_EXSTYLE,
                          GetWindowLongPtrW(GetHwnd(), GWL_EXSTYLE) | WS_EX_LAYERED);
        SetLayeredWindowAttributes(GetHwnd(), 0,
                                   Constants::Dedicated::PANEL_OPACITY, LWA_ALPHA);
    }
    Rebuild();
}

void RemotesWnd::Init(HINSTANCE hInstance, HWND hParent, int8_t) { Init(hInstance, hParent); }

void RemotesWnd::Show() {
    Rebuild();
    ShowCenterOverParent();
    // Opening the console is the moment you want to know what is up, so poll
    // immediately rather than showing a table of dashes until F5 is pressed.
    DoPollAll();
    Repaint();
}

// =============================================================================
// Startup auto-connect
// =============================================================================
void RemotesWnd::AutoConnectAll(HWND hOwner) {
    Remote::Mirror::SetOwner(hOwner);

    for (const Remote::RemoteEntry &e : Remote::LoadRemotes()) {
        if (!e.autoConnect) continue;
        // Returns immediately — the connection is made on the target's own
        // thread, so a screen that is switched off does not delay startup.
        (void) Remote::Mirror::AddTarget(e.name, e.host, e.port, e.password, e.exePath);
    }
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
        r.id        = t.id;
        r.name      = t.name;
        r.host      = t.host;
        r.port      = t.port;
        r.exePath     = t.exePath;
        r.sameMachine = t.sameMachine;
        r.lagUs       = t.lagUs;
        r.observing = t.observing;
        r.lastError = t.lastError;
        r.dot       = t.connected ? DotState::Up : DotState::Down;

        if (std::find(pending.begin(), pending.end(), r.id) != pending.end() &&
            r.dot == DotState::Down)
            r.dot = DotState::Pending;

        m_rows.push_back(std::move(r));
    }

    m_buttons.clear();
    m_buttons.push_back({L"Add (F9)",   BTN_ADD,      {}, true});
    m_buttons.push_back({L"Poll (F5)",  BTN_POLL,     {}, !m_rows.empty()});
    m_buttons.push_back({L"Sync all",   BTN_SYNC_ALL, {}, !m_rows.empty()});
    m_buttons.push_back({L"Remove",     BTN_REMOVE,   {}, !m_rows.empty()});
    m_buttons.push_back({L"Save",       BTN_SAVE,     {}, true});

    if (m_selected >= static_cast<int>(m_rows.size()))
        m_selected = std::max(0, static_cast<int>(m_rows.size()) - 1);
}

void RemotesWnd::PersistRows() {
    // The password is not in RowView — the panel never displays one, and it has
    // no business holding one. Re-read the file, update what the rows know
    // about, and write it back, so a saved list keeps its credentials.
    std::vector<Remote::RemoteEntry> stored = Remote::LoadRemotes();
    std::vector<Remote::RemoteEntry> out;

    for (const RowView &r : m_rows) {
        Remote::RemoteEntry e;
        e.name    = r.name;
        e.host    = r.host;
        e.port    = r.port;
        e.exePath = r.exePath;

        // Password and autoConnect are PRESERVED from the file, not invented
        // here. The panel never displays a password, and it must not decide on
        // the user's behalf that a remote should be reconnected at startup —
        // that flag is hand-set in qivRemotes.ini by someone who wants a screen
        // wall to come up already joined, and saving the list must not silently
        // turn it on for everyone else.
        for (const Remote::RemoteEntry &s : stored) {
            if (s.host == r.host && s.port == r.port) {
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
void RemotesWnd::DoPollAll() {
    Remote::Mirror::PingAll();
    m_status = L"Polling…";
    // The replies land on the sender threads; the timer brings the answers into
    // view without this function ever waiting for one.
    SetTimer(GetHwnd(), TIMER_PENDING, PENDING_DELAY_MS, nullptr);
    Repaint();
}

void RemotesWnd::DoAddTarget() {
    // Adding happens in F9, not here.
    //
    // F9 already IS the form: address, port and password with a Check
    // Connection button that proves them before anything is stored. Duplicating
    // those three fields in this window would mean two places to type the same
    // thing and two places for them to disagree — and, more to the point, a
    // remote is only worth recording once it has been shown to answer. F9
    // writes the row to qivRemotes.ini on a successful check; this console then
    // owns it from then on.
    DialogMessage(L"Remotes are added from the Remote Control panel (F9).\r\n\r\n"
                  L"Fill in the target address, port and password there and press "
                  L"Check Connection. Once it answers, it is written to\r\n\r\n"
                  L"    " + Remote::RemotesFilePath() + L"\r\n\r\n"
                  L"and appears here from then on.",
                  L"Remotes");
}

void RemotesWnd::DoRemoveTarget(int row) {
    if (row < 0 || row >= static_cast<int>(m_rows.size())) return;
    const RowView &r = m_rows[row];

    if (!DialogConfirm(L"Remove " + r.name + L" (" + r.host + L":" +
                       std::to_wstring(r.port) + L") from the list?\r\n\r\n"
                       L"The instance itself is not affected.",
                       L"Remotes"))
        return;

    Remote::Mirror::RemoveTarget(r.id);
    Rebuild();
    PersistRows();
    Repaint();
}

void RemotesWnd::DoStartTarget(int row) {
    if (row < 0 || row >= static_cast<int>(m_rows.size())) return;
    RowView &r = m_rows[row];

    // CreateProcess starts a process on THIS machine. There is no way to launch
    // one on another box without something already running there to ask, so for
    // a remote row the button has nothing it can do — say that rather than
    // starting a second copy of the viewer here, which is what the naive version
    // would do.
    if (!r.sameMachine) {
        DialogMessage(r.name + L" is on another machine.\r\n\r\n"
                      L"It can only be started at that machine — nothing here can "
                      L"launch a process on it. Stopping it works, because that "
                      L"travels down the connection it already has open.",
                      L"Remotes");
        return;
    }

    if (r.exePath.empty()) {
        DialogMessage(L"No exe recorded for this remote, so there is nothing to "
                      L"launch.\r\n\r\nRemove and re-add it with the exe path to "
                      L"enable the start button.", L"Remotes");
        return;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    // Non-const buffer: CreateProcessW may write to the command line argument.
    std::wstring cmd = L"\"" + r.exePath + L"\"";
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                                   0, nullptr, nullptr, &si, &pi);
    if (!ok) {
        const DWORD err = GetLastError();
        m_status = L"Launch failed (" + std::to_wstring(err) + L") — " + r.exePath;
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
                       L"Remotes"))
        return;

    // The existing HardQuit, down the connection already open. Note this is a
    // command the MIRROR deny-list refuses to fan out — one Ctrl+Q must never
    // take every screen down at once — but a deliberate per-row button is
    // exactly the case that deny-list exists to leave available.
    Remote::Mirror::SendTo(r.id, L"quit");
    r.dot    = DotState::Pending;
    m_status = L"Stopping " + r.name + L"…";
    SetTimer(GetHwnd(), TIMER_PENDING, PENDING_DELAY_MS, nullptr);
    Repaint();
}

void RemotesWnd::DoToggleObserve(int row) {
    if (row < 0 || row >= static_cast<int>(m_rows.size())) return;
    RowView &r = m_rows[row];

    const bool on = !r.observing;
    const std::wstring name = r.name; // Rebuild() invalidates `r`

    // Exclusive: SetObserving clears any other watched row, so the whole list
    // has to be re-read rather than just flipping this one.
    Remote::Mirror::SetObserving(r.id, on);
    Rebuild();

    m_status = on ? (L"Observing " + name + L" — this viewer now follows it")
                  : (L"Stopped observing " + name);
    Repaint();
}

void RemotesWnd::DoSyncAll() {
    // One push of this instance's whole view state. Mirroring forwards toggles,
    // and a toggle applied to a different starting state diverges — this is the
    // cure rather than the prevention, which is the cheaper trade for a set of
    // screens that normally start clean.
    Remote::Mirror::BroadcastSync(L"sync " + Remote::BuildSyncPayload(true),
                                  L"sync " + Remote::BuildSyncPayload(false));
    m_status = L"Pushed view state to " + std::to_wstring(m_rows.size()) + L" remote(s)";
    Repaint();
}

void RemotesWnd::DoSave() {
    PersistRows();
    m_status = L"Saved " + Remote::RemotesFilePath();
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

int RemotesWnd::HitTestEye(POINT pt) const {
    for (size_t i = 0; i < m_rows.size(); ++i)
        if (PtInRect(&m_rows[i].eyeRect, pt)) return static_cast<int>(i);
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
    switch (vk) {
        case VK_UP:
            if (m_selected > 0) { --m_selected; Repaint(); }
            return true;
        case VK_DOWN:
            if (m_selected + 1 < static_cast<int>(m_rows.size())) { ++m_selected; Repaint(); }
            return true;
        case VK_F5:
            DoPollAll();
            return true;
        case VK_DELETE:
            DoRemoveTarget(m_selected);
            return true;
        case VK_RETURN:
            // Enter on a row does the dot's job: bring it up, or take it down.
            if (m_selected < static_cast<int>(m_rows.size())) {
                if (m_rows[m_selected].dot == DotState::Up) DoStopTarget(m_selected);
                else                                        DoStartTarget(m_selected);
            }
            return true;
        default:
            break;
    }
    return false; // unhandled keys go to the app pipeline, per FloatingPanelWnd
}

bool RemotesWnd::OnLocalHide() { return false; } // nothing to clear — Esc closes

// =============================================================================
// Message handling
// =============================================================================
LRESULT RemotesWnd::HandlePanelMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_TIMER:
            if (wParam == TIMER_PENDING) {
                KillTimer(GetHwnd(), TIMER_PENDING);
                // Whatever was launched or stopped has had its moment; ask the
                // sender threads what actually happened.
                Remote::Mirror::PingAll();
                Rebuild();
                m_status.clear();
                Repaint();
            }
            return 0;

        case WM_SETCURSOR: {
            if (LOWORD(lParam) != HTCLIENT) break;
            POINT pt; GetCursorPos(&pt);
            ScreenToClient(GetHwnd(), &pt);
            const bool hot = HitTestButton(pt) >= 0 || HitTestDot(pt) >= 0 ||
                             HitTestEye(pt) >= 0 || HitTestRow(pt) >= 0;
            SetCursor(hot ? Constants::Cursors::CURR_CLICK
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
                    case BTN_ADD:      DoAddTarget();               break;
                    case BTN_POLL:     DoPollAll();                 break;
                    case BTN_SYNC_ALL: DoSyncAll();                 break;
                    case BTN_REMOVE:   DoRemoveTarget(m_selected);  break;
                    case BTN_SAVE:     DoSave();                    break;
                    default: break;
                }
                return 0;
            }

            // The dot and the eye are checked before the row, so clicking either
            // performs its action rather than merely selecting the line.
            const int dot = HitTestDot(pt);
            if (dot >= 0) {
                m_selected = dot;
                if (m_rows[dot].dot == DotState::Up) DoStopTarget(dot);
                else                                 DoStartTarget(dot);
                return 0;
            }

            const int eye = HitTestEye(pt);
            if (eye >= 0) {
                m_selected = eye;
                DoToggleObserve(eye);
                return 0;
            }

            const int row = HitTestRow(pt);
            if (row >= 0) { m_selected = row; Repaint(); }
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

            const float s   = app.dpiScale;
            const int pad   = static_cast<int>(PAD * s);
            const int rowH  = static_cast<int>(ROW_H * s);
            const int hdrH  = static_cast<int>(HDR_H * s);
            const int btnH  = static_cast<int>(BTN_H * s);

            // Column x-offsets, laid out once so header and rows cannot drift.
            const int cNum  = pad + static_cast<int>(6  * s);
            const int cName = pad + static_cast<int>(46 * s);
            const int cHost = pad + static_cast<int>(220 * s);
            const int cPort = pad + static_cast<int>(360 * s);
            const int cLag  = pad + static_cast<int>(430 * s);
            const int cDot  = pad + static_cast<int>(540 * s);
            const int cEye  = pad + static_cast<int>(590 * s);

            // ── Title ────────────────────────────────────────────────────────
            SelectObject(bb, m_hFontBold);
            SetTextColor(bb, fg);
            RECT tr{pad, static_cast<int>(6 * s), W - pad, static_cast<int>(26 * s)};
            DrawTextW(bb, L"Remotes — instances this copy drives", -1, &tr,
                      DT_LEFT | DT_SINGLELINE);

            SelectObject(bb, m_hFontSmall);
            SetTextColor(bb, dim);
            RECT sr{pad, tr.bottom, W - pad, tr.bottom + static_cast<int>(16 * s)};
            {
                const std::wstring sub =
                    std::wstring(L"F11 mirror ") + (app.passCommandToRemote ? L"ON" : L"off") +
                    L"   ·   F12 execute here " + (app.resendCommandToCaller ? L"ON" : L"off") +
                    L"   ·   dot = start/stop, eye = observe, F5 = poll";
                DrawTextW(bb, sub.c_str(), -1, &sr, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            }

            // ── Buttons ──────────────────────────────────────────────────────
            {
                const int gap   = static_cast<int>(BTN_GAP * s);
                const int count = static_cast<int>(m_buttons.size());
                const int total = W - pad * 2 - gap * (count - 1);
                const int bw    = total / count;
                int x = pad;
                const int y = static_cast<int>(TITLE_H * s);

                SelectObject(bb, m_hFontBody);
                for (Button &btn : m_buttons) {
                    btn.rect = {x, y, x + bw, y + btnH};
                    const int myIndex = static_cast<int>(&btn - m_buttons.data());
                    COLORREF base = PC::BTN_MAIN;
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

            int y = static_cast<int>(TITLE_H * s) + btnH + static_cast<int>(12 * s);

            // ── Column header ────────────────────────────────────────────────
            SelectObject(bb, m_hFontSmall);
            SetTextColor(bb, PC::HEADER);
            {
                auto hdr = [&](int x, const wchar_t *t) {
                    RECT r{x, y, W - pad, y + hdrH};
                    DrawTextW(bb, t, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                };
                hdr(cNum, L"#"); hdr(cName, L"Name"); hdr(cHost, L"Address");
                hdr(cPort, L"Port"); hdr(cLag, L"Lag"); hdr(cDot, L"Up");
                hdr(cEye, L"Watch");
            }
            y += hdrH;

            HPEN hp = CreatePen(PS_SOLID, 1, line);
            HGDIOBJ ohp = SelectObject(bb, hp);
            MoveToEx(bb, pad, y, nullptr);
            LineTo(bb, W - pad, y);
            SelectObject(bb, ohp); DeleteObject(hp);

            // ── Rows ─────────────────────────────────────────────────────────
            if (m_rows.empty()) {
                SelectObject(bb, m_hFontBody);
                SetTextColor(bb, dim);
                RECT er{pad, y + static_cast<int>(20 * s), W - pad,
                        y + static_cast<int>(80 * s)};
                DrawTextW(bb,
                          L"No remotes yet.\r\n\r\n"
                          L"Press Add, give it the address and port the other instance "
                          L"listens on, and it is remembered from then on.",
                          -1, &er, DT_LEFT | DT_WORDBREAK);
            }

            for (size_t i = 0; i < m_rows.size(); ++i) {
                RowView &r = m_rows[i];
                r.rect = {pad, y, W - pad, y + rowH};

                if (static_cast<int>(i) == m_selected || static_cast<int>(i) == m_hotRow) {
                    HBRUSH hb = CreateSolidBrush(
                        static_cast<int>(i) == m_selected ? selBg : hotBg);
                    FillRect(bb, &r.rect, hb);
                    DeleteObject(hb);
                }

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
                // and it will show a different picture than this screen. Seeing
                // that in the list beats discovering it from behaviour.
                cell(cHost, r.sameMachine ? r.host : (r.host + L"  (remote)"),
                     r.sameMachine ? PC::PATH : PC::CHOICE);
                cell(cPort, std::to_wstring(r.port),PC::NUMBER);
                cell(cLag,  FormatLag(r.lagUs),
                     r.dot == DotState::Up ? PC::NUMBER : dim);

                // ● — the state and the button in one. Amber means an action is
                // in flight, so a click is acknowledged before the re-poll
                // confirms it.
                const int dotR = static_cast<int>(6 * s);
                const int dotCY = y + rowH / 2;
                r.dotRect = {cDot - dotR * 2, dotCY - dotR * 2,
                             cDot + dotR * 2, dotCY + dotR * 2};
                {
                    const COLORREF dc2 = (r.dot == DotState::Up)      ? PC::ON
                                       : (r.dot == DotState::Pending) ? PC::WARN
                                                                      : PC::OFF;
                    HBRUSH db = CreateSolidBrush(dc2);
                    HPEN   dp = CreatePen(PS_SOLID, 1, line);
                    HGDIOBJ ob2 = SelectObject(bb, db);
                    HGDIOBJ op2 = SelectObject(bb, dp);
                    Ellipse(bb, cDot - dotR, dotCY - dotR, cDot + dotR, dotCY + dotR);
                    SelectObject(bb, ob2); SelectObject(bb, op2);
                    DeleteObject(db); DeleteObject(dp);
                }

                // 👁 — drawn as text so it follows the theme and needs no assets.
                r.eyeRect = {cEye - static_cast<int>(12 * s), y,
                             cEye + static_cast<int>(20 * s), y + rowH};
                {
                    SetTextColor(bb, r.observing ? PC::ON : dim);
                    RECT er2 = r.eyeRect;
                    DrawTextW(bb, r.observing ? L"◉" : L"○", -1, &er2,
                              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                }

                y += rowH;
            }

            // ── Footer ───────────────────────────────────────────────────────
            {
                const int fy = H - static_cast<int>(FOOTER_H * s);
                HPEN pen = CreatePen(PS_SOLID, 1, line);
                HGDIOBJ op = SelectObject(bb, pen);
                MoveToEx(bb, pad, fy - static_cast<int>(4 * s), nullptr);
                LineTo(bb, W - pad, fy - static_cast<int>(4 * s));
                SelectObject(bb, op); DeleteObject(pen);

                // A row that is down usually knows WHY, and "could not connect"
                // beside a red dot saves a round of guessing.
                std::wstring foot = m_status;
                if (foot.empty() && m_selected < static_cast<int>(m_rows.size()))
                    foot = m_rows[m_selected].lastError;

                SelectObject(bb, m_hFontSmall);
                SetTextColor(bb, dim);
                RECT frc{pad, fy, W - pad, fy + static_cast<int>(20 * s)};
                DrawTextW(bb, foot.c_str(), -1, &frc,
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
