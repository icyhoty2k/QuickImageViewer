#include "RendererD2D.h"
#include "../Overlays/OverlayManager.h"
#include "../AppState.h"
#include "../Platform/Constants.h"
#include "../WorkerThread.h"

#include <algorithm>
#include <chrono>
#include <vector>
#include <shlwapi.h>  // SHCreateMemStream

#include "DirWnd.h"

// Link the required import libraries
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dxguid.lib")

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
        HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED,
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
        g_overlayManager.UpdateTextFormat();
    }

    HRESULT hr = CreateDeviceResources();
    if (SUCCEEDED(hr)) {
        // Initialize to empty/default state
        m_pActiveDisplayNode = nullptr;
        // Hand text resources to the overlay manager — it does not own them
        g_overlayManager.Init(m_pDWriteFactory.Get(), m_pTextBrush.Get(), m_pDeviceContext.Get());
        g_overlayManager.UpdateTextFormat();

        // Compute initial rects from the actual window client size
        RECT rc{};
        GetClientRect(m_hwnd, &rc);
        g_overlayManager.OnResize(
                static_cast<float>(rc.right - rc.left),
                static_cast<float>(rc.bottom - rc.top));
    }
    return hr;
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

        // Cache the ID2D1DeviceContext5 interface once — used every frame for SVG.
        // Avoids a QueryInterface call inside Render().
        (void) m_pDeviceContext.As(&m_pDeviceContext5);
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
    // Notify overlay manager before device resources disappear
    g_overlayManager.OnDeviceLost();

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
    m_pDeviceContext5.Reset();
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

    // Create a short-lived DeviceContext for this upload.
    // ID2D1Device::CreateDeviceContext is thread-safe; each caller gets its own
    // context so there is no contention with PreloadBitmap worker threads.
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> uploadCtx;
    HRESULT hr = m_pD2DDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
                                                   uploadCtx.GetAddressOf());
    if (FAILED(hr)) return hr;

    Microsoft::WRL::ComPtr<ID2D1Bitmap1> newBitmap;
    hr = uploadCtx->CreateBitmapFromWicBitmap(bitmap, nullptr, &newBitmap);

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

    // Capture device context as a pointer for the task (D2D device contexts are thread-safe)
    Microsoft::WRL::ComPtr<ID2D1Device6> d2dDevice = m_pD2DDevice;

    g_ioWorker.PushTask([filePath, requestIndex, d2dDevice, this]() {
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

        // Pass the factory as a parameter to the lambda (injected by the thread pool)
        g_decoderWorker.PushTask([compressedBytes = std::move(compressedBytes), filePath, requestIndex, d2dDevice, this](IWICImagingFactory2 *wicFac) mutable {
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

            IWICBitmapSource *uploadSource = nullptr;
            Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
            WICPixelFormatGUID srcFmt{};
            if (FAILED(frame->GetPixelFormat(&srcFmt))) return;

            if (srcFmt == GUID_WICPixelFormat32bppPBGRA) {
                uploadSource = frame.Get();
            } else {
                if (FAILED(wicFac->CreateFormatConverter(&converter))) return;
                if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0f, WICBitmapPaletteTypeCustom)))
                    return;
                uploadSource = converter.Get();
            }

            Microsoft::WRL::ComPtr<ID2D1DeviceContext> taskCtx;
            if (FAILED(d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, taskCtx.GetAddressOf()))) return;

            Microsoft::WRL::ComPtr<ID2D1Bitmap1> newBitmap;
            HRESULT hr = taskCtx->CreateBitmapFromWicBitmap(uploadSource, nullptr, &newBitmap);
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

        if (m_pDeviceContext5) {
            m_pDeviceContext5->DrawSvgDocument(m_pActiveSvg.Get());
        }

        m_pDeviceContext->SetTransform(D2D1::Matrix3x2F::Identity());
        g_overlayManager.UpdateZoom(app.viewport.zoom, m_hwnd);
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
        g_overlayManager.UpdateZoom(app.viewport.zoom, m_hwnd);
        g_overlayManager.RenderAll(m_pDeviceContext.Get());
    }

    HRESULT hr = m_pDeviceContext->EndDraw();

    if (hr == D2DERR_RECREATE_TARGET ||
        hr == static_cast<HRESULT>(DXGI_ERROR_DEVICE_REMOVED) ||
        hr == static_cast<HRESULT>(DXGI_ERROR_DEVICE_RESET)) {
        DiscardDeviceResources();
        hr = CreateDeviceResources();
        if (SUCCEEDED(hr)) {
            // m_pTextFormat and m_pTextBrush are recreated inside CreateDeviceResources.
            // Re-hand them to the overlay manager so it doesn't hold stale pointers
            // from before the device loss.
            g_overlayManager.Init(m_pDWriteFactory.Get(), m_pTextBrush.Get(), m_pDeviceContext.Get());
            g_overlayManager.UpdateTextFormat();
            RECT rc{};
            GetClientRect(m_hwnd, &rc);
            g_overlayManager.OnResize(static_cast<float>(rc.right - rc.left),
                                      static_cast<float>(rc.bottom - rc.top));
        }
        return hr;
    }

    if (FAILED(hr)) return hr;

    HRESULT hrPresent = m_pSwapChain->Present(0, 0);
    if (hrPresent == static_cast<HRESULT>(DXGI_ERROR_DEVICE_REMOVED) ||
        hrPresent == static_cast<HRESULT>(DXGI_ERROR_DEVICE_RESET)) {
        DiscardDeviceResources();
        if (SUCCEEDED(CreateDeviceResources())) {
            g_overlayManager.Init(m_pDWriteFactory.Get(), m_pTextBrush.Get(), m_pDeviceContext.Get());
            g_overlayManager.UpdateTextFormat();
            RECT rc{};
            GetClientRect(m_hwnd, &rc);
            g_overlayManager.OnResize(static_cast<float>(rc.right - rc.left),
                                      static_cast<float>(rc.bottom - rc.top));
        }
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

    if (!m_pDeviceContext5) return E_UNEXPECTED;

    IStream *pStream = SHCreateMemStream(svgBytes.data(),
                                         static_cast<UINT>(svgBytes.size()));
    if (!pStream) return E_OUTOFMEMORY;

    D2D1_SIZE_F rtSize = m_pDeviceContext->GetSize();
    D2D1_SIZE_F viewport = (rtSize.width > 0 && rtSize.height > 0)
                               ? rtSize
                               : D2D1::SizeF(1920.0f, 1080.0f);

    Microsoft::WRL::ComPtr<ID2D1SvgDocument> svgDoc;
    HRESULT hr = m_pDeviceContext5->CreateSvgDocument(pStream, viewport, svgDoc.GetAddressOf());
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
//  ResolveThumbnailBitmaps
//  Called by ThumbnailPanelWnd::Render() to resolve bitmap pointers under
//  the minimum lock duration before any GPU work begins.
// =============================================================================
void RendererD2D::ResolveThumbnailBitmaps(const std::vector<UI::Thumbnail> &thumbnails,
                                          HWND hPanel,
                                          std::vector<ResolvedThumb> &out) {
    out.clear();
    out.reserve(thumbnails.size());

    std::lock_guard<std::mutex> cacheLock(m_cacheMutex);
    std::lock_guard<std::mutex> dirLock(m_dirThumbMutex);

    const PanelThumbEntry *panelEntry = nullptr;
    if (hPanel) {
        auto it = m_panelThumbCaches.find(hPanel);
        if (it != m_panelThumbCaches.end())
            panelEntry = &it->second;
    }

    for (const auto &thumb : thumbnails) {
        ResolvedThumb r;
        r.rect = thumb.rect;

        if (panelEntry) {
            auto it = panelEntry->bitmaps.find(thumb.filePath);
            if (it != panelEntry->bitmaps.end() && it->second)
                r.bitmap = it->second;
        }

        // Fall back to the full-res VRAM cache (covers CacheWnd and already-loaded images).
        if (!r.bitmap) {
            auto it = m_bitmapCache.find(thumb.filePath);
            if (it != m_bitmapCache.end() && it->second.bitmap)
                r.bitmap = it->second.bitmap;
        }

        out.push_back(std::move(r));
    }
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
void RendererD2D::ClearDirThumbnailCache(HWND hPanel) {
    std::lock_guard<std::mutex> lock(m_dirThumbMutex);
    m_panelThumbCaches.erase(hPanel);
}

// =============================================================================
//  RequestDirThumbnail
// =============================================================================
void RendererD2D::RequestDirThumbnail(const std::wstring &filePath, HWND hPanel) {
    {
        std::lock_guard<std::mutex> lock(m_dirThumbMutex);
        auto &entry = m_panelThumbCaches[hPanel];
        if (entry.bitmaps.count(filePath)) return;
        if (entry.inFlight.count(filePath)) return;
        entry.inFlight.insert(filePath);
    }

    Microsoft::WRL::ComPtr<ID2D1Device6> d2dDev = m_pD2DDevice;
    HWND hDir = hPanel;

    if (!d2dDev || !hDir) return;

    UINT thumbW = static_cast<UINT>(Constants::THUMBNAIL_PANEL_THUMB_WIDTH);
    UINT thumbH = static_cast<UINT>(Constants::THUMBNAIL_PANEL_THUMB_HEIGHT);

    g_ioWorker.PushTask([filePath, d2dDev, hDir, thumbW, thumbH, this]() {
        HANDLE hFile = CreateFileW(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) return;

        LARGE_INTEGER fileSize{};
        if (!GetFileSizeEx(hFile, &fileSize)) {
            CloseHandle(hFile);
            return;
        }

        std::vector<BYTE> bytes(static_cast<size_t>(fileSize.QuadPart));
        DWORD bytesRead = 0;
        bool ok = ReadFile(hFile, bytes.data(), static_cast<DWORD>(bytes.size()), &bytesRead, nullptr) && bytesRead == static_cast<DWORD>(bytes.size());
        CloseHandle(hFile);
        if (!ok) return;

        g_dirThumbWorker.PushTask([bytes = std::move(bytes), filePath, d2dDev, hDir, thumbW, thumbH, this](IWICImagingFactory2 *wicFac) mutable {
            Microsoft::WRL::ComPtr<IWICStream> stream;
            if (FAILED(wicFac->CreateStream(&stream))) return;
            if (FAILED(stream->InitializeFromMemory(bytes.data(), static_cast<DWORD>(bytes.size())))) return;

            Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
            if (FAILED(wicFac->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder))) return;

            Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
            if (FAILED(decoder->GetFrame(0, &frame))) return;

            UINT srcW = 0, srcH = 0;
            frame->GetSize(&srcW, &srcH);
            if (srcW == 0 || srcH == 0) return;

            float scale = std::min(static_cast<float>(thumbW) / srcW, static_cast<float>(thumbH) / srcH);
            UINT dstW = std::max(1U, static_cast<UINT>(srcW * scale));
            UINT dstH = std::max(1U, static_cast<UINT>(srcH * scale));

            Microsoft::WRL::ComPtr<IWICBitmapScaler> scaler;
            if (FAILED(wicFac->CreateBitmapScaler(&scaler))) return;
            if (FAILED(scaler->Initialize(frame.Get(), dstW, dstH, WICBitmapInterpolationModeFant))) return;

            IWICBitmapSource *uploadSource = nullptr;
            Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
            WICPixelFormatGUID scaledFmt{};
            if (FAILED(scaler->GetPixelFormat(&scaledFmt))) return;

            if (scaledFmt == GUID_WICPixelFormat32bppPBGRA) {
                uploadSource = scaler.Get();
            } else {
                if (FAILED(wicFac->CreateFormatConverter(&converter))) return;
                if (FAILED(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0f, WICBitmapPaletteTypeCustom)))
                    return;
                uploadSource = converter.Get();
            }

            Microsoft::WRL::ComPtr<ID2D1DeviceContext> taskCtx;
            if (FAILED(d2dDev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, taskCtx.GetAddressOf()))) return;

            Microsoft::WRL::ComPtr<ID2D1Bitmap1> thumbBitmap;
            if (FAILED(taskCtx->CreateBitmapFromWicBitmap(uploadSource, nullptr, &thumbBitmap))) return;

            {
                std::lock_guard<std::mutex> lock(m_dirThumbMutex);
                auto &entry = m_panelThumbCaches[hDir];
                entry.inFlight.erase(filePath);
                if (!entry.bitmaps.count(filePath))
                    entry.bitmaps[filePath] = thumbBitmap;
            }
            PostMessageW(hDir, Constants::WM_QIV_REPAINT, 0, 0);
        });
    });
}
