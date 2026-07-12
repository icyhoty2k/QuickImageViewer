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
    // SlotInfo — manages a single panel position with change notifications.
    // =========================================================================
    struct SlotInfo {
        std::wstring name;
        ThumbnailPanelWnd *panel = nullptr;

    private:
        bool m_isEmpty = true;

    public:
        bool isEmpty() const { return m_isEmpty; }

        void setEmpty(bool empty) {
            if (m_isEmpty != empty) {
                m_isEmpty = empty;
                OnSlotEmptyChanged(empty);
            }
        }

    private:
        void OnSlotEmptyChanged(bool empty);
    };

    // =========================================================================
    // PanelLayout — single source of truth for which panel occupies each slot.
    //
    // Positions:  0=center  1=top  2=right  3=bottom  4=left
    //
    // A slot is "occupied" when a visible ThumbnailPanelWnd is at that position.
    // UIManager keeps this up to date on every show/hide/move.
    // GetWindowBounds reads it to size vertical panels correctly.
    // MovePanel skips occupied slots so panels never overlap.
    // =========================================================================
    struct PanelLayout {
        SlotInfo center;
        SlotInfo top;
        SlotInfo right;
        SlotInfo bottom;
        SlotInfo left;

        PanelLayout() {
            center.name = L"center";
            top.name = L"top";
            right.name = L"right";
            bottom.name = L"bottom";
            left.name = L"left";
        }

        // Get slot by position index (0=center, 1=top, 2=right, 3=bottom, 4=left)
        SlotInfo *getSlot(int8_t pos) {
            switch (pos) {
                case 0: return &center;
                case 1: return &top;
                case 2: return &right;
                case 3: return &bottom;
                case 4: return &left;
                default: return nullptr;
            }
        }

        const SlotInfo *getSlot(int8_t pos) const {
            switch (pos) {
                case 0: return &center;
                case 1: return &top;
                case 2: return &right;
                case 3: return &bottom;
                case 4: return &left;
                default: return nullptr;
            }
        }

        // Get slot by position name ("center", "top", "right", "bottom", "left")
        SlotInfo *getSlotByName(const std::wstring &name) const {
            if (name == L"center") return const_cast<SlotInfo *>(&center);
            if (name == L"top") return const_cast<SlotInfo *>(&top);
            if (name == L"right") return const_cast<SlotInfo *>(&right);
            if (name == L"bottom") return const_cast<SlotInfo *>(&bottom);
            if (name == L"left") return const_cast<SlotInfo *>(&left);
            return nullptr;
        }

        void set(int8_t pos, ThumbnailPanelWnd *p) {
            SlotInfo *slot = getSlot(pos);
            if (slot) {
                slot->panel = p;
                slot->setEmpty(p == nullptr);
            }
        }

        void clear(int8_t pos) {
            SlotInfo *slot = getSlot(pos);
            if (slot) {
                slot->panel = nullptr;
                slot->setEmpty(true);
            }
        }

        // Clear any slot that points to this panel (used on hide when position unknown).
        void clearPanel(ThumbnailPanelWnd *p) {
            SlotInfo *slots[] = {&center, &top, &right, &bottom, &left};
            for (int i = 0; i < 5; ++i) {
                if (slots[i]->panel == p) {
                    slots[i]->panel = nullptr;
                    slots[i]->setEmpty(true);
                }
            }
        }

        bool occupied(int8_t pos) const {
            const SlotInfo *slot = getSlot(pos);
            return slot && slot->panel != nullptr;
        }

        bool topOccupied()    const { return top.panel != nullptr; }
        bool bottomOccupied() const { return bottom.panel != nullptr; }

        // Returns the next free position after 'current', cycling 1-4.
        // Never returns center (position 0) — center is reserved for primary panels.
        // Returns -1 if all positions 1-4 are occupied.
        int8_t nextFreePosition(int8_t current) const {
            for (int i = 1; i <= 4; ++i) {
                int8_t candidate = ((current - 1 + i) % 4) + 1;  // Cycle 1-4 only
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

            // Returns whichever DirWnd the user last clicked in.
            // Defaults to the primary F5 DirWnd.
            ThumbnailPanelWnd &getActiveDirWnd();

            // Called by ThumbnailPanelWnd on LButtonUp to make it the active panel.
            void SetActiveDirWnd(ThumbnailPanelWnd *panel);
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

            // Get position label for a spawned DirWnd folder (e.g., " (Left)", " (Right)", or empty)
            std::wstring GetSpawnedDirWndPositionLabel(const std::wstring &folderPath) const;

        private:
            HelpWnd        helpWnd;
            CacheWnd       cacheWnd;
            DirWnd         dirWnd;
            HistoryListWnd historyListWnd;

            HINSTANCE m_hInstance  = nullptr;
            HWND      m_hMainWnd   = nullptr;

            // The layout struct — single source of truth for all panel positions.
            PanelLayout m_layout;

            // Whichever DirWnd the user last clicked — receives navigation updates.
            // nullptr = use primary dirWnd.
            ThumbnailPanelWnd *m_activeDirWnd = nullptr;

            bool isInit(IPanelWindow &panel);
    };
}

extern UI::UIManager uiManager;
