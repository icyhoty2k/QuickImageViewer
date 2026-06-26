#pragma once

#include "IRenderer.h"
#include <d2d1.h>
#include <wrl/client.h>
#include <list>
#include <unordered_map>
#include <string>
#include <mutex> // Required for thread-safe preloading
#include "../Constants.h" // Access your VRAM_CACHE_IMAGES_COUNT

class RendererD2D final : public IImageRenderer
{
public:
    RendererD2D();
    ~RendererD2D() override;

    [[nodiscard]] HRESULT Initialize(HWND hwnd) override;
    void Resize(UINT width, UINT height) override;

    [[nodiscard]] HRESULT LoadBitmap(IWICBitmapSource* bitmap, UINT width, UINT height, const std::wstring& filePath) override;

    // Implementation of the new interface method
    [[nodiscard]] HRESULT PreloadBitmap(const std::wstring& filePath) override;

    [[nodiscard]] HRESULT Render() override;

private:
    struct CachedBitmap {
        Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
        std::list<std::wstring>::iterator lruIt;
    };

    // Direct2D Resources
    Microsoft::WRL::ComPtr<ID2D1Factory> m_pFactory;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> m_pRenderTarget;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> m_pBitmap;

    // Cache management
    std::unordered_map<std::wstring, CachedBitmap> m_bitmapCache;
    std::list<std::wstring> m_lruList;
    std::mutex m_cacheMutex; // Mutex to protect cache from background thread access

    HWND m_hwnd = nullptr;
    D2D1_COLOR_F m_clearColor = D2D1::ColorF(0.08f, 0.08f, 0.08f);
};