#pragma once
#include <windows.h>
#include <vector>
#include <string>
#include "FloatingPanelWnd.h"

namespace UI {
    class StatsWnd : public FloatingPanelWnd {
    public:
        void Init(HINSTANCE hInstance, HWND hParent) override;
        void Init(HINSTANCE hInstance, HWND hParent, int8_t position) override;
        void Show() override;

    protected:
        LRESULT HandlePanelMessage(UINT message, WPARAM wParam, LPARAM lParam) override;

    private:
        static constexpr UINT WM_STATS_COUNTS_READY = WM_APP + 103;
        static constexpr DWORD CMMM_SIG = 0x4D4D4D43; // 'CMMM' little-endian

        struct CacheFile {
            std::wstring name;
            UINT64 bytes  = 0;
            INT64  count  = -1; // -1=pending  -2=unreadable
        };

        struct ExtStat {
            std::wstring ext;
            int count = 0;
        };

        // ── Thumbnail cache ──────────────────────────────────────
        std::vector<CacheFile> m_cacheFiles;
        UINT64       m_cacheTotalBytes = 0;
        INT64        m_cacheTotalCount = -1;
        std::wstring m_thumbCachePath;
        RECT         m_linkRect        = {};

        // ── Current image ────────────────────────────────────────
        int    m_imgW          = 0;
        int    m_imgH          = 0;
        UINT64 m_imgFileBytes  = 0;

        // ── Playlist ─────────────────────────────────────────────
        int                  m_playlistSize  = 0;
        int                  m_currentIndex  = 0;
        UINT64               m_playlistBytes = 0;
        std::vector<ExtStat> m_extStats;      // top 6, sorted by count desc

        // ── History ──────────────────────────────────────────────
        int m_historyCount = 0;

        // ── Scroll ───────────────────────────────────────────────
        int m_scrollOffsetY = 0;
        int m_totalContentH = 0;

        // ── Async guard ──────────────────────────────────────────
        UINT32 m_showSession = 0;

        void GatherStats();
        void LaunchCountAsync();

        static std::wstring FormatBytes(UINT64 bytes);
        static std::wstring FormatCount(INT64 n);
        static INT64        CountThumbcacheEntries(const std::wstring &path);
    };
} // namespace UI
