#pragma once

#include <wincodec.h>
#include <wrl/client.h>
#include <string>

using Microsoft::WRL::ComPtr;

struct DecodedImage {
    ComPtr<IWICBitmap> bitmap;
    UINT width = 0;
    UINT height = 0;
};

class WicDecoder {
    public:
        static HRESULT DecodeImage(
                const std::wstring &filePath,
                DecodedImage &result
                );
};
