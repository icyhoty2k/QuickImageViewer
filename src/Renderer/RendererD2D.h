// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#pragma once

#include "IRenderer.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d2d1_3.h>
#include <d2d1svg.h>
#include <dwrite_3.h>
#include <wrl/client.h>
#include <list>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <mutex>
#include <vector>

#include "../UI/ThumbnailPanels/Thumbnail.h"
#include "WorkerThread.h"
#include "Platform/ConstantsTheme.h"

class RendererD2D final : public IImageRenderer {
    public:
        int g_hoverIndex = -1;

        RendererD2D() = default;

        ~RendererD2D() override = default;

        const wchar_t* GetName() const override { return L"Direct2D"; }
        [[nodiscard]] HRESULT Initialize(HWND hwnd) override;

        void Resize(UINT width, UINT height) override;

        [[nodiscard]] HRESULT LoadBitmap(IWICBitmapSource *bitmap, UINT width, UINT height, const std::wstring &filePath) override;

        [[nodiscard]] HRESULT PreloadBitmap(const std::wstring &filePath, int requestIndex, int expectedCurrentIndex = -1) override;

        [[nodiscard]] HRESULT Render() override;

        void UpdateColorEffects() override;

        [[nodiscard]] HRESULT SaveCurrentImageWithEffects(const std::wstring &outPath) override;

        void ClearActiveImage() override;

        // SVG support
        [[nodiscard]] HRESULT PreloadSvgFromBytes(std::vector<BYTE> svgBytes,
                                                  const std::wstring &filePath,
                                                  int requestIndex) override;

        bool HasActiveSvg() const override {
            return m_pActiveSvg != nullptr;
        }

        // Cache Management
        std::vector<CacheItem> GetCachedBitmaps() override;
        USHORT GetCachedOrientation(const std::wstring &filePath) override;

        void ClearCache() override;

        void ClearCache(const std::wstring &excludePath) override;

        void RemoveFromCache(const std::wstring &filePath) override;

        void GetImageCacheStats(int &count, UINT64 &estimatedBytes) override;
        void GetDirThumbCacheStats(int &count, UINT64 &estimatedBytes) override;

        // =====================================================================
        // Thumbnail Panel APIs
        // Each ThumbnailPanelWnd owns its own swap chain — these two accessors
        // let panels borrow the shared D3D/D2D device for one-time init.
        // =====================================================================
        ID3D11Device *GetD3DDevice() const { return m_pD3DDevice.Get(); }
        ID2D1Device6 *GetD2DDevice() const { return m_pD2DDevice.Get(); }

        // Resolve bitmap pointers for a set of thumbnails under the minimum lock
        // duration. Called by ThumbnailPanelWnd::Render() before GPU work begins.
        // hPanel = panel HWND → look in that panel's private dir-thumb cache (DirWnd)
        // hPanel = nullptr   → look in m_bitmapCache only (CacheWnd)
        struct ResolvedThumb {
            Microsoft::WRL::ComPtr<ID2D1Bitmap1> bitmap; // nullptr = placeholder
            D2D1_RECT_F rect;
        };
        void ResolveThumbnailBitmaps(const std::vector<UI::Thumbnail> &thumbnails,
                                     HWND hPanel,
                                     std::vector<ResolvedThumb> &out);

        // Queues an async decode+scale job for one file.
        // Posts WM_QIV_REPAINT to hPanel when the thumbnail lands.
        void RequestDirThumbnail(const std::wstring &filePath, HWND hPanel);

        // Drops the thumbnail cache that belongs to hPanel only.
        // Other panels are completely unaffected.
        void ClearDirThumbnailCache(HWND hPanel);

        void ApplyPreviousEffects() override;

        // Public DWrite resources
        Microsoft::WRL::ComPtr<IDWriteFactory3> m_pDWriteFactory;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> m_pTextFormat;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_pTextBrush;

