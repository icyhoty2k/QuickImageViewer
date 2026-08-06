// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

// winsock2.h MUST come before anything that pulls in windows.h — RemoteSettings.h
// includes windows.h, which would otherwise drag in the original winsock.h and
// redefine every socket type.
#include <winsock2.h>
#include <ws2tcpip.h>

#include "RemoteSettings.h"

#include <cstdint>
#include <cstring>

// =============================================================================
// AddressMatch — who is allowed to connect, decided from text.
//
// SPLIT OUT OF RemoteSettings.cpp SO IT CAN BE TESTED. This is the most
// security-relevant pure logic in the program: `InList` is what the accept gate
// asks before letting a peer speak, and a matcher that is too permissive never
// announces itself — there is no crash, no error, just a machine that admits
// somebody it should not have.
//
// It could not be tested where it lived, because RemoteSettings.cpp also pulls
// in AppState, RemoteBlacklist, RemoteCrypto and DedicatedSettings, and a unit
// test cannot link half the application. Nothing here reaches past winsock and
// the standard library, so test/qivTests.cpp links this one file.
//
// DECLARATIONS DID NOT MOVE. They are still in RemoteSettings.h, so every call
// site is untouched and this is a file boundary rather than a change in
// behaviour. Nothing in this file was rewritten during the move.
// =============================================================================

namespace Remote {

namespace {

    // A peer address can arrive with a scope id — "fe80::1%12" is what
    // GetNameInfoW returns for a link-local peer. It identifies the INTERFACE,
    // not the address, and InetPton rejects it, so it comes off first. Removing
    // it is also what lets a link-local address be written in a rule at all.
    std::wstring StripScope(const std::wstring &s) {
        const size_t pct = s.find(L'%');
        return pct == std::wstring::npos ? s : s.substr(0, pct);
    }

    // Host byte order, so the range comparison below is ordinary arithmetic.
    bool ParseV4(const std::wstring &s, uint32_t &out) {
        in_addr a{};
        if (InetPtonW(AF_INET, s.c_str(), &a) != 1) return false;
        out = ntohl(a.S_un.S_addr);
        return true;
    }

    bool ParseV6(const std::wstring &s, BYTE out[16]) {
        in6_addr a{};
        if (InetPtonW(AF_INET6, StripScope(s).c_str(), &a) != 1) return false;
        memcpy(out, &a, 16);
        return true;
    }

    // "<address>/<bits>", either family.
    struct Cidr {
        bool     v6      = false;
        uint32_t net4    = 0;
        BYTE     net6[16]{};
        int      bits    = 0;
    };

    // Separated from the matching so the SAME check can validate an entry as a
    // user types it. An unparseable rule must be refused at the panel, not left
    // in the list quietly matching nothing — a rule that is believed and does
    // not work is worse than one that was rejected out loud.
    bool ParseCidr(const std::wstring &pattern, Cidr &out) {
        const size_t slash = pattern.find(L'/');
        if (slash == std::wstring::npos) return false;

        const std::wstring net  = pattern.substr(0, slash);
        const std::wstring bits = pattern.substr(slash + 1);
        if (net.empty() || bits.empty()) return false;
        if (bits.find_first_not_of(L"0123456789") != std::wstring::npos) return false;
        out.bits = static_cast<int>(wcstol(bits.c_str(), nullptr, 10));

        out.v6 = net.find(L':') != std::wstring::npos;
        if (out.v6) {
            if (out.bits < 0 || out.bits > 128) return false;
            return ParseV6(net, out.net6);
        }
        if (out.bits < 0 || out.bits > 32) return false;
        return ParseV4(net, out.net4);
    }

