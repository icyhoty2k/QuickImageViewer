#pragma once

#include <windows.h>
#include <vector>
#include <string>
#include "ThumbnailPanelWnd.h"
#include "Thumbnail.h"
#include "../AppState.h"

namespace UI {
    class CacheWnd : public ThumbnailPanelWnd {
        public:
            void ClearThumbnailCache();

            // Compat wrappers so existing call-sites don't need renaming
            void SyncSelectionRectangle() {
                ThumbnailPanelWnd::SyncSelectionRectangle();
            }

            void UpdateCacheView() {
                ThumbnailPanelWnd::UpdateView();
            }

            void ToggleCacheWindow() {
                ThumbnailPanelWnd::Toggle();
            }

            void MoveCacheWindow() {
                ThumbnailPanelWnd::MovePanel();
            }

        protected:
            const wchar_t *ClassName() const override {
                return L"QIV_CacheWindow";
            }

            const wchar_t *WindowTitle() const override {
                return L"Cache";
            }

            int GetKeyToggle() const override;

            int GetKeyMove() const override;

            int GetKeyExtra() const override;

            void OnExtraKey() override {
                ClearThumbnailCache();
            }

            // CacheWnd owns its own data source (the VRAM bitmap cache) — it must
            // not be gated on app.playlist being populated.
            bool HasOwnPlaylist() const override { return true; }

            // Returns VRAM-cached file paths — base class handles all layout
            std::vector<std::wstring> GetSourceItems() const override;

            // Refresh the view whenever the active folder is rescanned so pruned
            // entries are removed from the display immediately.
            void OnFolderRefreshed(const std::wstring & /*dir*/,
                                   const std::vector<std::wstring> & /*playlist*/) override {
                UpdateCacheView();
            }

            // Delete from CacheWnd = evict from VRAM only; never touch the file on disk.
            void OnContextMenuDelete(const std::wstring &path) override {
                if (app.renderer) app.renderer->RemoveFromCache(path);
                UpdateCacheView();
            }

            const wchar_t *ContextMenuDeleteLabel() const override { return L"Remove from VCache"; }
            const wchar_t *ContextMenuExtraLabel()  const override { return L"Clear VCache"; }
            void           OnContextMenuExtra()           override { ClearThumbnailCache(); }
            bool           ShowContextMenuPaste()   const override { return false; }
    };
} // namespace UI
