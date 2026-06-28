#include "RendererD2D.h"
#include "../AppState.h"
#include "../Platform/Constants.h"
#include <algorithm>
#include "../WorkerThread.h"
#include <chrono>
#include <vector>
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

extern WorkerThread g_decoderWorker;
extern IoThreadPool g_ioWorker;

HRESULT RendererD2D::Initialize(HWND hwnd) {
    m_hwnd = hwnd;

    if (!m_pFactory) {
        HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, m_pFactory.GetAddressOf());
        if (FAILED(hr)) return hr;
    }

    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown **>(m_pDWriteFactory.GetAddressOf())))) {
        return E_FAIL;
    }

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

HRESULT RendererD2D::UploadAndCacheBitmap(const std::vector<BYTE> &pixelData, UINT stride, const std::wstring &filePath, UINT width, UINT height) {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    return UploadAndCacheBitmap_Locked(pixelData, stride, filePath, width, height);
}

HRESULT RendererD2D::UploadAndCacheBitmap_Locked(const std::vector<BYTE> &pixelData, UINT stride, const std::wstring &filePath, UINT width, UINT height) {
    if (!m_pRenderTarget || pixelData.empty()) return E_UNEXPECTED;

    Microsoft::WRL::ComPtr<ID2D1Bitmap> targetBitmap;

    // 1. Check if the exact file is already in the cache (e.g., reloading current image)
    auto existing = m_bitmapCache.find(filePath);
    if (existing != m_bitmapCache.end()) {
        if (existing->second.width == width && existing->second.height == height) {
            targetBitmap = existing->second.bitmap; // Claim this buffer for recycling
        }
        m_lruList.erase(existing->second.lruIt);
        m_bitmapCache.erase(existing);
    }

    // 2. Check the LRU eviction candidate if we hit the cache limit
    if (!targetBitmap && m_lruList.size() >= Constants::VRAM_CACHE_IMAGES_COUNT) {
        auto oldestIt = m_bitmapCache.find(m_lruList.back());
        if (oldestIt != m_bitmapCache.end()) {
            // Only recycle if the dimensions are an exact match
            if (oldestIt->second.width == width && oldestIt->second.height == height) {
                targetBitmap = oldestIt->second.bitmap; // Claim this buffer for recycling
            }
            m_bitmapCache.erase(oldestIt);
        }
        m_lruList.pop_back();
    }

    HRESULT hr = S_OK;

    // 3. Upload Pixels: Recycle if possible, Allocate if necessary
    if (targetBitmap) {
        // FAST PATH: Direct PCIe transfer to existing VRAM buffer. Zero OS allocation overhead.
        D2D1_RECT_U destRect = D2D1::RectU(0, 0, width, height);
        hr = targetBitmap->CopyFromMemory(&destRect, pixelData.data(), stride);
    } else {
        // SLOW PATH: Dimensions didn't match (or cache is still filling up), allocate new VRAM.
        D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
                );
        hr = m_pRenderTarget->CreateBitmap(
                D2D1::SizeU(width, height),
                pixelData.data(),
                stride,
                props,
                targetBitmap.GetAddressOf()
                );
    }

    if (FAILED(hr)) return hr;

    // 4. Register the buffer in the cache under the new file path
    m_lruList.push_front(filePath);
    m_bitmapCache.emplace(filePath, CachedBitmap{targetBitmap, m_lruList.begin(), width, height});

    return S_OK;
}

