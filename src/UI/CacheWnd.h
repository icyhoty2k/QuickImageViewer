#pragma once

#include <windows.h>
#include <vector>
#include <string>
#include "ThumbnailPanelWnd.h"
#include "Thumbnail.h"

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

            // --- NEW: Tell the base class which panel this is ---
            RendererD2D::ThumbnailPanelType GetPanelType() const override {
                return RendererD2D::ThumbnailPanelType::Cache;
            }

            int GetKeyToggle() const override;

            int GetKeyMove() const override;

            int GetKeyExtra() const override;

            void OnExtraKey() override {
                ClearThumbnailCache();
            }

            // Returns VRAM-cached file paths — base class handles all layout
            std::vector<std::wstring> GetSourceItems() const override;

            void PostBuildHook() override; // syncs legacy globals for RendererD2D
    };

    // Legacy globals — RendererD2D reads these directly
    extern float g_cacheOffset;
    extern std::vector<Thumbnail> g_thumbnailObjects;
} // namespace UI
