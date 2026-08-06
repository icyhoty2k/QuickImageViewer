// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <d2d1_1.h>
#include <functional>

// Forward declaration to avoid including the full d2d1.h in this header
struct ID2D1Bitmap1;

/// Base interface for image rendering strategies.
/// Implementations must handle bitmap loading, scaling, and painting.
class IImageRenderer {
    public:
        IImageRenderer() = default;

        virtual ~IImageRenderer() = default;

        std::function<void(int)> onImageChangedCallback;

        // Disable copy/move to enforce unique renderer ownership
        IImageRenderer(const IImageRenderer &) = delete;

        IImageRenderer &operator=(const IImageRenderer &) = delete;

        IImageRenderer(IImageRenderer &&) = delete;

        IImageRenderer &operator=(IImageRenderer &&) = delete;


        virtual void ApplyPreviousEffects() = 0;

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
        virtual HRESULT PreloadBitmap(const std::wstring &filePath, int requestIndex, int expectedCurrentIndex = -1) = 0;

        /// support color effects saturation contrast brightness
        virtual void UpdateColorEffects() {}

        /// Update the renderer's background color from the runtime theme factor.
        /// Called by AppCommands::changeAppThemeFactor after app.themeFactor is updated.
        virtual void SetThemeFactor(float /*factor*/) {}

        /// Renders the currently active image with all active color effects
        /// baked in and writes it to disk as a PNG, at native resolution
        /// (no resize, no aspect ratio change). Used by Ctrl+S
        /// (Shortcuts::ImageEffects::SC_COLOR_SAVE_TO_DISK).
        [[nodiscard]]
        virtual HRESULT SaveCurrentImageWithEffects(const std::wstring & /*outPath*/) {
            return E_NOTIMPL;
        }

        /// Clears the currently active image/SVG to show a blank frame during loading
        virtual void ClearActiveImage() {}

        // -------------------------------------------------------------------
        // SVG support
        // -------------------------------------------------------------------

        /// Rasterize an SVG from raw bytes on a worker thread, insert the result
        /// into the shared bitmap cache, then post WM_QIV_REPAINT so the UI thread
        /// displays it through the same cache-hit path as raster images.
        /// Non-blocking: returns S_OK once the work is queued (or on cache hit),
        /// E_NOTIMPL if the renderer has no SVG support.
        [[nodiscard]]
        virtual HRESULT PreloadSvgFromBytes(std::vector<BYTE> /*svgBytes*/,
                                            const std::wstring & /*filePath*/,
                                            int /*requestIndex*/) {
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
            ID2D1Bitmap1 *bitmap;
        };

        virtual std::vector<CacheItem> GetCachedBitmaps() {
            return {};
        }

        // Returns the EXIF orientation tag (274) stored when the bitmap was decoded.
        // 1 = normal (no transform). Returns 1 if the path is not in cache.
        virtual USHORT GetCachedOrientation(const std::wstring & /*filePath*/) {
            return 1;
        }

        virtual void ClearCache() {}
        virtual void ClearCache(const std::wstring & /*excludePath*/) {}
        virtual void RemoveFromCache(const std::wstring & /*filePath*/) {}

        virtual void GetImageCacheStats(int& count, UINT64& estimatedBytes) {
            count = 0; estimatedBytes = 0;
        }
        virtual void GetDirThumbCacheStats(int& count, UINT64& estimatedBytes) {
            count = 0; estimatedBytes = 0;
        }

        virtual const wchar_t* GetName() const { return L"Unknown"; }

        // -------------------------------------------------------------------
        // Animated GIF support
        // -------------------------------------------------------------------
        virtual bool IsAnimatedGif() const { return false; }
        // Delay (ms) for the current frame — call before arming the timer.
        virtual int  GetCurrentGifDelay() const { return 100; }
        // Advances to next frame; returns the new current frame's delay.
        virtual int  AdvanceGifFrame() { return 0; }
        virtual void ResetGifAnimation() {}

        // Has the decoder already tried this file and given up? Lets the UI
        // stop waiting for a decode that failed and say so instead — a file
        // whose extension promises an image the data is not (a .txt renamed to
        // .jpg) or a supported format whose bytes are damaged.
        //
        // Defaults to false: a renderer that does not track this simply never
        // reports a failure, which is the behaviour every caller had before.
        virtual bool DecodeFailed(const std::wstring & /*path*/) const { return false; }
};
