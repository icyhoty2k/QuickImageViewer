#pragma once

#include <windows.h>
#include <vector>
#include <string>

#include "IPanelWindow.h"

namespace UI {
    class HistoryListWnd : public IPanelWindow {
        // -------------------------------------------------------------------------
        // HistoryListWnd  —  Last-visited folder history panel.
        //
        // Shows the last Constants::HISTORY_MAX_DIRS folders the user opened,
        // most-recent at the top.  Click any row to open that folder (first image).
        //
        // Displayed as a centered floating GDI popup — same dark style as HelpWnd,
        // same WS_POPUP | WS_EX_TOPMOST | WS_EX_LAYERED flags.
        //
        // Shortcuts:
        //   F7  —  Toggle history panel (SC_PANEL_HISTORY_TOGGLE)
        //   Esc —  Hide panel (SC_LOCAL_HIDE)
        // -------------------------------------------------------------------------
        public:
            void Init(HINSTANCE hInstance, HWND hParent) override;

            void Init(HINSTANCE hInstance, HWND hParent, int8_t position) override;

            // Resize to fit current history count, then show.
            void Show() override;

            // Hide if visible, Show() if not.
            void Toggle() override;

            // Called by FileHandler after every successful folder load.
            void PushFolderHistory(const std::wstring &folderPath);

            // Returns the current history list (index 0 = most recent).
            const std::vector<std::wstring> &GetFolderHistory();

        protected:
            LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) override;

        private:
            void ToggleHistoryWindow();
    };

    // Free-function variants kept for FileHandler compatibility
    void PushFolderHistory(const std::wstring &folderPath);

    const std::vector<std::wstring> &GetFolderHistory();
} // namespace UI
