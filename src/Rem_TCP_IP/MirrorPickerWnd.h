#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include "UI/FloatingPanels/FloatingPanelWnd.h"

// =============================================================================
// MirrorPickerWnd (Ctrl+F11) — WHICH connected instances F11 drives.
//
// Was a checkable popup put up by F11 itself. Two things were wrong with that:
//
//   1. F11 is a TOGGLE. Making it also ask a question meant switching mirroring
//      on cost a keypress and a dismissal, every time, on the one path that has
//      to be instant. F11 is now nothing but the toggle again.
//
//   2. A popup cannot stay open. Win32 closes a menu on every pick, so the
//      multi-select had to be torn down and rebuilt after each tick, it could
//      not be left up beside the pictures while working, and it could not show
//      a target connecting or dropping without being reopened.
//
// So: a real panel, on Ctrl+F11, alongside F10 (targets) and F9 (this
// instance's listener). It stays open, refreshes itself on a timer, and every
// tick takes effect immediately — there is no OK and no Cancel, because the
// selection lives on the targets in Remote::Mirror and a "cancel" would need a
// copy of it to roll back to.
//
//     #   Name        Address              Mirror
//     1   Monitor2    127.0.0.1:8771         ☑
//     2   Monitor3    10.0.0.5:8772          ☐
//
// CONNECTED rows only. A listed-but-idle target receives nothing whatever this
// panel says, so offering it would be offering a choice with no effect —
// connecting it is the F10 console's job, not this one's.
//
// The selection is SESSION state (Remote::Mirror, not persisted): a narrowed
// selection describes what is on the desk right now, and a saved one would
// silently leave screens out of a later session for a reason nobody remembers.
//
// Full design record: docs/REMOTE_MIRRORING.md
// =============================================================================

namespace UI {

class MirrorPickerWnd : public FloatingPanelWnd {
    public:
        void Init(HINSTANCE hInstance, HWND hParent) override;
        void Init(HINSTANCE hInstance, HWND hParent, int8_t position) override;
        void Show() override;
        void Hide() override;

        ~MirrorPickerWnd() {
            if (m_hFontBody)  DeleteObject(m_hFontBody);
            if (m_hFontBold)  DeleteObject(m_hFontBold);
            if (m_hFontSmall) DeleteObject(m_hFontSmall);
            DestroyBackBuffer();
        }

    protected:
        LRESULT HandlePanelMessage(UINT message, WPARAM wParam, LPARAM lParam) override;
        bool    OnKeyDown(WPARAM vk, bool ctrl, bool shift, bool alt) override;

    private:
        struct RowView {
            int          id = 0;          // Remote::Mirror target id
            std::wstring name;
            std::wstring host;
            int          port = 0;
            bool         sameMachine = false;
            bool         mirroring   = true;
            RECT         rect{};          // whole row — the hit box, see OnClickRow
            RECT         markRect{};      // ☑ / ☐
        };

        struct Button {
            std::wstring   label;
            int            id = 0;
            RECT           rect{};
            bool           enabled = true;
        };

        // --- Actions ---------------------------------------------------------
        void DoToggleRow(int row);
        void DoSetAll(bool on);
        // Push this viewer's folder, picture and view state to the TICKED rows.
        // Aimed, not broadcast: BroadcastSync deliberately ignores the mirror
        // selection because it backs the console's "Sync all" button, whose
        // label promises every target.
        void DoSyncSelected();

        // --- Model -----------------------------------------------------------
        void Rebuild();      // Remote::Mirror::Targets() → m_rows (connected only)
        void BuildButtons();

        // --- Paint -----------------------------------------------------------
        void EnsureFonts(HDC dc);
        void EnsureBackBuffer(HDC refDC, int w, int h);
        void DestroyBackBuffer();
        void Repaint();
        int  HitTestRow(POINT pt) const;
        int  HitTestButton(POINT pt) const;

        std::vector<RowView> m_rows;
        std::vector<Button>  m_buttons;

        int m_selectedRow = 0;
        int m_hotRow      = -1;
        int m_hotButton   = -1;

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
