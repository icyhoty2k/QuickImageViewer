#pragma once

#include <windows.h>
#include <vector>
#include "Thumbnail.h"


namespace UI {
    // Cache window layout
    extern float g_cacheOffset;

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
    extern std::vector<Thumbnail> g_thumbnailObjects;
}
