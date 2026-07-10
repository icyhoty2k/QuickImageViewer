#include "DirWnd.h"
#include "../AppState.h"
#include "../Input/Shortcuts.h"
#include "../Renderer/RendererD2D.h"

namespace UI {
    // -------------------------------------------------------------------------
    // Shortcuts
    // -------------------------------------------------------------------------
    int DirWnd::GetKeyToggle() const {
        return Shortcuts::SC_PANEL_DIR_TOGGLE;
    }

    int DirWnd::GetKeyMove() const {
        return Shortcuts::SC_PANEL_DIR_MOVE;
    }

    // -------------------------------------------------------------------------
    // GetSourceItems — returns the current folder's image playlist
    // -------------------------------------------------------------------------
    std::vector<std::wstring> DirWnd::GetSourceItems() const {
        return app.playlist;
    }

    // -------------------------------------------------------------------------
    // PostBuildHook — queue async decodes for RendererD2D
    // -------------------------------------------------------------------------
    void DirWnd::PostBuildHook() {
        auto *r = dynamic_cast<RendererD2D *>(app.renderer.get());
        if (r) {
            for (const auto &t: m_thumbnails) {
                r->RequestDirThumbnail(t.filePath);
            }
        }
    }

    // -------------------------------------------------------------------------
    // ClearDirThumbnailCache
    // -------------------------------------------------------------------------
    void DirWnd::ClearDirThumbnailCache() {
        if (!app.renderer) return;
        auto *r = dynamic_cast<RendererD2D *>(app.renderer.get());
        if (r) {
            r->ClearDirThumbnailCache();
        }
    }
} // namespace UI