    private:
        // Device-independent resources
        Microsoft::WRL::ComPtr<ID2D1Factory7> m_pD2DFactory;
        // --- The Master Bypass Switch (Renderer-side) ---
        // This acts as the final "Display Node" that Render() draws.
        // It points to EITHER m_pBitmap (bypass) or the effect output (active).
        Microsoft::WRL::ComPtr<ID2D1Image> m_pActiveDisplayNode;
        // Device-dependent resources
        Microsoft::WRL::ComPtr<ID3D11Device> m_pD3DDevice;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_pD3DContext;
        Microsoft::WRL::ComPtr<IDXGISwapChain1> m_pSwapChain;
        Microsoft::WRL::ComPtr<ID2D1Device6> m_pD2DDevice;
        Microsoft::WRL::ComPtr<ID2D1DeviceContext7> m_pDeviceContext;

        // =====================================================================
        // NOTE: No shared decode/thumbnail DeviceContexts here.
        // ID2D1Device::CreateDeviceContext() is thread-safe; each worker task
        // creates its own short-lived DeviceContext from m_pD2DDevice, uses it,
        // and releases it. This eliminates all data races on shared D2D state.

        // ID2D1DeviceContext5 is queried once from m_pDeviceContext and cached
        // here to avoid a per-frame QueryInterface in the SVG render path.
        // Reset in DiscardDeviceResources(), repopulated in CreateDeviceResources().
        Microsoft::WRL::ComPtr<ID2D1DeviceContext5> m_pDeviceContext5;

        // Continuous adjustments only: saturation + contrast + brightness folded
        // into one 5x4 color matrix computed explicitly in UpdateColorEffects().
        // This node is ALWAYS first in the chain (it is a per-image correction,
        // not a stackable toggle).
        Microsoft::WRL::ComPtr<ID2D1Effect> m_pColorMatrixEffect;
        // Toggle effects. Each is its own node so BuildEffectChain() can place it
        // anywhere in the chain — the position comes from app.activeEffectsList,
        // i.e. the order the user switched them on.
        // Grayscale/invert/sepia used to be folded into m_pColorMatrixEffect,
        // which pinned them BEFORE every non-linear node — and since the
        // solarize LUT is invariant under inversion (solarize(1-v) == solarize(v)),
        // toggling Invert after Solarize was a visual no-op. Keep them separate.
        Microsoft::WRL::ComPtr<ID2D1Effect> m_pGrayscaleEffect; // CLSID_D2D1ColorMatrix
        Microsoft::WRL::ComPtr<ID2D1Effect> m_pInvertEffect; // CLSID_D2D1ColorMatrix
        Microsoft::WRL::ComPtr<ID2D1Effect> m_pSepiaEffect; // CLSID_D2D1ColorMatrix
        Microsoft::WRL::ComPtr<ID2D1Effect> m_pGammaEffect; // CLSID_D2D1GammaTransfer
        Microsoft::WRL::ComPtr<ID2D1Effect> m_pSolarizeEffect; // CLSID_D2D1TableTransfer
        // Threshold is two nodes: luminance collapse, then the 0/1 table.
        Microsoft::WRL::ComPtr<ID2D1Effect> m_pThresholdLumaEffect; // CLSID_D2D1ColorMatrix
        Microsoft::WRL::ComPtr<ID2D1Effect> m_pThresholdEffect; // CLSID_D2D1TableTransfer
        Microsoft::WRL::ComPtr<ID2D1Effect> m_pOutlineEffect; // CLSID_D2D1EdgeDetection
        Microsoft::WRL::ComPtr<ID2D1Effect> m_pScaleEffect;
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> m_pBackBufferBitmap;

