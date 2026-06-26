// Renderer/IImageRenderer.h
#pragma once

#include <windows.h>
#include <wincodec.h>

class IImageRenderer
{
public:
    IImageRenderer() = default;
    virtual ~IImageRenderer() = default;

    IImageRenderer(const IImageRenderer&) = delete;
    IImageRenderer& operator=(const IImageRenderer&) = delete;
    IImageRenderer(IImageRenderer&&) = delete;
    IImageRenderer& operator=(IImageRenderer&&) = delete;

    /// Initialize the renderer for the specified window.
    [[nodiscard]]
    virtual HRESULT Initialize(HWND hwnd) = 0;

    /// Called whenever the client area size changes.
    virtual void Resize(UINT width, UINT height) = 0;

    /// Loads a decoded WIC bitmap into the renderer.
    /// Ownership of the bitmap remains with the caller.
    [[nodiscard]]
    virtual HRESULT LoadBitmap(
        IWICBitmapSource* bitmap,
        UINT width,
        UINT height) = 0;

    /// Paints the current image.
    [[nodiscard]]
    virtual HRESULT Render() = 0;
};