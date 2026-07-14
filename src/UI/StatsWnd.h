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
        ~StatsWnd() {
            if (m_hFontBody) DeleteObject(m_hFontBody);
            if (m_hFontBold) DeleteObject(m_hFontBold);
            if (m_hFontSec)  DeleteObject(m_hFontSec);
            if (m_hFontLink) DeleteObject(m_hFontLink);
        }

    protected:
        LRESULT HandlePanelMessage(UINT message, WPARAM wParam, LPARAM lParam) override;

    private:
        struct CacheFile {
            std::wstring name;
            UINT64 bytes = 0;
            INT64  count = -1; // -1=no estimate  >=0=estimated
        };

        struct ExtStat {
            std::wstring ext;
            int count = 0;
        };

        // A clickable region: left-click opens a path in Explorer or regedit
        struct ClickLink {
            RECT         rect    = {};
            std::wstring target;        // file/folder path OR full registry key path
            bool         isReg   = false; // true → open regedit at target, false → Explorer
        };

        // ── Thumbnail cache ──────────────────────────────────────
        std::vector<CacheFile> m_cacheFiles;
        UINT64       m_cacheTotalBytes = 0;
        INT64        m_cacheTotalCount = 0;
        std::wstring m_thumbCachePath;

        // ── Current image ────────────────────────────────────────
        int          m_imgW         = 0;
        int          m_imgH         = 0;
        UINT64       m_imgFileBytes = 0;
        int          m_lastLoadMs   = -1;   // -1 = no measurement yet
        std::wstring m_lastCodec;

        // ── Playlist ─────────────────────────────────────────────
        int                  m_playlistSize  = 0;
        int                  m_currentIndex  = 0;
        UINT64               m_playlistBytes = 0;
        std::vector<ExtStat> m_extStats;

        // ── History ──────────────────────────────────────────────
        int m_historyCount  = 0;
        int m_instanceCount = 1;

        // ── Memory ───────────────────────────────────────────────
        SIZE_T m_memWorkingSet = 0;
        SIZE_T m_memPeak       = 0;
        SIZE_T m_memPrivate    = 0;
        UINT64 m_decodedBytes  = 0;

        // ── Threads ──────────────────────────────────────────────
        int m_ioThreads       = 0;
        int m_wicThreads      = 0;
        int m_dirThumbThreads = 0;
        int m_ioPending       = 0;
        int m_wicPending      = 0;
        int m_dirThumbPending = 0;

        // ── VRAM cache ───────────────────────────────────────────
        int    m_imgCacheCount      = 0;
        UINT64 m_imgCacheBytes      = 0;
        int    m_dirThumbCacheCount = 0;
        UINT64 m_dirThumbCacheBytes = 0;

        // ── App / registry ───────────────────────────────────────
        std::wstring m_exePath;
        std::wstring m_rendererName;
        bool         m_autostartEnabled = false;
        std::wstring m_autostartCmd;        // what's stored in the Run value

        // ── Clickable links (rebuilt each paint) ─────────────────
        std::vector<ClickLink> m_links;

        // ── Scroll ───────────────────────────────────────────────
        int m_scrollOffsetY = 0;
        int m_totalContentH = 0;

        // Cached GDI fonts — recreated only when DPI changes
        HFONT m_hFontBody = nullptr;
        HFONT m_hFontBold = nullptr;
        HFONT m_hFontSec  = nullptr;
        HFONT m_hFontLink = nullptr;
        int   m_cachedFontDpi = 0;

        void GatherStats();
        static void OpenRegedit(const std::wstring& fullKeyPath);

        static std::wstring FormatBytes(UINT64 bytes);
        static std::wstring FormatCount(INT64 n);
        static INT64        EstimateCount(const std::wstring& name, UINT64 fileBytes);
    };
} // namespace UI
