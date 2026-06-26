#pragma once

#include "IRenderer.h"
#include <d2d1.h>
#include <wrl/client.h> // For Microsoft::WRL::ComPtr

class RendererD2D final : public IImageRenderer
{
public:
    RendererD2D();
    ~RendererD2D() override;

    [[nodiscard]] HRESULT Initialize(HWND hwnd) override;
    void Resize(UINT width, UINT height) override;
    
    [[nodiscard]] HRESULT LoadBitmap(IWICBitmapSource* bitmap, UINT width, UINT height) override;
    [[nodiscard]] HRESULT Render() override;

private:
    // Direct2D Resources
    Microsoft::WRL::ComPtr<ID2D1Factory> m_pFactory;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> m_pRenderTarget;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> m_pBitmap;

    HWND m_hwnd = nullptr;
    D2D1_COLOR_F m_clearColor = D2D1::ColorF(0.08f, 0.08f, 0.08f); // Your RGB(20,20,20)
};