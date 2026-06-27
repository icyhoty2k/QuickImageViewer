#include "RendererD2D.h"
#include "../AppState.h"
#include "../Constants.h"
#include <algorithm>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")


HRESULT RendererD2D::Initialize(HWND hwnd) {
    m_hwnd = hwnd;

    // 1. Create the D2D Factory FIRST
    if (!m_pFactory) {
        HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, m_pFactory.GetAddressOf());
        if (FAILED(hr)) return hr;
    }

    // 2. Create the DWrite Factory
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(m_pDWriteFactory.GetAddressOf())))) {
        return E_FAIL;
    }

    // 3. Setup Text Format
    m_pDWriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                       DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"en-us", &m_pTextFormat);

    // 4. Create the Render Target
    RECT rc{};
    GetClientRect(hwnd, &rc);
    HRESULT hr = m_pFactory->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(),
        D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top)),
        m_pRenderTarget.GetAddressOf());

    if (FAILED(hr)) return hr;

    // 5. Create Brush (Requires active RenderTarget)
    return m_pRenderTarget->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::LightGreen), &m_pTextBrush);
}

void RendererD2D::Resize(UINT width, UINT height) {
    if (m_pRenderTarget) m_pRenderTarget->Resize(D2D1::SizeU(width, height));
}

// UI THREAD ONLY: Uploads decoded data to VRAM. Caller must NOT hold m_cacheMutex.
HRESULT RendererD2D::UploadAndCacheBitmap(IWICBitmapSource *bitmap, const std::wstring &filePath) {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    return UploadAndCacheBitmap_Locked(bitmap, filePath);
}

// UI THREAD ONLY: Internal version — caller must already hold m_cacheMutex.
HRESULT RendererD2D::UploadAndCacheBitmap_Locked(IWICBitmapSource *bitmap, const std::wstring &filePath) {
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
        HRESULT hr = UploadAndCacheBitmap(
            upload.converter.Get(),
            upload.filePath);
#ifdef _DEBUG
        if (FAILED(hr))
            OutputDebugStringW(L"CreateBitmapFromWic failed.\n");
#endif
    }
}

HRESULT RendererD2D::LoadBitmap(IWICBitmapSource *bitmap, UINT width, UINT height, const std::wstring &filePath) {
    // 1. Quick cache check
    {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        auto it = m_bitmapCache.find(filePath);
        if (it != m_bitmapCache.end()) {
            m_lruList.splice(m_lruList.begin(), m_lruList, it->second.lruIt);
            m_pBitmap = it->second.bitmap;
            return S_OK;
        }
    }

    if (!bitmap || !m_pRenderTarget) return E_FAIL;

    // 2. Perform expensive GPU upload OUTSIDE the lock
    Microsoft::WRL::ComPtr<ID2D1Bitmap> newBitmap;
    HRESULT hr = m_pRenderTarget->CreateBitmapFromWicBitmap(bitmap, nullptr, newBitmap.GetAddressOf());

    if (SUCCEEDED(hr)) {
        std::lock_guard<std::mutex> lock(m_cacheMutex);

        // Final sanity check: if it was added by a concurrent process
        // during our upload, remove the old one first.
        auto it = m_bitmapCache.find(filePath);
        if (it != m_bitmapCache.end()) {
            m_lruList.erase(it->second.lruIt);
            m_bitmapCache.erase(it);
        }

        if (m_lruList.size() >= Config::VRAM_CACHE_IMAGES_COUNT) {
            m_bitmapCache.erase(m_lruList.back());
            m_lruList.pop_back();
        }

        m_lruList.push_front(filePath);
        m_bitmapCache[filePath] = {newBitmap, m_lruList.begin()};
        m_pBitmap = newBitmap;
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
    if (FAILED(g_decoderWorker.wicFactory->CreateDecoderFromFilename(
        filePath.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)))
        return E_FAIL;

    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) return E_FAIL;

    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    HRESULT hr = g_decoderWorker.wicFactory->CreateFormatConverter(&converter);
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
        const auto size = m_pBitmap->GetSize();
        const D2D1_SIZE_F rtSize = m_pRenderTarget->GetSize();
        const D2D1_POINT_2F center = D2D1::Point2F(rtSize.width / 2.0f, rtSize.height / 2.0f);

        // Calculate basic geometry
        const float base = std::min(rtSize.width / size.width, rtSize.height / size.height);
        const float z = (g_app.viewport.zoom <= 0.0f) ? 1.0f : g_app.viewport.zoom;
        const float renderW = size.width * base * z;
        const float renderH = size.height * base * z;
        const float left = (rtSize.width - renderW) / 2.0f + g_app.viewport.offsetX;
        const float top = (rtSize.height - renderH) / 2.0f + g_app.viewport.offsetY;

        // Combine transformations
        // 1. Start with Identity
        D2D1_MATRIX_3X2_F transform = D2D1::Matrix3x2F::Identity();

        // 2. Apply flip horizontal if needed (scale X by -1, relative to center)
        if (g_app.viewport.flippedH) {
            transform = transform * D2D1::Matrix3x2F::Scale(-1.0f, 1.0f, center);
        }
        // 2. Apply flip vertical if needed (scale Y by -1, relative to center)
        if (g_app.viewport.flippedV) {
            transform = transform * D2D1::Matrix3x2F::Scale(1.0f, -1.0f, center);
        }

        // 3. Apply rotation (relative to center)
        transform = transform * D2D1::Matrix3x2F::Rotation((float) g_app.viewport.rotation, center);

        m_pRenderTarget->SetTransform(transform);
        m_pRenderTarget->DrawBitmap(m_pBitmap.Get(), D2D1::RectF(left, top, left + renderW, top + renderH));
        m_pRenderTarget->SetTransform(D2D1::Matrix3x2F::Identity());
        if (!g_app.playlist.empty() && g_app.showOverlayInfoText) {
            // 1. Get the current path
            std::wstring fullPath = g_app.playlist[g_app.currentIndex];

            // 2. Extract filename (everything after the last backslash)
            std::wstring fileName = fullPath.substr(fullPath.find_last_of(L"\\/") + 1);

            // 3. Format the display string
            std::wstring text = std::to_wstring(g_app.currentIndex + 1) + L" / " +
                                std::to_wstring(g_app.playlist.size()) + L" - " + fileName;
            D2D1_RECT_F layoutRect = D2D1::RectF(0 + 15.0f, 0 + 6.0f, rtSize.width - 10.0f, rtSize.height - 10.0f);
            m_pRenderTarget->DrawText(text.c_str(), (UINT32) text.length(), m_pTextFormat.Get(), layoutRect, m_pTextBrush.Get());
        }
    }

    HRESULT hr = m_pRenderTarget->EndDraw();

    if (hr == D2DERR_RECREATE_TARGET) {
        m_pBitmap.Reset();
        m_bitmapCache.clear();
        m_lruList.clear();
        m_pRenderTarget.Reset();

        RECT rc{};
        GetClientRect(m_hwnd, &rc);
        hr = m_pFactory->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(),
            D2D1::HwndRenderTargetProperties(m_hwnd, D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top)),
            m_pRenderTarget.GetAddressOf());
    }
    return hr;
}
