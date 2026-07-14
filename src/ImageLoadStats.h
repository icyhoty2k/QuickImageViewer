#pragma once
#include <windows.h>
#include <atomic>
#include <string>
#include <cwctype>

namespace ImageLoadStats {
    // Timestamp (µs) set at the start of every navigation (before cache probe).
    inline std::atomic<long long> g_loadStartUs{0};
    // Elapsed time in µs when the image is ready. -1 = not yet set.
    inline std::atomic<long long> g_lastLoadUs{-1};

    inline long long NowUs() {
        LARGE_INTEGER freq, counter;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&counter);
        return (counter.QuadPart * 1000000LL) / freq.QuadPart;
    }

    // Returns human-readable codec/decoder name from file extension.
    inline std::wstring CodecForPath(const std::wstring& path) {
        auto dot = path.rfind(L'.');
        std::wstring ext = (dot == std::wstring::npos) ? L"" : path.substr(dot + 1);
        for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));

        if (ext == L"qoi")                                                   return L"QOI";
        if (ext == L"tga")                                                   return L"TGA";
        if (ext == L"j2k" || ext == L"jp2" || ext == L"j2c")               return L"OpenJPEG";
        if (ext == L"exr")                                                   return L"TinyEXR";
        if (ext == L"hdr")                                                   return L"Radiance HDR";
        if (ext == L"pnm" || ext == L"ppm" || ext == L"pgm" || ext == L"pbm") return L"PNM";
        if (ext == L"svg")                                                   return L"SVG (resvg)";
        if (ext == L"jpg" || ext == L"jpeg")                                 return L"WIC JPEG";
        if (ext == L"png")                                                   return L"WIC PNG";
        if (ext == L"bmp")                                                   return L"WIC BMP";
        if (ext == L"gif")                                                   return L"WIC GIF";
        if (ext == L"tif" || ext == L"tiff")                                return L"WIC TIFF";
        if (ext == L"ico")                                                   return L"WIC ICO";
        if (ext == L"webp")                                                  return L"WIC WebP";
        if (ext == L"heic" || ext == L"heif")                               return L"WIC HEIC";
        if (ext == L"avif")                                                  return L"WIC AVIF";
        return L"WIC";
    }
} // namespace ImageLoadStats
