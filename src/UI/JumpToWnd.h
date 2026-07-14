#pragma once
#include <windows.h>
#include "FloatingPanelWnd.h"

namespace UI {
    class JumpToWnd : public FloatingPanelWnd {
    public:
        void Init(HINSTANCE hInstance, HWND hParent) override;
        void Init(HINSTANCE hInstance, HWND hParent, int8_t position) override;
        void Show() override;

    protected:
        LRESULT HandlePanelMessage(UINT message, WPARAM wParam, LPARAM lParam) override;

    private:
        wchar_t m_input[8]  = {};
        int     m_inputLen  = 0;
        int     m_total     = 0;
        bool    m_outOfRange = false;

        void CommitJump();
    };
} // namespace UI
