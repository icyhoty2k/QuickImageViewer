#include "RendererD2D.h"
#include "../AppState.h"
#include "../Platform/Constants.h"
#include "../WorkerThread.h"
#include "../SvgDecoder.h"
#include <algorithm>
#include <chrono>
#include <vector>
#include <shlwapi.h>  // SHCreateMemStream
#include "../UI/CacheWindow.h"
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
    if (!m_pColorMatrixEffect)
        return;

    // ----- Saturation -----
    // g_app.saturation: 0.0 = grayscale, 1.0 = neutral, >1.0 = oversaturated.
    // No internal D2D cap to worry about here (unlike CLSID_D2D1Saturation,
    // which hard-clamps at 2.0) — push it as far as AppMain allows.
    const float s = g_app.saturation;
    constexpr float lumR = 0.2126f;
    constexpr float lumG = 0.7152f;
    constexpr float lumB = 0.0722f;

    // 3x3 saturation matrix: out[i] = lum[j]*(1-s) + (i==j ? s : 0)
    float sat[3][3] = {
        { lumR * (1.0f - s) + s, lumG * (1.0f - s),       lumB * (1.0f - s) },
        { lumR * (1.0f - s),     lumG * (1.0f - s) + s,   lumB * (1.0f - s) },
        { lumR * (1.0f - s),     lumG * (1.0f - s),       lumB * (1.0f - s) + s },
    };

    // ----- Contrast -----
    // g_app.contrast: 1.0 = neutral. out = (in - 0.5) * c + 0.5
    // Folded into the matrix by scaling every row of `sat` by c; the offset
    // picks up 0.5 * (1 - c). No internal cap, unlike CLSID_D2D1Contrast.
    const float c = g_app.contrast;
    float m[3][3];
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col)
            m[row][col] = sat[row][col] * c;

    // ----- Brightness -----
    // g_app.brightness: 0.0 = neutral, +1.0 = pure white, -1.0 = pure black.
    // Simple additive offset, GPU-clamped to [0,1] — this is the part that
    // replaced CLSID_D2D1Brightness, whose white/black point properties
    // accepted our SetValue calls (confirmed via debug log) but silently
    // produced no visible change.
    const float b = std::clamp(g_app.brightness, -1.0f, 1.0f);
    const float contrastOffset = 0.5f * (1.0f - c);
    const float offset = contrastOffset + b;

    D2D1_MATRIX_5X4_F matrix = D2D1::Matrix5x4F(
            m[0][0], m[1][0], m[2][0], 0.0f,
            m[0][1], m[1][1], m[2][1], 0.0f,
            m[0][2], m[1][2], m[2][2], 0.0f,
            0.0f,    0.0f,    0.0f,    1.0f,
            offset,  offset,  offset,  0.0f);

    m_pColorMatrixEffect->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX, matrix);
    m_pColorMatrixEffect->SetValue(D2D1_COLORMATRIX_PROP_CLAMP_OUTPUT, TRUE);

#ifdef _DEBUG
    {
        wchar_t buf[128];
        swprintf_s(buf, L"[QIV] brightness=%.3f saturation=%.3f contrast=%.3f\n", b, s, c);
        OutputDebugStringW(buf);
    }
