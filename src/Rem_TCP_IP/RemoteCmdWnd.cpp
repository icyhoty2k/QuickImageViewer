#include "RemoteCmdWnd.h"
#include "RemoteProtocol.h"   // CommandTable — the ONE list of what is accepted
#include "RemoteExec.h"       // ExecutePayload — the also-run-here path

#include "AppState.h"
#include "Platform/Constants.h"
#include "Input/Command.h"
// Safe from a .cpp even though UIManager.h includes THIS header: the guard has
// already fired by the time it is reached, so there is no cycle. Same note as
// RemoteLogWnd.cpp, which reaches the other way for the same reason.
#include "UI/UIManager.h"      // the Log button opens/closes Ctrl+F12
#include "UI/GdiPool.h"

#include <algorithm>
#include <windowsx.h>

extern AppState app;

namespace UI {

namespace PC = Constants::Dedicated::PanelColors;

namespace {

    constexpr int PANEL_W = 860;
    constexpr int PANEL_H = 560;
    constexpr int PAD     = 14;
    constexpr int BTN_H   = 32;
    constexpr int BTN_GAP = 8;
    constexpr int FIELD_H = 30;
    constexpr int ROW_H   = 22;
    constexpr int HDR_H   = 20;

    // The left column holds the list; the right holds everything about the one
    // command that is highlighted.
    // The description row along the top: the highlighted command's description on
    // the left, its value's description on the right, each over the box it belongs
    // to. Fixed so the two boxes below stay level whichever text is longer — three
    // lines of the small font, which is what the longest entries in the table need.
    constexpr int DESC_H  = 48;

    // The recipients box under the buttons: which connected instances this send
    // reaches. Re-read when Remote::Mirror says a connection changed — see Show().
    constexpr int TARGETS_HDR_H = 20;

    // The session log along the bottom. Bounded so a panel left open all day cannot
    // grow without limit; the oldest rows go, because the recent end of a
    // conversation is the interesting one.
    constexpr size_t LOG_MAX_ROWS = 400;
    constexpr int LIST_W  = 380;
    constexpr int LOG_H   = 120;   // the replies strip along the bottom

    // BTN_LOG sits beside Clear and toggles the Ctrl+F12 wire log: typing a line
    // and reading what came back are one activity, and the log's own button row
    // carries the mirror of this one. Lit while that panel is open, so the pair
    // says which way you are about to jump.
    enum ButtonId { BTN_SEND = 1, BTN_CLEAR, BTN_LOG, BTN_CLOSE };

    bool BgIsDark(COLORREF bg) {
        const int lum = (GetRValue(bg) * 299 + GetGValue(bg) * 587 + GetBValue(bg) * 114) / 1000;
        return lum < 128;
    }

    int RectW(const RECT &r) { return static_cast<int>(r.right - r.left); }
    int RectH(const RECT &r) { return static_cast<int>(r.bottom - r.top); }

    // Substring, not prefix: half the wire names are compound (SortByDate,
    // ThemeFactorUp) and typing "date" should find the first of those.
    bool ContainsI(const std::wstring &hay, const std::wstring &needle) {
        if (needle.empty()) return true;
        if (needle.size() > hay.size()) return false;
        for (size_t i = 0; i + needle.size() <= hay.size(); ++i)
            if (_wcsnicmp(hay.c_str() + i, needle.c_str(), needle.size()) == 0) return true;
        return false;
    }

