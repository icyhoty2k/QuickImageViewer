#include "../WicDecoder.h"
#include "AppState.h"
#include "../WorkerThread.h"
#include "Constants.h"

using Microsoft::WRL::ComPtr;

void LoadImageIndex(HWND hWnd, int index) {
    // 1. Cancel any pending background preload work — we are going somewhere new
    g_decoderWorker.ClearQueue();
    if (index < 0 || index >= static_cast<int>(g_app.playlist.size())) return;

    g_app.currentIndex = index;
    const std::wstring &currentPath = g_app.playlist[g_app.currentIndex];

    // 2. Try to serve from VRAM cache first — avoids disk I/O + decode on the UI thread.
    //    Passing nullptr signals a cache-only lookup.
    if (g_app.renderer && SUCCEEDED(g_app.renderer->LoadBitmap(nullptr, 0, 0, currentPath))) {
        // Cache hit: image already in VRAM.
        // Read dimensions cheaply from the file header only (no pixel decode).
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        if (SUCCEEDED(g_app.wicFactory->CreateDecoderFromFilename(
            currentPath.c_str(), nullptr, GENERIC_READ,
            WICDecodeMetadataCacheOnDemand, &decoder)) &&
            SUCCEEDED(decoder->GetFrame(0, &frame))) {
            UINT w = 0, h = 0;
            frame->GetSize(&w, &h);
            g_app.imgWidth = static_cast<int>(w);
            g_app.imgHeight = static_cast<int>(h);
        }
    } else {
        // Cache miss: full decode from disk on the UI thread
        ComPtr<IWICBitmapDecoder> decoder;
        if (FAILED(g_app.wicFactory->CreateDecoderFromFilename(
            currentPath.c_str(), nullptr, GENERIC_READ,
            WICDecodeMetadataCacheOnDemand, &decoder)))
            return;

        ComPtr<IWICBitmapFrameDecode> frame;
        if (FAILED(decoder->GetFrame(0, &frame))) return;

        ComPtr<IWICFormatConverter> converter;
        g_app.wicFactory->CreateFormatConverter(&converter);
        converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                              WICBitmapDitherTypeNone, nullptr, 0.0f, WICBitmapPaletteTypeCustom);

        UINT width = 0, height = 0;
        converter->GetSize(&width, &height);

        // Store image dimensions for pan constraint logic in MouseHandler
        g_app.imgWidth = static_cast<int>(width);
        g_app.imgHeight = static_cast<int>(height);

        if (g_app.renderer) {
            g_app.renderer->LoadBitmap(converter.Get(), width, height, currentPath);
        }
    }

    // 3. Bidirectional preload — uses PRELOAD_LOOKASIDE_COUNT from Constants.h
    const int total = static_cast<int>(g_app.playlist.size());
    for (int i = 1; i <= Config::PRELOAD_LOOKASIDE_COUNT; ++i) {
        int fwd = index + i;
        int bwd = index - i;
        if (fwd < total) {
            std::wstring fwdPath = g_app.playlist[fwd];
            g_decoderWorker.PushTask([fwdPath]() {
                g_app.renderer->PreloadBitmap(fwdPath);
            });
        }
        if (bwd >= 0) {
            std::wstring bwdPath = g_app.playlist[bwd];
            g_decoderWorker.PushTask([bwdPath]() {
                g_app.renderer->PreloadBitmap(bwdPath);
            });
        }
    }

    g_app.viewport = ViewportState{};
    InvalidateRect(hWnd, nullptr, FALSE);
}
