// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#include "SimpleFormats.h"
#include <cstring>
#include <cmath>
#include <cstdio>
#include <climits>   // INT_MAX — PNM header fields saturate rather than overflow
#include <openjpeg.h>

#pragma warning(push)
#pragma warning(disable: 4100 4127 4244 4245 4267 4706 4996)
#define TINYEXR_IMPLEMENTATION
#include <tinyexr.h>
#pragma warning(pop)

using Microsoft::WRL::ComPtr;

static std::wstring LowerExt(const std::wstring& path) {
    const size_t dot = path.rfind(L'.');
    if (dot == std::wstring::npos) return {};
    std::wstring e = path.substr(dot);
    for (wchar_t& c : e) c = towlower(c);
    return e;
}

static ComPtr<IWICBitmap> WrapInWIC(
    IWICImagingFactory* wicFac,
    const BYTE* pixels, UINT w, UINT h,
    UINT& outW, UINT& outH)
{
    ComPtr<IWICBitmap> bmp;
    const HRESULT hr = wicFac->CreateBitmapFromMemory(
        w, h, GUID_WICPixelFormat32bppPBGRA,
        w * 4, w * h * 4, const_cast<BYTE*>(pixels), &bmp);
    if (FAILED(hr)) return nullptr;
    outW = w; outH = h;
    return bmp;
}

// =============================================================================
//  QOI  (Quite OK Image)  — https://qoiformat.org
//  Output: premultiplied BGRA
// =============================================================================
static ComPtr<IWICBitmap> DecodeQOI(
    const std::vector<BYTE>& data,
    IWICImagingFactory* wicFac,
    UINT& outW, UINT& outH)
{
    if (data.size() < 14) return nullptr;
    if (data[0]!='q'||data[1]!='o'||data[2]!='i'||data[3]!='f') return nullptr;

    const uint32_t w = ((uint32_t)data[4]<<24)|((uint32_t)data[5]<<16)|
                       ((uint32_t)data[6]<<8 )|data[7];
    const uint32_t h = ((uint32_t)data[8]<<24)|((uint32_t)data[9]<<16)|
                       ((uint32_t)data[10]<<8)|data[11];
    const uint8_t channels = data[12];

    if (w==0||h==0||channels<3||channels>4) return nullptr;
    if ((size_t)w*h > 400'000'000u) return nullptr;

    const size_t pixCount = (size_t)w * h;
    std::vector<BYTE> bgra(pixCount * 4);

    uint8_t idx[64][4] = {};
    uint8_t r=0, g=0, b=0, a=255;
    int run = 0;
    size_t pos = 14;

    for (size_t px = 0; px < pixCount; ++px) {
        if (run > 0) {
            --run;
        } else {
            if (pos >= data.size()) return nullptr;
            const uint8_t b1 = data[pos++];

            if (b1 == 0xFE) {                          // QOI_OP_RGB
                if (pos+3 > data.size()) return nullptr;
                r=data[pos++]; g=data[pos++]; b=data[pos++];
            } else if (b1 == 0xFF) {                   // QOI_OP_RGBA
                if (pos+4 > data.size()) return nullptr;
                r=data[pos++]; g=data[pos++]; b=data[pos++]; a=data[pos++];
            } else {
                const uint8_t tag = b1 >> 6;
                if (tag == 0) {                        // QOI_OP_INDEX
                    const uint8_t i = b1 & 0x3F;
                    r=idx[i][0]; g=idx[i][1]; b=idx[i][2]; a=idx[i][3];
                } else if (tag == 1) {                 // QOI_OP_DIFF
                    r += ((b1>>4)&3)-2;
                    g += ((b1>>2)&3)-2;
                    b +=  (b1&3)    -2;
                } else if (tag == 2) {                 // QOI_OP_LUMA
                    if (pos >= data.size()) return nullptr;
                    const uint8_t b2 = data[pos++];
                    const int dg = (b1&0x3F)-32;
                    r = static_cast<uint8_t>(r + dg - 8 + ((b2>>4)&0xF));
                    g = static_cast<uint8_t>(g + dg);
                    b = static_cast<uint8_t>(b + dg - 8 +  (b2&0xF));
                } else {                               // QOI_OP_RUN
                    run = b1 & 0x3F;
                }
            }
        }

        const uint32_t hash = ((uint32_t)r*3+(uint32_t)g*5+
                               (uint32_t)b*7+(uint32_t)a*11) & 63;
        idx[hash][0]=r; idx[hash][1]=g; idx[hash][2]=b; idx[hash][3]=a;

        BYTE* d = &bgra[px*4];
        if      (a==255) { d[0]=b; d[1]=g; d[2]=r; d[3]=255; }
        else if (a==0)   { d[0]=d[1]=d[2]=d[3]=0; }
        else {
            d[0]=(BYTE)((b*a+127)/255);
            d[1]=(BYTE)((g*a+127)/255);
            d[2]=(BYTE)((r*a+127)/255);
            d[3]=a;
        }
    }

    return WrapInWIC(wicFac, bgra.data(), w, h, outW, outH);
}

