#pragma once

#include "IRenderer.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d2d1_3.h>
#include <d2d1svg.h>
#include <dwrite_3.h>
#include <functional>
#include <wrl/client.h>
#include <list>
#include <unordered_map>
#include <string>
#include <mutex>
#include <vector>

class RendererD2D final : public IImageRenderer {
    public:
        int g_hoverIndex = -1;

        RendererD2D() = default;

        ~RendererD2D() override = default;

        [[nodiscard]] HRESULT Initialize(HWND hwnd) override;

        ID2D1DeviceContext7 *GetCacheContext() const {
            return m_pCacheDeviceContext.Get();
        }

        void UpdateTextFormat() override;

        void Resize(UINT width, UINT height) override;

        [[nodiscard]] HRESULT LoadBitmap(IWICBitmapSource *bitmap, UINT width, UINT height, const std::wstring &filePath) override;

        [[nodiscard]] HRESULT PreloadBitmap(const std::wstring &filePath, int requestIndex) override;

        [[nodiscard]] HRESULT Render() override;

        void UpdateColorEffects() override;

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

        // Cache Window
        HRESULT CreateCacheWindowDeviceResources(HWND hwnd);

        void DiscardCacheWindowDeviceResources();

        void RenderCacheWindow(int selectedIndex, int hoverIndex);

        void ResizeCacheWindow(UINT width, UINT height);

        // Public DWrite resources
        Microsoft::WRL::ComPtr<IDWriteFactory3> m_pDWriteFactory;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> m_pTextFormat;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_pTextBrush;

    private:
        // Device-independent resources
        Microsoft::WRL::ComPtr<ID2D1Factory7> m_pD2DFactory;

        // Device-dependent resources
        Microsoft::WRL::ComPtr<ID3D11Device> m_pD3DDevice;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_pD3DContext;
        Microsoft::WRL::ComPtr<IDXGISwapChain1> m_pSwapChain;
        Microsoft::WRL::ComPtr<ID2D1Device6> m_pD2DDevice;
        Microsoft::WRL::ComPtr<ID2D1DeviceContext7> m_pDeviceContext;
        Microsoft::WRL::ComPtr<ID2D1Effect> m_pSaturationEffect;
        Microsoft::WRL::ComPtr<ID2D1Effect> m_pContrastEffect;
        Microsoft::WRL::ComPtr<ID2D1Effect> m_pBrightnessEffect;
        Microsoft::WRL::ComPtr<ID2D1Effect> m_pScaleEffect;
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> m_pBackBufferBitmap;

        // Cache Window Resources
        HWND m_hCacheWnd = nullptr;
        Microsoft::WRL::ComPtr<IDXGISwapChain1> m_pCacheSwapChain;
        Microsoft::WRL::ComPtr<ID2D1DeviceContext7> m_pCacheDeviceContext;
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> m_pCacheBackBuffer;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_pCacheTextBrush;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_pCacheBorderBrush;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_pCacheButtonBrush;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_pCacheButtonTextBrush;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> m_pCacheTextFormat;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> m_pCacheButtonFormat;

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
};
