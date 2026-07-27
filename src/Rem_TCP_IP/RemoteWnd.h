#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include "UI/FloatingPanels/FloatingPanelWnd.h"
#include "UI/CustomControls/InputBox.h"

// =============================================================================
// RemoteWnd — configure and control the TCP/IP remote server.
//
// Lives in src/Rem_TCP_IP rather than UI/FloatingPanels because everything
// remote-related belongs together, the same way src/Dedicated keeps its own
// window beside its own settings.
//
// Two halves, because qIV is both ends of this protocol:
//
//   SERVER   the listener this instance runs — enable, identity, bind address,
//            port, allow/block lists, password, connection cap
//   PEER     another instance to connect OUT to — host, port, password, and a
//            command to send
//
// -----------------------------------------------------------------------------
// NOTHING HERE BLOCKS THE UI THREAD.
//
// Check Connection and Send both talk to a remote machine, which may be switched
// off. Remote::Client bounds every wait, but a bounded stall is still a stall and
// freezing the viewer for four seconds because a wall screen is unplugged would
// be unacceptable. Both run on a short-lived worker thread that posts its result
// back as WM_REMOTE_ASYNC_RESULT, carrying a heap std::wstring this window owns
// and deletes.
// -----------------------------------------------------------------------------
//
// Full design record: docs/REMOTE_TCP_IP_SPEC.md
// =============================================================================

namespace UI {

class RemoteWnd : public FloatingPanelWnd {
    public:
        void Init(HINSTANCE hInstance, HWND hParent) override;
        void Init(HINSTANCE hInstance, HWND hParent, int8_t position) override;
        void Show() override;

        ~RemoteWnd() {
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
        enum class Kind { Header, Text, Secret, Number, Toggle, Choice, ReadOnly };

        struct Row {
            Kind           kind = Kind::Text;
            std::wstring   label;
            std::wstring   value;
            const wchar_t *desc = L"";
            int            id   = 0;
            RECT           rect{};
        };

        struct Button {
            std::wstring label;
            int          id = 0;
            RECT         rect{};
            bool         enabled = true;
            int          row = 0;
        };

        // --- Actions ---------------------------------------------------------
        void DoStart();
        void DoStop();
        void DoCheckConnection();  // probe the PEER, on a worker thread
        void DoSendToPeer();       // connect, send one command, report the reply
        void DoSaveToIni();        // seeded generation — may create the .ini

        // Shared by the two peer actions: both need a worker so the panel stays
        // responsive while a dead host times out.
        void RunAsync(void (*work)(RemoteWnd *self));

        // --- Model -----------------------------------------------------------
        void BuildRows();
        void EditRow(int rowIndex);
        void BeginTextEdit(int rowIndex);
        void CommitTextEdit();
        void CancelTextEdit();
        void PullFromConfig();  // Remote::Config() → m_rows
        void PushToConfig();    // m_rows → Remote::Config()

        std::wstring StatusLine() const;

        // --- Dialog helpers (panel is topmost; dialogs are not) --------------
        void DialogMessage(const std::wstring &text, const wchar_t *caption);
        bool DialogConfirm(const std::wstring &text, const wchar_t *caption);
        int  DialogPromptInt(const wchar_t *caption, const wchar_t *label,
                             int cur, int lo, int hi, int def);
        void PushTopmostOff();
        void PopTopmost();

        // --- Paint -----------------------------------------------------------
        void EnsureFonts(HDC dc);
        void EnsureBackBuffer(HDC refDC, int w, int h);
        void DestroyBackBuffer();
        void Repaint();
        int  HitTestRow(POINT pt) const;
        int  HitTestButton(POINT pt) const;

        std::vector<Row>    m_rows;
        std::vector<Button> m_buttons;
        int m_selected  = 0;
        int m_hotRow    = -1;
        int m_hotButton = -1;

        // Peer target. Session-only by design: an address you are driving from
        // this machine is not part of THIS instance's configuration, and writing
        // it into the .ini would make a screen's config describe its neighbour.
        std::wstring m_peerHost = L"127.0.0.1";
        int          m_peerPort = 0;
        std::wstring m_peerPassword;
        std::wstring m_peerCommand = L"ping";

        // Plaintext only while the user is typing it. Hashed on save and cleared;
        // the panel never redisplays a password it has already stored.
        std::wstring m_newPassword;

        std::wstring m_lastResult; // last async reply or error, shown in the footer

        InputBox m_edit;
        int      m_editingRow = -1;

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
