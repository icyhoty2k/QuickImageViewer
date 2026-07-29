#include "RemoteLogWnd.h"
#include "RemoteMirror.h"   // BroadcastEnableLog — the switch travels

#include "AppState.h"
#include "Platform/Constants.h"
#include "Input/Command.h"       // InputManager::ExecuteCommand — the one sink
// Safe from a .cpp even though UIManager.h includes THIS header: the guard has
// already fired by the time it is reached, so there is no cycle.
#include "UI/UIManager.h"        // the two readouts open F10 / Ctrl+F11
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
    enum ButtonId { BTN_RECORD = 1, BTN_CLEAR, BTN_SAVE, BTN_LOAD,
                    // Beside Load…, because the log and the Send Command panel are
                    // two halves of one activity: you read what crossed the wire,
                    // then type the next line, then read what came back. Walking
                    // between them by keyboard shortcut meant leaving the panel you
                    // were reading. Lit while the other one is open, like the two
                    // buttons below.
                    BTN_SEND_CMD,
                    // Right-aligned status readouts that are also shortcuts.
                    // They sit in the empty half of the button row, which was
                    // doing nothing, and answer the question you have while
                    // looking at a log: is anything actually connected, and is
                    // anything actually being driven?
                    BTN_CONNS, BTN_CONTROL };

    // RECT members are LONG, and every size here is compared against an int
    // expression — std::max(0, rc.bottom - rc.top) will not deduce a common type
    // and fails to compile. Narrowed once, here, rather than with a cast at each
    // of the six call sites.
    int RectW(const RECT &r) { return static_cast<int>(r.right - r.left); }
    int RectH(const RECT &r) { return static_cast<int>(r.bottom - r.top); }

    // Open it, or close it if it is already open — and when opening, actually
    // put it in front.
    //
    // Show() alone is not enough: every panel here is WS_EX_TOPMOST, so they sit
    // in the same z-band and re-showing one that is already visible leaves it
    // wherever it was in that band — behind this log, usually, which is exactly
    // where you cannot see it. The explicit HWND_TOPMOST re-assert is what
    // reorders it WITHIN the band; SetForegroundWindow alone does not, because
    // the window never lost activation to begin with.
    void ToggleToFront(UI::IPanelWindow &panel) {
        if (panel.IsVisible()) { panel.Hide(); return; }

        panel.Show();
        if (HWND h = panel.GetHwnd()) {
            SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            SetForegroundWindow(h);
        }
    }

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
    // CS_DBLCLKS, or WM_LBUTTONDBLCLK is never sent and a double-click arrives
    // as two separate downs — the window class decides this, not the window.
    InitFloating(hInstance, hParent, L"qIVRemoteLogWnd", L"RemoteLog",
                 static_cast<int>(PANEL_W * s), static_cast<int>(PANEL_H * s),
                 CS_DBLCLKS);
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
    // The detail window is a satellite: it shows a row of THIS list, and its
    // Prev/Next step through this list's ordering. Leaving it floating after the
    // list closed would leave buttons that act on something not on screen.
    if (m_detail.GetHwnd()) m_detail.Hide();
    FloatingPanelWnd::Hide();
}

