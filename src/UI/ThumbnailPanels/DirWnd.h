// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#pragma once

#include <windows.h>
#include <vector>
#include <string>
#include <filesystem>
#include <algorithm>
#include <thread>
#include <atomic>
#include "ThumbnailPanelWnd.h"
#include "Thumbnail.h"
#include "../../Platform/FileHandler.h"
#include "../../Platform/Constants.h"

namespace UI {
    // =========================================================================
    // DirWatcher — watches one directory for filesystem changes and posts
    // WM_QIV_DIR_CHANGED to a given HWND. One instance per DirWnd/SpawnedDirWnd.
    // Main DirWnd targets m_hOwner (main HWND → existing debounce+reload chain).
    // SpawnedDirWnd targets m_hWnd (handled locally in DirWnd::HandleMessage).
    // =========================================================================
    struct DirWatcher {
        ~DirWatcher() { Stop(); }

        void Start(HWND hWnd, const std::wstring &dir);
        void Stop();

        static int ActiveCount() { return s_activeCount.load(std::memory_order_relaxed); }

    private:
        HANDLE m_hNotify = INVALID_HANDLE_VALUE;
        HANDLE m_hStop   = nullptr;
        std::thread m_thread;

        static std::atomic<int> s_activeCount;
    };

    class DirWnd : public ThumbnailPanelWnd {
        public:
            void DoClearDirThumbnailCache() override;

            void SyncDirSelectionRectangle() {
                ThumbnailPanelWnd::SyncSelectionRectangle();
            }

            // Public because UIManager calls it on the concrete F6 instance when
            // that instance is hidden and therefore in no layout slot.
            //
            // F6 holds a COPY of the viewer's playlist (or its own folder, when
            // F5 navigated it), so re-sorting app.playlist never reaches it. An
            // empty list needs nothing — GetSourceItems refills it from the
            // already-sorted app.playlist on next use.
            void OnSortOrderChanged() override {
                if (m_dirPlaylist.empty()) return;
                SortPathsInAppOrder(m_dirPlaylist);
                m_sourceDirty = true;
                UpdateView();
                SyncSelectionRectangle();
            }

            void UpdateDirView() {
                ThumbnailPanelWnd::UpdateView();
            }

            void ToggleDirWindow() {
                ThumbnailPanelWnd::Toggle();
            }

            void MoveDirWindow() {
                ThumbnailPanelWnd::MovePanel();
            }

            void HideDirWindow() {
                Hide(); // virtual dispatch → DirWnd::Hide() → stop watcher, then base
            }

        public:
            // Load playlist from folder (only used when F5 actively navigates)
            void LoadPlaylist(const std::wstring &folderPath);

            // Get the folder path currently displayed in F5
            std::wstring GetCurrentFolder() const {
                return m_currentFolder;
            }

            // Copy the sorted playlist (used to keep F5 in sync with main folder, isolated from spawned hijacking)
            void SetPlaylistCopy(const std::vector<std::wstring> &playlist) {
                m_dirPlaylist = playlist;
                m_thumbnails.clear(); // Clear cached thumbnails
                if (m_hWnd) {
                    // Force immediate rebuild: if visible, repaint now; if hidden, repaint when shown
                    InvalidateRect(m_hWnd, nullptr, FALSE);
                    if (IsWindowVisible(m_hWnd)) {
                        UpdateView(); // Immediate rebuild if visible
                    }
                }
            }

        public:
            void Hide() override;

        protected:
            LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) override;

            const wchar_t *ClassName() const override {
                return L"QIV_DirWindow";
            }

            const wchar_t *WindowTitle() const override {
                return L"Directory";
            }

            bool UsesDirThumbCache() const override {
                return true;
            }

            bool IsDirPanel() const override {
                return true;
            }

            int GetKeyToggle() const override;

            int GetKeyMove() const override;

            std::vector<std::wstring> GetSourceItems() const override;

            bool HasOwnPlaylist() const override {
                return true;
            }

            void PostBuildHook() override;

            void RefreshFromDisk(const std::wstring &dir) override;

            std::wstring GetPanelFolder() const override {
                if (!m_currentFolder.empty()) return m_currentFolder;
                if (!m_dirPlaylist.empty())
                    return fs::path(m_dirPlaylist[0]).parent_path().wstring();
                return {};
            }

