#pragma once
#include <windows.h>
#include <array>
#include <string>
#include <vector>

#include "IPanelWindow.h"
#include "FloatingPanels/HelpWnd.h"
#include "ThumbnailPanels/CacheWnd.h"
#include "ThumbnailPanels/DirWnd.h"
#include "FloatingPanels/HistoryListWnd.h"
#include "FloatingPanels/ExifWnd.h"
#include "FloatingPanels/JumpToWnd.h"
#include "FloatingPanels/ZoomWnd.h"
#include "FloatingPanels/FindWnd.h"
#include "FloatingPanels/StatsWnd.h"
#include "FloatingPanels/ZoomWnd.h"
#include "Dedicated/DedicatedWnd.h"
#include "Rem_TCP_IP/RemoteWnd.h"
#include "Rem_TCP_IP/RemoteClientsWnd.h"
#include "Rem_TCP_IP/RemotesWnd.h"
#include "Rem_TCP_IP/MirrorPickerWnd.h"
#include "Rem_TCP_IP/RemoteLogWnd.h"
#include "Rem_TCP_IP/RemoteCmdWnd.h"
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

        // Returns the next free position after 'current', cycling 0-4.
        // Can return center (position 0) as fallback for F5 DirWnd.
        // Returns -1 if all positions are occupied.
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
            // Re-opens exactly the panels that HideAllPanelWindows last hid — the
            // inverse of "Close All Panels". No-op if the snapshot is empty.
            void RestoreAllPanels();
            // True if any floating panel or spawned DirWnd is currently visible.
            bool AnyPanelVisible() const;

            HelpWnd         &getHelpWindow();
            CacheWnd        &getCacheWindow();
            DirWnd          &getDirWindow();
            ExifWnd         &getInfoWindow();
            void             RefreshInfoWindowIfVisible();
            void             RefreshStatsWindowIfVisible();

            // Returns whichever DirWnd the user last clicked in.
            // Defaults to the primary F5 DirWnd.
