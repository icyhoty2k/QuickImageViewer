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

        private:
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
                std::filesystem::path dir(folderPath);
                if (!std::filesystem::exists(dir) || !std::filesystem::is_directory(dir))
                    return;
                for (const auto &entry : std::filesystem::directory_iterator(dir)) {
                    if (!entry.is_regular_file()) continue;
                    if (!is_image_ext(entry.path().extension().wstring())) continue;
                    m_localPlaylist.push_back(std::filesystem::canonical(entry.path()).wstring());
                }
                std::sort(m_localPlaylist.begin(), m_localPlaylist.end());
            }

            std::wstring GetFolderPath() const { return m_folderPath; }

        protected:
            std::vector<std::wstring> GetSourceItems() const override { return m_localPlaylist; }
            bool HasOwnPlaylist() const override { return true; }

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
