class IPanelWindow {
    public:
        virtual ~IPanelWindow() = default;

        void Hide()   { if (m_hWnd && IsWindowVisible(m_hWnd)) ShowWindow(m_hWnd, SW_HIDE); }
        void Show()   { if (m_hWnd) ShowWindow(m_hWnd, SW_SHOW); }
        void Toggle() { IsWindowVisible(m_hWnd) ? Hide() : Show(); }
        bool IsVisible() const { return m_hWnd && IsWindowVisible(m_hWnd); }
        HWND GetHwnd() const { return m_hWnd; }

    protected:
        HWND m_hWnd = nullptr;
};