// =============================================================================
//  TGA  (Truevision TARGA)
//  Supports: type 1/2/3 (uncompressed) and type 9/10/11 (RLE).
//  Pixel depth: 8 (gray), 15, 16, 24, 32 bit.
//  Output: premultiplied BGRA.
//  TGA stores pixels in BGR(A) order natively — no channel swap needed.
// =============================================================================
static ComPtr<IWICBitmap> DecodeTGA(
    const std::vector<BYTE>& data,
    IWICImagingFactory* wicFac,
    UINT& outW, UINT& outH)
{
    if (data.size() < 18) return nullptr;

    const uint8_t  idLen    = data[0];
    const uint8_t  cmType   = data[1];
    const uint8_t  imgType  = data[2];
    const uint16_t cmFirst  = data[3] | ((uint16_t)data[4]<<8);
    const uint16_t cmLen    = data[5] | ((uint16_t)data[6]<<8);
    const uint8_t  cmBpp    = data[7];
    const uint16_t w        = data[12] | ((uint16_t)data[13]<<8);
    const uint16_t h        = data[14] | ((uint16_t)data[15]<<8);
    const uint8_t  depth    = data[16];
    const uint8_t  desc     = data[17];
    const bool     topLeft  = (desc >> 5) & 1;

    if (w==0||h==0) return nullptr;

    const bool isColorMapped = (imgType==1||imgType==9);
    const bool isTrueColor   = (imgType==2||imgType==10);
    const bool isGray        = (imgType==3||imgType==11);
    const bool isRLE         = (imgType==9||imgType==10||imgType==11);

    if (!isColorMapped && !isTrueColor && !isGray) return nullptr;
    if (isTrueColor   && depth!=15&&depth!=16&&depth!=24&&depth!=32) return nullptr;
    if (isGray        && depth!=8)  return nullptr;
    if (isColorMapped && depth!=8&&depth!=16) return nullptr;

    size_t pos = 18 + idLen;

    // Color map
    const size_t cmBytesPerEntry = (cmBpp+7)/8;
    std::vector<BYTE> colorMap;
    if (cmType==1 && cmLen>0) {
        const size_t cmBytes = (size_t)cmLen * cmBytesPerEntry;
        if (pos+cmBytes > data.size()) return nullptr;
        colorMap.assign(data.begin()+pos, data.begin()+pos+cmBytes);
        pos += cmBytes;
    }

    const size_t pixCount = (size_t)w * h;
    std::vector<BYTE> bgra(pixCount * 4);

    // Read one pixel into straight BGRA
    const auto readPixel = [&](BYTE* out) -> bool {
        if (isColorMapped) {
            uint16_t ci = 0;
            if (depth==8) {
                if (pos>=data.size()) return false;
                ci=data[pos++];
            } else {
                if (pos+2>data.size()) return false;
                ci=data[pos]|((uint16_t)data[pos+1]<<8); pos+=2;
            }
            // BELOW the map's first index is rejected here, BEFORE the
            // subtraction. Both are uint16_t, so ci < cmFirst promotes to a
            // negative int and the cast to size_t wraps it to near SIZE_MAX —
            // and for a difference of exactly -1 the product is
            // SIZE_MAX+1-cmBytesPerEntry, so cmPos+cmBytesPerEntry wraps back
            // to 0 and walks straight through the bounds test below. The read
            // that followed was a wild pointer, and a file with cmFirst=1 and a
            // pixel index of 0 is all it took.
            if (ci < cmFirst) { out[0]=out[1]=out[2]=0; out[3]=255; return true; }
            const size_t cmPos = (size_t)(ci-cmFirst)*cmBytesPerEntry;
            if (cmPos+cmBytesPerEntry > colorMap.size()) { out[0]=out[1]=out[2]=0; out[3]=255; return true; }
            if (cmBpp==32) {
                out[0]=colorMap[cmPos]; out[1]=colorMap[cmPos+1];
                out[2]=colorMap[cmPos+2]; out[3]=colorMap[cmPos+3];
            } else if (cmBpp==24) {
                out[0]=colorMap[cmPos]; out[1]=colorMap[cmPos+1];
                out[2]=colorMap[cmPos+2]; out[3]=255;
            } else {
                const uint16_t p = colorMap[cmPos]|((uint16_t)colorMap[cmPos+1]<<8);
                out[0]=(p&0x1F)<<3; out[1]=((p>>5)&0x1F)<<3; out[2]=((p>>10)&0x1F)<<3; out[3]=255;
            }
        } else if (isGray) {
            if (pos>=data.size()) return false;
            const BYTE v=data[pos++]; out[0]=out[1]=out[2]=v; out[3]=255;
        } else {
            if (depth==32) {
                if (pos+4>data.size()) return false;
                out[0]=data[pos]; out[1]=data[pos+1]; out[2]=data[pos+2]; out[3]=data[pos+3]; pos+=4;
            } else if (depth==24) {
                if (pos+3>data.size()) return false;
                out[0]=data[pos]; out[1]=data[pos+1]; out[2]=data[pos+2]; out[3]=255; pos+=3;
            } else {
                if (pos+2>data.size()) return false;
                const uint16_t p=data[pos]|((uint16_t)data[pos+1]<<8); pos+=2;
                out[0]=(p&0x1F)<<3; out[1]=((p>>5)&0x1F)<<3; out[2]=((p>>10)&0x1F)<<3; out[3]=255;
            }
        }
        return true;
    };

    if (isRLE) {
        for (size_t px=0; px<pixCount;) {
            if (pos>=data.size()) break;
            const uint8_t pkt=data[pos++];
            const int count=(pkt&0x7F)+1;
            if (pkt>>7) {
                BYTE pixel[4];
                if (!readPixel(pixel)) break;
                for (int i=0; i<count&&px<pixCount; ++i,++px) memcpy(&bgra[px*4],pixel,4);
            } else {
                for (int i=0; i<count&&px<pixCount; ++i,++px)
                    if (!readPixel(&bgra[px*4])) break;
            }
        }
    } else {
        for (size_t px=0; px<pixCount; ++px)
            if (!readPixel(&bgra[px*4])) break;
    }

    // TGA origin is bottom-left by default — flip vertically
    if (!topLeft) {
        const size_t rowBytes = (size_t)w*4;
        std::vector<BYTE> row(rowBytes);
        for (uint16_t y=0; y<h/2; ++y) {
            BYTE* t=&bgra[y*rowBytes], *b2=&bgra[(h-1-y)*rowBytes];
            memcpy(row.data(),t,rowBytes); memcpy(t,b2,rowBytes); memcpy(b2,row.data(),rowBytes);
        }
    }

    // Premultiply straight alpha (only 32-bit variants have alpha)
    const bool hasAlpha = (isTrueColor&&depth==32)||(isColorMapped&&cmBpp==32);
    if (hasAlpha) {
        for (size_t px=0; px<pixCount; ++px) {
            BYTE* d=&bgra[px*4];
            const BYTE al=d[3];
            if      (al==0)   { d[0]=d[1]=d[2]=0; }
            else if (al<255)  { d[0]=(BYTE)((d[0]*al+127)/255);
                                d[1]=(BYTE)((d[1]*al+127)/255);
                                d[2]=(BYTE)((d[2]*al+127)/255); }
        }
    }

    return WrapInWIC(wicFac, bgra.data(), w, h, outW, outH);
}

