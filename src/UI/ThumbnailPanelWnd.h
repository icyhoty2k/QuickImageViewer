#pragma once

#include <windows.h>
#include <cstdint>
#include <vector>

#include "IPanelWindow.h"
#include "Thumbnail.h"

namespace UI {
    // =========================================================================
    // ThumbnailPanelWnd
    //
    // Shared base for any panel that displays a scrollable filmstrip of
    // thumbnails (CacheWnd, DirWnd).  Handles all common Win32 message
    // routing, scroll/drag logic, hover/selection hit-testing, and geometry
    // layout.  Subclasses implement the handful of virtual hooks that differ
    // between panels (data source, renderer calls, shortcuts, window bounds).
    // =========================================================================
    class ThumbnailPanelWnd : public IPanelWindow {
        // ---------------------------------------------------------------------
        // Public API (mirrors what callers used directly on CacheWnd / DirWnd)
        // ---------------------------------------------------------------------
        public:
            // Init with default position
            void Init(HINSTANCE hInstance, HWND hParent) override;

            // Init with an explicit position slot
            void Init(HINSTANCE hInstance, HWND hParent, int8_t position) override;

            // Re-anchor the selection highlight to app.currentIndex
            void SyncSelectionRectangle();

            // Rebuild thumbnail geometry and request async decodes where needed
            void UpdateView();

            // Show/hide/move cycle
            void Toggle() override;

            void Hide() override;

            void MovePanel();

            // ---------------------------------------------------------------------
            // Shared state — exposed so the renderer can read the vectors directly
            // (mirrors old g_thumbnailObjects / g_cacheOffset / g_dirOffset globals)
            // ---------------------------------------------------------------------
        public:
            float m_offset = 0.0f;
            int m_selectedIdx = -1;
            int m_hoverIdx = -1;
            std::vector<Thumbnail> m_thumbnails;

            // ---------------------------------------------------------------------
            // Message routing
            // ---------------------------------------------------------------------
        protected:
            LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) override;

            // ---------------------------------------------------------------------
            // Hooks — implemented by each concrete subclass
            // ---------------------------------------------------------------------
        protected:
            // Window class name (must be unique per subclass)
            virtual const wchar_t *ClassName() const = 0;

            // Window title
            virtual const wchar_t *WindowTitle() const = 0;

            // Shortcuts this panel owns
            virtual int GetKeyToggle() const = 0;

            virtual int GetKeyMove() const = 0;

            // Optional extra key (e.g. SC_PANEL_CACHE_CLEAR); return -1 if unused
            virtual int GetKeyExtra() const {
                return -1;
            }

            virtual void OnExtraKey() {}

            // True when the current position slot is a vertical strip (affects drag axis).
            // 5 shared slots:  0=center-float  1=top  2=right  3=bottom  4=left
            // Vertical = right(2) or left(4).  Override not needed by subclasses.
            bool IsVertical() const;

            // Paint — calls the correct renderer method
            virtual void Render(int selectedIdx, int hoverIdx) = 0;

            // Resize the panel's D2D swap chain.
            // One line per subclass; kept virtual because each calls a different
            // renderer method (ResizeCacheWindow vs ResizeDirWindow).
            virtual void ResizeSwapChain(UINT w, UINT h) = 0;

            // Create per-panel D2D device resources (called once in Init)
            virtual void CreateDeviceResources() = 0;

            // Returns the ordered list of file paths this panel displays.
            // Base class calls this inside UpdateView() and handles all layout math.
            virtual std::vector<std::wstring> GetSourceItems() const = 0;

            // Called after thumbnails are built (e.g. DirWnd queues async decodes)
            virtual void PostBuildHook() {}

            // ---------------------------------------------------------------------
            // Internal helpers
            // ---------------------------------------------------------------------
        private:
            void ScrollToSelected();

            // Shared 5-slot geometry (same for both CacheWnd and DirWnd):
            //   0 = center floating  (80% wide, thumb-height tall)
            //   1 = top edge strip
            //   2 = right edge strip
            //   3 = bottom edge strip
            //   4 = left edge strip
            void GetWindowBounds(HWND hRef, int8_t position,
                                 int &x, int &y, int &w, int &h) const;

        protected:
            HWND m_hOwner = nullptr;
            int8_t m_position = 0;
    };
} // namespace UI
