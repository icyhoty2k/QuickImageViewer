#include "CacheWnd.h"
#include "../AppState.h"
#include "../Input/Shortcuts.h"
#include "../Renderer/RendererD2D.h"

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
