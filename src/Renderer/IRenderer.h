#pragma once

#include <windows.h>
#include <wincodec.h>
#include <string>
#include "../Platform/Constants.h"

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
};
