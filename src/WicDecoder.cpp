// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

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
    // Which way up — EXIF tag 274
    //
    // THIS PRODUCES PIXELS, NOT A VIEW, so it has to bake the rotation in. The
    // viewer applies orientation to its VIEWPORT instead
    // (FileHandler::ApplyOrientationToViewport), which turns what is on the
    // glass and leaves the file's pixels alone — correct there, and no help at
    // all to a caller that is about to hand the bytes to somebody else.
    //
    // The one caller is Copy image to clipboard, so the symptom was a photograph
    // that looked upright in the viewer and arrived on its side in whatever it
    // was pasted into. Ordinary for anything straight off a camera, which writes
    // the sensor's pixels unrotated and records the turn in this tag.
    //
    // CacheOnDemand above does not prevent this: it defers metadata until asked
    // rather than discarding it, and this is the asking.
    USHORT orientation = 1;
    {
        ComPtr<IWICMetadataQueryReader> metaRdr;
        if (SUCCEEDED(frame->GetMetadataQueryReader(&metaRdr))) {
            PROPVARIANT pv;
            PropVariantInit(&pv);
            if (FAILED(metaRdr->GetMetadataByName(L"/app1/ifd/{ushort=274}", &pv)) ||
                pv.vt == VT_EMPTY) {
                PropVariantClear(&pv);
                PropVariantInit(&pv);
                metaRdr->GetMetadataByName(L"/ifd/{ushort=274}", &pv);
            }
            if      (pv.vt == VT_UI2) orientation = pv.uiVal;
            else if (pv.vt == VT_UI4) orientation = static_cast<USHORT>(pv.ulVal);
            PropVariantClear(&pv);
        }
    }

    // The same eight cases ApplyOrientationToViewport maps, in WIC's vocabulary.
    // Kept identical to the mapping in RemoteImageXfer for the same reason: one
    // tag with two readings would rotate the clipboard and the screen different
    // amounts.
    WICBitmapTransformOptions transform = WICBitmapTransformRotate0;
    switch (orientation) {
        case 2: transform = WICBitmapTransformFlipHorizontal; break;
        case 3: transform = WICBitmapTransformRotate180;      break;
        case 4: transform = WICBitmapTransformFlipVertical;   break;
        case 5: transform = static_cast<WICBitmapTransformOptions>(
                    WICBitmapTransformRotate90 | WICBitmapTransformFlipHorizontal); break;
        case 6: transform = WICBitmapTransformRotate90;       break;
        case 7: transform = static_cast<WICBitmapTransformOptions>(
                    WICBitmapTransformRotate270 | WICBitmapTransformFlipHorizontal); break;
        case 8: transform = WICBitmapTransformRotate270;      break;
        default: break;   // 1, and anything a corrupt tag invents, mean "as-is"
    }


    //
    // Check source pixel format — skip converter if already 32bppPBGRA
    //
    WICPixelFormatGUID sourceFormat{};
    hr = frame->GetPixelFormat(&sourceFormat);
    if (FAILED(hr))
        return hr;

    // ONE TAIL FOR BOTH FORMATS. The two branches used to end in their own copy
    // of CreateBitmapFromSource and their own `return S_OK`, which meant a step
    // added after them — like the rotation below — had to be written twice or
    // silently applied to only one of them. They differ in ONE thing: what feeds
    // the bitmap. So that is all that is chosen here.
    ComPtr<IWICBitmapSource> source;

    if (sourceFormat == GUID_WICPixelFormat32bppPBGRA) {
        // Already the format we want — no conversion, as before.
        source = frame;
    } else {
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

        source = converter;
    }


    //
    // Turn it the right way up, if the tag says so
    //
    // Skipped entirely at orientation 1, which is almost every file that has
    // lived on a PC — so a picture that was already correct goes through exactly
    // the code it went through before this existed.
    if (transform != WICBitmapTransformRotate0) {
        ComPtr<IWICBitmapFlipRotator> rotator;

        hr = factory->CreateBitmapFlipRotator(&rotator);
        if (FAILED(hr))
            return hr;

        hr = rotator->Initialize(source.Get(), transform);
        if (FAILED(hr)) {
            OutputDebugStringW(
                    L"WIC2: Orientation transform failed\n"
                    );
            return hr;
        }

        source = rotator;

        // THE REPORTED SIZE TURNS WITH IT. width/height were read from the frame
        // before any of this, and a quarter turn swaps them — a caller sizing
        // anything off these would lay out a landscape box around a portrait
        // picture. Only the quarter turns: 180 and the two flips keep the shape.
        if (orientation == 5 || orientation == 6 ||
            orientation == 7 || orientation == 8) {
            const UINT swap = result.width;
            result.width  = result.height;
            result.height = swap;
        }
    }


    //
    // Make a cached RAM bitmap
    //
    hr = factory->CreateBitmapFromSource(
            source.Get(),

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
