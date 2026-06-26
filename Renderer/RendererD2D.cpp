#include "RendererD2D.h"
#include <d2d1.h>
#include "../AppState.h"

// Link the required libraries directly to ensure the linker finds them
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

RendererD2D::RendererD2D() = default;

RendererD2D::~RendererD2D() {
    // Release COM pointers manually (though ComPtr does this, it's good practice for clarity)
    m_pBitmap.Reset();
    m_pRenderTarget.Reset();
    m_pFactory.Reset();
}

// Create D2D Factory and Render Target
HRESULT RendererD2D::Initialize(HWND hwnd) {
    m_hwnd = hwnd;

    // Create the Direct2D factory
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, m_pFactory.GetAddressOf());

    if (SUCCEEDED(hr)) {
        RECT rc;
        GetClientRect(m_hwnd, &rc);

        // Create the window render target
        hr = m_pFactory->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(),
            D2D1::HwndRenderTargetProperties(m_hwnd, D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top)),
            m_pRenderTarget.GetAddressOf()
        );
    }
    return hr;
}

// Update the render target size when window is resized
void RendererD2D::Resize(UINT width, UINT height) {
    if (m_pRenderTarget) {
        m_pRenderTarget->Resize(D2D1::SizeU(width, height));
    }
}

// Convert the WIC bitmap into a GPU-side D2D Bitmap
HRESULT RendererD2D::LoadBitmap(IWICBitmapSource* bitmap, UINT width, UINT height) {
    if (!m_pRenderTarget) return E_FAIL;

    // Clear old bitmap before creating new one
    m_pBitmap.Reset();

    // Create D2D bitmap directly from the WIC source
    return m_pRenderTarget->CreateBitmapFromWicBitmap(bitmap, nullptr, m_pBitmap.GetAddressOf());
}

// Render the bitmap to the screen using Direct2D
HRESULT RendererD2D::Render() {
    if (!m_pRenderTarget) return E_FAIL;

    m_pRenderTarget->BeginDraw();
    m_pRenderTarget->Clear(m_clearColor);

    if (m_pBitmap) {
        D2D1_SIZE_F rtSize = m_pRenderTarget->GetSize();

        // Calculate aspect ratio
        float imageW = (float)m_pBitmap->GetSize().width;
        float imageH = (float)m_pBitmap->GetSize().height;

        float ratioX = rtSize.width / imageW;
        float ratioY = rtSize.height / imageH;
        float base = (std::min)(ratioX, ratioY);

        // Apply zoom. Ensure zoom is at least 1.0f if the struct reset failed
        float z = (g_app.viewport.zoom <= 0.0f) ? 1.0f : g_app.viewport.zoom;

        float renderW = imageW * base * z;
        float renderH = imageH * base * z;

        float left = (rtSize.width - renderW) / 2.0f + g_app.viewport.offsetX;
        float top = (rtSize.height - renderH) / 2.0f + g_app.viewport.offsetY;

        D2D1_RECT_F destRect = D2D1::RectF(left, top, left + renderW, top + renderH);
        m_pRenderTarget->DrawBitmap(m_pBitmap.Get(), destRect);
    }
    
    return m_pRenderTarget->EndDraw();
}