#include "RendererD2D.h"
#include "../AppState.h"
#include "../Constants.h"
#include <algorithm>
#include "../WorkerThread.h"
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

extern WorkerThread g_decoderWorker;
extern WorkerThread g_ioWorker;

HRESULT RendererD2D::Initialize(HWND hwnd) {
    m_hwnd = hwnd;

    if (!m_pFactory) {
        HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, m_pFactory.GetAddressOf());
        if (FAILED(hr)) return hr;
    }

    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(m_pDWriteFactory.GetAddressOf())))) {
        return E_FAIL;
    }

    // Attempt to create primary font, fallback to Arial if it fails
    HRESULT hrFont = m_pDWriteFactory->CreateTextFormat(
            L"Segoe UI", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            12.0f, L"en-us", &m_pTextFormat);

    if (FAILED(hrFont)) {
        (void) m_pDWriteFactory->CreateTextFormat(
                L"Arial", nullptr,
                DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                12.0f, L"en-us", &m_pTextFormat);
    }

    RECT rc{};
    GetClientRect(hwnd, &rc);
    HRESULT hr = m_pFactory->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(),
            D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top)),
            m_pRenderTarget.GetAddressOf());

    if (FAILED(hr)) return hr;

    return m_pRenderTarget->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::LightGreen), &m_pTextBrush);
}

void RendererD2D::Resize(UINT width, UINT height) {
    if (m_pRenderTarget) m_pRenderTarget->Resize(D2D1::SizeU(width, height));
}

HRESULT RendererD2D::UploadAndCacheBitmap(IWICBitmapSource *bitmap, const std::wstring &filePath, UINT width, UINT height) {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    return UploadAndCacheBitmap_Locked(bitmap, filePath, width, height);
}

HRESULT RendererD2D::UploadAndCacheBitmap_Locked(IWICBitmapSource *bitmap, const std::wstring &filePath, UINT width, UINT height) {
    if (!m_pRenderTarget) return E_UNEXPECTED;

    Microsoft::WRL::ComPtr<ID2D1Bitmap> newBitmap;
    HRESULT hr = m_pRenderTarget->CreateBitmapFromWicBitmap(bitmap, nullptr, newBitmap.GetAddressOf());
    if (FAILED(hr)) return hr;

    auto existing = m_bitmapCache.find(filePath);
    if (existing != m_bitmapCache.end()) {
        m_lruList.erase(existing->second.lruIt);
        m_bitmapCache.erase(existing);
    }

    if (m_lruList.size() >= Constants::VRAM_CACHE_IMAGES_COUNT) {
        m_bitmapCache.erase(m_lruList.back());
        m_lruList.pop_back();
    }

    m_lruList.push_front(filePath);
    m_bitmapCache.emplace(filePath, CachedBitmap{newBitmap, m_lruList.begin(), width, height});
    return S_OK;
}

void RendererD2D::ProcessPendingUploads() {
    while (true) {
        PendingUpload upload;
        {
            std::lock_guard<std::mutex> lock(m_cacheMutex);
            if (m_pendingUploads.empty()) break;
            upload = std::move(m_pendingUploads.front());
            m_pendingUploads.pop();
        }

        int currentIdx = g_app.wantedIndex.load(std::memory_order_acquire);
        int dist = std::abs(upload.playlistIndex - currentIdx);

        if (dist <= Constants::PRELOAD_LOOKASIDE_COUNT) {
            HRESULT hr = UploadAndCacheBitmap(upload.converter.Get(), upload.filePath, upload.width, upload.height);
#ifdef _DEBUG
            if (FAILED(hr)) OutputDebugStringW(L"CreateBitmapFromWic failed.\n");
#else
            (void) hr;
#endif
        }
    }
}