    // "192.168.0.10-192.168.0.50", or the shorthand "192.168.0.10-50" in which
    // the upper bound names only the last octet.
    //
    // IPv4 ONLY, deliberately. A v6 range is written as a prefix, which /N
    // already expresses exactly and unambiguously — and a 128-bit range needs
    // arithmetic this does not otherwise require.
    bool ParseRange(const std::wstring &pattern, uint32_t &lo, uint32_t &hi) {
        const size_t dash = pattern.find(L'-');
        if (dash == std::wstring::npos) return false;

        const std::wstring loText = pattern.substr(0, dash);
        const std::wstring hiText = pattern.substr(dash + 1);
        if (loText.empty() || hiText.empty()) return false;
        if (loText.find(L':') != std::wstring::npos) return false;
        if (!ParseV4(loText, lo)) return false;

        if (hiText.find(L'.') == std::wstring::npos) {
            // Shorthand — the upper bound replaces the final octet.
            if (hiText.find_first_not_of(L"0123456789") != std::wstring::npos) return false;
            const long last = wcstol(hiText.c_str(), nullptr, 10);
            if (last < 0 || last > 255) return false;
            hi = (lo & 0xFFFFFF00u) | static_cast<uint32_t>(last);
        } else if (!ParseV4(hiText, hi)) {
            return false;
        }

        // Backwards bounds match nothing, so they are a typo rather than a rule.
        return hi >= lo;
    }

} // namespace

// A stored list entry must at least look like an address literal. Anything with
// a path separator, a space in the middle or an obviously illegal character is
// dropped rather than silently compared against and never matched.
bool LooksLikeAddress(const std::wstring &s) {
    if (s.empty() || s.size() > 64) return false;
    for (wchar_t c : s) {
        const bool ok = (c >= L'0' && c <= L'9') ||
                        (c >= L'a' && c <= L'f') ||
                        (c >= L'A' && c <= L'F') ||
                        c == L'.' || c == L':' || c == L'*' ||
                        c == L'/' || c == L'-';
        if (!ok) return false;
    }

    // A CIDR or a range must PARSE, not merely be spelled out of legal
    // characters. The filter above happily passes "192.168.0.0/99" and
    // "10.0.0.5-1", and an entry that can never match any address is precisely
    // what this function exists to drop: the panel reports what it pruned, so a
    // refusal is visible, while a rule that is kept and silently matches nothing
    // is believed.
    //
    // The older forms are left alone. None of them can contain '/' or '-', so
    // nothing that worked before reaches this and changes behaviour.
    if (s.find(L'/') != std::wstring::npos) {
        Cidr c;
        return ParseCidr(s, c);
    }
    if (s.find(L'-') != std::wstring::npos) {
        uint32_t lo = 0, hi = 0;
        return ParseRange(s, lo, hi);
    }
    return true;
}

bool SameHost(const std::wstring &a, const std::wstring &b) {
    if (a.empty() || b.empty()) return false;

    // v6 first: an IPv4-mapped literal ("::ffff:1.2.3.4") parses as v6 and must
    // not be silently compared against the plain v4 form as though the two were
    // unrelated — checking v6 first keeps both sides in the same family.
    BYTE a6[16]{}, b6[16]{};
    const bool aIs6 = ParseV6(a, a6);
    const bool bIs6 = ParseV6(b, b6);
    if (aIs6 || bIs6) {
        if (aIs6 != bIs6) return false;
        return memcmp(a6, b6, 16) == 0;
    }

    uint32_t a4 = 0, b4 = 0;
    const bool aIs4 = ParseV4(a, a4);
    const bool bIs4 = ParseV4(b, b4);
    if (aIs4 || bIs4) {
        if (aIs4 != bIs4) return false;   // a literal and a name are not equal
        return a4 == b4;
    }

    // Neither parses — two host names. Case-insensitive, and nothing else: a
    // trailing dot or a different domain suffix is a different entry as far as
    // this list is concerned.
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

std::wstring BlockScope(const std::wstring &address) {
    BYTE b[16]{};
    // Not v6 — an IPv4 literal, or something that does not parse at all. Either
    // way there is no prefix to widen to, and returning it untouched keeps the
    // v4 behaviour exactly as it was.
    if (!ParseV6(address, b)) return address;

    // IPv4-MAPPED ("::ffff:192.168.1.5") IS NOT A v6 HOST. Its low 64 bits carry
    // a v4 address, so a /64 over one would read as ::ffff:0:0/64 and block the
    // ENTIRE IPv4 internet from a single wrong password. AcceptedPeerAddress
    // already flattens these to dotted quad before anything here sees them, so
    // this is a second line rather than the first — but it is the one mistake in
    // this function that would be catastrophic rather than merely wrong.
    bool mapped = (b[10] == 0xFF && b[11] == 0xFF);
    for (int i = 0; i < 10 && mapped; ++i)
        if (b[i] != 0) mapped = false;
    if (mapped) return address;

    // An all-zero prefix is "::" and its neighbours — loopback lives there, and
    // a rule spanning it is never what anyone wants. Nothing routable arrives
    // with one, so this is a guard, not a case.
    if (!b[0] && !b[1] && !b[2] && !b[3] && !b[4] && !b[5] && !b[6] && !b[7])
        return address;

    // The first four groups, then "::" — the canonical way to write a /64, and
    // what every other tool prints. Lower case and no leading zeros, so two
    // spellings of one prefix cannot produce two blacklist rows.
    wchar_t buf[48]{};
    swprintf_s(buf, L"%x:%x:%x:%x::/64",
               (b[0] << 8) | b[1], (b[2] << 8) | b[3],
               (b[4] << 8) | b[5], (b[6] << 8) | b[7]);
    return buf;
}

bool AddressMatches(const std::wstring &pattern, const std::wstring &addr) {
    if (pattern.empty()) return false;
    if (pattern == L"*") return true;

    // TEXT PREFIX, and it stays one. "192.168.1.*" is what most rules are, it
    // costs nothing to answer, and rewriting it as a /24 would change what
    // existing lists mean — the two are not the same rule. Note the sharp edge
    // it keeps: the prefix is compared as CHARACTERS, so "192.168.1*" without
    // the trailing dot also matches 192.168.10.x and 192.168.100.x. That is why
    // /N now exists beside it.
    if (pattern.back() == L'*') {
        const std::wstring prefix = pattern.substr(0, pattern.size() - 1);
        return addr.size() >= prefix.size() &&
               _wcsnicmp(addr.c_str(), prefix.c_str(), prefix.size()) == 0;
    }

    // "192.168.0.0/24", "2001:db8::/32". FAMILIES MUST AGREE: a v4 rule never
    // matches a v6 peer, which is what stops a /24 covering addresses that
    // merely begin with the same bytes in another notation.
    if (pattern.find(L'/') != std::wstring::npos) {
        Cidr c;
        if (!ParseCidr(pattern, c)) return false;

        if (c.v6) {
            BYTE a[16];
            if (!ParseV6(addr, a)) return false;
            const int whole = c.bits / 8, rest = c.bits % 8;
            if (memcmp(c.net6, a, static_cast<size_t>(whole)) != 0) return false;
            if (rest == 0) return true;
            const BYTE mask = static_cast<BYTE>(0xFF << (8 - rest));
            return (c.net6[whole] & mask) == (a[whole] & mask);
        }

        uint32_t a = 0;
        if (!ParseV4(addr, a)) return false;
        // /0 is every address, and it is handled here rather than by the shift:
        // shifting a 32-bit value by 32 is undefined behaviour, not zero.
        if (c.bits == 0) return true;
        const uint32_t mask = 0xFFFFFFFFu << (32 - c.bits);
        return (c.net4 & mask) == (a & mask);
    }

    // "192.168.0.10-192.168.0.50" or "192.168.0.10-50".
    if (pattern.find(L'-') != std::wstring::npos) {
        uint32_t lo = 0, hi = 0, a = 0;
        if (!ParseRange(pattern, lo, hi)) return false;
        if (!ParseV4(addr, a)) return false;
        return a >= lo && a <= hi;
    }

    // EXACT — and numerically first, because one address has more than one
    // spelling. "2001:db8::1" and "2001:0db8:0000:0000:0000:0000:0000:0001" are
    // the same host, and a rule typed the long way round matching nothing would
    // be a silent lockout. This also lets "fe80::1" match a peer that arrives
    // as "fe80::1%12" with its scope id attached.
    //
    // Text comparison remains the fallback, so anything that is not an address
    // literal behaves exactly as it did.
    uint32_t p4 = 0, a4 = 0;
    if (ParseV4(pattern, p4) && ParseV4(addr, a4)) return p4 == a4;

    BYTE p6[16], a6[16];
    if (ParseV6(pattern, p6) && ParseV6(addr, a6)) return memcmp(p6, a6, 16) == 0;

    return _wcsicmp(pattern.c_str(), addr.c_str()) == 0;
}

bool InList(const std::vector<std::wstring> &list, const std::wstring &addr) {
    for (const std::wstring &p : list)
        if (AddressMatches(p, addr)) return true;
    return false;
}

} // namespace Remote
