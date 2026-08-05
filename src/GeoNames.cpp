// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#include "GeoNames.h"
#include <windows.h>
#include <miniz.h>
#include <vector>
#include <mutex>
#include <cmath>
#include <cstdlib>
#include <unordered_map>
#include "../resources/resource.h"

namespace GeoNames {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static std::wstring Utf8ToWStr(const char* s, int len) {
    if (len <= 0 || !s) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s, len, nullptr, 0);
    if (n <= 0) return {};
    std::wstring ws(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, len, ws.data(), n);
    return ws;
}

// Reads a Windows RCDATA resource and zlib-decompresses it.
// Binary format: [uint32 LE uncompressed_size][zlib data]
static std::vector<char> LoadAndDecompress(int resourceId) {
    HRSRC hRes = FindResourceW(nullptr, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
    if (!hRes) return {};
    HGLOBAL hGlob = LoadResource(nullptr, hRes);
    if (!hGlob) return {};
    const BYTE* pData    = static_cast<const BYTE*>(LockResource(hGlob));
    const DWORD compSize = SizeofResource(nullptr, hRes);
    if (!pData || compSize < 4) return {};

    uint32_t uncompSize = 0;
    memcpy(&uncompSize, pData, 4);
    if (uncompSize == 0 || uncompSize > 128u * 1024 * 1024) return {};

    std::vector<char> buf(static_cast<size_t>(uncompSize) + 1, '\0');
    mz_ulong destLen = uncompSize;
    if (mz_uncompress(reinterpret_cast<mz_uint8*>(buf.data()),
                      &destLen, pData + 4, compSize - 4) != MZ_OK) return {};
    buf[destLen] = '\0';
    return buf;
}

// ---------------------------------------------------------------------------
// Data structures
// ---------------------------------------------------------------------------
struct City {
    float       lat, lon;
    std::string name;
    char        country[3]; // null-terminated 2-char ISO code
    std::string admin1;
    std::string admin2;
    std::string timezone;
};

struct CountryInfo {
    std::wstring name;
    std::wstring capital;
    std::wstring continent;
    std::wstring currency;
    std::wstring phone;
};

static std::vector<City>                              s_cities;
static std::unordered_map<std::string, std::wstring>  s_admin1;   // "US.CA"      → L"California"
static std::unordered_map<std::string, std::wstring>  s_admin2;   // "US.CA.037"  → L"Los Angeles County"
static std::unordered_map<std::string, CountryInfo>   s_countries; // "US"         → {...}
static std::once_flag                                 s_loadOnce;

// ---------------------------------------------------------------------------
// Parsers
// ---------------------------------------------------------------------------
static void ParseCities(const std::vector<char>& buf) {
    s_cities.reserve(150'000);
    const char* p   = buf.data();
    const char* end = p + buf.size() - 1; // exclude null terminator

    auto nt = [](const char* from, const char* limit) -> const char* {
        return static_cast<const char*>(memchr(from, '\t', limit - from));
    };

    while (p < end) {
        const char* nl = static_cast<const char*>(memchr(p, '\n', end - p));
        if (!nl) nl = end;

        // lat\tlon\tname\tcountry\tadmin1\tadmin2\ttimezone
        if (nl > p) {
            const char* t1 = nt(p,     nl); if (!t1) { p = nl+1; continue; }
            const char* t2 = nt(t1+1,  nl); if (!t2) { p = nl+1; continue; }
            const char* t3 = nt(t2+1,  nl); if (!t3) { p = nl+1; continue; }
            const char* t4 = nt(t3+1,  nl); if (!t4) { p = nl+1; continue; }
            const char* t5 = nt(t4+1,  nl); if (!t5) { p = nl+1; continue; }
            const char* t6 = nt(t5+1,  nl); // may be null if no timezone

            City c;
            c.lat  = static_cast<float>(strtod(p,    nullptr));
            c.lon  = static_cast<float>(strtod(t1+1, nullptr));
            c.name.assign(t2+1, t3 - t2 - 1);
            c.country[0] = (t4 - t3 >= 2) ? t3[1] : '\0';
            c.country[1] = (t4 - t3 >= 3) ? t3[2] : '\0';
            c.country[2] = '\0';
            c.admin1.assign(t4+1, t5 - t4 - 1);
            const char* a2end = t6 ? t6 : nl;
            if (a2end > t5+1) c.admin2.assign(t5+1, a2end - t5 - 1);
            if (t6 && nl > t6+1) c.timezone.assign(t6+1, nl - t6 - 1);
            s_cities.push_back(std::move(c));
        }
        p = nl + 1;
    }
}

static void ParseKeyValue(const std::vector<char>& buf,
                          std::unordered_map<std::string, std::wstring>& out) {
    const char* p   = buf.data();
    const char* end = p + buf.size() - 1;
    while (p < end) {
        const char* nl  = static_cast<const char*>(memchr(p, '\n', end - p));
        if (!nl) nl = end;
        if (nl > p) {
            const char* tab = static_cast<const char*>(memchr(p, '\t', nl - p));
            if (tab && nl > tab+1) {
                std::string  key(p,     tab - p);
                std::wstring val = Utf8ToWStr(tab+1, static_cast<int>(nl - tab - 1));
                out.emplace(std::move(key), std::move(val));
            }
        }
        p = nl + 1;
    }
}

static void ParseCountries(const std::vector<char>& buf) {
    const char* p   = buf.data();
    const char* end = p + buf.size() - 1;
    auto nt = [](const char* from, const char* limit) -> const char* {
        return static_cast<const char*>(memchr(from, '\t', limit - from));
    };
    while (p < end) {
        const char* nl = static_cast<const char*>(memchr(p, '\n', end - p));
        if (!nl) nl = end;
        // ISO\tName\tCapital\tContinent\tCurrency\tPhone
        if (nl > p) {
            const char* t1 = nt(p,    nl);
            const char* t2 = t1 ? nt(t1+1, nl) : nullptr;
            const char* t3 = t2 ? nt(t2+1, nl) : nullptr;
            const char* t4 = t3 ? nt(t3+1, nl) : nullptr;
            const char* t5 = t4 ? nt(t4+1, nl) : nullptr;
            if (t1 && t1 > p) {
                std::string   iso(p, t1 - p);
                CountryInfo   ci;
                if (t2)       ci.name      = Utf8ToWStr(t1+1, static_cast<int>(t2-t1-1));
                if (t3)       ci.capital   = Utf8ToWStr(t2+1, static_cast<int>(t3-t2-1));
                if (t4)       ci.continent = Utf8ToWStr(t3+1, static_cast<int>(t4-t3-1));
                if (t5)       ci.currency  = Utf8ToWStr(t4+1, static_cast<int>(t5-t4-1));
                if (t5 && nl > t5+1)
                              ci.phone     = Utf8ToWStr(t5+1, static_cast<int>(nl-t5-1));
                s_countries.emplace(std::move(iso), std::move(ci));
            }
        }
        p = nl + 1;
    }
}

static void Load() {
    auto cities  = LoadAndDecompress(IDR_CITIES_DATA);
    auto admin1  = LoadAndDecompress(IDR_ADMIN1_DATA);
    auto admin2  = LoadAndDecompress(IDR_ADMIN2_DATA);
    auto country = LoadAndDecompress(IDR_COUNTRY_DATA);

    if (!cities.empty())  ParseCities(cities);
    if (!admin1.empty())  ParseKeyValue(admin1,  s_admin1);
    if (!admin2.empty())  ParseKeyValue(admin2,  s_admin2);
    if (!country.empty()) ParseCountries(country);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void WarmUp() {
    std::call_once(s_loadOnce, Load);
}

Location Lookup(double lat, double lon) {
    std::call_once(s_loadOnce, Load);
    if (s_cities.empty()) return {};

    const float fLat   = static_cast<float>(lat);
    const float fLon   = static_cast<float>(lon);
    const float cosLat = cosf(fLat * 0.01745329252f);

    float       bestDist = FLT_MAX;
    const City* best     = nullptr;
    for (const auto& c : s_cities) {
        const float dlat = c.lat - fLat;
        const float dlon = (c.lon - fLon) * cosLat;
        const float d    = dlat*dlat + dlon*dlon;
        if (d < bestDist) { bestDist = d; best = &c; }
    }
    if (!best) return {};

    Location loc;
    loc.city     = Utf8ToWStr(best->name.c_str(),     static_cast<int>(best->name.size()));
    loc.timezone = Utf8ToWStr(best->timezone.c_str(), static_cast<int>(best->timezone.size()));

    // Admin1 (state / province)
    std::string a1key;
    a1key.reserve(10);
    a1key.append(best->country, 2);
    a1key += '.';
    a1key += best->admin1;
    auto it1 = s_admin1.find(a1key);
    if (it1 != s_admin1.end()) loc.state = it1->second;

    // Admin2 (district / county)
    std::string a2key;
    a2key.reserve(18);
    a2key.append(best->country, 2);
    a2key += '.';
    a2key += best->admin1;
    a2key += '.';
    a2key += best->admin2;
    auto it2 = s_admin2.find(a2key);
    if (it2 != s_admin2.end()) loc.district = it2->second;

    // Country info
    std::string ccKey(best->country, 2);
    auto itc = s_countries.find(ccKey);
    if (itc != s_countries.end()) {
        loc.country   = itc->second.name;
        loc.capital   = itc->second.capital;
        loc.continent = itc->second.continent;
        loc.currency  = itc->second.currency;
        loc.phone     = itc->second.phone;
    }

    return loc;
}

} // namespace GeoNames
