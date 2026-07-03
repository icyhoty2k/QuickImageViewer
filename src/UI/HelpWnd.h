#pragma once
#include <windows.h>
#include <string>
#include "IPanelWindow.h"

namespace UI {
    class HelpWnd : public IPanelWindow {
        public:
            void Init(HINSTANCE hInstance, HWND hParent) override;

            void Init(HINSTANCE hInstance, HWND hParent, int8_t position) override;

            void Show() override;

        protected:
            LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) override;

        private:
            // Formerly a global variable
            std::wstring m_fullTitle;
    };
} // namespace UI
