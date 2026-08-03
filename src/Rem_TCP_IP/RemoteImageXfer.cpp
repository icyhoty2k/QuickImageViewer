#include "RemoteImageXfer.h"

#include "Common/Base64.h"
#include "Platform/Constants.h"
#include "Platform/ConstantsStrings.h"

#include <wincodec.h>   // WIC decode / scale / JPEG encode for the preview
#include <wrl/client.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace Remote::Xfer {

namespace RT = Constants::RemoteTcpIp;

// Opens a base64 body line in a `SendDisplayedImage` reply. A marker rather than
// bare base64 so the body can never be mistaken for a status line, and so a
// third-party client can skip anything it does not recognise.
const wchar_t *DATA_PREFIX = L"DATA ";

bool BuildPreviewJpeg(const std::wstring &path, int maxDim, int quality,
                      std::vector<unsigned char> &out, std::wstring &errOut) {
    out.clear();
    maxDim  = std::clamp(maxDim,  RT::PREVIEW_MAX_DIM_MIN, RT::PREVIEW_MAX_DIM_MAX);
    quality = std::clamp(quality, RT::PREVIEW_QUALITY_MIN, RT::PREVIEW_QUALITY_MAX);

    using Microsoft::WRL::ComPtr;

    // This thread's own factory — see the header.
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory)))) {
        errOut = Constants::Messages::PREVIEW_NO_WIC;
        return false;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(
            path.c_str(), nullptr, GENERIC_READ,
            WICDecodeMetadataCacheOnDemand, &decoder))) {
        errOut = Constants::Messages::PREVIEW_CANNOT_DECODE;
        return false;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) {
        errOut = Constants::Messages::PREVIEW_CANNOT_DECODE;
        return false;
    }

    UINT w = 0, h = 0;
    if (FAILED(frame->GetSize(&w, &h)) || w == 0 || h == 0) {
        errOut = Constants::Messages::PREVIEW_CANNOT_DECODE;
        return false;
    }

    // Fit the longest edge, preserving aspect. Never up.
    UINT tw = w, th = h;
    const UINT longest = std::max(w, h);
    if (longest > static_cast<UINT>(maxDim)) {
        const double k = static_cast<double>(maxDim) / static_cast<double>(longest);
        tw = std::max(1u, static_cast<UINT>(w * k));
        th = std::max(1u, static_cast<UINT>(h * k));
    }

    // Convert BEFORE scaling: the scaler works on the format it is given, and
    // the JPEG encoder wants 24bpp BGR. Going through the converter first also
    // flattens any palette or alpha to something JPEG can actually represent —
    // a PNG with transparency would otherwise fail at the encoder.
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat24bppBGR,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeMedianCut))) {
        errOut = Constants::Messages::PREVIEW_CANNOT_DECODE;
        return false;
    }

    ComPtr<IWICBitmapSource> source = converter;
    ComPtr<IWICBitmapScaler>  scaler;
    if (tw != w || th != h) {
        if (FAILED(factory->CreateBitmapScaler(&scaler)) ||
            // Fant: the slowest of WIC's modes and the only one that does not
            // alias badly on a large downscale. This runs once per picture
            // change on a socket thread, so quality is the right trade.
            FAILED(scaler->Initialize(converter.Get(), tw, th,
                                      WICBitmapInterpolationModeFant))) {
            errOut = Constants::Messages::PREVIEW_CANNOT_DECODE;
            return false;
        }
        source = scaler;
    }

    ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) {
        errOut = Constants::Messages::PREVIEW_ENCODE_FAILED;
        return false;
    }

    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> outFrame;
    ComPtr<IPropertyBag2> props;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&outFrame, &props))) {
        errOut = Constants::Messages::PREVIEW_ENCODE_FAILED;
        return false;
    }

    {
        PROPBAG2 opt{};
        opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT v;
        VariantInit(&v);
        v.vt      = VT_R4;
        v.fltVal  = static_cast<float>(quality) / 100.0f;
        props->Write(1, &opt, &v);   // best effort: a default quality still encodes
    }

    GUID fmt = GUID_WICPixelFormat24bppBGR;
    if (FAILED(outFrame->Initialize(props.Get())) ||
        FAILED(outFrame->SetSize(tw, th)) ||
        FAILED(outFrame->SetPixelFormat(&fmt)) ||
        FAILED(outFrame->WriteSource(source.Get(), nullptr)) ||
        FAILED(outFrame->Commit()) ||
        FAILED(encoder->Commit())) {
        errOut = Constants::Messages::PREVIEW_ENCODE_FAILED;
        return false;
    }

    HGLOBAL hg = nullptr;
    if (FAILED(GetHGlobalFromStream(stream.Get(), &hg)) || !hg) {
        errOut = Constants::Messages::PREVIEW_ENCODE_FAILED;
        return false;
    }

    const SIZE_T size = GlobalSize(hg);
    if (size == 0 || size > RT::STREAM_MAX_BYTES) {
        errOut = Constants::Messages::PREVIEW_ENCODE_FAILED;
        return false;
    }

    if (const void *src = GlobalLock(hg)) {
        out.assign(static_cast<const unsigned char *>(src),
                   static_cast<const unsigned char *>(src) + size);
        GlobalUnlock(hg);
        return true;
    }

    errOut = Constants::Messages::PREVIEW_ENCODE_FAILED;
    return false;
}

