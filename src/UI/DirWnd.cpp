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
    // GetSourceItems — F5's own isolated playlist, auto-populated on first use
    // -------------------------------------------------------------------------
    std::vector<std::wstring> DirWnd::GetSourceItems() const {
        // On first use, copy app.playlist and lock it in place (protect from spawned hijacking)
        if (m_dirPlaylist.empty() && !app.playlist.empty()) {
            const_cast<DirWnd *>(this)->m_dirPlaylist = app.playlist;
        }
        return m_dirPlaylist;
    }

    // -------------------------------------------------------------------------
    // LoadPlaylist — populate F5's playlist from a folder
    // -------------------------------------------------------------------------
    void DirWnd::LoadPlaylist(const std::wstring &folderPath) {
        m_dirPlaylist.clear();
        m_currentFolder = folderPath;  // Track current folder for history marking
        std::filesystem::path dir(folderPath);
        if (!std::filesystem::exists(dir) || !std::filesystem::is_directory(dir))
            return;
        for (const auto &entry : std::filesystem::directory_iterator(dir)) {
            if (!entry.is_regular_file()) continue;
            if (!is_image_ext(entry.path().extension().wstring())) continue;
            m_dirPlaylist.push_back(std::filesystem::canonical(entry.path()).wstring());
        }
    }

    // -------------------------------------------------------------------------
    // PostBuildHook — queue async decodes for RendererD2D
    // -------------------------------------------------------------------------
    void DirWnd::PostBuildHook() {
        auto *r = dynamic_cast<RendererD2D *>(app.renderer.get());
        if (r) {
            for (const auto &t: m_thumbnails) {
                r->RequestDirThumbnail(t.filePath, m_hWnd);
            }
        }
    }

    // -------------------------------------------------------------------------
    // DoClearDirThumbnailCache
    // -------------------------------------------------------------------------
    void DirWnd::DoClearDirThumbnailCache() {
        if (!app.renderer) return;
        auto *r = dynamic_cast<RendererD2D *>(app.renderer.get());
        if (r) {
            r->ClearDirThumbnailCache(m_hWnd);
        }
    }

} // namespace UI
