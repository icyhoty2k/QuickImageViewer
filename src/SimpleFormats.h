#pragma once
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <string>
#include <vector>

namespace SimpleFormats {
    bool IsSimpleFormat(const std::wstring& filePath);

    Microsoft::WRL::ComPtr<IWICBitmap> Decode(
        const std::wstring& filePath,
        const std::vector<BYTE>& fileBytes,
        IWICImagingFactory* wicFac,
        UINT& outWidth,
        UINT& outHeight);
}
