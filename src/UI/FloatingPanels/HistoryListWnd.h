#pragma once

#include <windows.h>
#include <vector>
#include <string>

#include "FloatingPanelWnd.h"

namespace UI {
    class HistoryListWnd : public FloatingPanelWnd {
        // -----------------------------------------------------------------------
        // HistoryListWnd  —  Last-visited folder history panel.
        //
        // Shows up to HISTORY_MAX_DIRS_TO_SHOW regular entries plus up to
        // HISTORY_MAX_FAVORITES_TO_SHOW favorites.  Position of favorites
        // (top / bottom / in-place) is controlled by HISTORY_FAVORITES_POSITION.
        //
        // Shortcuts (active when panel is focused):
        //   Tab        — Toggle panel (SC_PANEL_HISTORY_TOGGLE)
        //   Space          — Toggle favorite on hovered row (HISTORY_FAVORITES_TOGGLE_KEY)
        //   Ctrl+Shift+Del — Clear all history except favorites (HISTORY_CLEAR_ALL_HISTORY_BUT_NOT_FAVORITES)
        //   Ctrl+Alt+Shift+Del — Clear all favorites except history (HISTORY_CLEAR_ALL_FAVORITES_BUT_NOT_HISTORY)
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
            void OnSetFocus() override;
            void OnKillFocus() override;
            bool    OnKeyDown(WPARAM vk, bool ctrl, bool shift, bool alt) override;
            bool    OnMButtonUp(int x, int y) override;
            LRESULT HandlePanelMessage(UINT message, WPARAM wParam, LPARAM lParam) override;

        public:
            ~HistoryListWnd() {
                if (m_hFontTitle)     DeleteObject(m_hFontTitle);
                if (m_hFontBody)      DeleteObject(m_hFontBody);
                if (m_hFontList)      DeleteObject(m_hFontList);
                if (m_hFontIndexLink) DeleteObject(m_hFontIndexLink);
                if (m_hFontLink)      DeleteObject(m_hFontLink);
                DestroyBackBuffer();
            }

        private:
            void EnsureBackBuffer(HDC refDC, int w, int h);
            void DestroyBackBuffer();
            HDC     m_bbDC     = nullptr;
            HBITMAP m_bbBmp    = nullptr;
            HBITMAP m_bbBmpOld = nullptr;
            int     m_bbW      = 0;
            int     m_bbH      = 0;

            void ToggleHistoryWindow();

            HFONT m_hFontTitle     = nullptr;
            HFONT m_hFontBody      = nullptr;
            HFONT m_hFontList      = nullptr;
            HFONT m_hFontIndexLink = nullptr;
            HFONT m_hFontLink      = nullptr;
            int   m_cachedFontDpi  = 0;

            std::wstring m_cachedSizeStr;

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

    // Remove all non-favorite entries from memory and rewrite qivHistory.txt only.
    void ClearHistoryKeepFavorites();

    // Remove all favorites from memory and rewrite qivFavorites.txt only.
    void ClearFavoritesKeepHistory();

    // Returns the full MRU list (index 0 = most recent).
    const std::vector<std::wstring> &GetFolderHistory();

    // Returns false if the folder does not exist or contains no supported images.
    // Result is cached per BuildDisplayList cycle — safe to call from paint or navigation.
    bool IsFolderValidForViewer(const std::wstring &folderPath);

    // Force the status cache entry for 'path' to Missing and repaint the history
    // panel if visible. Called by HandleScanComplete when F5 finds the dir gone.
    void InvalidateHistoryFolderStatus(const std::wstring &path);

    // Reset the cached status for 'path' to Unknown and repaint.
    // Call whenever the folder's contents may have changed so the panel
    // re-evaluates it on the next validation pass.
    void NotifyFolderContentsChanged(const std::wstring &path);

    // Open the history panel showing the full (uncapped) list.
    // Equivalent to Tab then Ctrl+Tab — used from the main app via Ctrl+Tab.
    void ToggleHistoryFull();

    // Horizontal-scroll navigation snapshot — taken from the raw backing array,
    // not the capped display list. Call CaptureNavigationSnapshot() to refresh;
    // version increments on each capture so callers can detect a change.
    void CaptureNavigationSnapshot();
    const std::vector<std::wstring> &GetNavigationSnapshot();
    int GetNavigationSnapshotVersion();

} // namespace UI
