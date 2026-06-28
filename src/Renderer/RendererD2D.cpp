#include "RendererD2D.h"
#include "../AppState.h"
#include "../Platform/Constants.h"
#include "../WorkerThread.h"
#include <algorithm>
#include <chrono>
#include <vector>
// Link the required import libraries
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dxguid.lib")

extern WorkerThread g_decoderWorker;
extern IoThreadPool g_ioWorker;

// =============================================================================
//  Initialize
// =============================================================================
HRESULT RendererD2D::Initialize(HWND hwnd) {
    m_hwnd = hwnd;

    // --- Device-independent factory (created once, never released on device loss) ---
    if (!m_pD2DFactory) {
        D2D1_FACTORY_OPTIONS opts{};
#ifdef _DEBUG
        opts.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif
        HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                       __uuidof(ID2D1Factory7),
                                       &opts,
                                       reinterpret_cast<void **>(m_pD2DFactory.GetAddressOf()));
        if (FAILED(hr)) return hr;
    }

    // --- DWrite factory (device-independent, created once) ---
    if (!m_pDWriteFactory) {
        HRESULT hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                         __uuidof(IDWriteFactory3),
                                         reinterpret_cast<IUnknown **>(m_pDWriteFactory.GetAddressOf()));
        if (FAILED(hr)) return hr;

        // Try Segoe UI first, fall back to Arial
        hr = m_pDWriteFactory->CreateTextFormat(
                L"Segoe UI", nullptr,
                DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                12.0f, L"en-us", &m_pTextFormat);

        if (FAILED(hr)) {
            (void) m_pDWriteFactory->CreateTextFormat(
                    L"Arial", nullptr,
                    DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                    12.0f, L"en-us", &m_pTextFormat);
        }
    }

    return CreateDeviceResources();
}

void RendererD2D::UpdateColorEffects() {
    if (!m_pSaturationEffect ||
        !m_pContrastEffect ||
        !m_pBrightnessEffect)
        return;
    m_pSaturationEffect->SetValue(
            D2D1_SATURATION_PROP_SATURATION,
            g_app.saturation);
    m_pContrastEffect->SetValue(
            D2D1_CONTRAST_PROP_CONTRAST,
            g_app.contrast);
    float b = g_app.brightness;
    m_pBrightnessEffect->SetValue(
            D2D1_BRIGHTNESS_PROP_WHITE_POINT,
            D2D1::Vector2F(
                    1.0f + b,
                    1.0f + b));
    m_pBrightnessEffect->SetValue(
            D2D1_BRIGHTNESS_PROP_BLACK_POINT,
            D2D1::Vector2F(
                    b < 0 ? -b : 0.0f,
                    b < 0 ? -b : 0.0f));
}