#endif
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


    // 9. Create color adjustment effect — single ColorMatrix combining
    // saturation + contrast + brightness, computed explicitly in
    // UpdateColorEffects(). Replaces the old D2D1Saturation/D2D1Contrast/
    // D2D1Brightness chain: those built-in effects either clamp internally
    // (saturation maxes at 2.0) or didn't behave as documented (Brightness's
    // white/black point silently did nothing despite valid SetValue calls).
    // ColorMatrix is a plain linear transform + offset — fully predictable.
    hr = m_pDeviceContext->CreateEffect(
            CLSID_D2D1ColorMatrix,
            &m_pColorMatrixEffect);

    if (FAILED(hr))
        return hr;


    hr = m_pDeviceContext->CreateEffect(
            CLSID_D2D1Scale,
            &m_pScaleEffect);

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
    m_pColorMatrixEffect.Reset();
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
    bool isCacheHit = false;

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
            isCacheHit = true;
        }
    }

    if (isCacheHit) {
        // Fire callback safely outside the mutex lock
        if (onImageChangedCallback) {
            onImageChangedCallback(g_app.currentIndex);
        }
        return S_OK;
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
        {
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
        }

        m_pBitmap = newBitmap;

        // Clear any active SVG so the raster render path takes over
        m_pActiveSvg.Reset();
        m_svgNativeW = 0.0f;
        m_svgNativeH = 0.0f;

        g_app.imgWidth = static_cast<int>(width);
        g_app.imgHeight = static_cast<int>(height);

        // Fire callback safely outside the mutex lock
        if (onImageChangedCallback) {
            onImageChangedCallback(g_app.currentIndex);
        }
    }
    return hr;
}

// =============================================================================
//  PreloadBitmap
// =============================================================================
HRESULT RendererD2D::PreloadBitmap(const std::wstring &filePath, int requestIndex) {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    if (m_bitmapCache.find(filePath) != m_bitmapCache.end()) {
        return S_OK; // Already cached, ignore the request
    }
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
                auto it = m_bitmapCache.find(filePath);
                if (it != m_bitmapCache.end()) {
                    // Safely erase the old entry to prevent orphaned nodes in m_lruList
                    m_lruList.erase(it->second.lruIt);
                    m_bitmapCache.erase(it);
                }
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
        // g_app.contrast neutral = 1.0f, so check offset from 1.0
        bool useEffects =
                std::abs(g_app.saturation - 1.0f) > 0.001f ||
                std::abs(g_app.contrast - 1.0f) > 0.001f ||
                std::abs(g_app.brightness) > 0.001f;
        if (useEffects &&
            m_pColorMatrixEffect &&
            m_pScaleEffect) {
            // Do NOT reset transform here — flip/rotation is already set above.
            m_pColorMatrixEffect->SetInput(0, image);
            m_pScaleEffect->SetInputEffect(0, m_pColorMatrixEffect.Get());
            m_pScaleEffect->SetValue(
                    D2D1_SCALE_PROP_SCALE,
                    D2D1::Vector2F(
                            renderW / imgSize.width,
                            renderH / imgSize.height));

            // D2D1_SCALE_PROP_INTERPOLATION_MODE takes D2D1_SCALE_INTERPOLATION_MODE, not D2D1_INTERPOLATION_MODE
            D2D1_SCALE_INTERPOLATION_MODE scaleInterp = isNative
                                                            ? D2D1_SCALE_INTERPOLATION_MODE_NEAREST_NEIGHBOR
                                                            : D2D1_SCALE_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC;
            m_pScaleEffect->SetValue(D2D1_SCALE_PROP_INTERPOLATION_MODE, scaleInterp);
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

    bool isCacheHit = false;

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
            isCacheHit = true;
        }
    }

    if (isCacheHit) {
        // Fire callback safely outside the mutex lock
        if (onImageChangedCallback) {
            onImageChangedCallback(g_app.currentIndex);
        }
        return S_OK;
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

    // Fire callback safely outside the mutex lock
    if (onImageChangedCallback) {
        onImageChangedCallback(g_app.currentIndex);
    }

    return S_OK;
}

// =============================================================================
//  Cache Management
// =============================================================================
std::vector<IImageRenderer::CacheItem> RendererD2D::GetCachedBitmaps() {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    std::vector<CacheItem> items;
    items.reserve(m_lruList.size());
    for (const std::wstring &path: m_lruList) {
        auto it = m_bitmapCache.find(path);
        if (it != m_bitmapCache.end()) {
            items.push_back({it->first, it->second.bitmap.Get()});
        }
    }
    return items;
}

// 1. The parameter-less overload forwards an empty string
void RendererD2D::ClearCache() {
    ClearCache(L"");
}