// =============================================================================
//  JPEG 2000  (.jp2 / .j2k / .jpf / .jpx)  — via OpenJPEG
//  Supports: grayscale, RGB, RGBA, any bit depth (scaled to 8-bit).
//  Output: premultiplied BGRA.
// =============================================================================
struct OPJ_MemStream { const OPJ_BYTE* buf; OPJ_SIZE_T len, pos; };

static OPJ_SIZE_T opjRead(void* dst, OPJ_SIZE_T nb, void* ud) {
    auto* ms = static_cast<OPJ_MemStream*>(ud);
    const OPJ_SIZE_T avail = ms->pos < ms->len ? ms->len - ms->pos : 0;
    const OPJ_SIZE_T n = nb < avail ? nb : avail;
    if (!n) return static_cast<OPJ_SIZE_T>(-1);
    memcpy(dst, ms->buf + ms->pos, n);
    ms->pos += n;
    return n;
}
static OPJ_OFF_T opjSkip(OPJ_OFF_T nb, void* ud) {
    auto* ms = static_cast<OPJ_MemStream*>(ud);
    const OPJ_SIZE_T np = ms->pos + static_cast<OPJ_SIZE_T>(nb);
    const OPJ_SIZE_T clamped = np < ms->len ? np : ms->len;
    const OPJ_OFF_T sk = static_cast<OPJ_OFF_T>(clamped - ms->pos);
    ms->pos = clamped;
    return sk;
}
static OPJ_BOOL opjSeek(OPJ_OFF_T nb, void* ud) {
    auto* ms = static_cast<OPJ_MemStream*>(ud);
    if (nb < 0 || static_cast<OPJ_SIZE_T>(nb) > ms->len) return OPJ_FALSE;
    ms->pos = static_cast<OPJ_SIZE_T>(nb);
    return OPJ_TRUE;
}

