#include "SvgDecoder.h"
#include <algorithm>

// ---------------------------------------------------------------------------
// SvgDecoder::IsSvgPath
// ---------------------------------------------------------------------------
bool SvgDecoder::IsSvgPath(const std::wstring &filePath) {
    if (filePath.size() < 4) return false;

    // Find the last dot
    size_t dot = filePath.rfind(L'.');
    if (dot == std::wstring::npos) return false;

    std::wstring ext = filePath.substr(dot);

    // Lower-case in place (ASCII only – extensions are ASCII)
    for (wchar_t &c: ext) {
        if (c >= L'A' && c <= L'Z') c = c + (L'a' - L'A');
    }

    return ext == L".svg";
}

// ---------------------------------------------------------------------------
// SvgDecoder::LoadFile
// ---------------------------------------------------------------------------
HRESULT SvgDecoder::LoadFile(const std::wstring &filePath,
                             std::vector<BYTE> &outBytes) {
    HANDLE hFile = CreateFileW(
            filePath.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);

    if (hFile == INVALID_HANDLE_VALUE)
        return HRESULT_FROM_WIN32(GetLastError());

    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(hFile, &fileSize)) {
        HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
        CloseHandle(hFile);
        return hr;
    }

    if (fileSize.QuadPart == 0 || fileSize.QuadPart > 64LL * 1024 * 1024) {
        // Refuse empty files or SVGs larger than 64 MB
        CloseHandle(hFile);
        return E_INVALIDARG;
    }

    outBytes.resize(static_cast<size_t>(fileSize.QuadPart));

    DWORD bytesRead = 0;
    BOOL ok = ReadFile(hFile, outBytes.data(),
                       static_cast<DWORD>(outBytes.size()),
                       &bytesRead, nullptr);

    CloseHandle(hFile);

    if (!ok || bytesRead != static_cast<DWORD>(outBytes.size()))
        return HRESULT_FROM_WIN32(GetLastError());

    return S_OK;
}