        // Per-panel dir-thumbnail cache.
        // Each DirWnd / SpawnedDirWnd owns its own entry keyed by its HWND,
        // so clearing one panel's cache never evicts thumbnails owned by another.
        //
        // BOUNDED BY BYTES, not by count — see EvictPanelThumbs.
        //
        // This cache had no eviction at all: thumbnails were inserted and only
        // ever dropped when the whole panel changed folder. Scrolling one panel
        // through a large folder therefore grew without limit — at 200% DPI a
        // thumbnail is 256x160x4, about 160 KB, so a 10,000-image folder is
        // roughly 1.6 GB of GPU memory, times up to five panels.
        //
        // app.dirThumbCacheMB existed the whole time — persisted, in the tray
        // menu, adjustable from 100 to 64000 MB, shown in the Stats panel — and
        // was read by nothing. Lowering it changed nothing. It is the budget now.
        //
        // Byte-exact rather than estimated: the size is known at creation from
        // the dimensions the thumbnail was actually made at, so DPI changes and
        // the odd non-standard thumbnail are accounted correctly.
        struct PanelThumbEntry {
            std::unordered_map<std::wstring, Microsoft::WRL::ComPtr<ID2D1Bitmap1>> bitmaps;
            std::unordered_set<std::wstring> inFlight;

            // Position of each of this panel's thumbnails in the GLOBAL lru list
            // below, plus what each one costs. The ordering itself is not kept
            // here — see m_thumbLru for why.
            std::unordered_map<std::wstring, std::list<std::pair<HWND, std::wstring>>::iterator> lruPos;
            std::unordered_map<std::wstring, size_t> bytes;
            size_t totalBytes = 0; // this panel's share, for reporting
        };
        std::unordered_map<HWND, PanelThumbEntry> m_panelThumbCaches;
        std::mutex m_dirThumbMutex;

        // ONE LRU ACROSS ALL PANELS, most-recently-drawn at the FRONT.
        //
        // The budget is a single number the user sets, so it has to be enforced
        // against a single total. The obvious alternative — give each panel
        // budget/N — is wrong twice over: hiding a panel does not clear its
        // cache, so an unwatched strip would still reserve its share; and panels
        // open and close constantly, so every change would evict everyone else
        // down and then hand the space back, churning for nothing.
        //
        // A shared list needs no divisor and no panel count. Thumbnails are
        // promoted when they are DRAWN, so a panel nobody is looking at simply
        // stops being touched and drifts to the back, where it is evicted first.
        // The strips in front of the user keep their thumbnails because they are
        // the ones being used. That is the behaviour a shared budget should have,
        // and it falls out of the ordering instead of being computed.
        std::list<std::pair<HWND, std::wstring>> m_thumbLru;
        size_t m_thumbTotalBytes = 0;

        // Drops least-recently-used thumbnails until EVERY panel is inside its
        // share of app.dirThumbCacheMB. The budget is shared between the five
        // strips, not granted to each — see the definition.
        // Caller must already hold m_dirThumbMutex.
        void EnforceThumbBudget();

        // Marks a thumbnail as just-used. Caller must hold m_dirThumbMutex.
        void TouchPanelThumb(PanelThumbEntry &entry, const std::wstring &path);

        // Cache
        struct CachedBitmap {
            Microsoft::WRL::ComPtr<ID2D1Bitmap1> bitmap;
            std::list<std::wstring>::iterator lruIt;
            UINT  width       = 0;
            UINT  height      = 0;
            USHORT orientation = 1; // EXIF tag 274: 1=normal, 3=180°, 6=90°CW, 8=270°CW, etc.
            // Animated GIF: all composited frames + per-frame delays in ms.
            // Empty for non-animated images.
            std::vector<Microsoft::WRL::ComPtr<ID2D1Bitmap1>> gifFrames;
            std::vector<int> gifDelays;
        };

        Microsoft::WRL::ComPtr<ID2D1Bitmap1> m_pBitmap;
        std::unordered_map<std::wstring, CachedBitmap> m_bitmapCache;
        std::list<std::wstring> m_lruList;
        std::unordered_set<std::wstring> m_bitmapInFlight;
        std::mutex m_cacheMutex;

