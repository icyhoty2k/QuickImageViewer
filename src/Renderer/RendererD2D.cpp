#include "RendererD2D.h"
#include "../AppState.h"
#include "../Platform/Constants.h"
#include "../WorkerThread.h"
#include "../SvgDecoder.h"
#include <algorithm>
#include <chrono>
#include <vector>
#include <shlwapi.h>  // SHCreateMemStream
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
                                         __uuidof(IDWriteFactory7),
                                         reinterpret_cast<IUnknown **>(m_pDWriteFactory.GetAddressOf()));
        if (FAILED(hr)) return hr;
        UpdateTextFormat();
    }

    return CreateDeviceResources();
}

void RendererD2D::UpdateTextFormat() {
    float scaledFontSize = 14.0f * g_app.dpiScale;

    m_pTextFormat.Reset();
    HRESULT hr = m_pDWriteFactory->CreateTextFormat(
            L"Segoe UI", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            scaledFontSize, L"en-us", &m_pTextFormat);

    // Fallback
    if (FAILED(hr)) {
        (void) m_pDWriteFactory->CreateTextFormat(
                L"Arial", nullptr,
                DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                scaledFontSize, L"en-us", &m_pTextFormat);
    }
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
            std::clamp(g_app.contrast, 0.0f, 2.0f));


    float b = std::clamp(g_app.brightness, -1.0f, 1.0f);

    // smooth brightness
    float white = 1.0f + (b * 0.5f);
    float black = (b < 0.0f) ? (-b * 0.5f) : 0.0f;

    m_pBrightnessEffect->SetValue(
            D2D1_BRIGHTNESS_PROP_WHITE_POINT,
            D2D1::Vector2F(white, white));

    m_pBrightnessEffect->SetValue(
            D2D1_BRIGHTNESS_PROP_BLACK_POINT,
            D2D1::Vector2F(black, black));
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
    hr = dxgiAdapter->GetParent(IID_PPV_ARGS(&dxgiFactory));
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
            &m_pSwapChain);
    if (FAILED(hr)) return hr;

    // Disable exclusive fullscreen transitions (we manage fullscreen ourselves)
    (void) dxgiFactory->MakeWindowAssociation(m_hwnd, DXGI_MWA_NO_ALT_ENTER);

    // 5. Create D2D device from the DXGI device
    hr = m_pD2DFactory->CreateDevice(dxgiDevice.Get(),
                                     &m_pD2DDevice);
    if (FAILED(hr)) return hr;

    // 6. Create device context, then QI up to ID2D1DeviceContext7.
    //    CreateDeviceContext on ID2D1Device6 tops out at DeviceContext6,
    //    so we create into the base interface first, then QueryInterface.
    {
        Microsoft::WRL::ComPtr<ID2D1DeviceContext> baseDC;
        hr = m_pD2DDevice->CreateDeviceContext(
                D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
                baseDC.GetAddressOf());
        if (FAILED(hr)) return hr;
        hr = baseDC.As(&m_pDeviceContext);
        if (FAILED(hr)) return hr;
    }

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
    HRESULT hr = m_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&dxgiBackBuffer));
    if (FAILED(hr)) return hr;

    D2D1_BITMAP_PROPERTIES1 bmpProps = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));

    hr = m_pDeviceContext->CreateBitmapFromDxgiSurface(
            dxgiBackBuffer.Get(), &bmpProps,
            &m_pBackBufferBitmap);
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
    // SVG documents are device-dependent – discard them too
    m_svgCache.clear();
    m_svgLruList.clear();
    m_pActiveSvg.Reset();
    m_svgNativeW = 0.0f;
    m_svgNativeH = 0.0f;
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
            // Clear any active SVG so the raster path takes over
            m_pActiveSvg.Reset();
            m_svgNativeW = 0.0f;
            m_svgNativeH = 0.0f;
            g_app.imgWidth = static_cast<int>(it->second.width);
            g_app.imgHeight = static_cast<int>(it->second.height);
            return S_OK;
        }
    }

    if (!bitmap || !m_pD2DDevice) return E_FAIL;

    // Use a temporary device context to create the bitmap from the WIC source.
    // This allows the bitmap to be created on the device rather than being tied
    // strictly to the UI thread's current context state.
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> tempCtx;
    HRESULT hr = m_pD2DDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &tempCtx);
    if (FAILED(hr)) return hr;

    Microsoft::WRL::ComPtr<ID2D1Bitmap1> newBitmap;
    hr = tempCtx->CreateBitmapFromWicBitmap(
            bitmap, nullptr,
            &newBitmap);

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

        // Clear any active SVG so the raster render path takes over
        m_pActiveSvg.Reset();
        m_svgNativeW = 0.0f;
        m_svgNativeH = 0.0f;

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
    if (!wicFac || !m_pD2DDevice) return E_UNEXPECTED;

    // Capture the D2D device for the background thread
    Microsoft::WRL::ComPtr<ID2D1Device6> d2dDevice = m_pD2DDevice;

    g_ioWorker.PushTask([filePath, requestIndex, wicFac, d2dDevice, this]() {
        if (g_app.wantedIndex.load(std::memory_order_acquire) != requestIndex) return;

        // Read compressed file bytes on the IO thread
        HANDLE hFile = CreateFileW(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile == INVALID_HANDLE_VALUE) return;

        LARGE_INTEGER fileSize;
        if (!GetFileSizeEx(hFile, &fileSize)) {
            CloseHandle(hFile);
            return;
        }

        std::vector<BYTE> compressedBytes(fileSize.QuadPart);
        DWORD bytesRead;
        if (!ReadFile(hFile, compressedBytes.data(), static_cast<DWORD>(fileSize.QuadPart), &bytesRead, NULL) || bytesRead != fileSize.QuadPart) {
            CloseHandle(hFile);
            return;
        }
        CloseHandle(hFile);

        g_decoderWorker.PushTask([compressedBytes = std::move(compressedBytes), filePath, requestIndex, wicFac, d2dDevice, this]() mutable {
            if (g_app.wantedIndex.load(std::memory_order_acquire) != requestIndex) return;

            // Create a decoder from the compressed memory stream
            Microsoft::WRL::ComPtr<IWICStream> wicStream;
            if (FAILED(wicFac->CreateStream(&wicStream))) return;
            if (FAILED(wicStream->InitializeFromMemory(compressedBytes.data(), static_cast<DWORD>(compressedBytes.size())))) return;

            Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
            if (FAILED(wicFac->CreateDecoderFromStream(wicStream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder))) return;

            Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
            if (FAILED(decoder->GetFrame(0, &frame))) return;

            UINT width = 0, height = 0;
            frame->GetSize(&width, &height);

            Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
            if (FAILED(wicFac->CreateFormatConverter(&converter))) return;

            if (FAILED(converter->Initialize(
                frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                WICBitmapDitherTypeNone, nullptr, 0.0f,
                WICBitmapPaletteTypeCustom)))
                return;

            // This is the new direct-to-VRAM path
            Microsoft::WRL::ComPtr<ID2D1DeviceContext> tempCtx;
            if (FAILED(d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &tempCtx))) return;

            Microsoft::WRL::ComPtr<ID2D1Bitmap1> newBitmap;
            HRESULT hr = tempCtx->CreateBitmapFromWicBitmap(converter.Get(), nullptr, &newBitmap);
            if (FAILED(hr)) return;

            {
                std::lock_guard<std::mutex> lock(m_cacheMutex);
                if (m_lruList.size() >= Constants::VRAM_CACHE_IMAGES_COUNT) {
                    m_bitmapCache.erase(m_lruList.back());
                    m_lruList.pop_back();
                }
                m_lruList.push_front(filePath);
                m_bitmapCache[filePath] = {newBitmap, m_lruList.begin(), width, height};
            }

            if (g_app.wantedIndex.load(std::memory_order_acquire) == requestIndex) {
                PostMessageW(m_hwnd, Constants::WM_QIV_REPAINT, 0, 0);
            }
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

    // =========================================================================
    //  SVG path  (mutually exclusive with the raster bitmap path below)
    // =========================================================================
    if (m_pActiveSvg) {
        const D2D1_SIZE_F rtSize = m_pDeviceContext->GetSize();

        // m_svgNativeW/H is the intrinsic SVG size in logical pixels.
        // If we couldn't read it, fall back to treating the SVG as square-ish
        // using the window dimensions.
        const float imgW = (m_svgNativeW > 1.0f) ? m_svgNativeW : rtSize.width;
        const float imgH = (m_svgNativeH > 1.0f) ? m_svgNativeH : rtSize.height;

        // Compute scaled render size (same logic as the raster path)
        float renderW = imgW;
        float renderH = imgH;
        const float ratioX = rtSize.width / imgW;
        const float ratioY = rtSize.height / imgH;

        switch (g_app.viewMode) {
            case Constants::ViewModes::ViewMode::FitToView_PreserveAspectRatio:
                renderW = imgW * std::min(ratioX, ratioY);
                renderH = imgH * std::min(ratioX, ratioY);
                break;
            case Constants::ViewModes::ViewMode::FitToWidth_DoNotPreserveAspectRatio:
                renderW = rtSize.width;
                renderH = imgH;
                if (renderH > rtSize.height) renderH = rtSize.height;
                break;
            case Constants::ViewModes::ViewMode::FitToHeight_DoNotPreserveAspectRatio:
                renderH = rtSize.height;
                renderW = imgW;
                if (renderW > rtSize.width) renderW = rtSize.width;
                break;
            case Constants::ViewModes::ViewMode::FitToWindow_DoNotPreserveAspectRatio:
                renderW = rtSize.width;
                renderH = rtSize.height;
                break;
            case Constants::ViewModes::ViewMode::OriginalImageSize_PreserveAspectRatio:
                renderW = imgW;
                renderH = imgH;
                break;
        }

        const float z = (g_app.viewport.zoom <= 0.0f) ? 1.0f : g_app.viewport.zoom;
        renderW *= z;
        renderH *= z;

        const float left = (rtSize.width - renderW) / 2.0f + g_app.viewport.offsetX;
        const float top = (rtSize.height - renderH) / 2.0f + g_app.viewport.offsetY;

        // DrawSvgDocument always draws at (0,0) in the DC coordinate system,
        // sized to whatever SetViewportSize says.  We position/rotate via the
        // DC world transform:
        //
        //   1. Scale: viewport = (renderW, renderH) handles this already –
        //      D2D scales the SVG tree to fill that rectangle.
        //   2. Translate to (left, top).
        //   3. Rotate / flip around the screen centre.
        //
        // Transform order (right-to-left):  T * R * F
        // We apply them left-to-right using the matrix multiply convention
        // where the last multiplied matrix is applied first in screen space.

        // Tell D2D how large to rasterise the SVG
        m_pActiveSvg->SetViewportSize(D2D1::SizeF(renderW, renderH));

        const D2D1_POINT_2F screenCenter =
                D2D1::Point2F(rtSize.width / 2.0f, rtSize.height / 2.0f);

        // Start with translation to the target top-left corner
        D2D1_MATRIX_3X2_F transform =
                D2D1::Matrix3x2F::Translation(left, top);

        // Then apply flip / rotation around the screen centre
        // (same order as the raster path)
        if (g_app.viewport.flippedH)
            transform = transform * D2D1::Matrix3x2F::Scale(-1.0f, 1.0f, screenCenter);
        if (g_app.viewport.flippedV)
            transform = transform * D2D1::Matrix3x2F::Scale(1.0f, -1.0f, screenCenter);
        if (g_app.viewport.rotation != 0)
            transform = transform * D2D1::Matrix3x2F::Rotation(
                                static_cast<float>(g_app.viewport.rotation), screenCenter);

        m_pDeviceContext->SetTransform(transform);

        // QI for DrawSvgDocument (requires ID2D1DeviceContext5)
        Microsoft::WRL::ComPtr<ID2D1DeviceContext5> ctx5;
        if (SUCCEEDED(m_pDeviceContext.As(&ctx5))) {
            ctx5->DrawSvgDocument(m_pActiveSvg.Get());
        }

        m_pDeviceContext->SetTransform(D2D1::Matrix3x2F::Identity());

        // Overlay text (same as raster path)
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
    } else if (m_pBitmap) {
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

// =============================================================================
//  ClearActiveImage  — drops current raster bitmap and SVG so next Render()
//  shows a blank frame instead of a stale image during async load.
// =============================================================================
void RendererD2D::ClearActiveImage() {
    m_pBitmap.Reset();
    m_pActiveSvg.Reset();
    m_svgNativeW = 0.0f;
    m_svgNativeH = 0.0f;
}

// =============================================================================
//  LoadSvgFromBytes  — parses SVG XML into an ID2D1SvgDocument on the UI thread
// =============================================================================
// Called from FileHandler (UI thread) after SvgDecoder::LoadFile() delivers
// the raw bytes.  ID2D1SvgDocument can only be created on the device context
// thread, so there is no background-thread path for SVG (unlike raster images).
//
// The document is kept in m_svgCache (simple unordered_map, no LRU needed –
// SVGs are tiny).  m_pActiveSvg is set so Render() draws it instead of
// m_pBitmap.  m_pBitmap is cleared so the raster path stays idle.
// =============================================================================
HRESULT RendererD2D::LoadSvgFromBytes(const std::vector<BYTE> &svgBytes,
                                      const std::wstring &filePath) {
    if (!m_pDeviceContext) return E_UNEXPECTED;
    if (svgBytes.empty()) return E_INVALIDARG;

    // Clear the raster bitmap immediately so we never draw a stale image
    // while the SVG is being set up, even if something fails below.
    m_pBitmap.Reset();
    m_pActiveSvg.Reset();

    // --- 1. Check SVG cache first ---
    {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        auto it = m_svgCache.find(filePath);
        if (it != m_svgCache.end()) {
            m_svgLruList.splice(m_svgLruList.begin(), m_svgLruList, it->second.lruIt);
            m_pActiveSvg = it->second.document;
            m_svgNativeW = it->second.viewportW;
            m_svgNativeH = it->second.viewportH;
            m_pBitmap.Reset(); // raster path must be idle
            g_app.imgWidth = static_cast<int>(m_svgNativeW);
            g_app.imgHeight = static_cast<int>(m_svgNativeH);
            return S_OK;
        }
    }

    // --- 2. QI for ID2D1DeviceContext5 (needed for CreateSvgDocument) ---
    Microsoft::WRL::ComPtr<ID2D1DeviceContext5> ctx5;
    HRESULT hr = m_pDeviceContext.As(&ctx5);
    if (FAILED(hr)) return hr; // ID2D1DeviceContext5 requires Win10 1703+

    // --- 3. Wrap raw bytes in an IStream ---
    IStream *pStream = SHCreateMemStream(svgBytes.data(),
                                         static_cast<UINT>(svgBytes.size()));
    if (!pStream) return E_OUTOFMEMORY;

    // --- 4. Use the current window size as initial viewport.
    //        D2D will override this with the SVG's own viewBox/width/height. ---
    D2D1_SIZE_F rtSize = m_pDeviceContext->GetSize();
    D2D1_SIZE_F viewport = (rtSize.width > 0 && rtSize.height > 0)
                               ? rtSize
                               : D2D1::SizeF(1920.0f, 1080.0f);

    Microsoft::WRL::ComPtr<ID2D1SvgDocument> svgDoc;
    hr = ctx5->CreateSvgDocument(pStream, viewport, svgDoc.GetAddressOf());
    pStream->Release();
    if (FAILED(hr)) return hr;

    // --- 5. Read back intrinsic size from the SVG root element ---
    // Strategy:
    //   a) Try viewBox first – its values are always in user units (pixels).
    //   b) Fall back to width/height attributes only when they are in plain
    //      pixel units (D2D1_SVG_LENGTH_UNITS_NUMBER), not percentages.
    //   c) If nothing works, use the viewport size we passed to CreateSvgDocument.
    float nativeW = 0.0f;
    float nativeH = 0.0f;

    Microsoft::WRL::ComPtr<ID2D1SvgElement> root;
    svgDoc->GetRoot(root.GetAddressOf());
    if (root) {
        // (a) viewBox  — most reliable, always in user units
        D2D1_SVG_VIEWBOX vb{};
        if (SUCCEEDED(root->GetAttributeValue(
            L"viewBox",
            D2D1_SVG_ATTRIBUTE_POD_TYPE_VIEWBOX,
            &vb,
            sizeof(vb)))) {
            if (vb.width > 0.0f && vb.height > 0.0f) {
                nativeW = vb.width;
                nativeH = vb.height;
            }
        }

        // (b) width / height – only trust them when units == NUMBER (plain px)
        if (nativeW <= 0.0f || nativeH <= 0.0f) {
            D2D1_SVG_LENGTH wLen{}, hLen{};
            bool wOk = SUCCEEDED(root->GetAttributeValue(L"width", &wLen));
            bool hOk = SUCCEEDED(root->GetAttributeValue(L"height", &hLen));

            if (wOk && hOk &&
                wLen.units == D2D1_SVG_LENGTH_UNITS_NUMBER &&
                hLen.units == D2D1_SVG_LENGTH_UNITS_NUMBER &&
                wLen.value > 0.0f && hLen.value > 0.0f) {
                nativeW = wLen.value;
                nativeH = hLen.value;
            }
        }
    }

    // (c) Last-resort: use the viewport size we gave CreateSvgDocument
    if (nativeW <= 0.0f) nativeW = viewport.width;
    if (nativeH <= 0.0f) nativeH = viewport.height;

    // --- 6. Cache it ---
    {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        if (m_svgLruList.size() >= Constants::VRAM_CACHE_SVG_COUNT) {
            m_svgCache.erase(m_svgLruList.back());
            m_svgLruList.pop_back();
        }
        m_svgLruList.push_front(filePath);
        m_svgCache[filePath] = CachedSvg{svgDoc, m_svgLruList.begin(), nativeW, nativeH};
    }

    // --- 7. Make it active ---
    m_pActiveSvg = svgDoc;
    m_svgNativeW = nativeW;
    m_svgNativeH = nativeH;
    m_pBitmap.Reset(); // clear any previous raster
    g_app.imgWidth = static_cast<int>(nativeW);
    g_app.imgHeight = static_cast<int>(nativeH);

    return S_OK;
}

// =============================================================================
//  Cache Management
// =============================================================================
std::vector<IImageRenderer::CacheItem> RendererD2D::GetCachedBitmaps() {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    std::vector<CacheItem> items;
    items.reserve(m_bitmapCache.size());
    for (const auto &pair: m_bitmapCache) {
        items.push_back({pair.first, pair.second.bitmap.Get()});
    }
    return items;
}

void RendererD2D::ClearCache() {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    m_bitmapCache.clear();
    m_lruList.clear();
    m_svgCache.clear();
    m_svgLruList.clear();
    // Also clear the active pointers
    m_pBitmap.Reset();
    m_pActiveSvg.Reset();
}

void RendererD2D::RemoveFromCache(const std::wstring &filePath) {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    auto it = m_bitmapCache.find(filePath);
    if (it != m_bitmapCache.end()) {
        m_lruList.erase(it->second.lruIt);
        m_bitmapCache.erase(it);
    }
    auto svg_it = m_svgCache.find(filePath);
    if (svg_it != m_svgCache.end()) {
        m_svgLruList.erase(svg_it->second.lruIt);
        m_svgCache.erase(svg_it);
    }
}

// =============================================================================
//  Cache Window
// =============================================================================

void RendererD2D::CreateCacheWindowDeviceResources(HWND hwnd) {
    if (m_pCacheSwapChain) return;

    Microsoft::WRL::ComPtr<IDXGIDevice1> dxgiDevice;
    m_pD3DDevice.As(&dxgiDevice);
    Microsoft::WRL::ComPtr<IDXGIAdapter> dxgiAdapter;
    dxgiDevice->GetAdapter(&dxgiAdapter);
    Microsoft::WRL::ComPtr<IDXGIFactory2> dxgiFactory;
    dxgiAdapter->GetParent(IID_PPV_ARGS(&dxgiFactory));

    DXGI_SWAP_CHAIN_DESC1 swapDesc{};
    swapDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    swapDesc.SampleDesc.Count = 1;
    swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapDesc.BufferCount = 2;
    swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    dxgiFactory->CreateSwapChainForHwnd(m_pD3DDevice.Get(), hwnd, &swapDesc, nullptr, nullptr, &m_pCacheSwapChain);

    // QI up to ID2D1DeviceContext7 via the base interface (same pattern as main context)
    {
        Microsoft::WRL::ComPtr<ID2D1DeviceContext> baseDC;
        m_pD2DDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, baseDC.GetAddressOf());
        if (baseDC) baseDC.As(&m_pCacheDeviceContext);
    }

    if (m_pCacheDeviceContext) {
        m_pCacheDeviceContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &m_pCacheTextBrush);
        m_pDWriteFactory->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"en-us", &m_pCacheTextFormat);
        m_pCacheDeviceContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Green), &m_pCacheBorderBrush);
    }

    ResizeCacheWindow(0, 0); // Create initial back buffer
}

