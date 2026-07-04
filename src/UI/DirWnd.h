#pragma once

#include <windows.h>
#include <vector>

#include "IPanelWindow.h"
#include "Thumbnail.h"

namespace UI {
    class DirWnd : public IPanelWindow {
        // -------------------------------------------------------------------------
        // DirWnd  —  Current-folder image browser panel.
        //
        // Same visual language as CacheWnd (D2D thumbnails, hover/select border,
        // drag-to-scroll, click-to-navigate) but positioned as a floating popup
        // (default: centered) and its data comes from the file-system (all images
        // in the current folder) rather than the VRAM bitmap cache.
        //
        // Shortcuts:
        //   F5  —  Toggle panel (SC_PANEL_DIR_TOGGLE)
        //   F6  —  Cycle position: center / top / right / bottom / left (SC_PANEL_DIR_MOVE)
        //   Esc —  Hide panel (SC_LOCAL_HIDE)
        // -------------------------------------------------------------------------
        public:
            void Init(HINSTANCE hInstance, HWND hParent) override;

            void Init(HINSTANCE hInstance, HWND hParent, int8_t position) override;

        protected:
            LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) override;

        private:
            // Re-anchors the selection highlight after a playlist change or scroll
            void SyncDirSelectionRectangle();

            // ---- Visibility ---------------------------------------------------------
            void ToggleDirWindow();

            void HideDirWindow();

            // ---- Layout -------------------------------------------------------------
            void MoveDirWindow(); // Cycles position through the preset slots

            // ---- Data ---------------------------------------------------------------
            // Rescans the current folder and rebuilds g_dirThumbnailObjects
            void UpdateDirView();

            // Drops all scaled dir thumbnails from the renderer cache.
            void ClearDirThumbnailCache();
    };

    extern float g_dirOffset;

    // The current set of thumbnail geometry objects (read by RendererD2D)
    extern std::vector<Thumbnail> g_dirThumbnailObjects;
}
