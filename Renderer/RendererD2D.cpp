#include "RendererD2D.h"
#include "../AppState.h"
#include "../Constants.h"
#include <algorithm>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")


HRESULT RendererD2D::Initialize(HWND hwnd) {
    m_hwnd = hwnd;
    if (!m_pFactory) {
        HRESULT hr = D2D1CreateFactory(
            D2D1_FACTORY_TYPE_SINGLE_THREADED,
            m_pFactory.GetAddressOf());
        if (FAILED(hr))
            return hr;
    }
    RECT rc{};
    GetClientRect(hwnd, &rc);
    return m_pFactory->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(),
        D2D1::HwndRenderTargetProperties(
            hwnd,
            D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top)),
        m_pRenderTarget.GetAddressOf());
}

void RendererD2D::Resize(UINT width, UINT height) {
    if (m_pRenderTarget) m_pRenderTarget->Resize(D2D1::SizeU(width, height));
}

// UI THREAD ONLY: Uploads decoded data to VRAM. Caller must NOT hold m_cacheMutex.
HRESULT RendererD2D::CreateBitmapFromWic(IWICBitmapSource *bitmap, const std::wstring &filePath) {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    return CreateBitmapFromWic_Locked(bitmap, filePath);
}

// UI THREAD ONLY: Internal version — caller must already hold m_cacheMutex.
HRESULT RendererD2D::CreateBitmapFromWic_Locked(IWICBitmapSource *bitmap, const std::wstring &filePath) {
    if (!m_pRenderTarget) return E_UNEXPECTED;

    Microsoft::WRL::ComPtr<ID2D1Bitmap> newBitmap;
    HRESULT hr = m_pRenderTarget->CreateBitmapFromWicBitmap(bitmap, nullptr, newBitmap.GetAddressOf());
    if (FAILED(hr)) return hr;

    // Remove existing entry if present.
    auto existing = m_bitmapCache.find(filePath);
    if (existing != m_bitmapCache.end()) {
        m_lruList.erase(existing->second.lruIt);
        m_bitmapCache.erase(existing);
    }

    // Evict least recently used image if cache is full.
    if (m_lruList.size() >= Config::VRAM_CACHE_IMAGES_COUNT) {
        m_bitmapCache.erase(m_lruList.back());
        m_lruList.pop_back();
    }

    // Insert the new bitmap.
    m_lruList.push_front(filePath);
    m_bitmapCache.emplace(
        filePath,
        CachedBitmap{newBitmap, m_lruList.begin()});
    return S_OK;
}

// UI THREAD ONLY: Drain the queue of images decoded in the background.
void RendererD2D::ProcessPendingUploads() {
    while (true) {
        PendingUpload upload;
        {
            std::lock_guard<std::mutex> lock(m_cacheMutex);
            if (m_pendingUploads.empty())
                break;
            upload = std::move(m_pendingUploads.front());
            m_pendingUploads.pop();
        }
        HRESULT hr = CreateBitmapFromWic(
            upload.converter.Get(),
            upload.filePath);
#ifdef _DEBUG
        if (FAILED(hr))
            OutputDebugStringW(L"CreateBitmapFromWic failed.\n");
#endif
    }
}

HRESULT RendererD2D::LoadBitmap(IWICBitmapSource *bitmap, UINT width, UINT height, const std::wstring &filePath) {
    std::lock_guard<std::mutex> lock(m_cacheMutex);

    auto it = m_bitmapCache.find(filePath);
    if (it != m_bitmapCache.end()) {
        // Cache hit: promote in LRU and set as active
        m_lruList.splice(m_lruList.begin(), m_lruList, it->second.lruIt);
        m_pBitmap = it->second.bitmap;
        return S_OK;
    }

    // Cache miss: if no bitmap was provided this was a cache-only probe — fail cleanly
    if (!bitmap) return E_FAIL;

    // Cache miss: decode into VRAM now (UI thread only)
    HRESULT hr = CreateBitmapFromWic_Locked(bitmap, filePath);
    if (SUCCEEDED(hr)) {
        // The new bitmap was just inserted at the front of the LRU list
        m_pBitmap = m_bitmapCache[filePath].bitmap;
    }
    return hr;
}

// BACKGROUND THREAD: Performs I/O and Decoding only
HRESULT RendererD2D::PreloadBitmap(const std::wstring &filePath) {
    // 1. Thread-safe check
    {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        if (m_bitmapCache.find(filePath) != m_bitmapCache.end()) return S_OK;
    }

    // 2. Decode using the global factory
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(g_app.wicFactory->CreateDecoderFromFilename(
        filePath.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)))
        return E_FAIL;

    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) return E_FAIL;

    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    HRESULT hr = g_app.wicFactory->CreateFormatConverter(&converter);
    if (FAILED(hr))
        return hr;

    hr = converter->Initialize(
        frame.Get(),
        GUID_WICPixelFormat32bppPBGRA,
        WICBitmapDitherTypeNone,
        nullptr,
        0.0f,
        WICBitmapPaletteTypeCustom);

    if (FAILED(hr))
        return hr;

    // 3. Hand-off: Push to queue and signal UI
    {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        m_pendingUploads.push({filePath, converter});
    }
    if (!PostMessage(m_hwnd, Config::WM_QIV_PENDING_UPLOADS, 0, 0))
        return HRESULT_FROM_WIN32(GetLastError());

    return S_OK;
}

HRESULT RendererD2D::Render() {
    if (!m_pRenderTarget) return E_FAIL;
    m_pRenderTarget->BeginDraw();
    m_pRenderTarget->Clear(m_clearColor);

    if (m_pBitmap) {
        D2D1_SIZE_F rtSize = m_pRenderTarget->GetSize();
        float imageW = (float) m_pBitmap->GetSize().width;
        float imageH = (float) m_pBitmap->GetSize().height;
        float base = (std::min)(rtSize.width / imageW, rtSize.height / imageH);
        float z = (g_app.viewport.zoom <= 0.0f) ? 1.0f : g_app.viewport.zoom;
        float renderW = imageW * base * z;
        float renderH = imageH * base * z;
        float left = (rtSize.width - renderW) / 2.0f + g_app.viewport.offsetX;
        float top = (rtSize.height - renderH) / 2.0f + g_app.viewport.offsetY;
        m_pRenderTarget->DrawBitmap(m_pBitmap.Get(), D2D1::RectF(left, top, left + renderW, top + renderH));
    }
    HRESULT hr = m_pRenderTarget->EndDraw();

    if (hr == D2DERR_RECREATE_TARGET) {
        m_pBitmap.Reset();
        m_pRenderTarget.Reset();

        RECT rc{};
        GetClientRect(m_hwnd, &rc);

        hr = m_pFactory->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(),
            D2D1::HwndRenderTargetProperties(
                m_hwnd,
                D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top)),
            m_pRenderTarget.GetAddressOf());

        if (SUCCEEDED(hr))
            m_pBitmap.Reset();
    }

    return hr;
}
