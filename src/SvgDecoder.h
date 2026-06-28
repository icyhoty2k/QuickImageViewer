#pragma once

#include <windows.h>
#include <string>
#include <vector>

// Reads raw SVG bytes from disk so RendererD2D can parse them
// with ID2D1DeviceContext5::CreateSvgDocument() on the UI thread.
// All the actual D2D SVG work happens in RendererD2D – this file
// is intentionally thin (just an IO helper).
class SvgDecoder {
    public:
        // Reads the entire file at filePath into outBytes.
        // Returns S_OK on success, HRESULT_FROM_WIN32 error otherwise.
        static HRESULT LoadFile(const std::wstring &filePath,
                                std::vector<BYTE> &outBytes);

        // Returns true when the extension (lowercased) is ".svg"
        static bool IsSvgPath(const std::wstring &filePath);
};
