#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include "UI/FloatingPanels/FloatingPanelWnd.h"
#include "UI/CustomControls/InputBox.h"
#include "RemoteMirror.h"   // CmdReply

// =============================================================================
// RemoteCmdWnd (Ctrl+F10) — pick a command, give it a value, send it.
//
// The manual counterpart to mirroring. F11 forwards what you DO; this sends what
// you TYPE — including the commands that have no key at all (`msgRemote`,
// `OpenFile`, `JumpToImage`, `SlideshowSetInterval`, `EnableRemoteLog`).
//
// Every command has ONE wire name, spelled exactly like its Command.h
// enumerator. What this panel lists is what goes on the socket.
//
//   Filter [jump          ]        JumpToImage  <value>
//   ┌ Commands ───────────┐        image NUMBER in the playlist, 1-based
//   │ FlipH              ░│
//   │▸JumpToImage  image…▓│        Value
//   │ NextImage          ▓│        [ 12                              ]
//   │ …                  ░│
//   └─────────────────────┘        [ Send ]   ☐ also run it here
//   ┌ Replies ────────────────────────────────────────────────────┐
//   │ Monitor2   OK goto=48/238                          0.4 ms  ░│
//   └─────────────────────────────────────────────────────────────┘
//
// BROWSE FIRST. The command list is PERMANENT and scrollable, not a popup you
// have to summon: nobody remembers ninety wire names, and a list that only
// appears on a chord is a list most people never find. The filter box narrows
// it; the detail pane on the right explains whatever is highlighted.
//
// The list is built from Remote::CommandTable() — the same table the wire parser
// accepts — so a name shown here cannot come back "unknown command", and there
// is exactly one row per command because the table is now one row per command.
//
// WHERE IT GOES: every CONTROLLED target — the ☑ rows in Ctrl+F11, the same
// selection F11 drives. The ☐ also-run-here box adds THIS instance, off by
// default because the usual reason to type a command here is to do something to
// the other screens.
//
// KEYS
//   type         narrows the list (focus starts in the filter box)
//   ↑ ↓ PgUp/Dn  move the highlight
//   Tab          filter ⇄ value
//   Enter        in the filter: take the highlighted command — to the value box
//                if it needs one, otherwise send. In the value box: send.
//   Esc          clear the filter, then close
//
// BOTH LISTS SCROLL, with the drawn bars this app uses everywhere else — native
// scrollbars are non-client and render as white gutters on a dark panel.
//
// REPLIES arrive one per target, asynchronously (WM_QIV_REMOTE_CMD_REPLY):
// sending happens on a sender thread and waiting for an answer on the UI thread
// would freeze the viewer doing the asking.
// =============================================================================

namespace UI {

class RemoteCmdWnd : public FloatingPanelWnd {
    public:
        void Init(HINSTANCE hInstance, HWND hParent) override;
        void Init(HINSTANCE hInstance, HWND hParent, int8_t position) override;
        void Show() override;

        ~RemoteCmdWnd() {
            if (m_hFontBody)  DeleteObject(m_hFontBody);
            if (m_hFontBold)  DeleteObject(m_hFontBold);
            if (m_hFontSmall) DeleteObject(m_hFontSmall);
            DestroyBackBuffer();
        }

    protected:
        LRESULT HandlePanelMessage(UINT message, WPARAM wParam, LPARAM lParam) override;
        bool    OnKeyDown(WPARAM vk, bool ctrl, bool shift, bool alt) override;
        bool    OnLocalHide() override;

    private:
        enum class Focus { Filter, Value };

        // One scrollable region. Two of them here (commands, replies) and the
        // arithmetic is identical, so it lives in one place rather than twice —
        // the drawn bar, the clamp, and the drag all read the same fields.
        struct ScrollView {
            RECT view{};      // where the rows are drawn, bar excluded
            RECT track{};     // empty when the content fits
            RECT thumb{};
            int  scrollY   = 0;
            int  contentH  = 0;
            bool thumbHot  = false;

            int  MaxScroll() const;
            void Clamp();
            void ScrollBy(int dy);
        };

        struct Command {
            std::wstring name;
            std::wstring desc;       // may be empty — see CommandEntry::desc
            bool         needsValue = false;
        };

        struct Reply {
            std::wstring target;
            std::wstring text;
            bool         ok = false;
            long long    deltaUs = -1;
        };

        struct Button {
            std::wstring label;
            int          id = 0;
            RECT         rect{};
            bool         enabled = true;
        };

        // --- Actions ---------------------------------------------------------
        void DoSend();
        void DoClear();
        void MoveSelection(int delta);
        // Enter in the filter box: commit the highlighted command. Sends it
        // outright when it takes no value, so the common case is one keypress.
        void TakeHighlighted();

        // --- Model -----------------------------------------------------------
        void BuildCommands();     // once, from Remote::CommandTable()
        void ApplyFilter();       // m_all → m_shown, preserving the highlight
        void BuildButtons();
        const Command *Selected() const;

        // --- Layout / paint ---------------------------------------------------
        void EnsureFonts(HDC dc);
        void EnsureBackBuffer(HDC refDC, int w, int h);
        void DestroyBackBuffer();
        void Repaint();
        void EnsureSelectionVisible();
        // Draws a view's bar and fills in its track/thumb. Empty rects when the
        // content fits, which is also how the hit tests know to ignore it.
        void DrawScrollBar(HDC bb, ScrollView &sv, float s, COLORREF trackCol,
                           COLORREF thumbCol, COLORREF thumbHotCol, bool dragging);
        int  HitTestButton(POINT pt) const;
        int  HitTestCommandRow(POINT pt) const;

        InputBox m_filterBox;
        InputBox m_valueBox;
        Focus    m_focus = Focus::Filter;

        std::vector<Command> m_all;     // every distinct wire command
        std::vector<Command> m_shown;   // after the filter
        int m_selected = 0;             // index into m_shown

        std::vector<Reply>  m_replies;
        std::vector<Button> m_buttons;

        ScrollView m_list;
        ScrollView m_log;

        // Which bar is being dragged, if any. Held because the pointer leaves
        // the thumb during a drag and the drag must continue.
        enum class Drag { None, List, Log };
        Drag m_drag = Drag::None;
        int  m_dragGrabPx    = 0;
        int  m_dragThumbSpan = 0;

        bool m_alsoLocal = false;
        RECT m_localRect{};
        RECT m_filterRect{}, m_valueRect{};

        std::wstring m_status;
        int          m_awaiting = 0;

        int m_hotButton = -1;

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
