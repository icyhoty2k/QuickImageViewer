//
// Created by aOffice on 03-Jul-26.
//

#include "UIManager.h"

#include "HelpWindow.h"
// Define the actual memory for the global pointer
UI::HelpWindow g_helpWindow;


void UIManager::Init(HINSTANCE hInstance, HWND hMainWnd) {
    m_hInstance = hInstance; // Save the instance
    m_hMainWnd = hMainWnd; // Save the main window handle
}

void UIManager::Show(UI::IPanelWindow panelWindow) {
    // 1. If we are dealing with the help window, handle lazy init
    // We check if the passed pointer matches the global one (or check by logic)
    if (panelWindow == g_helpWindow) {
        if (!g_helpWindow) {
            g_helpWindow = std::make_unique<UI::HelpWindow>();
            // You need to pass your cached hInstance/hMainWnd here
            g_helpWindow->Init(m_hInstance, m_hMainWnd);
        }
    }

    // 2. Now perform the show
    if (panelWindow) {
        panelWindow->Show();
    }
}

void UIManager::Hide(UI::IPanelWindow *panelWindow) {
    if (panelWindow && panelWindow->IsVisible()) {
        panelWindow->Hide();
    }
}

void UIManager::Toggle(UI::IPanelWindow &panelWindow) {
    if (!panelWindow) {
        getWindow(panelWindow);
    }

    if (panelWindow->IsVisible()) {
        Hide(panelWindow);
    } else {
        Show(panelWindow);
    }
}

// The dedicated getter that safely handles lazy initialization
void UIManager::getWindow(UI::IPanelWindow &panelWindow) {
    if (!g_helpWindow!
    )
    {
        g_helpWindow = std::make_unique<UI::HelpWindow>();
        g_helpWindow->Init(m_hInstance, m_hMainWnd);
    }
}