bool ReadImageFile(const std::wstring &path, std::vector<unsigned char> &out,
                   std::wstring &errOut) {
    out.clear();

    std::error_code ec;
    const auto size = fs::file_size(fs::path(path), ec);
    if (ec) {
        errOut = Constants::Messages::STREAM_ERR_UNREADABLE;
        return false;
    }
    // Checked BEFORE the read, not after: the point of a ceiling is not to
    // allocate the thing you are about to refuse.
    if (size == 0 || size > RT::STREAM_MAX_BYTES) {
        errOut = (size == 0) ? Constants::Messages::STREAM_ERR_UNREADABLE
                             : Constants::Messages::STREAM_ERR_TOO_LARGE;
        return false;
    }

    std::ifstream f(path, std::ios::binary);
    if (!f) {
        errOut = Constants::Messages::STREAM_ERR_UNREADABLE;
        return false;
    }

    out.resize(static_cast<size_t>(size));
    f.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(size));
    if (static_cast<size_t>(f.gcount()) != out.size()) {
        out.clear();
        errOut = Constants::Messages::STREAM_ERR_UNREADABLE;
        return false;
    }
    errOut.clear();
    return true;
}

std::vector<std::wstring> BuildStreamCommands(const std::wstring &fileName,
                                              const std::vector<unsigned char> &bytes) {
    std::vector<std::wstring> lines;
    if (bytes.empty()) return lines;

    lines.reserve(bytes.size() / RT::STREAM_CHUNK_BYTES + 3);

    // The NAME, never a path: the receiver wants an extension so it can pick a
    // decoder, and nothing else. Any directory part is stripped here rather than
    // trusted to the caller, so a path cannot leak across a machine boundary
    // where it would be meaningless anyway.
    const std::wstring bare = fs::path(fileName).filename().wstring();

    lines.push_back(L"StreamImageBegin " + std::to_wstring(bytes.size()) + L" " + bare);

    for (size_t off = 0; off < bytes.size(); off += RT::STREAM_CHUNK_BYTES) {
        const size_t n = (bytes.size() - off < RT::STREAM_CHUNK_BYTES)
                             ? bytes.size() - off
                             : RT::STREAM_CHUNK_BYTES;
        const std::string b64 = Common::Base64::Encode(bytes.data() + off, n);
        // ASCII by construction, so the widening is a copy and not a conversion.
        lines.push_back(L"StreamImageChunk " + std::wstring(b64.begin(), b64.end()));
    }

    lines.push_back(L"StreamImageShow");
    return lines;
}

std::wstring BuildDataReplyBody(const std::vector<unsigned char> &bytes) {
    std::wstring body;
    // Roughly: 4/3 for base64, plus a marker and CRLF per line.
    body.reserve((bytes.size() * 4) / 3 + 64);

    for (size_t off = 0; off < bytes.size(); off += RT::STREAM_CHUNK_BYTES) {
        const size_t n = (bytes.size() - off < RT::STREAM_CHUNK_BYTES)
                             ? bytes.size() - off
                             : RT::STREAM_CHUNK_BYTES;
        const std::string b64 = Common::Base64::Encode(bytes.data() + off, n);
        body += DATA_PREFIX;
        body.append(b64.begin(), b64.end());
        body += L"\r\n";
    }
    return body;
}

bool ParseDataReply(const std::wstring &reply, std::vector<unsigned char> &out) {
    out.clear();

    // Concatenate the base64 of every body line first, then decode ONCE: decoding
    // per line would break wherever a line boundary fell mid-quartet, which it
    // does whenever a chunk size is not a multiple of three.
    std::wstring b64;
    b64.reserve(reply.size());

    const size_t prefixLen = wcslen(DATA_PREFIX);
    size_t start = 0;
    while (start < reply.size()) {
        size_t end = reply.find(L"\r\n", start);
        if (end == std::wstring::npos) end = reply.size();

        if (reply.compare(start, prefixLen, DATA_PREFIX) == 0)
            b64.append(reply, start + prefixLen, end - start - prefixLen);

        start = (end == reply.size()) ? reply.size() : end + 2;
    }

    if (b64.empty()) return false;   // an answer with no body — nothing was sent
    return Common::Base64::Decode(b64.c_str(), b64.size(), out);
}

std::wstring FileNameFromDataReply(const std::wstring &reply) {
    // The OK line is the last one: "OK SendDisplayedImage=<bytes>;<name>".
    const size_t eq = reply.rfind(L'=');
    if (eq == std::wstring::npos) return {};
    const size_t semi = reply.find(L';', eq);
    if (semi == std::wstring::npos) return {};

    std::wstring name = reply.substr(semi + 1);
    // The OK line terminates the accumulation, so there is normally nothing after
    // it — but trim defensively rather than build a file name with a CR in it.
    while (!name.empty() && (name.back() == L'\r' || name.back() == L'\n'))
        name.pop_back();
    return name;
}

bool WriteTempImage(const std::wstring &fileName,
                    const std::vector<unsigned char> &bytes,
                    std::wstring &pathOut) {
    pathOut.clear();
    if (bytes.empty()) return false;

    wchar_t tempDir[MAX_PATH]{};
    const DWORD n = GetTempPathW(MAX_PATH, tempDir);
    if (n == 0 || n > MAX_PATH) return false;

    // The extension is the whole reason the name travelled — without it WIC has
    // nothing to select a decoder by. `.img` when the sender named nothing useful:
    // the decode then fails cleanly instead of writing a file with no extension
    // and failing somewhere less obvious.
    std::wstring ext = fs::path(fileName).extension().wstring();
    if (ext.empty() || ext.size() > 8) ext = L".img";

    // Unique per process and per transfer, so two arriving at once cannot collide
    // and a leftover from a previous run cannot be picked up as this one.
    static unsigned counter = 0;
    const std::wstring path = std::wstring(tempDir) + L"qIV_stream_" +
                              std::to_wstring(GetCurrentProcessId()) + L"_" +
                              std::to_wstring(++counter) + ext;

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char *>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    if (!f) return false;
    f.close();

    pathOut = path;
    return true;
}

} // namespace Remote::Xfer
