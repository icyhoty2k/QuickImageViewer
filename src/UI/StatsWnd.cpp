#include "StatsWnd.h"
#include "../AppState.h"
#include "../Platform/Constants.h"
#include "../WorkerThread.h"
#include "../Renderer/IRenderer.h"
#include "../ImageLoadStats.h"
#include "HistoryListWnd.h"
#include <algorithm>
#include <shellapi.h>
#include <windowsx.h>
#include <unordered_map>
#include <cwctype>
#include <psapi.h>

extern AppState app;
extern IoThreadPool g_ioWorker;

namespace UI {

// ─────────────────────────────────────────────────────────────────────────────
// Init
// ─────────────────────────────────────────────────────────────────────────────

void StatsWnd::Init(HINSTANCE hInstance, HWND hParent) {
    UINT dpi = static_cast<UINT>(app.dpiScale * 96.0f);
    InitFloating(hInstance, hParent, L"QivStatsWndClass", L"QIV Statistics",
                 MulDiv(580, dpi, 96), MulDiv(800, dpi, 96));
}

void StatsWnd::Init(HINSTANCE hInstance, HWND hParent, int8_t /*position*/) {
    Init(hInstance, hParent);
}

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

std::wstring StatsWnd::FormatBytes(UINT64 bytes) {
    wchar_t buf[32];
    if (bytes == 0)          swprintf_s(buf, L"0 B");
    else if (bytes < 1024ULL)          swprintf_s(buf, L"%llu B",   bytes);
    else if (bytes < 1024ULL * 1024)   swprintf_s(buf, L"%.1f KB",  bytes / 1024.0);
    else if (bytes < 1024ULL*1024*1024)swprintf_s(buf, L"%.2f MB",  bytes / (1024.0*1024.0));
    else                               swprintf_s(buf, L"%.2f GB",  bytes / (1024.0*1024.0*1024.0));
    return buf;
}

std::wstring StatsWnd::FormatCount(INT64 n) {
    if (n < 0)  return L"…";
    if (n == 0) return L"0";
    wchar_t buf[32];
    swprintf_s(buf, L"%lld", n);
    std::wstring s = buf;
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3)
        s.insert(i, L",");
    return s;
}

// Estimate thumbnail count from file size and resolution in the filename.
// thumbcache_256.db → res=256, bytes/thumb ≈ res²×3÷10 + 128 entry overhead.
// Returns -1 for non-numeric names (sr, wide, exif, idx, …).
INT64 StatsWnd::EstimateCount(const std::wstring& name, UINT64 fileBytes) {
    int res = 0;
    auto p = name.find(L"thumbcache_");
    if (p != std::wstring::npos) {
        p += 11;
        while (p < name.size() && iswdigit(name[p]))
            res = res * 10 + (name[p++] - L'0');
    }
    if (res <= 0 || fileBytes < 1024) return -1;
    UINT64 bpt = static_cast<UINT64>(res) * res * 3 / 10 + 128;
    return static_cast<INT64>(fileBytes / bpt);
}

// ─────────────────────────────────────────────────────────────────────────────
// GatherStats  — fully synchronous
// ─────────────────────────────────────────────────────────────────────────────

