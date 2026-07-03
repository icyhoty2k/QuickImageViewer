#include "UIManager.h"

UI::UIManager uiManager;

namespace UI {
    void UIManager::Init(HINSTANCE hInstance, HWND hMainWnd) {
        m_hInstance = hInstance;
        m_hMainWnd = hMainWnd;
    }

    void UIManager::Show(IPanelWindow &panel) {
        panel.Show();
    }

    void UIManager::Hide(IPanelWindow &panel) {
        panel.Hide();
    }

    void UIManager::Toggle(IPanelWindow &panel) {
        panel.Init(m_hInstance, m_hMainWnd);
        panel.Toggle();
    }

    HelpWnd &UIManager::getHelpWindow() {
        if (helpWnd.GetHwnd() == nullptr) {
            helpWnd.Init(m_hInstance, m_hMainWnd);
        }
        return helpWnd;
    }


    void UIManager::HideAll() {
        // Explicitly hide each member
        Hide(helpWnd);
        Hide(cacheWnd);
        Hide(currDirWnd);
        Hide(historyListWnd);
    }
} // namespace UI
