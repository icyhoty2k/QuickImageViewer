#include "DirWnd.h"
#include "../AppState.h"
#include "../Platform/Constants.h"
#include "../Input/Shortcuts.h"
#include "../Renderer/RendererD2D.h"

namespace UI {
    // Legacy globals kept so RendererD2D doesn't need changes
    float g_dirOffset = 0.0f;
    std::vector<Thumbnail> g_dirThumbnailObjects;

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
    // PostBuildHook — queue async decodes + sync legacy globals for RendererD2D
    // -------------------------------------------------------------------------
    void DirWnd::PostBuildHook() {
        g_dirOffset = m_offset;
        g_dirThumbnailObjects = m_thumbnails;

        auto *r = dynamic_cast<RendererD2D *>(app.renderer.get());
        if (r) {
            for (const auto &t: m_thumbnails)
                r->RequestDirThumbnail(t.filePath);
        }
    }

    // -------------------------------------------------------------------------
    // Renderer calls
    // -------------------------------------------------------------------------
    void DirWnd::Render(int selectedIdx, int hoverIdx) {
        if (!app.renderer) return;
        auto *r = dynamic_cast<RendererD2D *>(app.renderer.get());
        if (r && r->GetDirContext())
            r->RenderDirWindow(selectedIdx, hoverIdx);
    }

    void DirWnd::ResizeSwapChain(UINT w, UINT h) {
        if (!app.renderer) return;
        auto *r = dynamic_cast<RendererD2D *>(app.renderer.get());
        if (r) r->ResizeDirWindow(w, h);
    }

    void DirWnd::CreateDeviceResources() {
        if (!app.renderer) return;
        auto *r = dynamic_cast<RendererD2D *>(app.renderer.get());
        if (r) r->CreateDirWindowDeviceResources(m_hWnd);
    }

    // -------------------------------------------------------------------------
    // ClearDirThumbnailCache
    // -------------------------------------------------------------------------
    void DirWnd::ClearDirThumbnailCache() {
        if (!app.renderer) return;
        auto *r = dynamic_cast<RendererD2D *>(app.renderer.get());
        if (r) r->ClearDirThumbnailCache();
    }
} // namespace UI