static ComPtr<IWICBitmap> DecodeJP2(
    const std::vector<BYTE>& data,
    IWICImagingFactory* wicFac,
    UINT& outW, UINT& outH,
    OPJ_CODEC_FORMAT fmt)
{
    OPJ_MemStream ms{data.data(), data.size(), 0};

    opj_stream_t* stream = opj_stream_create(1 << 16, OPJ_TRUE);
    if (!stream) return nullptr;
    opj_stream_set_read_function(stream, opjRead);
    opj_stream_set_skip_function(stream, opjSkip);
    opj_stream_set_seek_function(stream, opjSeek);
    opj_stream_set_user_data(stream, &ms, nullptr);
    opj_stream_set_user_data_length(stream, data.size());

    opj_codec_t* codec = opj_create_decompress(fmt);
    if (!codec) { opj_stream_destroy(stream); return nullptr; }

    // Silence all OpenJPEG console output
    opj_set_info_handler(codec,    nullptr, nullptr);
    opj_set_warning_handler(codec, nullptr, nullptr);
    opj_set_error_handler(codec,   nullptr, nullptr);

    opj_dparameters_t params{};
    opj_set_default_decoder_parameters(&params);
    opj_setup_decoder(codec, &params);

    opj_image_t* image = nullptr;
    const bool ok = opj_read_header(stream, codec, &image)
                 && opj_decode(codec, stream, image)
                 && opj_end_decompress(codec, stream);

    opj_destroy_codec(codec);
    opj_stream_destroy(stream);

    if (!ok || !image) { if (image) opj_image_destroy(image); return nullptr; }

    const uint32_t w = image->x1 - image->x0;
    const uint32_t h = image->y1 - image->y0;
    const uint32_t nc = image->numcomps;

    if (w==0||h==0||nc==0) { opj_image_destroy(image); return nullptr; }

    // Scale any bit depth to 8-bit
    const int prec   = image->comps[0].prec;
    const int sgnd   = image->comps[0].sgnd;
    const int offset = sgnd ? (1 << (prec-1)) : 0;
    const int maxVal = (1 << prec) - 1;
    const auto scale8 = [&](OPJ_INT32 v) -> BYTE {
        v += offset;
        if (v < 0) v = 0; if (v > maxVal) v = maxVal;
        return prec==8 ? static_cast<BYTE>(v)
                       : static_cast<BYTE>((v * 255 + maxVal/2) / maxVal);
    };

    const size_t pixCount = static_cast<size_t>(w) * h;
    std::vector<BYTE> bgra(pixCount * 4);

    if (nc == 1) {
        // Grayscale
        const OPJ_INT32* c0 = image->comps[0].data;
        for (size_t i = 0; i < pixCount; ++i) {
            const BYTE v = scale8(c0[i]);
            bgra[i*4+0]=v; bgra[i*4+1]=v; bgra[i*4+2]=v; bgra[i*4+3]=255;
        }
    } else if (nc >= 3) {
        // RGB / RGBA — OpenJPEG gives planar R,G,B[,A]
        const OPJ_INT32* cR = image->comps[0].data;
        const OPJ_INT32* cG = image->comps[1].data;
        const OPJ_INT32* cB = image->comps[2].data;
        const OPJ_INT32* cA = (nc >= 4) ? image->comps[3].data : nullptr;
        for (size_t i = 0; i < pixCount; ++i) {
            const BYTE r=scale8(cR[i]), g=scale8(cG[i]), b=scale8(cB[i]);
            const BYTE a = cA ? scale8(cA[i]) : 255;
            if      (a==255) { bgra[i*4+0]=b; bgra[i*4+1]=g; bgra[i*4+2]=r; bgra[i*4+3]=255; }
            else if (a==0)   { bgra[i*4+0]=bgra[i*4+1]=bgra[i*4+2]=bgra[i*4+3]=0; }
            else {
                bgra[i*4+0]=(BYTE)((b*a+127)/255);
                bgra[i*4+1]=(BYTE)((g*a+127)/255);
                bgra[i*4+2]=(BYTE)((r*a+127)/255);
                bgra[i*4+3]=a;
            }
        }
    } else {
        opj_image_destroy(image);
        return nullptr;
    }

    opj_image_destroy(image);
    return WrapInWIC(wicFac, bgra.data(), w, h, outW, outH);
}