// =============================================================================
//  CreateDeviceResources  — builds D3D11 → DXGI → D2D7 chain
// =============================================================================
HRESULT RendererD2D::CreateDeviceResources() {
    // 1. Create D3D11 device with D2D interop flag
    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    UINT createFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    createFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL chosenLevel{};
    HRESULT hr = D3D11CreateDevice(
            nullptr, // default adapter
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            createFlags,
            featureLevels, ARRAYSIZE(featureLevels),
            D3D11_SDK_VERSION,
            m_pD3DDevice.GetAddressOf(),
            &chosenLevel,
            m_pD3DContext.GetAddressOf());

    if (FAILED(hr)) {
        // Retry without debug layer (SDK might not be installed)
        createFlags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                               createFlags, featureLevels, ARRAYSIZE(featureLevels),
                               D3D11_SDK_VERSION,
                               m_pD3DDevice.GetAddressOf(), &chosenLevel,
                               m_pD3DContext.GetAddressOf());
        if (FAILED(hr)) return hr;
    }

    // 2. Get the DXGI device from D3D11 device
    Microsoft::WRL::ComPtr<IDXGIDevice1> dxgiDevice;
    hr = m_pD3DDevice.As(&dxgiDevice);
    if (FAILED(hr)) return hr;

    // Limit pre-rendered frames to 1 to reduce input latency
    (void) dxgiDevice->SetMaximumFrameLatency(1);

    // 3. Get DXGI adapter → factory to create the swap chain
    Microsoft::WRL::ComPtr<IDXGIAdapter> dxgiAdapter;
    hr = dxgiDevice->GetAdapter(dxgiAdapter.GetAddressOf());
    if (FAILED(hr)) return hr;

    Microsoft::WRL::ComPtr<IDXGIFactory2> dxgiFactory;
    hr = dxgiAdapter->GetParent(__uuidof(IDXGIFactory2),
                                reinterpret_cast<void **>(dxgiFactory.GetAddressOf()));
    if (FAILED(hr)) return hr;

    // 4. Create DXGI swap chain for the HWND
    DXGI_SWAP_CHAIN_DESC1 swapDesc{};
    swapDesc.Width = 0; // auto from window
    swapDesc.Height = 0;
    swapDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; // required for D2D
    swapDesc.Stereo = FALSE;
    swapDesc.SampleDesc.Count = 1;
    swapDesc.SampleDesc.Quality = 0;
    swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapDesc.BufferCount = 2;
    swapDesc.Scaling = DXGI_SCALING_NONE;
    swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; // modern flip model
    swapDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    swapDesc.Flags = 0;

    hr = dxgiFactory->CreateSwapChainForHwnd(
            m_pD3DDevice.Get(), m_hwnd,
            &swapDesc, nullptr, nullptr,
            m_pSwapChain.GetAddressOf());
    if (FAILED(hr)) return hr;

    // Disable exclusive fullscreen transitions (we manage fullscreen ourselves)
    (void) dxgiFactory->MakeWindowAssociation(m_hwnd, DXGI_MWA_NO_ALT_ENTER);

    // 5. Create D2D device from the DXGI device
    hr = m_pD2DFactory->CreateDevice(dxgiDevice.Get(),
                                     reinterpret_cast<ID2D1Device **>(m_pD2DDevice.GetAddressOf()));
    if (FAILED(hr)) return hr;

    // 6. Create ID2D1DeviceContext7 from the D2D device
    hr = m_pD2DDevice->CreateDeviceContext(
            D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
            reinterpret_cast<ID2D1DeviceContext **>(m_pDeviceContext.GetAddressOf()));
    if (FAILED(hr)) return hr;

    // 7. Bind the swap chain back buffer as the D2D render target
    hr = CreateBackBufferBitmap();
    if (FAILED(hr)) return hr;

    // 8. Create text brush (device-dependent resource)
    hr = m_pDeviceContext->CreateSolidColorBrush(
            D2D1::ColorF(D2D1::ColorF::LightGreen),
            &m_pTextBrush);

    if (FAILED(hr))
        return hr;


    // 9. Create color adjustment effects
    hr = m_pDeviceContext->CreateEffect(
            CLSID_D2D1Saturation,
            &m_pSaturationEffect);

    if (FAILED(hr))
        return hr;


    hr = m_pDeviceContext->CreateEffect(
            CLSID_D2D1Contrast,
            &m_pContrastEffect);

    if (FAILED(hr))
        return hr;


    hr = m_pDeviceContext->CreateEffect(
            CLSID_D2D1Scale,
            &m_pScaleEffect);

    if (FAILED(hr))
        return hr;


    hr = m_pDeviceContext->CreateEffect(
            CLSID_D2D1Brightness,
            &m_pBrightnessEffect);

    if (FAILED(hr))
        return hr;


    // Initialize neutral effect values
    UpdateColorEffects();


    return S_OK;
}

// =============================================================================
//  CreateBackBufferBitmap  — wraps the DXGI back buffer in an ID2D1Bitmap1
// =============================================================================
HRESULT RendererD2D::CreateBackBufferBitmap() {
    m_pBackBufferBitmap.Reset();

    Microsoft::WRL::ComPtr<IDXGISurface> dxgiBackBuffer;
    HRESULT hr = m_pSwapChain->GetBuffer(0, __uuidof(IDXGISurface),
                                         reinterpret_cast<void **>(dxgiBackBuffer.GetAddressOf()));
    if (FAILED(hr)) return hr;

    D2D1_BITMAP_PROPERTIES1 bmpProps = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));

    hr = m_pDeviceContext->CreateBitmapFromDxgiSurface(
            dxgiBackBuffer.Get(), &bmpProps,
            m_pBackBufferBitmap.GetAddressOf());
    if (FAILED(hr)) return hr;

    m_pDeviceContext->SetTarget(m_pBackBufferBitmap.Get());
    return S_OK;
}

