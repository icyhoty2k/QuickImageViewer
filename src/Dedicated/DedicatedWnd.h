#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include "UI/FloatingPanels/FloatingPanelWnd.h"
#include "UI/CustomControls/InputBox.h"
#include "UI/CustomControls/ScrollView.h"
#include "DedicatedInstance.h"

// =============================================================================
// DedicatedWnd (F8) — build and manage dedicated instances.
//
// A dedicated instance is a SEPARATE COPY of the exe with its own .ini beside
// it: own identity, own settings, no registry, no history. Normally parked
// fullscreen on one monitor running a slideshow, optionally interleaving
// promotions from a second folder.
//
// The four actions across the top are the whole workflow:
//
//   Generate        copy this exe to a chosen folder under a *dedicated* name
//                   and write its .ini next to it
//   Add Startup     drop a shortcut in shell:startup pointing at that copy:
//                       qIV_dedicated_Lobby.exe -dedicated -config "…\x.ini"
//   Remove Startup  delete that shortcut again
//   Test            load any .ini, report what is wrong with it, offer a rebuild
//
// Everything below the buttons is the configuration itself — instance identity,
// content folders, promotion pacing, presentation, and every persisted app
// setting — so a screen can be configured entirely from here.
// =============================================================================

namespace UI {

class DedicatedWnd : public FloatingPanelWnd {
    public:
        void Init(HINSTANCE hInstance, HWND hParent) override;
        void Init(HINSTANCE hInstance, HWND hParent, int8_t position) override;
        void Show() override;

        ~DedicatedWnd() {
            if (m_hFontBody)  DeleteObject(m_hFontBody);
            if (m_hFontBold)  DeleteObject(m_hFontBold);
            if (m_hFontSmall) DeleteObject(m_hFontSmall);
            if (m_hFontLink)  DeleteObject(m_hFontLink);
            DestroyBackBuffer();
        }

    protected:
        LRESULT HandlePanelMessage(UINT message, WPARAM wParam, LPARAM lParam) override;
        bool    OnKeyDown(WPARAM vk, bool ctrl, bool shift, bool alt) override;
        bool    OnLocalHide() override;

    private:
        enum class Kind { Header, Text, Folder, Number, Toggle, Choice };

        // A configuration line. `desc` is the HelpWnd-style explanation drawn
        // under the value — settings are useless if their effect is a guess.
        struct Row {
            Kind           kind;
            std::wstring   label;
            std::wstring   value;
            const wchar_t *desc = L"";
            int            id   = 0; // index into the edit dispatch
            RECT           rect{};
        };

        struct Button {
            std::wstring label;
            int          id = 0;
            RECT         rect{};
            bool         enabled = true;
            int          row = 0; // 0 = first button row, 1 = second
        };

        // --- Actions ---------------------------------------------------------
        // Generating the copy and generating its config are separate on purpose:
        // a config is edited far more often than the executable is replaced, and
        // re-copying the exe every time risks overwriting a running screen.
        void DoGenerateApp();    // copy this exe under the instance name
        void DoGenerateConfig(); // write the .ini for that copy
        void DoAddImages();      // append a folder to the image list
        void DoAddPromotions();  // append a folder to the promotion list
        void DoAddStartup();     // shortcut → shell:startup
        void DoRemoveStartup();  // delete that shortcut
        void DoTest();               // validate an .ini, offer to rebuild
        void DoTestList(bool promotions); // validate one folder list + its content

        // Shared by the two Add buttons — same flow, different list file.
        void AppendFolderToList(bool promotions);

        // Read-only view of a list's contents, creating the file if absent.
        void ShowFolderList(bool promotions);

        // The exe whose lists are being edited: the copy being authored when it
        // exists, otherwise this running instance.
        std::wstring ListOwnerExe() const;

        // --- Model -----------------------------------------------------------
        void BuildRows();
        void EditRow(int rowIndex);
        void BeginTextEdit(int rowIndex);
        void CommitTextEdit();
        void CancelTextEdit();

        void EditRangePair(int &from, int &to, int maxValue,
                           const wchar_t *caption, const wchar_t *unit);
        bool PickFolder(std::wstring &inOut, const wchar_t *title);
        std::wstring PickIniFile(bool save);
        // Picks a folder list, filtered to that kind's own extension so an image
        // list can never be chosen where a promotion list is meant.
        std::wstring PickListFile(bool promotions);

        // Paths of the copy this panel last generated / is managing.
        std::wstring TargetExePath() const;
        std::wstring TargetIniPath() const;
        std::wstring StartupLinkPath() const;

        // --- Dialog helpers --------------------------------------------------
        // The panel is topmost; a themed dialog is not, so it would open BEHIND
        // it. These drop topmost for the duration and restore it after.
        void  DialogMessage(const std::wstring &text, const wchar_t *caption);
        bool  DialogConfirm(const std::wstring &text, const wchar_t *caption);
        int   DialogPromptInt(const wchar_t *caption, const wchar_t *label,
                              int cur, int lo, int hi, int def);
        int   DialogPromptFloat(const wchar_t *caption, const wchar_t *label,
                                float cur, float lo, float hi, float def);
        void  PushTopmostOff();
        void  PopTopmost();

        // --- Paint -----------------------------------------------------------
        void EnsureFonts(HDC dc);
        void EnsureBackBuffer(HDC refDC, int w, int h);
        void DestroyBackBuffer();
        void Repaint();
        int  HitTestRow(POINT pt) const;
        int  HitTestButton(POINT pt) const;

        // The list's scroll state. Was eight separate members and a private
        // ClampScroll(); the interactions on top of them now live in the base,
        // so this panel contributes only the two overrides below.
        // See UI/CustomControls/ScrollView.h.
        ScrollView m_list;

        ScrollView *ScrollViewAt(POINT) override { return &m_list; }
        int ScrollLinePx(const ScrollView &) const override;

        Dedicated::InstanceConfig m_cfg;
        std::wstring              m_targetFolder; // where the copy was generated

        std::vector<Row>    m_rows;
        std::vector<Button> m_buttons;
        int  m_selected  = 0;
        int  m_hotRow    = -1; // under the cursor — drives the hand cursor
        int  m_hotButton = -1;

        InputBox m_edit;
        int      m_editingRow = -1;

        HFONT m_hFontBody  = nullptr;
        HFONT m_hFontBold  = nullptr;
        HFONT m_hFontSmall = nullptr;
        HFONT m_hFontLink  = nullptr; // small + underline, per Constants::Links

        // The subtitle's "Editing: <path>" — the path half, drawn as a link.
        RECT m_iniLinkRect{};          // hit box, recomputed on every paint
        bool m_iniLinkHot = false;

        int   m_cachedFontDpi = 0;

        HDC     m_bbDC     = nullptr;
        HBITMAP m_bbBmp    = nullptr;
        HBITMAP m_bbBmpOld = nullptr;
        int     m_bbW = 0, m_bbH = 0;
};

} // namespace UI
