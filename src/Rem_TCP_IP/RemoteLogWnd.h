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

    protected:
        LRESULT HandlePanelMessage(UINT message, WPARAM wParam, LPARAM lParam) override;
        bool    OnKeyDown(WPARAM vk, bool ctrl, bool shift, bool alt) override;

    private:
        struct Field {
            const wchar_t *label;
            std::wstring   value;
        };

        void DoCopy();            // the whole entry, as text, to the clipboard
        void BuildFields();
        int  ContentHeightPx();   // measured with DT_CALCRECT — values wrap
        int  ViewHeightPx() const;
        void ScrollTo(int y);

        void EnsureFonts(HDC dc);
        void EnsureBackBuffer(HDC refDC, int w, int h);
        void DestroyBackBuffer();
        void Repaint();

        Remote::Log::Entry m_entry{};
        std::vector<Field> m_fields;

        RECT m_copyRect{};
        bool m_copyHot = false;

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

        std::vector<Remote::Log::Entry> m_rows;
        std::vector<Button>             m_buttons;
        std::vector<Column>             m_columns;

        SortKey m_sortKey       = SortKey::Seq;
        bool    m_sortAscending = true;

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