// =============================================================================
//  DiscardDeviceResources
// =============================================================================
void RendererD2D::DiscardDeviceResources() {
    // Clear the D2D target before releasing swap chain resources
    if (m_pDeviceContext) m_pDeviceContext->SetTarget(nullptr);

    m_pTextBrush.Reset();
    m_pBackBufferBitmap.Reset();
    m_pBitmap.Reset();
    m_bitmapCache.clear();
    m_lruList.clear();
    m_pDeviceContext.Reset();
    m_pD2DDevice.Reset();
    m_pSwapChain.Reset();
    m_pD3DContext.Reset();
    m_pD3DDevice.Reset();
    m_pSaturationEffect.Reset();
    m_pContrastEffect.Reset();
    m_pBrightnessEffect.Reset();
    m_pScaleEffect.Reset();
}

// =============================================================================
//  Resize
// =============================================================================
void RendererD2D::Resize(UINT width, UINT height) {
    if (!m_pSwapChain || !m_pDeviceContext) return;

    // Must clear the D2D target before resizing swap chain buffers
    m_pDeviceContext->SetTarget(nullptr);
    m_pBackBufferBitmap.Reset();

    HRESULT hr = m_pSwapChain->ResizeBuffers(0, width, height,
                                             DXGI_FORMAT_UNKNOWN, 0);
    if (SUCCEEDED(hr)) {
        (void) CreateBackBufferBitmap();
    }
}

// =============================================================================
//  UploadAndCacheBitmap  (thread-safe wrapper)
// =============================================================================
HRESULT RendererD2D::UploadAndCacheBitmap(const std::vector<BYTE> &pixelData, UINT stride,
                                          const std::wstring &filePath, UINT width, UINT height) {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    return UploadAndCacheBitmap_Locked(pixelData, stride, filePath, width, height);
}

// =============================================================================
//  UploadAndCacheBitmap_Locked  (caller must hold m_cacheMutex)
// =============================================================================
HRESULT RendererD2D::UploadAndCacheBitmap_Locked(const std::vector<BYTE> &pixelData, UINT stride,
                                                 const std::wstring &filePath, UINT width, UINT height) {
    if (!m_pDeviceContext || pixelData.empty()) return E_UNEXPECTED;

    Microsoft::WRL::ComPtr<ID2D1Bitmap1> targetBitmap;

    // 1. Check if the exact file is already cached (same path, reloading)
    auto existing = m_bitmapCache.find(filePath);
    if (existing != m_bitmapCache.end()) {
        if (existing->second.width == width && existing->second.height == height) {
            targetBitmap = existing->second.bitmap; // recycle the VRAM buffer
        }
        m_lruList.erase(existing->second.lruIt);
        m_bitmapCache.erase(existing);
    }

    // 2. LRU eviction if cache is full
    if (!targetBitmap && m_lruList.size() >= Constants::VRAM_CACHE_IMAGES_COUNT) {
        auto oldestIt = m_bitmapCache.find(m_lruList.back());
        if (oldestIt != m_bitmapCache.end()) {
            if (oldestIt->second.width == width && oldestIt->second.height == height) {
                targetBitmap = oldestIt->second.bitmap; // recycle if dimensions match
            }
            m_bitmapCache.erase(oldestIt);
        }
        m_lruList.pop_back();
    }

    HRESULT hr = S_OK;

    if (targetBitmap) {
        // FAST PATH: CopyFromMemory into existing VRAM buffer — zero allocation overhead
        D2D1_RECT_U destRect = D2D1::RectU(0, 0, width, height);
        hr = targetBitmap->CopyFromMemory(&destRect, pixelData.data(), stride);
    } else {
        // SLOW PATH: Allocate a new D2D bitmap on the GPU
        D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_NONE,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));

        hr = m_pDeviceContext->CreateBitmap(
                D2D1::SizeU(width, height),
                pixelData.data(),
                stride,
                props,
                targetBitmap.GetAddressOf());
    }

    if (FAILED(hr)) return hr;

    // 3. Register in the LRU cache
    m_lruList.push_front(filePath);
    m_bitmapCache.emplace(filePath, CachedBitmap{targetBitmap, m_lruList.begin(), width, height});
    return S_OK;
}

