#include "UIManager.h"
#include "../Platform/FileHandler.h"

UI::UIManager uiManager;

namespace UI {
    void UIManager::Init(HINSTANCE hInstance, HWND hMainWnd) {
        m_hInstance = hInstance;
        m_hMainWnd = hMainWnd;

        // Load saved folder history into RAM before any folder is opened.
        // PushFolderHistory is called by FileHandler during startup image open;
        // if the data isn't in RAM yet, the first push will append a new entry
        // but won't know what's already in the file, so nothing is lost.
        // The full file is loaded here so duplicate detection works correctly.
        LoadFolderHistoryFromDisk();
    }

    void UIManager::Show(IPanelWindow &panel) {
        panel.Show();
    }

    void UIManager::Hide(IPanelWindow &panel) {
        panel.Hide();
    }

    void UIManager::Toggle(IPanelWindow &panel) {
        panel.Toggle(); // panels are already initialized in Init()
    }

    void UIManager::HideAllPanelWindows() {
        Hide(helpWnd);
        Hide(cacheWnd);
        Hide(dirWnd);
        Hide(historyListWnd);
        HideAllSpawnedDirWnds();
    }

    HelpWnd &UIManager::getHelpWindow() {
        if (isInit(helpWnd)) {
            return helpWnd;
        }
        helpWnd.Init(m_hInstance, m_hMainWnd);
        return helpWnd;
    }

    CacheWnd &UIManager::getCacheWindow() {
        if (isInit(cacheWnd)) {
            return cacheWnd;
        }
        cacheWnd.Init(m_hInstance, m_hMainWnd, 3);
        return cacheWnd;
    }

    DirWnd &UIManager::getDirWindow() {
        if (isInit(dirWnd)) {
            return dirWnd;
        }
        dirWnd.Init(m_hInstance, m_hMainWnd, Constants::CURRENT_DIR_WINDOW_POSITION);
        return dirWnd;
    }

    HistoryListWnd &UIManager::getHistoryListWindow() {
        if (isInit(historyListWnd)) {
            return historyListWnd;
        }
        historyListWnd.Init(m_hInstance, m_hMainWnd);
        return historyListWnd;
    }

    // -------------------------------------------------------------------------
    // SpawnDirWndForFolder
    //
    // Called by HistoryListWnd when the user presses Shift+Enter on a row.
    // Assigns the next round-robin slot (0=left, 1=right, 2=center), allocates
    // the DirWnd on first use, and loads the requested folder into it.
    // The primary (F5) DirWnd is never touched here.
    // -------------------------------------------------------------------------
    void UIManager::SpawnDirWndForFolder(const std::wstring &folderPath) {
        int slot = m_nextSpawnSlot;
        m_nextSpawnSlot = (m_nextSpawnSlot + 1) % Constants::DIR_WND_MAX_INSTANCES;

        int8_t position = Constants::DIR_WND_SPAWN_POSITIONS[slot];

        DirWnd *target = m_spawnedDirWnds[slot];

        if (target == nullptr) {
            // First time this slot is used — allocate and initialise the window.
            target = new DirWnd();
            target->Init(m_hInstance, m_hMainWnd, position);
            m_spawnedDirWnds[slot] = target;
        }

        // Load the chosen folder into this DirWnd instance.
        // OpenDirectory updates app.playlist then calls UpdateDirView on the
        // primary window via the callback — the spawned window needs its own
        // explicit update after the playlist is ready.
        OpenDirectory(m_hMainWnd, folderPath);
        target->UpdateDirView();
        target->SyncDirSelectionRectangle();
        target->Show();
    }

    // -------------------------------------------------------------------------
    // HideAllSpawnedDirWnds
    //
    // Hides all three spawned slot windows (e.g. on Esc / HideAllPanelWindows).
    // Does not destroy them — they are reused on the next Shift+Enter.
    // -------------------------------------------------------------------------
    void UIManager::HideAllSpawnedDirWnds() {
        for (int i = 0; i < Constants::DIR_WND_MAX_INSTANCES; ++i) {
            if (m_spawnedDirWnds[i] != nullptr) {
                m_spawnedDirWnds[i]->Hide();
            }
        }
    }

    bool UIManager::isInit(IPanelWindow &panel) {
        return panel.GetHwnd() != nullptr;
    }
}
