#include "WicDecoder.h"

#include <windows.h>
#include <mutex>


using Microsoft::WRL::ComPtr;


static ComPtr<IWICImagingFactory2> GetWICFactory() {
    static ComPtr<IWICImagingFactory2> factory;
    static std::once_flag flag;

    std::call_once(flag, []() {
        HRESULT hr = CoCreateInstance(
                CLSID_WICImagingFactory2,
                nullptr,
                CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&factory)
                );

        if (FAILED(hr)) {
            OutputDebugStringW(
                    L"WIC2: Failed creating Imaging Factory\n"
                    );
        }
    });

    return factory;
}


HRESULT WicDecoder::DecodeImage(
        const std::wstring &filePath,
        DecodedImage &result
        ) {
    auto factory = GetWICFactory();

    if (!factory)
        return E_FAIL;


    //
    // Create decoder
    //
    ComPtr<IWICBitmapDecoder> decoder;

    // WICDecodeMetadataCacheOnDemand: defer metadata loading until it is
    // explicitly requested. We only read pixel data here, so this avoids
    // wasting time and RAM on EXIF/XMP/IPTC blocks we never touch.
    HRESULT hr = factory->CreateDecoderFromFilename(
            filePath.c_str(),
            nullptr,
            GENERIC_READ,
            WICDecodeMetadataCacheOnDemand,
            &decoder
            );

    if (FAILED(hr)) {
        OutputDebugStringW(
                L"WIC2: Decoder creation failed\n"
                );

        return hr;
    }


    //
    // First frame
    //
    ComPtr<IWICBitmapFrameDecode> frame;

    hr = decoder->GetFrame(
            0,
            &frame
            );

    if (FAILED(hr)) {
        OutputDebugStringW(
                L"WIC2: Frame decode failed\n"
                );

        return hr;
    }


    //
    // Get dimensions BEFORE conversion
    //
    hr = frame->GetSize(
            &result.width,
            &result.height
            );

    if (FAILED(hr))
        return hr;


    //
    // Check source pixel format — skip converter if already 32bppPBGRA
    //
    WICPixelFormatGUID sourceFormat{};
    hr = frame->GetPixelFormat(&sourceFormat);
    if (FAILED(hr))
        return hr;

    // If source is already 32bppPBGRA, use it directly
    if (sourceFormat == GUID_WICPixelFormat32bppPBGRA) {
        hr = factory->CreateBitmapFromSource(
                frame.Get(),
                WICBitmapCacheOnLoad,
                &result.bitmap
                );

        if (FAILED(hr)) {
            OutputDebugStringW(
                    L"WIC2: Bitmap creation failed (native format)\n"
                    );
            return hr;
        }

        return S_OK;
    }


    //
    // Convert to Direct2D compatible format (only if needed)
    //
    ComPtr<IWICFormatConverter> converter;

    hr = factory->CreateFormatConverter(
            &converter
            );

    if (FAILED(hr))
        return hr;

    hr = converter->Initialize(
            frame.Get(),

            GUID_WICPixelFormat32bppPBGRA,

            WICBitmapDitherTypeNone,

            nullptr,

            0.0,

            WICBitmapPaletteTypeCustom
            );

    if (FAILED(hr)) {
        OutputDebugStringW(
                L"WIC2: Pixel conversion failed\n"
                );

        return hr;
    }


    //
    // Make a cached RAM bitmap
    //
    hr = factory->CreateBitmapFromSource(
            converter.Get(),

            WICBitmapCacheOnLoad,

            &result.bitmap
            );

    if (FAILED(hr)) {
        OutputDebugStringW(
                L"WIC2: Bitmap creation failed\n"
                );

        return hr;
    }

    return S_OK;
}