void RendererD2D::ClearCache(const std::wstring &excludePath) {
    std::lock_guard<std::mutex> lock(m_cacheMutex);

    // 1. If no path is provided, nuke everything completely
    if (excludePath.empty()) {
        m_bitmapCache.clear();
        m_lruList.clear();
        m_svgCache.clear();
        m_svgLruList.clear();
        m_pBitmap.Reset();
        m_pActiveSvg.Reset();
        return;
    }

    // 2. Filter the Bitmap Cache
    auto bmpIt = m_bitmapCache.find(excludePath);
    bool foundBmp = (bmpIt != m_bitmapCache.end());
    CachedBitmap savedBmp;
    if (foundBmp) savedBmp = bmpIt->second; // Save it before wiping

    m_bitmapCache.clear();
    m_lruList.clear();

    if (foundBmp) {
        // Re-insert the active file and generate a fresh LRU iterator
        m_lruList.push_front(excludePath);
        savedBmp.lruIt = m_lruList.begin();
        m_bitmapCache[excludePath] = savedBmp;
    }

    // 3. Filter the SVG Cache
    auto svgIt = m_svgCache.find(excludePath);
    bool foundSvg = (svgIt != m_svgCache.end());
    CachedSvg savedSvg;
    if (foundSvg) savedSvg = svgIt->second; // Save it before wiping

    m_svgCache.clear();
    m_svgLruList.clear();

    if (foundSvg) {
        // Re-insert the active file and generate a fresh LRU iterator
        m_svgLruList.push_front(excludePath);
        savedSvg.lruIt = m_svgLruList.begin();
        m_svgCache[excludePath] = savedSvg;
    }

    // Notice we do NOT call m_pBitmap.Reset() or m_pActiveSvg.Reset() here.
    // This guarantees the image currently bound to the GPU stays visible on screen.
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

void RendererD2D::RenderCacheWindow(int selectedIndex, int hoverIndex) {
    if (!m_pCacheDeviceContext || !m_pCacheSwapChain) return;

    // Secure the cache access during rendering to prevent crashes if the
    // preloader background thread accesses it simultaneously.
    std::lock_guard<std::mutex> lock(m_cacheMutex);

    m_pCacheDeviceContext->BeginDraw();
    m_pCacheDeviceContext->Clear(D2D1::ColorF(0.08f, 0.08f, 0.08f, 1.0f));

    // Iterate through the UI layout objects
    for (size_t i = 0; i < UI::g_thumbnailObjects.size(); ++i) {
        const auto &thumb = UI::g_thumbnailObjects[i];

        // Check if the bitmap is available in the renderer's VRAM cache
        auto it = m_bitmapCache.find(thumb.filePath);

        if (it != m_bitmapCache.end() && it->second.bitmap) {
            // Bitmap found: Draw the real image
            m_pCacheDeviceContext->DrawBitmap(
                    it->second.bitmap.Get(),
                    thumb.rect,
                    1.0f,
                    D2D1_BITMAP_INTERPOLATION_MODE_LINEAR
                    );
        } else {
            // Bitmap missing: Draw a subtle placeholder box
            // This prevents the screen from going blank and shows the UI is active
            m_pCacheDeviceContext->FillRectangle(thumb.rect, m_pCacheButtonBrush.Get());

            // Optional: Trigger a background load if it's not already in progress
            // Ensure your Renderer has a mechanism to avoid spamming requests
            // PreloadBitmap(thumb.filePath, thumb.playlistIndex);
        }

        // Selection highlight
        if (static_cast<int>(i) == selectedIndex) {
            m_pCacheDeviceContext->DrawRectangle(thumb.rect, m_pCacheBorderBrush.Get(), Constants::CacheColors::SELECTION_BORDER_THICKNESS);
        }

        // Hover highlight
        if (static_cast<int>(i) == hoverIndex) {
            m_pCacheDeviceContext->DrawRectangle(thumb.rect, m_pCacheTextBrush.Get(), Constants::CacheColors::HOVER_THICKNESS);
        }
    }

    HRESULT hr = m_pCacheDeviceContext->EndDraw();

    // Handle device loss gracefully
    if (hr == D2DERR_RECREATE_TARGET || hr == static_cast<HRESULT>(DXGI_ERROR_DEVICE_REMOVED)) {
        // You would typically trigger a resource recreation here
    }

    m_pCacheSwapChain->Present(1, 0);
}

HRESULT RendererD2D::CreateCacheWindowDeviceResources(HWND hwnd) {
    m_hCacheWnd = hwnd;

    // 1. Create Swap Chain for the Cache Window
    DXGI_SWAP_CHAIN_DESC1 swapDesc{};
    swapDesc.Width = 0;
    swapDesc.Height = 0;
    swapDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    swapDesc.BufferCount = 2;
    swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapDesc.SampleDesc.Count = 1;

    Microsoft::WRL::ComPtr<IDXGIDevice1> dxgiDevice;
    m_pD3DDevice.As(&dxgiDevice);
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    dxgiDevice->GetAdapter(&adapter);
    Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
    adapter->GetParent(IID_PPV_ARGS(&factory));

    HRESULT hr = factory->CreateSwapChainForHwnd(m_pD3DDevice.Get(), hwnd, &swapDesc, nullptr, nullptr, &m_pCacheSwapChain);
    if (FAILED(hr)) return hr;

    // 2. Create D2D Device Context (Corrected initialization logic)
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> baseContext;
    hr = m_pD2DDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &baseContext);
    if (FAILED(hr)) return hr;

    // QueryInterface for the version your header uses (ID2D1DeviceContext7)
    hr = baseContext.As(&m_pCacheDeviceContext);
    if (FAILED(hr)) return hr;

    // 3. Bind Backbuffer
    RECT rc{};
    GetClientRect(hwnd, &rc);

    UINT w = static_cast<UINT>(rc.right - rc.left);
    UINT h = static_cast<UINT>(rc.bottom - rc.top);

    if (w == 0 || h == 0) {
        w = 800;
        h = Constants::CACHE_WINDOW_THICKNESS;
    }

    ResizeCacheWindow(w, h);
    // 4. Create Brushes
    m_pCacheDeviceContext->CreateSolidColorBrush(D2D1::ColorF(Constants::CacheColors::PLACEHOLDER), &m_pCacheButtonBrush);
    m_pCacheDeviceContext->CreateSolidColorBrush(D2D1::ColorF(Constants::CacheColors::SELECTION_BORDER), &m_pCacheBorderBrush);
    m_pCacheDeviceContext->CreateSolidColorBrush(D2D1::ColorF(Constants::CacheColors::HOVER), &m_pCacheTextBrush);

    return S_OK;
}

