#pragma once
#include <windows.h>
#include <array>

#include "IPanelWindow.h"
#include "HelpWnd.h"
#include "CacheWnd.h"
#include "DirWnd.h"
#include "HistoryListWnd.h"
#include "../Platform/Constants.h"

namespace UI {

    // =========================================================================
    // PanelLayout — single source of truth for which panel occupies each slot.
    //
    // Slots:  0=center  1=top  2=right  3=bottom  4=left
    //
    // A slot is "occupied" when a visible ThumbnailPanelWnd is at that position.
    // UIManager keeps this up to date on every show/hide/move.
    // GetWindowBounds reads it to size vertical panels correctly.
    // MovePanel skips occupied slots so panels never overlap.
    // =========================================================================
    struct PanelLayout {
        ThumbnailPanelWnd *slots[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};

        void set(int8_t pos, ThumbnailPanelWnd *p) {
            if (pos >= 0 && pos <= 4) slots[pos] = p;
        }

        void clear(int8_t pos) {
            if (pos >= 0 && pos <= 4) slots[pos] = nullptr;
        }

        // Clear any slot that points to this panel (used on hide when position unknown).
        void clearPanel(ThumbnailPanelWnd *p) {
            for (int i = 0; i < 5; ++i)
                if (slots[i] == p) slots[i] = nullptr;
        }

        bool occupied(int8_t pos) const {
            return pos >= 0 && pos <= 4 && slots[pos] != nullptr;
        }

        bool topOccupied()    const { return slots[1] != nullptr; }
        bool bottomOccupied() const { return slots[3] != nullptr; }

        // Returns the next free position after 'current', cycling 0-4.
        // Returns -1 if all positions are occupied (shouldn't happen in practice).
        int8_t nextFreePosition(int8_t current) const {
            for (int i = 1; i <= 5; ++i) {
                int8_t candidate = (current + i) % 5;
                if (!occupied(candidate)) return candidate;
            }
            return -1;
        }
    };

    class UIManager {
        public:
            void Init(HINSTANCE hInstance, HWND hMainWnd);

            void Toggle(IPanelWindow &panel);
            void Show(IPanelWindow &panel);
            void Hide(IPanelWindow &panel);
            void HideAllPanelWindows();

            HelpWnd         &getHelpWindow();
            CacheWnd        &getCacheWindow();
            DirWnd          &getDirWindow();
            HistoryListWnd  &getHistoryListWindow();

            void SpawnDirWndForFolder(const std::wstring &folderPath, HWND hHistoryWnd);
            void HideAllSpawnedDirWnds();

            // Called by ThumbnailPanelWnd when a panel becomes visible at a position.
            void OnPanelShown(ThumbnailPanelWnd *panel, int8_t position);

            // Called by ThumbnailPanelWnd when a panel is hidden.
            void OnPanelHidden(ThumbnailPanelWnd *panel);

            // Called by ThumbnailPanelWnd::MovePanel to get the next free position.
            // Skips positions already occupied by another visible panel.
            int8_t NextFreePosition(int8_t currentPosition) const;

            // Read-only access to the layout for GetWindowBounds.
            const PanelLayout &GetLayout() const { return m_layout; }

            // Resize all currently-visible vertical panels to reflect
            // the current top/bottom occupation state.
            void RefreshVerticalPanels();

        private:
            HelpWnd        helpWnd;
            CacheWnd       cacheWnd;
            DirWnd         dirWnd;
            HistoryListWnd historyListWnd;

            std::array<SpawnedDirWnd *, Constants::DIR_WND_MAX_INSTANCES> m_spawnedDirWnds
                = {nullptr, nullptr, nullptr, nullptr};
            int m_nextSpawnSlot = 0;

            HINSTANCE m_hInstance  = nullptr;
            HWND      m_hMainWnd   = nullptr;

            // The layout struct — single source of truth.
            PanelLayout m_layout;

            bool isInit(IPanelWindow &panel);
    };
}

extern UI::UIManager uiManager;
