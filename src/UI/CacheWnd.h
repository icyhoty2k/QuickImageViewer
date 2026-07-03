#pragma once

#include <windows.h>
#include <vector>

#include "IPanelWindow.h"
#include "Thumbnail.h"


namespace UI {
    class CacheWnd : public IPanelWindow {
        // Cache window layout

        void SyncSelectionRectangle();

        // Shared logical object representing a thumbnail

        void InitCacheWindow(HINSTANCE hInstance, HWND hParent, int8_t position);

        void MoveCacheWindow();

        void ToggleCacheWindow();

        void UpdateCacheView();

        void RenderCacheWindow(int selectedIndex, int hoverIndex);

        // Clears the cache
        void ClearThumbnailCache();

        // Exposed for the Renderer to use for drawing
    };

    extern float g_cacheOffset;

    extern std::vector<Thumbnail> g_thumbnailObjects;
}
