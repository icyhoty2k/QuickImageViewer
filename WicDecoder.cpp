#include "WicDecoder.h"
#include "AppState.h"

using Microsoft::WRL::ComPtr;

void LoadImageIndex(HWND hWnd, int index) {
    if (index < 0 || index >= static_cast<int>(g_app.playlist.size())) return;

    g_app.currentIndex = index;
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(g_app.wicFactory->CreateDecoderFromFilename(
            g_app.playlist[g_app.currentIndex].c_str(), nullptr, GENERIC_READ,
            WICDecodeMetadataCacheOnDemand, &decoder))) return;

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) return;

    ComPtr<IWICFormatConverter> converter;
    g_app.wicFactory->CreateFormatConverter(&converter);
    converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                          WICBitmapDitherTypeNone, nullptr, 0.0f, WICBitmapPaletteTypeCustom);

    UINT width = 0, height = 0;
    converter->GetSize(&width, &height);

    // Delegate bitmap loading to the renderer
    if (g_app.renderer) {
        g_app.renderer->LoadBitmap(converter.Get(), width, height);
    }

    g_app.viewport = ViewportState{}; 
    InvalidateRect(hWnd, nullptr, FALSE);
}