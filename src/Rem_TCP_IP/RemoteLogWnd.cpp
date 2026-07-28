#include "RemoteLogWnd.h"
#include "RemoteMirror.h"   // BroadcastEnableLog — the switch travels

#include "AppState.h"
#include "Platform/Constants.h"
#include "Input/Command.h"       // InputManager::ExecuteCommand — the one sink
#include "UI/GdiPool.h"          // pooled brushes and pens — never DeleteObject them

#include <algorithm>
#include <commdlg.h>
#include <windowsx.h>

extern AppState app;

namespace UI {

namespace PC = Constants::Dedicated::PanelColors;
namespace RL = Remote::Log;

namespace {

    // Wide: seven columns, and the two that matter most (Command and Response)
    // are the two that need the room. Still narrower than the content, which is
    // why this panel has a horizontal scrollbar and the others do not.
    // Wide enough that the seven columns (970 design units) plus the drawn
    // vertical bar and both pads fit without horizontal scrolling at 100%.
    constexpr int PANEL_W  = 1030;
    constexpr int PANEL_H  = 560;
    constexpr int PAD      = 14;
    constexpr int ROW_H    = 22;
    constexpr int HDR_H    = 26;
    constexpr int BTN_H    = 32;
    constexpr int BTN_GAP  = 8;
    constexpr int TITLE_H  = 50;
    constexpr int FOOTER_H = 28;

    // No refresh timer, deliberately — unlike the other Rem_TCP_IP panels. Those
    // poll because the thing they display (a target's live status) has no moment
    // of change to hook; this one is a list that only ever grows when something
    // adds to it, and the adder can say so. RemoteLog posts
    // WM_QIV_REMOTE_LOG_ADDED, coalesced at the source.
    enum ButtonId { BTN_RECORD = 1, BTN_CLEAR, BTN_SAVE, BTN_LOAD };

    // RECT members are LONG, and every size here is compared against an int
    // expression — std::max(0, rc.bottom - rc.top) will not deduce a common type
    // and fails to compile. Narrowed once, here, rather than with a cast at each
    // of the six call sites.
    int RectW(const RECT &r) { return static_cast<int>(r.right - r.left); }
    int RectH(const RECT &r) { return static_cast<int>(r.bottom - r.top); }

    bool BgIsDark(COLORREF bg) {
        const int lum = (GetRValue(bg) * 299 + GetGValue(bg) * 587 + GetBValue(bg) * 114) / 1000;
        return lum < 128;
    }