// =============================================================================
// Model
// =============================================================================
void RemoteLogWnd::Rebuild() {
    m_rows = RL::Snapshot();
    ApplySort();
    BuildButtons();

    // Re-find the selection by entry number. Rows arrive and the sort reorders,
    // so the index that pointed at an exchange a moment ago points at a
    // different one now. -1 when the selected entry has been trimmed away.
    m_selectedRow = -1;
    if (m_selectedSeq) {
        for (size_t i = 0; i < m_rows.size(); ++i)
            if (m_rows[i].seq == m_selectedSeq) { m_selectedRow = static_cast<int>(i); break; }
    }

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
    m_buttons.push_back({L"Send Command", BTN_SEND_CMD, {}, true});

    // Labels deliberately EMPTY. These two carry live counts, and building the
    // text here would freeze it until the next Rebuild — which only happens
    // when a log entry lands. The painter fills them in on every repaint
    // instead, so they are current whenever the panel is on screen.
    m_buttons.push_back({L"", BTN_CONNS,   {}, true});
    m_buttons.push_back({L"", BTN_CONTROL, {}, true});
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

int RemoteLogWnd::HitTestRow(POINT pt) const {
    const int top = ListTopPx();
    const int bot = top + ViewHeightPx();
    if (pt.y < top || pt.y >= bot) return -1;

    const float s = app.dpiScale;
    const int pad = static_cast<int>(PAD * s);
    if (pt.x < pad || pt.x >= pad + ViewWidthPx()) return -1;

    // Arithmetic, not stored rects: only the visible band is painted, so the
    // rows above and below the viewport have no rect to test.
    const int idx = (pt.y - top + m_scrollY) / RowHeightPx();
    return (idx >= 0 && idx < static_cast<int>(m_rows.size())) ? idx : -1;
}

void RemoteLogWnd::EnsureSelectionVisible() {
    if (m_selectedRow < 0) return;
    const int rowH = RowHeightPx();
    const int top  = m_selectedRow * rowH;
    const int bot  = top + rowH;

    // Only moves when the row is actually outside — scrolling a row that is
    // already visible into the middle of the view makes the list lurch on every
    // arrow press.
    if (top < m_scrollY)                     ScrollTo(m_scrollX, top);
    else if (bot > m_scrollY + ViewHeightPx()) ScrollTo(m_scrollX, bot - ViewHeightPx());
    else Repaint();
}

void RemoteLogWnd::DoOpenDetail() {
    if (m_selectedRow < 0 || m_selectedRow >= static_cast<int>(m_rows.size())) return;

    // Parented to the MAIN window, not to this panel: a panel owned by a panel
    // is destroyed when the owner hides, and closing the list while reading a
    // row would take the row with it.
    if (!m_detail.GetHwnd()) {
        m_detail.Init(reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(GetHwnd(), GWLP_HINSTANCE)),
                      m_hParent);
        m_detail.SetOwner(this);
    }
    m_detail.ShowEntry(m_rows[static_cast<size_t>(m_selectedRow)]);
}

bool RemoteLogWnd::CanStepSelection(int delta) const {
    const int next = m_selectedRow + delta;
    return m_selectedRow >= 0 && next >= 0 && next < static_cast<int>(m_rows.size());
}

bool RemoteLogWnd::StepSelection(int delta) {
    if (!CanStepSelection(delta)) return false;

    m_selectedRow += delta;
    m_selectedSeq  = m_rows[static_cast<size_t>(m_selectedRow)].seq;

    // Stepping from the detail window scrolls the LIST too, so closing it leaves
    // the caret where the reading got to rather than back where it started.
    EnsureSelectionVisible();

    if (m_detail.GetHwnd() && m_detail.IsVisible())
        m_detail.ShowEntry(m_rows[static_cast<size_t>(m_selectedRow)]);
    return true;
}

