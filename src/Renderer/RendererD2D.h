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

#include "Thumbnail.h"
#include "WorkerThread.h"

class RendererD2D final : public IImageRenderer {
    public:
        int g_hoverIndex = -1;

        RendererD2D() = default;

        ~RendererD2D() override = default;

        [[nodiscard]] HRESULT Initialize(HWND hwnd) override;

        void Resize(UINT width, UINT height) override;

        [[nodiscard]] HRESULT LoadBitmap(IWICBitmapSource *bitmap, UINT width, UINT height, const std::wstring &filePath) override;

        [[nodiscard]] HRESULT PreloadBitmap(const std::wstring &filePath, int requestIndex) override;

        [[nodiscard]] HRESULT Render() override;

        void UpdateColorEffects() override;

        [[nodiscard]] HRESULT SaveCurrentImageWithEffects(const std::wstring &outPath) override;

        void ClearActiveImage() override;

        // SVG support
        [[nodiscard]] HRESULT LoadSvgFromBytes(const std::vector<BYTE> &svgBytes, const std::wstring &filePath) override;

        bool HasActiveSvg() const override {
            return m_pActiveSvg != nullptr;
        }

        // Cache Management
        std::vector<CacheItem> GetCachedBitmaps() override;

        void ClearCache() override;

        void ClearCache(const std::wstring &excludePath) override;

        void RemoveFromCache(const std::wstring &filePath) override;

        // =====================================================================
        // Thumbnail Panel APIs
        // Each ThumbnailPanelWnd owns its own swap chain — these two accessors
        // let panels borrow the shared D3D/D2D device for one-time init.
        // =====================================================================
        ID3D11Device *GetD3DDevice() const { return m_pD3DDevice.Get(); }
        ID2D1Device6 *GetD2DDevice() const { return m_pD2DDevice.Get(); }

        // Resolve bitmap pointers for a set of thumbnails under the minimum lock
        // duration. Called by ThumbnailPanelWnd::Render() before GPU work begins.
        // useDirCache=true  → look in m_dirThumbCache first (DirWnd panels)
        // useDirCache=false → look in m_bitmapCache only   (CacheWnd panel)
        struct ResolvedThumb {
            Microsoft::WRL::ComPtr<ID2D1Bitmap1> bitmap; // nullptr = placeholder
            D2D1_RECT_F rect;
        };
        void ResolveThumbnailBitmaps(const std::vector<UI::Thumbnail> &thumbnails,
                                     bool useDirCache,
                                     std::vector<ResolvedThumb> &out);

        // Queues an async decode+scale job for one file.
        // Posts WM_QIV_REPAINT to all visible dir panel HWNDs when the thumbnail lands.
        void RequestDirThumbnail(const std::wstring &filePath, HWND hPanel);

        // Drops the entire dir thumbnail cache (call on folder change).
        void ClearDirThumbnailCache();

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

        // Single combined effect: saturation + contrast + brightness + grayscale
        // + invert + sepia folded into one 5x4 color matrix computed explicitly
        // in UpdateColorEffects() (all of these are linear transforms, so they
        // compose into a single matrix — no extra effect nodes needed).
        Microsoft::WRL::ComPtr<ID2D1Effect> m_pColorMatrixEffect;
        // Non-linear / spatial effects that CANNOT be folded into the matrix
        // above — each is its own D2D effect node, chained in a fixed order
        // and bypassed (input wired straight through) when its toggle is off.
        Microsoft::WRL::ComPtr<ID2D1Effect> m_pGammaEffect; // CLSID_D2D1GammaTransfer
        Microsoft::WRL::ComPtr<ID2D1Effect> m_pSolarizeEffect; // CLSID_D2D1TableTransfer
        Microsoft::WRL::ComPtr<ID2D1Effect> m_pThresholdEffect; // CLSID_D2D1Threshold
        Microsoft::WRL::ComPtr<ID2D1Effect> m_pOutlineEffect; // CLSID_D2D1EdgeDetection
        Microsoft::WRL::ComPtr<ID2D1Effect> m_pScaleEffect;
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> m_pBackBufferBitmap;

        // Dir thumbnail cache  —  small scaled-down bitmaps, shared across all DirWnd instances.
        std::unordered_map<std::wstring, Microsoft::WRL::ComPtr<ID2D1Bitmap1> > m_dirThumbCache;
        std::list<std::wstring> m_dirThumbLruList;
        // Running total of VRAM bytes used by m_dirThumbCache entries.
        // Incremented on insert, decremented on evict/clear.
        // Each entry contributes pixelWidth * pixelHeight * 4 bytes.
        size_t m_dirThumbCacheBytes = 0;
        // Tracks paths for which an IO+decode task is already in flight.
        // Guarded by m_dirThumbMutex. Prevents duplicate tasks being queued
        // when UpdateView() is called multiple times before tasks complete.
        std::unordered_set<std::wstring> m_dirThumbInFlight;
        std::mutex m_dirThumbMutex;

        // Cache
        struct CachedBitmap {
            Microsoft::WRL::ComPtr<ID2D1Bitmap1> bitmap;
            std::list<std::wstring>::iterator lruIt;
            UINT width = 0;
            UINT height = 0;
        };

        Microsoft::WRL::ComPtr<ID2D1Bitmap1> m_pBitmap;
        std::unordered_map<std::wstring, CachedBitmap> m_bitmapCache;
        std::list<std::wstring> m_lruList;
        std::mutex m_cacheMutex;

        // SVG
        struct CachedSvg {
            Microsoft::WRL::ComPtr<ID2D1SvgDocument> document;
            std::list<std::wstring>::iterator lruIt;
            float viewportW = 0.0f;
            float viewportH = 0.0f;
        };

        std::unordered_map<std::wstring, CachedSvg> m_svgCache;
        std::list<std::wstring> m_svgLruList;
        Microsoft::WRL::ComPtr<ID2D1SvgDocument> m_pActiveSvg;
        float m_svgNativeW = 0.0f;
        float m_svgNativeH = 0.0f;

        // Window state
        HWND m_hwnd = nullptr;
        D2D1_COLOR_F m_clearColor = D2D1::ColorF(0.08f, 0.08f, 0.08f);

        // Internal helpers
        HRESULT CreateDeviceResources();

        void DiscardDeviceResources();

        HRESULT CreateBackBufferBitmap();

        // Creates the non-linear effect nodes (gamma/solarize/threshold/outline)
        // lazily on first use, since most images never touch them.
        HRESULT EnsureExtraEffects();

        // Wires g_app's saturation/contrast/brightness/grayscale/invert/sepia
        // into m_pColorMatrixEffect, then chains whichever of gamma/solarize/
        // threshold/outline are currently toggled on, in that fixed order.
        // Returns the final ID2D1Image* ready to be scaled/drawn or captured.
        ID2D1Effect *BuildEffectChain(ID2D1Image *source);
};

// Declare the globals so all files see them
extern DecoderThreadPool g_decoderWorker;
extern IoThreadPool g_ioWorker;
extern DecoderThreadPool g_dirThumbWorker;
