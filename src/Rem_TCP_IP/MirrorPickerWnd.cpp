#include "MirrorPickerWnd.h"
#include "RemoteMirror.h"
#include "RemoteExec.h"   // BuildSyncPayload — the Sync now button

#include "AppState.h"
#include "Platform/Constants.h"
#include "UI/GdiPool.h"   // pooled brushes and pens — never DeleteObject them

#include <algorithm>
#include <windowsx.h>

extern AppState app;

namespace UI {

namespace PC = Constants::Dedicated::PanelColors;

namespace {

    // Narrow enough to sit beside the pictures rather than over them: this panel
    // is meant to be left open while working, unlike the F10 console.
    constexpr int PANEL_W  = 620;
    constexpr int PANEL_H  = 420;
    constexpr int PAD      = 14;
    constexpr int ROW_H    = 32;
    constexpr int HDR_H    = 26;
    constexpr int BTN_H    = 34;
    constexpr int BTN_GAP  = 8;
    constexpr int TITLE_H  = 52;
    constexpr int FOOTER_H = 30;

    // Targets connect and drop while this is open, and the panel is not told —
    // Remote::Mirror has no observer list, deliberately, because every other
    // reader of it polls. So does this.
    constexpr UINT_PTR TIMER_REFRESH    = 1;
    constexpr UINT     REFRESH_PERIOD_MS = 1200;

    enum ButtonId { BTN_ALL = 1, BTN_NONE, BTN_SYNC, BTN_CLOSE };

