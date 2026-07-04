#include "CacheWnd.h"
#include "../AppState.h"
#include "../Platform/Constants.h"
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
        std::wstring activeFile;
        if (!app.playlist.empty() && app.currentIndex >= 0 &&
            app.currentIndex < static_cast<int>(app.playlist.size())) {
            activeFile = app.playlist[app.currentIndex];
        }

        if (app.renderer) {
            app.renderer->ClearCache(activeFile);
        }
        UpdateView();
    }
} // namespace UI