// =============================================================================
//  ProcessPendingUploads  — called from the UI thread
// =============================================================================
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
            HRESULT hr = UploadAndCacheBitmap(upload.pixelData, upload.stride,
                                              upload.filePath, upload.width, upload.height);
#ifdef _DEBUG
            if (FAILED(hr)) OutputDebugStringW(L"RAM to VRAM upload failed.\n");
#else
            (void) hr;
#endif
        }
    }
}

// =============================================================================
//  LoadBitmap
// =============================================================================
HRESULT RendererD2D::LoadBitmap(IWICBitmapSource *bitmap, UINT width, UINT height,
                                const std::wstring &filePath) {
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

    if (!bitmap || !m_pDeviceContext) return E_FAIL;

    // CreateBitmapFromWicBitmap on ID2D1DeviceContext produces an ID2D1Bitmap1
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> newBitmap;
    HRESULT hr = m_pDeviceContext->CreateBitmapFromWicBitmap(
            bitmap, nullptr,
            reinterpret_cast<ID2D1Bitmap **>(newBitmap.GetAddressOf()));

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

// =============================================================================
//  PreloadBitmap
// =============================================================================
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

            UINT stride = width * 4;
            std::vector<BYTE> pixelData(stride * height);

            if (FAILED(converter->CopyPixels(nullptr, stride,
                static_cast<UINT>(pixelData.size()),
                pixelData.data())))
                return;

            {
                std::lock_guard<std::mutex> lock(m_cacheMutex);
                m_pendingUploads.push({
                    requestIndex, filePath,
                    std::move(pixelData), stride, width, height
                });
            }
            PostMessageW(m_hwnd, Constants::WM_QIV_PENDING_UPLOADS, 0, 0);
        });
    });
    return S_OK;
}

