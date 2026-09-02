// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#pragma once

#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_set>
#include <dxgi1_2.h>
#include <d2d1_3.h>
#include <dwrite_3.h>
#include <wrl/client.h>

#include "../IPanelWindow.h"
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
            void SetPosition(int8_t pos) { m_position = pos; }

            // True when the panel window exists and is currently shown.
            // Hidden panels are not in any layout slot and therefore miss
            // UIManager::NotifyFolderRefreshed — callers use this to decide
            // whether a panel needs a direct sync instead.
            bool IsPanelVisible() const { return m_hWnd && IsWindowVisible(m_hWnd); }

            void Repaint() { if (m_hWnd) InvalidateRect(m_hWnd, nullptr, FALSE); }

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

            // Multi-select state — path-stable across geometry rebuilds
            std::unordered_set<std::wstring> m_selectedPaths;

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

            // Called by the context menu Delete action.
            // Default: moves the files to the Recycle Bin.
            // CacheWnd overrides to remove only from the VRAM cache.
            virtual void OnContextMenuDelete(const std::vector<std::wstring> &paths);

            // Label for the Delete item in the right-click context menu.
            // CacheWnd overrides to clarify the action is VRAM-only.
            virtual const wchar_t *ContextMenuDeleteLabel() const { return L"Delete"; }

            // Optional extra context menu item appended below Paste.
            // Return nullptr (default) to omit it; CacheWnd returns "Clear VCache".
            virtual const wchar_t *ContextMenuExtraLabel() const { return nullptr; }
            virtual void           OnContextMenuExtra() {}

            // Return false to hide the Paste item entirely (CacheWnd — files can't be pasted there).
            virtual bool ShowContextMenuPaste() const { return true; }

        public:
            // Called by UIManager::NotifyFolderRefreshed on every visible panel.
            // Subclasses decide whether the dir matches and what to update.
            // Default: no-op (HelpWnd, JumpToWnd, etc. ignore this).
            virtual void OnFolderRefreshed(const std::wstring & /*dir*/,
                                           const std::vector<std::wstring> & /*playlist*/) {}

            // Called by UIManager::NotifySortOrderChanged on every panel that
            // holds a list of its own, after app.fileHandlerDefaultSortOrder or
            // app.fileHandlerIsReverseSortOrder changed at runtime. Default:
            // no-op (CacheWnd orders by cache recency, not by the file sort).
            virtual void OnSortOrderChanged() {}

            // Re-read the panel's folder from disk if it matches `dir`.
            // Called after a cut/paste so the strip reflects reality immediately,
            // without waiting for the dir watcher (which only watches one dir).
            virtual void RefreshFromDisk(const std::wstring & /*dir*/) {}

            // Returns the folder path currently displayed by this panel.
            // DirWnd and SpawnedDirWnd override; base returns empty (CacheWnd).
            virtual std::wstring GetPanelFolder() const { return {}; }

            HWND GetOwnerHwnd() const { return m_hOwner; }
            const std::wstring &GetDirSizeStr() const { return m_dirSizeStr; }
            int64_t             GetDirSizeBytes() const { return m_dirSizeBytes; }

            static std::wstring FormatDirSize(int64_t bytes);

        protected:

            bool IsVertical() const;

            // Empty-dir placeholder state — written by subclass OnFolderRefreshed,
            // read by base-class Render() / RenderEmptyPlaceholder().
            std::wstring m_emptyDir;
            bool         m_emptyDirActive  = false;
            bool         m_emptyDirMissing = false; // true when dir was deleted entirely
            // Cached DWrite path layout — reset by subclass when m_emptyDir changes
            // or by ResizeSwapChain when panel dimensions change.
            Microsoft::WRL::ComPtr<IDWriteTextLayout> m_emptyDirPathLayout;

            // Dir label rendered inside the scrollbar strip (z-order below the thumb).
            // Shows the full folder path with the last component + size in bold.
            // Cached; rebuilt when path, panel dimension, or strip thickness changes.
            Microsoft::WRL::ComPtr<IDWriteTextLayout> m_dirLabelLayout;
            std::wstring m_dirLabelTextCache;
            float        m_dirLabelDimCache = 0.0f;
            float        m_dirLabelBarCache = 0.0f;
            std::wstring m_dirSizeStr;   // e.g. "3.56 MB" — set by ComputeDirSize()
            int64_t      m_dirSizeBytes = 0; // raw bytes — set alongside m_dirSizeStr

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
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>  m_emptyDirWarningBrush;
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>  m_multiSelBrush;
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>  m_dirLabelActiveBrush;
            // Cull marks. Two brushes rather than one recoloured on the fly:
            // a strip can show a keep and a reject in the same frame, and
            // SetColor between them would be a state change per thumbnail.
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>  m_cullKeepBrush;
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>  m_cullRejectBrush;
            // Visual effects brushes and geometry (U key master toggle)
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>  m_glowBrush;       // accent-color border for selected thumb
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>  m_cornerBgBrush;   // background color for corner overdraw
            Microsoft::WRL::ComPtr<ID2D1PathGeometry>     m_cornerGeometry;   // 4-corner bite shapes (DPI-dependent)
            float m_cornerGeomDpi = 0.0f;                                      // dpiScale used when m_cornerGeometry was built
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>  m_dirLabelInactiveBrush;

            // Scrollbar LMB drag state
            bool  m_scrollDragging      = false;
            float m_scrollDragStartMouse  = 0.0f; // mouse axis at drag start
            float m_scrollDragStartOffset = 0.0f; // m_offset at drag start

        protected:
            // Moves the strip by one wheel step, wrap-around included.
            //
            // ONE BODY FOR BOTH WHEELS AND EVERY STRIP. It lives on the base
            // because DirWnd, CacheWnd and the spawned strips all inherit this
            // class's WndProc — a fix here reaches all of them, and none of them
            // needs a wheel handler of its own.
            //
            // `horizontal` says which wheel it came from, and it is not cosmetic:
            // the two send opposite signs for the same intent. See the definition.
            void ScrollByWheel(int delta, bool horizontal);

            // Builds the text shown in the scrollbar strip.
            // Returns {fullText, boldStartIndex}. Empty text = no label.
            // CacheWnd overrides to show vRam stats instead of a folder path.
            virtual std::pair<std::wstring, UINT32> BuildScrollbarLabel() const;

            // Utility used by subclass overrides of BuildScrollbarLabel().

        private:
            void ScrollToSelected();
            void RebuildGeometry();
            void RenderEmptyPlaceholder();
            void ComputeDirSize();
            void BuildCornerGeometry(float w, float h, float r);
            void GetWindowBounds(HWND hRef, int8_t position,
                                 int &x, int &y, int &w, int &h) const;

            Microsoft::WRL::ComPtr<IDWriteTextFormat> m_emptyFormat;
            Microsoft::WRL::ComPtr<IDWriteTextLayout> m_emptyNoImagesLayout;
            Microsoft::WRL::ComPtr<IDWriteTextLayout> m_emptyDirMissingLayout;
            Microsoft::WRL::ComPtr<IDWriteTextLayout> m_emptyCacheLayout;

            // Files currently "cut" to the clipboard — shown at reduced opacity.
            // Static so all panel instances see the same cut state.
            static std::unordered_set<std::wstring> s_cutPaths;

            // Path recorded on LMB-down to seed an OLE drag if the mouse moves far enough.
            // Cleared at drag start and on LMB-up. Static: only one drag at a time.
            static std::wstring s_dragSourcePath;

        protected:
            // Update the panel-position overlay in g_overlayManager to reflect the
            // current selection count, then invalidate the main window to repaint.
            // Call at every point where m_selectedPaths changes.
            void NotifySelectionOverlay();

            HWND m_hOwner = nullptr;
            int8_t m_position = 0;
            bool m_sourceDirty = true;
            std::wstring m_anchorPath;       // anchor for Shift+Click range selection
            // Per-panel saved state — restored when the panel becomes active so
            // the selection rectangle starts from the last known position.
            int          m_savedSelectedIdx = -1;
            float        m_savedOffset      = 0.0f;
            std::wstring m_savedImagePath;
            bool         m_justActivated    = false; // set in WM_LBUTTONDOWN, cleared in WM_LBUTTONUP
    };
} // namespace UI
