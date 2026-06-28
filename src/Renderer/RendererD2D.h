#pragma once

#include "IRenderer.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d2d1_3.h>
#include <d2d1_3helper.h>
#include <d2d1svg.h>
#include <dwrite_3.h>
#include <wrl/client.h>
#include <list>
#include <unordered_map>
#include <string>
#include <mutex>
#include <queue>
#include <vector>
#include <d2d1effects_1.h>


class RendererD2D final : public IImageRenderer {
    public:
        RendererD2D() = default;

        ~RendererD2D() override = default;

        [[nodiscard]] HRESULT Initialize(HWND hwnd) override;

        void UpdateTextFormat() override;

        void Resize(UINT width, UINT height) override;

        [[nodiscard]] HRESULT LoadBitmap(IWICBitmapSource *bitmap, UINT width, UINT height,
                                         const std::wstring &filePath) override;

        [[nodiscard]] HRESULT PreloadBitmap(const std::wstring &filePath, int requestIndex) override;

        [[nodiscard]] HRESULT Render() override;

        void ProcessPendingUploads() override;

        void UpdateColorEffects() override;

        // SVG support
        [[nodiscard]] HRESULT LoadSvgFromBytes(const std::vector<BYTE> &svgBytes,
                                               const std::wstring &filePath) override;

        bool HasActiveSvg() const override {
            return m_pActiveSvg != nullptr;
        }

        // Public DWrite resources (used by callers that draw text through the DeviceContext)
        Microsoft::WRL::ComPtr<IDWriteFactory3> m_pDWriteFactory;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> m_pTextFormat;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_pTextBrush;

    private:
        // -------------------------------------------------------------------------
        // Device-independent resources  (survive device loss)
        // -------------------------------------------------------------------------
        Microsoft::WRL::ComPtr<ID2D1Factory7> m_pD2DFactory;

        // -------------------------------------------------------------------------
        // Device-dependent resources  (recreated on D2DERR_RECREATE_TARGET / DXGI_ERROR_DEVICE_REMOVED)
        // -------------------------------------------------------------------------
        Microsoft::WRL::ComPtr<ID3D11Device> m_pD3DDevice;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_pD3DContext;
        Microsoft::WRL::ComPtr<IDXGISwapChain1> m_pSwapChain;
        Microsoft::WRL::ComPtr<ID2D1Device6> m_pD2DDevice;
        Microsoft::WRL::ComPtr<ID2D1DeviceContext7> m_pDeviceContext;
        Microsoft::WRL::ComPtr<ID2D1Effect> m_pSaturationEffect;
        Microsoft::WRL::ComPtr<ID2D1Effect> m_pContrastEffect;
        Microsoft::WRL::ComPtr<ID2D1Effect> m_pBrightnessEffect;
        Microsoft::WRL::ComPtr<ID2D1Effect> m_pScaleEffect;

        // Back-buffer D2D bitmap we render into each frame
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> m_pBackBufferBitmap;

        // -------------------------------------------------------------------------
        // Cache
        // -------------------------------------------------------------------------
        struct CachedBitmap {
            Microsoft::WRL::ComPtr<ID2D1Bitmap1> bitmap;
            std::list<std::wstring>::iterator lruIt;
            UINT width = 0;
            UINT height = 0;
        };

        struct PendingUpload {
            int playlistIndex;
            std::wstring filePath;
            std::vector<BYTE> pixelData;
            UINT stride;
            UINT width;
            UINT height;
        };

        Microsoft::WRL::ComPtr<ID2D1Bitmap1> m_pBitmap; // currently displayed bitmap

        std::unordered_map<std::wstring, CachedBitmap> m_bitmapCache;
        std::list<std::wstring> m_lruList;
        std::queue<PendingUpload> m_pendingUploads;
        std::mutex m_cacheMutex;
        std::mutex m_uploadMutex;

        // -------------------------------------------------------------------------
        // SVG  (ID2D1SvgDocument – device-dependent, released on device loss)
        // -------------------------------------------------------------------------
        struct CachedSvg {
            Microsoft::WRL::ComPtr<ID2D1SvgDocument> document;
            float viewportW = 0.0f;
            float viewportH = 0.0f;
        };

        // Keyed by canonical file path, same as bitmap cache
        std::unordered_map<std::wstring, CachedSvg> m_svgCache;

        // The SVG document to draw this frame (nullptr when a raster image is active)
        Microsoft::WRL::ComPtr<ID2D1SvgDocument> m_pActiveSvg;
        float m_svgNativeW = 0.0f; // intrinsic SVG width  (from viewBox / width attr)
        float m_svgNativeH = 0.0f; // intrinsic SVG height

        // -------------------------------------------------------------------------
        // Window / state
        // -------------------------------------------------------------------------
        HWND m_hwnd = nullptr;
        D2D1_COLOR_F m_clearColor = D2D1::ColorF(0.08f, 0.08f, 0.08f);

        // -------------------------------------------------------------------------
        // Internal helpers
        // -------------------------------------------------------------------------
        HRESULT CreateDeviceResources();

        void DiscardDeviceResources();

        HRESULT CreateBackBufferBitmap();

        HRESULT UploadAndCacheBitmap(const std::vector<BYTE> &pixelData, UINT stride,
                                     const std::wstring &filePath, UINT width, UINT height);

        HRESULT UploadAndCacheBitmap_Locked(const std::vector<BYTE> &pixelData, UINT stride,
                                            const std::wstring &filePath, UINT width, UINT height);
};
