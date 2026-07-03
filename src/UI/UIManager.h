#pragma once
#include <windows.h>

#include "IPanelWindow.h"
#include "HelpWnd.h"
#include "CacheWnd.h"
#include "CurrDirWnd.h"


namespace UI {
    class UIManager {
        public:
            void Init(HINSTANCE hInstance, HWND hMainWnd);

            // Pass the panel by reference. No enums needed.
            void Toggle(IPanelWindow &panel);

            void Show(IPanelWindow &panel);

            void Hide(IPanelWindow &panel);

            void HideAll();

            HelpWnd &getHelpWindow();

            CacheWnd &getCacheWindow();

            CurrDirWnd &getDirWindow();


            // --- Stack-Allocated Panels ---
            // Publicly accessible so you can pass them to the methods above.

            // CacheThumbnailsWindow cacheWindow;
            // CurrentDirThumbnailsWindow dirWindow;
            // HistoryListDirWindow historyWindow;

        private:
            HelpWnd helpWnd;
            CacheWnd cacheWnd;
            CurrDirWnd currDirWnd;

            HINSTANCE m_hInstance = nullptr;
            HWND m_hMainWnd = nullptr;
    };
}

extern UI::UIManager uiManager;