    bool BgIsDark(COLORREF bg) {
        const int lum = (GetRValue(bg) * 299 + GetGValue(bg) * 587 + GetBValue(bg) * 114) / 1000;
        return lum < 128;
    }

} // namespace

// =============================================================================
// Init / Show / Hide
// =============================================================================
void MirrorPickerWnd::Init(HINSTANCE hInstance, HWND hParent) {
    const float s = app.dpiScale;
    InitFloating(hInstance, hParent, L"qIVMirrorPickerWnd", L"Remotes Control",
                 static_cast<int>(PANEL_W * s), static_cast<int>(PANEL_H * s));
    if (GetHwnd()) {
        SetWindowLongPtrW(GetHwnd(), GWL_EXSTYLE,
                          GetWindowLongPtrW(GetHwnd(), GWL_EXSTYLE) | WS_EX_LAYERED);
        SetLayeredWindowAttributes(GetHwnd(), 0,
                                   Constants::Dedicated::PANEL_OPACITY, LWA_ALPHA);
    }
    Rebuild();
}

void MirrorPickerWnd::Init(HINSTANCE hInstance, HWND hParent, int8_t) {
    Init(hInstance, hParent);
}

void MirrorPickerWnd::Show() {
    Rebuild();
    m_status.clear();
    ShowCenterOverParent();
    // Only while visible: a hidden panel polling a target list nobody is looking
    // at is a timer message per second for nothing.
    if (GetHwnd()) SetTimer(GetHwnd(), TIMER_REFRESH, REFRESH_PERIOD_MS, nullptr);
    Repaint();
}

void MirrorPickerWnd::Hide() {
    if (GetHwnd()) KillTimer(GetHwnd(), TIMER_REFRESH);
    FloatingPanelWnd::Hide();
}

// =============================================================================
// Model
// =============================================================================
void MirrorPickerWnd::Rebuild() {
    // Remember what was selected by ID, not by index: a row that drops while the
    // panel is open renumbers everything below it, and a caret that jumped to a
    // different screen because a third one went offline is exactly the kind of
    // surprise this panel exists to prevent.
    const int selId = (m_selectedRow >= 0 && m_selectedRow < static_cast<int>(m_rows.size()))
                          ? m_rows[m_selectedRow].id
                          : 0;

    m_rows.clear();
    for (const Remote::Mirror::TargetView &v : Remote::Mirror::Targets()) {
        if (!v.connected) continue;   // see the header: idle rows receive nothing
        RowView r;
        r.id          = v.id;
        r.name        = v.name;
        r.host        = v.host;
        r.port        = v.port;
        r.sameMachine = v.sameMachine;
        r.mirroring   = v.mirroring;
        m_rows.push_back(std::move(r));
    }

    m_selectedRow = 0;
    for (size_t i = 0; i < m_rows.size(); ++i)
        if (m_rows[i].id == selId) { m_selectedRow = static_cast<int>(i); break; }

    BuildButtons();
}

void MirrorPickerWnd::BuildButtons() {
    const bool haveRows = !m_rows.empty();
    const bool any = std::any_of(m_rows.begin(), m_rows.end(),
                                 [](const RowView &r) { return r.mirroring; });

    m_buttons.clear();
    m_buttons.push_back({L"All",      BTN_ALL,   {}, haveRows});
    m_buttons.push_back({L"None",     BTN_NONE,  {}, haveRows});
    // Greyed rather than hidden when nothing is ticked: a button that comes and
    // goes moves the one beside it under the cursor between two clicks.
    m_buttons.push_back({L"Sync now", BTN_SYNC,  {}, any});
    m_buttons.push_back({L"Close",    BTN_CLOSE, {}, true});
}

// =============================================================================
// Actions
// =============================================================================
void MirrorPickerWnd::DoToggleRow(int row) {
    if (row < 0 || row >= static_cast<int>(m_rows.size())) return;

    // Takes effect NOW, on the target. There is no OK button because there is
    // nothing to apply — see the header.
    const RowView &r = m_rows[row];
    Remote::Mirror::SetMirroring(r.id, !r.mirroring);

    Rebuild();
    m_status = L"Mirroring to " + Remote::Mirror::SelectionSummary();
    Repaint();
}

void MirrorPickerWnd::DoSetAll(bool on) {
    // Acts on the CONNECTED rows only, matching what this panel shows. A hidden
    // row silently changed by a button the user cannot see it under is the kind
    // of surprise this feature exists to fix.
    for (const RowView &r : m_rows) Remote::Mirror::SetMirroring(r.id, on);
    Rebuild();
    m_status = on ? L"Mirroring to every connected instance"
                  : L"Mirroring to nobody — F11 forwards nothing";
    Repaint();
}

void MirrorPickerWnd::DoSyncSelected() {
    // Mirroring forwards TOGGLES, and a toggle applied to a different starting
    // state diverges. Lining the screens up is the natural first move once you
    // have just said which screens those are.
    //
    // Two spellings, chosen per target: the full one carries the folder and the
    // image path and goes only to instances that share this filesystem; the
    // portable one names the file without a path, because a drive letter from
    // here means nothing there.
    const std::wstring full     = L"sync " + Remote::BuildSyncPayload(true);
    const std::wstring portable = L"sync " + Remote::BuildSyncPayload(false);

    int n = 0;
    for (const RowView &r : m_rows) {
        if (!r.mirroring) continue;
        Remote::Mirror::SendTo(r.id, r.sameMachine ? full : portable);
        ++n;
    }

    m_status = L"Sent folder · image · view to " + std::to_wstring(n) + L" instance(s)";
    Repaint();
}

// =============================================================================
// Hit tests
// =============================================================================
int MirrorPickerWnd::HitTestRow(POINT pt) const {
    for (size_t i = 0; i < m_rows.size(); ++i)
        if (PtInRect(&m_rows[i].rect, pt)) return static_cast<int>(i);
    return -1;
}

int MirrorPickerWnd::HitTestButton(POINT pt) const {
    for (size_t i = 0; i < m_buttons.size(); ++i)
        if (m_buttons[i].enabled && PtInRect(&m_buttons[i].rect, pt))
            return static_cast<int>(i);
    return -1;
}

// =============================================================================
// Keyboard
// =============================================================================
bool MirrorPickerWnd::OnKeyDown(WPARAM vk, bool /*ctrl*/, bool /*shift*/, bool /*alt*/) {
    switch (vk) {
        case VK_UP:
            if (m_selectedRow > 0) { --m_selectedRow; Repaint(); }
            return true;
        case VK_DOWN:
            if (m_selectedRow + 1 < static_cast<int>(m_rows.size())) {
                ++m_selectedRow;
                Repaint();
            }
            return true;
        case VK_SPACE:
        case VK_RETURN:
            DoToggleRow(m_selectedRow);
            return true;
        case 'A': DoSetAll(true);   return true;
        case 'N': DoSetAll(false);  return true;
        case 'S':
            if (std::any_of(m_rows.begin(), m_rows.end(),
                            [](const RowView &r) { return r.mirroring; }))
                DoSyncSelected();
            return true;
        case VK_F5:
            Rebuild();
            Repaint();
            return true;
        default:
            break;
    }
    // Everything else goes to the app pipeline, per FloatingPanelWnd — including
    // F11 itself, so mirroring can be switched on without leaving this panel.
    return false;
}

// =============================================================================
// Message handling
// =============================================================================
LRESULT MirrorPickerWnd::HandlePanelMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_TIMER:
            if (wParam == TIMER_REFRESH) {
                Rebuild();
                Repaint();
            }
            return 0;

