#include "UIManager.h"

UI::UIManager uiManager;

namespace UI {
    void UIManager::Init(HINSTANCE hInstance, HWND hMainWnd) {
        m_hInstance = hInstance;
        m_hMainWnd = hMainWnd;

        // Init all panels once here so they are ready before first toggle
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
