#include "UIManager.h"

UI::UIManager uiManager;

namespace UI {
    void UIManager::Init(HINSTANCE hInstance, HWND hMainWnd) {
        m_hInstance = hInstance;
        m_hMainWnd = hMainWnd;

        // Init all panels once here so they are ready before first toggle
        helpWnd.Init(hInstance, hMainWnd);
        cacheWnd.Init(hInstance, hMainWnd, 0);
        dirWnd.Init(hInstance, hMainWnd);
        historyListWnd.Init(hInstance, hMainWnd);
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

    void UIManager::HideAll() {
        Hide(helpWnd);
        Hide(cacheWnd);
        Hide(dirWnd);
        Hide(historyListWnd);
    }

    HelpWnd &UIManager::getHelpWindow() {
        return helpWnd;
    }

    CacheWnd &UIManager::getCacheWindow() {
        return cacheWnd;
    }

    DirWnd &UIManager::getDirWindow() {
        return dirWnd;
    }

    HistoryListWnd &UIManager::getHistoryListWindow() {
        return historyListWnd;
    }
} // namespace UI