    constexpr const wchar_t *FILE_FILTER =
        L"qIV remote log (*.log;*.txt)\0*.log;*.txt\0All files (*.*)\0*.*\0\0";

} // namespace

// =============================================================================
// Init / Show / Hide
// =============================================================================
void RemoteLogWnd::Init(HINSTANCE hInstance, HWND hParent) {
    const float s = app.dpiScale;
    InitFloating(hInstance, hParent, L"qIVRemoteLogWnd", L"RemoteLog",
                 static_cast<int>(PANEL_W * s), static_cast<int>(PANEL_H * s));
    if (!GetHwnd()) return;

    SetWindowLongPtrW(GetHwnd(), GWL_EXSTYLE,
                      GetWindowLongPtrW(GetHwnd(), GWL_EXSTYLE) | WS_EX_LAYERED);
    SetLayeredWindowAttributes(GetHwnd(), 0,
                               Constants::Dedicated::PANEL_OPACITY, LWA_ALPHA);

    // Widths in design units; scaled at paint time. They SUM TO LESS THAN the
    // default client width minus the vertical bar, so all seven columns —
    // including Δ, the rightmost — are visible without touching the horizontal
    // scrollbar. A column you have to discover by scrolling is a column most
    // people never find. The bar is still there for when the panel is resized
    // narrow or the DPI rounds against us.
    m_columns = {
        { L"#",        76,  true,  SortKey::Seq   },
        { L"Sender",   120, false, SortKey::Seq   },
        { L"Command",  210, false, SortKey::Seq   },
        { L"Receiver", 120, false, SortKey::Seq   },
        { L"Response", 240, false, SortKey::Seq   },
        { L"Time",     110, true,  SortKey::Time  },
        // Spelt out rather than a bare "Δ": the round trip is the number this
        // panel exists to show, and a single Greek letter at the far right of
        // seven columns does not say so.
        { L"Δ time",   94,  true,  SortKey::Delta },
    };

    Rebuild();
}

void RemoteLogWnd::Init(HINSTANCE hInstance, HWND hParent, int8_t) {
    Init(hInstance, hParent);
}

void RemoteLogWnd::Show() {
    Rebuild();
    m_status.clear();
    ShowCenterOverParent();
    // Subscribe. Everything that lands from here on says so; nothing is polled.
    Remote::Log::SetNotifyWindow(GetHwnd());
    ClampScroll();
    Repaint();
}

void RemoteLogWnd::Hide() {
    // Unsubscribe FIRST, so a sender thread cannot post to a window that is on
    // its way out. A closed panel costs the producers one atomic load.
    Remote::Log::SetNotifyWindow(nullptr);
    FloatingPanelWnd::Hide();
}

// =============================================================================
// Model
// =============================================================================
void RemoteLogWnd::Rebuild() {
    m_rows = RL::Snapshot();
    ApplySort();
    BuildButtons();

    // Tail-following, but only while the view is already at the bottom — see
    // the header. Set before the clamp so the clamp confirms it rather than
    // fighting it.
    if (m_followTail) m_scrollY = std::max(0, ContentHeightPx() - ViewHeightPx());
    ClampScroll();
}

void RemoteLogWnd::ApplySort() {
    // std::stable_sort, not sort: two entries can share a timestamp (the clock
    // is milliseconds, a loopback exchange is faster than that), and an unstable
    // sort would shuffle them differently on every refresh — a list that flickers
    // while nothing is happening.
    const bool asc = m_sortAscending;
    switch (m_sortKey) {
        case SortKey::Seq:
            std::stable_sort(m_rows.begin(), m_rows.end(),
                             [asc](const RL::Entry &a, const RL::Entry &b) {
                                 return asc ? a.seq < b.seq : b.seq < a.seq;
                             });
            break;
        case SortKey::Time:
            std::stable_sort(m_rows.begin(), m_rows.end(),
                             [asc](const RL::Entry &a, const RL::Entry &b) {
                                 return asc ? a.whenFt < b.whenFt : b.whenFt < a.whenFt;
                             });
            break;
        case SortKey::Delta:
            std::stable_sort(m_rows.begin(), m_rows.end(),
                             [asc](const RL::Entry &a, const RL::Entry &b) {
                                 return asc ? a.deltaUs < b.deltaUs : b.deltaUs < a.deltaUs;
                             });
            break;
    }
}

void RemoteLogWnd::BuildButtons() {
    const bool any = !m_rows.empty();
    m_buttons.clear();
    m_buttons.push_back({app.remoteLogEnabled ? L"Recording: ON" : L"Recording: OFF",
                         BTN_RECORD, {}, true});
    m_buttons.push_back({L"Clear",  BTN_CLEAR, {}, any});
    m_buttons.push_back({L"Save…",  BTN_SAVE,  {}, any});
    m_buttons.push_back({L"Load…",  BTN_LOAD,  {}, true});
}

// =============================================================================
// Actions
// =============================================================================
void RemoteLogWnd::DoToggleLogging() {
    // Through ExecuteCommand rather than flipping the flag here. That is the one
    // sink every input path funnels into, so the switch behaves identically
    // whether it came from this button, from a script, or from another instance
    // — and the fan-out to the targets lives in one place instead of two.
    InputManager::ExecuteCommand(m_hParent, Command::EnableRemoteLog);

    m_status = app.remoteLogEnabled
                   ? L"Recording — and the connected instances were told to record too"
                   : L"Stopped — the connected instances were told to stop too";
    Rebuild();
    Repaint();
}

void RemoteLogWnd::DoClear() {
    RL::Clear();
    m_scrollY   = 0;
    m_followTail = true;
    m_status    = L"Cleared. Entry numbers carry on from where they were — a "
                  L"number never means two different exchanges.";
    Rebuild();
    Repaint();
}

void RemoteLogWnd::DoSave() {
    wchar_t path[MAX_PATH] = L"qivRemoteLog.log";

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = GetHwnd();
    ofn.lpstrFilter = FILE_FILTER;
    ofn.lpstrFile   = path;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrTitle  = L"Save remote log";
    ofn.lpstrDefExt = L"log";
    ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    // The panel is WS_EX_TOPMOST and the common dialog is not, so the dialog
    // would open BEHIND it. Dropped for the duration and restored after.
    SetWindowPos(GetHwnd(), HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    const BOOL picked = GetSaveFileNameW(&ofn);
    SetWindowPos(GetHwnd(), HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    if (!picked) return;

    std::wstring err;
    if (RL::SaveTo(path, err)) {
        m_status = L"Saved " + std::to_wstring(m_rows.size()) + L" entries to " + path;
    } else {
        m_status = err;
        MessageBoxW(GetHwnd(), err.c_str(), L"RemoteLog", MB_OK | MB_ICONWARNING);
    }
    Repaint();
}

void RemoteLogWnd::DoLoad() {
    wchar_t path[MAX_PATH] = L"";

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = GetHwnd();
    ofn.lpstrFilter = FILE_FILTER;
    ofn.lpstrFile   = path;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrTitle  = L"Open a saved remote log";
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    SetWindowPos(GetHwnd(), HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    const BOOL picked = GetOpenFileNameW(&ofn);
    SetWindowPos(GetHwnd(), HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    if (!picked) return;

    // Loading REPLACES what is in memory (RemoteLog.h says why), so a live
    // recording is about to be thrown away. Asked, not assumed.
    if (RL::Count() > 0 &&
        MessageBoxW(GetHwnd(),
                    L"Loading a saved log REPLACES what is recorded now.\r\n\r\n"
                    L"Save the current one first if you still need it. Continue?",
                    L"RemoteLog", MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;

    std::wstring err;
    if (RL::LoadFrom(path, err)) {
        m_followTail = false;   // a loaded log is read from the top, not tailed
        m_scrollY    = 0;
        m_status     = std::wstring(L"Loaded ") + path;
        Rebuild();
    } else {
        m_status = err;
        MessageBoxW(GetHwnd(), err.c_str(), L"RemoteLog", MB_OK | MB_ICONWARNING);
    }
    Repaint();
}

void RemoteLogWnd::DoSort(SortKey key) {
    // Same column again reverses; a different column starts ascending. Starting
    // a NEW column in the previous column's direction is the behaviour that
    // makes a table feel like it ignored the click.
    if (m_sortKey == key) m_sortAscending = !m_sortAscending;
    else { m_sortKey = key; m_sortAscending = true; }

    // Any explicit sort ends tail-following: you sorted to look at something,
    // and being scrolled away from it a second later is the opposite of that.
    m_followTail = false;

    ApplySort();
    Repaint();
}

// =============================================================================
// Geometry / scrolling
// =============================================================================
int RemoteLogWnd::RowHeightPx() const {
    return static_cast<int>(ROW_H * app.dpiScale);
}

int RemoteLogWnd::HeaderTopPx() const {
    const float s = app.dpiScale;
    return static_cast<int>(TITLE_H * s) + static_cast<int>(BTN_H * s) +
           static_cast<int>(10 * s);
}

int RemoteLogWnd::ListTopPx() const {
    return HeaderTopPx() + static_cast<int>(HDR_H * app.dpiScale);
}

int RemoteLogWnd::ContentWidthPx() const {
    const float s = app.dpiScale;
    int w = 0;
    for (const Column &c : m_columns) w += static_cast<int>(c.width * s);
    return w;
}

int RemoteLogWnd::ContentHeightPx() const {
    return static_cast<int>(m_rows.size()) * RowHeightPx();
}

// The list viewport, with BOTH drawn bars carved out. Each bar is only reserved
// when its axis actually overflows — reserving unconditionally would leave a
// dead gutter on a panel with four entries in it.
int RemoteLogWnd::ViewWidthPx() const {
    if (!GetHwnd()) return 0;
    RECT rc{};
    GetClientRect(GetHwnd(), &rc);
    const float s = app.dpiScale;
    const int pad = static_cast<int>(PAD * s);
    const int sbW = static_cast<int>(Constants::Dedicated::PANEL_SCROLLBAR_W * s);

    int w = RectW(rc) - pad * 2;
    if (ContentHeightPx() > RectH(rc) - ListTopPx() - static_cast<int>(FOOTER_H * s))
        w -= sbW;
    return std::max(0, w);
}

int RemoteLogWnd::ViewHeightPx() const {
    if (!GetHwnd()) return 0;
    RECT rc{};
    GetClientRect(GetHwnd(), &rc);
    const float s = app.dpiScale;
    const int pad = static_cast<int>(PAD * s);
    const int sbW = static_cast<int>(Constants::Dedicated::PANEL_SCROLLBAR_W * s);

    int h = RectH(rc) - ListTopPx() - static_cast<int>(FOOTER_H * s);
    if (ContentWidthPx() > RectW(rc) - pad * 2) h -= sbW;
    return std::max(0, h);
}

void RemoteLogWnd::ClampScroll() {
    m_scrollX = std::clamp(m_scrollX, 0, std::max(0, ContentWidthPx()  - ViewWidthPx()));
    m_scrollY = std::clamp(m_scrollY, 0, std::max(0, ContentHeightPx() - ViewHeightPx()));
}

void RemoteLogWnd::ScrollTo(int x, int y) {
    if (!GetHwnd()) return;

    const int maxY = std::max(0, ContentHeightPx() - ViewHeightPx());
    const int maxX = std::max(0, ContentWidthPx()  - ViewWidthPx());

    m_scrollX = std::clamp(x, 0, maxX);
    m_scrollY = std::clamp(y, 0, maxY);

    // Re-arm tail-following the moment the view is back at the bottom, so
    // scrolling down to catch up resumes it without a second gesture.
    m_followTail = (m_scrollY >= maxY);

    Repaint();
}

// =============================================================================
// Hit tests
// =============================================================================
int RemoteLogWnd::HitTestButton(POINT pt) const {
    for (size_t i = 0; i < m_buttons.size(); ++i)
        if (m_buttons[i].enabled && PtInRect(&m_buttons[i].rect, pt))
            return static_cast<int>(i);
    return -1;
}

int RemoteLogWnd::HitTestHeader(POINT pt) const {
    for (size_t i = 0; i < m_columns.size(); ++i)
        if (m_columns[i].sortable && PtInRect(&m_columns[i].headerRect, pt))
            return static_cast<int>(i);
    return -1;
}

// =============================================================================
// Keyboard
// =============================================================================
bool RemoteLogWnd::OnKeyDown(WPARAM vk, bool ctrl, bool /*shift*/, bool /*alt*/) {
    const int row  = RowHeightPx();
    const int page = std::max(row, ViewHeightPx());

    switch (vk) {
        case VK_UP:    ScrollTo(m_scrollX, m_scrollY - row);  return true;
        case VK_DOWN:  ScrollTo(m_scrollX, m_scrollY + row);  return true;
        case VK_PRIOR: ScrollTo(m_scrollX, m_scrollY - page); return true;
        case VK_NEXT:  ScrollTo(m_scrollX, m_scrollY + page); return true;
        case VK_LEFT:  ScrollTo(m_scrollX - row * 2, m_scrollY); return true;
        case VK_RIGHT: ScrollTo(m_scrollX + row * 2, m_scrollY); return true;
        case VK_HOME:  ScrollTo(0, 0); return true;
        case VK_END:   ScrollTo(m_scrollX, ContentHeightPx()); return true;
        case VK_F5:    Rebuild(); Repaint(); return true;
        case 'S': if (ctrl) { DoSave(); return true; } break;
        case 'O': if (ctrl) { DoLoad(); return true; } break;
        default: break;
    }
    // Everything else to the app pipeline — including Ctrl+F12, so the same key
    // that opened this closes it.
    return false;
}

// =============================================================================
// Message handling
// =============================================================================
LRESULT RemoteLogWnd::HandlePanelMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        // One or more entries landed. No payload and no count: by the time this
        // runs there may be more, so the answer is always "re-snapshot".
        case Constants::WM_QIV_REMOTE_LOG_ADDED:
            // BEFORE the snapshot, not after. Clearing afterwards would drop an
            // entry that arrived during Rebuild — it would find the gate still
            // closed, skip its post, and the panel would sit stale until the
            // next unrelated exchange. Clearing first costs at worst one
            // redundant rebuild, which is the right way round to be wrong.
            RL::ClearNotifyPending();
            Rebuild();
            Repaint();
            return 0;

        // Belt and braces alongside Hide(): a panel can also go away without
        // being hidden first (process teardown), and a registered HWND that no
        // longer exists would have every producer post, fail, and reset the gate
        // for the rest of the session.
        case WM_DESTROY:
            Remote::Log::SetNotifyWindow(nullptr);
            break;

        case WM_SIZE:
            ClampScroll();
            Repaint();
            return 0;

        case WM_MOUSEWHEEL: {
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            UINT lines = 3;
            SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
            if (lines == 0) lines = 3;
            ScrollTo(m_scrollX,
                     m_scrollY - (delta / WHEEL_DELTA) * static_cast<int>(lines) * RowHeightPx());
            return 0;
        }

        // Shift+wheel scrolls sideways on every list Windows ships; a table this
        // wide is exactly where that reflex gets used.
        case WM_MOUSEHWHEEL: {
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            ScrollTo(m_scrollX + (delta / WHEEL_DELTA) * RowHeightPx() * 3, m_scrollY);
            return 0;
        }

        case WM_SETCURSOR: {
            if (LOWORD(lParam) != HTCLIENT) break;
            POINT pt; GetCursorPos(&pt);
            ScreenToClient(GetHwnd(), &pt);
            const bool hot = HitTestButton(pt) >= 0 || HitTestHeader(pt) >= 0;
            SetCursor(hot ? Constants::Cursors::CURR_CLICK
                          : Constants::Cursors::CURR_DEFAULT);
            return TRUE;
        }

        case WM_MOUSEMOVE: {
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};

            // A drag in progress owns the mouse: the pointer leaves the thumb
            // constantly while dragging, and letting the hover logic run would
            // fight it.
            if (m_drag != Drag::None) {
                if (m_drag == Drag::Vert) {
                    const int trackH = RectH(m_vTrack);
                    const int travel = std::max(1, trackH - m_dragThumbSpan);
                    const int wanted = pt.y - m_vTrack.top - m_dragGrabPx;
                    ScrollTo(m_scrollX,
                             MulDiv(std::clamp(wanted, 0, travel),
                                    std::max(0, ContentHeightPx() - ViewHeightPx()), travel));
                } else {
                    const int trackW = RectW(m_hTrack);
                    const int travel = std::max(1, trackW - m_dragThumbSpan);
                    const int wanted = pt.x - m_hTrack.left - m_dragGrabPx;
                    ScrollTo(MulDiv(std::clamp(wanted, 0, travel),
                                    std::max(0, ContentWidthPx() - ViewWidthPx()), travel),
                             m_scrollY);
                }
                return 0;
            }

            const int b = HitTestButton(pt);
            const int h = HitTestHeader(pt);
            const bool vHot = PtInRect(&m_vThumb, pt) != 0;
            const bool hHot = PtInRect(&m_hThumb, pt) != 0;
            if (b != m_hotButton || h != m_hotHeader ||
                vHot != m_vThumbHot || hHot != m_hThumbHot) {
                m_hotButton = b; m_hotHeader = h;
                m_vThumbHot = vHot; m_hThumbHot = hHot;
                Repaint();
            }
            return 0;
        }

        case WM_LBUTTONUP:
            if (m_drag != Drag::None) {
                m_drag = Drag::None;
                ReleaseCapture();
                Repaint();
            }
            return 0;

        // The drag can be cancelled by something other than the button coming
        // up — an Alt+Tab, a message box. Without this the panel would think it
        // was still dragging and every mouse-move would scroll.
        case WM_CAPTURECHANGED:
            m_drag = Drag::None;
            Repaint();
            return 0;

        case WM_LBUTTONDOWN: {
            SetFocus(GetHwnd());
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};

            const int b = HitTestButton(pt);
            if (b >= 0) {
                switch (m_buttons[b].id) {
                    case BTN_RECORD: DoToggleLogging(); break;
                    case BTN_CLEAR:  DoClear();         break;
                    case BTN_SAVE:   DoSave();          break;
                    case BTN_LOAD:   DoLoad();          break;
                    default: break;
                }
                return 0;
            }

            const int h = HitTestHeader(pt);
            if (h >= 0) { DoSort(m_columns[h].key); return 0; }

            // --- The drawn scrollbars ---------------------------------------
            // Thumb → drag. Track above/below the thumb → page, the same as a
            // native bar, so the muscle memory carries over.
            if (PtInRect(&m_vThumb, pt)) {
                m_drag           = Drag::Vert;
                m_dragGrabPx     = pt.y - m_vThumb.top;
                m_dragThumbSpan  = RectH(m_vThumb);
                SetCapture(GetHwnd());
                return 0;
            }
            if (PtInRect(&m_vTrack, pt)) {
                ScrollTo(m_scrollX,
                         m_scrollY + (pt.y < m_vThumb.top ? -ViewHeightPx() : ViewHeightPx()));
                return 0;
            }
            if (PtInRect(&m_hThumb, pt)) {
                m_drag          = Drag::Horz;
                m_dragGrabPx    = pt.x - m_hThumb.left;
                m_dragThumbSpan = RectW(m_hThumb);
                SetCapture(GetHwnd());
                return 0;
            }
            if (PtInRect(&m_hTrack, pt)) {
                ScrollTo(m_scrollX + (pt.x < m_hThumb.left ? -ViewWidthPx() : ViewWidthPx()),
                         m_scrollY);
                return 0;
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
            const COLORREF altBg = dark ? RGB(42,42,46)    : RGB(244,244,246);
            const COLORREF line  = dark ? RGB(64,64,64)    : RGB(220,220,220);

            FillRect(bb, &rc, Gdi::Brush(bg));
            SetBkMode(bb, TRANSPARENT);

            const float s   = app.dpiScale;
            const int pad   = static_cast<int>(PAD * s);
            const int rowH  = RowHeightPx();
            const int hdrH  = static_cast<int>(HDR_H * s);
            const int btnH  = static_cast<int>(BTN_H * s);

            // ── Title ────────────────────────────────────────────────────────
            SelectObject(bb, m_hFontBold);
            SetTextColor(bb, fg);
            RECT tr{pad, static_cast<int>(6 * s), W - pad, static_cast<int>(26 * s)};
            DrawTextW(bb, L"RemoteLog — what crossed the wire", -1, &tr,
                      DT_LEFT | DT_SINGLELINE);

            SelectObject(bb, m_hFontSmall);
            SetTextColor(bb, dim);
            {
                RECT sr{pad, tr.bottom, W - pad, tr.bottom + static_cast<int>(16 * s)};
                const std::wstring sub =
                    std::wstring(L"→ sent by this instance · ← received   ·   ") +
                    std::to_wstring(m_rows.size()) + L" of " +
                    std::to_wstring(RL::CAPACITY) + L" entries   ·   " +
                    L"click #, Time or Δ time to sort   ·   F5 refreshes";
                DrawTextW(bb, sub.c_str(), -1, &sr, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            }

            // ── Buttons ──────────────────────────────────────────────────────
            {
                const int gap = static_cast<int>(BTN_GAP * s);
                int x = pad;
                int y = static_cast<int>(TITLE_H * s);
                for (Button &btn : m_buttons) {
                    // Recording is wider: its label carries the state, so it
                    // changes width, and a button that resizes as you press it
                    // moves the one beside it under the cursor.
                    const int bw = static_cast<int>((btn.id == BTN_RECORD ? 150 : 92) * s);
                    btn.rect = {x, y, x + bw, y + btnH};
                    const int myIndex = static_cast<int>(&btn - m_buttons.data());

                    // The recording button is the affirmative action here and is
                    // the only one that carries state, so it takes the accent —
                    // and only while it is actually ON.
                    COLORREF base = (btn.id == BTN_RECORD && app.remoteLogEnabled)
                                        ? PC::BTN_ALT : PC::BTN_MAIN;
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

            // ── Column header ────────────────────────────────────────────────
            // Scrolls HORIZONTALLY with the columns and is fixed vertically —
            // the whole reason a table has a header row.
            const int hdrY = HeaderTopPx();
            {
                SelectObject(bb, m_hFontSmall);
                int x = pad - m_scrollX;
                for (Column &c : m_columns) {
                    const int cw = static_cast<int>(c.width * s);
                    c.headerRect = {x, hdrY, x + cw, hdrY + hdrH};

                    const int myIndex = static_cast<int>(&c - m_columns.data());
                    if (c.sortable && myIndex == m_hotHeader)
                        FillRect(bb, &c.headerRect,
                                 Gdi::Brush(dark ? RGB(56,56,60) : RGB(228,228,232)));

                    // The sorted column says so, and says which way. An arrow is
                    // the whole feedback for a click that only reorders rows the
                    // user may not be looking at.
                    std::wstring title = c.title;
                    if (c.sortable && c.key == m_sortKey)
                        title += m_sortAscending ? L" ▲" : L" ▼";

                    SetTextColor(bb, (c.sortable && c.key == m_sortKey) ? fg : PC::HEADER);
                    RECT hr{x + static_cast<int>(4 * s), hdrY, x + cw, hdrY + hdrH};
                    DrawTextW(bb, title.c_str(), -1, &hr,
                              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                    x += cw;
                }

                HGDIOBJ ohp = SelectObject(bb, Gdi::Pen(line));
                MoveToEx(bb, pad, hdrY + hdrH, nullptr);
                LineTo(bb, W - pad, hdrY + hdrH);
                SelectObject(bb, ohp);
            }

            // ── Rows ─────────────────────────────────────────────────────────
            const int listTop = ListTopPx();
            // The bars are INSIDE the client area, so the row band stops short of
            // them — otherwise a long Response would paint straight through the
            // vertical thumb.
            const int listBot = listTop + ViewHeightPx();
            {
                // Clipped, so a long Response cannot paint over the footer or the
                // scrollbars, and a horizontally scrolled column cannot spill
                // past the left edge.
                const int saved = SaveDC(bb);
                IntersectClipRect(bb, pad, listTop, pad + ViewWidthPx(), listBot);

                SelectObject(bb, m_hFontBody);

                // Only the visible band is drawn. With CAPACITY at 20000 the
                // difference between this and painting every row is the
                // difference between a panel that scrolls and one that does not.
                const int first = std::max(0, m_scrollY / rowH);
                const int last  = std::min(static_cast<int>(m_rows.size()),
                                           first + (listBot - listTop) / rowH + 2);

                for (int i = first; i < last; ++i) {
                    const RL::Entry &e = m_rows[static_cast<size_t>(i)];
                    const int y = listTop + i * rowH - m_scrollY;

                    RECT rr{pad, y, W - pad, y + rowH};
                    // Banded, not selected-highlighted: nothing here is
                    // selectable, and seven columns of small text across a wide
                    // window is where the eye loses the row it is reading.
                    if (i % 2) FillRect(bb, &rr, Gdi::Brush(altBg));

                    const bool out = (e.dir == RL::Direction::Out);

                    int x = pad - m_scrollX;
                    auto cell = [&](int colIndex, const std::wstring &t, COLORREF col) {
                        const int cw = static_cast<int>(m_columns[colIndex].width * s);
                        SetTextColor(bb, col);
                        RECT cr{x + static_cast<int>(4 * s), y,
                                x + cw - static_cast<int>(4 * s), y + rowH};
                        DrawTextW(bb, t.c_str(), -1, &cr,
                                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                        x += cw;
                    };

                    // The arrow is the direction, in the column that is already
                    // narrow and already just a number.
                    cell(0, (out ? L"→ " : L"← ") + std::to_wstring(e.seq),
                         out ? dim : PC::CHOICE);
                    cell(1, e.sender,   PC::TEXT);
                    cell(2, e.command,  PC::PATH);
                    cell(3, e.receiver, PC::TEXT);
                    // An ERR reply is the row you opened the log to find.
                    cell(4, e.response,
                         e.response.rfind(L"ERR", 0) == 0 ? PC::WARN : dim);
                    cell(5, RL::FormatTime(e.whenFt), PC::NUMBER);
                    cell(6, RL::FormatDelta(e.deltaUs), PC::NUMBER);
                }

                RestoreDC(bb, saved);
            }

            if (m_rows.empty()) {
                SelectObject(bb, m_hFontBody);
                SetTextColor(bb, dim);
                RECT er{pad, listTop + static_cast<int>(16 * s), W - pad,
                        listTop + static_cast<int>(110 * s)};
                DrawTextW(bb,
                          app.remoteLogEnabled
                              ? L"Recording, but nothing has crossed the wire yet. "
                                L"Connect an instance in Remote Servers (F10) and give "
                                L"this viewer a command with F11 mirroring on."
                              : L"Recording is OFF, which is the default — the log costs "
                                L"nothing while it is off. Press Recording to start; the "
                                L"connected instances are told to start too, so both ends "
                                L"of every exchange are written down.",
                          -1, &er, DT_LEFT | DT_WORDBREAK);
            }

            // ── Scrollbars ───────────────────────────────────────────────────
            // Drawn, not native — see the header. Same colours and the same
            // 12-unit width as the F8 panel, so the two look like one app.
            //
            // The rects are stored as they are drawn, and the hit tests read
            // exactly what is on screen. An empty rect means "that axis does not
            // overflow", which is also how the hit test knows to ignore it.
            {
                const int sbW = static_cast<int>(Constants::Dedicated::PANEL_SCROLLBAR_W * s);
                const int minT = static_cast<int>(Constants::Dedicated::PANEL_SCROLL_MIN_H * s);
                const int viewW = ViewWidthPx();
                const int viewH = ViewHeightPx();
                const int contW = ContentWidthPx();
                const int contH = ContentHeightPx();

                m_vTrack = {}; m_vThumb = {};
                m_hTrack = {}; m_hThumb = {};

                if (contH > viewH && viewH > 0) {
                    m_vTrack = {W - pad - sbW, listTop, W - pad, listTop + viewH};
                    FillRect(bb, &m_vTrack, Gdi::Brush(PC::SCROLL_TRACK));

                    // Proportional, floored so it stays grabbable: with 20000
                    // entries the honest height would be a couple of pixels.
                    int th = std::max(minT, MulDiv(viewH, viewH, contH));
                    th = std::min(th, viewH);
                    const int travel = std::max(0, viewH - th);
                    const int ty = listTop +
                                   (contH > viewH ? MulDiv(m_scrollY, travel, contH - viewH) : 0);

                    m_vThumb = {m_vTrack.left + static_cast<int>(2 * s), ty,
                                m_vTrack.right - static_cast<int>(2 * s), ty + th};
                    FillRect(bb, &m_vThumb, Gdi::Brush(
                        (m_vThumbHot || m_drag == Drag::Vert) ? PC::SCROLL_THUMB_HOT
                                                             : PC::SCROLL_THUMB));
                }

                if (contW > viewW && viewW > 0) {
                    const int hy = listTop + viewH;
                    m_hTrack = {pad, hy, pad + viewW, hy + sbW};
                    FillRect(bb, &m_hTrack, Gdi::Brush(PC::SCROLL_TRACK));

                    int tw = std::max(minT, MulDiv(viewW, viewW, contW));
                    tw = std::min(tw, viewW);
                    const int travel = std::max(0, viewW - tw);
                    const int tx = pad +
                                   (contW > viewW ? MulDiv(m_scrollX, travel, contW - viewW) : 0);

                    m_hThumb = {tx, m_hTrack.top + static_cast<int>(2 * s),
                                tx + tw, m_hTrack.bottom - static_cast<int>(2 * s)};
                    FillRect(bb, &m_hThumb, Gdi::Brush(
                        (m_hThumbHot || m_drag == Drag::Horz) ? PC::SCROLL_THUMB_HOT
                                                             : PC::SCROLL_THUMB));
                }
            }

            // ── Footer ───────────────────────────────────────────────────────
            {
                const int fy = H - static_cast<int>(FOOTER_H * s);
                HGDIOBJ op = SelectObject(bb, Gdi::Pen(line));
                MoveToEx(bb, pad, fy - static_cast<int>(4 * s), nullptr);
                LineTo(bb, W - pad, fy - static_cast<int>(4 * s));
                SelectObject(bb, op);

                std::wstring foot = m_status;
                if (foot.empty())
                    foot = m_followTail
                               ? L"Following the newest entry. Scroll up to hold still."
                               : L"Scrolled — scroll back to the bottom to follow again.";

                SelectObject(bb, m_hFontSmall);
                SetTextColor(bb, dim);
                RECT frc{pad, fy, W - pad, fy + static_cast<int>(18 * s)};
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
void RemoteLogWnd::EnsureFonts(HDC dc) {
    const int dpi = GetDeviceCaps(dc, LOGPIXELSY);
    if (m_hFontBody && dpi == m_cachedFontDpi) return;
    if (m_hFontBody)  DeleteObject(m_hFontBody);
    if (m_hFontBold)  DeleteObject(m_hFontBold);
    if (m_hFontSmall) DeleteObject(m_hFontSmall);
    m_cachedFontDpi = dpi;
    auto mk = [&](int pt, int w, const wchar_t *face) {
        return CreateFontW(-MulDiv(pt, dpi, 72), 0, 0, 0, w, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, VARIABLE_PITCH, face);
    };
    // The rows are wire text and timings, so they get a MONOSPACED face: a
    // column of "0.4 ms" / "12.1 ms" only reads as a column when the digits line
    // up, and a mirrored command is a protocol token, not prose.
    m_hFontBody  = mk(9,  FW_NORMAL,   L"Consolas");
    m_hFontBold  = mk(11, FW_SEMIBOLD, L"Segoe UI");
    m_hFontSmall = mk(8,  FW_NORMAL,   L"Segoe UI");
}

void RemoteLogWnd::EnsureBackBuffer(HDC refDC, int w, int h) {
    if (m_bbDC && m_bbW == w && m_bbH == h) return;
    DestroyBackBuffer();
    m_bbDC     = CreateCompatibleDC(refDC);
    m_bbBmp    = CreateCompatibleBitmap(refDC, w, h);
    m_bbBmpOld = static_cast<HBITMAP>(SelectObject(m_bbDC, m_bbBmp));
    m_bbW = w; m_bbH = h;
}

void RemoteLogWnd::DestroyBackBuffer() {
    if (!m_bbDC) return;
    SelectObject(m_bbDC, m_bbBmpOld);
    DeleteObject(m_bbBmp);
    DeleteDC(m_bbDC);
    m_bbDC = nullptr; m_bbBmp = nullptr; m_bbBmpOld = nullptr; m_bbW = m_bbH = 0;
}

void RemoteLogWnd::Repaint() {
    if (GetHwnd()) InvalidateRect(GetHwnd(), nullptr, FALSE);
}

} // namespace UI
