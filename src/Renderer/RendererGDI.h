#pragma once

#include "IRenderer.h"
#include <string>

class RendererGDI final : public IImageRenderer {
    public:
        RendererGDI();

        ~RendererGDI() override;

        [[nodiscard]] HRESULT Initialize(HWND hwnd) override;

        void Resize(UINT width, UINT height) override;

        [[nodiscard]] HRESULT LoadBitmap(
                IWICBitmapSource *bitmap,
                UINT width,
                UINT height,
                const std::wstring &filePath) override;

        [[nodiscard]] HRESULT Render() override;

        [[nodiscard]] HRESULT PreloadBitmap(const std::wstring &filePath, int requestIndex) override;

        void ProcessPendingUploads() override;

        void UpdateColorEffects() override;

        void UpdateTextFormat() override;

        void ApplyPreviousEffects() override {
            // GDI does not use the effect pipeline, so this is just an empty implementation.
        }

    private:
        void DestroyBackBuffer();

        [[nodiscard]] HRESULT CreateBackBuffer(UINT width, UINT height);

    private:
        HWND m_hwnd = nullptr;
        UINT m_windowWidth = 0;
        UINT m_windowHeight = 0;
        UINT m_imageWidth = 0;
        UINT m_imageHeight = 0;

        HDC m_backDC = nullptr;
        HBITMAP m_backBitmap = nullptr;
        HBITMAP m_backBitmapOld = nullptr;
        HBRUSH m_backgroundBrush = nullptr;
};