    void FrameBox(HDC bb, const RECT &r, COLORREF fill, COLORREF border) {
        FillRect(bb, &r, Gdi::Brush(fill));
        HGDIOBJ op = SelectObject(bb, Gdi::Pen(border));
        HGDIOBJ ob = SelectObject(bb, GetStockObject(NULL_BRUSH));
        Rectangle(bb, r.left, r.top, r.right, r.bottom);
        SelectObject(bb, ob); SelectObject(bb, op);
    }

} // namespace

// =============================================================================
// ScrollView
// =============================================================================
int RemoteCmdWnd::ScrollView::MaxScroll() const {
    return std::max(0, contentH - RectH(view));
}

void RemoteCmdWnd::ScrollView::Clamp() {
    scrollY = std::clamp(scrollY, 0, MaxScroll());
}

void RemoteCmdWnd::ScrollView::ScrollBy(int dy) {
    scrollY += dy;
    Clamp();
}

// =============================================================================
// Init / Show
// =============================================================================
void RemoteCmdWnd::Init(HINSTANCE hInstance, HWND hParent) {
    const float s = app.dpiScale;
    InitFloating(hInstance, hParent, L"qIVRemoteCmdWnd", L"Send Command",
                 static_cast<int>(PANEL_W * s), static_cast<int>(PANEL_H * s));
    if (!GetHwnd()) return;

    SetWindowLongPtrW(GetHwnd(), GWL_EXSTYLE,
                      GetWindowLongPtrW(GetHwnd(), GWL_EXSTYLE) | WS_EX_LAYERED);
    SetLayeredWindowAttributes(GetHwnd(), 0,
                               Constants::Dedicated::PANEL_OPACITY, LWA_ALPHA);

    m_filterBox.SetMaxLength(48);
    m_valueBox.SetMaxLength(512);

    BuildCommands();
    ApplyFilter();
    BuildButtons();
}

void RemoteCmdWnd::Init(HINSTANCE hInstance, HWND hParent, int8_t) {
    Init(hInstance, hParent);
}

void RemoteCmdWnd::Show() {
    // The LOG survives a close and reopen, along with the filter and the value: the
    // panel is closed and opened constantly while working — to see the picture
    // behind it, to look at the wire log — and a record that emptied itself each
    // time would be no record. Clear is the button for that, and it says so.
    //
    // The waiting counter does NOT survive: any answer still owed to a send from
    // before the panel was closed will never be counted down, and a footer stuck on
    // "waiting for 2…" forever is worse than not knowing.
    m_awaiting = 0;
    m_status.clear();
    m_focus = Focus::Filter;

    ApplyFilter();
    BuildButtons();
    RefreshTargets();
    ShowCenterOverParent();
    SetFocus(GetHwnd());
    // Instances come and go while this panel sits open, and the recipient list is
    // the one part of it that would otherwise show whatever was true when it was
    // last touched. TOLD, not polled: Remote::Mirror posts
    // WM_QIV_REMOTE_TARGETS_CHANGED on every connect/disconnect, coalesced at the
    // source, and it holds several subscribers so the F10 console can be open at the
    // same time. This panel briefly used a timer for exactly this, because that
    // notification took a single HWND.
    Remote::Mirror::AddPanelNotify(GetHwnd());
    Repaint();
}

void RemoteCmdWnd::Hide() {
    // Unsubscribe FIRST, so no sender thread can post to a window on its way out.
    Remote::Mirror::RemovePanelNotify(GetHwnd());
    FloatingPanelWnd::Hide();
}

void RemoteCmdWnd::RefreshTargets() {
    const std::vector<Remote::Mirror::TargetView> live = Remote::Mirror::Targets();

    m_targetRows.clear();
    for (const Remote::Mirror::TargetView &t : live) {
        // CONNECTED only. A listed-but-idle row cannot answer, so a tick beside it
        // would promise a delivery the send cannot make.
        if (!t.connected) continue;
        m_targetRows.push_back({t.id, t.name, t.host, t.port});
    }

    // Exclusions for instances that no longer exist are dropped, so a removed row
    // cannot come back unticked years later under a recycled id.
    std::vector<int> stillHere;
    stillHere.reserve(m_untickedIds.size());
    for (const int id : m_untickedIds)
        for (const TargetRow &r : m_targetRows)
            if (r.id == id) { stillHere.push_back(id); break; }
    m_untickedIds.swap(stillHere);
}

void RemoteCmdWnd::AddLogRow(Reply &&row) {
    // NEWEST AT THE TOP — the opposite of the Ctrl+F12 wire log, deliberately.
    //
    // That log is a transcript you read forwards to follow an exchange. This one
    // answers "what did that just say?", and the answer to that is always the newest
    // line: at the top it is there the instant it lands, with no scrolling and
    // nothing to follow. The wire log's order is right for its job and wrong for
    // this one.
    row.seq = m_nextSeq++;
    m_replies.insert(m_replies.begin(), std::move(row));

    // Bounded. The OLDEST is dropped, which is now the LAST row rather than the
    // first — a panel left open through a long session must not grow without limit,
    // and the recent end of a conversation is the interesting one.
    while (m_replies.size() > LOG_MAX_ROWS) m_replies.pop_back();

    // Keep the reader's PLACE. Everything already in the box has just moved down by
    // one row, so a reader who had scrolled into the history would otherwise drift
    // by a row per arriving message. At the top (the normal case) the offset stays 0
    // and the new line simply appears.
    const int rowH = static_cast<int>(ROW_H * app.dpiScale);
    m_log.contentH = static_cast<int>(m_replies.size()) * rowH;
    if (m_log.scrollY > 0) m_log.scrollY += rowH;
    m_log.Clamp();
}

std::vector<int> RemoteCmdWnd::CheckedTargetIds() const {
    std::vector<int> ids;
    ids.reserve(m_targetRows.size());
    for (const TargetRow &r : m_targetRows) {
        const bool unticked =
            std::find(m_untickedIds.begin(), m_untickedIds.end(), r.id) !=
            m_untickedIds.end();
        if (!unticked) ids.push_back(r.id);
    }
    return ids;
}

// =============================================================================
// Model
// =============================================================================
void RemoteCmdWnd::BuildCommands() {
    m_all.clear();

    size_t count = 0;
    const Remote::CommandEntry *table = Remote::CommandTable(count);
    if (!table) return;

    for (size_t i = 0; i < count; ++i) {
        const Remote::CommandEntry &e = table[i];

        // The table is one row per command and a static_assert proves it, so
        // this is belt and braces rather than real de-duplication — but a
        // silently doubled row here would be a command offered twice, and the
        // check costs nothing at startup.
        bool already = false;
        for (size_t j = 0; j < i; ++j)
            if (table[j].cmd == e.cmd) { already = true; break; }
        if (already) continue;

        m_all.push_back({e.name,
                         e.desc ? e.desc : L"",
                         e.valueDesc ? e.valueDesc : L"",
                         e.payload == Remote::PayloadRule::Required});
    }

    std::sort(m_all.begin(), m_all.end(),
              [](const Command &a, const Command &b) {
                  return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
              });
}

void RemoteCmdWnd::ApplyFilter() {
    // Remember by NAME. Narrowing the filter renumbers everything, and an index
    // would silently come to mean a different command — the one about to be sent.
    const std::wstring keep =
        (m_selected >= 0 && m_selected < static_cast<int>(m_shown.size()))
            ? m_shown[static_cast<size_t>(m_selected)].name
            : std::wstring{};

    const std::wstring f = m_filterBox.GetText();
    m_shown.clear();
    for (const Command &c : m_all)
        if (ContainsI(c.name, f) || ContainsI(c.desc, f)) m_shown.push_back(c);

    m_selected = 0;
    if (!keep.empty()) {
        for (size_t i = 0; i < m_shown.size(); ++i)
            if (m_shown[i].name == keep) { m_selected = static_cast<int>(i); break; }
    }

    m_list.contentH = static_cast<int>(m_shown.size()) *
                      static_cast<int>(ROW_H * app.dpiScale);
    m_list.Clamp();
}

void RemoteCmdWnd::BuildButtons() {
    m_buttons.clear();
    m_buttons.push_back({L"Send",  BTN_SEND,  {}, Selected() != nullptr});
    m_buttons.push_back({L"Clear", BTN_CLEAR, {}, true});
    m_buttons.push_back({L"Log",   BTN_LOG,   {}, true});
    m_buttons.push_back({L"Close", BTN_CLOSE, {}, true});
}

const RemoteCmdWnd::Command *RemoteCmdWnd::Selected() const {
    if (m_selected < 0 || m_selected >= static_cast<int>(m_shown.size())) return nullptr;
    return &m_shown[static_cast<size_t>(m_selected)];
}

// =============================================================================
// Actions
// =============================================================================
void RemoteCmdWnd::MoveSelection(int delta) {
    if (m_shown.empty()) return;
    m_selected = std::clamp(m_selected + delta, 0, static_cast<int>(m_shown.size()) - 1);
    EnsureSelectionVisible();
    BuildButtons();
    Repaint();
}

void RemoteCmdWnd::EnsureSelectionVisible() {
    const int rowH = static_cast<int>(ROW_H * app.dpiScale);
    const int top  = m_selected * rowH;
    const int bot  = top + rowH;
    const int viewH = RectH(m_list.view);

    // Only when it would actually leave the view — pulling the highlight to the
    // middle on every arrow press makes the list lurch.
    if (top < m_list.scrollY)              m_list.scrollY = top;
    else if (bot > m_list.scrollY + viewH) m_list.scrollY = bot - viewH;
    m_list.Clamp();
}

void RemoteCmdWnd::TakeHighlighted() {
    const Command *c = Selected();
    if (!c) return;

    // Needs a value → hand the caret to the value box. Takes none → there is
    // nothing left to decide, so send it. One keypress for the common case.
    if (c->needsValue) {
        m_focus = Focus::Value;
        Repaint();
    } else {
        DoSend();
    }
}

void RemoteCmdWnd::DoSend() {
    const Command *c = Selected();
    if (!c) {
        m_status = L"Pick a command from the list first.";
        Repaint();
        return;
    }

    const std::wstring value = m_valueBox.GetText();

    // Refused HERE rather than let the far end answer "ERR payload required":
    // that answer arrives once per target, so one mistake made on this keyboard
    // would produce one error line per screen.
    if (c->needsValue && value.empty()) {
        m_status = L"`" + c->name + L"` needs a value.";
        m_focus  = Focus::Value;
        Repaint();
        return;
    }

    const std::wstring line = (c->needsValue || !value.empty())
                                  ? (c->name + L" " + value)
                                  : c->name;

    // NOT cleared. The log keeps the whole session — see AddLogRow.
    //
    // The ticks in THIS panel's Send-to box decide the recipients — not the
    // Ctrl+F11 Control selection, which decides where keystrokes go. Refreshed
    // first, so a screen that dropped a moment ago is not counted as reached.
    RefreshTargets();
    m_awaiting = Remote::Mirror::SendToIds(line, CheckedTargetIds(), GetHwnd());

    // The SENT line goes in first, so the answers that follow have something to be
    // answers to. Without it the log was a column of replies to a command you had
    // to remember typing.
    AddLogRow({0, /*outbound=*/true,
               m_awaiting == 1 ? std::wstring(L"1 instance")
                               : (std::to_wstring(m_awaiting) + L" instances"),
               line, m_awaiting > 0 || m_alsoLocal, -1});

    if (m_alsoLocal) {
        // The headless path, not the bare sink: the payload commands raise
        // panels and dialogs through InputManager::ExecuteCommand and would hold
        // this press open until somebody dismissed a window. ExecutePayload is
        // the same body the wire uses — "also run it here" must mean exactly
        // what it means over there.
        std::wstring reply;
        Remote::RemoteRequest req = Remote::ParseLine(line);
        if (req.status != Remote::ParseStatus::Ok) {
            AddLogRow({0, false, L"(this instance)",
                       L"ERR this build cannot run that locally", false, -1});
        } else if (Remote::ExecutePayload(m_hParent, req.cmd, req.payload, reply)) {
            AddLogRow({0, false, L"(this instance)", reply, true, -1});
        } else {
            InputManager::ExecuteCommand(m_hParent, req.cmd);
            AddLogRow({0, false, L"(this instance)", L"OK", true, -1});
        }
    }

    m_status =
        (m_awaiting == 0 && !m_alsoLocal)
            ? (m_targetRows.empty()
                   ? std::wstring(L"Nothing to send to — no instance is connected "
                                  L"(connect one in F10).")
                   : std::wstring(L"Nothing to send to — no instance is ticked in "
                                  L"Send to."))
            : (L"Sent `" + line + L"` to " + std::to_wstring(m_awaiting) +
               L" instance(s)" + (m_alsoLocal ? L" and this one" : L""));
    Repaint();
}

void RemoteCmdWnd::DoClear() {
    m_filterBox.Clear();
    m_valueBox.Clear();
    // The one place the log is emptied — and the numbering restarts at 1 with it.
    // Clear here means "start a fresh session", so the first line of that session is
    // line 1; carrying the old count on would leave a number nothing on screen
    // explains.
    m_replies.clear();
    m_nextSeq = 1;
    m_awaiting = 0;
    m_status.clear();
    m_log.scrollY = 0;
    m_focus = Focus::Filter;
    ApplyFilter();
    BuildButtons();
    Repaint();
}

// =============================================================================
// Hit tests
// =============================================================================
int RemoteCmdWnd::HitTestButton(POINT pt) const {
    for (size_t i = 0; i < m_buttons.size(); ++i)
        if (m_buttons[i].enabled && PtInRect(&m_buttons[i].rect, pt))
            return static_cast<int>(i);
    return -1;
}

int RemoteCmdWnd::HitTestCommandRow(POINT pt) const {
    if (!PtInRect(&m_list.view, pt)) return -1;
    const int rowH = static_cast<int>(ROW_H * app.dpiScale);
    const int idx  = (pt.y - m_list.view.top + m_list.scrollY) / rowH;
    return (idx >= 0 && idx < static_cast<int>(m_shown.size())) ? idx : -1;
}

int RemoteCmdWnd::HitTestTargetRow(POINT pt) const {
    if (!PtInRect(&m_targets.view, pt)) return -1;
    const int rowH = static_cast<int>(ROW_H * app.dpiScale);
    const int idx  = (pt.y - m_targets.view.top + m_targets.scrollY) / rowH;
    return (idx >= 0 && idx < static_cast<int>(m_targetRows.size())) ? idx : -1;
}

// =============================================================================
// Keyboard
// =============================================================================
bool RemoteCmdWnd::OnKeyDown(WPARAM vk, bool /*ctrl*/, bool /*shift*/, bool /*alt*/) {
    InputBox &box = (m_focus == Focus::Filter) ? m_filterBox : m_valueBox;

    switch (vk) {
        case VK_TAB:
            m_focus = (m_focus == Focus::Filter) ? Focus::Value : Focus::Filter;
            Repaint();
            return true;

        // The list is always on screen, so the arrows always drive it — whichever
        // box has the caret. Nothing else in this panel wants them.
        case VK_UP:    MoveSelection(-1); return true;
        case VK_DOWN:  MoveSelection(+1); return true;
        case VK_PRIOR: MoveSelection(-8); return true;
        case VK_NEXT:  MoveSelection(+8); return true;

        case VK_RETURN:
            if (m_focus == Focus::Filter) TakeHighlighted();
            else                          DoSend();
            return true;

        default:
            break;
    }

    const InputResult r = box.RouteKey(vk, GetHwnd());
    if (r != InputResult::Ignored) {
        if (m_focus == Focus::Filter) ApplyFilter();
        if (r == InputResult::ConsumedRepaint || r == InputResult::RequestClear) {
            BuildButtons();
            Repaint();
        }
        // RequestClose is an empty box taking Esc — OnLocalHide decides that, so
        // the filter and the panel close in the right order.
        return r != InputResult::RequestClose;
    }
    return false; // unhandled → the app pipeline, per FloatingPanelWnd
}

bool RemoteCmdWnd::OnLocalHide() {
    // Esc clears what is typed before it closes anything — one keypress, one
    // decision.
    if (!m_filterBox.GetText().empty() || !m_valueBox.GetText().empty()) {
        DoClear();
        return true;
    }
    return false;
}

// =============================================================================
// Message handling
// =============================================================================
LRESULT RemoteCmdWnd::HandlePanelMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case Constants::WM_QIV_REMOTE_CMD_REPLY: {
            auto *r = reinterpret_cast<Remote::Mirror::CmdReply *>(lParam);
            if (!r) return 0;
            // Tail-following and the row cap both live in AddLogRow, so an answer
            // and a send are recorded by the same rules.
            AddLogRow({0, /*outbound=*/false, r->target, r->reply, r->ok, r->deltaUs});
            if (m_awaiting > 0) --m_awaiting;
            delete r;
            Repaint();
            return 0;
        }