void StatsWnd::GatherStats() {
    // ── Thumbnail cache files ────────────────────────────────────────────────
    m_cacheFiles.clear();
    m_cacheTotalBytes = 0;
    m_cacheTotalCount = 0;
    m_thumbCachePath.clear();

    wchar_t localAppData[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH) > 0) {
        m_thumbCachePath = std::wstring(localAppData) + L"\\Microsoft\\Windows\\Explorer";
        std::wstring pattern = m_thumbCachePath + L"\\thumbcache_*.db";
        WIN32_FIND_DATAW fd;
        HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                UINT64 sz = ((UINT64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
                INT64  est = EstimateCount(fd.cFileName, sz);
                m_cacheFiles.push_back({ fd.cFileName, sz, est });
                m_cacheTotalBytes += sz;
                if (est >= 0) m_cacheTotalCount += est;
            } while (FindNextFileW(hFind, &fd));
            FindClose(hFind);
        }
        std::sort(m_cacheFiles.begin(), m_cacheFiles.end(),
            [](const CacheFile &a, const CacheFile &b) { return a.bytes > b.bytes; });
    }

    // ── Current image ────────────────────────────────────────────────────────
    m_imgW = app.imgWidth;
    m_imgH = app.imgHeight;
    m_imgFileBytes = 0;
    if (app.currentIndex >= 0 && app.currentIndex < (int)app.playlist.size()) {
        auto it = app.playlistFileSizes.find(app.playlist[app.currentIndex]);
        if (it != app.playlistFileSizes.end())
            m_imgFileBytes = static_cast<UINT64>(std::max<int64_t>(0, it->second));
    }
    m_decodedBytes = (m_imgW > 0 && m_imgH > 0)
        ? static_cast<UINT64>(m_imgW) * m_imgH * 4 : 0;

    // ── Playlist ─────────────────────────────────────────────────────────────
    m_playlistSize  = static_cast<int>(app.playlist.size());
    m_currentIndex  = app.currentIndex;
    m_playlistBytes = 0;
    for (auto &[path, sz] : app.playlistFileSizes)
        m_playlistBytes += static_cast<UINT64>(std::max<int64_t>(0, sz));

    std::unordered_map<std::wstring, int> extMap;
    for (auto &path : app.playlist) {
        auto dot = path.rfind(L'.');
        if (dot != std::wstring::npos) {
            std::wstring ext = path.substr(dot + 1);
            for (auto &c : ext) c = static_cast<wchar_t>(towlower(c));
            extMap[ext]++;
        }
    }
    m_extStats.clear();
    for (auto &[ext, cnt] : extMap) m_extStats.push_back({ ext, cnt });
    std::sort(m_extStats.begin(), m_extStats.end(),
        [](const ExtStat &a, const ExtStat &b) { return a.count > b.count; });
    if (m_extStats.size() > 6) m_extStats.resize(6);

    // ── History / instances ──────────────────────────────────────────────────
    m_historyCount  = static_cast<int>(GetFolderHistory().size());
    m_instanceCount = app.GetInstanceCount();

    // ── Thread counts & queue depths ──────────────────────────────────────────
    m_ioThreads       = g_ioWorker.getThreadCount();
    m_wicThreads      = g_decoderWorker.getThreadCount();
    m_dirThumbThreads = g_dirThumbWorker.getThreadCount();
    m_ioPending       = static_cast<int>(g_ioWorker.PendingTaskCount());
    m_wicPending      = static_cast<int>(g_decoderWorker.PendingTaskCount());
    m_dirThumbPending = static_cast<int>(g_dirThumbWorker.PendingTaskCount());

    // ── VRAM cache stats ──────────────────────────────────────────────────────
    m_imgCacheCount      = 0; m_imgCacheBytes      = 0;
    m_dirThumbCacheCount = 0; m_dirThumbCacheBytes = 0;
    if (app.renderer) {
        app.renderer->GetImageCacheStats(m_imgCacheCount, m_imgCacheBytes);
        app.renderer->GetDirThumbCacheStats(m_dirThumbCacheCount, m_dirThumbCacheBytes);
    }

    // ── Last load time & codec ────────────────────────────────────────────────
    m_lastLoadMs = ImageLoadStats::g_lastLoadMs.load(std::memory_order_relaxed);
    m_lastCodec.clear();
    if (app.currentIndex >= 0 && app.currentIndex < static_cast<int>(app.playlist.size()))
        m_lastCodec = ImageLoadStats::CodecForPath(app.playlist[app.currentIndex]);

    // ── Process memory ────────────────────────────────────────────────────────
    PROCESS_MEMORY_COUNTERS_EX pmc = { sizeof(pmc) };
    if (K32GetProcessMemoryInfo(GetCurrentProcess(),
                                reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                                sizeof(pmc))) {
        m_memWorkingSet = pmc.WorkingSetSize;
        m_memPeak       = pmc.PeakWorkingSetSize;
        m_memPrivate    = pmc.PrivateUsage;
    }

    // ── Exe path ──────────────────────────────────────────────────────────────
    wchar_t exeBuf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exeBuf, MAX_PATH);
    m_exePath = exeBuf;

    // ── Renderer name ─────────────────────────────────────────────────────────
    m_rendererName = (app.renderer) ? app.renderer->GetName() : L"None";

    // ── Autostart (HKCU Run key) ──────────────────────────────────────────────
    m_autostartEnabled = false;
    m_autostartCmd.clear();
    {
        HKEY hk = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, Constants::Registry::RUN_KEY, 0,
                          KEY_QUERY_VALUE, &hk) == ERROR_SUCCESS) {
            wchar_t val[MAX_PATH * 2] = {};
            DWORD   sz   = sizeof(val);
            DWORD   type = 0;
            if (RegQueryValueExW(hk, Constants::Registry::RUN_VALUE_NAME,
                                 nullptr, &type,
                                 reinterpret_cast<BYTE*>(val), &sz) == ERROR_SUCCESS) {
                m_autostartEnabled = true;
                m_autostartCmd     = val;
            }
            RegCloseKey(hk);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// OpenRegedit  — navigate regedit to a full "Computer\HKEY_..." key path
// ─────────────────────────────────────────────────────────────────────────────

void StatsWnd::OpenRegedit(const std::wstring& fullKeyPath) {
    HKEY hk = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Applets\\Regedit",
            0, nullptr, 0, KEY_SET_VALUE, nullptr, &hk, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(hk, L"LastKey", 0, REG_SZ,
            reinterpret_cast<const BYTE*>(fullKeyPath.c_str()),
            static_cast<DWORD>((fullKeyPath.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(hk);
    }
    ShellExecuteW(nullptr, L"open", L"regedit.exe", nullptr, nullptr, SW_SHOW);
}

// ─────────────────────────────────────────────────────────────────────────────
// Show
// ─────────────────────────────────────────────────────────────────────────────

void StatsWnd::Show() {
    if (!m_hWnd) return;
    m_scrollOffsetY = 0;
    m_links.clear();
    GatherStats();

    RECT pr; GetWindowRect(m_hParent, &pr);
    RECT wr; GetWindowRect(m_hWnd,    &wr);
    int ww = wr.right - wr.left, wh = wr.bottom - wr.top;
    int px = pr.left + (pr.right  - pr.left - ww) / 2;
    int py = pr.top  + (pr.bottom - pr.top  - wh) / 2;
    SetWindowPos(m_hWnd, HWND_TOPMOST, px, py, 0, 0, SWP_NOSIZE | SWP_FRAMECHANGED);
    ShowWindow(m_hWnd, SW_SHOW);
    SetForegroundWindow(m_hWnd);
    InvalidateRect(m_hWnd, nullptr, FALSE);
}

// ─────────────────────────────────────────────────────────────────────────────
// Message handler
// ─────────────────────────────────────────────────────────────────────────────

LRESULT StatsWnd::HandlePanelMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {

    // ── Mouse wheel scroll ────────────────────────────────────────────────────
    case WM_MOUSEWHEEL: {
        int step = static_cast<int>(30.0f * app.dpiScale);
        m_scrollOffsetY -= (GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? step : -step);
        RECT rc; GetClientRect(m_hWnd, &rc);
        int maxS = std::max(0, m_totalContentH - static_cast<int>(rc.bottom - rc.top));
        m_scrollOffsetY = std::clamp(m_scrollOffsetY, 0, maxS);
        InvalidateRect(m_hWnd, nullptr, FALSE);
        return 0;
    }

    // ── Click on a link ───────────────────────────────────────────────────────
    case WM_LBUTTONUP: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        for (auto &lnk : m_links) {
            if (PtInRect(&lnk.rect, pt)) {
                if (lnk.isReg)
                    OpenRegedit(lnk.target);
                else
                    ShellExecuteW(nullptr, L"explore", lnk.target.c_str(), nullptr, nullptr, SW_SHOW);
                break;
            }
        }
        return 0;
    }

    // ── Hand cursor over any link ─────────────────────────────────────────────
    case WM_SETCURSOR: {
        POINT pt; GetCursorPos(&pt); ScreenToClient(m_hWnd, &pt);
        for (auto &lnk : m_links) {
            if (PtInRect(&lnk.rect, pt)) {
                SetCursor(LoadCursor(nullptr, IDC_HAND));
                return TRUE;
            }
        }
        break;
    }

    // ── Paint ─────────────────────────────────────────────────────────────────
    case WM_PAINT: {
        m_links.clear();
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(m_hWnd, &ps);
        RECT rc; GetClientRect(m_hWnd, &rc);

        // Background
        COLORREF bgColor = GetBgColor();
        HBRUSH bgBr = CreateSolidBrush(bgColor);
        FillRect(hdc, &rc, bgBr);
        DeleteObject(bgBr);
        SetBkMode(hdc, TRANSPARENT);

        // ── Metrics ──────────────────────────────────────────────────────────
        const int dpi  = static_cast<int>(app.dpiScale * 96);
        const int pad  = MulDiv(14, dpi, 96);
        const int row  = MulDiv(20, dpi, 96);
        const int sec  = MulDiv(18, dpi, 96);
        const int gap  = MulDiv(6,  dpi, 96);
        const int sgap = MulDiv(16, dpi, 96);
        const int fs   = MulDiv(12, dpi, 96);
        const int fss  = MulDiv(11, dpi, 96);
        const int sb   = MulDiv(6,  dpi, 96); // scrollbar width
        const int colR = rc.right - pad - sb;

        // 3-column boundaries for cache rows
        const int c2 = pad + static_cast<int>((colR - pad) * 0.52f); // size right edge
        const int c3 = colR;

        // ── Fonts ─────────────────────────────────────────────────────────────
        if (dpi != m_cachedFontDpi) {
            if (m_hFontBody) { DeleteObject(m_hFontBody); m_hFontBody = nullptr; }
            if (m_hFontBold) { DeleteObject(m_hFontBold); m_hFontBold = nullptr; }
            if (m_hFontSec)  { DeleteObject(m_hFontSec);  m_hFontSec  = nullptr; }
            if (m_hFontLink) { DeleteObject(m_hFontLink); m_hFontLink = nullptr; }
            m_hFontBody = CreateFontW(-fs,  0, 0, 0, FW_NORMAL,   0, 0,    0, DEFAULT_CHARSET,
                                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                       DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            m_hFontBold = CreateFontW(-fs,  0, 0, 0, FW_SEMIBOLD, 0, 0,    0, DEFAULT_CHARSET,
                                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                       DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            m_hFontSec  = CreateFontW(-fss, 0, 0, 0, FW_BOLD,     0, 0,    0, DEFAULT_CHARSET,
                                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                       DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            m_hFontLink = CreateFontW(-fs,  0, 0, 0, FW_NORMAL,   0, TRUE, 0, DEFAULT_CHARSET,
                                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                       DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            m_cachedFontDpi = dpi;
        }
        HFONT hOld  = static_cast<HFONT>(SelectObject(hdc, m_hFontBody));

        // ── Colors ────────────────────────────────────────────────────────────
        const COLORREF clrCyan   = Constants::Theme::ThemedColor(0.39f, 0.78f, 1.0f, app.themeFactor);
        const COLORREF clrGreen  = Constants::Theme::ThemedColor(0.40f, 0.90f, 0.55f, app.themeFactor);
        const COLORREF clrYellow = Constants::Theme::ThemedColor(1.0f,  0.87f, 0.0f,  app.themeFactor);
        const COLORREF clrOrange = Constants::Theme::ThemedColor(1.0f,  0.62f, 0.25f, app.themeFactor);
        const COLORREF clrLabel  = Constants::Theme::ThemedGray(0.58f, app.themeFactor);
        const COLORREF clrValue  = Constants::Theme::ThemedGray(0.90f, app.themeFactor);
        const COLORREF clrDim    = Constants::Theme::ThemedGray(0.22f, app.themeFactor);
        const COLORREF clrSepLine= Constants::Theme::ThemedColor(0.18f, 0.44f, 0.60f, app.themeFactor);

        // ── Drawing helpers ───────────────────────────────────────────────────
        int y = pad - m_scrollOffsetY;

        // Label on left, underlined clickable value on right
        auto linkRow = [&](const wchar_t* label, const std::wstring& display,
                           const std::wstring& target, bool isReg) {
            if (y + row > 0 && y < rc.bottom) {
                SelectObject(hdc, m_hFontBody);
                SetTextColor(hdc, clrLabel);
                RECT rL = { pad + MulDiv(6,dpi,96), y, c3 - MulDiv(4,dpi,96), y + row };
                DrawTextW(hdc, label, -1, &rL, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                SelectObject(hdc, m_hFontLink);
                SetTextColor(hdc, clrCyan);
                RECT rV = { pad, y, c3, y + row };
                DrawTextW(hdc, display.c_str(), -1, &rV,
                          DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                m_links.push_back({ { pad, y, c3, y + row }, target, isReg });
            }
            y += row;
        };

        // Simple label + right-aligned value
        auto row2 = [&](const wchar_t *label, const std::wstring &val,
                        COLORREF valColor, bool bold = false) {
            if (y + row > 0 && y < rc.bottom) {
                SelectObject(hdc, bold ? m_hFontBold : m_hFontBody);
                RECT rL = { pad + MulDiv(6,dpi,96), y, c3 - MulDiv(4,dpi,96), y + row };
                RECT rV = { pad, y, c3, y + row };
                SetTextColor(hdc, clrLabel);
                DrawTextW(hdc, label, -1, &rL, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                SetTextColor(hdc, valColor);
                DrawTextW(hdc, val.c_str(), -1, &rV, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            }
            y += row;
        };

        // Section header with left accent stripe + underline
        auto section = [&](const wchar_t *title, COLORREF accentColor) {
            y += MulDiv(4,dpi,96);
            if (y + sec > 0 && y < rc.bottom) {
                // Left accent stripe
                HBRUSH acBr = CreateSolidBrush(accentColor);
                RECT strp = { pad, y + MulDiv(2,dpi,96), pad + MulDiv(3,dpi,96), y + sec - MulDiv(2,dpi,96) };
                FillRect(hdc, &strp, acBr);
                DeleteObject(acBr);
                // Title
                SelectObject(hdc, m_hFontSec);
                SetTextColor(hdc, accentColor);
                RECT r = { pad + MulDiv(8,dpi,96), y, c3, y + sec };
                DrawTextW(hdc, title, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            }
            y += sec;
            // Underline
            if (y >= 0 && y < rc.bottom) {
                HPEN hPen = CreatePen(PS_SOLID, 1, clrSepLine);
                HPEN hOldP = static_cast<HPEN>(SelectObject(hdc, hPen));
                MoveToEx(hdc, pad, y, nullptr);
                LineTo(hdc, c3, y);
                SelectObject(hdc, hOldP);
                DeleteObject(hPen);
            }
            y += gap + MulDiv(2,dpi,96);
        };

        // Thin dotted separator
        auto dotSep = [&]() {
            if (y >= 0 && y < rc.bottom) {
                HPEN hPen = CreatePen(PS_DOT, 1, clrDim);
                HPEN hOldP = static_cast<HPEN>(SelectObject(hdc, hPen));
                MoveToEx(hdc, pad + MulDiv(6,dpi,96), y, nullptr);
                LineTo(hdc, c3, y);
                SelectObject(hdc, hOldP);
                DeleteObject(hPen);
            }
            y += MulDiv(5,dpi,96);
        };

        // 3-column cache row: name | size | count
        auto cacheRow = [&](const CacheFile &e, bool altBg) {
            if (y + row > 0 && y < rc.bottom) {
                if (altBg) {
                    HBRUSH altBr = CreateSolidBrush(
                        Constants::Theme::ThemedGray(0.06f, app.themeFactor));
                    RECT rAlt = { pad, y, c3, y + row };
                    FillRect(hdc, &rAlt, altBr);
                    DeleteObject(altBr);
                }
                // Name
                SelectObject(hdc, m_hFontBody);
                SetTextColor(hdc, clrValue);
                RECT rName = { pad + MulDiv(6,dpi,96), y, c2 - MulDiv(4,dpi,96), y + row };
                DrawTextW(hdc, e.name.c_str(), -1, &rName, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                // Size
                SetTextColor(hdc, clrGreen);
                RECT rSz = { c2 - MulDiv(100,dpi,96), y, c2, y + row };
                DrawTextW(hdc, FormatBytes(e.bytes).c_str(), -1, &rSz, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
                // Count  (estimated — prefix with ~)
                std::wstring cntStr = (e.count < 0) ? L"–" : (L"~" + FormatCount(e.count));
                SetTextColor(hdc, e.count >= 0 ? clrCyan : clrDim);
                RECT rCnt = { c2 + MulDiv(4,dpi,96), y, c3, y + row };
                DrawTextW(hdc, cntStr.c_str(), -1, &rCnt, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            }
            y += row;
        };

        // ═════════════════════════════════════════════════════════════════════
        // Section 1 — Windows Thumbnail Cache
        // ═════════════════════════════════════════════════════════════════════
        section(L"  WINDOWS THUMBNAIL CACHE", clrCyan);

        // Column header
        if (y + row > 0 && y < rc.bottom) {
            SelectObject(hdc, m_hFontSec);
            SetTextColor(hdc, clrDim);
            RECT rhFile = { pad + MulDiv(6,dpi,96), y, c2, y + row };
            RECT rhSize = { c2 - MulDiv(100,dpi,96), y, c2, y + row };
            RECT rhCnt  = { c2 + MulDiv(4,dpi,96),  y, c3, y + row };
            DrawTextW(hdc, L"File",       -1, &rhFile, DT_LEFT  | DT_VCENTER | DT_SINGLELINE);
            DrawTextW(hdc, L"Size",       -1, &rhSize, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            DrawTextW(hdc, L"Thumbnails", -1, &rhCnt,  DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }
        y += row;

        if (m_cacheFiles.empty()) {
            row2(L"No thumbcache files found", L"", clrDim);
        } else {
            for (size_t i = 0; i < m_cacheFiles.size(); ++i)
                cacheRow(m_cacheFiles[i], i % 2 == 1);
        }
        dotSep();

        // Total row
        if (y + row > 0 && y < rc.bottom) {
            SelectObject(hdc, m_hFontBold);
            SetTextColor(hdc, clrLabel);
            RECT rLabel = { pad + MulDiv(6,dpi,96), y, c2, y + row };
            DrawTextW(hdc, L"Total", -1, &rLabel, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            SetTextColor(hdc, clrYellow);
            RECT rSzT = { c2 - MulDiv(100,dpi,96), y, c2, y + row };
            DrawTextW(hdc, FormatBytes(m_cacheTotalBytes).c_str(), -1, &rSzT, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            std::wstring totCnt = L"~" + FormatCount(m_cacheTotalCount) + L" thumbs";
            SetTextColor(hdc, clrYellow);
            RECT rCntT = { c2 + MulDiv(4,dpi,96), y, c3, y + row };
            DrawTextW(hdc, totCnt.c_str(), -1, &rCntT, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }
        y += row + MulDiv(4,dpi,96);

        // Clickable folder path link
        if (!m_thumbCachePath.empty()) {
            if (y + row > 0 && y < rc.bottom) {
                SelectObject(hdc, m_hFontLink);
                SetTextColor(hdc, clrCyan);
                RECT rLink = { pad + MulDiv(6,dpi,96), y, c3, y + row };
                DrawTextW(hdc, (L"▸  " + m_thumbCachePath).c_str(), -1, &rLink,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                m_links.push_back({ { rLink.left, y, c3, y + row }, m_thumbCachePath, false });
            }
            y += row;
        }

        y += sgap;

        // ═════════════════════════════════════════════════════════════════════
        // Section 2 — Current Image
        // ═════════════════════════════════════════════════════════════════════
        section(L"  CURRENT IMAGE", clrGreen);

        if (m_imgW > 0 && m_imgH > 0) {
            wchar_t resBuf[32];
            swprintf_s(resBuf, L"%d × %d px", m_imgW, m_imgH);
            row2(L"Resolution",  resBuf, clrValue);
            if (m_imgFileBytes > 0)
                row2(L"File size", FormatBytes(m_imgFileBytes), clrGreen);

            // Megapixel
            double mp = (double)m_imgW * m_imgH / 1'000'000.0;
            wchar_t mpBuf[32];
            swprintf_s(mpBuf, L"%.1f MP", mp);
            row2(L"Megapixels", mpBuf, clrValue);

            // Zoom / rotation
            if (app.viewport.zoom != 1.0f) {
                wchar_t zBuf[32];
                swprintf_s(zBuf, L"%.2f×", app.viewport.zoom);
                row2(L"Zoom", zBuf, clrOrange);
            }
            if (app.viewport.rotation != 0) {
                wchar_t rBuf[16];
                swprintf_s(rBuf, L"%d°", app.viewport.rotation);
                row2(L"Rotation", rBuf, clrOrange);
            }

            // Codec & load time
            if (!m_lastCodec.empty())
                row2(L"Codec / decoder", m_lastCodec, clrCyan);
            if (m_lastLoadMs >= 0) {
                wchar_t lBuf[32];
                swprintf_s(lBuf, L"%d ms", m_lastLoadMs);
                COLORREF lClr = m_lastLoadMs < 100 ? clrGreen
                              : m_lastLoadMs < 400 ? clrValue : clrOrange;
                row2(L"Last load time", lBuf, lClr);
            }
        } else {
            row2(L"No image loaded", L"", clrDim);
        }

        y += sgap;

        // ═════════════════════════════════════════════════════════════════════
        // Section 3 — Playlist
        // ═════════════════════════════════════════════════════════════════════
        section(L"  PLAYLIST", clrOrange);

        row2(L"Images", FormatCount(m_playlistSize), clrValue);
        if (m_playlistSize > 0 && m_currentIndex >= 0) {
            row2(L"Viewing",
                 FormatCount(m_currentIndex + 1) + L"  of  " + FormatCount(m_playlistSize),
                 clrValue);
        }
        if (m_playlistBytes > 0) {
            row2(L"Total size on disk", FormatBytes(m_playlistBytes), clrGreen);
            if (m_playlistSize > 0)
                row2(L"Average per image", FormatBytes(m_playlistBytes / m_playlistSize), clrValue);
        }

        // Extension breakdown
        if (!m_extStats.empty()) {
            y += MulDiv(4,dpi,96);
            dotSep();
            for (auto &e : m_extStats) {
                float pct = (m_playlistSize > 0)
                    ? (float)e.count / m_playlistSize * 100.0f : 0.0f;
                wchar_t valBuf[48];
                swprintf_s(valBuf, L"%s   %.1f%%", FormatCount(e.count).c_str(), pct);
                std::wstring label = L"." + e.ext;
                row2(label.c_str(), valBuf, clrValue);
            }
        }

        y += sgap;

        // ═════════════════════════════════════════════════════════════════════
        // Section 4 — History & Session
        // ═════════════════════════════════════════════════════════════════════
        section(L"  HISTORY & SESSION", clrYellow);

        row2(L"Folders visited",  FormatCount(m_historyCount),  clrValue);
        row2(L"App instances",    FormatCount(m_instanceCount),  clrValue);

        y += sgap;

        // ═════════════════════════════════════════════════════════════════════
        // Section 5 — Threads & Memory
        // ═════════════════════════════════════════════════════════════════════
        const COLORREF clrPurple = Constants::Theme::ThemedColor(0.80f, 0.50f, 1.0f, app.themeFactor);
        section(L"  THREADS & MEMORY", clrPurple);

        {
            int totalThreads = 1 + m_ioThreads + m_wicThreads + m_dirThumbThreads;
            wchar_t buf[32];
            swprintf_s(buf, L"%d", totalThreads);
            row2(L"Total app threads", buf, clrValue, true);
        }
        {
            wchar_t buf[32]; swprintf_s(buf, L"%d  (UI + render)", 1);
            row2(L"  UI thread", buf, clrValue);
        }
        {
            wchar_t buf[32]; swprintf_s(buf, L"%d  (%s)", m_ioThreads,
                m_ioThreads <= 1 ? L"HDD mode" : L"SSD/NVMe mode");
            row2(L"  IO worker", buf, clrCyan);
            if (m_ioPending > 0) {
                wchar_t q[32]; swprintf_s(q, L"%d pending", m_ioPending);
                row2(L"    queue", q, clrOrange);
            }
        }
        {
            wchar_t buf[32]; swprintf_s(buf, L"%d  (WIC decode)", m_wicThreads);
            row2(L"  WIC decoder", buf, clrCyan);
            if (m_wicPending > 0) {
                wchar_t q[32]; swprintf_s(q, L"%d pending", m_wicPending);
                row2(L"    queue", q, clrOrange);
            }
        }
        {
            wchar_t buf[32]; swprintf_s(buf, L"%d  (dir thumbnails)", m_dirThumbThreads);
            row2(L"  Dir thumbnail", buf, clrCyan);
            if (m_dirThumbPending > 0) {
                wchar_t q[32]; swprintf_s(q, L"%d pending", m_dirThumbPending);
                row2(L"    queue", q, clrOrange);
            }
        }
        {
            wchar_t buf[32]; swprintf_s(buf, L"%d", app.hardwareThreads);
            row2(L"  CPU logical cores", buf, clrValue);
        }

        dotSep();

        row2(L"Working set",       FormatBytes(m_memWorkingSet), clrGreen,  true);
        row2(L"Peak working set",  FormatBytes(m_memPeak),       clrValue);
        row2(L"Private bytes",     FormatBytes(m_memPrivate),    clrValue);
        if (m_decodedBytes > 0)
            row2(L"Decoded bitmap", FormatBytes(m_decodedBytes), clrOrange);

        y += sgap;

        // ═════════════════════════════════════════════════════════════════════
        // Section 6 — VRAM Cache
        // ═════════════════════════════════════════════════════════════════════
        {
            const COLORREF clrTeal = Constants::Theme::ThemedColor(0.0f, 0.85f, 0.85f, app.themeFactor);
            section(L"  VRAM CACHE", clrTeal);

            // Image (lookaside) cache
            {
                wchar_t buf[64];
                swprintf_s(buf, L"%d / %d  slots", m_imgCacheCount,
                           static_cast<int>(Constants::VRAM_CACHE_IMAGES_COUNT));
                COLORREF c = (m_imgCacheCount == static_cast<int>(Constants::VRAM_CACHE_IMAGES_COUNT))
                             ? clrOrange : clrValue;
                row2(L"Image cache", buf, c, true);
            }
            if (m_imgCacheBytes > 0)
                row2(L"  VRAM used", FormatBytes(m_imgCacheBytes), clrGreen);

            if (m_dirThumbCacheCount > 0) {
                dotSep();
                wchar_t buf[32]; swprintf_s(buf, L"%d entries", m_dirThumbCacheCount);
                row2(L"Dir thumbnails", buf, clrValue, true);
                row2(L"  VRAM used", FormatBytes(m_dirThumbCacheBytes), clrGreen);
            }
        }

        y += sgap;

        // ═════════════════════════════════════════════════════════════════════
        // Section 7 — App & Display
        // ═════════════════════════════════════════════════════════════════════
        section(L"  APP & DISPLAY", clrPurple);

        {
            const wchar_t *vmNames[] = { L"",
                L"Fit to View",
                L"Fit to Width",
                L"Fit to Height",
                L"Fill Window",
                L"Original Size" };
            int vmi = static_cast<int>(app.viewMode);
            if (vmi >= 1 && vmi <= 5)
                row2(L"View mode", vmNames[vmi], clrValue);
        }
        {
            const wchar_t *sortNames[] = { L"Name", L"Date modified", L"File size", L"Extension", L"Disk order" };
            int si = std::clamp(app.fileHandlerDefaultSortOrder, 0, 4);
            std::wstring s = sortNames[si];
            if (app.fileHandlerIsReverseSortOrder) s += L"  ↓";
            row2(L"Sort order", s, clrValue);
        }
        {
            wchar_t buf[16]; swprintf_s(buf, L"%.0f%%", app.dpiScale * 100.0f);
            row2(L"DPI scale", buf, clrValue);
        }
        {
            wchar_t buf[32]; swprintf_s(buf, L"%d × %d", app.screenW, app.screenH);
            row2(L"Screen resolution", buf, clrValue);
        }
        {
            wchar_t buf[16]; swprintf_s(buf, L"%d", app.hardwareThreads);
            row2(L"CPU threads", buf, clrValue);
        }
        {
            wchar_t buf[16]; swprintf_s(buf, L"%.0f%%", app.themeFactor * 100.0f);
            row2(L"Theme factor", buf, clrValue);
        }
        row2(L"Version", std::wstring(Constants::APP_VERSION), clrValue);
        if (!m_rendererName.empty())
            row2(L"Renderer", m_rendererName, clrCyan);

        // Exe path — clickable, opens folder in Explorer
        if (!m_exePath.empty()) {
            std::wstring folder = m_exePath;
            auto bs = folder.rfind(L'\\');
            if (bs != std::wstring::npos) folder.resize(bs);
            linkRow(L"Executable", m_exePath, folder, false);
        }

        y += sgap;

        // ═════════════════════════════════════════════════════════════════════
        // Section 8 — Registry
        // ═════════════════════════════════════════════════════════════════════
        section(L"  REGISTRY", clrPurple);

        // App preferences key
        {
            std::wstring fullKey = L"Computer\\HKEY_CURRENT_USER\\" +
                                   std::wstring(Constants::Registry::ROOT_KEY);
            std::wstring display = L"HKCU\\" + std::wstring(Constants::Registry::ROOT_KEY);
            linkRow(L"App settings", display, fullKey, true);
        }

        // Auto-start status
        {
            std::wstring status = m_autostartEnabled ? L"Enabled" : L"Disabled";
            row2(L"Auto-start on login", status,
                 m_autostartEnabled ? clrGreen : clrDim);
        }

        // Run key (always shown as clickable link)
        {
            std::wstring fullKey = L"Computer\\HKEY_CURRENT_USER\\" +
                                   std::wstring(Constants::Registry::RUN_KEY);
            std::wstring display = L"HKCU\\" + std::wstring(Constants::Registry::RUN_KEY);
            linkRow(L"Startup key", display, fullKey, true);
        }

        // Open-with registration
        {
            std::wstring fullKey = L"Computer\\HKEY_CURRENT_USER\\" +
                                   std::wstring(Constants::Registry::OPEN_WITH_ROOT);
            std::wstring display = L"HKCU\\" + std::wstring(Constants::Registry::OPEN_WITH_ROOT);
            linkRow(L"Open-with key", display, fullKey, true);
        }

        y += pad;
        m_totalContentH = y + m_scrollOffsetY;

        // ── Scrollbar ─────────────────────────────────────────────────────────
        int clientH = rc.bottom - rc.top;
        if (m_totalContentH > clientH) {
            int trackH  = clientH - 2 * pad;
            float ratio = std::clamp(static_cast<float>(clientH) / m_totalContentH, 0.0f, 1.0f);
            int thumbH  = std::max(MulDiv(20, dpi, 96), static_cast<int>(trackH * ratio));
            float maxS  = static_cast<float>(std::max(1, m_totalContentH - clientH));
            int thumbY  = pad + static_cast<int>((trackH - thumbH) * m_scrollOffsetY / maxS);
            int sX      = rc.right - sb;

            HBRUSH trBr = CreateSolidBrush(clrDim);
            RECT track  = { sX, pad, rc.right - 1, pad + trackH };
            FillRect(hdc, &track, trBr);
            DeleteObject(trBr);

            HBRUSH thBr = CreateSolidBrush(Constants::Theme::ThemedGray(0.48f, app.themeFactor));
            RECT thumb  = { sX, thumbY, rc.right - 1, thumbY + thumbH };
            FillRect(hdc, &thumb, thBr);
            DeleteObject(thBr);
        }

        SelectObject(hdc, hOld);
        // Fonts are cached members — not deleted here
        EndPaint(m_hWnd, &ps);
        return 0;
    }

    default: break;
    }
    return DefWindowProcW(m_hWnd, message, wParam, lParam);
}

} // namespace UI
