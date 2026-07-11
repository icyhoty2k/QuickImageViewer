#pragma once

#include <windows.h>
#include <cstdint>
#include <vector>
#include <dxgi1_2.h>
#include <d2d1_3.h>
#include <wrl/client.h>

#include "IPanelWindow.h"
#include "Thumbnail.h"

// Forward-declare so we can borrow devices without pulling in the full header
// in most translation units. ThumbnailPanelWnd.cpp includes RendererD2D.h.
class RendererD2D;

namespace UI {
    // =========================================================================
    // ThumbnailPanelWnd
    //
    // Shared base for CacheWnd, DirWnd, and SpawnedDirWnd.
    // Each instance owns its own DXGI swap chain + D2D device context so that
    // any number of panels can be alive simultaneously without interfering with
    // each other or with the main viewer renderer.
    //
    // The shared RendererD2D is used for two things only:
    //   1. One-time borrow of ID3D11Device + ID2D1Device6 during Init to create
    //      this panel's swap chain (CreateDeviceResources).
    //   2. Shared thumbnail/bitmap cache lookups at render time
    //      (m_dirThumbCache / m_bitmapCache stay in RendererD2D).
    // =========================================================================
    class ThumbnailPanelWnd : public IPanelWindow {
        public:
            void Init(HINSTANCE hInstance, HWND hParent) override;
            void Init(HINSTANCE hInstance, HWND hParent, int8_t position) override;

            void SyncSelectionRectangle();
            void UpdateView();

            // Compat wrappers — named to match existing call sites in AppMain/FileHandler.
            // All DirWnd and CacheWnd subclasses inherit these.
            void SyncDirSelectionRectangle() { SyncSelectionRectangle(); }
            void UpdateDirView()             { UpdateView(); }
            void ClearDirThumbnailCache(); // implemented in DirWnd; no-op in CacheWnd

            // Virtual so DirWnd can override with the actual renderer cache clear.
            virtual void DoClearDirThumbnailCache() {}

            void Toggle() override;
            void Show() override;
            void Hide() override;
            void MovePanel();

            // Returns the current position slot (0=center,1=top,2=right,3=bottom,4=left)
            int8_t GetPosition() const { return m_position; }

            // Recompute and apply window bounds for the current position slot.
            // Called by UIManager::RefreshVerticalPanels when a neighbouring
            // horizontal panel is shown or hidden.
            void RefreshBounds();

        public:
            // Shared geometry state — read by Render()
            float m_offset = 0.0f;
            int m_selectedIdx = -1;
            int m_hoverIdx = -1;
            std::vector<Thumbnail> m_thumbnails;

        protected:
            LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) override;

            // ------------------------------------------------------------------
            // Hooks — concrete subclasses implement these
            // ------------------------------------------------------------------
            virtual const wchar_t *ClassName() const = 0;
            virtual const wchar_t *WindowTitle() const = 0;
            virtual int GetKeyToggle() const = 0;
            virtual int GetKeyMove() const = 0;
            virtual int GetKeyExtra() const { return -1; }
            virtual void OnExtraKey() {}

            // Returns the ordered list of file paths this panel displays.
            virtual std::vector<std::wstring> GetSourceItems() const = 0;

            // True for DirWnd and SpawnedDirWnd — these track the active panel.
            virtual bool IsDirPanel() const { return false; }
            virtual bool HasOwnPlaylist() const { return false; }

            // True when this panel should look up bitmaps from m_dirThumbCache
            // (DirWnd + SpawnedDirWnd). False = CacheWnd uses m_bitmapCache.
            virtual bool UsesDirThumbCache() const { return false; }

            // Called after thumbnails are built (DirWnd queues async decodes).
            virtual void PostBuildHook() {}

            bool IsVertical() const;

            // ------------------------------------------------------------------
            // Per-panel D3D/D2D resources — owned here, not in RendererD2D
            // ------------------------------------------------------------------
            void CreateDeviceResources();
            void ResizeSwapChain(UINT w, UINT h);
            void Render(int selectedIdx, int hoverIdx);

            // Per-panel swap chain and D2D context (one per ThumbnailPanelWnd instance)
            Microsoft::WRL::ComPtr<IDXGISwapChain1>       m_swapChain;
            Microsoft::WRL::ComPtr<ID2D1DeviceContext7>   m_panelContext;
            Microsoft::WRL::ComPtr<ID2D1Bitmap1>          m_panelBackBuffer;
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>  m_placeholderBrush;
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>  m_borderBrush;
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>  m_hoverBrush;
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>  m_scrollTrackBrush;
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>  m_scrollThumbBrush;

            // Scrollbar LMB drag state
            bool  m_scrollDragging      = false;
            float m_scrollDragStartMouse  = 0.0f; // mouse axis at drag start
            float m_scrollDragStartOffset = 0.0f; // m_offset at drag start

        private:
            void ScrollToSelected();
            void RebuildGeometry();
            void GetWindowBounds(HWND hRef, int8_t position,
                                 int &x, int &y, int &w, int &h) const;

        protected:
            HWND m_hOwner = nullptr;
            int8_t m_position = 0;
    };
} // namespace UI
