#include "RendererD2D.h"
#include "../Overlays/OverlayManager.h"
#include "../AppState.h"
#include "../Platform/Constants.h"
#include "../WorkerThread.h"
#include "../SvgDecoder.h"
#include <algorithm>
#include <chrono>
#include <vector>
#include <shlwapi.h>  // SHCreateMemStream

#include "DirWnd.h"
#include "../UI/CacheWnd.h"
// Link the required import libraries
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dxguid.lib")

extern WorkerThread g_decoderWorker;
extern IoThreadPool g_ioWorker;
extern WorkerThread g_dirThumbWorker;

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

    HRESULT hr = CreateDeviceResources();
    if (SUCCEEDED(hr)) {
        // Initialize to empty/default state
        m_pActiveDisplayNode = nullptr;
        // Hand text resources to the overlay manager — it does not own them
        g_overlayManager.Init(m_pTextFormat.Get(), m_pTextBrush.Get());
    }
    return hr;
}

void RendererD2D::UpdateTextFormat() {
    float scaledFontSize = 14.0f * app.dpiScale;

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
    if (!m_pColorMatrixEffect) return;
    (void) EnsureExtraEffects();
    // 1. Check BOTH booleans. If effects are active AND the preview toggle is on...
    if (app.hasActiveEffects && app.effectPreviewEnabled && m_pBitmap) {
        // Always rebuild the chain unconditionally.
        ApplyPreviousEffects();
    } else {
        // Safe bypass: no effects active OR preview toggled off.
        m_pActiveDisplayNode = m_pBitmap; // FAST PATH
    }
    // Non-linear effect nodes are created lazily here
    (void) EnsureExtraEffects();

    constexpr float lumR = 0.2126f;
    constexpr float lumG = 0.7152f;
    constexpr float lumB = 0.0722f;

    // ----- Base matrix: Sepia (fixed tone matrix) OR Saturation/Grayscale -----
    float base[3][3];
    if (app.effectSepia) {
        base[0][0] = 0.393f;
        base[0][1] = 0.769f;
        base[0][2] = 0.189f;
        base[1][0] = 0.349f;
        base[1][1] = 0.686f;
        base[1][2] = 0.168f;
        base[2][0] = 0.272f;
        base[2][1] = 0.534f;
        base[2][2] = 0.131f;
    } else {
        const float s = app.effectGrayscale ? 0.0f : app.saturation;
        base[0][0] = lumR * (1.0f - s) + s;
        base[0][1] = lumG * (1.0f - s);
        base[0][2] = lumB * (1.0f - s);
        base[1][0] = lumR * (1.0f - s);
        base[1][1] = lumG * (1.0f - s) + s;
        base[1][2] = lumB * (1.0f - s);
        base[2][0] = lumR * (1.0f - s);
        base[2][1] = lumG * (1.0f - s);
        base[2][2] = lumB * (1.0f - s) + s;
    }

    // ----- Contrast -----
    const float c = app.contrast;
    float m[3][3];
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col)
            m[row][col] = base[row][col] * c;

    // ----- Brightness -----
    const float b = std::clamp(app.brightness, -1.0f, 1.0f);
    const float contrastOffset = 0.5f * (1.0f - c);
    float offset = contrastOffset + b;

    // ----- Invert -----
    if (app.effectInvert) {
        for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 3; ++col)
                m[row][col] = -m[row][col];
        offset = 1.0f - offset;
    }

    D2D1_MATRIX_5X4_F matrix = D2D1::Matrix5x4F(
            m[0][0], m[1][0], m[2][0], 0.0f,
            m[0][1], m[1][1], m[2][1], 0.0f,
            m[0][2], m[1][2], m[2][2], 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f,
            offset, offset, offset, 0.0f);

    m_pColorMatrixEffect->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX, matrix);
    m_pColorMatrixEffect->SetValue(D2D1_COLORMATRIX_PROP_CLAMP_OUTPUT, TRUE);

    // ----- Gamma -----
    if (m_pGammaEffect) {
        const float g = (app.gamma <= 0.01f) ? 0.01f : app.gamma;
        const float exponent = 1.0f / g;
        m_pGammaEffect->SetValue(D2D1_GAMMATRANSFER_PROP_RED_EXPONENT, exponent);
        m_pGammaEffect->SetValue(D2D1_GAMMATRANSFER_PROP_GREEN_EXPONENT, exponent);
        m_pGammaEffect->SetValue(D2D1_GAMMATRANSFER_PROP_BLUE_EXPONENT, exponent);
    }

#ifdef _DEBUG
    {
        wchar_t buf[160];
        swprintf_s(buf, L"[QIV] brightness=%.3f saturation=%.3f contrast=%.3f gamma=%.3f gray=%d inv=%d sepia=%d sol=%d out=%d thr=%d\n",
                   b, app.saturation, c, app.gamma,
                   app.effectGrayscale, app.effectInvert, app.effectSepia,
                   app.effectSolarize, app.effectOutline, app.effectThreshold);
        OutputDebugStringW(buf);
    }
#endif
}

