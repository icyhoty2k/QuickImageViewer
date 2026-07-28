#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include "UI/FloatingPanels/FloatingPanelWnd.h"
#include "UI/CustomControls/InputBox.h"

// =============================================================================
// RemotesWnd (F10) — the console for the instances this copy drives.
//
// One row per target:
//
//     #   Name        IP           Port   Lag      ●    👁
//     1   Monitor2    127.0.0.1    8771   0.4 ms   ●    👁
//     2   Monitor3    127.0.0.1    8772   —        ●    👁̶
//
//   ●   green when the target answered the last poll, red when it did not.
//       Clicking a RED dot launches that target's exe; clicking a GREEN one
//       sends it `quit`. Amber while an action is in flight, because a dot that
//       does not change until the next poll reads as a click that did nothing.
//
//   👁  toggles observation. On, the target reports what IT does back to us and
//       this viewer follows along — which is how you watch a screen that is
//       running its own slideshow. Off, it only listens.
//
//       A RADIO BUTTON, not a checkbox: switching one on switches the others
//       off. Observing means doing what the other instance does, and two at
//       once would interleave two unrelated streams of actions into this one
//       screen — the result follows neither.
//
//   F5  polls every row and fills in the lag column.
//
// WHY THIS IS A SEPARATE PANEL FROM F9. F9 configures ONE connection in detail —
// the listener this instance runs, and a single peer to send a command to. This
// manages the SET: which screens exist, whether they are up, and the two
// per-screen actions worth a single click. Folding them together would put a
// form and a table in one window and make the common case (glance, see all
// green) harder to reach.
//
// -----------------------------------------------------------------------------
// NOTHING HERE BLOCKS THE UI THREAD.
//
// Every socket operation goes through Remote::Mirror, which owns a thread per
// target and a queue the UI thread pushes into. This panel never touches a
// socket: it queues an action and paints from a snapshot. A target that is
// switched off cannot make the console hesitate, which matters because the
// console is exactly what you open WHEN one is switched off.
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
        // the main window exists — not from Init, because a target list is
        // worth acting on whether or not the panel is ever opened.
        static void AutoConnectAll(HWND hOwner);

    protected:
        LRESULT HandlePanelMessage(UINT message, WPARAM wParam, LPARAM lParam) override;
        bool    OnKeyDown(WPARAM vk, bool ctrl, bool shift, bool alt) override;
        bool    OnLocalHide() override;

    private:
        // What a row's dot is showing. Pending exists so a click is visibly
        // acknowledged before the next poll confirms it.
        enum class DotState { Down, Up, Pending };

        struct RowView {
            int          id = 0;      // Remote::Mirror target id
            std::wstring name;
            std::wstring host;
            int          port = 0;
            std::wstring exePath;
            bool         sameMachine = false;
            std::wstring lastError;
            long long    lagUs    = -1;
            bool         observing = false;
            DotState     dot      = DotState::Down;

            RECT rect{};      // whole row
            RECT dotRect{};   // ● hit box
            RECT eyeRect{};   // 👁 hit box
        };

        struct Button {
            std::wstring label;
            int          id = 0;
            RECT         rect{};
            bool         enabled = true;
        };

        // --- Actions ---------------------------------------------------------
        void DoPollAll();          // F5
        void DoAddTarget();        // prompt for host/port/password, then connect
        void DoRemoveTarget(int row);
        void DoStartTarget(int row);   // CreateProcess on its exe
        void DoStopTarget(int row);    // `quit` down the live connection
        void DoToggleObserve(int row);
        void DoSyncAll();          // push this instance's view state to every target
        void DoSave();             // rewrite qivRemotes.ini from the current rows

        // --- Model -----------------------------------------------------------
        void Rebuild();            // Remote::Mirror::Targets() → m_rows
        void PersistRows();        // m_rows → qivRemotes.ini

        // --- Paint -----------------------------------------------------------
        void EnsureFonts(HDC dc);
        void EnsureBackBuffer(HDC refDC, int w, int h);
        void DestroyBackBuffer();
        void Repaint();
        int  HitTestRow(POINT pt) const;
        int  HitTestDot(POINT pt) const;
        int  HitTestEye(POINT pt) const;
        int  HitTestButton(POINT pt) const;

        // --- Dialog helpers (panel is topmost; dialogs are not) --------------
        void DialogMessage(const std::wstring &text, const wchar_t *caption);
        bool DialogConfirm(const std::wstring &text, const wchar_t *caption);
        int  DialogPromptInt(const wchar_t *caption, const wchar_t *label,
                             int cur, int lo, int hi, int def);
        void PushTopmostOff();
        void PopTopmost();

        std::vector<RowView> m_rows;
        std::vector<Button>  m_buttons;
        int m_selected  = 0;
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
