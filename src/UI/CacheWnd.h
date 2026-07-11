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

            int GetKeyToggle() const override;

            int GetKeyMove() const override;

            int GetKeyExtra() const override;

            void OnExtraKey() override {
                ClearThumbnailCache();
            }

            // Returns VRAM-cached file paths — base class handles all layout
            std::vector<std::wstring> GetSourceItems() const override;
    };
} // namespace UI
