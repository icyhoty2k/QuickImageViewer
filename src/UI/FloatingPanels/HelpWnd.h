#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include "FloatingPanelWnd.h"
#include "CustomControls/InputBox.h"
#include "Common/FuzzyMatch.h"

namespace UI {
    // =========================================================================
    // HelpWnd — scrollable, sectioned shortcut & CLI reference.
    //
    // Layout: 2-column rows. Left column = shortcut chord (yellow, bold),
    // right column = word-wrapped description (may span multiple lines).
    // Row heights are measured per entry with DT_CALCRECT and cached until
    // the window width or DPI changes.
    //
    // All shortcut labels are built at runtime from Shortcuts.h constants
    // (via KeyName/MapVirtualKey) and numeric values from Constants.h, so
    // remapping a key automatically updates the help text.
    //
    // Rendering is fully double-buffered (memory DC + BitBlt, WM_ERASEBKGND
    // swallowed) — no flicker on scroll or repaint.
    // =========================================================================
    class HelpWnd : public FloatingPanelWnd {
        public:
            void Init(HINSTANCE hInstance, HWND hParent) override;
            void Init(HINSTANCE hInstance, HWND hParent, int8_t position) override;
            void Show() override;

            ~HelpWnd() {
                DestroyFonts();
                DestroyBackBuffer();
            }

        protected:
            bool    OnKeyDown(WPARAM vk, bool ctrl, bool shift, bool alt) override;
            bool    OnLocalHide() override;
            LRESULT HandlePanelMessage(UINT message, WPARAM wParam, LPARAM lParam) override;

        private:
            struct HelpEntry {
                std::wstring shortcut;    // left column (yellow)
                std::wstring description; // right column (white, wraps)
                int sectionId;
            };

            // Live filter (reuses the shared InputBox control). Fuzzy/wildcard
            // match on shortcut + description (same matcher as Find/History); only
            // sections with a matching entry are shown. Matched characters in the
            // single-line shortcut chord are highlighted via Common::DrawMatchText
            // (descriptions word-wrap, so they filter but are not per-char colored).
            InputBox     m_filter;
            std::wstring m_query;                // lowercased filter text ("" = no filter)
            int          m_visibleContentHeight = 0; // filtered content height (scroll math)

            // Per-entry match state, parallel to m_entries, recomputed on query change.
            struct EntryMatch {
                bool visible = true;
                int  scCount = 0;                                   // # shortcut match positions
                int  scPos[Common::FUZZY_MAX_QUERY] = {};           // positions into the shortcut
            };
            std::vector<EntryMatch> m_entryMatch;

            void RebuildFilter();                    // recompute m_entryMatch from m_query
            bool EntryVisible(size_t i) const;
            bool SectionHasVisible(int sectionId) const;

            struct HelpSection {
                std::wstring icon;
                std::wstring title;
                std::wstring subtitle; // one-line gray explanation under the header
            };

            // Build sections + entries from Shortcuts.h / Constants.h values
            void BuildHelpContent();

            // Export the full help (sections + entries) to a Desktop text file
            void ExportToText() const;

            // Fonts are cached per DPI
            void EnsureFonts(UINT dpi);
            void DestroyFonts();

            // Measure all row/header heights for the given content width.
            // Cached — re-runs only when width or DPI changes.
            void MeasureContent(HDC hdc, int keyColW, int descColW, UINT dpi);

            // Double buffer management
            void EnsureBackBuffer(HDC refDC, int w, int h);
            void DestroyBackBuffer();

            // Content
            std::wstring m_fullTitle;
            std::vector<HelpSection> m_sections;
            std::vector<HelpEntry> m_entries;

            // Measured layout cache
            std::vector<int> m_rowHeights;    // one per entry
            std::vector<int> m_headerHeights; // one per section (title + subtitle block)
            int m_measuredWidth = 0;          // content width the cache was measured for
            int m_measuredDpi = 0;
            int m_totalContentHeight = 0;

            // Geometry cached during paint, reused by scroll/drag/key handlers
            int m_contentTop = 0;
            int m_viewHeight = 1;

            // Scrolling state
            int m_scrollOffsetY = 0;
            bool m_sbDragging = false;
            int m_sbDragStartY = 0;
            int m_sbDragStartOffset = 0;

            // Footer link rect
            RECT m_footerLinkRect = {0, 0, 0, 0};

            // Shortcut column font is slightly larger than description font
            static constexpr float KEY_FONT_SCALE = 1.1f;

            HFONT m_hFontTitle      = nullptr;
            HFONT m_hFontSubtitle   = nullptr;
            HFONT m_hFontSection    = nullptr;
            HFONT m_hFontSectionSub = nullptr;
            HFONT m_hFontShortcut   = nullptr;
            HFONT m_hFontDesc       = nullptr;
            HFONT m_hFontFooter     = nullptr;
            int   m_cachedFontDpi   = 0;

            // Double buffer
            HDC     m_memDC     = nullptr;
            HBITMAP m_memBmp    = nullptr;
            HBITMAP m_memBmpOld = nullptr;
            int m_bufW = 0;
            int m_bufH = 0;
    };
} // namespace UI