        case WM_CHAR: {
            InputBox &box = (m_focus == Focus::Filter) ? m_filterBox : m_valueBox;
            if (box.RouteChar(static_cast<wchar_t>(wParam), GetHwnd()) ==
                InputResult::ConsumedRepaint) {
                if (m_focus == Focus::Filter) ApplyFilter();
                BuildButtons();
                Repaint();
            }
            return 0;
        }

        case WM_SETCURSOR: {
            if (LOWORD(lParam) != HTCLIENT) break;
            POINT pt; GetCursorPos(&pt);
            ScreenToClient(GetHwnd(), &pt);
            const bool hot = HitTestButton(pt) >= 0 || HitTestCommandRow(pt) >= 0 ||
                             HitTestTargetRow(pt) >= 0 ||
                             PtInRect(&m_targetsAllRect, pt) ||
                             PtInRect(&m_targetsNoneRect, pt) ||
                             PtInRect(&m_localRect, pt) ||
                             PtInRect(&m_filterRect, pt) || PtInRect(&m_valueRect, pt);
            SetCursor(hot ? Constants::Cursors::CURR_CLICK
                          : Constants::Cursors::CURR_DEFAULT);
            return TRUE;
        }

        // A target connected, dropped, or changed why it is down. The recipients box
        // is the part of this panel that goes stale otherwise — it offers a tick per
        // connected instance, and a tick for a screen that has gone is a promise the
        // send cannot keep.
        case Constants::WM_QIV_REMOTE_TARGETS_CHANGED:
            // BEFORE the re-read, so a change landing during it posts again rather
            // than finding the gate closed and being dropped — the same ordering the
            // F10 console uses, and for the same reason.
            Remote::Mirror::ClearPanelNotifyPending(GetHwnd());
            RefreshTargets();
            Repaint();
            return 0;