// =============================================================================
//  Radiance HDR  (.hdr)  — RGBE / Greg Ward format
//  Tone-mapped to 8-bit LDR via per-channel Reinhard. Output: premultiplied BGRA.
// =============================================================================
static ComPtr<IWICBitmap> DecodeHDR(
    const std::vector<BYTE>& data,
    IWICImagingFactory* wicFac,
    UINT& outW, UINT& outH)
{
    if (data.size() < 10 || data[0] != '#' || data[1] != '?') return nullptr;

    // Find blank-line end of header (\n\n)
    size_t pos = 0;
    while (pos + 1 < data.size()) {
        if (data[pos] == '\n' && data[pos+1] == '\n') { pos += 2; break; }
        ++pos;
    }
    if (pos >= data.size()) return nullptr;

    // Parse size line: "-Y H +X W"
    size_t lineEnd = pos;
    while (lineEnd < data.size() && data[lineEnd] != '\n') ++lineEnd;
    const std::string sizeLine(reinterpret_cast<const char*>(data.data()+pos), lineEnd-pos);
    pos = lineEnd + 1;

    int W = 0, H = 0;
    if (sscanf_s(sizeLine.c_str(), "-Y %d +X %d", &H, &W) != 2 || W<=0 || H<=0) return nullptr;
    if (static_cast<size_t>(W)*H > 100'000'000u) return nullptr;

    const size_t pixCount = static_cast<size_t>(W) * H;
    std::vector<BYTE> rgbe(pixCount * 4);

    for (int y = 0; y < H; ++y) {
        BYTE* row = &rgbe[static_cast<size_t>(y) * W * 4];
        if (pos + 4 > data.size()) return nullptr;

        // New adaptive RLE: scanline header is [02 02 W>>8 W&FF]
        const bool newRLE = data[pos]==2 && data[pos+1]==2 &&
                            ((data[pos+2]<<8)|data[pos+3]) == W;
        if (newRLE) {
            pos += 4;
            for (int ch = 0; ch < 4; ++ch) {
                int x = 0;
                while (x < W) {
                    if (pos >= data.size()) return nullptr;
                    const BYTE code = data[pos++];
                    if (code > 128) {
                        const int cnt = code - 128;
                        if (pos >= data.size()) return nullptr;
                        const BYTE val = data[pos++];
                        for (int i = 0; i < cnt && x < W; ++i, ++x) row[x*4+ch] = val;
                    } else {
                        const int cnt = code;
                        for (int i = 0; i < cnt && x < W; ++i, ++x) {
                            if (pos >= data.size()) return nullptr;
                            row[x*4+ch] = data[pos++];
                        }
                    }
                }
            }
        } else {
            // Uncompressed RGBE
            for (int x = 0; x < W; ++x) {
                if (pos+4 > data.size()) return nullptr;
                row[x*4]=data[pos]; row[x*4+1]=data[pos+1];
                row[x*4+2]=data[pos+2]; row[x*4+3]=data[pos+3]; pos+=4;
            }
        }
    }

    std::vector<BYTE> bgra(pixCount * 4);
    for (size_t i = 0; i < pixCount; ++i) {
        const BYTE e = rgbe[i*4+3];
        float lr, lg, lb;
        if (e == 0) { lr = lg = lb = 0.0f; }
        else {
            const float scale = ldexpf(1.0f, e - 128 - 8);
            lr = rgbe[i*4+0] * scale;
            lg = rgbe[i*4+1] * scale;
            lb = rgbe[i*4+2] * scale;
        }
        lr = lr / (1.0f + lr);
        lg = lg / (1.0f + lg);
        lb = lb / (1.0f + lb);
        lr = powf(lr, 1.0f/2.2f); lg = powf(lg, 1.0f/2.2f); lb = powf(lb, 1.0f/2.2f);
        bgra[i*4+0]=static_cast<BYTE>(lb*255.0f+0.5f);
        bgra[i*4+1]=static_cast<BYTE>(lg*255.0f+0.5f);
        bgra[i*4+2]=static_cast<BYTE>(lr*255.0f+0.5f);
        bgra[i*4+3]=255;
    }

    return WrapInWIC(wicFac, bgra.data(), static_cast<UINT>(W), static_cast<UINT>(H), outW, outH);
}

