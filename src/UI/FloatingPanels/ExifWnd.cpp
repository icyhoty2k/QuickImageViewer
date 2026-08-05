// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#include "ExifWnd.h"
#include "UI/GdiPool.h" // pooled brushes and pens — never DeleteObject them
#include "../../GeoNames.h"
#include "../../Platform/Constants.h"
#include "../../Platform/ConstantsTheme.h"
#include "../../AppState.h"
#include "../../WorkerThread.h"
#include "../../Input/MouseHandler.h"
#include "Shortcuts.h"
#include <dwmapi.h>
#include <windowsx.h>
#include <shellapi.h>
#include <algorithm>
#include <filesystem>
#include <cwctype>
#include <wrl/client.h>
#include <shobjidl.h>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

static constexpr UINT WM_EXIF_READY = WM_APP + 100;
static constexpr UINT WM_EXIF_THUMB_READY = WM_APP + 101;

namespace UI {
    // ---------------------------------------------------------------------------
    // File-local helpers
    // ---------------------------------------------------------------------------

    static std::wstring WStrFromAnsi(const char *s) {
        if (!s || !*s) return {};
        int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
        if (n <= 1) n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
        if (n <= 1) return {};
        std::wstring ws(n - 1, L'\0');
        if (!MultiByteToWideChar(CP_UTF8, 0, s, -1, ws.data(), n))
            MultiByteToWideChar(CP_ACP, 0, s, -1, ws.data(), n);
        return ws;
    }

    static std::wstring PropToStr(const PROPVARIANT &pv) {
        if (pv.vt == VT_LPSTR && pv.pszVal) return WStrFromAnsi(pv.pszVal);
        if (pv.vt == VT_LPWSTR && pv.pwszVal) return pv.pwszVal;
        return {};
    }

    // VT_UI8 in WIC = ULARGE_INTEGER: LowPart = numerator, HighPart = denominator
    static std::wstring FormatRational(ULARGE_INTEGER ul, const wchar_t *unit = nullptr, int decimals = 2) {
        const ULONG num = ul.LowPart, den = ul.HighPart;
        if (den == 0) return L"—"; // em dash
        wchar_t buf[32];
        if (num == 0) {
            swprintf_s(buf, L"0%s", unit ? unit : L"");
            return buf;
        }
        if (num % den == 0) {
            swprintf_s(buf, L"%lu%s", num / den, unit ? unit : L"");
            return buf;
        }
        if (decimals == 1) swprintf_s(buf, L"%.1f%s", (double) num / den, unit ? unit : L"");
        else swprintf_s(buf, L"%.2f%s", (double) num / den, unit ? unit : L"");
        return buf;
    }

    static std::wstring FormatExposure(ULARGE_INTEGER ul) {
        const ULONG num = ul.LowPart, den = ul.HighPart;
        if (den == 0) return L"—";
        if (num == 0) return L"0 s";
        if (num >= den) {
            wchar_t buf[32];
            if (num % den == 0) swprintf_s(buf, L"%lu s", num / den);
            else swprintf_s(buf, L"%.2f s", (double) num / den);
            return buf;
        }
        // Simplify fraction via GCD
        ULONG g = num, r = den;
        while (r) {
            ULONG t = r;
            r = g % r;
            g = t;
        }
        const ULONG sn = num / g, sd = den / g;
        wchar_t buf[32];
        if (sn == 1) swprintf_s(buf, L"1/%lu s", sd);
        else swprintf_s(buf, L"%.4f s", (double) num / den);
        return buf;
    }

    static double GpsDecimal(const PROPVARIANT &pv, const std::wstring &ref) {
        if (!(pv.vt & VT_VECTOR) || pv.cauh.cElems < 3 || !pv.cauh.pElems) return 0.0;
        auto rat = [](ULARGE_INTEGER u) {
            return u.HighPart ? (double) u.LowPart / u.HighPart : 0.0;
        };
        double dec = rat(pv.cauh.pElems[0]) + rat(pv.cauh.pElems[1]) / 60.0 + rat(pv.cauh.pElems[2]) / 3600.0;
        if (!ref.empty() && (ref[0] == L'S' || ref[0] == L's' || ref[0] == L'W' || ref[0] == L'w'))
            dec = -dec;
        return dec;
    }

    static std::wstring FormatGps(const PROPVARIANT &pv, const std::wstring &ref) {
        if (!(pv.vt & VT_VECTOR) || pv.cauh.cElems < 3 || !pv.cauh.pElems) return {};
        auto rat = [](ULARGE_INTEGER u) {
            return u.HighPart ? (double) u.LowPart / u.HighPart : 0.0;
        };
        const double dec = rat(pv.cauh.pElems[0])
                           + rat(pv.cauh.pElems[1]) / 60.0
                           + rat(pv.cauh.pElems[2]) / 3600.0;
        wchar_t buf[64];
        swprintf_s(buf, L"%.6f° %s", dec, ref.empty() ? L"" : ref.c_str());
        return buf;
    }

    static void CopyToClipboard(HWND hwnd, const std::wstring &text) {
        if (text.empty() || !OpenClipboard(hwnd)) return;
        EmptyClipboard();
        const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (hMem) {
            void *ptr = GlobalLock(hMem);
            if (ptr) {
                memcpy(ptr, text.c_str(), bytes);
                GlobalUnlock(hMem);
            }
            SetClipboardData(CF_UNICODETEXT, hMem);
        }
        CloseClipboard();
    }

    static std::wstring FormatFileSize(LONGLONG bytes) {
        wchar_t b[32];
        if (bytes < 1024LL) swprintf_s(b, L"%lld B", bytes);
        else if (bytes < 1024LL * 1024) swprintf_s(b, L"%.1f KB", (double) bytes / 1024.0);
        else swprintf_s(b, L"%.2f MB", (double) bytes / (1024.0 * 1024.0));
        return b;
    }

