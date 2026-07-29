#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include "UI/FloatingPanels/FloatingPanelWnd.h"
#include "RemoteLog.h"

// =============================================================================
// RemoteLogWnd (Ctrl+F12) — what the instances actually said to each other.
//
//   #    Sender     Command    Receiver   Response        Time          Δ time
//   417  Master     next       Monitor2   OK next=48/238  14:07:32.416  0.4 ms
//   418  10.0.0.5   goto 12    Master     OK goto=12/238  14:07:32.911  1.2 ms
//
// Δ time is the ROUND TRIP for a sent line (the target's reply came back this
// much later) and the HANDLING time for a received one (this instance took this
// long to do it). Both answer "who is slow", from the end that can tell.
//
// Both directions in ONE list, deliberately. A mirroring session is a
// conversation, and splitting it into an "out" table and an "in" table hides the
// only thing worth seeing: what this instance did in response to what it was
// told, in the order it happened.
//
// TWO LIVE COUNTS sit at the right of the button row, in space that was empty:
//
//   Connections  n / m   how many targets are CONNECTED, of how many are listed
//   Control      n / m   how many of the connected ones F11 actually DRIVES —
//                        the mirror ticks in Ctrl+F11
//
// They are also TOGGLE BUTTONS for the panels behind those numbers: pressing
// one opens the F10 console or the Ctrl+F11 panel and brings it to the front,
// pressing it again closes it. "2 / 5" is immediately followed by wanting to
// know which three are missing, and then by wanting the window out of the way
// again.
//
// Bringing it to the front needs an explicit HWND_TOPMOST re-assert, not just
// Show(): every panel in this app is WS_EX_TOPMOST, so they share one z-band,
// and re-showing a window that is already visible leaves it wherever it sat in
// that band — behind this log, which is exactly where it cannot be read.
//
// Their fill shows the TOGGLE state (is that panel open?), not the count. A
// toggle whose lit state tracked something other than what it toggles is a
// button that lies about its own press. The count instead speaks through the
// TEXT: dimmed digits mean zero.
//
// Their labels are built by the PAINTER, not by BuildButtons: a label baked in
// at rebuild time would freeze until the next log entry landed, and the whole
// value of a count is that it is current.
//
// RECORDING IS OFF BY DEFAULT (Constants::RemoteTcpIp::REMOTE_LOG_DEFAULT) and
// is switched on by the button in this panel — which also pushes the same switch
// to every connected instance, because a log of one end of a conversation
// answers half the question. Opening the panel does NOT start recording: looking
// at what was recorded and deciding to record are separate acts.
//
// SCROLLBARS ARE DRAWN, not WS_HSCROLL/WS_VSCROLL. The native ones were tried
// and are wrong here: they are non-client, so they ignore the panel's theme and
// render as white gutters on a dark window whatever DWM is told about the frame.
// Every other scrolling surface in this app draws its own (DedicatedWnd), using
// the same PC::SCROLL_TRACK / SCROLL_THUMB / SCROLL_THUMB_HOT and
// PANEL_SCROLLBAR_W — so this one does too, and they match.
//
// Both axes, because the seven columns are wider than the window at small sizes
// and the list is unbounded downward. Wheel, shift-wheel, thumb drag, click in
// the track to page, and the arrow/page keys all move the same pixel offsets.
//
// SORTING is on three columns only: #, Time and Δ time. Those are the questions the
// log is opened to answer ("what happened next", "what was slow"). The text
// columns are deliberately not sortable — grouping a conversation by sender
// destroys the ordering that makes it a conversation.
//
// The store itself is RemoteLog.h. This panel only ever holds a Snapshot, so a
// repaint never blocks a sender thread and the list can never move underneath
// the painter.
//
// NO REFRESH TIMER — unlike the other Rem_TCP_IP panels. Those poll because
// what they show (a target's live status) has no moment of change to hook. A
// log only grows when something adds to it, and the adder can say so: the panel
// registers with Remote::Log::SetNotifyWindow while it is visible and is sent
// WM_QIV_REMOTE_LOG_ADDED, coalesced at the source so a burst of mirrored
// keystrokes costs one message and one rebuild. A closed panel costs the
// producers one atomic load.
// =============================================================================

namespace UI {

class RemoteLogWnd;   // the detail window steps through ITS ordering — see below

// =============================================================================
// RemoteLogEntryWnd — ONE entry, every field on its own row, nothing trimmed.
//
// The list is a table, and a table ellipsises: a wire command with a path in it
// and a reply carrying a whole sync payload are both far wider than any column
// that still leaves room for six others. So the row you actually care about gets
// a window where each field is a labelled block that WRAPS instead of being cut.
//
// A satellite of the list, not a panel in its own right: no shortcut, no menu
// entry, reachable only by double-clicking (or pressing Enter on) a row. It
// lives in this file rather than its own pair because it has no meaning apart
// from the list that opens it.
//
// A SNAPSHOT of the entry, copied at open time. The store trims from the front
// as it fills, so holding a pointer into it would dangle the moment 20000 more
// exchanges went past — and the whole point of this window is to sit open while
// you read it.
// =============================================================================
class RemoteLogEntryWnd : public FloatingPanelWnd {
    public:
        void Init(HINSTANCE hInstance, HWND hParent) override;
        void Init(HINSTANCE hInstance, HWND hParent, int8_t position) override;
        void Show() override;

