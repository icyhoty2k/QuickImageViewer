// UIManager.h
#pragma once
#include <windows.h>
#include <memory>


#include "IPanelWindow.h"


namespace UI {
    class HelpWindow;
}

// Declaration of the global instance
extern UI::HelpWindow g_helpWindow;

class UIManager {
    public:
        void Init(HINSTANCE hInstance, HWND hMainWnd);

        // Public triggers mapped to your shortcuts
        void Toggle(UI::IPanelWindow &panelWindow);

        void Show(UI::IPanelWindow *panelWindow);

        void Hide(UI::IPanelWindow *panelWindow);

        void HideAll(UI::IPanelWindow *panelWindow);

        void getWindow(UI::IPanelWindow &panelWindow);

    private:
        HINSTANCE m_hInstance = nullptr;
        HWND m_hMainWnd = nullptr;
};