// =============================================================================
//  EnsureExtraEffects
// =============================================================================
HRESULT RendererD2D::EnsureExtraEffects() {
    if (!m_pDeviceContext) return E_FAIL;
    HRESULT hr = S_OK;

    if (!m_pGammaEffect) {
        hr = m_pDeviceContext->CreateEffect(CLSID_D2D1GammaTransfer, &m_pGammaEffect);
        if (FAILED(hr)) return hr;
        m_pGammaEffect->SetValue(D2D1_GAMMATRANSFER_PROP_ALPHA_DISABLE, TRUE);
    }

    if (!m_pSolarizeEffect) {
        hr = m_pDeviceContext->CreateEffect(CLSID_D2D1TableTransfer, &m_pSolarizeEffect);
        if (FAILED(hr)) return hr;

        std::vector<float> lut(256);
        for (int i = 0; i < 256; ++i) {
            const float v = static_cast<float>(i) / 255.0f;
            lut[i] = (v > Constants::SOLARIZE_THRESHOLD) ? (1.0f - v) : v;
        }
        const auto *lutBytes = reinterpret_cast<const BYTE *>(lut.data());
        const UINT32 lutSize = static_cast<UINT32>(lut.size() * sizeof(float));
        m_pSolarizeEffect->SetValue(D2D1_TABLETRANSFER_PROP_RED_TABLE, lutBytes, lutSize);
        m_pSolarizeEffect->SetValue(D2D1_TABLETRANSFER_PROP_GREEN_TABLE, lutBytes, lutSize);
        m_pSolarizeEffect->SetValue(D2D1_TABLETRANSFER_PROP_BLUE_TABLE, lutBytes, lutSize);
        m_pSolarizeEffect->SetValue(D2D1_TABLETRANSFER_PROP_ALPHA_DISABLE, TRUE);
    }

    if (!m_pThresholdEffect) {
        hr = m_pDeviceContext->CreateEffect(CLSID_D2D1TableTransfer, &m_pThresholdEffect);
        if (FAILED(hr)) return hr;

        std::vector<float> lut(256);
        for (int i = 0; i < 256; ++i) {
            const float v = static_cast<float>(i) / 255.0f;
            lut[i] = (v >= Constants::BW_THRESHOLD_LEVEL) ? 1.0f : 0.0f;
        }
        const auto *lutBytes = reinterpret_cast<const BYTE *>(lut.data());
        const UINT32 lutSize = static_cast<UINT32>(lut.size() * sizeof(float));
        m_pThresholdEffect->SetValue(D2D1_TABLETRANSFER_PROP_RED_TABLE, lutBytes, lutSize);
        m_pThresholdEffect->SetValue(D2D1_TABLETRANSFER_PROP_GREEN_TABLE, lutBytes, lutSize);
        m_pThresholdEffect->SetValue(D2D1_TABLETRANSFER_PROP_BLUE_TABLE, lutBytes, lutSize);
        m_pThresholdEffect->SetValue(D2D1_TABLETRANSFER_PROP_ALPHA_DISABLE, TRUE);
    }

    if (!m_pOutlineEffect) {
        hr = m_pDeviceContext->CreateEffect(CLSID_D2D1EdgeDetection, &m_pOutlineEffect);
        if (FAILED(hr)) {
            m_pOutlineEffect = nullptr;
        } else {
            m_pOutlineEffect->SetValue(D2D1_EDGEDETECTION_PROP_STRENGTH, Constants::OUTLINE_STRENGTH);
            m_pOutlineEffect->SetValue(D2D1_EDGEDETECTION_PROP_BLUR_RADIUS, Constants::OUTLINE_BLUR_RADIUS);
        }
    }

    return S_OK;
}

void RendererD2D::ApplyPreviousEffects() {
    (void) EnsureExtraEffects();

    ID2D1Effect *finalEffect = BuildEffectChain(m_pBitmap.Get());

    if (finalEffect) {
        Microsoft::WRL::ComPtr<ID2D1Image> effectOutput;
        finalEffect->GetOutput(&effectOutput);
        m_pActiveDisplayNode = effectOutput;
    }
}

// =============================================================================
//  BuildEffectChain
// =============================================================================
ID2D1Effect *RendererD2D::BuildEffectChain(ID2D1Image *source) {
    if (!m_pColorMatrixEffect) return nullptr;
    (void) EnsureExtraEffects();

    m_pColorMatrixEffect->SetInput(0, source);
    ID2D1Effect *current = m_pColorMatrixEffect.Get();

    if (std::abs(app.gamma - 1.0f) > 0.001f && m_pGammaEffect) {
        m_pGammaEffect->SetInputEffect(0, current);
        current = m_pGammaEffect.Get();
    }
    if (app.effectSolarize && m_pSolarizeEffect) {
        m_pSolarizeEffect->SetInputEffect(0, current);
        current = m_pSolarizeEffect.Get();
    }
    if (app.effectThreshold && m_pThresholdEffect) {
        m_pThresholdEffect->SetInputEffect(0, current);
        current = m_pThresholdEffect.Get();
    }
    if (app.effectOutline && m_pOutlineEffect) {
        m_pOutlineEffect->SetInputEffect(0, current);
        current = m_pOutlineEffect.Get();
    }

    return current;
}