        ~RemoteLogEntryWnd() {
            if (m_hFontBody)  DeleteObject(m_hFontBody);
            if (m_hFontBold)  DeleteObject(m_hFontBold);
            if (m_hFontSmall) DeleteObject(m_hFontSmall);
            DestroyBackBuffer();
        }

        // Load an entry and bring the window up. Replaces whatever was shown —
        // one detail window, reused, because a second double-click meaning "also
        // show me this one" would leave a trail of windows to close.
        void ShowEntry(const Remote::Log::Entry &e);

        // Prev/Next walk the LIST's current ordering, not the entry numbers: the
        // list owns the sort, and stepping by seq while the user is sorted by
        // Δ time would jump somewhere they cannot see. So the step is delegated
        // back to the owner, which moves its selection and calls ShowEntry.
        void SetOwner(RemoteLogWnd *owner) { m_owner = owner; }

    protected:
        LRESULT HandlePanelMessage(UINT message, WPARAM wParam, LPARAM lParam) override;
        bool    OnKeyDown(WPARAM vk, bool ctrl, bool shift, bool alt) override;

    private:
        struct Field {
            const wchar_t *label;
            std::wstring   value;
        };

        struct Btn {
            const wchar_t *label;
            int            id = 0;
            RECT           rect{};
            bool           enabled = true;
        };

        void DoCopy();            // the whole entry, as text, to the clipboard
        void DoStep(int delta);   // −1 previous, +1 next, in the list's order
        void BuildFields();
        int  HitTestBtn(POINT pt) const;
        int  ContentHeightPx();   // measured with DT_CALCRECT — values wrap
        int  ViewHeightPx() const;
        void ScrollTo(int y);

        void EnsureFonts(HDC dc);
        void EnsureBackBuffer(HDC refDC, int w, int h);
        void DestroyBackBuffer();
        void Repaint();

        RemoteLogWnd      *m_owner = nullptr;
        Remote::Log::Entry m_entry{};
        std::vector<Field> m_fields;
        std::vector<Btn>   m_buttons;

        int m_hotBtn = -1;

        RECT m_track{}, m_thumb{};
        bool m_thumbHot  = false;
        bool m_dragging  = false;
        int  m_dragGrabPx    = 0;
        int  m_dragThumbSpan = 0;

        int m_scrollY     = 0;
        int m_contentH    = 0;   // filled by the painter, used by the scrollers

        HFONT m_hFontBody  = nullptr;
        HFONT m_hFontBold  = nullptr;
        HFONT m_hFontSmall = nullptr;
        int   m_cachedFontDpi = 0;

        HDC     m_bbDC     = nullptr;
        HBITMAP m_bbBmp    = nullptr;
        HBITMAP m_bbBmpOld = nullptr;
        int     m_bbW = 0, m_bbH = 0;
};

class RemoteLogWnd : public FloatingPanelWnd {
    public:
        void Init(HINSTANCE hInstance, HWND hParent) override;
        void Init(HINSTANCE hInstance, HWND hParent, int8_t position) override;
        void Show() override;
        void Hide() override;

        // Move the selection by `delta` rows in the CURRENT ordering and push
        // the new row into the detail window if it is open. Public because the
        // detail window's Prev/Next call it — the list owns the sort, so it is
        // the only thing that knows what "next" means.
        //
        // Returns false when the move would leave the list, so the caller can
        // grey a button rather than silently do nothing.
        bool StepSelection(int delta);
        bool CanStepSelection(int delta) const;

        ~RemoteLogWnd() {
            if (m_hFontBody)  DeleteObject(m_hFontBody);
            if (m_hFontBold)  DeleteObject(m_hFontBold);
            if (m_hFontSmall) DeleteObject(m_hFontSmall);
            DestroyBackBuffer();
        }

    protected:
        LRESULT HandlePanelMessage(UINT message, WPARAM wParam, LPARAM lParam) override;
        bool    OnKeyDown(WPARAM vk, bool ctrl, bool shift, bool alt) override;

    private:
        // Only the three that answer a question the log is opened with — see the
        // header. The text columns are not in this enum on purpose.
        enum class SortKey { Seq, Time, Delta };

        struct Button {
            std::wstring   label;
            int            id = 0;
            RECT           rect{};
            bool           enabled = true;
        };

