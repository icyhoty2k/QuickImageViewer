#pragma once
#include <windows.h>
#include <vector>
#include "FloatingPanelWnd.h"

namespace UI {
    class FindWnd : public FloatingPanelWnd {
    public:
        void Init(HINSTANCE hInstance, HWND hParent) override;
        void Init(HINSTANCE hInstance, HWND hParent, int8_t position) override;
        void Show() override;

    protected:
        bool    OnKeyDown(WPARAM vk, bool ctrl, bool shift, bool alt) override;
        LRESULT HandlePanelMessage(UINT message, WPARAM wParam, LPARAM lParam) override;

    private:
        static constexpr int MAX_QUERY    = 200;
        static constexpr int VISIBLE_ROWS = 8;

        struct MatchResult {
            int playlistIdx;
            int score;
            int positions[MAX_QUERY]; // matched char positions in filename
            int posCount;
        };

        wchar_t                  m_query[MAX_QUERY + 2] = {};
        int                      m_queryLen  = 0;
        std::vector<MatchResult> m_results;
        int                      m_selIdx    = 0;
        int                      m_rowScroll = 0;

        void RebuildMatches();
        void CommitOpen();
        void AdjustScroll();
    };
} // namespace UI