HRESULT RendererD2D::LoadBitmap(IWICBitmapSource *bitmap, UINT width, UINT height, const std::wstring &filePath) {
    {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        auto it = m_bitmapCache.find(filePath);
        if (it != m_bitmapCache.end()) {
            m_lruList.splice(m_lruList.begin(), m_lruList, it->second.lruIt);
            m_pBitmap = it->second.bitmap;
            g_app.imgWidth = static_cast<int>(it->second.width);
            g_app.imgHeight = static_cast<int>(it->second.height);
            return S_OK;
        }
    }

    if (!bitmap || !m_pRenderTarget) return E_FAIL;

    Microsoft::WRL::ComPtr<ID2D1Bitmap> newBitmap;
    HRESULT hr = m_pRenderTarget->CreateBitmapFromWicBitmap(bitmap, nullptr, newBitmap.GetAddressOf());

    if (SUCCEEDED(hr)) {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        auto it = m_bitmapCache.find(filePath);
        if (it != m_bitmapCache.end()) {
            m_lruList.erase(it->second.lruIt);
            m_bitmapCache.erase(it);
        }

        if (m_lruList.size() >= Constants::VRAM_CACHE_IMAGES_COUNT) {
            m_bitmapCache.erase(m_lruList.back());
            m_lruList.pop_back();
        }

        m_lruList.push_front(filePath);
        m_bitmapCache[filePath] = {newBitmap, m_lruList.begin(), width, height};
        m_pBitmap = newBitmap;

        g_app.imgWidth = static_cast<int>(width);
        g_app.imgHeight = static_cast<int>(height);
    }
    return hr;
}

HRESULT RendererD2D::PreloadBitmap(const std::wstring &filePath, int requestIndex) {
    g_ioWorker.PushTask([filePath, requestIndex, this]() {
        Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
        if (FAILED(g_app.wicFactory->CreateDecoderFromFilename(
            filePath.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)))
            return;

        g_decoderWorker.PushTask([decoder, filePath, requestIndex, this]() {
            if (g_app.wantedIndex.load(std::memory_order_acquire) != requestIndex) return;

            Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
            if (FAILED(decoder->GetFrame(0, &frame))) return;

            UINT width, height;
            frame->GetSize(&width, &height);

            Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
            if (FAILED(g_app.wicFactory->CreateFormatConverter(&converter))) return;

            if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                WICBitmapDitherTypeNone, nullptr, 0.0f, WICBitmapPaletteTypeCustom)))
                return;

            std::lock_guard<std::mutex> lock(m_cacheMutex);
            m_pendingUploads.push({requestIndex, filePath, converter, width, height});
            PostMessageW(m_hwnd, Constants::WM_QIV_PENDING_UPLOADS, 0, 0);
        });
    });
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

        const float base = std::min(rtSize.width / size.width, rtSize.height / size.height);
        const float z = (g_app.viewport.zoom <= 0.0f) ? 1.0f : g_app.viewport.zoom;
        const float renderW = size.width * base * z;
        const float renderH = size.height * base * z;
        const float left = (rtSize.width - renderW) / 2.0f + g_app.viewport.offsetX;
        const float top = (rtSize.height - renderH) / 2.0f + g_app.viewport.offsetY;

        D2D1_MATRIX_3X2_F transform = D2D1::Matrix3x2F::Identity();
        if (g_app.viewport.flippedH) transform = transform * D2D1::Matrix3x2F::Scale(-1.0f, 1.0f, center);
        if (g_app.viewport.flippedV) transform = transform * D2D1::Matrix3x2F::Scale(1.0f, -1.0f, center);
        transform = transform * D2D1::Matrix3x2F::Rotation(static_cast<float>(g_app.viewport.rotation), center);

        m_pRenderTarget->SetTransform(transform);
        m_pRenderTarget->DrawBitmap(m_pBitmap.Get(), D2D1::RectF(left, top, left + renderW, top + renderH));
        m_pRenderTarget->SetTransform(D2D1::Matrix3x2F::Identity());

        if (!g_app.playlist.empty() && g_app.showOverlayInfoText) {
            std::wstring fullPath = g_app.playlist[g_app.currentIndex];
            std::wstring fileName = fullPath.substr(fullPath.find_last_of(L"\\/") + 1);
            std::wstring text = std::to_wstring(g_app.currentIndex + 1) + L" / " +
                                std::to_wstring(g_app.playlist.size()) + L" - " + fileName;
            D2D1_RECT_F layoutRect = D2D1::RectF(15.0f, 6.0f, rtSize.width - 10.0f, rtSize.height - 10.0f);

            // Safety check: ensure resources exist before drawing
            if (m_pTextFormat && m_pTextBrush) {
                m_pRenderTarget->DrawText(text.c_str(), static_cast<UINT32>(text.length()),
                                          m_pTextFormat.Get(), layoutRect, m_pTextBrush.Get());
            }
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
        hr = m_pFactory->CreateHwndRenderTarget(D2D1::RenderTargetProperties(),
                                                D2D1::HwndRenderTargetProperties(m_hwnd, D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top)),
                                                m_pRenderTarget.GetAddressOf());
        if (SUCCEEDED(hr)) {
            m_pTextBrush.Reset();
            (void) m_pRenderTarget->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::LightGreen), &m_pTextBrush);
        }
    }
    return hr;
}
