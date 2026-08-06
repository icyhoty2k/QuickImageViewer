// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#include "StatsWnd.h"
#include "UI/GdiPool.h" // pooled brushes and pens — never DeleteObject them
#include "UI/CustomControls/ScrollView.h" // WheelDeltaToPixels — the one wheel rule
#include "../../AppState.h"
#include "../../Platform/Constants.h"
#include "../../Platform/WriteQueue.h"
#include "../../Platform/MonitorInfo.h"     // the display list — same order Ctrl+M uses
#include "../../Persistence/RegistryManager.h"
#include "../../Dedicated/DedicatedSettings.h" // IsDedicatedFlag — no registry writes
#include "../../WorkerThread.h"
#include "../../Renderer/IRenderer.h"
#include "../../ImageLoadStats.h"
#include "../../Rem_TCP_IP/RemoteServer.h"   // listener state for the REMOTE CONTROL section
#include "../../Rem_TCP_IP/RemoteMirror.h"   // target counts for the same
#include "../../Rem_TCP_IP/RemoteSettings.h" // configured port / AllowList / password
#include "../../Rem_TCP_IP/RemoteLog.h"      // wire-log recording state

#include <tlhelp32.h> // the real OS thread count — see the THREADS section
#include "HistoryListWnd.h"
#include "../ThumbnailPanels/DirWnd.h"
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
        if (bytes == 0) swprintf_s(buf, L"0 B");
        else if (bytes < 1024ULL) swprintf_s(buf, L"%llu B", bytes);
        else if (bytes < 1024ULL * 1024) swprintf_s(buf, L"%.1f KB", bytes / 1024.0);
        else if (bytes < 1024ULL * 1024 * 1024)swprintf_s(buf, L"%.2f MB", bytes / (1024.0 * 1024.0));
        else swprintf_s(buf, L"%.2f GB", bytes / (1024.0 * 1024.0 * 1024.0));
        return buf;
    }

    std::wstring StatsWnd::FormatCount(INT64 n) {
        if (n < 0) return L"…";
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
    INT64 StatsWnd::EstimateCount(const std::wstring &name, UINT64 fileBytes) {
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

        DWORD needed = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
        std::wstring localAppData(needed > 0 ? needed : 1, L'\0');
        if (needed > 0 && GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData.data(), needed) > 0) {
            localAppData.resize(needed - 1); // trim null terminator
            m_thumbCachePath = localAppData + L"\\Microsoft\\Windows\\Explorer";
            std::wstring pattern = m_thumbCachePath + L"\\thumbcache_*.db";
            WIN32_FIND_DATAW fd;
            HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
            if (hFind != INVALID_HANDLE_VALUE) {
                do {
                    UINT64 sz = ((UINT64) fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
                    INT64 est = EstimateCount(fd.cFileName, sz);
                    m_cacheFiles.push_back({fd.cFileName, sz, est});
                    m_cacheTotalBytes += sz;
                    if (est >= 0) m_cacheTotalCount += est;
                } while (FindNextFileW(hFind, &fd));
                FindClose(hFind);
            }
            std::sort(m_cacheFiles.begin(), m_cacheFiles.end(),
                      [](const CacheFile &a, const CacheFile &b) {
                          return a.bytes > b.bytes;
                      });
        }

        // ── Current image ────────────────────────────────────────────────────────
        m_imgW = app.imgWidth;
        m_imgH = app.imgHeight;
        m_imgFileBytes = 0;
        if (app.currentIndex >= 0 && app.currentIndex < (int) app.playlist.size()) {
            auto it = app.playlistFileSizes.find(app.playlist[app.currentIndex]);
            if (it != app.playlistFileSizes.end())
                m_imgFileBytes = static_cast<UINT64>(std::max<int64_t>(0, it->second));
        }
        m_decodedBytes = (m_imgW > 0 && m_imgH > 0)
                             ? static_cast<UINT64>(m_imgW) * m_imgH * 4
                             : 0;

        // ── Playlist ─────────────────────────────────────────────────────────────
        m_playlistSize = static_cast<int>(app.playlist.size());
        m_currentIndex = app.currentIndex;
        m_playlistBytes = 0;
        for (auto &[path, sz]: app.playlistFileSizes)
            m_playlistBytes += static_cast<UINT64>(std::max<int64_t>(0, sz));

        std::unordered_map<std::wstring, int> extMap;
        for (auto &path: app.playlist) {
            auto dot = path.rfind(L'.');
            if (dot != std::wstring::npos) {
                std::wstring ext = path.substr(dot + 1);
                for (auto &c: ext) c = static_cast<wchar_t>(towlower(c));
                extMap[ext]++;
            }
        }
        m_extStats.clear();
        for (auto &[ext, cnt]: extMap) m_extStats.push_back({ext, cnt});
        std::sort(m_extStats.begin(), m_extStats.end(),
                  [](const ExtStat &a, const ExtStat &b) {
                      return a.count > b.count;
                  });
        if (m_extStats.size() > 6) m_extStats.resize(6);

        // ── History / instances ──────────────────────────────────────────────────
        m_historyCount = static_cast<int>(GetFolderHistory().size());
        m_instanceCount = app.GetInstanceCount();

        // ── Thread counts & queue depths ──────────────────────────────────────────
        m_ioThreads         = g_ioWorker.getThreadCount();
        m_wicThreads        = g_decoderWorker.getThreadCount();
        m_dirThumbThreads   = g_dirThumbWorker.getThreadCount();
        m_writeQueueThreads = g_writeQueue.ThreadCount();
        m_dirWatcherThreads = UI::DirWatcher::ActiveCount();
        m_ioPending         = static_cast<int>(g_ioWorker.PendingTaskCount());
        m_wicPending        = static_cast<int>(g_decoderWorker.PendingTaskCount());
        m_dirThumbPending   = static_cast<int>(g_dirThumbWorker.PendingTaskCount());

        // ── VRAM cache stats ──────────────────────────────────────────────────────
        m_imgCacheCount = 0;
        m_imgCacheBytes = 0;
        m_dirThumbCacheCount = 0;
        m_dirThumbCacheBytes = 0;
        if (app.renderer) {
            app.renderer->GetImageCacheStats(m_imgCacheCount, m_imgCacheBytes);
            app.renderer->GetDirThumbCacheStats(m_dirThumbCacheCount, m_dirThumbCacheBytes);
        }

        // ── Last load time & codec ────────────────────────────────────────────────
        m_lastLoadUs = ImageLoadStats::g_lastLoadUs.load(std::memory_order_relaxed);
        m_lastCodec.clear();
        if (app.currentIndex >= 0 && app.currentIndex < static_cast<int>(app.playlist.size()))
            m_lastCodec = ImageLoadStats::CodecForPath(app.playlist[app.currentIndex]);

        // ── Process memory ────────────────────────────────────────────────────────
        PROCESS_MEMORY_COUNTERS_EX pmc = {sizeof(pmc)};
        if (K32GetProcessMemoryInfo(GetCurrentProcess(),
                                    reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&pmc),
                                    sizeof(pmc))) {
            m_memWorkingSet = pmc.WorkingSetSize;
            m_memPeak = pmc.PeakWorkingSetSize;
            m_memPrivate = pmc.PrivateUsage;
        }

        // ── Exe path ──────────────────────────────────────────────────────────────
        m_exePath = Persistence::Registry::GetExePathW();

        // ── Renderer name ─────────────────────────────────────────────────────────
        m_rendererName = (app.renderer) ? app.renderer->GetName() : L"None";

        // ── Autostart (HKCU Run key) ──────────────────────────────────────────────
        m_autostartEnabled = false;
        m_autostartCmd.clear();
        {
            HKEY hk = nullptr;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, Constants::Registry::RUN_KEY, 0,
                              KEY_QUERY_VALUE, &hk) == ERROR_SUCCESS) {
                DWORD sz = 0, type = 0;
                if (RegQueryValueExW(hk, Constants::Registry::RUN_VALUE_NAME,
                                     nullptr, &type, nullptr, &sz) == ERROR_SUCCESS && sz > 0) {
                    std::wstring val(sz / sizeof(wchar_t), L'\0');
                    if (RegQueryValueExW(hk, Constants::Registry::RUN_VALUE_NAME,
                                         nullptr, &type,
                                         reinterpret_cast<BYTE *>(val.data()), &sz) == ERROR_SUCCESS) {
                        while (!val.empty() && val.back() == L'\0') val.pop_back();
                        m_autostartEnabled = true;
                        m_autostartCmd = std::move(val);
                    }
                }
                RegCloseKey(hk);
            }
        }
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // OpenRegedit  — navigate regedit to a full "Computer\HKEY_..." key path
    // ─────────────────────────────────────────────────────────────────────────────

    void StatsWnd::OpenRegedit(const std::wstring &fullKeyPath) {
        // Regedit has no command line for "open at this key" — the only way is
        // to write its LastKey value and let it restore there on launch.
        //
        // A DEDICATED instance must not: it writes nothing to the registry, and
        // it has no registry settings to inspect anyway (they live in its .ini),
        // so the jump target would be meaningless. Regedit still opens, just
        // wherever it was left.
        if (!Dedicated::IsDedicatedFlag()) {
            HKEY hk = nullptr;
            if (RegCreateKeyExW(HKEY_CURRENT_USER,
                                L"Software\\Microsoft\\Windows\\CurrentVersion\\Applets\\Regedit",
                                0, nullptr, 0, KEY_SET_VALUE, nullptr, &hk, nullptr) == ERROR_SUCCESS) {
                RegSetValueExW(hk, L"LastKey", 0, REG_SZ,
                               reinterpret_cast<const BYTE *>(fullKeyPath.c_str()),
                               static_cast<DWORD>((fullKeyPath.size() + 1) * sizeof(wchar_t)));
                RegCloseKey(hk);
            }
        }
        ShellExecuteW(nullptr, L"open", L"regedit.exe", nullptr, nullptr, SW_SHOW);
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // Show
    // ─────────────────────────────────────────────────────────────────────────────

    void StatsWnd::Show() {
        if (!m_hWnd) return;
        m_view.scrollY = 0;
        m_links.clear();
        GatherStats();

        ShowCenterOverParent();
        InvalidateRect(m_hWnd, nullptr, FALSE);
    }

    void StatsWnd::Refresh() {
        if (!m_hWnd || !IsWindowVisible(m_hWnd)) return;
        m_links.clear();
        GatherStats();
        InvalidateRect(m_hWnd, nullptr, FALSE);
    }

    void StatsWnd::EnsureBackBuffer(HDC refDC, int w, int h) {
        if (m_bbDC && w == m_bbW && h == m_bbH) return;
        DestroyBackBuffer();
        m_bbDC = CreateCompatibleDC(refDC);
        m_bbBmp = CreateCompatibleBitmap(refDC, w, h);
        m_bbBmpOld = static_cast<HBITMAP>(SelectObject(m_bbDC, m_bbBmp));
        m_bbW = w;
        m_bbH = h;
    }

    void StatsWnd::DestroyBackBuffer() {
        if (m_bbDC) {
            if (m_bbBmpOld) SelectObject(m_bbDC, m_bbBmpOld);
            DeleteDC(m_bbDC);
            m_bbDC = nullptr;
        }
        if (m_bbBmp) {
            DeleteObject(m_bbBmp);
            m_bbBmp = nullptr;
        }
        m_bbBmpOld = nullptr;
        m_bbW = m_bbH = 0;
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // Message handler
    // ─────────────────────────────────────────────────────────────────────────────

    // One wheel "line" is roughly one row of this panel's stacked text. The base
    // applies the user's Mouse setting and the Shift accelerator on top.
    int StatsWnd::ScrollLinePx(const UI::ScrollView &) const {
        return static_cast<int>(10.0f * app.dpiScale);
    }

    LRESULT StatsWnd::HandlePanelMessage(UINT message, WPARAM wParam, LPARAM lParam) {
        if (message == WM_ERASEBKGND) return 1;
        switch (message) {
            // No wheel, drag, paging or bar-cursor cases: FloatingPanelWnd
            // handles all of them against ScrollViewAt() and consumes the
            // message before this panel is asked.

            // ── Click on a link ───────────────────────────────────────────────────────
            //
            // A thumb release never arrives here — the base holds capture for
            // the whole drag — so this cannot mistake letting go of the
            // scrollbar over a link for a click on that link.
            case WM_LBUTTONUP: {
                POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                for (auto &lnk: m_links) {
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
                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(m_hWnd, &pt);
                for (auto &lnk: m_links) {
                    if (PtInRect(&lnk.rect, pt)) {
                        SetCursor(Constants::Cursors::CURR_CLICK);
                        return TRUE;
                    }
                }
                break;
            }

            // ── Paint ─────────────────────────────────────────────────────────────────
            case WM_PAINT: {
                m_links.clear();
                PAINTSTRUCT ps;
                HDC screenDC = BeginPaint(m_hWnd, &ps);
                RECT rc;
                GetClientRect(m_hWnd, &rc);
                EnsureBackBuffer(screenDC, rc.right, rc.bottom);
                HDC hdc = m_bbDC;

                // Background
                COLORREF bgColor = GetBgColor();
                FillRect(hdc, &rc, UI::Gdi::Brush(bgColor));
                SetBkMode(hdc, TRANSPARENT);

                // ── Metrics ──────────────────────────────────────────────────────────
                const int dpi = static_cast<int>(app.dpiScale * 96);
                const int pad = MulDiv(14, dpi, 96);
                const int row = MulDiv(20, dpi, 96);
                const int sec = MulDiv(18, dpi, 96);
                const int gap = MulDiv(6, dpi, 96);
                const int sgap = MulDiv(16, dpi, 96);
                const int fs = MulDiv(12, dpi, 96);
                const int fss = MulDiv(11, dpi, 96);
                // The shared thickness, not a local 6 — this panel's bar was half
                // the width of every other one in the app.
                const int sb = UI::ScrollBarThicknessPx(app.dpiScale);
                const int colR = rc.right - pad - sb;

                // 3-column boundaries for cache rows
                const int c2 = pad + static_cast<int>((colR - pad) * 0.52f); // size right edge
                const int c3 = colR;

                // ── Fonts ─────────────────────────────────────────────────────────────
                if (dpi != m_cachedFontDpi) {
                    if (m_hFontBody) {
                        DeleteObject(m_hFontBody);
                        m_hFontBody = nullptr;
                    }
                    if (m_hFontBold) {
                        DeleteObject(m_hFontBold);
                        m_hFontBold = nullptr;
                    }
                    if (m_hFontSec) {
                        DeleteObject(m_hFontSec);
                        m_hFontSec = nullptr;
                    }
                    if (m_hFontLink) {
                        DeleteObject(m_hFontLink);
                        m_hFontLink = nullptr;
                    }
                    m_hFontBody = CreateFontW(-fs, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                              DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
                    m_hFontBold = CreateFontW(-fs, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                              DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
                    m_hFontSec = CreateFontW(-fss, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                             DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
                    m_hFontLink = CreateFontW(-fs, 0, 0, 0, FW_NORMAL, 0, Constants::Links::UNDERLINE, 0, DEFAULT_CHARSET,
                                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                              DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
                    m_cachedFontDpi = dpi;
                }
                HFONT hOld = static_cast<HFONT>(SelectObject(hdc, m_hFontBody));

                // ── Colors ────────────────────────────────────────────────────────────
                const COLORREF clrCyan = Constants::Theme::ThemedColor(0.39f, 0.78f, 1.0f, app.themeFactor);
                const COLORREF clrLink = Constants::Theme::ThemedColor(
                        Constants::Links::COLOR_R_F, Constants::Links::COLOR_G_F,
                        Constants::Links::COLOR_B_F, app.themeFactor);
                const COLORREF clrGreen = Constants::Theme::ThemedColor(0.40f, 0.90f, 0.55f, app.themeFactor);
                const COLORREF clrYellow = Constants::Theme::ThemedColor(1.0f, 0.87f, 0.0f, app.themeFactor);
                const COLORREF clrOrange = Constants::Theme::ThemedColor(1.0f, 0.62f, 0.25f, app.themeFactor);
                const COLORREF clrLabel = Constants::Theme::ThemedGray(0.58f, app.themeFactor);
                const COLORREF clrValue = Constants::Theme::ThemedGray(0.90f, app.themeFactor);
                const COLORREF clrDim = Constants::Theme::ThemedGray(0.22f, app.themeFactor);
                const COLORREF clrSepLine = Constants::Theme::ThemedColor(0.18f, 0.44f, 0.60f, app.themeFactor);

                // ── Drawing helpers ───────────────────────────────────────────────────
                int y = pad - m_view.scrollY;

                // Label on left, underlined clickable value on right
                auto linkRow = [&](const wchar_t *label, const std::wstring &display,
                                   const std::wstring &target, bool isReg) {
                    if (y + row > 0 && y < rc.bottom) {
                        SelectObject(hdc, m_hFontBody);
                        SetTextColor(hdc, clrLabel);
                        RECT rL = {pad + MulDiv(6, dpi, 96), y, c3 - MulDiv(4, dpi, 96), y + row};
                        DrawTextW(hdc, label, -1, &rL, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                        SelectObject(hdc, m_hFontLink);
                        SetTextColor(hdc, clrLink);
                        RECT rV = {pad, y, c3, y + row};
                        DrawTextW(hdc, display.c_str(), -1, &rV,
                                  DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                        m_links.push_back({{pad, y, c3, y + row}, target, isReg});
                    }
                    y += row;
                };

                // Simple label + right-aligned value
                auto row2 = [&](const wchar_t *label, const std::wstring &val,
                                COLORREF valColor, bool bold = false) {
                    if (y + row > 0 && y < rc.bottom) {
                        SelectObject(hdc, bold ? m_hFontBold : m_hFontBody);
                        RECT rL = {pad + MulDiv(6, dpi, 96), y, c3 - MulDiv(4, dpi, 96), y + row};
                        RECT rV = {pad, y, c3, y + row};
                        SetTextColor(hdc, clrLabel);
                        DrawTextW(hdc, label, -1, &rL, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                        SetTextColor(hdc, valColor);
                        DrawTextW(hdc, val.c_str(), -1, &rV, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
                    }
                    y += row;
                };

                // Section header with left accent stripe + underline
                auto section = [&](const wchar_t *title, COLORREF accentColor) {
                    y += MulDiv(4, dpi, 96);
                    if (y + sec > 0 && y < rc.bottom) {
                        // Left accent stripe
                        RECT strp = {pad, y + MulDiv(2, dpi, 96), pad + MulDiv(3, dpi, 96), y + sec - MulDiv(2, dpi, 96)};
                        FillRect(hdc, &strp, UI::Gdi::Brush(accentColor));
                        // Title
                        SelectObject(hdc, m_hFontSec);
                        SetTextColor(hdc, accentColor);
                        RECT r = {pad + MulDiv(8, dpi, 96), y, c3, y + sec};
                        DrawTextW(hdc, title, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                    }
                    y += sec;
                    // Underline
                    if (y >= 0 && y < rc.bottom) {
                        HPEN hOldP = static_cast<HPEN>(SelectObject(hdc, UI::Gdi::Pen(clrSepLine)));
                        MoveToEx(hdc, pad, y, nullptr);
                        LineTo(hdc, c3, y);
                        SelectObject(hdc, hOldP);
                    }
                    y += gap + MulDiv(2, dpi, 96);
                };

                // Thin dotted separator
                auto dotSep = [&]() {
                    if (y >= 0 && y < rc.bottom) {
                        HPEN hPen = CreatePen(PS_DOT, 1, clrDim);
                        HPEN hOldP = static_cast<HPEN>(SelectObject(hdc, hPen));
                        MoveToEx(hdc, pad + MulDiv(6, dpi, 96), y, nullptr);
                        LineTo(hdc, c3, y);
                        SelectObject(hdc, hOldP);
                        DeleteObject(hPen);
                    }
                    y += MulDiv(5, dpi, 96);
                };

                // 3-column cache row: name | size | count
                auto cacheRow = [&](const CacheFile &e, bool altBg) {
                    if (y + row > 0 && y < rc.bottom) {
                        if (altBg) {
                            RECT rAlt = {pad, y, c3, y + row};
                            FillRect(hdc, &rAlt, UI::Gdi::Brush(
                                    Constants::Theme::ThemedGray(0.06f, app.themeFactor)));
                        }
                        // Name
                        SelectObject(hdc, m_hFontBody);
                        SetTextColor(hdc, clrValue);
                        RECT rName = {pad + MulDiv(6, dpi, 96), y, c2 - MulDiv(4, dpi, 96), y + row};
                        DrawTextW(hdc, e.name.c_str(), -1, &rName, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                        // Size
                        SetTextColor(hdc, clrGreen);
                        RECT rSz = {c2 - MulDiv(100, dpi, 96), y, c2, y + row};
                        DrawTextW(hdc, FormatBytes(e.bytes).c_str(), -1, &rSz, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
                        // Count  (estimated — prefix with ~)
                        std::wstring cntStr = (e.count < 0) ? L"–" : (L"~" + FormatCount(e.count));
                        SetTextColor(hdc, e.count >= 0 ? clrCyan : clrDim);
                        RECT rCnt = {c2 + MulDiv(4, dpi, 96), y, c3, y + row};
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
                    RECT rhFile = {pad + MulDiv(6, dpi, 96), y, c2, y + row};
                    RECT rhSize = {c2 - MulDiv(100, dpi, 96), y, c2, y + row};
                    RECT rhCnt = {c2 + MulDiv(4, dpi, 96), y, c3, y + row};
                    DrawTextW(hdc, L"File", -1, &rhFile, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                    DrawTextW(hdc, L"Size", -1, &rhSize, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
                    DrawTextW(hdc, L"Thumbnails", -1, &rhCnt, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
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
                    RECT rLabel = {pad + MulDiv(6, dpi, 96), y, c2, y + row};
                    DrawTextW(hdc, L"Total", -1, &rLabel, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                    SetTextColor(hdc, clrYellow);
                    RECT rSzT = {c2 - MulDiv(100, dpi, 96), y, c2, y + row};
                    DrawTextW(hdc, FormatBytes(m_cacheTotalBytes).c_str(), -1, &rSzT, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
                    std::wstring totCnt = L"~" + FormatCount(m_cacheTotalCount) + L" thumbs";
                    SetTextColor(hdc, clrYellow);
                    RECT rCntT = {c2 + MulDiv(4, dpi, 96), y, c3, y + row};
                    DrawTextW(hdc, totCnt.c_str(), -1, &rCntT, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
                }
                y += row + MulDiv(4, dpi, 96);

                // Clickable folder path link
                if (!m_thumbCachePath.empty()) {
                    if (y + row > 0 && y < rc.bottom) {
                        SelectObject(hdc, m_hFontLink);
                        SetTextColor(hdc, clrLink);
                        RECT rLink = {pad + MulDiv(6, dpi, 96), y, c3, y + row};
                        DrawTextW(hdc, (std::wstring(Constants::ThemeIcons::ICON_FOLDER_ARROW) + L"  " + m_thumbCachePath).c_str(), -1, &rLink,
                                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                        m_links.push_back({{rLink.left, y, c3, y + row}, m_thumbCachePath, false});
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
                    row2(L"Resolution", resBuf, clrValue);
                    if (m_imgFileBytes > 0)
                        row2(L"File size", FormatBytes(m_imgFileBytes), clrGreen);

                    // Megapixel
                    double mp = (double) m_imgW * m_imgH / 1'000'000.0;
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
                    if (m_lastLoadUs >= 0) {
                        wchar_t lBuf[32];
                        swprintf_s(lBuf, L"%.3f ms", m_lastLoadUs / 1000.0);
                        COLORREF lClr = m_lastLoadUs < 100'000
                                            ? clrGreen
                                            : m_lastLoadUs < 400'000
                                                  ? clrValue
                                                  : clrOrange;
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
                    y += MulDiv(4, dpi, 96);
                    dotSep();
                    for (auto &e: m_extStats) {
                        float pct = (m_playlistSize > 0)
                                        ? (float) e.count / m_playlistSize * 100.0f
                                        : 0.0f;
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

                row2(L"Folders visited", FormatCount(m_historyCount), clrValue);
                row2(L"App instances", FormatCount(m_instanceCount), clrValue);

                y += sgap;

                // ═════════════════════════════════════════════════════════════════════
                // Section 4b — Remote control
                //
                // The remote subsystem is roughly a fifth of the program and had no
                // presence on this panel at all, so "is my phone actually connected?"
                // — the question a user asks first — had no answer anywhere in the UI
                // except by opening F9 and counting rows.
                //
                // Every value below is an atomic load or a short list walk, audited on
                // 2026-08-05 as costing nothing on the keystroke path. This panel
                // repaints on a timer, not per frame, so even the list walks are free
                // here.
                // ═════════════════════════════════════════════════════════════════════
                {
                    const COLORREF clrTeal =
                        Constants::Theme::ThemedColor(0.30f, 0.85f, 0.85f, app.themeFactor);
                    section(L"  REMOTE CONTROL", clrTeal);

                    const bool serving = Remote::IsRunning();

                    // Colour carries the state: a listener that is open is a thing the
                    // user should be able to see at a glance, because it is the one
                    // setting here with a security meaning.
                    row2(L"Local server", serving ? L"Running" : L"Stopped",
                         serving ? clrGreen : clrDim, true);

                    if (serving) {
                        const std::wstring ep = Remote::BoundEndpoint();
                        if (!ep.empty()) row2(L"  Listening on", ep.c_str(), clrValue);

                        row2(L"  Encryption",
                             Remote::IsEncrypted() ? L"TLS" : L"Plaintext (loopback)",
                             Remote::IsEncrypted() ? clrGreen : clrOrange);

                        wchar_t buf[32];
                        swprintf_s(buf, L"%d", Remote::ActiveConnections());
                        row2(L"  Clients connected", buf, clrValue);

                        // Observers are the clients being PUSHED events, which is what
                        // a phone's live preview uses. Distinct from "connected": a
                        // client can be attached and watching nothing.
                        //
                        // NAMED, not counted. "Yes" answered whether anything was
                        // watching; with three tablets on a wall the question is
                        // WHICH, and the answer already exists — `hello <name>` is
                        // sent by every client that has one, and the connection list
                        // now carries whether that client asked to observe.
                        {
                            const std::vector<Remote::ClientInfo> conns = Remote::Connections();

                            std::wstring watchers;
                            int watching = 0;
                            for (const Remote::ClientInfo &c : conns) {
                                if (!c.observing) continue;
                                ++watching;
                                if (!watchers.empty()) watchers += L", ";
                                // A client that never sent `hello` falls back to its
                                // address: unnamed is not anonymous, and an operator
                                // deciding what to unplug needs something to go on.
                                watchers += c.name.empty() ? c.address : c.name;
                            }

                            if (watching == 0) {
                                row2(L"  Watching (observe)", L"No", clrDim);
                            } else {
                                row2(L"  Watching (observe)", watchers, clrGreen);
                            }
                        }
                    }

                    // CONFIGURED values, shown whether or not the listener is up —
                    // "why will it not start" is asked with the server stopped, and
                    // a panel that hides its settings until it works is no help then.
                    {
                        // Remote::Settings is the STRUCT; the accessors live directly
                        // in namespace Remote, not in a namespace of that name.
                        const Remote::Settings &cfg = Remote::Config();

                        if (!cfg.name.empty())
                            row2(L"  Instance name", cfg.name.c_str(), clrValue);

                        wchar_t buf[64];
                        swprintf_s(buf, L"%d", cfg.port);
                        row2(L"  Configured port", buf, clrValue);

                        swprintf_s(buf, L"%d", cfg.maxConnections);
                        row2(L"  Max connections", buf, clrValue);

                        // A password is the ONLY thing standing between the AllowList
                        // and someone who can reach the port, so its absence is worth
                        // colouring rather than merely stating.
                        const bool hasPw = !cfg.passwordHash.empty();
                        row2(L"  Password", hasPw ? L"Set" : L"NOT SET",
                             hasPw ? clrGreen : clrOrange);

                        // An empty AllowList and a "*" entry are very different from a
                        // named list, and both are easy to leave behind after testing.
                        swprintf_s(buf, L"%zu entr%s", cfg.allowList.size(),
                                   cfg.allowList.size() == 1 ? L"y" : L"ies");
                        bool wildcard = false;
                        for (const std::wstring &a : cfg.allowList)
                            if (a == L"*") wildcard = true;
                        row2(L"  AllowList",
                             cfg.allowList.empty() ? L"Empty" : (wildcard ? L"* (any address)" : buf),
                             wildcard ? clrOrange : (cfg.allowList.empty() ? clrDim : clrValue));

                        row2(L"  Autostart", cfg.autostart ? L"On" : L"Off",
                             cfg.autostart ? clrValue : clrDim);
                    }

                    const int targets  = Remote::Mirror::ConnectedCount();
                    const int mirrored = Remote::Mirror::MirroredLiveCount();
                    const int declared = Remote::Mirror::TargetCount();
                    wchar_t tbuf[80];
                    swprintf_s(tbuf, L"%d connected   %d mirrored   (%d saved)",
                               targets, mirrored, declared);
                    row2(L"Driving others", declared > 0 ? tbuf : L"None saved",
                         targets > 0 ? clrValue : clrDim, true);

                    row2(L"  Mirror session", Remote::Mirror::SessionActive() ? L"Active" : L"Idle",
                         Remote::Mirror::SessionActive() ? clrGreen : clrDim);

                    // Off by default and cheap to forget about, but it writes a row
                    // per command while on.
                    //
                    // BOTH DESTINATIONS, because they are independent switches and
                    // either one alone means work is happening. Reporting only the
                    // panel's recording state said "Off" while a file was being
                    // written to disk — the one line here whose whole job is to
                    // stop a diagnostic being left on and forgotten.
                    {
                        const bool rec  = Remote::Log::IsEnabled();
                        const bool file = Remote::Log::FileLoggingIsOn();

                        const wchar_t *what = L"Off";
                        if (rec && file) what = L"Recording + file";
                        else if (rec)    what = L"Recording";
                        else if (file)   what = L"To file";

                        row2(L"  Wire log (Ctrl+F12)", what,
                             (rec || file) ? clrOrange : clrDim);
                    }
                }

                y += sgap;

                // ═════════════════════════════════════════════════════════════════════
                // Section 5 — Threads & Memory
                // ═════════════════════════════════════════════════════════════════════
                const COLORREF clrPurple = Constants::Theme::ThemedColor(0.80f, 0.50f, 1.0f, app.themeFactor);
                section(L"  THREADS & MEMORY", clrPurple);

                {
                    int totalThreads = 1 + m_ioThreads + m_wicThreads + m_dirThumbThreads + m_writeQueueThreads + m_dirWatcherThreads;
                    wchar_t buf[32];
                    swprintf_s(buf, L"%d", totalThreads);
                    row2(L"Accounted threads", buf, clrValue, true);
                }

                // THE REAL NUMBER, asked of the OS rather than added up from the
                // pools this panel knows about.
                //
                // The hand-counted total above misses everything nobody remembered to
                // register: one socket thread per remote client, one sender thread per
                // mirror target, and whatever the CRT, COM, WIC and the D3D/DXGI
                // runtime start on their own — which is most of them. Reporting the
                // sum of known pools as "total" was simply wrong, and wrong in the
                // direction that hides a leak.
                //
                // A snapshot walks every thread on the system, so this belongs on a
                // timer-driven panel and nowhere near a hot path.
                {
                    int osThreads = 0;
                    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
                    if (snap != INVALID_HANDLE_VALUE) {
                        THREADENTRY32 te{};
                        te.dwSize = sizeof(te);
                        const DWORD self = GetCurrentProcessId();
                        if (Thread32First(snap, &te)) {
                            do {
                                if (te.th32OwnerProcessID == self) ++osThreads;
                            } while (Thread32Next(snap, &te));
                        }
                        CloseHandle(snap);
                    }
                    wchar_t buf[64];
                    swprintf_s(buf, L"%d  (OS count)", osThreads);
                    row2(L"Live OS threads", buf, osThreads > 0 ? clrGreen : clrDim, true);
                }
                {
                    // The two pools that grow with what is CONNECTED rather than with
                    // the machine, so they are the ones that explain a number moving
                    // while nothing else changed.
                    wchar_t buf[64];
                    swprintf_s(buf, L"%d client + %d mirror sender",
                               Remote::IsRunning() ? Remote::ActiveConnections() : 0,
                               Remote::Mirror::ConnectedCount());
                    row2(L"  Remote threads", buf, clrValue);
                }
                {
                    wchar_t buf[32];
                    swprintf_s(buf, L"%d  (UI + render)", 1);
                    row2(L"  UI thread", buf, clrValue);
                }
                {
                    wchar_t buf[32];
                    swprintf_s(buf, L"%d  (%s)", m_ioThreads,
                               m_ioThreads <= 1 ? L"HDD mode" : L"SSD/NVMe mode");
                    row2(L"  IO worker", buf, clrCyan);
                    if (m_ioPending > 0) {
                        wchar_t q[32];
                        swprintf_s(q, L"%d pending", m_ioPending);
                        row2(L"    queue", q, clrOrange);
                    }
                }
                {
                    wchar_t buf[32];
                    swprintf_s(buf, L"%d  (WIC decode)", m_wicThreads);
                    row2(L"  WIC decoder", buf, clrCyan);
                    if (m_wicPending > 0) {
                        wchar_t q[32];
                        swprintf_s(q, L"%d pending", m_wicPending);
                        row2(L"    queue", q, clrOrange);
                    }
                }
                {
                    wchar_t buf[32];
                    swprintf_s(buf, L"%d  (dir thumbnails)", m_dirThumbThreads);
                    row2(L"  Dir thumbnail", buf, clrCyan);
                    if (m_dirThumbPending > 0) {
                        wchar_t q[32];
                        swprintf_s(q, L"%d pending", m_dirThumbPending);
                        row2(L"    queue", q, clrOrange);
                    }
                }
                {
                    wchar_t buf[64];
                    swprintf_s(buf, L"%d  (async registry / file drain)", m_writeQueueThreads);
                    row2(L"  Write queue", buf, clrCyan);
                }
                if (m_dirWatcherThreads > 0) {
                    wchar_t buf[32];
                    swprintf_s(buf, L"%d  (dir change watch)", m_dirWatcherThreads);
                    row2(L"  Dir watcher", buf, clrCyan);
                }
                {
                    wchar_t buf[32];
                    swprintf_s(buf, L"%d", app.hardwareThreads);
                    row2(L"  CPU logical cores", buf, clrValue);
                }

                dotSep();

                row2(L"Working set", FormatBytes(m_memWorkingSet), clrGreen, true);
                row2(L"Peak working set", FormatBytes(m_memPeak), clrValue);
                row2(L"Private bytes", FormatBytes(m_memPrivate), clrValue);
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
                                   static_cast<int>(app.vramCacheCount));
                        COLORREF c = (m_imgCacheCount == static_cast<int>(app.vramCacheCount))
                                         ? clrOrange
                                         : clrValue;
                        row2(L"Image cache", buf, c, true);
                    }
                    if (m_imgCacheBytes > 0)
                        row2(L"  VRAM used", FormatBytes(m_imgCacheBytes), clrGreen);

                    if (m_dirThumbCacheCount > 0) {
                        dotSep();
                        wchar_t buf[32];
                        swprintf_s(buf, L"%d entries", m_dirThumbCacheCount);
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
                    const wchar_t *vmNames[] = {
                        L"",
                        L"Fit to View",
                        L"Fit to Width",
                        L"Fit to Height",
                        L"Fill Window",
                        L"Original Size"
                    };
                    int vmi = static_cast<int>(app.viewMode);
                    if (vmi >= 1 && vmi <= 5)
                        row2(L"View mode", vmNames[vmi], clrValue);
                }
                {
                    const wchar_t *sortNames[] = {L"Name", L"Date modified", L"File size", L"Extension", L"Disk order"};
                    int si = std::clamp(app.fileHandlerDefaultSortOrder, 0, 4);
                    std::wstring s = sortNames[si];
                    if (app.fileHandlerIsReverseSortOrder) s += std::wstring(L"  ") + Constants::ThemeIcons::ICON_ARROW_DOWN;
                    row2(L"Sort order", s, clrValue);
                }
                {
                    wchar_t buf[16];
                    swprintf_s(buf, L"%.0f%%", app.dpiScale * 100.0f);
                    row2(L"DPI scale", buf, clrValue);
                }
                {
                    wchar_t buf[32];
                    swprintf_s(buf, L"%d × %d", app.screenW, app.screenH);
                    row2(L"Screen resolution", buf, clrValue);
                }

                // ─────────────────────────────────────────────────────────────
                // Monitors.
                //
                // Enumerated through MonitorInfo so this list is in the SAME
                // order Ctrl+M walks — left to right by virtual-desktop
                // position, not driver order. A panel that numbered them
                // differently from the key that moves between them would be
                // worse than not numbering them at all.
                //
                // Read on each paint rather than cached: a monitor can be
                // unplugged, a laptop undocked, or the arrangement changed in
                // Settings while this panel is open, and a stale list here is
                // indistinguishable from a wrong one. The call is a handful of
                // OS queries on a panel that only repaints when visible.
                // ─────────────────────────────────────────────────────────────
                {
                    const std::vector<MonitorInfo::Entry> mons = MonitorInfo::Enumerate();
                    HWND hOwner = GetParent(m_hWnd) ? GetParent(m_hWnd) : m_hWnd;
                    const int onIdx = MonitorInfo::IndexOfWindow(hOwner, mons);

                    wchar_t buf[64];
                    swprintf_s(buf, L"%zu", mons.size());
                    row2(L"Monitors", buf, clrValue);

                    for (size_t i = 0; i < mons.size(); ++i) {
                        const MonitorInfo::Entry &m = mons[i];

                        // "1." rather than "Monitor 1" — the number is the same
                        // one Ctrl+M reports, and the column is narrow.
                        wchar_t label[32];
                        swprintf_s(label, L"  %zu.", i + 1);

                        std::wstring val = m.name;

                        wchar_t geo[96];
                        swprintf_s(geo, L"  %d × %d", m.Width(), m.Height());
                        val += geo;

                        if (m.refreshHz > 0) {
                            swprintf_s(geo, L" @ %d Hz", m.refreshHz);
                            val += geo;
                        }
                        if (m.dpi > 0) {
                            // Shown as a percentage, matching the DPI scale row
                            // above and the number Windows itself displays —
                            // "144 dpi" makes the reader do the division.
                            swprintf_s(geo, L"  %d%%", MulDiv(static_cast<int>(m.dpi), 100, 96));
                            val += geo;
                        }
                        if (m.primary) val += L"  primary";

                        // The one the viewer is actually on, called out because
                        // that is the question this panel gets opened to answer.
                        const bool here = (onIdx >= 0 && static_cast<size_t>(onIdx) == i);
                        if (here) val += L"  ← qIV";

                        row2(label, val, here ? clrGreen : clrValue, here);
                    }

                    // The bounding box of every screen together. Only shown with
                    // more than one, where it is the number that explains window
                    // placement and mirroring behaviour — with a single monitor
                    // it just repeats the resolution row above.
                    if (mons.size() > 1) {
                        const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
                        const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
                        if (vw > 0 && vh > 0) {
                            wchar_t vbuf[48];
                            swprintf_s(vbuf, L"%d × %d", vw, vh);
                            row2(L"  Desktop spans", vbuf, clrValue);
                        }
                    }
                }

                // Uptime.
                //
                // Taken from the PROCESS creation time rather than a timer this
                // app starts: a counter of its own would only ever measure how
                // long the panel had been able to count, and this is the number
                // that matters on a machine nobody is sitting at. An unattended
                // display running a slideshow is the case it answers — "has the
                // wall been up since Tuesday, or did something restart it?"
                {
                    FILETIME ftCreate = {}, ftExit = {}, ftKernel = {}, ftUser = {};
                    if (GetProcessTimes(GetCurrentProcess(), &ftCreate, &ftExit,
                                        &ftKernel, &ftUser)) {
                        FILETIME ftNow = {};
                        GetSystemTimeAsFileTime(&ftNow);

                        ULARGE_INTEGER a, b;
                        a.LowPart = ftCreate.dwLowDateTime;  a.HighPart = ftCreate.dwHighDateTime;
                        b.LowPart = ftNow.dwLowDateTime;     b.HighPart = ftNow.dwHighDateTime;

                        if (b.QuadPart > a.QuadPart) {
                            // 100-nanosecond ticks to seconds.
                            const unsigned long long secs = (b.QuadPart - a.QuadPart) / 10000000ULL;
                            const unsigned long long d = secs / 86400;
                            const unsigned long long h = (secs % 86400) / 3600;
                            const unsigned long long m = (secs % 3600) / 60;
                            const unsigned long long s = secs % 60;

                            wchar_t ubuf[64];
                            if (d > 0)      swprintf_s(ubuf, L"%llud %lluh %llum", d, h, m);
                            else if (h > 0) swprintf_s(ubuf, L"%lluh %llum", h, m);
                            else if (m > 0) swprintf_s(ubuf, L"%llum %llus", m, s);
                            else            swprintf_s(ubuf, L"%llus", s);
                            row2(L"Uptime", ubuf, clrValue);
                        }
                    }
                }
                {
                    wchar_t buf[16];
                    swprintf_s(buf, L"%d", app.hardwareThreads);
                    row2(L"CPU threads", buf, clrValue);
                }
                {
                    wchar_t buf[16];
                    swprintf_s(buf, L"%.0f%%", app.themeFactor * 100.0f);
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
                section(L"  CONFIGURATION", clrPurple);

                // WHICH STORE IS AUTHORITATIVE — the first thing to establish, because
                // every path below is only meaningful once you know whether this copy
                // is reading it. A dedicated instance keeps its settings in an INI
                // beside the exe and does not touch the keys listed underneath; a
                // normal one is the other way round. Showing the registry paths with
                // no indication of which mode is running let a dedicated instance look
                // as though it were ignoring settings that it was never reading.
                {
                    const bool dedicated = Dedicated::IsDedicatedFlag();
                    row2(L"Settings store",
                         dedicated ? L"INI file beside the exe  (dedicated instance)"
                                   : L"Registry  (HKCU)",
                         dedicated ? clrOrange : clrValue, true);
                }

                // The remote subsystem keeps its OWN file, separate from both of the
                // above, so "where does the server read its port from" has a third
                // answer that is easy to miss.
                {
                    const bool ini = Remote::IniExists();
                    row2(L"Remote server config",
                         ini ? (Remote::SectionExists()
                                    ? L"qivLocalServer.ini"
                                    : L"qivLocalServer.ini  (no section — defaults in use)")
                             : L"Not present — built-in defaults",
                         ini ? clrValue : clrDim);
                }

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
                m_view.contentH = y + m_view.scrollY;

                // ── Scrollbar ─────────────────────────────────────────────────────────
                // Which bars, where the content goes, and the clamp — one call.
                const int clientH = rc.bottom - rc.top;
                m_view.Layout(RECT{0, pad, rc.right, pad + (clientH - 2 * pad)}, sb);

                UI::DrawBars(hdc, m_view, app.dpiScale,
                             UI::ThemeScrollBarColors(app.themeFactor));

                SelectObject(hdc, hOld);
                // Fonts are cached members — not deleted here
                BitBlt(screenDC, 0, 0, rc.right, rc.bottom, hdc, 0, 0, SRCCOPY);
                EndPaint(m_hWnd, &ps);
                return 0;
            }

            default: break;
        }
        return DefWindowProcW(m_hWnd, message, wParam, lParam);
    }
} // namespace UI