ThumbnailPanelWnd &getActiveDirWnd();

            // Called by ThumbnailPanelWnd on LButtonUp to make it the active panel.
            void SetActiveDirWnd(ThumbnailPanelWnd *panel);
            HistoryListWnd  &getHistoryListWindow();
            JumpToWnd       &getJumpToWindow();
            ZoomWnd         &getZoomWindow();
            FindWnd         &getFindWindow();
            void             ToggleJumpToWindow();   // hides FindWnd if visible, then toggles JumpToWnd
            void             ToggleFindWindow();     // hides JumpToWnd if visible, then toggles FindWnd
            void             ToggleZoomWindow();     // toggles ZoomWnd
            StatsWnd        &getStatsWindow();
            DedicatedWnd    &getDedicatedWindow();
            RemoteWnd       &getRemoteWindow();
            RemoteClientsWnd &getRemoteClientsWindow();
            RemotesWnd      &getRemotesConsoleWindow();
            MirrorPickerWnd &getMirrorPickerWindow();
            RemoteLogWnd    &getRemoteLogWindow();
            RemoteCmdWnd    &getRemoteCmdWindow();
            void             ApplyAlwaysOnTop(bool onTop);

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

            // Returns the visible spawned panel whose folder matches folderPath, or nullptr.
            // Only checks the spawned pool — never returns the primary F6 DirWnd.
            SpawnedDirWnd *FindSpawnedDirWnd(const std::wstring &folderPath) const;

            // Get {sizeStr, imageCount} from the open SpawnedDirWnd showing folderPath.
            // Returns {"", 0} when no SpawnedDirWnd is open for that path.
            std::pair<std::wstring, int> GetSpawnedDirWndSizeInfo(const std::wstring &folderPath) const;

            // Sum size (bytes) and image count across ALL currently-visible DirWnds (F6 + spawned).
            // Returns {"", 0} when no DirWnd is open.
            std::pair<std::wstring, int> GetAllOpenDirWndsSummary() const;

            // Called by HandleScanComplete after every directory scan (including empty
            // results). Fans out to every visible panel so each can decide whether to
            // sync its playlist, show the empty-dir placeholder, or refresh its view.
            void NotifyFolderRefreshed(const std::wstring &dir,
                                       const std::vector<std::wstring> &playlist,
                                       bool updatePrimaryDirWnd = true);

            void RepaintAllPanels();
            void RefreshPanelDirs(const std::wstring &dir1, const std::wstring &dir2);

            // Update DWM title-bar theme and repaint all floating panel windows.
            // Call whenever app.isDarkThemed or app.themeFactor changes.
            void NotifyThemeChanged();

        private:
            HelpWnd        helpWnd;
            CacheWnd       cacheWnd;
            DirWnd         dirWnd;
            HistoryListWnd historyListWnd;
            ExifWnd        exifWnd;
            JumpToWnd      jumpToWnd;
            ZoomWnd        zoomWnd;
            FindWnd        findWnd;
            StatsWnd       statsWnd;
            DedicatedWnd   dedicatedWnd;
            RemoteWnd      remoteWnd;
            RemoteClientsWnd remoteClientsWnd;
            RemotesWnd     remotesWnd;
            MirrorPickerWnd mirrorPickerWnd;
            RemoteLogWnd    remoteLogWnd;
            RemoteCmdWnd    remoteCmdWnd;

            // =================================================================
            // EVERY fixed floating panel, listed ONCE.
            //
            // Two functions walk this set — HideAllPanelWindows() and
            // AnyPanelVisible() — and each used to carry its own copy of it.
            // They drifted, and silently: both listed only the original eight,
            // so Find, the Dedicated panel, F9 (Local Server), F10 (Remote
            // Servers), the mirror picker, Ctrl+F10 (Send Command) and Ctrl+F12
            // (wire log) were left on screen by "close all panels" — and
            // AnyPanelVisible reported nothing was open while they were, which
            // also made ToggleAllPanels restore instead of close.
            //
            // A panel added to the class but not to a list is the failure this
            // prevents: there is now one place to forget, not two.
            //
            // Declared AFTER the panels themselves because a default member
            // initializer may only take the address of an already-declared
            // member. The spawned DirWnd pool is deliberately not here — it has
            // its own array and its own IsPanelVisible().
            // =================================================================
            static constexpr size_t FIXED_PANEL_COUNT = 16;
            IPanelWindow *const m_fixedPanels[FIXED_PANEL_COUNT] = {
                &helpWnd,     &cacheWnd,       &dirWnd,       &historyListWnd,
                &exifWnd,     &jumpToWnd,      &zoomWnd,      &findWnd,
                &statsWnd,    &dedicatedWnd,   &remoteWnd,    &remoteClientsWnd,
                &remotesWnd,  &mirrorPickerWnd, &remoteLogWnd, &remoteCmdWnd
            };

            // Fixed pool of 4 pre-allocated SpawnedDirWnd instances — one per layout slot
            // (top, left, right, bottom). Reused across spawns; never deleted at runtime.
            SpawnedDirWnd m_spawned0{0};
            SpawnedDirWnd m_spawned1{1};
            SpawnedDirWnd m_spawned2{2};
            SpawnedDirWnd m_spawned3{3};
            SpawnedDirWnd* const m_spawnedPool[Constants::DIR_WND_MAX_INSTANCES] {
                &m_spawned0, &m_spawned1, &m_spawned2, &m_spawned3
            };

            HINSTANCE m_hInstance  = nullptr;
            HWND      m_hMainWnd   = nullptr;

            // The layout struct — single source of truth for all panel positions.
            PanelLayout m_layout;

            // Whichever DirWnd the user last clicked — receives navigation updates.
            // nullptr = use primary dirWnd.
            ThumbnailPanelWnd *m_activeDirWnd = nullptr;

            // Snapshot of panels visible at the last HideAllPanelWindows() call,
            // replayed by RestoreAllPanels(). Only overwritten when non-empty so a
            // second "Close All Panels" doesn't wipe a still-restorable set.
            std::vector<IPanelWindow *> m_restoreList;

            bool isInit(IPanelWindow &panel);
    };
}

extern UI::UIManager uiManager;
