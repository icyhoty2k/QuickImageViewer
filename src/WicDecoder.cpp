#include "../WicDecoder.h"
#include "AppState.h"
#include "../WorkerThread.h"
#include "Constants.h"

using Microsoft::WRL::ComPtr;

void LoadImageIndex(HWND hWnd, int index) {
    // 1. Cancel any pending background preload work
    g_decoderWorker.ClearQueue();
    if (index < 0 || index >= static_cast<int>(g_app.playlist.size())) return;

    g_app.currentIndex = index;
    const std::wstring &currentPath = g_app.playlist[g_app.currentIndex];

    // 2. Probe the Cache (nullptr signals cache probe)
    // If FAILED, we have a Cache Miss and must decode from disk.
    if (g_app.renderer && FAILED(g_app.renderer->LoadBitmap(nullptr, 0, 0, currentPath))) {
        ComPtr<IWICBitmapDecoder> decoder;
        if (FAILED(g_app.wicFactory->CreateDecoderFromFilename(
            currentPath.c_str(), nullptr, GENERIC_READ,
            WICDecodeMetadataCacheOnDemand, &decoder)))
            return;

        ComPtr<IWICBitmapFrameDecode> frame;
        if (FAILED(decoder->GetFrame(0, &frame))) return;

        ComPtr<IWICFormatConverter> converter;
        if (FAILED(g_app.wicFactory->CreateFormatConverter(&converter))) return;

        if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone, nullptr, 0.0f,
            WICBitmapPaletteTypeCustom)))
            return;

        UINT width = 0, height = 0;
        if (FAILED(converter->GetSize(&width, &height))) return;

        // Store image dimensions for pan constraint logic
        g_app.imgWidth = static_cast<int>(width);
        g_app.imgHeight = static_cast<int>(height);

        // Send to renderer (which should also add it to the cache)
        (void) g_app.renderer->LoadBitmap(converter.Get(), width, height, currentPath);
    }

    // 3. Update the Window Title dynamically
    std::wstring fileName = currentPath.substr(currentPath.find_last_of(L"\\/") + 1);
    std::wstring windowTitle = fileName + L" - QuickImageViewer";
    SetWindowTextW(hWnd, windowTitle.c_str());

    // 4. Bidirectional preload
    const int total = static_cast<int>(g_app.playlist.size());
    for (int i = 1; i <= Config::PRELOAD_LOOKASIDE_COUNT; ++i) {
        int fwd = index + i;
        int bwd = index - i;
        if (fwd < total) {
            std::wstring fwdPath = g_app.playlist[fwd];
            g_decoderWorker.PushTask([fwdPath]() {
                if (g_app.renderer) (void) g_app.renderer->PreloadBitmap(fwdPath);
            });
        }
        if (bwd >= 0) {
            std::wstring bwdPath = g_app.playlist[bwd];
            g_decoderWorker.PushTask([bwdPath]() {
                if (g_app.renderer) (void) g_app.renderer->PreloadBitmap(bwdPath);
            });
        }
    }

    g_app.viewport = ViewportState{};
    InvalidateRect(hWnd, nullptr, FALSE);
}
