// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#include "DirWnd.h"
#include "../../AppState.h"
#include "../../Input/Shortcuts.h"
#include "../../Renderer/RendererD2D.h"
#include "../FloatingPanels/HistoryListWnd.h"

namespace UI {
    // -------------------------------------------------------------------------
    // DirWatcher
    // -------------------------------------------------------------------------
    std::atomic<int> DirWatcher::s_activeCount{0};

    void DirWatcher::Start(HWND hWnd, const std::wstring &dir) {
        if (!Constants::WATCH_DIR_FOR_CHANGES) return;
        Stop(); // stop any previous watch before starting a new one

        HANDLE hNotify = FindFirstChangeNotificationW(
            dir.c_str(), FALSE,
            FILE_NOTIFY_CHANGE_FILE_NAME | // file added / deleted / renamed
            FILE_NOTIFY_CHANGE_SIZE      | // file grew or shrank
            FILE_NOTIFY_CHANGE_LAST_WRITE); // file modified in-place
        if (hNotify == INVALID_HANDLE_VALUE) return;

        HANDLE hStop = CreateEventW(nullptr, /*manualReset=*/TRUE, FALSE, nullptr);
        if (!hStop) {
            FindCloseChangeNotification(hNotify);
            return;
        }

        m_hNotify = hNotify;
        m_hStop   = hStop;

        s_activeCount.fetch_add(1, std::memory_order_relaxed);
        m_thread = std::thread([hNotify, hStop, hWnd]() {
            HANDLE handles[2] = {hNotify, hStop};
            for (;;) {
                DWORD r = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
                if (r != WAIT_OBJECT_0) break; // stop event or error
                PostMessageW(hWnd, Constants::WM_QIV_DIR_CHANGED, 0, 0);
                if (!FindNextChangeNotification(hNotify)) break;
            }
        });
    }

    void DirWatcher::Stop() {
        if (m_hStop) SetEvent(m_hStop);
        if (m_thread.joinable()) {
            m_thread.join();
            s_activeCount.fetch_sub(1, std::memory_order_relaxed);
        }
        if (m_hNotify != INVALID_HANDLE_VALUE) {
            FindCloseChangeNotification(m_hNotify);
            m_hNotify = INVALID_HANDLE_VALUE;
        }
        if (m_hStop) {
            CloseHandle(m_hStop);
            m_hStop = nullptr;
        }
    }

    // -------------------------------------------------------------------------
    // DirWnd::Hide — stop watcher before hiding so no dangling thread runs
    // -------------------------------------------------------------------------
    void DirWnd::Hide() {
        m_watcher.Stop();
        ThumbnailPanelWnd::Hide();
    }

    // -------------------------------------------------------------------------
    // DirWnd::HandleMessage — intercept per-panel dir-change notifications.
    // Only SpawnedDirWnd instances ever receive WM_QIV_DIR_CHANGED on their
    // own HWND (their watcher targets m_hWnd). The main DirWnd's watcher
    // targets m_hOwner so the main window's debounce chain handles it.
    // -------------------------------------------------------------------------
    LRESULT DirWnd::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
        if (message == Constants::WM_QIV_DIR_CHANGED) {
            // Debounce: collapse rapid filesystem events into one reload.
            KillTimer(m_hWnd, Constants::PANEL_DIR_WATCHER_TIMER_ID);
            SetTimer(m_hWnd, Constants::PANEL_DIR_WATCHER_TIMER_ID,
                     Constants::DIR_WATCHER_DEBOUNCE_MS, nullptr);
            return 0;
        }
        if (message == WM_TIMER && wParam == Constants::PANEL_DIR_WATCHER_TIMER_ID) {
            KillTimer(m_hWnd, Constants::PANEL_DIR_WATCHER_TIMER_ID);
            const std::wstring folder = GetPanelFolder();
            if (!folder.empty()) {
                RefreshFromDisk(folder);
                UI::NotifyFolderContentsChanged(folder);
            }
            return 0;
        }
        return ThumbnailPanelWnd::HandleMessage(message, wParam, lParam);
    }


    // -------------------------------------------------------------------------
    // Shortcuts
    // -------------------------------------------------------------------------
    int DirWnd::GetKeyToggle() const {
        return Shortcuts::SC_PANEL_DIR_TOGGLE;
    }

    int DirWnd::GetKeyMove() const {
        return Shortcuts::SC_PANEL_DIR_MOVE;
    }

    // -------------------------------------------------------------------------
    // GetSourceItems — F5's own isolated playlist, auto-populated on first use
    // -------------------------------------------------------------------------
    std::vector<std::wstring> DirWnd::GetSourceItems() const {
        // On first use, copy app.playlist and lock it in place (protect from spawned hijacking)
        if (m_dirPlaylist.empty() && !app.playlist.empty()) {
            const_cast<DirWnd *>(this)->m_dirPlaylist = app.playlist;
        }
        return m_dirPlaylist;
    }

    // -------------------------------------------------------------------------
    // LoadPlaylist — populate F5's playlist from a folder
    // -------------------------------------------------------------------------
    void DirWnd::LoadPlaylist(const std::wstring &folderPath) {
        m_sourceDirty = true;
        m_dirPlaylist.clear();
        m_currentFolder = folderPath; // Track current folder for history marking
        std::filesystem::path dir(folderPath);
        if (!std::filesystem::exists(dir) || !std::filesystem::is_directory(dir))
            return;
        // Calculated speculation: pre-size to the previous scan's image count.
        m_dirPlaylist.reserve(DirScanReserveHint());
        for (const auto &entry: std::filesystem::directory_iterator(dir)) {
            if (!entry.is_regular_file()) continue;
            if (!is_image_ext(entry.path().extension().wstring())) continue;
            m_dirPlaylist.push_back(std::filesystem::canonical(entry.path()).wstring());
        }
        RecordDirScanCount(m_dirPlaylist.size());
    }

    // -------------------------------------------------------------------------
    // RefreshFromDisk — rescan this panel's folder if it matches `dir`
    // -------------------------------------------------------------------------
    void DirWnd::RefreshFromDisk(const std::wstring &dir) {
        // Determine which folder F6 is currently showing.
        std::wstring myDir = m_currentFolder;
        if (myDir.empty() && !m_dirPlaylist.empty())
            myDir = std::filesystem::path(m_dirPlaylist[0]).parent_path().wstring();
        if (myDir.empty()) return;

        bool match = false;
        try {
            match = std::filesystem::equivalent(std::filesystem::path(myDir),
                                                std::filesystem::path(dir));
        } catch (...) {
            match = (myDir == dir);
        }
        if (!match) return;

        LoadPlaylist(dir);
        UpdateDirView();
    }

    // -------------------------------------------------------------------------
    // PostBuildHook — queue async decodes for RendererD2D
    // -------------------------------------------------------------------------
    void DirWnd::PostBuildHook() {
        auto *r = dynamic_cast<RendererD2D *>(app.renderer.get());
        if (r) {
            for (const auto &t: m_thumbnails) {
                r->RequestDirThumbnail(t.filePath, m_hWnd);
            }
        }
    }

    // -------------------------------------------------------------------------
    // DoClearDirThumbnailCache
    // -------------------------------------------------------------------------
    void DirWnd::DoClearDirThumbnailCache() {
        if (!app.renderer) return;
        auto *r = dynamic_cast<RendererD2D *>(app.renderer.get());
        if (r) {
            r->ClearDirThumbnailCache(m_hWnd);
        }
    }
} // namespace UI