// =============================================================================
//  CreateDeviceResources
// =============================================================================
HRESULT RendererD2D::CreateDeviceResources() {
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
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            createFlags, featureLevels, ARRAYSIZE(featureLevels),
            D3D11_SDK_VERSION, m_pD3DDevice.GetAddressOf(),
            &chosenLevel, m_pD3DContext.GetAddressOf());

    if (FAILED(hr)) {
        createFlags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                               createFlags, featureLevels, ARRAYSIZE(featureLevels),
                               D3D11_SDK_VERSION, m_pD3DDevice.GetAddressOf(),
                               &chosenLevel, m_pD3DContext.GetAddressOf());
        if (FAILED(hr)) return hr;
    }

    Microsoft::WRL::ComPtr<IDXGIDevice1> dxgiDevice;
    hr = m_pD3DDevice.As(&dxgiDevice);
    if (FAILED(hr)) return hr;

    (void) dxgiDevice->SetMaximumFrameLatency(1);

    Microsoft::WRL::ComPtr<IDXGIAdapter> dxgiAdapter;
    hr = dxgiDevice->GetAdapter(dxgiAdapter.GetAddressOf());
    if (FAILED(hr)) return hr;

    Microsoft::WRL::ComPtr<IDXGIFactory2> dxgiFactory;
    hr = dxgiAdapter->GetParent(IID_PPV_ARGS(&dxgiFactory));
    if (FAILED(hr)) return hr;

    DXGI_SWAP_CHAIN_DESC1 swapDesc{};
    swapDesc.Width = 0;
    swapDesc.Height = 0;
    swapDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    swapDesc.Stereo = FALSE;
    swapDesc.SampleDesc.Count = 1;
    swapDesc.SampleDesc.Quality = 0;
    swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapDesc.BufferCount = 2;
    swapDesc.Scaling = DXGI_SCALING_NONE;
    swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    swapDesc.Flags = 0;

    hr = dxgiFactory->CreateSwapChainForHwnd(
            m_pD3DDevice.Get(), m_hwnd,
            &swapDesc, nullptr, nullptr,
            &m_pSwapChain);
    if (FAILED(hr)) return hr;

    (void) dxgiFactory->MakeWindowAssociation(m_hwnd, DXGI_MWA_NO_ALT_ENTER);

    hr = m_pD2DFactory->CreateDevice(dxgiDevice.Get(), &m_pD2DDevice);
    if (FAILED(hr)) return hr;

    {
        Microsoft::WRL::ComPtr<ID2D1DeviceContext> baseDC;
        hr = m_pD2DDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, baseDC.GetAddressOf());
        if (FAILED(hr)) return hr;
        hr = baseDC.As(&m_pDeviceContext);
        if (FAILED(hr)) return hr;
    }

    hr = CreateBackBufferBitmap();
    if (FAILED(hr)) return hr;

    hr = m_pDeviceContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::LightGreen), &m_pTextBrush);
    if (FAILED(hr)) return hr;

    hr = m_pDeviceContext->CreateEffect(CLSID_D2D1ColorMatrix, &m_pColorMatrixEffect);
    if (FAILED(hr)) return hr;

    hr = m_pDeviceContext->CreateEffect(CLSID_D2D1Scale, &m_pScaleEffect);
    if (FAILED(hr)) return hr;

    UpdateColorEffects();

    return S_OK;
}

// =============================================================================
//  CreateBackBufferBitmap
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
    if (m_pDeviceContext) m_pDeviceContext->SetTarget(nullptr);

    m_pTextBrush.Reset();
    m_pBackBufferBitmap.Reset();
    m_pBitmap.Reset();
    m_bitmapCache.clear();
    m_lruList.clear();
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
    m_pGammaEffect.Reset();
    m_pSolarizeEffect.Reset();
    m_pThresholdEffect.Reset();
    m_pOutlineEffect.Reset();
    m_pScaleEffect.Reset();
}