        case WM_SETCURSOR: {
            if (LOWORD(lParam) != HTCLIENT) break;
            POINT pt; GetCursorPos(&pt);
            ScreenToClient(GetHwnd(), &pt);
            const bool hot = HitTestButton(pt) >= 0 || HitTestRow(pt) >= 0;
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

        case WM_MOUSELEAVE:
            m_hotRow = -1; m_hotButton = -1;
            Repaint();
            return 0;

        case WM_LBUTTONDOWN: {
            SetFocus(GetHwnd());
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};

            const int b = HitTestButton(pt);
            if (b >= 0) {
                switch (m_buttons[b].id) {
                    case BTN_ALL:   DoSetAll(true);    break;
                    case BTN_NONE:  DoSetAll(false);   break;
                    case BTN_SYNC:  DoSyncSelected();  break;
                    case BTN_CLOSE: Hide();            break;
                    default: break;
                }
                return 0;
            }

            // The WHOLE row toggles, not just the tick box. This panel has
            // exactly one action per row, so there is nothing for a click to be
            // ambiguous between — unlike the F10 console, where three different
            // controls share a line and the row itself only selects.
            const int r = HitTestRow(pt);
            if (r >= 0) {
                m_selectedRow = r;
                DoToggleRow(r);
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
            const int hdrH  = static_cast<int>(HDR_H * s);
            const int btnH  = static_cast<int>(BTN_H * s);

            // ── Title ────────────────────────────────────────────────────────
            SelectObject(bb, m_hFontBold);
            SetTextColor(bb, fg);
            RECT tr{pad, static_cast<int>(6 * s), W - pad, static_cast<int>(26 * s)};
            DrawTextW(bb, L"Remotes Control — which instances F11 drives", -1, &tr,
                      DT_LEFT | DT_SINGLELINE);

            SelectObject(bb, m_hFontSmall);
            SetTextColor(bb, dim);
            {
                RECT sr{pad, tr.bottom, W - pad, tr.bottom + static_cast<int>(16 * s)};
                const std::wstring sub =
                    std::wstring(L"F11 mirror ") + (app.passCommandToRemote ? L"ON" : L"off") +
                    L"   ·   click a row to tick it · A all · N none · S sync · F5 refresh";
                DrawTextW(bb, sub.c_str(), -1, &sr, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            }

            int y = static_cast<int>(TITLE_H * s);

            // ── Buttons ──────────────────────────────────────────────────────
            {
                const int gap = static_cast<int>(BTN_GAP * s);
                const int bw  = static_cast<int>(96 * s);
                int x = pad;
                for (Button &btn : m_buttons) {
                    btn.rect = {x, y, x + bw, y + btnH};
                    const int myIndex = static_cast<int>(&btn - m_buttons.data());

                    COLORREF base = (btn.id == BTN_SYNC) ? PC::BTN_ALT : PC::BTN_MAIN;
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

            // ── Rows ─────────────────────────────────────────────────────────
            const int cNum  = pad + static_cast<int>(6   * s);
            const int cName = pad + static_cast<int>(46  * s);
            const int cHost = pad + static_cast<int>(250 * s);
            // The tick is the column you press, so it goes LAST and is centred —
            // the same layout rule the F10 console's Link button follows.
            const int cMark = pad + static_cast<int>(510 * s);

            SelectObject(bb, m_hFontSmall);
            SetTextColor(bb, PC::HEADER);
            {
                auto hdr = [&](int x, const wchar_t *t) {
                    RECT r{x, y, W - pad, y + hdrH};
                    DrawTextW(bb, t, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                };
                auto hdrC = [&](int cx, const wchar_t *t) {
                    const int half = static_cast<int>(50 * s);
                    RECT r{cx - half, y, cx + half, y + hdrH};
                    DrawTextW(bb, t, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                };
                hdr(cNum, L"#"); hdr(cName, L"Name"); hdr(cHost, L"Address");
                hdrC(cMark, L"Mirror");
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
                        y + static_cast<int>(80 * s)};
                DrawTextW(bb,
                          L"Nothing is connected, so there is nothing to pick between. "
                          L"Open Remote Servers (F10) to add instances and connect them "
                          L"— a row that is only listed receives nothing whatever is "
                          L"ticked here.",
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
                cell(cName, r.name.empty() ? r.host : r.name, PC::TEXT);
                // Marked, because it behaves differently: an instance that does
                // not share this filesystem gets the portable command set and is
                // a parallel viewer rather than a mirror of this screen.
                cell(cHost, r.host + L":" + std::to_wstring(r.port) +
                            (r.sameMachine ? L"" : L"  (remote)"),
                     r.sameMachine ? PC::PATH : PC::CHOICE);

                // ☑ / ☐ — text rather than an owner-drawn control so it follows
                // the theme and needs no assets. Symmetric about cMark, or
                // DT_CENTER would put it off the axis its header sits on.
                r.markRect = {cMark - static_cast<int>(16 * s), y,
                              cMark + static_cast<int>(16 * s), y + rowH};
                {
                    SetTextColor(bb, r.mirroring ? PC::ON : dim);
                    RECT mr = r.markRect;
                    DrawTextW(bb, r.mirroring ? L"☑" : L"☐", -1, &mr,
                              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
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

                // Mirroring on with nothing ticked forwards nothing at all. Said
                // here rather than silently corrected: F11 is the toggle and
                // this panel does not own it, so quietly switching it off from
                // under the user would be a second place deciding that flag.
                const bool any = std::any_of(m_rows.begin(), m_rows.end(),
                                             [](const RowView &v) { return v.mirroring; });
                std::wstring foot = m_status;
                COLORREF     fc   = dim;
                if (app.passCommandToRemote && !m_rows.empty() && !any) {
                    foot = L"F11 is ON but nothing is ticked — no keystroke is being "
                           L"forwarded anywhere.";
                    fc   = PC::WARN;
                } else if (foot.empty()) {
                    foot = L"Ticks take effect immediately. Esc closes; the selection stays.";
                }

                SelectObject(bb, m_hFontSmall);
                SetTextColor(bb, fc);
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

// =============================================================================
// Paint helpers
// =============================================================================
void MirrorPickerWnd::EnsureFonts(HDC dc) {
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

void MirrorPickerWnd::EnsureBackBuffer(HDC refDC, int w, int h) {
    if (m_bbDC && m_bbW == w && m_bbH == h) return;
    DestroyBackBuffer();
    m_bbDC     = CreateCompatibleDC(refDC);
    m_bbBmp    = CreateCompatibleBitmap(refDC, w, h);
    m_bbBmpOld = static_cast<HBITMAP>(SelectObject(m_bbDC, m_bbBmp));
    m_bbW = w; m_bbH = h;
}

void MirrorPickerWnd::DestroyBackBuffer() {
    if (!m_bbDC) return;
    SelectObject(m_bbDC, m_bbBmpOld);
    DeleteObject(m_bbBmp);
    DeleteDC(m_bbDC);
    m_bbDC = nullptr; m_bbBmp = nullptr; m_bbBmpOld = nullptr; m_bbW = m_bbH = 0;
}

void MirrorPickerWnd::Repaint() {
    if (GetHwnd()) InvalidateRect(GetHwnd(), nullptr, FALSE);
}

} // namespace UI