    // ---------------------------------------------------------------------------
    // Init / Show
    // ---------------------------------------------------------------------------

    void ExifWnd::Init(HINSTANCE hInstance, HWND hParent, int8_t) {
        Init(hInstance, hParent);
    }

    void ExifWnd::Init(HINSTANCE hInstance, HWND hParent) {
        UINT dpi = static_cast<UINT>(app.dpiScale * 96.0f);
        InitFloating(hInstance, hParent, L"QIV_InfoWindow", L"Image Info",
                     MulDiv(520, dpi, 96), MulDiv(640, dpi, 96));
    }

    // One wheel "line" here is one metadata row. The base multiplies it by the
    // user's Mouse setting and the Shift accelerator — this only says what a
    // line means in this panel.
    int ExifWnd::ScrollLinePx(const UI::ScrollView &) const {
        return static_cast<int>(14 * app.dpiScale);
    }

    void ExifWnd::Refresh() {
        if (!m_hWnd || !IsWindowVisible(m_hWnd)) return;
        if (app.currentIndex < 0 || app.currentIndex >= static_cast<int>(app.playlist.size())) return;

        std::wstring path = app.playlist[app.currentIndex];
        int imgW = app.imgWidth;
        int imgH = app.imgHeight;
        HWND hwnd = m_hWnd;

        // EXIF metadata rows — fast, just reads file headers + metadata tags.
        g_ioWorker.PushTask([path, imgW, imgH, hwnd]() {
            auto *result = new ExifResult(GatherExifData(path, imgW, imgH));
            if (!PostMessageW(hwnd, WM_EXIF_READY, 0, reinterpret_cast<LPARAM>(result)))
                delete result;
        });

        // Shell thumbnail — separate task so EXIF rows appear immediately while
        // the thumbnail fills in (may generate+cache on first access).
        const LONG thumbSize = static_cast<LONG>(Constants::EXIF_THUMB_DISPLAY_SIZE * app.dpiScale);
        g_ioWorker.PushTask([path, hwnd, thumbSize]() {
            ComPtr<IShellItem> shellItem;
            if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&shellItem)))) return;

            ComPtr<IShellItemImageFactory> imgFactory;
            if (FAILED(shellItem->QueryInterface(IID_PPV_ARGS(&imgFactory)))) return;

            HBITMAP hBitmap = nullptr;
            const SIZE sz = {thumbSize, thumbSize};
            if (FAILED(imgFactory->GetImage(sz, static_cast<SIIGBF>(Constants::SHELL_THUMB_FLAGS), &hBitmap)) || !hBitmap) return;

            if (!PostMessageW(hwnd, WM_EXIF_THUMB_READY, 0, reinterpret_cast<LPARAM>(hBitmap)))
                DeleteObject(hBitmap);
        });
    }

    void ExifWnd::Show() {
        if (!m_hWnd) return;

        // Clear stale content immediately so the window shows empty while loading.
        m_rows.clear();
        if (m_thumbBitmap) {
            DeleteObject(m_thumbBitmap);
            m_thumbBitmap = nullptr;
        }
        m_thumbW = m_thumbH = 0;
        m_view.scrollY  = 0;
        m_view.contentH = 0;
        m_selectedRows.clear();
        m_anchorRow = -1;

        ShowCenterOverParent();
        InvalidateRect(m_hWnd, nullptr, FALSE);

        // Kick off async EXIF + thumbnail fetch (window is now visible so Refresh() proceeds).
        Refresh();
    }

    // ---------------------------------------------------------------------------
    // EXIF reading — static, callable from any COM-initialized thread
    // ---------------------------------------------------------------------------

    ExifWnd::ExifResult ExifWnd::GatherExifData(
            const std::wstring &path, int imgW, int imgH) {
        ExifResult result;
        auto &rows = result.rows;

        const auto addSection = [&](const wchar_t *title) {
            rows.push_back({title, {}, true});
        };
        const auto addRow = [&](const wchar_t *label, const std::wstring &value) {
            if (!value.empty()) rows.push_back({label, value, false});
        };

        // -- File section (no WIC) -------------------------------------------
        addSection(L"FILE");
        addRow(L"Name", fs::path(path).filename().wstring());

        if (imgW > 0 && imgH > 0) {
            wchar_t dim[32];
            swprintf_s(dim, L"%d × %d px", imgW, imgH);
            addRow(L"Dimensions", dim);
        }

        WIN32_FILE_ATTRIBUTE_DATA attr;
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attr)) {
            LONGLONG sz = (static_cast<LONGLONG>(attr.nFileSizeHigh) << 32) | attr.nFileSizeLow;
            addRow(L"File size", FormatFileSize(sz));
        }

        std::wstring ext = fs::path(path).extension().wstring();
        if (!ext.empty()) {
            std::wstring extUp;
            for (wchar_t c: ext) extUp += static_cast<wchar_t>(::towupper(c));
            addRow(L"Type", extUp);
        }

        // Create a thread-local WIC factory — the g_ioWorker thread is MTA, so using
        // app.wicFactory (STA, owned by the UI thread) from here is a COM violation.
        ComPtr<IWICImagingFactory> wicFactory;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&wicFactory))))
            return result;

        ComPtr<IWICBitmapDecoder> decoder;
        if (FAILED(wicFactory->CreateDecoderFromFilename(
            path.c_str(), nullptr, GENERIC_READ,
            WICDecodeMetadataCacheOnDemand, &decoder)))
            return result;

        ComPtr<IWICBitmapFrameDecode> frame;
        if (FAILED(decoder->GetFrame(0, &frame))) return result;

        ComPtr<IWICMetadataQueryReader> reader;
        if (FAILED(frame->GetMetadataQueryReader(&reader))) return result;

        // Try JPEG path first; fall back to TIFF path on VT_EMPTY
        const auto q = [&](const wchar_t *jpegPath, const wchar_t *tiffPath = nullptr) -> PROPVARIANT {
            PROPVARIANT pv;
            PropVariantInit(&pv);
            reader->GetMetadataByName(jpegPath, &pv);
            if (pv.vt == VT_EMPTY && tiffPath)
                reader->GetMetadataByName(tiffPath, &pv);
            return pv;
        };

        // -- GPS (first — highest priority for phone photos) -----------------
        {
            PROPVARIANT pvLatRef = q(L"/app1/ifd/gps/{ushort=1}", L"/ifd/gps/{ushort=1}");
            PROPVARIANT pvLat = q(L"/app1/ifd/gps/{ushort=2}", L"/ifd/gps/{ushort=2}");
            PROPVARIANT pvLonRef = q(L"/app1/ifd/gps/{ushort=3}", L"/ifd/gps/{ushort=3}");
            PROPVARIANT pvLon = q(L"/app1/ifd/gps/{ushort=4}", L"/ifd/gps/{ushort=4}");
            PROPVARIANT pvAltRef = q(L"/app1/ifd/gps/{ushort=5}", L"/ifd/gps/{ushort=5}");
            PROPVARIANT pvAlt = q(L"/app1/ifd/gps/{ushort=6}", L"/ifd/gps/{ushort=6}");
            PROPVARIANT pvGpsTime = q(L"/app1/ifd/gps/{ushort=7}", L"/ifd/gps/{ushort=7}");
            PROPVARIANT pvGpsDate = q(L"/app1/ifd/gps/{ushort=29}", L"/ifd/gps/{ushort=29}");

            const std::wstring latRef = PropToStr(pvLatRef);
            const std::wstring lonRef = PropToStr(pvLonRef);
            const std::wstring lat = FormatGps(pvLat, latRef);
            const std::wstring lon = FormatGps(pvLon, lonRef);

            if (!lat.empty() || !lon.empty()) {
                addSection(L"GPS");
                addRow(L"Latitude", lat);
                addRow(L"Longitude", lon);

                // "Open in Maps" — plain click opens Google Maps in browser
                if (!lat.empty() && !lon.empty()) {
                    const double latDec = GpsDecimal(pvLat, latRef);
                    const double lonDec = GpsDecimal(pvLon, lonRef);
                    wchar_t url[128], coords[64];
                    swprintf_s(url, L"https://maps.google.com/?q=%.6f,%.6f", latDec, lonDec);
                    swprintf_s(coords, L"%.6f, %.6f  →  open in maps", latDec, lonDec);
                    rows.push_back({L"Location", std::wstring(coords), false, std::wstring(url)});

                    const auto loc = GeoNames::Lookup(latDec, lonDec);
                    if (!loc.city.empty()) addRow(L"City", loc.city);
                    if (!loc.district.empty()) addRow(L"District", loc.district);
                    if (!loc.state.empty()) addRow(L"State", loc.state);
                    if (!loc.country.empty()) addRow(L"Country", loc.country);
                    if (!loc.continent.empty()) addRow(L"Continent", loc.continent);
                    if (!loc.capital.empty()) addRow(L"Capital", loc.capital);
                    if (!loc.currency.empty()) addRow(L"Currency", loc.currency);
                    if (!loc.phone.empty()) addRow(L"Phone", loc.phone);
                    if (!loc.timezone.empty()) addRow(L"Timezone", loc.timezone);
                }

                if (pvAlt.vt == VT_UI8 && pvAlt.uhVal.HighPart > 0) {
                    const double alt = static_cast<double>(pvAlt.uhVal.LowPart) / pvAlt.uhVal.HighPart;
                    const bool below = (pvAltRef.vt == VT_UI1 && pvAltRef.bVal == 1);
                    wchar_t b[32];
                    swprintf_s(b, L"%.1f m %s", alt, below ? L"(below sea)" : L"(above sea)");
                    addRow(L"Altitude", b);
                }

                if (pvGpsTime.vt == (VT_VECTOR | VT_UI8) && pvGpsTime.cauh.cElems >= 3) {
                    const auto ri = [](ULARGE_INTEGER u) -> UINT {
                        return u.HighPart ? u.LowPart / u.HighPart : 0;
                    };
                    const auto rd = [](ULARGE_INTEGER u) -> double {
                        return u.HighPart ? (double) u.LowPart / u.HighPart : 0.0;
                    };
                    UINT h = ri(pvGpsTime.cauh.pElems[0]);
                    UINT m = ri(pvGpsTime.cauh.pElems[1]);
                    double s = rd(pvGpsTime.cauh.pElems[2]);
                    std::wstring gpsDate = PropToStr(pvGpsDate);
                    if (gpsDate.size() >= 10 && gpsDate[4] == L':') {
                        gpsDate[4] = L'-';
                        gpsDate[7] = L'-';
                    }
                    wchar_t tb[16];
                    swprintf_s(tb, L"%02u:%02u:%04.1f UTC", h, m, s);
                    addRow(L"GPS time", gpsDate.empty() ? std::wstring(tb) : gpsDate + L" " + tb);
                }
            }

            PropVariantClear(&pvLatRef);
            PropVariantClear(&pvLat);
            PropVariantClear(&pvLonRef);
            PropVariantClear(&pvLon);
            PropVariantClear(&pvAltRef);
            PropVariantClear(&pvAlt);
            PropVariantClear(&pvGpsTime);
            PropVariantClear(&pvGpsDate);
        }

        // -- Camera ----------------------------------------------------------
        {
            PROPVARIANT pvMake = q(L"/app1/ifd/{ushort=271}", L"/ifd/{ushort=271}");
            PROPVARIANT pvModel = q(L"/app1/ifd/{ushort=272}", L"/ifd/{ushort=272}");
            PROPVARIANT pvLensMk = q(L"/app1/ifd/exif/{ushort=42035}", L"/ifd/exif/{ushort=42035}");
            PROPVARIANT pvLensMd = q(L"/app1/ifd/exif/{ushort=42036}", L"/ifd/exif/{ushort=42036}");

            std::wstring make = PropToStr(pvMake);
            std::wstring model = PropToStr(pvModel);
            std::wstring lensMk = PropToStr(pvLensMk);
            std::wstring lensMd = PropToStr(pvLensMd);

            PropVariantClear(&pvMake);
            PropVariantClear(&pvModel);
            PropVariantClear(&pvLensMk);
            PropVariantClear(&pvLensMd);

            if (!make.empty() || !model.empty() || !lensMk.empty() || !lensMd.empty()) {
                addSection(L"CAMERA");
                addRow(L"Make", make);
                addRow(L"Model", model);
                addRow(L"Lens make", lensMk);
                addRow(L"Lens model", lensMd);
            }
        }

        // -- Capture ---------------------------------------------------------
        {
            PROPVARIANT pvDt = q(L"/app1/ifd/exif/{ushort=36867}", L"/ifd/exif/{ushort=36867}");
            if (pvDt.vt == VT_EMPTY) {
                PropVariantClear(&pvDt);
                pvDt = q(L"/app1/ifd/{ushort=306}", L"/ifd/{ushort=306}");
            }
            PROPVARIANT pvExp = q(L"/app1/ifd/exif/{ushort=33434}", L"/ifd/exif/{ushort=33434}");
            PROPVARIANT pvFNum = q(L"/app1/ifd/exif/{ushort=33437}", L"/ifd/exif/{ushort=33437}");
            PROPVARIANT pvIso = q(L"/app1/ifd/exif/{ushort=34855}", L"/ifd/exif/{ushort=34855}");
            PROPVARIANT pvFl = q(L"/app1/ifd/exif/{ushort=37386}", L"/ifd/exif/{ushort=37386}");
            PROPVARIANT pvFl35 = q(L"/app1/ifd/exif/{ushort=41989}", L"/ifd/exif/{ushort=41989}");
            PROPVARIANT pvFlsh = q(L"/app1/ifd/exif/{ushort=37385}", L"/ifd/exif/{ushort=37385}");
            PROPVARIANT pvProg = q(L"/app1/ifd/exif/{ushort=34850}", L"/ifd/exif/{ushort=34850}");
            PROPVARIANT pvBias = q(L"/app1/ifd/exif/{ushort=37380}", L"/ifd/exif/{ushort=37380}");
            PROPVARIANT pvExpM = q(L"/app1/ifd/exif/{ushort=41986}", L"/ifd/exif/{ushort=41986}");
            PROPVARIANT pvMeter = q(L"/app1/ifd/exif/{ushort=37383}", L"/ifd/exif/{ushort=37383}");
            PROPVARIANT pvWB = q(L"/app1/ifd/exif/{ushort=41987}", L"/ifd/exif/{ushort=41987}");
            PROPVARIANT pvLS = q(L"/app1/ifd/exif/{ushort=37384}", L"/ifd/exif/{ushort=37384}");
            PROPVARIANT pvScene = q(L"/app1/ifd/exif/{ushort=41990}", L"/ifd/exif/{ushort=41990}");
            PROPVARIANT pvCS = q(L"/app1/ifd/exif/{ushort=40961}", L"/ifd/exif/{ushort=40961}");

            const bool hasCapture = pvDt.vt != VT_EMPTY || pvExp.vt != VT_EMPTY
                                    || pvFNum.vt != VT_EMPTY || pvIso.vt != VT_EMPTY
                                    || pvFl.vt != VT_EMPTY;
            if (hasCapture) {
                addSection(L"CAPTURE");

                std::wstring dt = PropToStr(pvDt);
                if (!dt.empty()) {
                    if (dt.size() >= 10 && dt[4] == L':') {
                        dt[4] = L'-';
                        dt[7] = L'-';
                    }
                    addRow(L"Date taken", dt);
                }

                if (pvExp.vt == VT_UI8) addRow(L"Exposure", FormatExposure(pvExp.uhVal));

                if (pvFNum.vt == VT_UI8) {
                    std::wstring fn = FormatRational(pvFNum.uhVal, nullptr, 1);
                    if (fn != L"—") addRow(L"Aperture", L"f/" + fn);
                }

                if (pvIso.vt == VT_UI2) {
                    wchar_t b[16];
                    swprintf_s(b, L"%u", pvIso.uiVal);
                    addRow(L"ISO", b);
                } else if (pvIso.vt == (VT_VECTOR | VT_UI2) && pvIso.caui.cElems > 0) {
                    wchar_t b[16];
                    swprintf_s(b, L"%u", pvIso.caui.pElems[0]);
                    addRow(L"ISO", b);
                }

                if (pvFl.vt == VT_UI8) addRow(L"Focal length", FormatRational(pvFl.uhVal, L" mm", 1));
                if (pvFl35.vt == VT_UI2 && pvFl35.uiVal > 0) {
                    wchar_t b[16];
                    swprintf_s(b, L"%u mm", pvFl35.uiVal);
                    addRow(L"35mm equiv", b);
                }

                if (pvFlsh.vt == VT_UI2) addRow(L"Flash", (pvFlsh.uiVal & 0x01) ? L"Fired" : L"No flash");

                if (pvProg.vt == VT_UI2) {
                    constexpr const wchar_t *progs[] = {
                        nullptr, L"Manual", L"Normal", L"Aperture priority",
                        L"Shutter priority", L"Creative", L"Action", L"Portrait", L"Landscape"
                    };
                    if (pvProg.uiVal < 9 && progs[pvProg.uiVal])
                        addRow(L"Program", progs[pvProg.uiVal]);
                }

                if (pvBias.vt == VT_I8) {
                    const LONG num = static_cast<LONG>(pvBias.hVal.LowPart);
                    const LONG den = pvBias.hVal.HighPart;
                    if (den != 0 && num != 0) {
                        wchar_t b[32];
                        swprintf_s(b, L"%+.1f EV", (double) num / den);
                        addRow(L"Exp. bias", b);
                    }
                }

                if (pvExpM.vt == VT_UI2) {
                    constexpr const wchar_t *modes[] = {L"Auto", L"Manual", L"Auto bracket"};
                    if (pvExpM.uiVal < 3) addRow(L"Exp. mode", modes[pvExpM.uiVal]);
                }

                if (pvMeter.vt == VT_UI2 && pvMeter.uiVal > 0) {
                    constexpr const wchar_t *meters[] = {
                        nullptr, L"Average", L"Centre-weighted", L"Spot",
                        L"Multi-spot", L"Pattern", L"Partial"
                    };
                    if (pvMeter.uiVal < 7 && meters[pvMeter.uiVal])
                        addRow(L"Metering", meters[pvMeter.uiVal]);
                }

                if (pvWB.vt == VT_UI2)
                    addRow(L"White balance", pvWB.uiVal == 0 ? L"Auto" : L"Manual");

                if (pvLS.vt == VT_UI2 && pvLS.uiVal > 0) {
                    const wchar_t *ls = nullptr;
                    switch (pvLS.uiVal) {
                        case 1: ls = L"Daylight";
                            break;
                        case 2: ls = L"Fluorescent";
                            break;
                        case 3: ls = L"Tungsten";
                            break;
                        case 4: ls = L"Flash";
                            break;
                        case 9: ls = L"Fine weather";
                            break;
                        case 10: ls = L"Cloudy";
                            break;
                        case 11: ls = L"Shade";
                            break;
                        case 255: ls = L"Other";
                            break;
                    }
                    if (ls) addRow(L"Light source", ls);
                }

                if (pvScene.vt == VT_UI2) {
                    constexpr const wchar_t *scenes[] = {L"Standard", L"Landscape", L"Portrait", L"Night"};
                    if (pvScene.uiVal < 4) addRow(L"Scene", scenes[pvScene.uiVal]);
                }

                if (pvCS.vt == VT_UI2) {
                    const wchar_t *cs = (pvCS.uiVal == 1)
                                            ? L"sRGB"
                                            : (pvCS.uiVal == 0xFFFF)
                                                  ? L"Uncalibrated"
                                                  : nullptr;
                    if (cs) addRow(L"Color space", cs);
                }
            }

            PropVariantClear(&pvDt);
            PropVariantClear(&pvExp);
            PropVariantClear(&pvFNum);
            PropVariantClear(&pvIso);
            PropVariantClear(&pvFl);
            PropVariantClear(&pvFl35);
            PropVariantClear(&pvFlsh);
            PropVariantClear(&pvProg);
            PropVariantClear(&pvBias);
            PropVariantClear(&pvExpM);
            PropVariantClear(&pvMeter);
            PropVariantClear(&pvWB);
            PropVariantClear(&pvLS);
            PropVariantClear(&pvScene);
            PropVariantClear(&pvCS);
        }

        // -- Author / rights -------------------------------------------------
        {
            PROPVARIANT pvDesc = q(L"/app1/ifd/{ushort=270}", L"/ifd/{ushort=270}");
            PROPVARIANT pvArtist = q(L"/app1/ifd/{ushort=315}", L"/ifd/{ushort=315}");
            PROPVARIANT pvCopy = q(L"/app1/ifd/{ushort=33432}", L"/ifd/{ushort=33432}");
            PROPVARIANT pvSoft = q(L"/app1/ifd/{ushort=305}", L"/ifd/{ushort=305}");

            std::wstring desc = PropToStr(pvDesc);
            std::wstring artist = PropToStr(pvArtist);
            std::wstring copy = PropToStr(pvCopy);
            std::wstring soft = PropToStr(pvSoft);

            PropVariantClear(&pvDesc);
            PropVariantClear(&pvArtist);
            PropVariantClear(&pvCopy);
            PropVariantClear(&pvSoft);

            if (!desc.empty() || !artist.empty() || !copy.empty() || !soft.empty()) {
                addSection(L"AUTHOR");
                addRow(L"Description", desc);
                addRow(L"Artist", artist);
                addRow(L"Copyright", copy);
                addRow(L"Software", soft);
            }
        }

        return result;
    }

    // ---------------------------------------------------------------------------
    // WM_PAINT + input
    // ---------------------------------------------------------------------------

    bool ExifWnd::OnKeyDown(WPARAM vk, bool /*ctrl*/, bool /*shift*/, bool /*alt*/) {
        switch (vk) {
            case 'M':
                Hide();
                return true;
            // A real page rather than a fixed 200px — the view knows its own
            // height, which is what a page means.
            case VK_PRIOR:
                m_view.ScrollBy(0, -m_view.Height());
                InvalidateRect(m_hWnd, nullptr, FALSE);
                return true;
            case VK_NEXT:
                m_view.ScrollBy(0, m_view.Height());
                InvalidateRect(m_hWnd, nullptr, FALSE);
                return true;
            case VK_HOME:
                m_view.scrollY = 0;
                InvalidateRect(m_hWnd, nullptr, FALSE);
                return true;
            case VK_END:
                // Was INT_MAX and left for the paint to clamp. Asking the view
                // means the offset is never briefly nonsense, which matters now
                // that the thumb is drawn from it.
                m_view.scrollY = m_view.MaxScrollY();
                InvalidateRect(m_hWnd, nullptr, FALSE);
                return true;
        }
        return false;
    }

    void ExifWnd::EnsureBackBuffer(HDC refDC, int w, int h) {
        if (m_bbDC && w == m_bbW && h == m_bbH) return;
        DestroyBackBuffer();
        m_bbDC = CreateCompatibleDC(refDC);
        m_bbBmp = CreateCompatibleBitmap(refDC, w, h);
        m_bbBmpOld = static_cast<HBITMAP>(SelectObject(m_bbDC, m_bbBmp));
        m_bbW = w;
        m_bbH = h;
    }

    void ExifWnd::DestroyBackBuffer() {
        if (m_bbDC) {
            if (m_bbBmpOld) SelectObject(m_bbDC, m_bbBmpOld);
            DeleteDC(m_bbDC);
            m_bbDC = nullptr;
        }
        if (m_bbBmp) {
            DeleteObject(m_bbBmp);
            m_bbBmp = nullptr;
        }
        m_bbBmpOld = nullptr;
        m_bbW = m_bbH = 0;
    }

    LRESULT ExifWnd::HandlePanelMessage(UINT message, WPARAM wParam, LPARAM lParam) {
        if (message == WM_ERASEBKGND) return 1;
        switch (message) {
            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC screenDC = BeginPaint(m_hWnd, &ps);
                RECT rc;
                GetClientRect(m_hWnd, &rc);
                EnsureBackBuffer(screenDC, rc.right, rc.bottom);
                HDC hdc = m_bbDC;

                const UINT dpi = static_cast<UINT>(app.dpiScale * 96.0f);
                const int pad = MulDiv(12, dpi, 96);
                const int sbW = UI::ScrollBarThicknessPx(app.dpiScale);
                const int rowH = MulDiv(22, dpi, 96);
                const int sectH = MulDiv(28, dpi, 96);
                const int gap = MulDiv(4, dpi, 96);
                const int labelW = MulDiv(150, dpi, 96);
                const int fSize = MulDiv(11, dpi, 96);
                const int sfSize = MulDiv(10, dpi, 96);
                const int valGap = MulDiv(6, dpi, 96);

                const COLORREF clrBg = GetBgColor();
                const COLORREF clrLbl = Constants::Theme::ThemedColor(
                        Constants::Theme::ExifWindow::LABEL_R,
                        Constants::Theme::ExifWindow::LABEL_G,
                        Constants::Theme::ExifWindow::LABEL_B, app.themeFactor);
                const COLORREF clrVal = Constants::Theme::ThemedGray(
                        Constants::Theme::ExifWindow::VALUE, app.themeFactor);
                const COLORREF clrSect = Constants::Theme::ThemedColor(
                        Constants::Theme::ExifWindow::SECTION_R,
                        Constants::Theme::ExifWindow::SECTION_G,
                        Constants::Theme::ExifWindow::SECTION_B, app.themeFactor);
                // The SECTION HEADER stripe. It read SCROLLBAR_TRACK_* until the
                // scrollbar palette moved out from under it — same colour, name
                // now says what it paints.
                const COLORREF clrSBg = Constants::Theme::ThemedColor(
                        Constants::Theme::ExifWindow::SECTION_BG_R,
                        Constants::Theme::ExifWindow::SECTION_BG_G,
                        Constants::Theme::ExifWindow::SECTION_BG_B, app.themeFactor);

                FillRect(hdc, &rc, UI::Gdi::Brush(clrBg));

                SetBkMode(hdc, TRANSPARENT);

                if (dpi != m_cachedFontDpi) {
                    if (m_hFontNorm) {
                        DeleteObject(m_hFontNorm);
                        m_hFontNorm = nullptr;
                    }
                    if (m_hFontBold) {
                        DeleteObject(m_hFontBold);
                        m_hFontBold = nullptr;
                    }
                    m_hFontNorm = CreateFontW(-fSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                              DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                              VARIABLE_PITCH, L"Segoe UI");
                    m_hFontBold = CreateFontW(-sfSize, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                              DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                              VARIABLE_PITCH, L"Segoe UI");
                    m_cachedFontDpi = dpi;
                }
                HFONT hOld = static_cast<HFONT>(SelectObject(hdc, m_hFontNorm));

                const int cTop = pad;
                const int cBot = rc.bottom - pad;
                // The content band's height is m_view.Height() now — the view
                // rect is set from cTop/cBot below and MaxScrollY reads it, so a
                // separate cH would be the same number computed twice.
                const int cR = rc.right - sbW - pad / 2;

                // Thumbnail column: right side of FILE section
                const int exifThumbSize = m_thumbBitmap ? MulDiv(Constants::EXIF_THUMB_DISPLAY_SIZE, dpi, 96) : 0;
                const int exifThumbGap = m_thumbBitmap ? MulDiv(Constants::EXIF_THUMB_COL_GAP, dpi, 96) : 0;
                const int thumbColW = exifThumbSize + exifThumbGap;
                const int textCR = cR - thumbColW; // right edge for FILE section text

                if (m_view.contentH == 0) {
                    int h = 0;
                    for (const auto &r: m_rows) h += r.isSection ? (sectH + gap) : rowH;
                    m_view.contentH = h + pad;
                }

                // The view is the content band, minus the bar's column. Set
                // before the clamp, because Height() is what MaxScrollY reads.
                m_view.view = {0, cTop, cR, cBot};
                m_view.Clamp();
                const int maxScroll = m_view.MaxScrollY();

                HRGN hClip = CreateRectRgn(0, cTop, cR, cBot);
                SelectClipRgn(hdc, hClip);

                // Draw selection highlights before text (same hover color as HistoryListWnd)
                if (!m_selectedRows.empty()) {
                    HBRUSH hSel = UI::Gdi::Brush(RGB(40, 60, 80));
                    int hy = cTop - m_view.scrollY;
                    for (int i = 0; i < static_cast<int>(m_rows.size()); ++i) {
                        const int hh = m_rows[i].isSection ? (sectH + gap) : rowH;
                        if (m_selectedRows.count(i)) {
                            RECT hr = {0, hy, cR, hy + hh};
                            FillRect(hdc, &hr, hSel);
                        }
                        hy += hh;
                    }
                }

                // Pre-scan FILE section bounds for right-side thumbnail positioning
                int fileRowsY = 0, fileRowsH = 0;
                {
                    bool inFile = false;
                    int scanY = cTop - m_view.scrollY;
                    for (const auto &r: m_rows) {
                        const int h = r.isSection ? (sectH + gap) : rowH;
                        if (r.isSection) {
                            if (r.label == L"FILE") {
                                inFile = true;
                                fileRowsY = scanY + h;
                            } else if (inFile) {
                                inFile = false;
                            }
                        } else if (inFile) {
                            fileRowsH += h;
                        }
                        scanY += h;
                    }
                }

                // Draw thumbnail in right column of FILE section
                if (m_thumbBitmap && exifThumbSize > 0 && m_thumbW > 0 && m_thumbH > 0) {
                    const float scale = std::min(
                            static_cast<float>(exifThumbSize) / m_thumbW,
                            static_cast<float>(exifThumbSize) / m_thumbH);
                    const int dstW = static_cast<int>(m_thumbW * scale);
                    const int dstH = static_cast<int>(m_thumbH * scale);
                    const int szGap = MulDiv(3, dpi, 96);
                    const int blockH = dstH + szGap + rowH; // image + gap + size label
                    const int thumbX = textCR + exifThumbGap + (exifThumbSize - dstW) / 2;
                    const int thumbY = fileRowsY + std::max(0, (fileRowsH - blockH) / 2);

                    if (thumbY < cBot && thumbY + dstH > cTop) {
                        HDC hMem = CreateCompatibleDC(hdc);
                        HBITMAP hPrevBmp = static_cast<HBITMAP>(SelectObject(hMem, m_thumbBitmap));
                        StretchBlt(hdc, thumbX, thumbY, dstW, dstH, hMem, 0, 0, m_thumbW, m_thumbH, SRCCOPY);
                        SelectObject(hMem, hPrevBmp);
                        DeleteDC(hMem);

                        // "256 × 170" label below thumbnail
                        const int szY = thumbY + dstH + szGap;
                        if (szY < cBot) {
                            wchar_t sizeText[32];
                            swprintf_s(sizeText, L"%d × %d", m_thumbW, m_thumbH);
                            RECT szR = {thumbX, szY, thumbX + dstW, szY + rowH};
                            SelectObject(hdc, m_hFontNorm);
                            SetTextColor(hdc, clrVal);
                            DrawTextW(hdc, sizeText, -1, &szR, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
                        }
                    }
                }

                int y = cTop - m_view.scrollY;
                bool inFileSection = false;
                for (const auto &row: m_rows) {
                    if (row.isSection) {
                        inFileSection = (row.label == L"FILE");
                        RECT sr = {0, y, rc.right, y + sectH};
                        FillRect(hdc, &sr, UI::Gdi::Brush(clrSBg));

                        SelectObject(hdc, m_hFontBold);
                        SetTextColor(hdc, clrSect);
                        RECT tr = {pad, y, cR, y + sectH};
                        DrawTextW(hdc, row.label.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                        y += sectH + gap;
                    } else {
                        SelectObject(hdc, m_hFontNorm);
                        SetTextColor(hdc, clrLbl);
                        RECT lr = {pad, y, pad + labelW, y + rowH};
                        DrawTextW(hdc, row.label.c_str(), -1, &lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

                        const COLORREF valColor = row.action.empty() ? clrVal : Constants::Theme::Markers::INFO;
                        SetTextColor(hdc, valColor);
                        const int valCR = (inFileSection && thumbColW > 0) ? textCR : cR;
                        RECT vr = {pad + labelW + valGap, y, valCR, y + rowH};
                        DrawTextW(hdc, row.value.c_str(), -1, &vr,
                                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                        y += rowH;
                    }
                }

                SelectClipRgn(hdc, nullptr);
                DeleteObject(hClip);

                // Scrollbar. Geometry and the thumb rect come from the shared
                // view now, so the hit box below is exactly what was drawn —
                // this panel used to compute the two independently and its drag
                // therefore did not track the cursor.
                m_view.ClearBars();
                if (maxScroll > 0) {
                    const int sbX = rc.right - sbW - MulDiv(3, dpi, 96);
                    m_view.vTrack = {sbX, cTop, sbX + sbW, cBot};
                }
                UI::DrawBars(hdc, m_view, app.dpiScale,
                             UI::ThemeScrollBarColors(app.themeFactor));

                SelectObject(hdc, hOld);
                // Fonts are cached members — not deleted here
                BitBlt(screenDC, 0, 0, rc.right, rc.bottom, hdc, 0, 0, SRCCOPY);
                EndPaint(m_hWnd, &ps);
                return 0;
            }


            case WM_SETCURSOR: {
                if (LOWORD(lParam) != HTCLIENT) break;
                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(m_hWnd, &pt);
                UINT dpi = static_cast<UINT>(app.dpiScale * 96.0f);
                const int pad = MulDiv(12, dpi, 96);
                const int rowH = MulDiv(22, dpi, 96);
                const int sectH = MulDiv(28, dpi, 96);
                const int gap = MulDiv(4, dpi, 96);
                RECT rc;
                GetClientRect(m_hWnd, &rc);
                const int sbX = rc.right - UI::ScrollBarThicknessPx(app.dpiScale) - MulDiv(3, dpi, 96);
                bool hand = false;
                if (pt.x < sbX) {
                    int y = pad - m_view.scrollY;
                    for (int i = 0; i < static_cast<int>(m_rows.size()); ++i) {
                        const int h = m_rows[i].isSection ? (sectH + gap) : rowH;
                        if (pt.y >= y && pt.y < y + h) {
                            hand = !m_rows[i].action.empty();
                            break;
                        }
                        y += h;
                    }
                }
                SetCursor(hand ? Constants::Cursors::CURR_CLICK : Constants::Cursors::CURR_DEFAULT);
                return TRUE;
            }

            case WM_NCHITTEST: {
                LRESULT hit = DefWindowProcW(m_hWnd, message, wParam, lParam);
                return (hit == HTCAPTION) ? HTCLIENT : hit;
            }

            case WM_LBUTTONDOWN:
            case WM_RBUTTONDOWN: {
                POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};

                if (MouseHandler::IsDragAction(message)) {
                    m_moving = true;
                    ClientToScreen(m_hWnd, &pt);
                    m_moveStartCursor = pt;
                    GetWindowRect(m_hWnd, &m_moveStartRect);
                    SetCapture(m_hWnd);
                    return 0;
                }

                // View-control button: scrollbar drag or row selection
                RECT rc;
                GetClientRect(m_hWnd, &rc);
                const UINT dpi = static_cast<UINT>(app.dpiScale * 96.0f);
                const int sbW = UI::ScrollBarThicknessPx(app.dpiScale);
                const int sbX = rc.right - sbW - MulDiv(3, dpi, 96);

                // The bar itself never reaches here — FloatingPanelWnd consumes
                // clicks on a thumb or a track before this panel is asked. What
                // is left is the bar's COLUMN with no bar drawn, which must not
                // fall through to row selection.
                if (pt.x >= sbX) {
                    // nothing to do
                } else {
                    const int pad = MulDiv(12, dpi, 96);
                    const int rowH = MulDiv(22, dpi, 96);
                    const int sectH = MulDiv(28, dpi, 96);
                    const int gap = MulDiv(4, dpi, 96);
                    int y = pad - m_view.scrollY;
                    int hit = -1;
                    for (int i = 0; i < static_cast<int>(m_rows.size()); ++i) {
                        const int h = m_rows[i].isSection ? (sectH + gap) : rowH;
                        if (pt.y >= y && pt.y < y + h) {
                            hit = i;
                            break;
                        }
                        y += h;
                    }
                    if (hit >= 0) {
                        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                        const bool plain = !ctrl && !shift;

                        if (shift && m_anchorRow >= 0) {
                            m_selectedRows.clear();
                            const int lo = std::min(m_anchorRow, hit);
                            const int hi = std::max(m_anchorRow, hit);
                            for (int i = lo; i <= hi; ++i) m_selectedRows.insert(i);
                        } else if (ctrl) {
                            if (m_selectedRows.erase(hit) == 0) m_selectedRows.insert(hit);
                            m_anchorRow = hit;
                        } else {
                            m_selectedRows.clear();
                            m_selectedRows.insert(hit);
                            m_anchorRow = hit;
                        }
                        InvalidateRect(m_hWnd, nullptr, FALSE);

                        // Plain click on action row (e.g. "Open in Maps") → open URL
                        if (plain && !m_rows[hit].action.empty()) {
                            ShellExecuteW(nullptr, L"open", m_rows[hit].action.c_str(),
                                          nullptr, nullptr, SW_SHOW);
                        } else {
                            // Build copy text from all selected rows in display order
                            std::wstring text;
                            for (int i = 0; i < static_cast<int>(m_rows.size()); ++i) {
                                if (!m_selectedRows.count(i)) continue;
                                if (!text.empty()) text += L"\r\n";
                                const auto &row = m_rows[i];
                                text += row.isSection ? row.label : row.label + L": " + row.value;
                            }
                            CopyToClipboard(m_hWnd, text);
                        }
                    } else if (!(GetKeyState(VK_CONTROL) & 0x8000) && !(GetKeyState(VK_SHIFT) & 0x8000)) {
                        if (!m_selectedRows.empty()) {
                            m_selectedRows.clear();
                            m_anchorRow = -1;
                            InvalidateRect(m_hWnd, nullptr, FALSE);
                        }
                    }
                }
                return 0;
            }

            case WM_MOUSEMOVE: {
                if (m_moving) {
                    POINT cur;
                    GetCursorPos(&cur);
                    SetWindowPos(m_hWnd, nullptr,
                                 m_moveStartRect.left + (cur.x - m_moveStartCursor.x),
                                 m_moveStartRect.top + (cur.y - m_moveStartCursor.y),
                                 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                    return 0;
                }
                // A thumb drag never reaches here — the base holds capture and
                // consumes the move while one is in progress.
                return 0;
            }

            case WM_LBUTTONUP:
            case WM_RBUTTONUP:
                if (m_moving) {
                    m_moving = false;
                    ReleaseCapture();
                }
                return 0;

            case WM_EXIF_READY: {
                auto *result = reinterpret_cast<ExifResult *>(lParam);
                m_rows = std::move(result->rows);
                delete result;
                m_view.contentH = 0;
                m_view.scrollY  = 0;
                m_selectedRows.clear();
                m_anchorRow = -1;
                InvalidateRect(m_hWnd, nullptr, FALSE);
                return 0;
            }

            case WM_EXIF_THUMB_READY: {
                if (m_thumbBitmap) {
                    DeleteObject(m_thumbBitmap);
                    m_thumbBitmap = nullptr;
                }
                m_thumbBitmap = reinterpret_cast<HBITMAP>(lParam);
                if (m_thumbBitmap) {
                    BITMAP bm{};
                    GetObject(m_thumbBitmap, sizeof(bm), &bm);
                    m_thumbW = bm.bmWidth;
                    m_thumbH = bm.bmHeight;
                }
                m_view.contentH = 0; // layout changes when thumb column appears
                InvalidateRect(m_hWnd, nullptr, FALSE);
                return 0;
            }

            case WM_DESTROY:
                if (m_thumbBitmap) {
                    DeleteObject(m_thumbBitmap);
                    m_thumbBitmap = nullptr;
                }
                return 0;

            case WM_CLOSE:
                Hide();
                return 0;
        }

        return DefWindowProcW(m_hWnd, message, wParam, lParam);
    }
} // namespace UI