// =============================================================================
//  PNM family  (.pbm / .pgm / .ppm)
//  ASCII variants P1-P3 and binary variants P4-P6 (up to 16-bit maxval).
//  Output: premultiplied BGRA (all opaque).
// =============================================================================
static ComPtr<IWICBitmap> DecodePNM(
    const std::vector<BYTE>& data,
    IWICImagingFactory* wicFac,
    UINT& outW, UINT& outH)
{
    if (data.size() < 3 || data[0] != 'P') return nullptr;
    const char type = static_cast<char>(data[1]);
    if (type < '1' || type > '6') return nullptr;

    size_t pos = 2;

    const auto skipWS = [&]() {
        while (pos < data.size()) {
            if (data[pos]=='#') { while (pos<data.size()&&data[pos]!='\n') ++pos; }
            else if (data[pos]==' '||data[pos]=='\t'||data[pos]=='\r'||data[pos]=='\n') ++pos;
            else break;
        }
    };
    // SATURATES instead of overflowing. `v = v*10 + d` on a header field that
    // is just a long run of digits is signed overflow — undefined behaviour,
    // and in a release build it wraps to a value that can pass the W/H and
    // maxVal range checks below. Every caller treats a large number as a
    // rejection, so stopping at INT_MAX loses nothing real.
    const auto readInt = [&]() -> int {
        skipWS();
        int v=0; bool got=false;
        while (pos<data.size()&&data[pos]>='0'&&data[pos]<='9') {
            const int d = data[pos++]-'0';
            if (v > (INT_MAX - d) / 10) v = INT_MAX;
            else                        v = v*10 + d;
            got=true;
        }
        return got ? v : -1;
    };

    const int W = readInt(), H = readInt();
    if (W<=0||H<=0||static_cast<size_t>(W)*H>100'000'000u) return nullptr;

    int maxVal = 1;
    if (type!='1'&&type!='4') { maxVal=readInt(); if (maxVal<=0||maxVal>65535) return nullptr; }

    // Binary formats: skip exactly one whitespace after last header token
    if (type=='4'||type=='5'||type=='6') { if (pos<data.size()) ++pos; }

    const size_t pixCount = static_cast<size_t>(W)*H;
    std::vector<BYTE> bgra(pixCount*4);

    // CLAMPED FIRST. The ASCII variants (P1/P2/P3) read their samples with
    // readInt, which has no idea what maxVal is, so a file can hand this a
    // value far above it — `v*255` then overflows int on the way to a byte that
    // was going to be wrong anyway. A sample above maxVal is a malformed file,
    // and white is the honest answer for it.
    const auto norm8 = [&](int v) -> BYTE {
        if (v < 0)      v = 0;
        if (v > maxVal) v = maxVal;
        return maxVal==255 ? static_cast<BYTE>(v) : static_cast<BYTE>(v*255/maxVal);
    };

    if (type=='6') {
        for (size_t i=0; i<pixCount; ++i) {
            if (maxVal<=255) {
                if (pos+3>data.size()) break;
                const BYTE r=data[pos],g=data[pos+1],b=data[pos+2]; pos+=3;
                bgra[i*4+0]=norm8(b); bgra[i*4+1]=norm8(g); bgra[i*4+2]=norm8(r); bgra[i*4+3]=255;
            } else {
                if (pos+6>data.size()) break;
                const int r=(data[pos]<<8)|data[pos+1]; pos+=2;
                const int g=(data[pos]<<8)|data[pos+1]; pos+=2;
                const int b=(data[pos]<<8)|data[pos+1]; pos+=2;
                bgra[i*4+0]=norm8(b); bgra[i*4+1]=norm8(g); bgra[i*4+2]=norm8(r); bgra[i*4+3]=255;
            }
        }
    } else if (type=='5') {
        for (size_t i=0; i<pixCount; ++i) {
            if (maxVal<=255) {
                if (pos>=data.size()) break;
                const BYTE v=norm8(data[pos++]);
                bgra[i*4+0]=bgra[i*4+1]=bgra[i*4+2]=v; bgra[i*4+3]=255;
            } else {
                if (pos+2>data.size()) break;
                const int s=(data[pos]<<8)|data[pos+1]; pos+=2;
                const BYTE v=norm8(s);
                bgra[i*4+0]=bgra[i*4+1]=bgra[i*4+2]=v; bgra[i*4+3]=255;
            }
        }
    } else if (type=='4') {
        const size_t rowBytes=(static_cast<size_t>(W)+7)/8;
        for (int y=0; y<H; ++y)
            for (int x=0; x<W; ++x) {
                const size_t bi=pos+static_cast<size_t>(y)*rowBytes+x/8;
                if (bi>=data.size()) break;
                const BYTE v=(data[bi]>>(7-(x%8)))&1 ? 0 : 255;
                const size_t px=static_cast<size_t>(y)*W+x;
                bgra[px*4+0]=bgra[px*4+1]=bgra[px*4+2]=v; bgra[px*4+3]=255;
            }
    } else if (type=='3') {
        for (size_t i=0; i<pixCount; ++i) {
            const int r=readInt(),g=readInt(),b=readInt(); if (r<0||g<0||b<0) break;
            bgra[i*4+0]=norm8(b); bgra[i*4+1]=norm8(g); bgra[i*4+2]=norm8(r); bgra[i*4+3]=255;
        }
    } else if (type=='2') {
        for (size_t i=0; i<pixCount; ++i) {
            const int v=readInt(); if (v<0) break;
            const BYTE bv=norm8(v); bgra[i*4+0]=bgra[i*4+1]=bgra[i*4+2]=bv; bgra[i*4+3]=255;
        }
    } else { // P1
        for (size_t i=0; i<pixCount; ++i) {
            const int v=readInt(); if (v<0) break;
            const BYTE bv=v ? 0 : 255;
            bgra[i*4+0]=bgra[i*4+1]=bgra[i*4+2]=bv; bgra[i*4+3]=255;
        }
    }

    return WrapInWIC(wicFac, bgra.data(), static_cast<UINT>(W), static_cast<UINT>(H), outW, outH);
}

