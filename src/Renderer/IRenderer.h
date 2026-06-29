#pragma once

#include <windows.h>
#include <wincodec.h>
#include <string>
#include <vector>
#include <d2d1_1.h>

// Forward declaration to avoid including the full d2d1.h in this header
struct ID2D1Bitmap1;

/// Base interface for image rendering strategies.
/// Implementations must handle bitmap loading, scaling, and painting.
class IImageRenderer {
    public:
        IImageRenderer() = default;

        virtual ~IImageRenderer() = default;

        // Disable copy/move to enforce unique renderer ownership
        IImageRenderer(const IImageRenderer &) = delete;

        IImageRenderer &operator=(const IImageRenderer &) = delete;

        IImageRenderer(IImageRenderer &&) = delete;

        IImageRenderer &operator=(IImageRenderer &&) = delete;

        virtual void UpdateTextFormat() = 0;

        virtual void ProcessPendingUploads() {}

        /// Initialize the renderer resources for the specified window handle.
        [[nodiscard]]
        virtual HRESULT Initialize(HWND hwnd) = 0;

        /// Update internal buffers when the client area changes dimensions.
        virtual void Resize(UINT width, UINT height) = 0;

        /// Loads a decoded WIC bitmap into the renderer's pipeline.
        /// If bitmap is nullptr, performs a cache-only lookup: returns S_OK on hit, E_FAIL on miss.
        /// @param bitmap Source WIC bitmap for processing. May be nullptr for cache probe.
        /// @param width Width of the source image (ignored on cache hit or nullptr probe).
        /// @param height Height of the source image (ignored on cache hit or nullptr probe).
        /// @param filePath The absolute path used for identifying cached GPU resources.
        [[nodiscard]]
        virtual HRESULT LoadBitmap(
                IWICBitmapSource *bitmap,
                UINT width,
                UINT height,
                const std::wstring &filePath) = 0;

        /// Paints the active bitmap to the target surface.
        [[nodiscard]]
        virtual HRESULT Render() = 0;

        /// support background preloading
        [[nodiscard]]
        virtual HRESULT PreloadBitmap(const std::wstring &filePath, int requestIndex) = 0;

        /// support color effects saturation contrast brightness
        virtual void UpdateColorEffects() {}

        /// Clears the currently active image/SVG to show a blank frame during loading
        virtual void ClearActiveImage() {}

        // -------------------------------------------------------------------
        // SVG support
        // -------------------------------------------------------------------

        /// Load an SVG from raw bytes into the renderer's SVG cache.
        /// Returns S_OK on success, E_NOTIMPL if the renderer has no SVG support.
        [[nodiscard]]
        virtual HRESULT LoadSvgFromBytes(const std::vector<BYTE> &svgBytes,
                                         const std::wstring &filePath) {
            (void) svgBytes;
            (void) filePath;
            return E_NOTIMPL;
        }

        /// Returns true when the currently active "image" is an SVG document.
        virtual bool HasActiveSvg() const {
            return false;
        }

        // -------------------------------------------------------------------
        // Cache Management
        // -------------------------------------------------------------------
        struct CacheItem {
            std::wstring filePath;
            ID2D1Bitmap1* bitmap;
        };

        virtual std::vector<CacheItem> GetCachedBitmaps() { return {}; }
        virtual void ClearCache() {}
        virtual void RemoveFromCache(const std::wstring& filePath) {}
};