// =============================================================================
// Keyboard
// =============================================================================
bool RemoteLogWnd::OnKeyDown(WPARAM vk, bool ctrl, bool /*shift*/, bool /*alt*/) {
    const int row  = RowHeightPx();
    const int page = std::max(row, ViewHeightPx());

    // Rows per page, so PgUp/PgDn move the SELECTION by a screenful rather than
    // scrolling out from under it.
    const int perPage = std::max(1, page / row);

    switch (vk) {
        // Up/Down move the selection, not the view — the list is clickable now,
        // and a caret that the arrow keys cannot move is a caret that looks
        // broken. The view follows via EnsureSelectionVisible.
        case VK_UP:
            if (m_selectedRow < 0) { m_selectedRow = 0; m_selectedSeq = m_rows.empty() ? 0 : m_rows[0].seq; EnsureSelectionVisible(); }
            else StepSelection(-1);
            return true;
        case VK_DOWN:
            if (m_selectedRow < 0) { m_selectedRow = 0; m_selectedSeq = m_rows.empty() ? 0 : m_rows[0].seq; EnsureSelectionVisible(); }
            else StepSelection(+1);
            return true;
        case VK_PRIOR:
            if (!StepSelection(-perPage)) StepSelection(-m_selectedRow);
            return true;
        case VK_NEXT:
            if (!StepSelection(+perPage))
                StepSelection(static_cast<int>(m_rows.size()) - 1 - m_selectedRow);
            return true;
        // Horizontal stays pure scrolling: there is nothing to select sideways.
        case VK_LEFT:  ScrollTo(m_scrollX - row * 2, m_scrollY); return true;
        case VK_RIGHT: ScrollTo(m_scrollX + row * 2, m_scrollY); return true;
        case VK_HOME:  ScrollTo(0, 0); return true;
        case VK_END:   ScrollTo(m_scrollX, ContentHeightPx()); return true;
        case VK_RETURN: DoOpenDetail(); return true;
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
                    case BTN_SEND_CMD:
                        ToggleToFront(uiManager.getRemoteCmdWindow());
                        Repaint();
                        break;
                    case BTN_CONNS:
                        ToggleToFront(uiManager.getRemotesConsoleWindow());
                        // The button's own lit state just changed, and opening
                        // another window takes the focus — so this panel would
                        // not otherwise repaint until something else touched it.
                        Repaint();
                        break;
                    case BTN_CONTROL:
                        ToggleToFront(uiManager.getMirrorPickerWindow());
                        Repaint();
                        break;
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

            // Last, so the bars and the header get the click first.
            const int r = HitTestRow(pt);
            if (r >= 0) {
                m_selectedRow = r;
                m_selectedSeq = m_rows[static_cast<size_t>(r)].seq;
                Repaint();
            }
            return 0;
        }

        // Double-click a row → the whole entry, untrimmed. The table has to
        // ellipsise; this is where the full text lives.
        case WM_LBUTTONDBLCLK: {
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const int r = HitTestRow(pt);
            if (r >= 0) {
                m_selectedRow = r;
                m_selectedSeq = m_rows[static_cast<size_t>(r)].seq;
                DoOpenDetail();
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
            const COLORREF selBg = dark ? RGB(58,86,132)   : RGB(203,222,250);
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
                    L"click #, Time or Δ time to sort   ·   double-click a row for the "
                    L"full text   ·   the two counts on the right open F10 / Ctrl+F11";
                DrawTextW(bb, sub.c_str(), -1, &sr, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            }

            // ── Buttons ──────────────────────────────────────────────────────
            {
                const int gap  = static_cast<int>(BTN_GAP * s);
                const int y    = static_cast<int>(TITLE_H * s);
                const int wide = static_cast<int>(168 * s);   // the two readouts

                // Read ONCE for this frame, so the two buttons cannot disagree
                // about how many targets are connected.
                const int total  = Remote::Mirror::TargetCount();
                const int live   = Remote::Mirror::ConnectedCount();
                const int driven = Remote::Mirror::MirroredLiveCount();

                // The readouts are laid out from the RIGHT edge, so the gap
                // between the two groups grows with the window instead of
                // leaving the row lopsided.
                int x     = pad;
                int rightX = W - pad;

                for (Button &btn : m_buttons) {
                    const bool isStatus = (btn.id == BTN_CONNS || btn.id == BTN_CONTROL);

                    // Recording is wider: its label carries the state, so it
                    // changes width, and a button that resizes as you press it
                    // moves the one beside it under the cursor.
                    // Send Command needs room for its label, the same way Recording
                    // does — a two-word button clipped to 92 px reads as "Send Co…".
                    const int bw = isStatus ? wide
                                 : static_cast<int>((btn.id == BTN_RECORD   ? 150 :
                                                     btn.id == BTN_SEND_CMD ? 130 : 92) * s);

                    if (isStatus) {
                        // Right to left, in reverse of the order they are
                        // declared, so Connections ends up left of Control.
                        rightX -= bw;
                        btn.rect = {rightX, y, rightX + bw, y + btnH};
                        rightX -= gap;
                    } else {
                        btn.rect = {x, y, x + bw, y + btnH};
                        x += bw + gap;
                    }

                    // Live text, built here rather than in BuildButtons — see
                    // the note there. "active / total", the way every other
                    // count in this app reads.
                    std::wstring label = btn.label;
                    if (btn.id == BTN_CONNS)
                        label = L"Connections  " + std::to_wstring(live) +
                                L" / " + std::to_wstring(total);
                    else if (btn.id == BTN_CONTROL)
                        label = L"Control  " + std::to_wstring(driven) +
                                L" / " + std::to_wstring(live);

                    const int myIndex = static_cast<int>(&btn - m_buttons.data());

                    // All three of these are TOGGLES, so the accent shows what
                    // is switched ON — for the two readouts that means "the
                    // panel I open is open", not "my count is non-zero". A
                    // toggle whose lit state tracked something other than what
                    // it toggles is a button that lies about its own press.
                    COLORREF base = PC::BTN_MAIN;
                    if (btn.id == BTN_RECORD && app.remoteLogEnabled) base = PC::BTN_ALT;
                    else if (btn.id == BTN_CONNS &&
                             uiManager.getRemotesConsoleWindow().IsVisible()) base = PC::BTN_ALT;
                    else if (btn.id == BTN_CONTROL &&
                             uiManager.getMirrorPickerWindow().IsVisible())   base = PC::BTN_ALT;
                    else if (btn.id == BTN_SEND_CMD &&
                             uiManager.getRemoteCmdWindow().IsVisible())      base = PC::BTN_ALT;

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

                    // The "is anything there" signal moved to the TEXT, since
                    // the fill now belongs to the toggle: dimmed digits mean the
                    // count is zero, which reads at a glance without stealing
                    // the accent from the toggle state.
                    COLORREF fgText = RGB(245, 245, 245);
                    if (!btn.enabled)                            fgText = dim;
                    else if (btn.id == BTN_CONNS   && live   == 0) fgText = dim;
                    else if (btn.id == BTN_CONTROL && driven == 0) fgText = dim;
                    SetTextColor(bb, fgText);
                    RECT lr = btn.rect;
                    DrawTextW(bb, label.c_str(), -1, &lr,
                              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
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

                    RECT rr{pad, y, pad + ViewWidthPx(), y + rowH};
                    // Banded so the eye keeps its place across seven columns of
                    // small text, and the selected row on top of that — it is
                    // what Enter and the detail window act on.
                    if (i == m_selectedRow)  FillRect(bb, &rr, Gdi::Brush(selBg));
                    else if (i % 2)          FillRect(bb, &rr, Gdi::Brush(altBg));

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
// RemoteLogEntryWnd — one entry, nothing trimmed
// =============================================================================
namespace {
    constexpr int DETAIL_W    = 660;
    constexpr int DETAIL_H    = 520;
    constexpr int DETAIL_LBL  = 96;   // label gutter, design units
    constexpr int DETAIL_GAP  = 10;   // between fields

    enum DetailBtnId { DBTN_PREV = 1, DBTN_NEXT, DBTN_COPY, DBTN_CLOSE };
}

void RemoteLogEntryWnd::Init(HINSTANCE hInstance, HWND hParent) {
    const float s = app.dpiScale;
    InitFloating(hInstance, hParent, L"qIVRemoteLogEntryWnd", L"Log entry",
                 static_cast<int>(DETAIL_W * s), static_cast<int>(DETAIL_H * s));
    if (!GetHwnd()) return;
    SetWindowLongPtrW(GetHwnd(), GWL_EXSTYLE,
                      GetWindowLongPtrW(GetHwnd(), GWL_EXSTYLE) | WS_EX_LAYERED);
    SetLayeredWindowAttributes(GetHwnd(), 0,
                               Constants::Dedicated::PANEL_OPACITY, LWA_ALPHA);
}

void RemoteLogEntryWnd::Init(HINSTANCE hInstance, HWND hParent, int8_t) {
    Init(hInstance, hParent);
}

void RemoteLogEntryWnd::Show() { ShowCenterOverParent(); Repaint(); }

void RemoteLogEntryWnd::ShowEntry(const Remote::Log::Entry &e) {
    // COPIED, not referenced — the store trims from the front as it fills, and
    // this window is meant to sit open while you read it.
    m_entry = e;
    BuildFields();

    // Back to the top for a new entry: stepping to the next one and landing
    // halfway down its Response is not where anybody wants to start reading.
    m_scrollY = 0;

    if (!IsVisible()) ShowCenterOverParent();
    else              SetForegroundWindow(GetHwnd());
    Repaint();
}

void RemoteLogEntryWnd::BuildFields() {
    const bool out = (m_entry.dir == Remote::Log::Direction::Out);

    m_fields = {
        { L"#",         std::to_wstring(m_entry.seq) },
        { L"Direction", out ? L"→ sent by this instance" : L"← received" },
        { L"Sender",    m_entry.sender },
        { L"Command",   m_entry.command },
        { L"Receiver",  m_entry.receiver },
        { L"Response",  m_entry.response },
        { L"Time",      Remote::Log::FormatTime(m_entry.whenFt) },
        { L"Δ time",    Remote::Log::FormatDelta(m_entry.deltaUs) +
                        (out ? L"   (round trip)" : L"   (handling time here)") },
    };

    // Greyed rather than hidden at the ends of the list: buttons that vanish
    // move the ones beside them under the cursor between two clicks.
    const bool canPrev = m_owner && m_owner->CanStepSelection(-1);
    const bool canNext = m_owner && m_owner->CanStepSelection(+1);

    m_buttons = {
        { L"◀ Previous", DBTN_PREV,  {}, canPrev },
        { L"Next ▶",     DBTN_NEXT,  {}, canNext },
        { L"Copy",       DBTN_COPY,  {}, true    },
        { L"Close",      DBTN_CLOSE, {}, true    },
    };
}

void RemoteLogEntryWnd::DoStep(int delta) {
    // Delegated: the LIST owns the sort, so it is the only thing that knows
    // which entry is next. It calls back into ShowEntry.
    if (m_owner) m_owner->StepSelection(delta);
}

void RemoteLogEntryWnd::DoCopy() {
    std::wstring text;
    for (const Field &f : m_fields) {
        text += f.label;
        text += L": ";
        text += f.value;
        text += L"\r\n";
    }

    if (!OpenClipboard(GetHwnd())) return;
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (void *p = GlobalLock(h)) {
            memcpy(p, text.c_str(), bytes);
            GlobalUnlock(h);
            // Ownership passes to the clipboard on success only — freeing it
            // after a successful SetClipboardData is a double free.
            if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
        } else {
            GlobalFree(h);
        }
    }
    CloseClipboard();
}

int RemoteLogEntryWnd::HitTestBtn(POINT pt) const {
    for (size_t i = 0; i < m_buttons.size(); ++i)
        if (m_buttons[i].enabled && PtInRect(&m_buttons[i].rect, pt))
            return static_cast<int>(i);
    return -1;
}

int RemoteLogEntryWnd::ViewHeightPx() const {
    if (!GetHwnd()) return 0;
    RECT rc{};
    GetClientRect(GetHwnd(), &rc);
    const float s = app.dpiScale;
    return std::max(0, RectH(rc) - static_cast<int>((TITLE_H + BTN_H + 12) * s));
}

void RemoteLogEntryWnd::ScrollTo(int y) {
    m_scrollY = std::clamp(y, 0, std::max(0, m_contentH - ViewHeightPx()));
    Repaint();
}

bool RemoteLogEntryWnd::OnKeyDown(WPARAM vk, bool /*ctrl*/, bool /*shift*/, bool /*alt*/) {
    switch (vk) {
        // Prev/Next on the keys that already mean "the one before / after" in
        // every list in this app.
        case VK_LEFT:
        case VK_PRIOR: DoStep(-1); return true;
        case VK_RIGHT:
        case VK_NEXT:  DoStep(+1); return true;

        case VK_UP:    ScrollTo(m_scrollY - static_cast<int>(24 * app.dpiScale)); return true;
        case VK_DOWN:  ScrollTo(m_scrollY + static_cast<int>(24 * app.dpiScale)); return true;
        case VK_HOME:  ScrollTo(0); return true;
        case VK_END:   ScrollTo(m_contentH); return true;
        case 'C':      DoCopy(); return true;
        default: break;
    }
    return false;
}

LRESULT RemoteLogEntryWnd::HandlePanelMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_SETCURSOR: {
            if (LOWORD(lParam) != HTCLIENT) break;
            POINT pt; GetCursorPos(&pt);
            ScreenToClient(GetHwnd(), &pt);
            SetCursor(HitTestBtn(pt) >= 0 ? Constants::Cursors::CURR_CLICK
                                          : Constants::Cursors::CURR_DEFAULT);
            return TRUE;
        }

        case WM_MOUSEMOVE: {
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (m_dragging) {
                const int travel = std::max(1, RectH(m_track) - m_dragThumbSpan);
                const int wanted = pt.y - m_track.top - m_dragGrabPx;
                ScrollTo(MulDiv(std::clamp(wanted, 0, travel),
                                std::max(0, m_contentH - ViewHeightPx()), travel));
                return 0;
            }
            const int b = HitTestBtn(pt);
            const bool tHot = PtInRect(&m_thumb, pt) != 0;
            if (b != m_hotBtn || tHot != m_thumbHot) {
                m_hotBtn = b; m_thumbHot = tHot;
                Repaint();
            }
            return 0;
        }

        case WM_LBUTTONDOWN: {
            SetFocus(GetHwnd());
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};

            const int b = HitTestBtn(pt);
            if (b >= 0) {
                switch (m_buttons[b].id) {
                    case DBTN_PREV:  DoStep(-1); break;
                    case DBTN_NEXT:  DoStep(+1); break;
                    case DBTN_COPY:  DoCopy();   break;
                    case DBTN_CLOSE: Hide();     break;
                    default: break;
                }
                return 0;
            }
            if (PtInRect(&m_thumb, pt)) {
                m_dragging      = true;
                m_dragGrabPx    = pt.y - m_thumb.top;
                m_dragThumbSpan = RectH(m_thumb);
                SetCapture(GetHwnd());
                return 0;
            }
            if (PtInRect(&m_track, pt))
                ScrollTo(m_scrollY + (pt.y < m_thumb.top ? -ViewHeightPx() : ViewHeightPx()));
            return 0;
        }

        case WM_LBUTTONUP:
            if (m_dragging) { m_dragging = false; ReleaseCapture(); Repaint(); }
            return 0;

        case WM_CAPTURECHANGED:
            m_dragging = false;
            Repaint();
            return 0;

        case WM_MOUSEWHEEL: {
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            ScrollTo(m_scrollY - (delta / WHEEL_DELTA) * static_cast<int>(48 * app.dpiScale));
            return 0;
        }

        case WM_SIZE:
            Repaint();
            return 0;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(GetHwnd(), &ps);
            RECT rc; GetClientRect(GetHwnd(), &rc);
            const int W = rc.right - rc.left, H = rc.bottom - rc.top;

            EnsureFonts(dc);
            EnsureBackBuffer(dc, W, H);
            HDC bb = m_bbDC;

            const COLORREF bg   = GetBgColor();
            const bool     dark = BgIsDark(bg);
            const COLORREF fg   = dark ? RGB(235,235,235) : RGB(24,24,24);
            const COLORREF dim  = dark ? RGB(150,150,150) : RGB(110,110,110);
            const COLORREF line = dark ? RGB(64,64,64)    : RGB(220,220,220);

            FillRect(bb, &rc, Gdi::Brush(bg));
            SetBkMode(bb, TRANSPARENT);

            const float s   = app.dpiScale;
            const int pad   = static_cast<int>(PAD * s);
            const int btnH  = static_cast<int>(BTN_H * s);
            const int lblW  = static_cast<int>(DETAIL_LBL * s);
            const int gap   = static_cast<int>(DETAIL_GAP * s);
            const int sbW   = static_cast<int>(Constants::Dedicated::PANEL_SCROLLBAR_W * s);

            // ── Title ────────────────────────────────────────────────────────
            SelectObject(bb, m_hFontBold);
            SetTextColor(bb, fg);
            {
                RECT tr{pad, static_cast<int>(6 * s), W - pad, static_cast<int>(26 * s)};
                const std::wstring t = L"Log entry #" + std::to_wstring(m_entry.seq);
                DrawTextW(bb, t.c_str(), -1, &tr, DT_LEFT | DT_SINGLELINE);

                SelectObject(bb, m_hFontSmall);
                SetTextColor(bb, dim);
                RECT sr{pad, tr.bottom, W - pad, tr.bottom + static_cast<int>(16 * s)};
                DrawTextW(bb, L"Full text, nothing trimmed   ·   ← → steps entries   ·   "
                              L"C copies   ·   Esc closes",
                          -1, &sr, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            }

            // ── Buttons ──────────────────────────────────────────────────────
            {
                const int bgap = static_cast<int>(BTN_GAP * s);
                const int bw   = static_cast<int>(104 * s);
                int x = pad;
                const int y = static_cast<int>(TITLE_H * s);
                for (Btn &btn : m_buttons) {
                    btn.rect = {x, y, x + bw, y + btnH};
                    const int myIndex = static_cast<int>(&btn - m_buttons.data());

                    COLORREF base = PC::BTN_MAIN;
                    if (!btn.enabled) base = bg;
                    else if (myIndex == m_hotBtn)
                        base = RGB(std::min(255, GetRValue(base) + 40),
                                   std::min(255, GetGValue(base) + 40),
                                   std::min(255, GetBValue(base) + 40));

                    FillRect(bb, &btn.rect, Gdi::Brush(base));
                    HGDIOBJ op = SelectObject(bb, Gdi::Pen(line));
                    HGDIOBJ ob = SelectObject(bb, GetStockObject(NULL_BRUSH));
                    Rectangle(bb, btn.rect.left, btn.rect.top, btn.rect.right, btn.rect.bottom);
                    SelectObject(bb, ob); SelectObject(bb, op);

                    SelectObject(bb, m_hFontSmall);
                    SetTextColor(bb, btn.enabled ? RGB(245,245,245) : dim);
                    RECT lr = btn.rect;
                    DrawTextW(bb, btn.label, -1, &lr,
                              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    x += bw + bgap;
                }
            }

            // ── Fields ───────────────────────────────────────────────────────
            const int listTop = static_cast<int>((TITLE_H + BTN_H + 12) * s);
            const int viewH   = ViewHeightPx();

            // Measured first, because the value column's height depends on how
            // the text wraps and the scrollbar has to exist before the rows are
            // drawn against it.
            int valueRight = W - pad;
            {
                // Reserve the bar only when it is needed, which needs the height
                // — measured once with the bar absent, then again if it turned
                // out to be needed. Two passes rather than a guess.
                for (int pass = 0; pass < 2; ++pass) {
                    SelectObject(bb, m_hFontBody);
                    int h = 0;
                    for (const Field &f : m_fields) {
                        RECT mr{pad + lblW, 0, valueRight, 0};
                        DrawTextW(bb, f.value.empty() ? L"—" : f.value.c_str(), -1, &mr,
                                  DT_LEFT | DT_WORDBREAK | DT_CALCRECT | DT_EDITCONTROL);
                        h += std::max(static_cast<int>(18 * s), RectH(mr)) + gap;
                    }
                    m_contentH = h;
                    if (m_contentH <= viewH || valueRight != W - pad) break;
                    valueRight = W - pad - sbW; // needs a bar: measure again narrower
                }
            }
            m_scrollY = std::clamp(m_scrollY, 0, std::max(0, m_contentH - viewH));

            {
                const int saved = SaveDC(bb);
                IntersectClipRect(bb, pad, listTop, W - pad, listTop + viewH);

                int y = listTop - m_scrollY;
                for (const Field &f : m_fields) {
                    SelectObject(bb, m_hFontBody);
                    RECT vr{pad + lblW, y, valueRight, y};
                    DrawTextW(bb, f.value.empty() ? L"—" : f.value.c_str(), -1, &vr,
                              DT_LEFT | DT_WORDBREAK | DT_CALCRECT | DT_EDITCONTROL);
                    const int blockH = std::max(static_cast<int>(18 * s), RectH(vr));

                    // Label and value share a baseline; the value is the one
                    // that wraps, so the label sits at the top of the block.
                    SelectObject(bb, m_hFontSmall);
                    SetTextColor(bb, PC::HEADER);
                    RECT lr{pad, y, pad + lblW - static_cast<int>(8 * s),
                            y + static_cast<int>(18 * s)};
                    DrawTextW(bb, f.label, -1, &lr, DT_LEFT | DT_SINGLELINE);

                    SelectObject(bb, m_hFontBody);
                    SetTextColor(bb, fg);
                    RECT dr{pad + lblW, y, valueRight, y + blockH};
                    DrawTextW(bb, f.value.empty() ? L"—" : f.value.c_str(), -1, &dr,
                              DT_LEFT | DT_WORDBREAK | DT_EDITCONTROL);

                    y += blockH + gap;
                }

                RestoreDC(bb, saved);
            }

            // ── Scrollbar ────────────────────────────────────────────────────
            m_track = {}; m_thumb = {};
            if (m_contentH > viewH && viewH > 0) {
                m_track = {W - pad - sbW, listTop, W - pad, listTop + viewH};
                FillRect(bb, &m_track, Gdi::Brush(PC::SCROLL_TRACK));

                const int minT = static_cast<int>(Constants::Dedicated::PANEL_SCROLL_MIN_H * s);
                int th = std::max(minT, MulDiv(viewH, viewH, m_contentH));
                th = std::min(th, viewH);
                const int travel = std::max(0, viewH - th);
                const int ty = listTop + MulDiv(m_scrollY, travel,
                                                std::max(1, m_contentH - viewH));

                m_thumb = {m_track.left + static_cast<int>(2 * s), ty,
                           m_track.right - static_cast<int>(2 * s), ty + th};
                FillRect(bb, &m_thumb, Gdi::Brush(
                    (m_thumbHot || m_dragging) ? PC::SCROLL_THUMB_HOT : PC::SCROLL_THUMB));
            }

            BitBlt(dc, 0, 0, W, H, bb, 0, 0, SRCCOPY);
            EndPaint(GetHwnd(), &ps);
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;

        default:
            break;
    }
    return DefWindowProcW(GetHwnd(), message, wParam, lParam);
}

void RemoteLogEntryWnd::EnsureFonts(HDC dc) {
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
    // Monospaced values, same as the list: these are protocol tokens and paths,
    // and a proportional font makes a long payload harder to read, not easier.
    m_hFontBody  = mk(9,  FW_NORMAL,   L"Consolas");
    m_hFontBold  = mk(11, FW_SEMIBOLD, L"Segoe UI");
    m_hFontSmall = mk(8,  FW_NORMAL,   L"Segoe UI");
}

void RemoteLogEntryWnd::EnsureBackBuffer(HDC refDC, int w, int h) {
    if (m_bbDC && m_bbW == w && m_bbH == h) return;
    DestroyBackBuffer();
    m_bbDC     = CreateCompatibleDC(refDC);
    m_bbBmp    = CreateCompatibleBitmap(refDC, w, h);
    m_bbBmpOld = static_cast<HBITMAP>(SelectObject(m_bbDC, m_bbBmp));
    m_bbW = w; m_bbH = h;
}

void RemoteLogEntryWnd::DestroyBackBuffer() {
    if (!m_bbDC) return;
    SelectObject(m_bbDC, m_bbBmpOld);
    DeleteObject(m_bbBmp);
    DeleteDC(m_bbDC);
    m_bbDC = nullptr; m_bbBmp = nullptr; m_bbBmpOld = nullptr; m_bbW = m_bbH = 0;
}

void RemoteLogEntryWnd::Repaint() {
    if (GetHwnd()) InvalidateRect(GetHwnd(), nullptr, FALSE);
}

int RemoteLogEntryWnd::ContentHeightPx() { return m_contentH; }

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
