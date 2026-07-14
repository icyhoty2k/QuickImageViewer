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
        bool    OnKeyDown(WPARAM vk, bool ctrl, bool shift, bool alt) override;
        LRESULT HandlePanelMessage(UINT message, WPARAM wParam, LPARAM lParam) override;

    private:
        wchar_t m_input[8]  = {};
        int     m_inputLen  = 0;
        int     m_total     = 0;
        bool    m_outOfRange = false;

        void CommitJump();

        ~JumpToWnd() {
            if (m_hFont)      DeleteObject(m_hFont);
            if (m_hFontInput) DeleteObject(m_hFontInput);
        }

        HFONT m_hFont        = nullptr;
        HFONT m_hFontInput   = nullptr;
        int   m_cachedFontDpi = 0;
    };
} // namespace UI
