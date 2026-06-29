#pragma once

#include <windows.h>
#include <vector>
#include <string>
#include <d2d1.h>

namespace UI {
    // Cache window layout
    extern float g_cacheOffset;

    // Shared logical object representing a thumbnail
    struct Thumbnail {
        D2D1_RECT_F rect; // Shared coordinate space
        std::wstring filePath;
        int playlistIndex;

        // Logic for "Live" hit detection
        bool HitTest(int x, int y) const {
            return (x >= rect.left && x <= rect.right &&
                    y >= rect.top && y <= rect.bottom);
        }
    };


    void InitCacheWindow(HINSTANCE hInstance, HWND hParent);

    void InitCacheWindow(HINSTANCE hInstance, HWND hParent, int8_t position);

    void MoveCacheWindow();

    void ToggleCacheWindow();

    void UpdateCacheView();

    void RenderCacheWindow(int selectedIndex, int hoverIndex);


    // Exposed for the Renderer to use for drawing
    extern std::vector<Thumbnail> g_thumbnailObjects;
}
