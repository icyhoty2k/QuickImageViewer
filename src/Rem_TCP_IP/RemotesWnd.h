#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include "UI/FloatingPanels/FloatingPanelWnd.h"
#include "UI/CustomControls/InputBox.h"
#include "RemoteMirror.h" // Down — the classified reason a row is not connected

// =============================================================================
// RemotesWnd (F10) — everything about the instances this copy drives.
//
// The counterpart to F9: that panel describes what this instance IS (its
// listener), this one describes what it TALKS TO. They were one window and it
// read badly, because a form for "my identity" and a form for "my neighbours"
// have nothing to do with each other beyond sharing a protocol.
//
// Two sections:
//
//   New connection   address, port, password, name, exe. Save records it in
//                    qivRemotes.ini; Connect dials it. Two acts, two buttons —
//                    a screen can be listed while it is switched off, or before
//                    it has been configured at all, and the alternative was a
//                    saved list that could only contain reachable things.
//
//   Remotes          one row per known instance:
//
//     #   Name        IP           Port   Lag      Up   Link  Watch
//     1   Monitor2    127.0.0.1    8771   0.4 ms   ●    on    ◉
//     2   Monitor3    10.0.0.5     8772   offline  ●    on    ○
//     3   Spare       127.0.0.1    8773   not conn ●    off   ○
//
//   Three separate things, deliberately not merged into one control:
//
//     ●  the PROCESS. Green when the target answered the last poll, red when it
//        did not, grey when nobody has asked. Clicking a red dot launches that
//        target's exe; a green one sends it `quit`. Amber while an action is in
//        flight, because a dot that does not change until the next poll reads
//        as a click that did nothing.
//
//     Link  the CONNECTION. A row can be listed and idle — recorded for a screen
//        that is switched off, or not wanted right now. Per row rather than a
//        button acting on "the selection", because the same click that selects
//        a row also loads it into the form, which made "the selected row"
//        ambiguous at exactly the moment it mattered.
//
//     ◉  observation. On, the target reports what IT does and this viewer
//        follows along — how you watch a screen running its own slideshow.
//        A RADIO BUTTON, not a checkbox: switching one on switches the others
//        off, because two at once would interleave two unrelated streams of
//        actions into this one screen and follow neither.
//
//     F5 polls every row and fills in the lag column.
//
// -----------------------------------------------------------------------------
// NOTHING HERE BLOCKS THE UI THREAD.
//
// Row actions go through Remote::Mirror, which owns a thread and a queue per
// target; this panel only enqueues and paints from a snapshot. Connect & Save is
// the one operation that must wait for a network answer, so it runs on a
// short-lived worker and posts its result back — a target that is switched off
// takes seconds to fail, and the console is exactly what you open WHEN one is.
// -----------------------------------------------------------------------------
//
// Full design record: docs/REMOTE_TCP_IP_SPEC.md
// =============================================================================

namespace UI {

class RemotesWnd : public FloatingPanelWnd {
    public:
        void Init(HINSTANCE hInstance, HWND hParent) override;
        void Init(HINSTANCE hInstance, HWND hParent, int8_t position) override;
        void Show() override;

        ~RemotesWnd() {
            if (m_hFontBody)  DeleteObject(m_hFontBody);
            if (m_hFontBold)  DeleteObject(m_hFontBold);
            if (m_hFontSmall) DeleteObject(m_hFontSmall);
            DestroyBackBuffer();
        }

        // Connects every row marked AutoConnect. Called once at startup, after
        // the main window exists — not from Init, because a target list is worth
        // acting on whether or not the panel is ever opened.
        static void AutoConnectAll(HWND hOwner);

    protected:
        LRESULT HandlePanelMessage(UINT message, WPARAM wParam, LPARAM lParam) override;
        bool    OnKeyDown(WPARAM vk, bool ctrl, bool shift, bool alt) override;
        bool    OnLocalHide() override;

    private:
        // What a row's dot is showing.
        //   Idle    listed but not being dialled — nothing has been tried, so
        //           red would be a claim about a target nobody asked about.
        //   Down    dialling and failing. The reason is in the Lag column.
        //   Up      connected.
        //   Pending an action is in flight, so a click is acknowledged before
        //           the next poll confirms it.
        enum class DotState { Idle, Down, Up, Pending };

        // The New-connection form. Ids rather than indices so the field a row
        // edits survives the list being rebuilt underneath it.
        enum FieldId { F_NONE = 0, F_HOST, F_PORT, F_PASSWORD, F_NAME, F_EXE };

        struct Field {
            FieldId        id = F_NONE;
            std::wstring   label;
            std::wstring   value;
            const wchar_t *desc = L"";
            bool           secret = false; // shown as (set)/(none), never echoed
            RECT           rect{};
        };