void RendererD2D::ResizeCacheWindow(UINT width, UINT height) {
    if (!m_pCacheSwapChain || !m_pCacheDeviceContext)
        return;
    m_pCacheDeviceContext->SetTarget(nullptr);
    m_pCacheBackBuffer.Reset();
    HRESULT hr = m_pCacheSwapChain->ResizeBuffers(
            0,
            width,
            height,
            DXGI_FORMAT_UNKNOWN,
            0);
    if (FAILED(hr))
        return;
    Microsoft::WRL::ComPtr<IDXGISurface> surface;
    hr = m_pCacheSwapChain->GetBuffer(
            0,
            IID_PPV_ARGS(&surface));
    if (FAILED(hr))
        return;
    D2D1_BITMAP_PROPERTIES1 props =
            D2D1::BitmapProperties1(
                    D2D1_BITMAP_OPTIONS_TARGET |
                    D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                    D2D1::PixelFormat(
                            DXGI_FORMAT_B8G8R8A8_UNORM,
                            D2D1_ALPHA_MODE_IGNORE));
    hr = m_pCacheDeviceContext->CreateBitmapFromDxgiSurface(
            surface.Get(),
            &props,
            &m_pCacheBackBuffer);
    if (FAILED(hr))
        return;
    m_pCacheDeviceContext->SetTarget(
            m_pCacheBackBuffer.Get());
}