// =============================================================================
//  Render
// =============================================================================
HRESULT RendererD2D::Render() {
    if (!m_pDeviceContext || !m_pSwapChain) return E_FAIL;

    m_pDeviceContext->BeginDraw();
    m_pDeviceContext->Clear(m_clearColor);

    if (m_pBitmap) {
        const D2D1_SIZE_F imgSize = m_pBitmap->GetSize();
        const D2D1_SIZE_F rtSize = m_pDeviceContext->GetSize();
        const D2D1_POINT_2F center = D2D1::Point2F(rtSize.width / 2.0f, rtSize.height / 2.0f);

        float ratioX = rtSize.width / imgSize.width;
        float ratioY = rtSize.height / imgSize.height;
        float renderW = imgSize.width;
        float renderH = imgSize.height;

        switch (g_app.viewMode) {
            case Constants::ViewModes::ViewMode::FitToView_PreserveAspectRatio:
                renderW = imgSize.width * std::min(ratioX, ratioY);
                renderH = imgSize.height * std::min(ratioX, ratioY);
                break;
            case Constants::ViewModes::ViewMode::FitToWidth_DoNotPreserveAspectRatio:
                renderW = rtSize.width;
                renderH = imgSize.height;
                if (renderH > rtSize.height) renderH = rtSize.height;
                break;
            case Constants::ViewModes::ViewMode::FitToHeight_DoNotPreserveAspectRatio:
                renderH = rtSize.height;
                renderW = imgSize.width;
                if (renderW > rtSize.width) renderW = rtSize.width;
                break;
            case Constants::ViewModes::ViewMode::FitToWindow_DoNotPreserveAspectRatio:
                renderW = rtSize.width;
                renderH = rtSize.height;
                break;
            case Constants::ViewModes::ViewMode::OriginalImageSize_PreserveAspectRatio:
                renderW = imgSize.width;
                renderH = imgSize.height;
                break;
        }

        const float z = (g_app.viewport.zoom <= 0.0f) ? 1.0f : g_app.viewport.zoom;
        renderW *= z;
        renderH *= z;

        const float left = (rtSize.width - renderW) / 2.0f + g_app.viewport.offsetX;
        const float top = (rtSize.height - renderH) / 2.0f + g_app.viewport.offsetY;

        D2D1_MATRIX_3X2_F transform = D2D1::Matrix3x2F::Identity();
        if (g_app.viewport.flippedH)
            transform = transform * D2D1::Matrix3x2F::Scale(-1.0f, 1.0f, center);
        if (g_app.viewport.flippedV)
            transform = transform * D2D1::Matrix3x2F::Scale(1.0f, -1.0f, center);
        transform = transform * D2D1::Matrix3x2F::Rotation(
                            static_cast<float>(g_app.viewport.rotation), center);

        m_pDeviceContext->SetTransform(transform);

        bool isNative = (std::abs(g_app.viewport.zoom - 1.0f) < 0.001f);
        D2D1_INTERPOLATION_MODE interpMode = isNative
                                                 ? D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR
                                                 : D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC;

        ID2D1Image *image = m_pBitmap.Get();
        bool useEffects =
                std::abs(g_app.saturation - 1.0f) > 0.001f ||
                std::abs(g_app.contrast - 1.0f) > 0.001f ||
                std::abs(g_app.brightness) > 0.001f;
        if (useEffects &&
            m_pSaturationEffect &&
            m_pContrastEffect &&
            m_pBrightnessEffect &&
            m_pScaleEffect) { // Added missing scale effect check
            m_pDeviceContext->SetTransform(D2D1::Matrix3x2F::Identity());
            m_pSaturationEffect->SetInput(0, image);
            m_pContrastEffect->SetInputEffect(0, m_pSaturationEffect.Get());
            m_pBrightnessEffect->SetInputEffect(0, m_pContrastEffect.Get());
            m_pScaleEffect->SetInputEffect(0, m_pBrightnessEffect.Get());
            m_pScaleEffect->SetValue(
                    D2D1_SCALE_PROP_SCALE,
                    D2D1::Vector2F(
                            renderW / imgSize.width,
                            renderH / imgSize.height));

            m_pScaleEffect->SetValue(
                    D2D1_SCALE_PROP_INTERPOLATION_MODE,
                    interpMode);
            D2D1_POINT_2F targetOffset = D2D1::Point2F(left, top);
            m_pDeviceContext->DrawImage(
                    m_pScaleEffect.Get(), // 1. The Effect
                    targetOffset, // 2. The Point (passed by value, NOT a pointer)
                    interpMode, // 3. Interpolation Mode
                    D2D1_COMPOSITE_MODE_SOURCE_OVER // 4. Composite Mode
                    );
        } else {
            m_pDeviceContext->DrawBitmap(
                    m_pBitmap.Get(),
                    D2D1::RectF(left, top, left + renderW, top + renderH),
                    1.0f,
                    interpMode);
        }

        m_pDeviceContext->SetTransform(D2D1::Matrix3x2F::Identity());

        if (!g_app.playlist.empty() && g_app.showOverlayInfoText) {
            std::wstring fullPath = g_app.playlist[g_app.currentIndex];
            std::wstring fileName = fullPath.substr(fullPath.find_last_of(L"\\/") + 1);
            std::wstring text = std::to_wstring(g_app.currentIndex + 1) + L" / " +
                                std::to_wstring(g_app.playlist.size()) + L" - " + fileName;
            D2D1_RECT_F layoutRect = D2D1::RectF(15.0f, 6.0f,
                                                 rtSize.width - 10.0f, rtSize.height - 10.0f);
            if (m_pTextFormat && m_pTextBrush) {
                m_pDeviceContext->DrawText(text.c_str(), static_cast<UINT32>(text.length()),
                                           m_pTextFormat.Get(), layoutRect, m_pTextBrush.Get());
            }
        }
    }

    HRESULT hr = m_pDeviceContext->EndDraw();

    if (hr == D2DERR_RECREATE_TARGET ||
        hr == static_cast<HRESULT>(DXGI_ERROR_DEVICE_REMOVED) ||
        hr == static_cast<HRESULT>(DXGI_ERROR_DEVICE_RESET)) {
        DiscardDeviceResources();
        hr = CreateDeviceResources();
        return hr;
    }

    if (FAILED(hr)) return hr;

    HRESULT hrPresent = m_pSwapChain->Present(0, 0);
    if (hrPresent == static_cast<HRESULT>(DXGI_ERROR_DEVICE_REMOVED) ||
        hrPresent == static_cast<HRESULT>(DXGI_ERROR_DEVICE_RESET)) {
        DiscardDeviceResources();
        (void) CreateDeviceResources();
    }

    return hr;
}
