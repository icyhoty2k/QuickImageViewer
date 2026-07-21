#include "RendererD2D.h"
#include "../Overlays/OverlayManager.h"
#include "../AppState.h"
#include "../Platform/Constants.h"
#include "../Platform/ConstantsStrings.h"
#include "../WorkerThread.h"
#include "../SimpleFormats.h"
#include "../ImageLoadStats.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <vector>
#include <shlwapi.h>   // SHCreateMemStream
#include <shobjidl.h>  // IShellItemImageFactory, SHCreateItemFromParsingName

#include "../UI/ThumbnailPanels/DirWnd.h"

// resvg C API (static lib)
#include <resvg.h>

// Link the required import libraries
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "shell32.lib")

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
    // Non-linear effect nodes are created lazily here
    (void) EnsureExtraEffects();
    // 1. Check BOTH booleans. If effects are active AND the preview toggle is on...
    if (app.hasActiveEffects && app.effectPreviewEnabled && m_pBitmap) {
        // Always rebuild the chain unconditionally.
        ApplyPreviousEffects();
    } else {
        // Safe bypass: no effects active OR preview toggled off.
        m_pActiveDisplayNode = m_pBitmap; // FAST PATH
    }

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

    hr = m_pDeviceContext->CreateSolidColorBrush(
        D2D1::ColorF(210.0f / 255.0f, 70.0f / 255.0f, 70.0f / 255.0f),
        &m_pFolderDeletedBrush);
    if (FAILED(hr)) return hr;

    hr = m_pDeviceContext->CreateSolidColorBrush(
        D2D1::ColorF(Constants::Links::COLOR_R_F,
                     Constants::Links::COLOR_G_F,
                     Constants::Links::COLOR_B_F),
        &m_pLinkBrush);
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
    m_pFolderDeletedBrush.Reset();
    m_pLinkBrush.Reset();
    // The overlay layout carries a drawing effect referencing m_pLinkBrush
    // (device-bound) — drop it so it rebuilds cleanly on the new device.
    m_pFolderDeletedLayout.Reset();
    m_lastFolderOverlayKey.clear();
    m_pBackBufferBitmap.Reset();
    m_pBitmap.Reset();
    m_bitmapCache.clear();
    m_lruList.clear();
    m_pActiveSvg.Reset();
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
        // Overlay layout is cached — just retarget its bounds, no recreation.
        if (m_pFolderDeletedLayout) {
            (void) m_pFolderDeletedLayout->SetMaxWidth(static_cast<float>(width));
            (void) m_pFolderDeletedLayout->SetMaxHeight(static_cast<float>(height));
        }
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
            m_pActiveSvg.Reset();
            m_svgNativeW = 0.0f;
            m_svgNativeH = 0.0f;
            app.imgWidth = static_cast<int>(it->second.width);
            app.imgHeight = static_cast<int>(it->second.height);
            // Restore GIF animation state (empty vectors = static image)
            m_gifFrame  = 0;
            m_gifFrames = it->second.gifFrames;
            m_gifDelays = it->second.gifDelays;
            m_pBitmap   = m_gifFrames.empty() ? it->second.bitmap : m_gifFrames[0];
            isCacheHit  = true;
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
            if (m_lruList.size() >= static_cast<size_t>(app.vramCacheCount)) {
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
//  Animated GIF helpers
// =============================================================================
int RendererD2D::GetCurrentGifDelay() const {
    if (m_gifDelays.empty()) return 100;
    return m_gifDelays[m_gifFrame % static_cast<int>(m_gifDelays.size())];
}

int RendererD2D::AdvanceGifFrame() {
    if (m_gifFrames.empty()) return 0;
    m_gifFrame = (m_gifFrame + 1) % static_cast<int>(m_gifFrames.size());
    m_pBitmap = m_gifFrames[m_gifFrame];
    m_pActiveDisplayNode = nullptr; // bypass effect chain; draw m_pBitmap directly
    return m_gifDelays[m_gifFrame];
}

void RendererD2D::ResetGifAnimation() {
    m_gifFrame = 0;
    m_gifFrames.clear();
    m_gifDelays.clear();
}

// =============================================================================
//  PreloadBitmap
// =============================================================================
HRESULT RendererD2D::PreloadBitmap(const std::wstring &filePath, int requestIndex, int expectedCurrentIndex) {
    // guardIndex: for neighbor preloads, expectedCurrentIndex is the image the user
    // is currently viewing; cancel if the user has moved away from it.
    // For the main-image load (default -1), fall back to requestIndex (same behavior).
    const int guardIndex = (expectedCurrentIndex >= 0) ? expectedCurrentIndex : requestIndex;

    // Main image loads (expectedCurrentIndex < 0) are guarded by path IDENTITY, so
    // they survive an index change from the folder re-sort that runs after the
    // initial 1-file F2 open. Neighbor preloads keep the index guard so they cancel
    // when the user navigates away from the anchor image.
    const bool   isMain   = (expectedCurrentIndex < 0);
    const size_t pathHash = std::hash<std::wstring>{}(filePath);

    {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        if (m_bitmapCache.find(filePath) != m_bitmapCache.end()) return S_OK;
        if (m_bitmapInFlight.count(filePath)) return S_OK;
        m_bitmapInFlight.insert(filePath);
    }

    // Capture device context as a pointer for the task (D2D device contexts are thread-safe)
    Microsoft::WRL::ComPtr<ID2D1Device6> d2dDevice = m_pD2DDevice;

    g_ioWorker.PushTask([filePath, requestIndex, guardIndex, isMain, pathHash, d2dDevice, this]() {
        const bool stale = isMain
            ? (app.wantedPathHash.load(std::memory_order_acquire) != pathHash)
            : (app.wantedIndex.load(std::memory_order_acquire)    != guardIndex);
        if (stale) {
            std::lock_guard<std::mutex> lock(m_cacheMutex);
            m_bitmapInFlight.erase(filePath);
            return;
        }

        HANDLE hFile = CreateFileW(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile == INVALID_HANDLE_VALUE) {
            std::lock_guard<std::mutex> lock(m_cacheMutex);
            m_bitmapInFlight.erase(filePath);
            return;
        }

        LARGE_INTEGER fileSize;
        if (!GetFileSizeEx(hFile, &fileSize)) {
            CloseHandle(hFile);
            std::lock_guard<std::mutex> lock(m_cacheMutex);
            m_bitmapInFlight.erase(filePath);
            return;
        }

        std::vector<BYTE> compressedBytes(fileSize.QuadPart);
        DWORD bytesRead;
        if (!ReadFile(hFile, compressedBytes.data(), static_cast<DWORD>(fileSize.QuadPart), &bytesRead, NULL) || bytesRead != fileSize.QuadPart) {
            CloseHandle(hFile);
            std::lock_guard<std::mutex> lock(m_cacheMutex);
            m_bitmapInFlight.erase(filePath);
            return;
        }
        CloseHandle(hFile);

        // Pass the factory as a parameter to the lambda (injected by the thread pool)
        g_decoderWorker.PushTask([compressedBytes = std::move(compressedBytes), filePath, requestIndex, guardIndex, isMain, pathHash, d2dDevice, this](IWICImagingFactory2 *wicFac) mutable {
            // Release inFlight immediately so a new PreloadBitmap call can be queued
            // while decode is still in progress (e.g. neighbour preload races).
            { std::lock_guard<std::mutex> lock(m_cacheMutex); m_bitmapInFlight.erase(filePath); }

            const bool stale = isMain
                ? (app.wantedPathHash.load(std::memory_order_acquire) != pathHash)
                : (app.wantedIndex.load(std::memory_order_acquire)    != guardIndex);
            if (stale) return;

            Microsoft::WRL::ComPtr<ID2D1DeviceContext> taskCtx;
            if (FAILED(d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, taskCtx.GetAddressOf()))) return;

            Microsoft::WRL::ComPtr<ID2D1Bitmap1> newBitmap;
            UINT width = 0, height = 0;
            USHORT orientation = 1;

            if (SimpleFormats::IsSimpleFormat(filePath)) {
                Microsoft::WRL::ComPtr<IWICBitmap> sfBmp =
                    SimpleFormats::Decode(filePath, compressedBytes, wicFac, width, height);
                if (!sfBmp) return;
                if (FAILED(taskCtx->CreateBitmapFromWicBitmap(sfBmp.Get(), nullptr, &newBitmap))) return;
            } else {
                // --- Standard WIC path ---
                Microsoft::WRL::ComPtr<IWICStream> wicStream;
                if (FAILED(wicFac->CreateStream(&wicStream))) return;
                if (FAILED(wicStream->InitializeFromMemory(compressedBytes.data(), static_cast<DWORD>(compressedBytes.size())))) return;

                Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
                if (FAILED(wicFac->CreateDecoderFromStream(wicStream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder))) return;

                UINT frameCount = 1;
                decoder->GetFrameCount(&frameCount);

                if (frameCount > 1) {
                    // ── Animated GIF path ──────────────────────────────────────────
                    // Read logical screen size from the GIF container metadata.
                    UINT screenW = 0, screenH = 0;
                    {
                        Microsoft::WRL::ComPtr<IWICMetadataQueryReader> cmr;
                        if (SUCCEEDED(decoder->GetMetadataQueryReader(&cmr))) {
                            PROPVARIANT pv;
                            PropVariantInit(&pv);
                            if (SUCCEEDED(cmr->GetMetadataByName(L"/logscrdesc/Width",  &pv)) && pv.vt == VT_UI2) screenW = pv.uiVal; PropVariantClear(&pv);
                            if (SUCCEEDED(cmr->GetMetadataByName(L"/logscrdesc/Height", &pv)) && pv.vt == VT_UI2) screenH = pv.uiVal; PropVariantClear(&pv);
                        }
                    }

                    // Fallback: use frame 0 size
                    if (screenW == 0 || screenH == 0) {
                        Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> f0;
                        if (SUCCEEDED(decoder->GetFrame(0, &f0))) f0->GetSize(&screenW, &screenH);
                    }
                    if (screenW == 0 || screenH == 0) return;

                    width  = screenW;
                    height = screenH;
                    orientation = 1; // GIF has no EXIF

                    // Compositing canvas — PBGRA32, transparent black
                    const UINT canvasStride = screenW * 4;
                    std::vector<BYTE> canvas(canvasStride * screenH, 0);
                    std::vector<BYTE> prevCanvas; // for disposal mode 3

                    std::vector<Microsoft::WRL::ComPtr<ID2D1Bitmap1>> gifFrames;
                    std::vector<int> gifDelays;
                    gifFrames.reserve(frameCount);
                    gifDelays.reserve(frameCount);

                    const D2D1_BITMAP_PROPERTIES1 bmpProps = D2D1::BitmapProperties1(
                        D2D1_BITMAP_OPTIONS_NONE,
                        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));

                    for (UINT fi = 0; fi < frameCount; ++fi) {
                        if (isMain ? (app.wantedPathHash.load(std::memory_order_acquire) != pathHash)
                                   : (app.wantedIndex.load(std::memory_order_acquire) != guardIndex)) return;

                        prevCanvas = canvas;

                        Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> gifFrame;
                        if (FAILED(decoder->GetFrame(fi, &gifFrame))) break;

                        UINT fw = 0, fh = 0;
                        gifFrame->GetSize(&fw, &fh);

                        int left = 0, top = 0, disposal = 0, delayCs = 10;
                        {
                            Microsoft::WRL::ComPtr<IWICMetadataQueryReader> fmr;
                            if (SUCCEEDED(gifFrame->GetMetadataQueryReader(&fmr))) {
                                PROPVARIANT pv;
                                PropVariantInit(&pv);
                                if (SUCCEEDED(fmr->GetMetadataByName(L"/imgdesc/Left",     &pv)) && pv.vt == VT_UI2) left     = pv.uiVal; PropVariantClear(&pv);
                                if (SUCCEEDED(fmr->GetMetadataByName(L"/imgdesc/Top",      &pv)) && pv.vt == VT_UI2) top      = pv.uiVal; PropVariantClear(&pv);
                                if (SUCCEEDED(fmr->GetMetadataByName(L"/grctlext/Disposal",&pv)) && pv.vt == VT_UI1) disposal = pv.bVal;   PropVariantClear(&pv);
                                if (SUCCEEDED(fmr->GetMetadataByName(L"/grctlext/Delay",  &pv)) && pv.vt == VT_UI2) delayCs  = pv.uiVal;  PropVariantClear(&pv);
                            }
                        }

                        const int delayMs = std::max(20, delayCs * 10);

                        // Convert frame to PBGRA32 (WIC handles GIF transparency via palette)
                        Microsoft::WRL::ComPtr<IWICFormatConverter> conv;
                        if (FAILED(wicFac->CreateFormatConverter(&conv))) break;
                        Microsoft::WRL::ComPtr<IWICPalette> pal;
                        wicFac->CreatePalette(&pal);
                        gifFrame->CopyPalette(pal.Get());
                        if (FAILED(conv->Initialize(gifFrame.Get(), GUID_WICPixelFormat32bppPBGRA,
                                                    WICBitmapDitherTypeNone, pal.Get(), 0.0,
                                                    WICBitmapPaletteTypeCustom))) break;

                        const UINT fStride = fw * 4;
                        std::vector<BYTE> framePx(fStride * fh);
                        if (FAILED(conv->CopyPixels(nullptr, fStride, static_cast<UINT>(framePx.size()), framePx.data()))) break;

                        // Composite frame onto canvas: only copy non-transparent pixels
                        const UINT clampW = std::min(fw, screenW > static_cast<UINT>(left) ? screenW - static_cast<UINT>(left) : 0u);
                        const UINT clampH = std::min(fh, screenH > static_cast<UINT>(top)  ? screenH - static_cast<UINT>(top)  : 0u);
                        for (UINT row = 0; row < clampH; ++row) {
                            const BYTE *src = framePx.data() + row * fStride;
                            BYTE *dst = canvas.data() + (static_cast<UINT>(top) + row) * canvasStride + static_cast<UINT>(left) * 4;
                            for (UINT col = 0; col < clampW; ++col, src += 4, dst += 4) {
                                if (src[3] > 0) { dst[0]=src[0]; dst[1]=src[1]; dst[2]=src[2]; dst[3]=src[3]; }
                            }
                        }

                        // Upload canvas snapshot as D2D bitmap for this frame
                        Microsoft::WRL::ComPtr<ID2D1Bitmap1> d2dFrame;
                        if (SUCCEEDED(taskCtx->CreateBitmap(D2D1::SizeU(screenW, screenH),
                                                            canvas.data(), canvasStride, bmpProps, &d2dFrame))) {
                            gifFrames.push_back(d2dFrame);
                            gifDelays.push_back(delayMs);
                        }

                        // Apply disposal mode before next frame
                        switch (disposal) {
                            case 2: { // Restore to background (transparent)
                                for (UINT row = 0; row < clampH; ++row)
                                    memset(canvas.data() + (static_cast<UINT>(top) + row) * canvasStride + static_cast<UINT>(left) * 4, 0, clampW * 4);
                                break;
                            }
                            case 3: // Restore to previous
                                canvas = prevCanvas;
                                break;
                            default: break; // 0 / 1: leave canvas as-is
                        }
                    }

                    if (gifFrames.empty()) return;
                    newBitmap = gifFrames[0];

                    {
                        std::lock_guard<std::mutex> lock(m_cacheMutex);
                        auto it = m_bitmapCache.find(filePath);
                        if (it != m_bitmapCache.end()) { m_lruList.erase(it->second.lruIt); m_bitmapCache.erase(it); }
                        if (m_lruList.size() >= static_cast<size_t>(app.vramCacheCount)) { m_bitmapCache.erase(m_lruList.back()); m_lruList.pop_back(); }
                        m_lruList.push_front(filePath);
                        auto &entry = m_bitmapCache[filePath];
                        entry.bitmap      = newBitmap;
                        entry.lruIt       = m_lruList.begin();
                        entry.width       = screenW;
                        entry.height      = screenH;
                        entry.orientation = 1;
                        entry.gifFrames   = std::move(gifFrames);
                        entry.gifDelays   = std::move(gifDelays);
                    }

                    const bool gifIsCurrent = isMain
                        ? (app.wantedPathHash.load(std::memory_order_acquire) == pathHash)
                        : (app.wantedIndex.load(std::memory_order_acquire)    == requestIndex);
                    if (gifIsCurrent) {
                        long long start = ImageLoadStats::g_loadStartUs.load(std::memory_order_relaxed);
                        if (start > 0) ImageLoadStats::g_lastLoadUs.store(ImageLoadStats::NowUs() - start, std::memory_order_relaxed);
                        PostMessageW(m_hwnd, Constants::WM_QIV_REPAINT, 0, 0);
                    } else {
                        PostMessageW(m_hwnd, Constants::WM_QIV_REPAINT, 1, 0);
                    }
                    return; // GIF path handled everything — skip normal cache-store below
                }

                // ── Single-frame WIC path (non-GIF or static GIF) ─────────────
                Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
                if (FAILED(decoder->GetFrame(0, &frame))) return;

                frame->GetSize(&width, &height);

                // Read EXIF orientation (tag 274) — no extra I/O, bytes are in RAM
                {
                    Microsoft::WRL::ComPtr<IWICMetadataQueryReader> metaRdr;
                    if (SUCCEEDED(frame->GetMetadataQueryReader(&metaRdr))) {
                        PROPVARIANT pv;
                        PropVariantInit(&pv);
                        if (FAILED(metaRdr->GetMetadataByName(L"/app1/ifd/{ushort=274}", &pv)) || pv.vt == VT_EMPTY) {
                            PropVariantClear(&pv);
                            PropVariantInit(&pv);
                            metaRdr->GetMetadataByName(L"/ifd/{ushort=274}", &pv);
                        }
                        if      (pv.vt == VT_UI2) orientation = pv.uiVal;
                        else if (pv.vt == VT_UI4) orientation = static_cast<USHORT>(pv.ulVal);
                        PropVariantClear(&pv);
                    }
                }

                IWICBitmapSource *uploadSource = nullptr;
                Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
                WICPixelFormatGUID srcFmt{};
                if (FAILED(frame->GetPixelFormat(&srcFmt))) return;

                if (srcFmt == GUID_WICPixelFormat32bppPBGRA) {
                    uploadSource = frame.Get();
                } else {
                    if (FAILED(wicFac->CreateFormatConverter(&converter))) return;
                    if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                                                     WICBitmapDitherTypeNone, nullptr, 0.0f,
                                                     WICBitmapPaletteTypeCustom))) return;
                    uploadSource = converter.Get();
                }

                if (FAILED(taskCtx->CreateBitmapFromWicBitmap(uploadSource, nullptr, &newBitmap))) return;
            }

            {
                std::lock_guard<std::mutex> lock(m_cacheMutex);
                auto it = m_bitmapCache.find(filePath);
                if (it != m_bitmapCache.end()) {
                    m_lruList.erase(it->second.lruIt);
                    m_bitmapCache.erase(it);
                }
                if (m_lruList.size() >= static_cast<size_t>(app.vramCacheCount)) {
                    m_bitmapCache.erase(m_lruList.back());
                    m_lruList.pop_back();
                }
                m_lruList.push_front(filePath);
                m_bitmapCache[filePath] = {newBitmap, m_lruList.begin(), width, height, orientation};
            }

            const bool isCurrent = isMain
                ? (app.wantedPathHash.load(std::memory_order_acquire) == pathHash)
                : (app.wantedIndex.load(std::memory_order_acquire)    == requestIndex);
            if (isCurrent) {
                long long start = ImageLoadStats::g_loadStartUs.load(std::memory_order_relaxed);
                if (start > 0) {
                    ImageLoadStats::g_lastLoadUs.store(
                        ImageLoadStats::NowUs() - start, std::memory_order_relaxed);
                }
                PostMessageW(m_hwnd, Constants::WM_QIV_REPAINT, 0, 0);
            } else {
                PostMessageW(m_hwnd, Constants::WM_QIV_REPAINT, 1, 0);
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

        // Slideshow fade transitions dim the IMAGE, not the window. A layer is
        // used (rather than DrawBitmap's opacity arg) because it also covers the
        // DrawImage effect path, and it leaves the cleared background and the
        // overlays — drawn after PopLayer — fully opaque.
        const float transitionAlpha =
                std::clamp(app.slideshow.transition.alpha, 0.0f, 1.0f);
        const bool useAlphaLayer = (transitionAlpha < 0.999f);
        if (useAlphaLayer) {
            m_pDeviceContext->PushLayer(
                    D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr,
                                           D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                           D2D1::IdentityMatrix(), transitionAlpha),
                    nullptr);
        }

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

        if (useAlphaLayer) m_pDeviceContext->PopLayer();

        m_pDeviceContext->SetTransform(D2D1::Matrix3x2F::Identity());
        g_overlayManager.UpdateZoom(app.viewport.zoom, m_hwnd);
        g_overlayManager.RenderAll(m_pDeviceContext.Get());
    }

    // Persistent overlay: "Directory Missing" (red) or "No Images" (normal).
    // Shown on a black viewport; stays until the user opens a new folder.
    // Format + layout are DWrite objects — created once and cached; they survive
    // device loss and resize, so per-frame cost is a single DrawTextLayout call.
    if (app.folderOverlay != AppState::FolderOverlayState::None
        && m_pFolderDeletedBrush && m_pDWriteFactory) {

        if (!m_pFolderOverlayFormat) {
            (void) m_pDWriteFactory->CreateTextFormat(
                Constants::Overlay::MSG_CENTER__FONT_FAMILY_DEFAULT, nullptr,
                DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                Constants::Overlay::MSG_CENTER_FONT_SIZE * app.dpiScale,
                Constants::Overlay::MSG_ALL_FONT_LOCALE,
                m_pFolderOverlayFormat.GetAddressOf());
        }

        const bool isMissing = (app.folderOverlay == AppState::FolderOverlayState::Missing);
        // Composite cache key — layout must rebuild when state or path changes.
        std::wstring key = (isMissing ? L"M:" : L"E:") + app.folderOverlayPath;

        if (m_pFolderOverlayFormat &&
            (m_lastFolderOverlayKey != key || !m_pFolderDeletedLayout)) {
            m_pFolderDeletedLayout.Reset();
            m_lastFolderOverlayKey = key;
            const wchar_t *header = isMissing ? Constants::Messages::EMPTY_DIR_MISSING
                                              : Constants::Messages::EMPTY_DIR_NO_IMAGES;
            std::wstring msg = std::wstring(header) + L"\n" + app.folderOverlayPath;
            const D2D1_SIZE_F sz = m_pDeviceContext->GetSize();
            m_pDWriteFactory->CreateTextLayout(
                msg.c_str(), static_cast<UINT32>(msg.size()),
                m_pFolderOverlayFormat.Get(), sz.width, sz.height,
                m_pFolderDeletedLayout.GetAddressOf());
            if (m_pFolderDeletedLayout) {
                m_pFolderDeletedLayout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                m_pFolderDeletedLayout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                m_pFolderDeletedLayout->SetWordWrapping(DWRITE_WORD_WRAPPING_EMERGENCY_BREAK);

                // Style the path line as an app-standard clickable link
                // (color + underline come from Constants::Links).
                DWRITE_TEXT_RANGE linkRange = {
                    static_cast<UINT32>(wcslen(header)) + 1, // +1 = '\n'
                    static_cast<UINT32>(app.folderOverlayPath.size())
                };
                if (m_pLinkBrush)
                    m_pFolderDeletedLayout->SetDrawingEffect(m_pLinkBrush.Get(), linkRange);
                if (Constants::Links::UNDERLINE)
                    m_pFolderDeletedLayout->SetUnderline(TRUE, linkRange);
            }
        }

        if (m_pFolderDeletedLayout) {
            ID2D1SolidColorBrush *brush = isMissing ? m_pFolderDeletedBrush.Get()
                                                    : m_pTextBrush.Get();
            m_pDeviceContext->DrawTextLayout(
                D2D1::Point2F(0.0f, 0.0f),
                m_pFolderDeletedLayout.Get(),
                brush);

            // Hit-test the path line (second line) so MouseHandler can make it
            // clickable — layout is drawn at (0,0), so metrics are client coords.
            const wchar_t *header = isMissing ? Constants::Messages::EMPTY_DIR_MISSING
                                              : Constants::Messages::EMPTY_DIR_NO_IMAGES;
            const UINT32 pathStart = static_cast<UINT32>(wcslen(header)) + 1; // +1 = '\n'
            const UINT32 pathLen   = static_cast<UINT32>(app.folderOverlayPath.size());
            DWRITE_HIT_TEST_METRICS htm[8];
            UINT32 htmCount = 0;
            if (pathLen > 0 &&
                SUCCEEDED(m_pFolderDeletedLayout->HitTestTextRange(
                    pathStart, pathLen, 0.0f, 0.0f, htm, 8, &htmCount)) &&
                htmCount > 0) {
                D2D1_RECT_F r = D2D1::RectF(htm[0].left, htm[0].top,
                                            htm[0].left + htm[0].width,
                                            htm[0].top + htm[0].height);
                for (UINT32 i = 1; i < htmCount; ++i) {
                    r.left   = std::min(r.left,   htm[i].left);
                    r.top    = std::min(r.top,    htm[i].top);
                    r.right  = std::max(r.right,  htm[i].left + htm[i].width);
                    r.bottom = std::max(r.bottom, htm[i].top  + htm[i].height);
                }
                app.folderOverlayPathRect = r;
            }
        }
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
//  DecodeSvgToBitmap  — worker-safe: parse + rasterize (resvg) + GPU upload.
//  Writes no shared state. Runs on a decoder worker thread using the injected
//  per-thread WIC factory; the caller inserts outBmp into m_bitmapCache.
// =============================================================================
HRESULT RendererD2D::DecodeSvgToBitmap(const std::vector<BYTE> &svgBytes,
                                       IWICImagingFactory2 *wicFac,
                                       Microsoft::WRL::ComPtr<ID2D1Bitmap1> &outBmp,
                                       UINT &outW, UINT &outH) {
    if (svgBytes.empty()) return E_INVALIDARG;
    if (!m_pD2DDevice || !wicFac) return E_UNEXPECTED;

    // --- resvg options: initialized once (system font scan is IO-heavy) ---
    // call_once is thread-safe; the first SVG on any worker pays the cost once.
    static resvg_options* s_opts = nullptr;
    static std::once_flag  s_once;
    std::call_once(s_once, []() {
        s_opts = resvg_options_create();
        if (s_opts) resvg_options_load_system_fonts(s_opts);
    });
    if (!s_opts) return E_FAIL;

    // Parse SVG bytes
    resvg_render_tree* tree = nullptr;
    const int32_t parseErr = resvg_parse_tree_from_data(
        reinterpret_cast<const char*>(svgBytes.data()),
        svgBytes.size(),
        s_opts,
        &tree);
    if (parseErr != RESVG_OK || !tree) return E_FAIL;

    // Determine raster size (native SVG units, capped at 8192 px)
    resvg_size sz = resvg_get_image_size(tree);
    uint32_t w = (sz.width  > 0.5f) ? static_cast<uint32_t>(sz.width  + 0.5f) : 1920u;
    uint32_t h = (sz.height > 0.5f) ? static_cast<uint32_t>(sz.height + 0.5f) : 1080u;
    constexpr uint32_t kMaxDim = 8192u;
    if (w > kMaxDim || h > kMaxDim) {
        const float s = std::min(static_cast<float>(kMaxDim) / w,
                                 static_cast<float>(kMaxDim) / h);
        w = std::max(1u, static_cast<uint32_t>(w * s));
        h = std::max(1u, static_cast<uint32_t>(h * s));
    }

    // Rasterize: resvg outputs premultiplied RGBA8888
    const size_t bufBytes = static_cast<size_t>(w) * h * 4;
    std::vector<char> rgba(bufBytes, 0);
    resvg_render(tree, resvg_transform_identity(), w, h, rgba.data());
    resvg_tree_destroy(tree);

    // Swap R↔B to get premultiplied BGRA (D2D native format)
    auto* p = reinterpret_cast<BYTE*>(rgba.data());
    for (size_t i = 0; i < bufBytes; i += 4)
        std::swap(p[i], p[i + 2]);

    // Create WIC bitmap wrapper around the pixel buffer
    Microsoft::WRL::ComPtr<IWICBitmap> wicBmp;
    HRESULT hr = wicFac->CreateBitmapFromMemory(
        w, h,
        GUID_WICPixelFormat32bppPBGRA,
        w * 4, static_cast<UINT>(bufBytes),
        p,
        &wicBmp);
    if (FAILED(hr)) return hr;

    // Upload to VRAM using a short-lived device context (thread-safe)
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> uploadCtx;
    hr = m_pD2DDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
                                           uploadCtx.GetAddressOf());
    if (FAILED(hr)) return hr;

    hr = uploadCtx->CreateBitmapFromWicBitmap(wicBmp.Get(), nullptr, &outBmp);
    if (FAILED(hr)) return hr;

    outW = w;
    outH = h;
    return S_OK;
}