        struct RowView {
            int          id = 0;      // Remote::Mirror target id
            std::wstring name;
            std::wstring host;
            int          port = 0;
            std::wstring exePath;
            bool         sameMachine = false;
            bool         connecting  = false; // listed and dialling, vs listed and idle
            std::wstring lastError;
            Remote::Mirror::Down down = Remote::Mirror::Down::None;
            // Checked when the list is rebuilt, not when the dot is clicked: a
            // row whose exe has been moved or deleted should say so before you
            // press the button that would fail.
            bool         exeMissing = false;
            long long    lagUs    = -1;
            bool         observing = false;
            DotState     dot      = DotState::Down;

            RECT rect{};      // whole row
            RECT dotRect{};   // ● hit box — start / stop the PROCESS
            RECT linkRect{};  // on/off hit box — connect / disconnect the LINK
            RECT eyeRect{};   // ◉ hit box — observe
        };

        struct Button {
            std::wstring   label;
            int            id = 0;
            RECT           rect{};
            bool           enabled = true;
            const wchar_t *tip = L""; // hover text — see ShowTipAt
        };

        // --- Actions ---------------------------------------------------------
        // Record the form, without dialling it. Adding a remote and connecting
        // to it are separate acts — a screen can be listed while it is switched
        // off, or before it has been set up at all.
        //
        // Adds a row, or UPDATES the one loaded by clicking it (m_editingRowId).
        void DoSaveEntry();
        // Abandon an edit and go back to describing a new remote.
        void DoNewEntry();
        // Clicking a row loads it into the form — its address, port, name, exe
        // and stored credential — so it can be corrected in place rather than
        // removed and retyped.
        void FillFormFromRow(int row);
        // Start or stop dialling one row — the per-row Link cell.
        void DoToggleConnect(int row);
        // Every row at once.
        void DoConnectAll(bool on);
        // Pick another instance's .ini (or .exe) and fill the form from it —
        // port, name, exe and credentials, with nothing to type.
        void DoImportFromFile();
        void DoPollAll();          // F5
        void DoRemoveTarget(int row);
        void DoStartTarget(int row);   // CreateProcess on its exe (same machine only)
        void DoStopTarget(int row);    // `quit` down the live connection
        void DoToggleObserve(int row);
        void DoSyncAll();          // push this instance's view state to every target

        // --- Form ------------------------------------------------------------
        void BuildFields();
        void EditField(int fieldIndex);
        void BeginTextEdit(int fieldIndex);
        void CommitTextEdit();
        void CancelTextEdit();

        // --- Model -----------------------------------------------------------
        void Rebuild();            // Remote::Mirror::Targets() → m_rows
        void PersistRows();        // m_rows → qivRemotes.ini

        // Hover help. Several controls here are a single glyph or a two-letter
        // word, and what they do is not guessable from the drawing — the dot,
        // the Link cell and the observe circle all live in the same row and mean
        // three different things.
        //
        // `clientRect` is the control being explained, in CLIENT coordinates;
        // ThemedTooltip anchors to it and dismisses itself when the cursor
        // leaves, so no bookkeeping is needed here beyond calling it.
        void ShowTipAt(const RECT &clientRect, const wchar_t *text);
        void UpdateTip(POINT ptClient);

        // What the tooltip is currently explaining, so a mouse-move inside one
        // control does not re-show the same popup on every pixel.
        const void *m_tipOwner = nullptr;

        // --- Paint -----------------------------------------------------------
        void EnsureFonts(HDC dc);
        void EnsureBackBuffer(HDC refDC, int w, int h);
        void DestroyBackBuffer();
        void Repaint();
        int  HitTestField(POINT pt) const;
        int  HitTestRow(POINT pt) const;
        int  HitTestDot(POINT pt) const;
        int  HitTestLink(POINT pt) const;
        int  HitTestEye(POINT pt) const;
        int  HitTestButton(POINT pt) const;

        // --- Dialog helpers (panel is topmost; dialogs are not) --------------
        void DialogMessage(const std::wstring &text, const wchar_t *caption);
        bool DialogConfirm(const std::wstring &text, const wchar_t *caption);
        int  DialogPromptInt(const wchar_t *caption, const wchar_t *label,
                             int cur, int lo, int hi, int def);
        void PushTopmostOff();
        void PopTopmost();

        std::vector<Field>   m_fields;
        std::vector<RowView> m_rows;
        std::vector<Button>  m_buttons;

        // What is currently typed into the New-connection form. Held here rather
        // than read back out of the Field values so a secret is never stored as
        // the literal text "(set)".
        std::wstring m_newHost = L"127.0.0.1";
        int          m_newPort = 0;
        std::wstring m_newPassword;
        std::wstring m_newName;
        std::wstring m_newExe;

        // Which existing row the form is editing, or 0 for "a new one". Set by
        // clicking a row. Without it, correcting a saved remote would mean
        // Remove-then-retype, and Save would refuse the edit as a duplicate
        // name — the row it collided with being the row being edited.
        int m_editingRowId = 0;

        InputBox m_edit;
        int      m_editingField = -1;

        int m_selectedRow  = 0;
        int m_selectedField = -1;
        int m_hotRow    = -1;
        int m_hotButton = -1;

        std::wstring m_status; // footer line

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
