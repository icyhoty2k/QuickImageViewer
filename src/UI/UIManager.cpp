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
        dirWnd.Init(m_hInstance, m_hMainWnd, 1);
        return dirWnd;
    }

    HistoryListWnd &UIManager::getHistoryListWindow() {
        if (isInit(historyListWnd)) {
            return historyListWnd;
        }
        historyListWnd.Init(m_hInstance, m_hMainWnd);
        return historyListWnd;
    }

    bool UIManager::isInit(IPanelWindow &panel) {
        return panel.GetHwnd() != nullptr;
    }
}
