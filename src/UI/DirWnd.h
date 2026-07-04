#pragma once

#include <windows.h>
#include <vector>
#include <string>
#include "ThumbnailPanelWnd.h"
#include "Thumbnail.h"

namespace UI {
    class DirWnd : public ThumbnailPanelWnd {
        public:
            void ClearDirThumbnailCache();

            // Compat wrappers
            void SyncDirSelectionRectangle() {
                ThumbnailPanelWnd::SyncSelectionRectangle();
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
                ThumbnailPanelWnd::Hide();
            }

        protected:
            const wchar_t *ClassName() const override {
                return L"QIV_DirWindow";
            }

            const wchar_t *WindowTitle() const override {
                return L"Directory";
            }

            // --- NEW: Tell the base class which panel this is ---
            RendererD2D::ThumbnailPanelType GetPanelType() const override {
                return RendererD2D::ThumbnailPanelType::Dir;
            }

            int GetKeyToggle() const override;

            int GetKeyMove() const override;

            // Returns the current folder playlist — base class handles all layout
            std::vector<std::wstring> GetSourceItems() const override;

            void PostBuildHook() override; // async decodes + legacy global sync
    };

    // Legacy globals — RendererD2D reads these directly
    extern float g_dirOffset;
    extern std::vector<Thumbnail> g_dirThumbnailObjects;
} // namespace UI
