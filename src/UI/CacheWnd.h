#pragma once

#include <windows.h>
#include <vector>
#include <string>
#include "ThumbnailPanelWnd.h"
#include "Thumbnail.h"
#include "../AppState.h"
#include "../Platform/Constants.h"

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
            void OnContextMenuDelete(const std::vector<std::wstring> &paths) override {
                if (app.renderer) {
                    for (const auto &p : paths) app.renderer->RemoveFromCache(p);
                }
                UpdateCacheView();
            }

            const wchar_t *ContextMenuDeleteLabel() const override { return L"Remove from VCache"; }
            const wchar_t *ContextMenuExtraLabel()  const override { return L"Clear VCache"; }
            void           OnContextMenuExtra()           override { ClearThumbnailCache(); }
            bool           ShowContextMenuPaste()   const override { return false; }

            // vRam(current/max) → VRAM size — uses actual GPU pixel dimensions, not disk size.
            std::pair<std::wstring, UINT32> BuildScrollbarLabel() const override {
                if (!app.renderer) return {{}, 0};
                int count = 0;
                UINT64 bytes = 0;
                app.renderer->GetImageCacheStats(count, bytes);
                std::wstring text = L"vRam";
                const UINT32 boldStart = 4; // everything after "vRam" is bold
                text += L'(' + std::to_wstring(count)
                      + L'/' + std::to_wstring(Constants::VRAM_CACHE_IMAGES_COUNT) + L')';
                text += L" → " + FormatDirSize(static_cast<int64_t>(bytes));
                return {std::move(text), boldStart};
            }
    };
} // namespace UI
