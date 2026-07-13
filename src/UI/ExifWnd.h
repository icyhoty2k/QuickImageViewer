#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <set>
#include "IPanelWindow.h"

namespace UI {
    class ExifWnd : public IPanelWindow {
    public:
        void Init(HINSTANCE hInstance, HWND hParent) override;
        void Init(HINSTANCE hInstance, HWND hParent, int8_t position) override;
        void Show() override;
        // Called when the displayed image changes while the window is open.
        // Queues EXIF reading on the IO thread — zero UI-thread cost.
        void Refresh();

    protected:
        LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) override;

    private:
        struct ExifRow {
            std::wstring label;
            std::wstring value;
            bool isSection = false;
            std::wstring action; // non-empty: URL opened on plain click instead of copy
        };

        // Builds EXIF rows for the given image — no AppState access except wicFactory.
        // Safe to call from any COM-initialized thread.
        static std::vector<ExifRow> GatherExifRows(const std::wstring& path, int imgW, int imgH);

        void LoadExifData(); // thin wrapper for the Show() (UI-thread) path

        std::vector<ExifRow> m_rows;
        int  m_scrollOffsetY      = 0;
        int  m_totalContentHeight = 0;
        bool  m_sbDragging         = false;
        int   m_sbDragStartY       = 0;
        int   m_sbDragStartOffset  = 0;
        bool  m_moving             = false;
        POINT m_moveStartCursor    = {};
        RECT  m_moveStartRect      = {};
        int   m_anchorRow          = -1;
        std::set<int> m_selectedRows;
    };
} // namespace UI
