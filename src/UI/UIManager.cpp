#include "UIManager.h"

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
    // a SpawnedDirWnd on first use, and loads the folder into its private
    // playlist.  app.playlist and the main viewer are NOT touched.
    // The primary (F5) DirWnd is never touched here.
    // -------------------------------------------------------------------------
    void UIManager::SpawnDirWndForFolder(const std::wstring &folderPath, HWND hHistoryWnd) {
        int slot = m_nextSpawnSlot;
        m_nextSpawnSlot = (m_nextSpawnSlot + 1) % Constants::DIR_WND_MAX_INSTANCES;

        int8_t position = Constants::DIR_WND_SPAWN_POSITIONS[slot];

        SpawnedDirWnd *target = m_spawnedDirWnds[slot];

        if (target == nullptr) {
            target = new SpawnedDirWnd(slot);
            target->Init(m_hInstance, m_hMainWnd, position);
            m_spawnedDirWnds[slot] = target;
        }

        target->LoadFolder(folderPath);

        // Show the window without stealing focus from HistoryListWnd.
        // ShowWindow with SW_SHOWNOACTIVATE makes it visible without activating.
        // UpdateDirView must be called after the window is visible so that
        // IsWindowVisible() returns true inside UpdateView().
        ShowWindow(target->GetHwnd(), SW_SHOWNOACTIVATE);
        target->UpdateDirView();

        // Return keyboard focus to the history panel so the user can keep
        // navigating and spawning more folders without re-opening it.
        if (hHistoryWnd) {
            SetForegroundWindow(hHistoryWnd);
            SetFocus(hHistoryWnd);
        }
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
