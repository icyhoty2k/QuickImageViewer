#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include "FloatingPanelWnd.h"

namespace UI {
    class HelpWnd : public FloatingPanelWnd {
        public:
            void Init(HINSTANCE hInstance, HWND hParent) override;

            void Init(HINSTANCE hInstance, HWND hParent, int8_t position) override;

            void Show() override;

        protected:
            bool    OnKeyDown(WPARAM vk, bool ctrl, bool shift, bool alt) override;
            LRESULT HandlePanelMessage(UINT message, WPARAM wParam, LPARAM lParam) override;

        private:
            struct HelpEntry {
                std::wstring shortcut;  // Yellow text
                std::wstring description; // White text
                int sectionId;  // Which section (color group)
            };

            // Build help content from constants
            void BuildHelpContent();

            // Export help text to file
            void ExportToText() const;

            // Content storage
            std::wstring m_fullTitle;
            std::vector<HelpEntry> m_entries;

            // Scrolling state
            int m_scrollOffsetY = 0;
            int m_totalContentHeight = 0;
            bool m_sbDragging = false;
            int m_sbDragStartY = 0;
            int m_sbDragStartOffset = 0;

            // Footer link rect
            RECT m_footerLinkRect = {0, 0, 0, 0};

            // Layout settings
            static constexpr int COLUMNS = 3;
            static constexpr float SHORTCUT_SIZE_MULTIPLIER = 1.3f;  // Yellow is 1.3x bigger
    };
} // namespace UI
