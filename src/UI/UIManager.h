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
    class UIManager {
        public:
            void Init(HINSTANCE hInstance, HWND hMainWnd);

            // Pass the panel by reference. No enums needed.
            void Toggle(IPanelWindow &panel);

            void Show(IPanelWindow &panel);

            void Hide(IPanelWindow &panel);

            void HideAllPanelWindows();

            HelpWnd &getHelpWindow();

            CacheWnd &getCacheWindow();

            // Returns the primary (F5) DirWnd — always independent of spawned ones.
            DirWnd &getDirWindow();

            HistoryListWnd &getHistoryListWindow();

            // Spawn a DirWnd for 'folderPath' from the history panel (Shift+Enter).
            // Uses slots 0-2 in round-robin order (left, right, center).
            // If all 3 slots are already occupied the oldest one is reused.
            // The primary (F5) DirWnd is never touched.
            void SpawnDirWndForFolder(const std::wstring &folderPath, HWND hHistoryWnd);

            // Hide all 3 spawned DirWnd instances (called from HideAllPanelWindows).
            void HideAllSpawnedDirWnds();

        private:
            HelpWnd helpWnd;
            CacheWnd cacheWnd;

            // Primary DirWnd — owned by F5, always top strip (position 1).
            // Never affected by SpawnDirWndForFolder.
            DirWnd dirWnd;

            HistoryListWnd historyListWnd;

            // Spawned DirWnd slots (history Shift+Enter).
            // Slot 0 → left (position 4)
            // Slot 1 → right (position 2)
            // Slot 2 → center floating (position 0)
            // Pointers are nullptr until first use; memory owned here.
            std::array<SpawnedDirWnd *, Constants::DIR_WND_MAX_INSTANCES> m_spawnedDirWnds = {nullptr, nullptr, nullptr};

            // Index of the next slot to use (round-robin, 0-2).
            int m_nextSpawnSlot = 0;

            HINSTANCE m_hInstance = nullptr;
            HWND m_hMainWnd = nullptr;

            bool isInit(IPanelWindow &panel);
    };
}

extern UI::UIManager uiManager;