void RendererD2D::DiscardCacheWindowDeviceResources() {
    m_pCacheSwapChain.Reset();
    m_pCacheBackBuffer.Reset();
    m_pCacheTextBrush.Reset();
    m_pCacheBorderBrush.Reset();
    m_pCacheTextFormat.Reset();
    m_pCacheDeviceContext.Reset();
}

void RendererD2D::RenderCacheWindow(int selectedIndex) {
    if (!m_pCacheDeviceContext || !m_pCacheSwapChain) return;

    m_pCacheDeviceContext->BeginDraw();
    m_pCacheDeviceContext->Clear(D2D1::ColorF(0.1f, 0.1f, 0.1f));

    auto items = GetCachedBitmaps();
    float x = 10.0f, y = 50.0f;
    float thumbWidth = 120.0f, thumbHeight = 90.0f;
    float padding = 10.0f;
    D2D1_SIZE_F rtSize = m_pCacheDeviceContext->GetSize();

    for (size_t i = 0; i < items.size(); ++i) {
        D2D1_RECT_F thumbRect = D2D1::RectF(x, y, x + thumbWidth, y + thumbHeight);
        if (items[i].bitmap) {
            m_pCacheDeviceContext->DrawBitmap(items[i].bitmap, thumbRect);
        }

        if (i == selectedIndex) {
            m_pCacheDeviceContext->DrawRectangle(&thumbRect, m_pCacheBorderBrush.Get(), 2.0f);
        }

        std::wstring fileName = items[i].filePath.substr(items[i].filePath.find_last_of(L"\\/") + 1);
        D2D1_RECT_F textRect = D2D1::RectF(x, y + thumbHeight, x + thumbWidth, y + thumbHeight + 30);
        if (m_pCacheTextFormat && m_pCacheTextBrush) {
            m_pCacheDeviceContext->DrawTextW(fileName.c_str(), (UINT32) fileName.length(), m_pCacheTextFormat.Get(), textRect, m_pCacheTextBrush.Get());
        }

        x += thumbWidth + padding;
        if (x + thumbWidth > rtSize.width - padding) {
            x = 10.0f;
            y += thumbHeight + padding + 30.0f;
        }
    }

    m_pCacheDeviceContext->EndDraw();
    m_pCacheSwapChain->Present(1, 0);
}

void RendererD2D::ResizeCacheWindow(UINT width, UINT height) {
    if (m_pCacheSwapChain) {
        m_pCacheDeviceContext->SetTarget(nullptr);
        m_pCacheBackBuffer.Reset();
        m_pCacheSwapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);

        Microsoft::WRL::ComPtr<IDXGISurface> dxgiBackBuffer;
        m_pCacheSwapChain->GetBuffer(0, IID_PPV_ARGS(&dxgiBackBuffer));

        D2D1_BITMAP_PROPERTIES1 bmpProps = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE)
                );
        m_pCacheDeviceContext->CreateBitmapFromDxgiSurface(dxgiBackBuffer.Get(), &bmpProps, &m_pCacheBackBuffer);
        m_pCacheDeviceContext->SetTarget(m_pCacheBackBuffer.Get());
    }
}