        // One column: where it starts, how wide, and whether its header is a
        // sort button. Held as data rather than as seven sets of local variables
        // so the header, the cells and the hit test cannot disagree about where
        // a column is — which is the bug every hand-laid-out table grows.
        struct Column {
            const wchar_t *title;
            int            width;    // design units, scaled by dpi at paint time
            bool           sortable;
            SortKey        key;
            RECT           headerRect{};
        };

        // --- Actions ---------------------------------------------------------
        void DoToggleLogging();   // the switch, and it travels to the targets
        void DoClear();
        void DoSave();
        void DoLoad();
        void DoSort(SortKey key); // same key again reverses
        // Open the selected row in the detail window — the answer to a table
        // that has to ellipsise. Double-click, or Enter.
        void DoOpenDetail();

        // --- Model -----------------------------------------------------------
        void Rebuild();           // Remote::Log::Snapshot() → m_rows, then sorted
        void ApplySort();
        void BuildButtons();

        // --- Scrolling -------------------------------------------------------
        // Content size is columns × rows in pixels. One clamp function, called
        // from WM_SIZE, from Rebuild and after a DPI change, so the two axes can
        // never be clamped against different ideas of how big the content is.
        void ClampScroll();
        int  ContentWidthPx() const;
        int  ContentHeightPx() const;
        int  RowHeightPx() const;
        int  HeaderTopPx() const;   // y of the column header, below the buttons
        int  ListTopPx() const;     // y of the first row
        int  ViewWidthPx() const;   // list area, bars excluded
        int  ViewHeightPx() const;
        void ScrollTo(int x, int y);

        // Which bar the mouse is dragging. Held rather than derived, because the
        // pointer leaves the thumb during a drag and the drag must continue —
        // that is what a captured drag means.
        enum class Drag { None, Vert, Horz };

        // --- Paint -----------------------------------------------------------
        void EnsureFonts(HDC dc);
        void EnsureBackBuffer(HDC refDC, int w, int h);
        void DestroyBackBuffer();
        void Repaint();
        int  HitTestButton(POINT pt) const;
        int  HitTestHeader(POINT pt) const;  // index into m_columns, or -1
        // Row under the cursor, or -1. Computed from the scroll offset rather
        // than from stored rects: only the visible band is painted, so most rows
        // have no rect to test against.
        int  HitTestRow(POINT pt) const;
        // Scrolls the selected row into view after a keyboard move.
        void EnsureSelectionVisible();

        std::vector<Remote::Log::Entry> m_rows;
        std::vector<Button>             m_buttons;
        std::vector<Column>             m_columns;

        SortKey m_sortKey       = SortKey::Seq;
        bool    m_sortAscending = true;

        // Tracked by ENTRY NUMBER, not row index. New entries arrive constantly
        // and a sort reorders everything; an index would silently come to mean a
        // different exchange, which is exactly the row the user is about to open.
        // 0 = nothing selected.
        long long m_selectedSeq = 0;
        int       m_selectedRow = -1;   // resolved from m_selectedSeq by Rebuild

        // The detail window. Owned here because it has no life of its own — it
        // exists to show a row of this list.
        RemoteLogEntryWnd m_detail;

        // Scroll offsets in PIXELS, both clamped by UpdateScrollBars. Pixels
        // rather than rows for the vertical one too, so a wheel notch and a thumb
        // drag move the same units and the list does not jump between them.
        int m_scrollX = 0;
        int m_scrollY = 0;

        // Follow the tail while the newest entry is already on screen — the
        // behaviour a live log needs — but stop the moment the user scrolls up,
        // because yanking the view back to the bottom while they are reading is
        // the single worst thing a log window can do.
        bool m_followTail = true;

        int m_hotButton = -1;
        int m_hotHeader = -1;

        // Filled by the painter, read by the hit tests. Empty when that axis has
        // nothing to scroll — which is also how the hit test knows to ignore it.
        RECT m_vTrack{}, m_vThumb{};
        RECT m_hTrack{}, m_hThumb{};
        bool m_vThumbHot = false;
        bool m_hThumbHot = false;

        Drag m_drag = Drag::None;
        int  m_dragGrabPx    = 0;  // cursor offset INSIDE the thumb when grabbed
        int  m_dragThumbSpan = 0;  // thumb length at grab time, for the mapping

        std::wstring m_status;   // footer line

        HFONT m_hFontBody  = nullptr;
        HFONT m_hFontBold  = nullptr;
        HFONT m_hFontSmall = nullptr;
        int   m_cachedFontDpi = 0;

        HDC     m_bbDC     = nullptr;
        HBITMAP m_bbBmp    = nullptr;
        HBITMAP m_bbBmpOld = nullptr;
        int     m_bbW = 0, m_bbH = 0;
};

} // namespace UI