        // Animated GIF runtime state (active image only)
        std::vector<Microsoft::WRL::ComPtr<ID2D1Bitmap1>> m_gifFrames;
        std::vector<int> m_gifDelays;
        int m_gifFrame = 0;

    public:
        bool IsAnimatedGif()        const override { return !m_gifFrames.empty(); }
        int  GetCurrentGifDelay()   const override;
        int  AdvanceGifFrame()            override;
        void ResetGifAnimation()          override;
    private:

        // Persistent overlay (Missing / Empty) — drawn until user opens a new folder.
        // Format + layout are DWrite (device-independent): created once, survive
        // device loss and resize — only the brush is device-dependent.
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_pFolderDeletedBrush;   // red — Missing state
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_pLinkBrush;            // Constants::Links — clickable path
        Microsoft::WRL::ComPtr<IDWriteTextFormat>     m_pFolderOverlayFormat;  // created lazily, cached forever
        Microsoft::WRL::ComPtr<IDWriteTextLayout>     m_pFolderDeletedLayout;
        std::wstring                                  m_lastFolderOverlayKey;  // state + path, drives lazy rebuild

        // SVG: active D2D SVG document (legacy path, never set by resvg;
        // resvg-rasterized SVGs go into m_bitmapCache as bitmaps instead)
        Microsoft::WRL::ComPtr<ID2D1SvgDocument> m_pActiveSvg;
        float m_svgNativeW = 0.0f;
        float m_svgNativeH = 0.0f;

        // Worker-safe SVG rasterization: parse + render + GPU upload only.
        // Writes no shared state; the caller inserts outBmp into m_bitmapCache.
        // Uses the injected per-thread WIC factory (wicFac) — never app.wicFactory.
        [[nodiscard]] HRESULT DecodeSvgToBitmap(const std::vector<BYTE> &svgBytes,
                                                IWICImagingFactory2 *wicFac,
                                                Microsoft::WRL::ComPtr<ID2D1Bitmap1> &outBmp,
                                                UINT &outW, UINT &outH);

        // Window state
        HWND m_hwnd = nullptr;
        D2D1_COLOR_F m_clearColor = D2D1::ColorF(
                Constants::Theme::Background::MAIN_WINDOW,
                Constants::Theme::Background::MAIN_WINDOW,
                Constants::Theme::Background::MAIN_WINDOW);

        void SetThemeFactor(float factor) override {
            const float v = Constants::Theme::Apply(Constants::Theme::Background::MAIN_WINDOW, factor);
            m_clearColor = D2D1::ColorF(v, v, v);
        }

        // Internal helpers
        HRESULT CreateDeviceResources();

        void DiscardDeviceResources();

        HRESULT CreateBackBufferBitmap();

        // Creates the toggle effect nodes (grayscale/invert/sepia/gamma/
        // solarize/threshold/outline) lazily on first use, since most images
        // never touch them.
        HRESULT EnsureExtraEffects();

        // Wires app's saturation/contrast/brightness into m_pColorMatrixEffect
        // (always first), then gamma, then chains every toggled-on effect in
        // app.activeEffectsList order — the order the user switched them on, so
        // each effect operates on the result of the ones before it.
        // Returns the final ID2D1Image* ready to be scaled/drawn or captured.
        ID2D1Effect *BuildEffectChain(ID2D1Image *source);

        // Appends one effect's node(s) to `current` and returns the new tail.
        ID2D1Effect *ChainEffectByName(const std::wstring &name, ID2D1Effect *current);

        // Clears input 0 on every effect node so a bypassed graph holds no
        // reference to the source bitmap.
        void ReleaseEffectInputs();
};

// Declare the globals so all files see them
extern DecoderThreadPool g_decoderWorker;
extern IoThreadPool g_ioWorker;
extern DecoderThreadPool g_dirThumbWorker;