            void OnFolderRefreshed(const std::wstring &dir,
                                   const std::vector<std::wstring> &playlist) override {
                // Reject scan results that belong to a different folder.
                // When a SpawnedDirWnd triggers a scan, F6 must not adopt
                // that result and wipe its own playlist and selection.
                const std::wstring myFolder = GetPanelFolder();
                if (!myFolder.empty()) {
                    bool match = false;
                    try {
                        match = std::filesystem::equivalent(
                                std::filesystem::path(myFolder), std::filesystem::path(dir));
                    } catch (...) {
                        match = (myFolder == dir);
                    }
                    if (!match) return;
                }

                // Watch this folder for changes; posts to m_hOwner so the main
                // window's debounce → ReloadCurrentDirectory chain handles reload.
                // Watch even when empty so adding the first image is detected.
                m_watcher.Start(m_hOwner, dir);

                if (playlist.empty()) {
                    std::error_code ec;
                    m_emptyDirMissing = !std::filesystem::is_directory(std::filesystem::path(dir), ec) || ec;
                    m_dirPlaylist.clear();
                    m_emptyDir = dir;
                    m_emptyDirPathLayout.Reset();
                    m_sourceDirty = true;
                    UpdateView();
                } else {
                    m_emptyDir.clear();
                    m_emptyDirActive = false;
                    m_emptyDirMissing = false;
                    m_emptyDirPathLayout.Reset();
                    SetPlaylistCopy(playlist);
                }
            }

        protected:
            DirWatcher m_watcher;

