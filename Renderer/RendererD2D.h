#pragma once

#include "IRenderer.h"
#include <d2d1.h>
#include <wrl/client.h>
#include <list>
#include <unordered_map>
#include <string>
#include <mutex>
#include <queue>
#include "../Constants.h"

class RendererD2D final : public IImageRenderer
{
public:
    RendererD2D();
    ~RendererD2D() override;

    [[nodiscard]] HRESULT Initialize(HWND hwnd) override;
    void Resize(UINT width, UINT height) override;
    [[nodiscard]] HRESULT LoadBitmap(IWICBitmapSource* bitmap, UINT width, UINT height, const std::wstring& filePath) override;
    [[nodiscard]] HRESULT PreloadBitmap(const std::wstring& filePath) override;
    [[nodiscard]] HRESULT Render() override;

    // UI THREAD: Processes the background-decoded images
    void ProcessPendingUploads();

private:
    struct CachedBitmap {
        Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
        std::list<std::wstring>::iterator lruIt;
    };

    struct PendingUpload {
        std::wstring filePath;
        Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    };

    // Direct2D Resources
    Microsoft::WRL::ComPtr<ID2D1Factory> m_pFactory;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> m_pRenderTarget;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> m_pBitmap;

    HRESULT CreateBitmapFromWic(IWICBitmapSource* bitmap, const std::wstring& filePath);
    // Internal version — caller must already hold m_cacheMutex
    HRESULT CreateBitmapFromWic_Locked(IWICBitmapSource* bitmap, const std::wstring& filePath);

    // Cache management
    std::unordered_map<std::wstring, CachedBitmap> m_bitmapCache;
    std::list<std::wstring> m_lruList;
    std::queue<PendingUpload> m_pendingUploads; // Moved inside class

    std::mutex m_cacheMutex;

    HWND m_hwnd = nullptr;
    D2D1_COLOR_F m_clearColor = D2D1::ColorF(0.08f, 0.08f, 0.08f);
};