        // Belt and braces alongside Hide(): a window can also go away without being
        // hidden first.
        case WM_DESTROY:
            Remote::Mirror::RemovePanelNotify(GetHwnd());
            break;

        case WM_MOUSEMOVE: {
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};

            if (m_drag != Drag::None) {
                ScrollView &sv = (m_drag == Drag::List)    ? m_list
                               : (m_drag == Drag::Targets) ? m_targets
                                                           : m_log;
                const int travel = std::max(1, RectH(sv.track) - m_dragThumbSpan);
                const int want   = pt.y - sv.track.top - m_dragGrabPx;
                sv.scrollY = MulDiv(std::clamp(want, 0, travel), sv.MaxScroll(), travel);
                sv.Clamp();
                Repaint();
                return 0;
            }

            const int b = HitTestButton(pt);
            const bool lh = PtInRect(&m_list.thumb, pt) != 0;
            const bool gh = PtInRect(&m_log.thumb, pt) != 0;
            if (b != m_hotButton || lh != m_list.thumbHot || gh != m_log.thumbHot) {
                m_hotButton = b; m_list.thumbHot = lh; m_log.thumbHot = gh;
                Repaint();
            }
            return 0;
        }

        case WM_LBUTTONUP:
            if (m_drag != Drag::None) { m_drag = Drag::None; ReleaseCapture(); Repaint(); }
            else {
                InputBox &box = (m_focus == Focus::Filter) ? m_filterBox : m_valueBox;
                if (box.RouteMouse(message, wParam, lParam, GetHwnd()) ==
                    InputResult::ConsumedRepaint)
                    Repaint();
            }
            return 0;

        // A drag can end without the button coming up — Alt+Tab, a message box.
        case WM_CAPTURECHANGED:
            m_drag = Drag::None;
            Repaint();
            return 0;

        case WM_MOUSEWHEEL: {
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(GetHwnd(), &pt);
            const int rowH = static_cast<int>(ROW_H * app.dpiScale);
            // Whichever list the pointer is over. Defaulting to the commands is
            // right when it is over neither: that is the list you are reading.
            ScrollView &sv = PtInRect(&m_log.view, pt)     ? m_log
                           : PtInRect(&m_targets.view, pt) ? m_targets
                                                           : m_list;
            sv.ScrollBy(-(GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA) * rowH * 3);
            Repaint();
            return 0;
        }

        case WM_LBUTTONDOWN: {
            SetFocus(GetHwnd());
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};

            // Scrollbars first — they overlap nothing, but they are the smallest
            // targets and must not be stolen by a list row.
            {
                ScrollView *svs[3]   = {&m_list, &m_log, &m_targets};
                const Drag  which[3] = {Drag::List, Drag::Log, Drag::Targets};
                for (int i = 0; i < 3; ++i) {
                    ScrollView *sv = svs[i];
                    if (PtInRect(&sv->thumb, pt)) {
                        m_drag          = which[i];
                        m_dragGrabPx    = pt.y - sv->thumb.top;
                        m_dragThumbSpan = RectH(sv->thumb);
                        SetCapture(GetHwnd());
                        return 0;
                    }
                    if (PtInRect(&sv->track, pt)) {
                        sv->ScrollBy(pt.y < sv->thumb.top ? -RectH(sv->view)
                                                          : RectH(sv->view));
                        Repaint();
                        return 0;
                    }
                }
            }

            const int row = HitTestCommandRow(pt);
            if (row >= 0) {
                m_selected = row;

                // The name goes INTO the box — it is the command box as much as it is
                // the filter — but the filter is deliberately NOT re-run: narrowing
                // the list to the one row just clicked throws away the browsing
                // position, and browsing is what the list is for.
                //
                // Whole text selected, so the next character typed replaces a name
                // the user did not type rather than appending to it.
                if (const Command *c = Selected()) {
                    m_filterBox.SetText(c->name);
                    m_filterBox.SelectAll();
                }

                // Selecting is not sending. The list is a browser, and a click
                // that fired a command at every screen would make it a minefield.
                if (Selected() && Selected()->needsValue) m_focus = Focus::Value;
                BuildButtons();
                Repaint();
                return 0;
            }

            const int b = HitTestButton(pt);
            if (b >= 0) {
                switch (m_buttons[b].id) {
                    case BTN_SEND:  DoSend();  break;
                    case BTN_CLEAR: DoClear(); break;
                    case BTN_LOG: {
                        // Open it, or close it if it is open — and when opening,
                        // actually put it in FRONT. Every panel here is
                        // WS_EX_TOPMOST, so re-showing one already visible leaves it
                        // wherever it sat in that band, which is usually behind this
                        // window. The HWND_TOPMOST re-assert is what reorders it
                        // within the band. (RemoteLogWnd.cpp does the same for the
                        // button pointing back here.)
                        UI::IPanelWindow &log = uiManager.getRemoteLogWindow();
                        if (log.IsVisible()) {
                            log.Hide();
                        } else {
                            log.Show();
                            if (HWND h = log.GetHwnd())
                                SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0,
                                             SWP_NOMOVE | SWP_NOSIZE);
                        }
                        // Its lit state just changed, and opening another window
                        // takes the focus, so nothing else would repaint this.
                        Repaint();
                        break;
                    }
                    case BTN_CLOSE: Hide();    break;
                    default: break;
                }
                return 0;
            }

            if (PtInRect(&m_localRect, pt)) {
                m_alsoLocal = !m_alsoLocal;
                Repaint();
                return 0;
            }

            // --- Recipients ------------------------------------------------
            // The WHOLE ROW toggles, not just the box: the row is the thing being
            // included or left out, and a 14 px checkbox is a poor target on a
            // touchpad. Nothing else on a row is clickable, so there is no second
            // meaning to collide with.
            {
                const int tr = HitTestTargetRow(pt);
                if (tr >= 0) {
                    const int id = m_targetRows[static_cast<size_t>(tr)].id;
                    auto it = std::find(m_untickedIds.begin(), m_untickedIds.end(), id);
                    if (it == m_untickedIds.end()) m_untickedIds.push_back(id);
                    else                           m_untickedIds.erase(it);
                    Repaint();
                    return 0;
                }
                if (!m_targetRows.empty() && PtInRect(&m_targetsAllRect, pt)) {
                    m_untickedIds.clear();
                    Repaint();
                    return 0;
                }
                if (!m_targetRows.empty() && PtInRect(&m_targetsNoneRect, pt)) {
                    m_untickedIds.clear();
                    for (const TargetRow &t : m_targetRows) m_untickedIds.push_back(t.id);
                    Repaint();
                    return 0;
                }
            }

            if (PtInRect(&m_filterRect, pt))      m_focus = Focus::Filter;
            else if (PtInRect(&m_valueRect, pt))  m_focus = Focus::Value;

            {
                InputBox &box = (m_focus == Focus::Filter) ? m_filterBox : m_valueBox;
                box.RouteMouse(message, wParam, lParam, GetHwnd());
            }
            Repaint();
            return 0;
        }

        case WM_RBUTTONDOWN: {
            InputBox &box = (m_focus == Focus::Filter) ? m_filterBox : m_valueBox;
            if (box.RouteMouse(message, wParam, lParam, GetHwnd()) ==
                InputResult::ConsumedRepaint)
                Repaint();
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

            const COLORREF bg    = GetBgColor();
            const bool     dark  = BgIsDark(bg);
            const COLORREF fg    = dark ? RGB(235,235,235) : RGB(24,24,24);
            const COLORREF dim   = dark ? RGB(150,150,150) : RGB(110,110,110);
            const COLORREF selBg = dark ? RGB(58,86,132)   : RGB(203,222,250);
            const COLORREF altBg = dark ? RGB(42,42,46)    : RGB(246,246,248);
            const COLORREF boxBg = dark ? RGB(30,30,34)    : RGB(252,252,253);
            const COLORREF line  = dark ? RGB(64,64,64)    : RGB(220,220,220);

            FillRect(bb, &rc, Gdi::Brush(bg));
            SetBkMode(bb, TRANSPARENT);

            const float s   = app.dpiScale;
            const int pad   = static_cast<int>(PAD * s);
            const int fldH  = static_cast<int>(FIELD_H * s);
            const int rowH  = static_cast<int>(ROW_H * s);
            const int hdrH  = static_cast<int>(HDR_H * s);
            const int btnH  = static_cast<int>(BTN_H * s);
            const int listW = static_cast<int>(LIST_W * s);
            const int logH  = static_cast<int>(LOG_H * s);
            const int gap   = static_cast<int>(16 * s);

            const int rightX   = pad + listW + gap;
            const int listBot  = H - pad - logH - static_cast<int>(22 * s);

            // ── Row 1: the two DESCRIPTIONS, side by side ────────────────────
            //
            // The panel's title and its key-hint line used to live here. Both were
            // text you read once: the window is called Send Command, and the keys
            // are in Help. What you want in their place is what the highlighted
            // command DOES, and what its value MEANS — one over each of the two
            // boxes below, on one line, so the pairing is visible rather than
            // remembered.
            //
            // Fixed height for both cells, so the boxes underneath stay level
            // whichever description happens to be longer. Anything past three lines
            // is ellipsised — the list on the left shows the same text, and the
            // panel is resizable.
            const int descTop = static_cast<int>(8 * s);
            const int descH   = static_cast<int>(DESC_H * s);
            const int top     = descTop + descH + static_cast<int>(6 * s);
            {
                const Command *c = Selected();

                SelectObject(bb, m_hFontSmall);

                // LEFT — what the command does. Its NAME is not repeated here: the
                // list shows it, highlighted, two lines down.
                SetTextColor(bb, c ? fg : dim);
                RECT cd{pad, descTop, pad + listW, descTop + descH};
                DrawTextW(bb,
                          !c ? L"Pick a command from the list below."
                             : (c->desc.empty() ? c->name.c_str() : c->desc.c_str()),
                          -1, &cd, DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);

                // RIGHT — what its value means, aligned with the left cell and
                // sitting over the Value box it describes.
                SetTextColor(bb, (c && c->needsValue) ? fg : dim);
                RECT vd{rightX, descTop, W - pad, descTop + descH};
                DrawTextW(bb,
                          !c ? L""
                             : !c->needsValue
                                   ? L"This command takes no value."
                                   : (c->valueDesc.empty() ? L"Takes a value."
                                                           : c->valueDesc.c_str()),
                          -1, &vd, DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);
            }

            // ── Left: filter + the command list ──────────────────────────────
            {
                SelectObject(bb, m_hFontSmall);
                SetTextColor(bb, PC::HEADER);
                RECT fl{pad, top, pad + listW, top + hdrH};
                DrawTextW(bb, L"Filter", -1, &fl, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

                m_filterRect = {pad, top + hdrH, pad + listW, top + hdrH + fldH};
                FrameBox(bb, m_filterRect, boxBg, line);
                m_filterBox.Draw(bb, m_hFontBody, m_filterRect, static_cast<int>(6 * s),
                                 GetFocus() == GetHwnd() && m_focus == Focus::Filter);

                const int listTop = m_filterRect.bottom + static_cast<int>(10 * s);

                SetTextColor(bb, PC::HEADER);
                SelectObject(bb, m_hFontSmall);
                RECT cl{pad, listTop, pad + listW, listTop + hdrH};
                const std::wstring lh = L"Commands  (" + std::to_wstring(m_shown.size()) +
                                        L" of " + std::to_wstring(m_all.size()) + L")";
                DrawTextW(bb, lh.c_str(), -1, &cl, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

                // The key to the two blues, in the second blue itself — a colour code
                // nothing explains is a colour code nobody reads.
                SetTextColor(bb, PC::PATH_ALT);
                RECT cll = cl;
                DrawTextW(bb, L"takes a value", -1, &cll,
                          DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

                RECT frame{pad, listTop + hdrH, pad + listW, listBot};
                FrameBox(bb, frame, boxBg, line);

                const int sbW = static_cast<int>(Constants::Dedicated::PANEL_SCROLLBAR_W * s);
                m_list.contentH = static_cast<int>(m_shown.size()) * rowH;
                const bool needBar = m_list.contentH > RectH(frame) - 2;
                m_list.view = {frame.left + 1, frame.top + 1,
                               frame.right - 1 - (needBar ? sbW : 0), frame.bottom - 1};
                m_list.Clamp();

                const int saved = SaveDC(bb);
                IntersectClipRect(bb, m_list.view.left, m_list.view.top,
                                  m_list.view.right, m_list.view.bottom);

                const int first = std::max(0, m_list.scrollY / rowH);
                const int last  = std::min(static_cast<int>(m_shown.size()),
                                           first + RectH(m_list.view) / rowH + 2);
                for (int i = first; i < last; ++i) {
                    const Command &c = m_shown[static_cast<size_t>(i)];
                    const int ry = m_list.view.top + i * rowH - m_list.scrollY;
                    RECT rr{m_list.view.left, ry, m_list.view.right, ry + rowH};

                    if (i == m_selected)   FillRect(bb, &rr, Gdi::Brush(selBg));
                    else if (i % 2)        FillRect(bb, &rr, Gdi::Brush(altBg));

                    // THE NAME ONLY. The description used to share this row and was
                    // clipped to a few words by the column width, which made it a
                    // teaser rather than an explanation — and it is already stated in
                    // full above the box, for whichever row is highlighted. One
                    // sentence, one place.
                    //
                    // TWO BLUES: whether a command takes a VALUE is the one thing you
                    // want to know before picking it, because it decides whether the
                    // Value box has to be filled in. Carried by colour rather than by a
                    // second column, which is what the row is now wide enough for.
                    SelectObject(bb, m_hFontBody);
                    SetTextColor(bb, c.needsValue ? PC::PATH_ALT : PC::PATH);
                    RECT nr{rr.left + static_cast<int>(8 * s), ry,
                            rr.right - static_cast<int>(6 * s), ry + rowH};
                    DrawTextW(bb, c.name.c_str(), -1, &nr,
                              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                }
                RestoreDC(bb, saved);

                m_list.track = {};
                m_list.thumb = {};
                if (needBar) {
                    m_list.track = {frame.right - 1 - sbW, m_list.view.top,
                                    frame.right - 1, m_list.view.bottom};
                    DrawScrollBar(bb, m_list, s, PC::SCROLL_TRACK, PC::SCROLL_THUMB,
                                  PC::SCROLL_THUMB_HOT, m_drag == Drag::List);
                }
            }

            // ── Right: the Value box, LEVEL with the filter box, then Send ────
            {
                int y = top;
                const Command *c = Selected();

                // The caption carries the command NAME, so what is armed sits
                // directly over the box you are typing its value into — the pane no
                // longer needs a heading of its own to say it.
                SelectObject(bb, m_hFontSmall);
                SetTextColor(bb, PC::HEADER);
                RECT vl{rightX, y, W - pad, y + hdrH};
                DrawTextW(bb,
                          !c ? L"Value"
                             : (c->needsValue ? (c->name + L"  <value>").c_str()
                                              : c->name.c_str()),
                          -1, &vl, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                y += hdrH;

                m_valueRect = {rightX, y, W - pad, y + fldH};
                FrameBox(bb, m_valueRect, boxBg, line);
                m_valueBox.Draw(bb, m_hFontBody, m_valueRect, static_cast<int>(6 * s),
                                GetFocus() == GetHwnd() && m_focus == Focus::Value);
                y += fldH + static_cast<int>(14 * s);

                const int gapB = static_cast<int>(BTN_GAP * s);
                const int bw   = static_cast<int>(92 * s);
                int x = rightX;
                for (Button &btn : m_buttons) {
                    btn.rect = {x, y, x + bw, y + btnH};
                    const int myIndex = static_cast<int>(&btn - m_buttons.data());

                    // Send is accented because it is the action. Log is accented
                    // while the panel it toggles is open — a toggle whose lit state
                    // tracked anything else would lie about its own press.
                    COLORREF base = (btn.id == BTN_SEND) ? PC::BTN_ALT
                                  : (btn.id == BTN_LOG &&
                                     uiManager.getRemoteLogWindow().IsVisible())
                                        ? PC::BTN_ALT
                                        : PC::BTN_MAIN;
                    if (!btn.enabled) base = bg;
                    else if (myIndex == m_hotButton)
                        base = RGB(std::min(255, GetRValue(base) + 40),
                                   std::min(255, GetGValue(base) + 40),
                                   std::min(255, GetBValue(base) + 40));

                    FrameBox(bb, btn.rect, base, line);
                    SelectObject(bb, m_hFontSmall);
                    SetTextColor(bb, btn.enabled ? RGB(245,245,245) : dim);
                    RECT lr = btn.rect;
                    DrawTextW(bb, btn.label.c_str(), -1, &lr,
                              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    x += bw + gapB;
                }
                y += btnH + static_cast<int>(10 * s);

                // Under Send, because it changes what Send does.
                m_localRect = {rightX, y, W - pad, y + static_cast<int>(22 * s)};
                SelectObject(bb, m_hFontBody);
                SetTextColor(bb, m_alsoLocal ? PC::ON : dim);
                RECT cr = m_localRect;
                DrawTextW(bb, m_alsoLocal ? L"☑  also run it here"
                                          : L"☐  also run it here",
                          -1, &cr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                y = m_localRect.bottom + static_cast<int>(12 * s);

                // ── Recipients: WHICH connected instances this send reaches ────
                //
                // Its own selection, deliberately not the Ctrl+F11 Control ticks.
                // Those decide where KEYSTROKES go; this decides where one typed
                // line goes, and lining a screen up by hand should not change what
                // F11 drives afterwards.
                //
                // The same box as the command list, on purpose: a checkbox, a name,
                // an address. Everything connected is ticked to begin with, because
                // "send to whatever is up" is the normal case — the state held is
                // the EXCLUSIONS (see m_untickedIds).
                {
                    const std::vector<int> checked = CheckedTargetIds();

                    SelectObject(bb, m_hFontSmall);
                    SetTextColor(bb, PC::HEADER);
                    RECT th{rightX, y, W - pad, y + static_cast<int>(TARGETS_HDR_H * s)};
                    const std::wstring hdr =
                        L"Send to  (" + std::to_wstring(checked.size()) + L" of " +
                        std::to_wstring(m_targetRows.size()) + L" connected)";
                    DrawTextW(bb, hdr.c_str(), -1, &th,
                              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

                    // All / None, right-aligned on the header line. Two words rather
                    // than buttons: they belong to the list, not to the row of
                    // actions above, and a wall of eight screens should not need
                    // eight clicks to exclude one.
                    const int allW  = static_cast<int>(34 * s);
                    const int noneW = static_cast<int>(44 * s);
                    m_targetsNoneRect = {W - pad - noneW, th.top, W - pad, th.bottom};
                    m_targetsAllRect  = {m_targetsNoneRect.left - static_cast<int>(6 * s) - allW,
                                         th.top,
                                         m_targetsNoneRect.left - static_cast<int>(6 * s),
                                         th.bottom};
                    SetTextColor(bb, m_targetRows.empty() ? dim : PC::PATH);
                    RECT ar = m_targetsAllRect, nrr = m_targetsNoneRect;
                    DrawTextW(bb, L"All",  -1, &ar,  DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
                    DrawTextW(bb, L"None", -1, &nrr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

                    RECT frame{rightX, th.bottom, W - pad, listBot};
                    FrameBox(bb, frame, boxBg, line);

                    const int sbW =
                        static_cast<int>(Constants::Dedicated::PANEL_SCROLLBAR_W * s);
                    m_targets.contentH = static_cast<int>(m_targetRows.size()) * rowH;
                    const bool needBar = m_targets.contentH > RectH(frame) - 2;
                    m_targets.view = {frame.left + 1, frame.top + 1,
                                      frame.right - 1 - (needBar ? sbW : 0),
                                      frame.bottom - 1};
                    m_targets.Clamp();

                    const int saved = SaveDC(bb);
                    IntersectClipRect(bb, m_targets.view.left, m_targets.view.top,
                                      m_targets.view.right, m_targets.view.bottom);

                    if (m_targetRows.empty()) {
                        SelectObject(bb, m_hFontSmall);
                        SetTextColor(bb, dim);
                        RECT er{m_targets.view.left + static_cast<int>(8 * s),
                                m_targets.view.top + static_cast<int>(6 * s),
                                m_targets.view.right, m_targets.view.top + rowH * 2};
                        // Says what to DO about it. "No instances" alone leaves the
                        // reader looking for the thing that is broken.
                        DrawTextW(bb, L"Nothing connected — connect a server in F10.",
                                  -1, &er, DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);
                    }

                    const int firstT = std::max(0, m_targets.scrollY / rowH);
                    const int lastT  = std::min(static_cast<int>(m_targetRows.size()),
                                                firstT + RectH(m_targets.view) / rowH + 2);
                    for (int i = firstT; i < lastT; ++i) {
                        const TargetRow &t = m_targetRows[static_cast<size_t>(i)];
                        const int ry = m_targets.view.top + i * rowH - m_targets.scrollY;
                        RECT rr{m_targets.view.left, ry, m_targets.view.right, ry + rowH};
                        if (i % 2) FillRect(bb, &rr, Gdi::Brush(altBg));

                        const bool ticked =
                            std::find(checked.begin(), checked.end(), t.id) != checked.end();

                        SelectObject(bb, m_hFontBody);
                        SetTextColor(bb, ticked ? PC::ON : dim);
                        RECT kr{rr.left + static_cast<int>(8 * s), ry,
                                rr.left + static_cast<int>(30 * s), ry + rowH};
                        DrawTextW(bb, ticked ? L"☑" : L"☐", -1, &kr,
                                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);

                        // Dimmed when unticked, so a glance at the box says where the
                        // next Send lands without reading every checkbox.
                        SetTextColor(bb, ticked ? PC::TEXT : dim);
                        RECT nrw{rr.left + static_cast<int>(34 * s), ry,
                                 rr.left + static_cast<int>(170 * s), ry + rowH};
                        DrawTextW(bb,
                                  t.name.empty() ? L"(unnamed)" : t.name.c_str(), -1, &nrw,
                                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

                        SelectObject(bb, m_hFontSmall);
                        SetTextColor(bb, ticked ? PC::PATH : dim);
                        RECT hr{rr.left + static_cast<int>(176 * s), ry,
                                rr.right - static_cast<int>(6 * s), ry + rowH};
                        const std::wstring addr = t.host + L":" + std::to_wstring(t.port);
                        DrawTextW(bb, addr.c_str(), -1, &hr,
                                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                    }
                    RestoreDC(bb, saved);

                    m_targets.track = {};
                    m_targets.thumb = {};
                    if (needBar) {
                        m_targets.track = {frame.right - 1 - sbW, m_targets.view.top,
                                           frame.right - 1, m_targets.view.bottom};
                        DrawScrollBar(bb, m_targets, s, PC::SCROLL_TRACK, PC::SCROLL_THUMB,
                                      PC::SCROLL_THUMB_HOT, m_drag == Drag::Targets);
                    }
                }
            }

            // ── Bottom: status + the session LOG ─────────────────────────────
            {
                const int stY = listBot + static_cast<int>(4 * s);
                SelectObject(bb, m_hFontSmall);
                SetTextColor(bb, m_status.empty() ? dim : fg);
                RECT st{pad, stY, W - pad, stY + static_cast<int>(16 * s)};
                const std::wstring line2 =
                    m_status.empty()
                        ? std::wstring(L"Nothing sent yet.")
                        : (m_awaiting > 0
                               ? m_status + L"   ·   waiting for " +
                                     std::to_wstring(m_awaiting) + L"…"
                               : m_status);
                DrawTextW(bb, line2.c_str(), -1, &st,
                          DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

                // Row count on the right of the same line — it is the log's header,
                // and a log that does not say how much it is holding gives no clue
                // that it has started dropping its oldest rows.
                if (!m_replies.empty()) {
                    SetTextColor(bb, dim);
                    RECT cnt = st;
                    const std::wstring hdr =
                        L"Log  " + std::to_wstring(m_replies.size()) +
                        (m_replies.size() >= LOG_MAX_ROWS
                             ? std::wstring(L" (oldest dropping)")
                             : std::wstring());
                    DrawTextW(bb, hdr.c_str(), -1, &cnt,
                              DT_RIGHT | DT_SINGLELINE | DT_END_ELLIPSIS);
                }

                RECT frame{pad, H - pad - logH, W - pad, H - pad};
                FrameBox(bb, frame, boxBg, line);

                const int sbW = static_cast<int>(Constants::Dedicated::PANEL_SCROLLBAR_W * s);
                m_log.contentH = static_cast<int>(m_replies.size()) * rowH;
                const bool needBar = m_log.contentH > RectH(frame) - 2;
                m_log.view = {frame.left + 1, frame.top + 1,
                              frame.right - 1 - (needBar ? sbW : 0), frame.bottom - 1};
                m_log.Clamp();

                const int saved = SaveDC(bb);
                IntersectClipRect(bb, m_log.view.left, m_log.view.top,
                                  m_log.view.right, m_log.view.bottom);

                const int first = std::max(0, m_log.scrollY / rowH);
                const int last  = std::min(static_cast<int>(m_replies.size()),
                                           first + RectH(m_log.view) / rowH + 2);
                // Five columns, the same reading order as the Ctrl+F12 wire log:
                // number, direction, who, what, how long. Both directions share one
                // table in the order things actually happened, because a typed
                // session is a conversation and splitting it into "sent" and
                // "received" halves loses which answer belongs to which send.
                for (int i = first; i < last; ++i) {
                    const Reply &r = m_replies[static_cast<size_t>(i)];
                    const int ry = m_log.view.top + i * rowH - m_log.scrollY;
                    RECT rowRc{m_log.view.left, ry, m_log.view.right, ry + rowH};

                    // The SENT rows are shaded, not the alternate ones: banding by
                    // parity in a two-direction log fights the thing you actually
                    // scan for, which is where each exchange starts.
                    if (r.outbound) FillRect(bb, &rowRc, Gdi::Brush(altBg));

                    SelectObject(bb, m_hFontSmall);
                    SetTextColor(bb, dim);
                    RECT sr{m_log.view.left + static_cast<int>(6 * s), ry,
                            m_log.view.left + static_cast<int>(44 * s), ry + rowH};
                    DrawTextW(bb, std::to_wstring(r.seq).c_str(), -1, &sr,
                              DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

                    // Direction, as an arrow rather than a word: it is read once per
                    // row and a column of "sent"/"reply" is wider and slower.
                    SetTextColor(bb, r.outbound ? PC::NUMBER : (r.ok ? PC::ON : PC::WARN));
                    RECT ar{m_log.view.left + static_cast<int>(52 * s), ry,
                            m_log.view.left + static_cast<int>(72 * s), ry + rowH};
                    DrawTextW(bb, r.outbound ? L"→" : L"←", -1, &ar,
                              DT_LEFT | DT_VCENTER | DT_SINGLELINE);

                    SelectObject(bb, m_hFontBody);
                    SetTextColor(bb, PC::TEXT);
                    RECT tnr{m_log.view.left + static_cast<int>(74 * s), ry,
                             m_log.view.left + static_cast<int>(214 * s), ry + rowH};
                    DrawTextW(bb, r.target.c_str(), -1, &tnr,
                              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

                    // The command line, or the answer to one. An ERR is coloured on
                    // the way in, so a failed send does not need reading to be seen.
                    SetTextColor(bb, r.outbound ? fg : (r.ok ? PC::PATH : PC::WARN));
                    RECT vr{m_log.view.left + static_cast<int>(222 * s), ry,
                            m_log.view.right - static_cast<int>(76 * s), ry + rowH};
                    DrawTextW(bb, r.text.c_str(), -1, &vr,
                              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

                    if (r.deltaUs >= 0) {
                        SelectObject(bb, m_hFontSmall);
                        SetTextColor(bb, dim);
                        RECT tdr{m_log.view.right - static_cast<int>(72 * s), ry,
                                 m_log.view.right - static_cast<int>(8 * s), ry + rowH};
                        wchar_t b2[32];
                        swprintf_s(b2, L"%.1f ms", static_cast<double>(r.deltaUs) / 1000.0);
                        DrawTextW(bb, b2, -1, &tdr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
                    }
                }
                RestoreDC(bb, saved);

                m_log.track = {};
                m_log.thumb = {};
                if (needBar) {
                    m_log.track = {frame.right - 1 - sbW, m_log.view.top,
                                   frame.right - 1, m_log.view.bottom};
                    DrawScrollBar(bb, m_log, s, PC::SCROLL_TRACK, PC::SCROLL_THUMB,
                                  PC::SCROLL_THUMB_HOT, m_drag == Drag::Log);
                }
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

// =============================================================================
// Paint helpers
// =============================================================================
void RemoteCmdWnd::DrawScrollBar(HDC bb, ScrollView &sv, float s, COLORREF trackCol,
                                 COLORREF thumbCol, COLORREF thumbHotCol, bool dragging) {
    FillRect(bb, &sv.track, Gdi::Brush(trackCol));

    const int viewH = RectH(sv.view);
    const int minT  = static_cast<int>(Constants::Dedicated::PANEL_SCROLL_MIN_H * s);
    // Floored so it stays grabbable: proportional height against a long list is
    // a couple of pixels.
    int th = std::max(minT, MulDiv(viewH, viewH, std::max(1, sv.contentH)));
    th = std::min(th, viewH);

    const int travel = std::max(0, viewH - th);
    const int ty = sv.track.top + (sv.MaxScroll() > 0
                                       ? MulDiv(sv.scrollY, travel, sv.MaxScroll())
                                       : 0);

    sv.thumb = {sv.track.left + static_cast<int>(2 * s), ty,
                sv.track.right - static_cast<int>(2 * s), ty + th};
    FillRect(bb, &sv.thumb,
             Gdi::Brush((sv.thumbHot || dragging) ? thumbHotCol : thumbCol));
}

void RemoteCmdWnd::EnsureFonts(HDC dc) {
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
    // Monospaced for the wire text — command names and replies are protocol
    // tokens, and this is where you compare them character by character.
    m_hFontBody  = mk(10, FW_NORMAL,   L"Consolas");
    m_hFontBold  = mk(11, FW_SEMIBOLD, L"Segoe UI");
    m_hFontSmall = mk(8,  FW_NORMAL,   L"Segoe UI");
}

void RemoteCmdWnd::EnsureBackBuffer(HDC refDC, int w, int h) {
    if (m_bbDC && m_bbW == w && m_bbH == h) return;
    DestroyBackBuffer();
    m_bbDC     = CreateCompatibleDC(refDC);
    m_bbBmp    = CreateCompatibleBitmap(refDC, w, h);
    m_bbBmpOld = static_cast<HBITMAP>(SelectObject(m_bbDC, m_bbBmp));
    m_bbW = w; m_bbH = h;
}

void RemoteCmdWnd::DestroyBackBuffer() {
    if (!m_bbDC) return;
    SelectObject(m_bbDC, m_bbBmpOld);
    DeleteObject(m_bbBmp);
    DeleteDC(m_bbDC);
    m_bbDC = nullptr; m_bbBmp = nullptr; m_bbBmpOld = nullptr; m_bbW = m_bbH = 0;
}

void RemoteCmdWnd::Repaint() {
    if (GetHwnd()) InvalidateRect(GetHwnd(), nullptr, FALSE);
}

} // namespace UI
