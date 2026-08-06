// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#include "CacheWnd.h"
#include "../../AppState.h"
#include "../../Input/Shortcuts.h"
#include "../../Renderer/RendererD2D.h"

namespace UI {
    // -------------------------------------------------------------------------
    // Shortcuts
    // -------------------------------------------------------------------------
    int CacheWnd::GetKeyToggle() const {
        return Shortcuts::SC_PANEL_CACHE_TOGGLE;
    }

    int CacheWnd::GetKeyMove() const {
        return Shortcuts::SC_PANEL_CACHE_MOVE;
    }

    int CacheWnd::GetKeyExtra() const {
        return Shortcuts::SC_PANEL_CACHE_CLEAR;
    }

    // -------------------------------------------------------------------------
    // GetSourceItems — returns the VRAM-cached file paths in LRU order
    // -------------------------------------------------------------------------
    std::vector<std::wstring> CacheWnd::GetSourceItems() const {
        auto cached = app.renderer->GetCachedBitmaps();
        std::vector<std::wstring> paths;
        paths.reserve(cached.size());
        for (const auto &item: cached)
            paths.push_back(item.filePath);
        return paths;
    }


    // -------------------------------------------------------------------------
    // ClearThumbnailCache
    // -------------------------------------------------------------------------
    void CacheWnd::ClearThumbnailCache() {
        if (app.renderer)
            app.renderer->ClearCache();
        UpdateView();
    }
} // namespace UI
