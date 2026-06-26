#include "WicDecoder.h"
#include "AppState.h"

using Microsoft::WRL::ComPtr;

void LoadImageIndex(HWND hWnd, int index) {
    if (index < 0 || index >= static_cast<int>(g_app.playlist.size())) return;
    
    if (g_app.hDIB) {
        DeleteObject(g_app.hDIB);
        g_app.hDIB = nullptr;
    }

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
    g_app.imgWidth = static_cast<int>(width);
    g_app.imgHeight = static_cast<int>(height);

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = g_app.imgWidth;
    bmi.bmiHeader.biHeight = -g_app.imgHeight;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pPixels = nullptr;
    HDC hdcScreen = GetDC(nullptr);
    g_app.hDIB = CreateDIBSection(hdcScreen, &bmi, DIB_RGB_COLORS, &pPixels, nullptr, 0);
    ReleaseDC(nullptr, hdcScreen);

    if (g_app.hDIB && pPixels) {
        converter->CopyPixels(nullptr, width * 4, width * 4 * height, static_cast<BYTE*>(pPixels));
    }

    g_app.viewport = ViewportState{}; 
    InvalidateRect(hWnd, nullptr, FALSE);
}