// =============================================================================
//  OpenEXR  (.exr)  — via tinyexr
//  Tone-mapped to 8-bit LDR via per-channel Reinhard. Output: premultiplied BGRA.
// =============================================================================
static ComPtr<IWICBitmap> DecodeEXR(
    const std::vector<BYTE>& data,
    IWICImagingFactory* wicFac,
    UINT& outW, UINT& outH)
{
    float* rgba = nullptr;
    int w = 0, h = 0;
    const char* err = nullptr;
    if (LoadEXRFromMemory(&rgba, &w, &h, data.data(), data.size(), &err) != TINYEXR_SUCCESS) {
        if (err) FreeEXRErrorMessage(err);
        return nullptr;
    }

    const size_t pixCount = static_cast<size_t>(w)*h;
    std::vector<BYTE> bgra(pixCount*4);

    for (size_t i = 0; i < pixCount; ++i) {
        float lr=rgba[i*4+0], lg=rgba[i*4+1], lb=rgba[i*4+2];
        const float a=rgba[i*4+3];
        lr=lr/(1.0f+lr); lg=lg/(1.0f+lg); lb=lb/(1.0f+lb);
        lr=powf(lr<0.f?0.f:lr, 1.0f/2.2f);
        lg=powf(lg<0.f?0.f:lg, 1.0f/2.2f);
        lb=powf(lb<0.f?0.f:lb, 1.0f/2.2f);
        const BYTE ba=static_cast<BYTE>(a*255.0f+0.5f);
        if (ba==255) {
            bgra[i*4+0]=static_cast<BYTE>(lb*255.f+0.5f);
            bgra[i*4+1]=static_cast<BYTE>(lg*255.f+0.5f);
            bgra[i*4+2]=static_cast<BYTE>(lr*255.f+0.5f);
            bgra[i*4+3]=255;
        } else if (ba==0) {
            bgra[i*4+0]=bgra[i*4+1]=bgra[i*4+2]=bgra[i*4+3]=0;
        } else {
            bgra[i*4+0]=static_cast<BYTE>(lb*a*255.f+0.5f);
            bgra[i*4+1]=static_cast<BYTE>(lg*a*255.f+0.5f);
            bgra[i*4+2]=static_cast<BYTE>(lr*a*255.f+0.5f);
            bgra[i*4+3]=ba;
        }
    }

    free(rgba);
    return WrapInWIC(wicFac, bgra.data(), static_cast<UINT>(w), static_cast<UINT>(h), outW, outH);
}

