// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <windows.h>
#include <string>

// =============================================================================
//  Utf8 — wide <-> UTF-8, and the one line-ending rule for text files
//
//  WHY THIS EXISTS AS A HEADER RATHER THAN INSIDE ONE .cpp. The conversion was
//  written out by hand in four places already (RotatingLogFile, RemoteLog,
//  RemoteBlacklist, RemoteCrypto), and a fifth was about to be added for the
//  history files. More importantly, none of those copies could be tested: they
//  are private statics inside translation units the test binary does not link.
//  This one is header-only, so qivTests reaches it.
//
//  🔥 THE BUG THAT PROMPTED IT. qivHistory.txt and qivFavorites.txt were read
//  and written through std::wifstream / std::wofstream with no imbue - the
//  default C locale, whose codecvt can only represent what fits in one byte.
//  Writing a single character outside that range does not drop the character;
//  it puts the STREAM into fail+bad state, and every line after it is silently
//  discarded. Measured: an ASCII path, a Cyrillic path, then a third ASCII path
//  gave good=1, then fail=1 bad=1, and the third line never reached the disk.
//  A user with a Cyrillic, Greek or accented folder name lost every history
//  entry after it on each rewrite, with nothing reported anywhere.
// =============================================================================

namespace Common::Utf8 {

    // Wide -> UTF-8. Empty in, empty out; a conversion failure also yields empty
    // rather than a partial string, so a caller writing the result cannot emit
    // half a path.
    inline std::string Encode(const std::wstring &w) {
        if (w.empty()) return {};
        const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                          nullptr, 0, nullptr, nullptr);
        if (n <= 0) return {};
        std::string out(static_cast<size_t>(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                            out.data(), n, nullptr, nullptr);
        return out;
    }

    // UTF-8 -> wide. Same empty-on-failure rule, same reason.
    inline std::wstring Decode(const std::string &s) {
        if (s.empty()) return {};
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                          nullptr, 0);
        if (n <= 0) return {};
        std::wstring out(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
        return out;
    }

    // The three bytes Notepad writes when somebody hand-edits one of these files
    // and picks "UTF-8 with BOM". Without stripping it the first path in the file
    // carries an invisible U+FEFF and is rejected as unusable.
    inline void StripBom(std::string &bytes) {
        if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
            static_cast<unsigned char>(bytes[1]) == 0xBB &&
            static_cast<unsigned char>(bytes[2]) == 0xBF)
            bytes.erase(0, 3);
    }

    // CRLF -> LF, by dropping every CR.
    //
    // Reading a text file in BINARY mode - which is the only way to control the
    // encoding - means the CRT no longer strips these. A CR left on the end of a
    // line becomes part of the path and makes it unusable, so every entry in a
    // CRLF file would be dropped. Removing all CRs rather than only the paired
    // ones is safe here: no path may contain one.
    inline void StripCr(std::wstring &text) {
        size_t w = 0;
        for (size_t r = 0; r < text.size(); ++r)
            if (text[r] != L'\r') text[w++] = text[r];
        text.resize(w);
    }

} // namespace Common::Utf8