void RendererD2D::ProcessPendingUploads() {
    auto startTime = std::chrono::high_resolution_clock::now();
    while (true) {
        PendingUpload upload;
        {
            std::lock_guard<std::mutex> lock(m_cacheMutex);
            if (m_pendingUploads.empty()) break;
            auto now = std::chrono::high_resolution_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count() > 3) {
                PostMessageW(m_hwnd, Constants::WM_QIV_PENDING_UPLOADS, 0, 0);
                break;
            }
            upload = std::move(m_pendingUploads.front());
            m_pendingUploads.pop();
        }

        int currentIdx = g_app.wantedIndex.load(std::memory_order_acquire);
        int dist = std::abs(upload.playlistIndex - currentIdx);

        if (dist <= Constants::PRELOAD_LOOKASIDE_COUNT) {
            HRESULT hr = UploadAndCacheBitmap(upload.pixelData, upload.stride, upload.filePath, upload.width, upload.height);
#ifdef _DEBUG
            if (FAILED(hr)) OutputDebugStringW(L"RAM to VRAM upload failed.\n");
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
    Microsoft::WRL::ComPtr<IWICImagingFactory2> wicFac = g_decoderWorker.wicFactory;
    if (!wicFac) return E_UNEXPECTED;

    g_ioWorker.PushTask([filePath, requestIndex, wicFac, this]() {
        if (g_app.wantedIndex.load(std::memory_order_acquire) != requestIndex) return;

        Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
        HRESULT hr = wicFac->CreateDecoderFromFilename(
                filePath.c_str(), nullptr, GENERIC_READ,
                WICDecodeMetadataCacheOnDemand, &decoder);
        if (FAILED(hr)) return;

        g_decoderWorker.PushTask([decoder, filePath, requestIndex, this]() {
            if (g_app.wantedIndex.load(std::memory_order_acquire) != requestIndex) return;

            Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
            if (FAILED(decoder->GetFrame(0, &frame))) return;

            UINT width = 0, height = 0;
            frame->GetSize(&width, &height);

            Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
            if (FAILED(g_decoderWorker.wicFactory->CreateFormatConverter(&converter))) return;

            if (FAILED(converter->Initialize(
                frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                WICBitmapDitherTypeNone, nullptr, 0.0f,
                WICBitmapPaletteTypeCustom)))
                return;

            // Extract pixels directly into standard system RAM
            UINT stride = width * 4; // 32bpp = 4 bytes per pixel
            std::vector<BYTE> pixelData(stride * height);

            HRESULT copyHr = converter->CopyPixels(
                    nullptr, stride,
                    static_cast<UINT>(pixelData.size()),
                    pixelData.data()
                    );

            if (FAILED(copyHr)) return;

            {
                std::lock_guard<std::mutex> lock(m_cacheMutex);
                // Move the raw buffer into the queue (std::move ensures no deep copy overhead)
                m_pendingUploads.push({requestIndex, filePath, std::move(pixelData), stride, width, height});
            }
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
        // 1. Calculate ratios for FitToView and FitToWindow
        float ratioX = rtSize.width / size.width;
        float ratioY = rtSize.height / size.height;

        float renderW = size.width;
        float renderH = size.height;

        // 2. Exact, rigid axis control
        switch (g_app.viewMode) {
            case Constants::ViewModes::ViewMode::FitToView:
                renderW = size.width * std::min(ratioX, ratioY);
                renderH = size.height * std::min(ratioX, ratioY);
                break;

            case Constants::ViewModes::ViewMode::FitToWidth:
                // 1. Force width to window edges
                renderW = rtSize.width;

                // 2. Take the original height (do NOT multiply or grow it)
                renderH = size.height;

                // 3. The Hard Stop: If the image is taller than the window, crush it to the window height
                if (renderH > rtSize.height) {
                    renderH = rtSize.height;
                }
                break;

            case Constants::ViewModes::ViewMode::FitToHeight:
                // 1. Force height to window edges
                renderH = rtSize.height;

                // 2. Take the original width (do NOT multiply or grow it)
                renderW = size.width;

                // 3. The Hard Stop: If the image is wider than the window, crush it to the window width
                if (renderW > rtSize.width) {
                    renderW = rtSize.width;
                }
                break;

            case Constants::ViewModes::ViewMode::FitToWindow:
                // Stretch both axes to fill the window completely
                renderW = rtSize.width;
                renderH = rtSize.height;
                break;

            case Constants::ViewModes::ViewMode::OriginalImageSize:
                // Raw 1:1 pixels, no bounds checking
                renderW = size.width;
                renderH = size.height;
                break;
        }

        // 3. Apply Zoom
        const float z = (g_app.viewport.zoom <= 0.0f) ? 1.0f : g_app.viewport.zoom;
        renderW *= z;
        renderH *= z;

        const float left = (rtSize.width - renderW) / 2.0f + g_app.viewport.offsetX;
        const float top = (rtSize.height - renderH) / 2.0f + g_app.viewport.offsetY;

        D2D1_MATRIX_3X2_F transform = D2D1::Matrix3x2F::Identity();
        if (g_app.viewport.flippedH) transform = transform * D2D1::Matrix3x2F::Scale(-1.0f, 1.0f, center);
        if (g_app.viewport.flippedV) transform = transform * D2D1::Matrix3x2F::Scale(1.0f, -1.0f, center);
        transform = transform * D2D1::Matrix3x2F::Rotation(static_cast<float>(g_app.viewport.rotation), center);

        bool isNative = (std::abs(g_app.viewport.zoom - 1.0f) < 0.001f);
        D2D1_BITMAP_INTERPOLATION_MODE interpMode = isNative
                                                        ? D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR
                                                        : D2D1_BITMAP_INTERPOLATION_MODE_LINEAR;

        m_pRenderTarget->SetTransform(transform);

        m_pRenderTarget->DrawBitmap(
                m_pBitmap.Get(),
                D2D1::RectF(left, top, left + renderW, top + renderH),
                1.0f,
                interpMode
                );

        m_pRenderTarget->SetTransform(D2D1::Matrix3x2F::Identity());

        if (!g_app.playlist.empty() && g_app.showOverlayInfoText) {
            std::wstring fullPath = g_app.playlist[g_app.currentIndex];
            std::wstring fileName = fullPath.substr(fullPath.find_last_of(L"\\/") + 1);
            std::wstring text = std::to_wstring(g_app.currentIndex + 1) + L" / " +
                                std::to_wstring(g_app.playlist.size()) + L" - " + fileName;
            D2D1_RECT_F layoutRect = D2D1::RectF(15.0f, 6.0f, rtSize.width - 10.0f, rtSize.height - 10.0f);

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
        hr = m_pFactory->CreateHwndRenderTarget(
                D2D1::RenderTargetProperties(),
                D2D1::HwndRenderTargetProperties(m_hwnd, D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top)),
                m_pRenderTarget.GetAddressOf()
                );

        if (SUCCEEDED(hr)) {
            m_pTextBrush.Reset();
            (void) m_pRenderTarget->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::LightGreen), &m_pTextBrush);
        }
    }

    return hr;
}
