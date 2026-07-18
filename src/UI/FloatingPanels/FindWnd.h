#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include "FloatingPanelWnd.h"

namespace UI {
    class FindWnd : public FloatingPanelWnd {
    public:
        void Init(HINSTANCE hInstance, HWND hParent) override;
        void Init(HINSTANCE hInstance, HWND hParent, int8_t position) override;
        void Show() override;
        ~FindWnd() {
            if (m_hFontNorm) DeleteObject(m_hFontNorm);
            if (m_hFontBold) DeleteObject(m_hFontBold);
        }

    protected:
        bool    OnKeyDown(WPARAM vk, bool ctrl, bool shift, bool alt) override;
        LRESULT HandlePanelMessage(UINT message, WPARAM wParam, LPARAM lParam) override;

    private:
        static constexpr int MAX_QUERY    = 200;
        static constexpr int VISIBLE_ROWS = 8;

        struct MatchResult {
            int          playlistIdx; // -1 for cache-only entries
            std::wstring path;        // full path (always set)
            int          score;
            int          positions[MAX_QUERY];
            int          posCount;
        };

        int m_cachedExtraCount = 0; // entries from VRAM cache not in playlist

        wchar_t                  m_query[MAX_QUERY + 2] = {};
        int                      m_queryLen  = 0;
        std::vector<MatchResult> m_results;
        int                      m_selIdx    = 0;
        int                      m_rowScroll = 0;

        void RebuildMatches();
        void CommitOpen();
        void AdjustScroll();

        HFONT m_hFontNorm    = nullptr;
        HFONT m_hFontBold    = nullptr;
        int   m_cachedFontDpi = 0;
    };
} // namespace UI