// =============================================================================
//  Resize
// =============================================================================
void RendererD2D::Resize(UINT width, UINT height) {
    if (!m_pSwapChain || !m_pDeviceContext) return;

    m_pDeviceContext->SetTarget(nullptr);
    m_pBackBufferBitmap.Reset();

    HRESULT hr = m_pSwapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    if (SUCCEEDED(hr)) {
        (void) CreateBackBufferBitmap();
        g_overlayManager.OnResize(static_cast<float>(width), static_cast<float>(height));
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
            m_pActiveSvg.Reset();
            m_svgNativeW = 0.0f;
            m_svgNativeH = 0.0f;
            app.imgWidth = static_cast<int>(it->second.width);
            app.imgHeight = static_cast<int>(it->second.height);
            isCacheHit = true;
        }
    }

    if (isCacheHit) {
        m_pActiveDisplayNode = nullptr;
        if (onImageChangedCallback) {
            onImageChangedCallback(app.currentIndex);
        }
        return S_OK;
    }

    if (!bitmap || !m_pD2DDevice) return E_FAIL;

    Microsoft::WRL::ComPtr<ID2D1DeviceContext> tempCtx;
    HRESULT hr = m_pD2DDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &tempCtx);
    if (FAILED(hr)) return hr;

    Microsoft::WRL::ComPtr<ID2D1Bitmap1> newBitmap;
    hr = tempCtx->CreateBitmapFromWicBitmap(bitmap, nullptr, &newBitmap);

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
        m_pActiveDisplayNode = nullptr;
        m_pActiveSvg.Reset();
        m_svgNativeW = 0.0f;
        m_svgNativeH = 0.0f;

        app.imgWidth = static_cast<int>(width);
        app.imgHeight = static_cast<int>(height);

        if (onImageChangedCallback) {
            onImageChangedCallback(app.currentIndex);
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
        return S_OK;
    }
    Microsoft::WRL::ComPtr<IWICImagingFactory2> wicFac = g_decoderWorker.wicFactory;
    if (!wicFac || !m_pD2DDevice) return E_UNEXPECTED;

    Microsoft::WRL::ComPtr<ID2D1Device6> d2dDevice = m_pD2DDevice;

    g_ioWorker.PushTask([filePath, requestIndex, wicFac, d2dDevice, this]() {
        if (app.wantedIndex.load(std::memory_order_acquire) != requestIndex) return;

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
            if (app.wantedIndex.load(std::memory_order_acquire) != requestIndex) return;

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

            Microsoft::WRL::ComPtr<ID2D1DeviceContext> tempCtx;
            if (FAILED(d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &tempCtx))) return;

            Microsoft::WRL::ComPtr<ID2D1Bitmap1> newBitmap;
            HRESULT hr = tempCtx->CreateBitmapFromWicBitmap(converter.Get(), nullptr, &newBitmap);
            if (FAILED(hr)) return;

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

            if (app.wantedIndex.load(std::memory_order_acquire) == requestIndex) {
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

    if (m_pActiveSvg) {
        const D2D1_SIZE_F rtSize = m_pDeviceContext->GetSize();
        const float imgW = (m_svgNativeW > 1.0f) ? m_svgNativeW : rtSize.width;
        const float imgH = (m_svgNativeH > 1.0f) ? m_svgNativeH : rtSize.height;

        float renderW = imgW;
        float renderH = imgH;
        const float ratioX = rtSize.width / imgW;
        const float ratioY = rtSize.height / imgH;

        switch (app.viewMode) {
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

        const float z = (app.viewport.zoom <= 0.0f) ? 1.0f : app.viewport.zoom;
        renderW *= z;
        renderH *= z;

        const float left = (rtSize.width - renderW) / 2.0f + app.viewport.offsetX;
        const float top = (rtSize.height - renderH) / 2.0f + app.viewport.offsetY;

        m_pActiveSvg->SetViewportSize(D2D1::SizeF(renderW, renderH));

        const D2D1_POINT_2F screenCenter = D2D1::Point2F(rtSize.width / 2.0f, rtSize.height / 2.0f);

        D2D1_MATRIX_3X2_F transform = D2D1::Matrix3x2F::Translation(left, top);

        if (app.viewport.flippedH)
            transform = transform * D2D1::Matrix3x2F::Scale(-1.0f, 1.0f, screenCenter);
        if (app.viewport.flippedV)
            transform = transform * D2D1::Matrix3x2F::Scale(1.0f, -1.0f, screenCenter);
        if (app.viewport.rotation != 0)
            transform = transform * D2D1::Matrix3x2F::Rotation(
                                static_cast<float>(app.viewport.rotation), screenCenter);

        m_pDeviceContext->SetTransform(transform);

        Microsoft::WRL::ComPtr<ID2D1DeviceContext5> ctx5;
        if (SUCCEEDED(m_pDeviceContext.As(&ctx5))) {
            ctx5->DrawSvgDocument(m_pActiveSvg.Get());
        }

        m_pDeviceContext->SetTransform(D2D1::Matrix3x2F::Identity());

        g_overlayManager.RenderAll(m_pDeviceContext.Get());
    } else if (m_pBitmap) {
        const D2D1_SIZE_F imgSize = m_pBitmap->GetSize();
        const D2D1_SIZE_F rtSize = m_pDeviceContext->GetSize();
        const D2D1_POINT_2F center = D2D1::Point2F(rtSize.width / 2.0f, rtSize.height / 2.0f);

        float ratioX = rtSize.width / imgSize.width;
        float ratioY = rtSize.height / imgSize.height;
        float renderW = imgSize.width;
        float renderH = imgSize.height;

        switch (app.viewMode) {
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

        const float z = (app.viewport.zoom <= 0.0f) ? 1.0f : app.viewport.zoom;
        renderW *= z;
        renderH *= z;

        const float left = (rtSize.width - renderW) / 2.0f + app.viewport.offsetX;
        const float top = (rtSize.height - renderH) / 2.0f + app.viewport.offsetY;

        D2D1_MATRIX_3X2_F transform = D2D1::Matrix3x2F::Identity();
        if (app.viewport.flippedH)
            transform = transform * D2D1::Matrix3x2F::Scale(-1.0f, 1.0f, center);
        if (app.viewport.flippedV)
            transform = transform * D2D1::Matrix3x2F::Scale(1.0f, -1.0f, center);
        transform = transform * D2D1::Matrix3x2F::Rotation(
                            static_cast<float>(app.viewport.rotation), center);

        m_pDeviceContext->SetTransform(transform);

        bool isNative = (std::abs(app.viewport.zoom - 1.0f) < 0.001f);
        D2D1_INTERPOLATION_MODE interpMode = isNative
                                                 ? D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR
                                                 : D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC;

        if (app.effectPreviewEnabled && app.hasActiveEffects &&
            m_pActiveDisplayNode && m_pActiveDisplayNode.Get() != m_pBitmap.Get() &&
            m_pScaleEffect) {
            m_pScaleEffect->SetInput(0, m_pActiveDisplayNode.Get());
            m_pScaleEffect->SetValue(
                    D2D1_SCALE_PROP_SCALE,
                    D2D1::Vector2F(
                            renderW / imgSize.width,
                            renderH / imgSize.height));

            D2D1_SCALE_INTERPOLATION_MODE scaleInterp = isNative
                                                            ? D2D1_SCALE_INTERPOLATION_MODE_NEAREST_NEIGHBOR
                                                            : D2D1_SCALE_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC;
            m_pScaleEffect->SetValue(D2D1_SCALE_PROP_INTERPOLATION_MODE, scaleInterp);
            D2D1_POINT_2F targetOffset = D2D1::Point2F(left, top);
            m_pDeviceContext->DrawImage(
                    m_pScaleEffect.Get(),
                    targetOffset,
                    interpMode,
                    D2D1_COMPOSITE_MODE_SOURCE_OVER
                    );
        } else {
            m_pDeviceContext->DrawBitmap(
                    m_pBitmap.Get(),
                    D2D1::RectF(left, top, left + renderW, top + renderH),
                    1.0f,
                    interpMode);
        }

        m_pDeviceContext->SetTransform(D2D1::Matrix3x2F::Identity());

        g_overlayManager.RenderAll(m_pDeviceContext.Get());
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
//  ClearActiveImage
// =============================================================================
void RendererD2D::ClearActiveImage() {
    m_pBitmap.Reset();
    m_pActiveSvg.Reset();
    m_svgNativeW = 0.0f;
    m_svgNativeH = 0.0f;
}

// =============================================================================
//  LoadSvgFromBytes
// =============================================================================
HRESULT RendererD2D::LoadSvgFromBytes(const std::vector<BYTE> &svgBytes,
                                      const std::wstring &filePath) {
    if (!m_pDeviceContext) return E_UNEXPECTED;
    if (svgBytes.empty()) return E_INVALIDARG;

    m_pBitmap.Reset();
    m_pActiveSvg.Reset();

    bool isCacheHit = false;

    {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        auto it = m_svgCache.find(filePath);
        if (it != m_svgCache.end()) {
            m_svgLruList.splice(m_svgLruList.begin(), m_svgLruList, it->second.lruIt);
            m_pActiveSvg = it->second.document;
            m_svgNativeW = it->second.viewportW;
            m_svgNativeH = it->second.viewportH;
            m_pBitmap.Reset();
            app.imgWidth = static_cast<int>(m_svgNativeW);
            app.imgHeight = static_cast<int>(m_svgNativeH);
            isCacheHit = true;
        }
    }

    if (isCacheHit) {
        if (onImageChangedCallback) {
            onImageChangedCallback(app.currentIndex);
        }
        return S_OK;
    }

    Microsoft::WRL::ComPtr<ID2D1DeviceContext5> ctx5;
    HRESULT hr = m_pDeviceContext.As(&ctx5);
    if (FAILED(hr)) return hr;

    IStream *pStream = SHCreateMemStream(svgBytes.data(),
                                         static_cast<UINT>(svgBytes.size()));
    if (!pStream) return E_OUTOFMEMORY;

    D2D1_SIZE_F rtSize = m_pDeviceContext->GetSize();
    D2D1_SIZE_F viewport = (rtSize.width > 0 && rtSize.height > 0)
                               ? rtSize
                               : D2D1::SizeF(1920.0f, 1080.0f);

    Microsoft::WRL::ComPtr<ID2D1SvgDocument> svgDoc;
    hr = ctx5->CreateSvgDocument(pStream, viewport, svgDoc.GetAddressOf());
    pStream->Release();
    if (FAILED(hr)) return hr;

    float nativeW = 0.0f;
    float nativeH = 0.0f;

    Microsoft::WRL::ComPtr<ID2D1SvgElement> root;
    svgDoc->GetRoot(root.GetAddressOf());
    if (root) {
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

    if (nativeW <= 0.0f) nativeW = viewport.width;
    if (nativeH <= 0.0f) nativeH = viewport.height;

    {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        if (m_svgLruList.size() >= Constants::VRAM_CACHE_SVG_COUNT) {
            m_svgCache.erase(m_svgLruList.back());
            m_svgLruList.pop_back();
        }
        m_svgLruList.push_front(filePath);
        m_svgCache[filePath] = CachedSvg{svgDoc, m_svgLruList.begin(), nativeW, nativeH};
    }

    m_pActiveSvg = svgDoc;
    m_svgNativeW = nativeW;
    m_svgNativeH = nativeH;
    m_pBitmap.Reset();
    app.imgWidth = static_cast<int>(nativeW);
    app.imgHeight = static_cast<int>(nativeH);

    if (onImageChangedCallback) {
        onImageChangedCallback(app.currentIndex);
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

void RendererD2D::ClearCache() {
    ClearCache(L"");
}

void RendererD2D::ClearCache(const std::wstring &excludePath) {
    std::lock_guard<std::mutex> lock(m_cacheMutex);

    if (excludePath.empty()) {
        m_bitmapCache.clear();
        m_lruList.clear();
        m_svgCache.clear();
        m_svgLruList.clear();
        m_pBitmap.Reset();
        m_pActiveSvg.Reset();
        return;
    }

    auto bmpIt = m_bitmapCache.find(excludePath);
    bool foundBmp = (bmpIt != m_bitmapCache.end());
    CachedBitmap savedBmp;
    if (foundBmp) savedBmp = bmpIt->second;

    m_bitmapCache.clear();
    m_lruList.clear();

    if (foundBmp) {
        m_lruList.push_front(excludePath);
        savedBmp.lruIt = m_lruList.begin();
        m_bitmapCache[excludePath] = savedBmp;
    }

    auto svgIt = m_svgCache.find(excludePath);
    bool foundSvg = (svgIt != m_svgCache.end());
    CachedSvg savedSvg;
    if (foundSvg) savedSvg = svgIt->second;

    m_svgCache.clear();
    m_svgLruList.clear();

    if (foundSvg) {
        m_svgLruList.push_front(excludePath);
        savedSvg.lruIt = m_svgLruList.begin();
        m_svgCache[excludePath] = savedSvg;
    }
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
//  Thumbnail Panels (Cache & Dir Windows)
// =============================================================================

HRESULT RendererD2D::CreatePanelDeviceResources(ThumbnailPanelType type, HWND hwnd) {
    ThumbnailPanel &panel = GetPanel(type);
    panel.hwnd = hwnd;

    DXGI_SWAP_CHAIN_DESC1 swapDesc{};
    swapDesc.Width = 0;
    swapDesc.Height = 0;
    swapDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    swapDesc.BufferCount = 2;
    swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapDesc.SampleDesc.Count = 1;

    Microsoft::WRL::ComPtr<IDXGIDevice1> dxgiDevice;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    Microsoft::WRL::ComPtr<IDXGIFactory2> factory;

    m_pD3DDevice.As(&dxgiDevice);
    dxgiDevice->GetAdapter(&adapter);
    adapter->GetParent(IID_PPV_ARGS(&factory));

    HRESULT hr = factory->CreateSwapChainForHwnd(
            m_pD3DDevice.Get(), hwnd, &swapDesc, nullptr, nullptr, &panel.swapChain);
    if (FAILED(hr)) return hr;

    Microsoft::WRL::ComPtr<ID2D1DeviceContext> baseContext;
    hr = m_pD2DDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &baseContext);
    if (FAILED(hr)) return hr;

    hr = baseContext.As(&panel.deviceContext);
    if (FAILED(hr)) return hr;

    RECT rc{};
    GetClientRect(hwnd, &rc);
    UINT w = static_cast<UINT>(rc.right - rc.left);
    UINT h = static_cast<UINT>(rc.bottom - rc.top);

    if (w == 0 || h == 0) {
        w = (type == ThumbnailPanelType::Dir) ? 1200 : 800;
        h = Constants::CACHE_WINDOW_THICKNESS;
    }

    ResizePanel(type, w, h);

    panel.deviceContext->CreateSolidColorBrush(D2D1::ColorF(Constants::CacheColors::PLACEHOLDER), &panel.placeholderBrush);
    panel.deviceContext->CreateSolidColorBrush(D2D1::ColorF(Constants::CacheColors::SELECTION_BORDER), &panel.borderBrush);
    panel.deviceContext->CreateSolidColorBrush(D2D1::ColorF(Constants::CacheColors::HOVER), &panel.hoverBrush);

    return S_OK;
}

void RendererD2D::ResizePanel(ThumbnailPanelType type, UINT width, UINT height) {
    ThumbnailPanel &panel = GetPanel(type);
    if (!panel.swapChain || !panel.deviceContext) return;

    panel.deviceContext->SetTarget(nullptr);
    panel.backBuffer.Reset();

    HRESULT hr = panel.swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) return;

    Microsoft::WRL::ComPtr<IDXGISurface> surface;
    hr = panel.swapChain->GetBuffer(0, IID_PPV_ARGS(&surface));
    if (FAILED(hr)) return;

    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));

    hr = panel.deviceContext->CreateBitmapFromDxgiSurface(surface.Get(), &props, &panel.backBuffer);
    if (FAILED(hr)) return;

    panel.deviceContext->SetTarget(panel.backBuffer.Get());
}

void RendererD2D::RenderPanel(ThumbnailPanelType type, int selectedIndex, int hoverIndex, const std::vector<UI::Thumbnail> &thumbnails) {
    ThumbnailPanel &panel = GetPanel(type);
    if (!panel.deviceContext || !panel.swapChain) return;

    std::lock_guard<std::mutex> lock(m_cacheMutex);

    panel.deviceContext->BeginDraw();
    panel.deviceContext->Clear(D2D1::ColorF(0.08f, 0.08f, 0.08f, 1.0f));

    // The legacy ternary operator is removed. We now iterate directly over the passed parameter:
    for (size_t i = 0; i < thumbnails.size(); ++i) {
        const auto &thumb = thumbnails[i];
        bool drawn = false;

        // 1. Check Dir thumbnail cache if active
        if (type == ThumbnailPanelType::Dir) {
            std::lock_guard<std::mutex> dirLock(m_dirThumbMutex);
            auto it = m_dirThumbCache.find(thumb.filePath);
            if (it != m_dirThumbCache.end() && it->second) {
                panel.deviceContext->DrawBitmap(it->second.Get(), thumb.rect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                drawn = true;
            }
        }

        // 2. Fall back to global VRAM Cache
        if (!drawn) {
            auto it = m_bitmapCache.find(thumb.filePath);
            if (it != m_bitmapCache.end() && it->second.bitmap) {
                panel.deviceContext->DrawBitmap(it->second.bitmap.Get(), thumb.rect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                drawn = true;
            }
        }

        // 3. Fall back to placeholder
        if (!drawn) {
            panel.deviceContext->FillRectangle(thumb.rect, panel.placeholderBrush.Get());
        }

        // Selection highlight
        if (static_cast<int>(i) == selectedIndex) {
            panel.deviceContext->DrawRectangle(thumb.rect, panel.borderBrush.Get(), Constants::CacheColors::SELECTION_BORDER_THICKNESS);
        }

        // Hover highlight
        if (static_cast<int>(i) == hoverIndex) {
            panel.deviceContext->DrawRectangle(thumb.rect, panel.hoverBrush.Get(), Constants::CacheColors::HOVER_THICKNESS);
        }
    }

    HRESULT hr = panel.deviceContext->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET || hr == static_cast<HRESULT>(DXGI_ERROR_DEVICE_REMOVED)) {
        // Device loss
    }

    panel.swapChain->Present(1, 0);
}

void RendererD2D::DiscardPanelDeviceResources(ThumbnailPanelType type) {
    ThumbnailPanel &panel = GetPanel(type);
    if (panel.deviceContext) panel.deviceContext->SetTarget(nullptr);
    panel.backBuffer.Reset();
    panel.placeholderBrush.Reset();
    panel.borderBrush.Reset();
    panel.hoverBrush.Reset();
    panel.swapChain.Reset();
    panel.deviceContext.Reset();
    panel.hwnd = nullptr;
}

ID2D1DeviceContext7 *RendererD2D::GetPanelContext(ThumbnailPanelType type) {
    return GetPanel(type).deviceContext.Get();
}

// =============================================================================
//  SaveCurrentImageWithEffects
// =============================================================================
HRESULT RendererD2D::SaveCurrentImageWithEffects(const std::wstring &outPath) {
    if (!m_pBitmap || !m_pDeviceContext || !m_pD3DDevice) return E_FAIL;

    HRESULT hr = S_OK;
    D2D1_SIZE_U pixelSize = m_pBitmap->GetPixelSize();
    if (pixelSize.width == 0 || pixelSize.height == 0) return E_FAIL;

    D3D11_TEXTURE2D_DESC texDesc{};
    texDesc.Width = pixelSize.width;
    texDesc.Height = pixelSize.height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_RENDER_TARGET;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> offscreenTex;
    hr = m_pD3DDevice->CreateTexture2D(&texDesc, nullptr, &offscreenTex);
    if (FAILED(hr)) return hr;

    Microsoft::WRL::ComPtr<IDXGISurface> dxgiSurface;
    hr = offscreenTex.As(&dxgiSurface);
    if (FAILED(hr)) return hr;

    D2D1_BITMAP_PROPERTIES1 targetProps = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));

    Microsoft::WRL::ComPtr<ID2D1Bitmap1> offscreenBitmap;
    hr = m_pDeviceContext->CreateBitmapFromDxgiSurface(dxgiSurface.Get(), &targetProps, &offscreenBitmap);
    if (FAILED(hr)) return hr;

    Microsoft::WRL::ComPtr<ID2D1Image> previousTarget;
    m_pDeviceContext->GetTarget(&previousTarget);

    m_pDeviceContext->SetTarget(offscreenBitmap.Get());
    m_pDeviceContext->BeginDraw();
    m_pDeviceContext->Clear(D2D1::ColorF(D2D1::ColorF::Black));
    m_pDeviceContext->SetTransform(D2D1::Matrix3x2F::Identity());

    ID2D1Effect *finalEffect = BuildEffectChain(m_pBitmap.Get());
    if (finalEffect) {
        m_pDeviceContext->DrawImage(finalEffect, D2D1::Point2F(0.0f, 0.0f));
    } else {
        m_pDeviceContext->DrawBitmap(
                m_pBitmap.Get(),
                D2D1::RectF(0.0f, 0.0f,
                            static_cast<float>(pixelSize.width),
                            static_cast<float>(pixelSize.height)),
                1.0f,
                D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    }

    hr = m_pDeviceContext->EndDraw();
    m_pDeviceContext->SetTarget(previousTarget.Get());
    if (FAILED(hr)) return hr;

    D3D11_TEXTURE2D_DESC stagingDesc = texDesc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> stagingTex;
    hr = m_pD3DDevice->CreateTexture2D(&stagingDesc, nullptr, &stagingTex);
    if (FAILED(hr)) return hr;

    m_pD3DContext->CopyResource(stagingTex.Get(), offscreenTex.Get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = m_pD3DContext->Map(stagingTex.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) return hr;

    const UINT rowBytes = pixelSize.width * 4;
    std::vector<BYTE> pixels(static_cast<size_t>(pixelSize.width) * pixelSize.height * 4);
    for (UINT row = 0; row < pixelSize.height; ++row) {
        memcpy(pixels.data() + static_cast<size_t>(row) * rowBytes,
               static_cast<const BYTE *>(mapped.pData) + static_cast<size_t>(row) * mapped.RowPitch,
               rowBytes);
    }
    m_pD3DContext->Unmap(stagingTex.Get(), 0);

    Microsoft::WRL::ComPtr<IWICImagingFactory> wicFac = app.wicFactory;
    if (!wicFac) return E_FAIL;

    Microsoft::WRL::ComPtr<IWICStream> wicStream;
    hr = wicFac->CreateStream(&wicStream);
    if (FAILED(hr)) return hr;

    hr = wicStream->InitializeFromFilename(outPath.c_str(), GENERIC_WRITE);
    if (FAILED(hr)) return hr;

    Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
    hr = wicFac->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (FAILED(hr)) return hr;

    hr = encoder->Initialize(wicStream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(hr)) return hr;

    Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
    Microsoft::WRL::ComPtr<IPropertyBag2> frameProps;
    hr = encoder->CreateNewFrame(&frame, &frameProps);
    if (FAILED(hr)) return hr;

    hr = frame->Initialize(frameProps.Get());
    if (FAILED(hr)) return hr;

    hr = frame->SetSize(pixelSize.width, pixelSize.height);
    if (FAILED(hr)) return hr;

    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    hr = frame->SetPixelFormat(&fmt);
    if (FAILED(hr)) return hr;

    hr = frame->WritePixels(pixelSize.height, rowBytes,
                            static_cast<UINT>(pixels.size()), pixels.data());
    if (FAILED(hr)) return hr;

    hr = frame->Commit();
    if (FAILED(hr)) return hr;

    return encoder->Commit();
}

// =============================================================================
//  ClearDirThumbnailCache
// =============================================================================
void RendererD2D::ClearDirThumbnailCache() {
    std::lock_guard<std::mutex> lock(m_dirThumbMutex);
    m_dirThumbCache.clear();
}

// =============================================================================
//  RequestDirThumbnail
// =============================================================================
void RendererD2D::RequestDirThumbnail(const std::wstring &filePath) {
    {
        std::lock_guard<std::mutex> lock(m_dirThumbMutex);
        if (m_dirThumbCache.count(filePath)) return;
    }

    Microsoft::WRL::ComPtr<IWICImagingFactory2> wicFac = g_decoderWorker.wicFactory;
    Microsoft::WRL::ComPtr<ID2D1Device6> d2dDev = m_pD2DDevice;

    // Access the dir panel's hwnd via the unified struct
    HWND hDir = GetPanel(ThumbnailPanelType::Dir).hwnd;

    if (!wicFac || !d2dDev || !hDir) return;

    UINT thumbW = static_cast<UINT>(Constants::CACHE_THUMB_WIDTH);
    UINT thumbH = static_cast<UINT>(Constants::CACHE_THUMB_HEIGHT);

    g_ioWorker.PushTask([filePath, wicFac, d2dDev, hDir, thumbW, thumbH, this]() {
        HANDLE hFile = CreateFileW(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                   nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) return;

        LARGE_INTEGER fileSize{};
        if (!GetFileSizeEx(hFile, &fileSize)) {
            CloseHandle(hFile);
            return;
        }

        std::vector<BYTE> bytes(static_cast<size_t>(fileSize.QuadPart));
        DWORD bytesRead = 0;
        bool ok = ReadFile(hFile, bytes.data(), static_cast<DWORD>(bytes.size()), &bytesRead, nullptr)
                  && bytesRead == static_cast<DWORD>(bytes.size());
        CloseHandle(hFile);
        if (!ok) return;

        g_decoderWorker.PushTask([bytes = std::move(bytes), filePath, wicFac, d2dDev,
                    hDir, thumbW, thumbH, this]() mutable {
                    Microsoft::WRL::ComPtr<IWICStream> stream;
                    if (FAILED(wicFac->CreateStream(&stream))) return;
                    if (FAILED(stream->InitializeFromMemory(bytes.data(),
                        static_cast<DWORD>(bytes.size()))))
                        return;

                    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
                    if (FAILED(wicFac->CreateDecoderFromStream(stream.Get(), nullptr,
                        WICDecodeMetadataCacheOnDemand, &decoder)))
                        return;

                    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
                    if (FAILED(decoder->GetFrame(0, &frame))) return;

                    UINT srcW = 0, srcH = 0;
                    frame->GetSize(&srcW, &srcH);
                    if (srcW == 0 || srcH == 0) return;

                    float scaleX = static_cast<float>(thumbW) / static_cast<float>(srcW);
                    float scaleY = static_cast<float>(thumbH) / static_cast<float>(srcH);
                    float scale = (scaleX < scaleY) ? scaleX : scaleY;
                    UINT dstW = static_cast<UINT>(static_cast<float>(srcW) * scale);
                    UINT dstH = static_cast<UINT>(static_cast<float>(srcH) * scale);
                    if (dstW == 0) dstW = 1;
                    if (dstH == 0) dstH = 1;

                    Microsoft::WRL::ComPtr<IWICBitmapScaler> scaler;
                    if (FAILED(wicFac->CreateBitmapScaler(&scaler))) return;
                    if (FAILED(scaler->Initialize(frame.Get(), dstW, dstH,
                        WICBitmapInterpolationModeFant)))
                        return;

                    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
                    if (FAILED(wicFac->CreateFormatConverter(&converter))) return;
                    if (FAILED(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA,
                        WICBitmapDitherTypeNone, nullptr, 0.0f,
                        WICBitmapPaletteTypeCustom)))
                        return;

                    Microsoft::WRL::ComPtr<ID2D1DeviceContext> tempCtx;
                    if (FAILED(d2dDev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &tempCtx))) return;

                    Microsoft::WRL::ComPtr<ID2D1Bitmap1> thumbBitmap;
                    if (FAILED(tempCtx->CreateBitmapFromWicBitmap(converter.Get(), nullptr, &thumbBitmap))) return;

                    {
                        std::lock_guard<std::mutex> lock(m_dirThumbMutex);
                        m_dirThumbCache[filePath] = thumbBitmap;
                    }

                    PostMessageW(hDir, Constants::WM_QIV_REPAINT, 0, 0);
                });
    });
}