// =============================================================================
//  Public API
// =============================================================================
namespace SimpleFormats {

bool IsSimpleFormat(const std::wstring& filePath) {
    const std::wstring e = LowerExt(filePath);
    return e==L".qoi" || e==L".tga"
        || e==L".jp2" || e==L".j2k" || e==L".j2c" || e==L".jpf" || e==L".jpx"
        || e==L".hdr" || e==L".exr"
        || e==L".ppm" || e==L".pgm" || e==L".pbm";
}

ComPtr<IWICBitmap> Decode(
    const std::wstring& filePath,
    const std::vector<BYTE>& fileBytes,
    IWICImagingFactory* wicFac,
    UINT& outWidth, UINT& outHeight)
{
    const std::wstring e = LowerExt(filePath);
    if (e==L".qoi") return DecodeQOI(fileBytes, wicFac, outWidth, outHeight);
    if (e==L".tga") return DecodeTGA(fileBytes, wicFac, outWidth, outHeight);
    if (e==L".jp2"||e==L".jpf"||e==L".jpx")
        return DecodeJP2(fileBytes, wicFac, outWidth, outHeight, OPJ_CODEC_JP2);
    if (e==L".j2k"||e==L".j2c")
        return DecodeJP2(fileBytes, wicFac, outWidth, outHeight, OPJ_CODEC_J2K);
    if (e==L".hdr") return DecodeHDR(fileBytes, wicFac, outWidth, outHeight);
    if (e==L".exr") return DecodeEXR(fileBytes, wicFac, outWidth, outHeight);
    if (e==L".ppm"||e==L".pgm"||e==L".pbm") return DecodePNM(fileBytes, wicFac, outWidth, outHeight);
    return nullptr;
}

} // namespace SimpleFormats