// =============================================================================
//  PreloadSvgFromBytes  — async SVG entry point.
//  Rasterizes + uploads on the decoder worker, inserts into the shared bitmap
//  cache, then posts WM_QIV_REPAINT so the UI thread displays it through the
//  same cache-hit path as raster images. Never blocks the UI thread.
// =============================================================================
HRESULT RendererD2D::PreloadSvgFromBytes(std::vector<BYTE> svgBytes,
                                         const std::wstring &filePath,
                                         int requestIndex) {
    if (svgBytes.empty()) return E_INVALIDARG;
    if (!m_pD2DDevice) return E_UNEXPECTED;

    {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        // Already rasterized on a previous visit — display via the cache-hit path.
        if (m_bitmapCache.find(filePath) != m_bitmapCache.end()) {
            PostMessageW(m_hwnd, Constants::WM_QIV_REPAINT, 0, 0);
            return S_OK;
        }
        // A rasterization for this path is already queued — dedupe.
        if (m_bitmapInFlight.count(filePath)) return S_OK;
        m_bitmapInFlight.insert(filePath);
    }

    // SVGs are always the current main image here — guard by path identity so the
    // post-open folder re-sort (which renumbers indices) can't cancel this decode.
    (void) requestIndex;
    const size_t pathHash = std::hash<std::wstring>{}(filePath);

    g_decoderWorker.PushTask(
        [this, svgBytes = std::move(svgBytes), filePath, pathHash]
        (IWICImagingFactory2 *wicFac) mutable {
            // Release the in-flight marker up front (mirrors PreloadBitmap) so a
            // later request for the same path can be queued while this one runs.
            { std::lock_guard<std::mutex> lock(m_cacheMutex); m_bitmapInFlight.erase(filePath); }

            // User navigated away while the task was queued — drop the work.
            if (app.wantedPathHash.load(std::memory_order_acquire) != pathHash)
                return;

            Microsoft::WRL::ComPtr<ID2D1Bitmap1> d2dBmp;
            UINT w = 0, h = 0;
            if (FAILED(DecodeSvgToBitmap(svgBytes, wicFac, d2dBmp, w, h)))
                return;

            // Insert into bitmap cache (same LRU as regular images).
            {
                std::lock_guard<std::mutex> lock(m_cacheMutex);
                auto it = m_bitmapCache.find(filePath);
                if (it != m_bitmapCache.end()) {
                    m_lruList.erase(it->second.lruIt);
                    m_bitmapCache.erase(it);
                }
                if (m_lruList.size() >= static_cast<size_t>(app.vramCacheCount)) {
                    m_bitmapCache.erase(m_lruList.back());
                    m_lruList.pop_back();
                }
                m_lruList.push_front(filePath);
                m_bitmapCache[filePath] = {d2dBmp, m_lruList.begin(), w, h};
            }

            // Finalize on the UI thread only if the user is still on this image.
            if (app.wantedPathHash.load(std::memory_order_acquire) == pathHash)
                PostMessageW(m_hwnd, Constants::WM_QIV_REPAINT, 0, 0);
        });

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

USHORT RendererD2D::GetCachedOrientation(const std::wstring &filePath) {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    auto it = m_bitmapCache.find(filePath);
    return (it != m_bitmapCache.end()) ? it->second.orientation : 1;
}

void RendererD2D::ClearCache() {
    ClearCache(L"");
}

void RendererD2D::ClearCache(const std::wstring &excludePath) {
    std::lock_guard<std::mutex> lock(m_cacheMutex);

    if (excludePath.empty()) {
        m_bitmapCache.clear();
        m_lruList.clear();
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
}

void RendererD2D::RemoveFromCache(const std::wstring &filePath) {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    auto it = m_bitmapCache.find(filePath);
    if (it != m_bitmapCache.end()) {
        m_lruList.erase(it->second.lruIt);
        m_bitmapCache.erase(it);
    }
}

void RendererD2D::GetImageCacheStats(int &count, UINT64 &estimatedBytes) {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    count = static_cast<int>(m_bitmapCache.size());
    estimatedBytes = 0;
    for (auto &[path, entry] : m_bitmapCache)
        estimatedBytes += static_cast<UINT64>(entry.width) * entry.height * 4;
}

void RendererD2D::GetDirThumbCacheStats(int &count, UINT64 &estimatedBytes) {
    std::lock_guard<std::mutex> lock(m_dirThumbMutex);
    count = 0;
    for (auto &[hwnd, panel] : m_panelThumbCaches)
        count += static_cast<int>(panel.bitmaps.size());
    UINT64 physW = static_cast<UINT64>(Constants::THUMBNAIL_PANEL_THUMB_WIDTH  * app.dpiScale + 0.5f);
    UINT64 physH = static_cast<UINT64>(Constants::THUMBNAIL_PANEL_THUMB_HEIGHT * app.dpiScale + 0.5f);
    estimatedBytes = static_cast<UINT64>(count) * physW * physH * 4;
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
    const D2D1_SIZE_U imgSize = m_pBitmap->GetPixelSize();
    if (imgSize.width == 0 || imgSize.height == 0) return E_FAIL;

    // Bake rotation + flip into the output: 90°/270° swaps width and height.
    const int  rot   = app.viewport.rotation;
    const bool flipH = app.viewport.flippedH;
    const bool flipV = app.viewport.flippedV;
    const bool swapDims = (rot == 90 || rot == 270);
    const UINT outW = swapDims ? imgSize.height : imgSize.width;
    const UINT outH = swapDims ? imgSize.width  : imgSize.height;

    D3D11_TEXTURE2D_DESC texDesc{};
    texDesc.Width  = outW;
    texDesc.Height = outH;
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

    // Build transform that maps the source bitmap to the output canvas,
    // mirroring the same flip-then-rotate order used in Render().
    {
        const D2D1_POINT_2F origin = D2D1::Point2F(0.0f, 0.0f);
        D2D1_MATRIX_3X2_F tr = D2D1::Matrix3x2F::Translation(
            -static_cast<float>(imgSize.width)  / 2.0f,
            -static_cast<float>(imgSize.height) / 2.0f);
        if (flipH) tr = tr * D2D1::Matrix3x2F::Scale(-1.0f,  1.0f, origin);
        if (flipV) tr = tr * D2D1::Matrix3x2F::Scale( 1.0f, -1.0f, origin);
        if (rot != 0) tr = tr * D2D1::Matrix3x2F::Rotation(static_cast<float>(rot), origin);
        tr = tr * D2D1::Matrix3x2F::Translation(outW / 2.0f, outH / 2.0f);
        m_pDeviceContext->SetTransform(tr);
    }

    ID2D1Effect *finalEffect = BuildEffectChain(m_pBitmap.Get());
    if (finalEffect) {
        m_pDeviceContext->DrawImage(finalEffect, D2D1::Point2F(0.0f, 0.0f));
    } else {
        m_pDeviceContext->DrawBitmap(
                m_pBitmap.Get(),
                D2D1::RectF(0.0f, 0.0f,
                            static_cast<float>(imgSize.width),
                            static_cast<float>(imgSize.height)),
                1.0f,
                D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
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

    const UINT rowBytes = outW * 4;
    std::vector<BYTE> pixels(static_cast<size_t>(outW) * outH * 4);
    for (UINT row = 0; row < outH; ++row) {
        memcpy(pixels.data() + static_cast<size_t>(row) * rowBytes,
               static_cast<const BYTE *>(mapped.pData) + static_cast<size_t>(row) * mapped.RowPitch,
               rowBytes);
    }
    m_pD3DContext->Unmap(stagingTex.Get(), 0);

    Microsoft::WRL::ComPtr<IWICImagingFactory> wicFac = app.wicFactory;
    if (!wicFac) return E_FAIL;

    // Map file extension → WIC container GUID.
    GUID containerFmt = GUID_ContainerFormatPng; // default
    bool isJpeg = false;
    {
        auto dot = outPath.rfind(L'.');
        if (dot != std::wstring::npos) {
            std::wstring ext = outPath.substr(dot + 1);
            for (auto &c : ext) c = static_cast<wchar_t>(towlower(c));
            if      (ext == L"jpg" || ext == L"jpeg") { containerFmt = GUID_ContainerFormatJpeg; isJpeg = true; }
            else if (ext == L"bmp")                    { containerFmt = GUID_ContainerFormatBmp;  }
            else if (ext == L"tif" || ext == L"tiff")  { containerFmt = GUID_ContainerFormatTiff; }
            else if (ext == L"gif")                    { containerFmt = GUID_ContainerFormatGif;  }
            // png and everything else → default GUID_ContainerFormatPng
        }
    }

    Microsoft::WRL::ComPtr<IWICStream> wicStream;
    hr = wicFac->CreateStream(&wicStream);
    if (FAILED(hr)) return hr;

    hr = wicStream->InitializeFromFilename(outPath.c_str(), GENERIC_WRITE);
    if (FAILED(hr)) return hr;

    Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
    hr = wicFac->CreateEncoder(containerFmt, nullptr, &encoder);
    if (FAILED(hr)) return hr;

    hr = encoder->Initialize(wicStream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(hr)) return hr;

    Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
    Microsoft::WRL::ComPtr<IPropertyBag2> frameProps;
    hr = encoder->CreateNewFrame(&frame, &frameProps);
    if (FAILED(hr)) return hr;

    if (isJpeg) {
        // Set JPEG quality to 92%.
        PROPBAG2 qualityOpt{};
        qualityOpt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT qualityVal{};
        qualityVal.vt = VT_R4;
        qualityVal.fltVal = 0.92f;
        frameProps->Write(1, &qualityOpt, &qualityVal);
    }

    hr = frame->Initialize(frameProps.Get());
    if (FAILED(hr)) return hr;

    hr = frame->SetSize(outW, outH);
    if (FAILED(hr)) return hr;

    if (isJpeg) {
        // JPEG has no alpha channel — pack BGRA pixels down to BGR24.
        WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
        hr = frame->SetPixelFormat(&fmt);
        if (FAILED(hr)) return hr;

        const UINT jpegStride = outW * 3;
        std::vector<BYTE> jpegPx(static_cast<size_t>(jpegStride) * outH);
        const UINT srcPx = outW * outH;
        for (UINT i = 0; i < srcPx; ++i) {
            jpegPx[i * 3 + 0] = pixels[i * 4 + 0]; // B
            jpegPx[i * 3 + 1] = pixels[i * 4 + 1]; // G
            jpegPx[i * 3 + 2] = pixels[i * 4 + 2]; // R
        }
        hr = frame->WritePixels(outH, jpegStride,
                                static_cast<UINT>(jpegPx.size()), jpegPx.data());
    } else {
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        hr = frame->SetPixelFormat(&fmt);
        if (FAILED(hr)) return hr;

        hr = frame->WritePixels(outH, rowBytes,
                                static_cast<UINT>(pixels.size()), pixels.data());
    }
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
//  RequestDirThumbnail  —  queries the Windows Shell thumbnail cache.
//  Windows generates and persistently caches the thumbnail on first access;
//  subsequent requests for the same file are near-instant cache lookups.
//  The SHELL_THUMB_FLAGS constant controls caching/generation behavior.
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
    if (!d2dDev || !hPanel) return;

    const LONG  thumbW    = static_cast<LONG>(Constants::THUMBNAIL_PANEL_THUMB_WIDTH  * app.dpiScale);
    const LONG  thumbH    = static_cast<LONG>(Constants::THUMBNAIL_PANEL_THUMB_HEIGHT * app.dpiScale);
    const float dpiScale  = app.dpiScale;
    HWND hDir = hPanel;

    g_dirThumbWorker.PushTask([filePath, d2dDev, hDir, thumbW, thumbH, dpiScale, this](IWICImagingFactory2 *wicFac) {
        auto releaseInFlight = [&] {
            std::lock_guard<std::mutex> lk(m_dirThumbMutex);
            m_panelThumbCaches[hDir].inFlight.erase(filePath);
        };

        // 1. Ask Windows Shell for the thumbnail — generates and caches if not present.
        Microsoft::WRL::ComPtr<IShellItem> shellItem;
        if (FAILED(SHCreateItemFromParsingName(filePath.c_str(), nullptr, IID_PPV_ARGS(&shellItem))))
            { releaseInFlight(); return; }

        Microsoft::WRL::ComPtr<IShellItemImageFactory> imgFactory;
        if (FAILED(shellItem->QueryInterface(IID_PPV_ARGS(&imgFactory))))
            { releaseInFlight(); return; }

        HBITMAP hBitmap = nullptr;
        const SIZE sz = { thumbW, thumbH };
        if (FAILED(imgFactory->GetImage(sz, static_cast<SIIGBF>(Constants::SHELL_THUMB_FLAGS), &hBitmap)) || !hBitmap)
            { releaseInFlight(); return; }

        // 2. HBITMAP → IWICBitmap (pixel copy — DeleteObject immediately after).
        Microsoft::WRL::ComPtr<IWICBitmap> wicBmp;
        HRESULT hr = wicFac->CreateBitmapFromHBITMAP(hBitmap, nullptr, WICBitmapIgnoreAlpha, &wicBmp);
        DeleteObject(hBitmap);
        if (FAILED(hr)) { releaseInFlight(); return; }

        // 3. Convert to PBGRA (required by D2D).
        Microsoft::WRL::ComPtr<IWICFormatConverter> conv;
        if (FAILED(wicFac->CreateFormatConverter(&conv)) ||
            FAILED(conv->Initialize(wicBmp.Get(), GUID_WICPixelFormat32bppPBGRA,
                                    WICBitmapDitherTypeNone, nullptr, 0.0f, WICBitmapPaletteTypeCustom)))
            { releaseInFlight(); return; }

        // 4. Upload to D2D.
        Microsoft::WRL::ComPtr<ID2D1DeviceContext> taskCtx;
        if (FAILED(d2dDev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, taskCtx.GetAddressOf())))
            { releaseInFlight(); return; }

        D2D1_BITMAP_PROPERTIES1 bmpProps{};
        bmpProps.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
        bmpProps.dpiX = dpiScale * 96.0f;
        bmpProps.dpiY = dpiScale * 96.0f;

        Microsoft::WRL::ComPtr<ID2D1Bitmap1> thumbBitmap;
        if (FAILED(taskCtx->CreateBitmapFromWicBitmap(conv.Get(), &bmpProps, &thumbBitmap)))
            { releaseInFlight(); return; }

        {
            std::lock_guard<std::mutex> lk(m_dirThumbMutex);
            auto &entry = m_panelThumbCaches[hDir];
            entry.inFlight.erase(filePath);
            entry.bitmaps.try_emplace(filePath, thumbBitmap);
        }
        PostMessageW(hDir, Constants::WM_QIV_REPAINT, 0, 0);
    });
}
