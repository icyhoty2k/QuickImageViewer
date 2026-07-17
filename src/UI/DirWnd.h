#pragma once

#include <windows.h>
#include <vector>
#include <string>
#include <filesystem>
#include <algorithm>
#include "ThumbnailPanelWnd.h"
#include "Thumbnail.h"
#include "../Platform/FileHandler.h"

namespace UI {
    class DirWnd : public ThumbnailPanelWnd {
        public:
            void DoClearDirThumbnailCache() override;

            void SyncDirSelectionRectangle() { ThumbnailPanelWnd::SyncSelectionRectangle(); }
            void UpdateDirView()             { ThumbnailPanelWnd::UpdateView(); }
            void ToggleDirWindow()           { ThumbnailPanelWnd::Toggle(); }
            void MoveDirWindow()             { ThumbnailPanelWnd::MovePanel(); }
            void HideDirWindow()             { ThumbnailPanelWnd::Hide(); }

        public:
            // Load playlist from folder (only used when F5 actively navigates)
            void LoadPlaylist(const std::wstring &folderPath);

            // Get the folder path currently displayed in F5
            std::wstring GetCurrentFolder() const { return m_currentFolder; }
            // Copy the sorted playlist (used to keep F5 in sync with main folder, isolated from spawned hijacking)
            void SetPlaylistCopy(const std::vector<std::wstring> &playlist) {
                m_dirPlaylist = playlist;
                m_thumbnails.clear();  // Clear cached thumbnails
                if (m_hWnd) {
                    // Force immediate rebuild: if visible, repaint now; if hidden, repaint when shown
                    InvalidateRect(m_hWnd, nullptr, FALSE);
                    if (IsWindowVisible(m_hWnd)) {
                        UpdateView();  // Immediate rebuild if visible
                    }
                }
            }

        protected:
            const wchar_t *ClassName()    const override { return L"QIV_DirWindow"; }
            const wchar_t *WindowTitle()  const override { return L"Directory"; }
            bool UsesDirThumbCache()      const override { return true; }
            bool IsDirPanel()             const override { return true; }

            int GetKeyToggle() const override;
            int GetKeyMove()   const override;

            std::vector<std::wstring> GetSourceItems() const override;
            bool HasOwnPlaylist() const override { return true; }

            void PostBuildHook() override;

            void RefreshFromDisk(const std::wstring &dir) override;

            std::wstring GetPanelFolder() const override {
                if (!m_currentFolder.empty()) return m_currentFolder;
                if (!m_dirPlaylist.empty())
                    return fs::path(m_dirPlaylist[0]).parent_path().wstring();
                return {};
            }

            // DirWnd always tracks app.playlist — no folder comparison needed.
            void OnFolderRefreshed(const std::wstring &dir,
                                   const std::vector<std::wstring> &playlist) override {
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
                    m_emptyDirActive  = false;
                    m_emptyDirMissing = false;
                    m_emptyDirPathLayout.Reset();
                    SetPlaylistCopy(playlist);
                }
            }

        private:
            std::wstring m_currentFolder;  // Track current folder for history marking
            std::vector<std::wstring> m_dirPlaylist;  // F5 owns its own playlist, isolated from app.playlist
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
                namespace fs = std::filesystem;
                fs::path dir(folderPath);
                // Non-throwing filesystem calls only — this runs on the UI thread
                // (Shift+Enter in HistoryListWnd). A filesystem_error escaping the
                // wndproc terminates the process (0xC0000409).
                std::error_code ec;
                if (!fs::is_directory(dir, ec) || ec)
                    return;
                for (auto it = fs::directory_iterator(
                             dir, fs::directory_options::skip_permission_denied, ec);
                     !ec && it != fs::directory_iterator(); it.increment(ec)) {
                    if (!it->is_regular_file(ec)) { ec.clear(); continue; }
                    if (!is_image_ext(it->path().extension().wstring())) continue;
                    fs::path canon = fs::canonical(it->path(), ec);
                    if (ec) { ec.clear(); canon = it->path(); }
                    m_localPlaylist.push_back(canon.wstring());
                }
                std::sort(m_localPlaylist.begin(), m_localPlaylist.end());
            }

            std::wstring GetFolderPath() const { return m_folderPath; }

        protected:
            std::vector<std::wstring> GetSourceItems() const override { return m_localPlaylist; }
            bool HasOwnPlaylist() const override { return true; }

            // Only sync when the scanned dir matches this panel's folder.
            void OnFolderRefreshed(const std::wstring &dir,
                                   const std::vector<std::wstring> &playlist) override {
                namespace fs = std::filesystem;
                bool match = false;
                try { match = !m_folderPath.empty() && fs::equivalent(m_folderPath, dir); }
                catch (...) { match = (m_folderPath == dir); }
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
                    m_emptyDirActive  = false;
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
                try { match = fs::equivalent(fs::path(m_folderPath), fs::path(dir)); }
                catch (...) { match = (m_folderPath == dir); }
                if (!match) return;
                LoadFolder(dir);
                m_sourceDirty = true; // force full thumbnail rebuild, not just geometry
                UpdateDirView();
            }

            std::wstring GetPanelFolder() const override { return m_folderPath; }

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

            const wchar_t *WindowTitle() const override { return L"Directory (Spawned)"; }

        private:
            std::vector<std::wstring> m_localPlaylist;
            std::wstring m_folderPath;
            int m_slot = 0;
    };

} // namespace UI
