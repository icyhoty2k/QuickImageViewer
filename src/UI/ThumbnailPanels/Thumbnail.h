// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

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
