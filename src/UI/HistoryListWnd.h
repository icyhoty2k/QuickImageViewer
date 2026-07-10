#pragma once

#include <windows.h>
#include <vector>
#include <string>

#include "IPanelWindow.h"

namespace UI {
    class HistoryListWnd : public IPanelWindow {
        // -----------------------------------------------------------------------
        // HistoryListWnd  —  Last-visited folder history panel.
        //
        // Shows up to HISTORY_MAX_DIRS_TO_SHOW regular entries plus up to
        // HISTORY_MAX_FAVORITES_TO_SHOW favorites.  Position of favorites
        // (top / bottom / in-place) is controlled by HISTORY_FAVORITES_POSITION.
        //
        // Shortcuts (active when panel is focused):
        //   Tab        — Toggle panel (SC_PANEL_HISTORY_TOGGLE)
        //   Space      — Toggle favorite on hovered row (HISTORY_FAVORITES_TOGGLE_KEY)
        //   Delete     — Clear all history except favorites (HISTORY_CLEAR_ALL_HISTORY_BUT_NOT_FAVORITES)
        //   Up/Down    — Move selection
        //   Enter      — Open selected folder
        //   Esc        — Hide panel
        // -----------------------------------------------------------------------
        public:
            void Init(HINSTANCE hInstance, HWND hParent) override;
            void Init(HINSTANCE hInstance, HWND hParent, int8_t position) override;

            void Show() override;
            void Toggle() override;

            void PushFolderHistory(const std::wstring &folderPath);
            const std::vector<std::wstring> &GetFolderHistory();

        protected:
            LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) override;

        private:
            void ToggleHistoryWindow();
    };

    // -----------------------------------------------------------------------
    // Free-function API — used by FileHandler, UIManager, and CommandExecuter
    // -----------------------------------------------------------------------

    // Call once at startup (from UIManager::Init) before any folder is opened.
    // Loads the full history + favorites from disk into RAM.
    void LoadFolderHistoryFromDisk();

    // Called by FileHandler after every successful folder load.
    void PushFolderHistory(const std::wstring &folderPath);

    // Toggle favorite status on the path at display index 'rowIndex'.
    void ToggleFavorite(int rowIndex);

    // Remove all non-favorite entries from memory and rewrite the file.
    void ClearHistoryKeepFavorites();

    // Returns the full MRU list (index 0 = most recent).
    const std::vector<std::wstring> &GetFolderHistory();

} // namespace UI
