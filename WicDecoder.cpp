#include "WicDecoder.h"
#include "AppState.h"
#include "WorkerThread.h"

using Microsoft::WRL::ComPtr;

void LoadImageIndex(HWND hWnd, int index) {
    // 1. Clear previous background work to pivot to the new image
    g_decoderWorker.ClearQueue();
    if (index < 0 || index >= static_cast<int>(g_app.playlist.size())) return;

    g_app.currentIndex = index;
    const std::wstring& currentPath = g_app.playlist[g_app.currentIndex]; // Get path once

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(g_app.wicFactory->CreateDecoderFromFilename(
            currentPath.c_str(), nullptr, GENERIC_READ,
            WICDecodeMetadataCacheOnDemand, &decoder))) return;

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) return;

    ComPtr<IWICFormatConverter> converter;
    g_app.wicFactory->CreateFormatConverter(&converter);
    converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                          WICBitmapDitherTypeNone, nullptr, 0.0f, WICBitmapPaletteTypeCustom);

    UINT width = 0, height = 0;
    converter->GetSize(&width, &height);

    // Delegate bitmap loading to the renderer with the filePath
    if (g_app.renderer) {
        g_app.renderer->LoadBitmap(converter.Get(), width, height, currentPath);
    }
    // 2. Preload the next image in the playlist
    int nextIndex = index + 1;
    if (nextIndex < static_cast<int>(g_app.playlist.size())) {
        g_decoderWorker.PushTask([nextIndex]() {
     g_app.renderer->PreloadBitmap(g_app.playlist[nextIndex]);
 });
    }
    g_app.viewport = ViewportState{}; 
    InvalidateRect(hWnd, nullptr, FALSE);
}
