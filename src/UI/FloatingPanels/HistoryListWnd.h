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
        //   Ctrl+Del       — Delete the hovered row (Ctrl+Z restores it)
        //   Ctrl+Shift+Del — Clear all history except favorites (HISTORY_CLEAR_ALL_HISTORY_BUT_NOT_FAVORITES)
        //   Ctrl+Alt+Shift+Del — Clear all favorites except history (HISTORY_CLEAR_ALL_FAVORITES_BUT_NOT_HISTORY)
        //   Up/Down    — Move selection
        //   Enter      — Open selected folder
        //   Esc        — Clear filter if typed, else hide panel
        //
        // NOT handled here — forwarded to the main app so they behave the same
        // with the panel open as without it:
        //   Home / End / PageUp / PageDown / Insert / Delete
        // These are the image- and folder-navigation cluster; walking the list
        // while looking at it is the point. While the filter box holds text,
        // Home / End / Delete revert to caret and forward-delete until Esc
        // clears it. Row deletion moved to Ctrl+Del to free plain Delete.
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
            bool    OnLocalHide() override;
            bool    OnMButtonUp(int x, int y) override;
            LRESULT HandlePanelMessage(UINT message, WPARAM wParam, LPARAM lParam) override;

            // The scroll state is a file static, like everything else in this
            // panel's implementation, so these are defined in the .cpp where it
            // is visible. The base drives both wheels, the thumb drag, track
            // paging and the bar cursor against what they return.
            UI::ScrollView *ScrollViewAt(POINT) override;
            int ScrollLinePx(const UI::ScrollView &) const override;

            // Every scroll moves rows under the cursor, so the hover popup now
            // describes a row that is not there. Dropped here rather than in a
            // wheel case, because paging the track and dragging the thumb do it
            // just as much and only this hook sees all three.
            void OnScrolled() override;

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

            // Re-reads qivHistory.txt's size into m_cachedSizeStr. Called on open
            // and on F5 — the file changes as folders are visited and cleared.
            void RefreshCachedFileSize();

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

    // Starts the folder sweep in the background, without opening the panel.
    //
    // Called at startup so the work happens while the user is browsing images.
    // By the time they press Tab the list is usually already validated, instead
    // of the panel opening and only then beginning to walk every folder.
    // No-op if the panel window does not exist yet, or nothing needs scanning.
    void StartBackgroundHistoryScan();

    // Called by FileHandler after every successful folder load.
    void PushFolderHistory(const std::wstring &folderPath);

    // Tells the panel which folder the MAIN VIEWER is now showing, and repaints
    // it if visible. This is what drives the green "you are here" row.
    //
    // Needed because the panel cannot reliably work that out for itself: it used
    // to derive the folder from app.playlist, but a missing or empty folder
    // leaves the playlist cleared (or still describing the previous folder), so
    // the marker stopped following as soon as a walk passed through one. The
    // viewer knows; it just has to say so.
    //
    // Does NOT reorder the list or count as user navigation — purely "the green
    // row is now here".
    void NotifyCurrentFolder(const std::wstring &folderPath);

    // Toggle favorite status on the path at display index 'rowIndex'.
    void ToggleFavorite(int rowIndex);

    // Remove all non-favorite entries from memory and rewrite qivHistory.txt only.
    void ClearHistoryKeepFavorites();

    // Remove all favorites from memory and rewrite qivFavorites.txt only.
    void ClearFavoritesKeepHistory();

    // Returns the full MRU list (index 0 = most recent).
    const std::vector<std::wstring> &GetFolderHistory();

    // True when two paths name the SAME directory on disk, following junctions,
    // directory symlinks and subst drives. Uses the panel's cached link info, so
    // a repeat comparison is a hash lookup rather than filesystem work.
    //
    // Exists because paths are no longer canonicalized on the way in: qIV keeps
    // whichever spelling the user actually used (D:\... stays D:\...), which means
    // "is this the folder I am already in?" can no longer be answered by comparing
    // strings.
    bool SameRealFolder(const std::wstring &a, const std::wstring &b);

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

    // Re-applies app.historyFullModeEnabled to an already-open panel: rebuilds
    // the row set, refits the window, repaints. Call after changing that flag
    // from OUTSIDE this file (the tray's "History: Open Full List" item), which
    // would otherwise leave the panel showing the old row set until something
    // else happened to invalidate it. No-op when the panel is not visible.
    void RefreshHistoryFullMode();

    // Which rows a walk is allowed to land on. The History panel's list is one
    // list containing both kinds of row; these are three disjoint-or-total views
    // of it, not three lists.
    enum class WalkScope {
        All,              // horizontal mouse wheel — every row the panel shows
        NonFavoritesOnly, // PageUp / PageDown
        FavoritesOnly,    // Insert / Delete
    };

    // THE one folder-walking function. Every caller — horizontal wheel, the
    // history keys and the favorite keys — goes through here, so all three
    // behave identically.
    //
    //   scope    which rows are eligible (see above)
    //   reverse  false = down the list (next), true = up the list (previous)
    //
    // Walks a FROZEN snapshot of the panel's display list. That matters because
    // opening a folder promotes it to the front of the MRU store, which would
    // otherwise reshuffle the list mid-walk and make every step ping-pong
    // between two folders. The snapshot is re-taken only when it goes stale:
    // the current folder is no longer the one this walk opened (you navigated
    // some other way), the panel's full/short mode changed, or the list itself
    // was edited (favorite toggled, history cleared). A plain MRU promotion is
    // deliberately NOT a staleness trigger — that is the reordering we ignore.
    //
    // Which rows exist comes from the panel's own live full/short state, so a
    // walk never visits a folder the panel is not currently showing. There is
    // no parameter for it — a caller-supplied flag could disagree with the panel.
    //
    // Wraps at both ends and never re-opens the folder already on screen.
    // MISSING folders are stepped over (reported in the centre message); EMPTY
    // folders are OPENED, matching what Enter on that row does — they exist, so
    // navigating to one is real navigation and the panel, the green row and the
    // MRU order all stay in agreement. Returns false (and posts a centre message)
    // if nothing is eligible.
    bool WalkHistoryFolder(HWND hOwner, WalkScope scope, bool reverse);

    // Invalidates any in-progress walk snapshot. Call after an edit that changes
    // WHICH rows exist or which category a row is in — toggling a favorite,
    // clearing history, clearing favorites. A plain MRU reorder must NOT call
    // this; ignoring reorders is the entire point of the snapshot.
    void InvalidateWalkSnapshot();

} // namespace UI
