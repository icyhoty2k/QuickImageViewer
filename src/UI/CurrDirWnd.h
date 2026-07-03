#pragma once

#include <windows.h>
#include <vector>
#include "Thumbnail.h"

namespace UI {
    class CurrDirWnd {
        // -------------------------------------------------------------------------
        // DirWindow  —  Current-folder image browser panel.
        //
        // Same visual language as CacheWindow (D2D thumbnails, hover/select border,
        // drag-to-scroll, click-to-navigate) but positioned as a centered floating
        // popup instead of an edge strip, and its data comes from the file-system
        // (all images in the current folder) rather than the VRAM bitmap cache.
        //
        // Shortcuts:
        //   F5  —  Toggle panel (SC_PANEL_DIR_TOGGLE)
        //   F6  —  Cycle position: center / top / right / bottom / left (SC_PANEL_DIR_MOVE)
        //   Esc —  Hide panel (SC_LOCAL_HIDE)
        // -------------------------------------------------------------------------


        // Layout scroll offset for the dir panel
        extern float g_dirOffset;

        // The current set of thumbnail geometry objects (read by RendererD2D)
        extern std::vector<Thumbnail> g_dirThumbnailObjects;

        // Re-anchors the selection highlight after a playlist change or scroll
        void SyncDirSelectionRectangle();

        // ---- Lifecycle ----------------------------------------------------------
        void InitDirWindow(HINSTANCE hInstance, HWND hParent);

        // ---- Visibility ---------------------------------------------------------
        void ToggleDirWindow();

        void HideDirWindow();

        // ---- Layout -------------------------------------------------------------
        void MoveDirWindow(); // Cycles position through the preset slots

        // ---- Data ---------------------------------------------------------------
        // Rescans the current folder and rebuilds g_dirThumbnailObjects
        void UpdateDirView();

        // Drops all scaled dir thumbnails from the renderer cache.
        // Call this whenever the active folder changes so stale images are not shown.
        void ClearDirThumbnailCache();
    };
}