        private:
            std::wstring m_currentFolder; // Track current folder for history marking
            std::vector<std::wstring> m_dirPlaylist; // F5 owns its own playlist, isolated from app.playlist
    };

    // =========================================================================
    // SpawnedDirWnd
    //
    // Spawned from HistoryListWnd (Shift+Enter). Owns its own playlist so it
    // never touches app.playlist. Each instance gets its own swap chain via
    // ThumbnailPanelWnd::CreateDeviceResources — no special panel type needed.
    // Clicking a thumbnail calls OpenSpecificImage (playlistIndex == -1 path
    // in ThumbnailPanelWnd) which loads the file into the main viewer.
    // =========================================================================
    class SpawnedDirWnd : public DirWnd {
        public:
            // slot: 0=left, 1=right, 2=center — used for a unique Win32 class name
            // so each instance registers its own WNDCLASS and gets its own HWND
            // without colliding with the primary DirWnd or each other.
            explicit SpawnedDirWnd(int slot) : m_slot(slot) {}

            void LoadFolder(const std::wstring &folderPath) {
                m_folderPath = folderPath;
                m_localPlaylist.clear();
                m_thumbnails.clear(); // force full rebuild; recycled panel may have stale thumbnails

                // AND the renderer's GPU cache for this panel, which m_thumbnails
                // above does not touch.
                //
                // These panels come from a pool of four and are RECYCLED: spawning
                // one picks the first hidden instance and retargets it here, under
                // the same HWND. The renderer keys its thumbnail cache by HWND, so
                // without this the previous folder's bitmaps stay resident —
                // unreachable, since lookup only ever asks for paths in the
                // current list, but still holding GPU memory.
                //
                // FileHandler already clears on its own folder-change paths. The
                // spawn path from HistoryListWnd did not, which is the one that
                // retargets a panel most often.
                ClearDirThumbnailCache();
                namespace fs = std::filesystem;
                fs::path dir(folderPath);
                // Non-throwing filesystem calls only — this runs on the UI thread
                // (Shift+Enter in HistoryListWnd). A filesystem_error escaping the
                // wndproc terminates the process (0xC0000409).
                std::error_code ec;
                if (!fs::is_directory(dir, ec) || ec)
                    return;

                // Collected into a ScanResult so the list can be handed to the
                // one sorter the background scan also uses. Size and time come
                // straight off the directory_entry, which cached them during
                // enumeration on Windows — no extra syscall — and the date and
                // size sort orders need them.
                ScanResult sr;
                for (auto it = fs::directory_iterator(
                             dir, fs::directory_options::skip_permission_denied, ec);
                     !ec && it != fs::directory_iterator(); it.increment(ec)) {
                    if (!it->is_regular_file(ec)) {
                        ec.clear();
                        continue;
                    }
                    if (!is_image_ext(it->path().extension().wstring())) continue;
                    fs::path canon = fs::canonical(it->path(), ec);
                    if (ec) {
                        ec.clear();
                        canon = it->path();
                    }
                    std::wstring canonStr = canon.wstring();

                    std::error_code entryEc;
                    const auto sz = it->file_size(entryEc);
                    sr.fileSizes[canonStr] = entryEc ? int64_t{0} : static_cast<int64_t>(sz);
                    entryEc.clear();
                    const auto tm = it->last_write_time(entryEc);
                    sr.fileTimes[canonStr] = entryEc ? fs::file_time_type{} : tm;

                    sr.playlist.push_back(std::move(canonStr));
                }

                // NOT std::sort. Ordinal order puts "img10" before "img2" and
                // ignores the user's date/size/type choice entirely, so this
                // panel disagreed with the background scan of the same folder.
                // The first click here triggers such a scan, the panel adopts
                // its result, and every thumbnail jumped to a new slot.
                SortScanResultInAppOrder(sr);
                m_localPlaylist = std::move(sr.playlist);

                // Watch for changes; posts to m_hWnd so this panel's
                // HandleMessage handles reload locally without touching app.playlist.
                m_watcher.Start(m_hWnd, folderPath);
            }

            std::wstring GetFolderPath() const {
                return m_folderPath;
            }

        protected:
            std::vector<std::wstring> GetSourceItems() const override {
                return m_localPlaylist;
            }

            bool HasOwnPlaylist() const override {
                return true;
            }

            // Overrides DirWnd's version — the list to re-sort is the local one,
            // and this panel's folder is usually not the folder the viewer shows.
            void OnSortOrderChanged() override {
                if (m_localPlaylist.empty()) return;
                SortPathsInAppOrder(m_localPlaylist);
                m_sourceDirty = true;
                UpdateView();
                SyncSelectionRectangle();
            }

            // Only sync when the scanned dir matches this panel's folder.
            void OnFolderRefreshed(const std::wstring &dir,
                                   const std::vector<std::wstring> &playlist) override {
                namespace fs = std::filesystem;
                bool match = false;
                try {
                    match = !m_folderPath.empty() && fs::equivalent(m_folderPath, dir);
                } catch (...) {
                    match = (m_folderPath == dir);
                }
                if (!match) return;

                if (playlist.empty()) {
                    std::error_code ec;
                    m_emptyDirMissing = !fs::is_directory(fs::path(dir), ec) || ec;
                    m_localPlaylist.clear();
                    m_emptyDir = dir;
                    m_emptyDirPathLayout.Reset();
                    m_sourceDirty = true;
                    UpdateView();
                } else {
                    m_emptyDir.clear();
                    m_emptyDirActive = false;
                    m_emptyDirMissing = false;
                    m_emptyDirPathLayout.Reset();
                    m_localPlaylist = playlist;
                    m_sourceDirty = true;
                    UpdateView();
                }
            }

            void RefreshFromDisk(const std::wstring &dir) override {
                if (m_folderPath.empty()) return;
                namespace fs = std::filesystem;
                bool match = false;
                try {
                    match = fs::equivalent(fs::path(m_folderPath), fs::path(dir));
                } catch (...) {
                    match = (m_folderPath == dir);
                }
                if (!match) return;
                LoadFolder(dir);
                m_sourceDirty = true; // force full thumbnail rebuild, not just geometry
                UpdateDirView();
            }

            std::wstring GetPanelFolder() const override {
                return m_folderPath;
            }

            // Each slot gets a unique Win32 class name so RegisterClassW doesn't
            // silently reuse the first registration for all three slots.
            const wchar_t *ClassName() const override {
                // Static storage per slot — safe because slot values are 0/1/2.
                static const wchar_t *names[] = {
                    L"QIV_SpawnedDirWindow_0",
                    L"QIV_SpawnedDirWindow_1",
                    L"QIV_SpawnedDirWindow_2",
                    L"QIV_SpawnedDirWindow_3"
                };
                return names[m_slot < 4 ? m_slot : 0];
            }

            const wchar_t *WindowTitle() const override {
                return L"Directory (Spawned)";
            }

        private:
            std::vector<std::wstring> m_localPlaylist;
            std::wstring m_folderPath;
            int m_slot = 0;
    };
} // namespace UI
