#pragma once

#include <string>
#include <d2d1.h>

namespace UI {
    // Shared logical object representing a thumbnail for any panel
    struct Thumbnail {
        D2D1_RECT_F rect;
        std::wstring filePath;
        int playlistIndex;

        // Logic for "Live" hit detection
        bool HitTest(int x, int y) const {
            return (x >= rect.left && x <= rect.right &&
                    y >= rect.top && y <= rect.bottom);
        }
    };
}
