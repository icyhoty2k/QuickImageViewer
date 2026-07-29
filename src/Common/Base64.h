#pragma once
#include <string>
#include <vector>

// =============================================================================
// Base64 — for putting image BYTES on a line-based text protocol.
//
// Hand-written rather than CryptBinaryToStringA: that call needs crypt32 linked
// and its flag combinations decide padding and line breaks for you, which is
// exactly the kind of hidden behaviour this codebase avoids. Forty lines that do
// precisely one thing are easier to trust than a flag word.
//
// Encode produces UNPADDED-free, standard alphabet output with '=' padding and no
// line breaks: the caller decides how to chunk it, because the protocol's line
// limit is the caller's business.
//
// Decode ignores anything that is not an alphabet character — whitespace, CR, LF
// — so a body reassembled from several wire lines needs no cleaning first.
// =============================================================================
namespace Common::Base64 {

    inline const char *Alphabet() {
        return "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    }

    inline std::string Encode(const unsigned char *data, size_t n) {
        const char *A = Alphabet();
        std::string out;
        out.reserve(((n + 2) / 3) * 4);

        size_t i = 0;
        for (; i + 3 <= n; i += 3) {
            const unsigned v = (static_cast<unsigned>(data[i]) << 16) |
                               (static_cast<unsigned>(data[i + 1]) << 8) |
                               static_cast<unsigned>(data[i + 2]);
            out += A[(v >> 18) & 0x3F];
            out += A[(v >> 12) & 0x3F];
            out += A[(v >> 6) & 0x3F];
            out += A[v & 0x3F];
        }

        // The tail: one or two bytes left, padded to a full quartet.
        if (i < n) {
            const bool two = (n - i) == 2;
            const unsigned v = (static_cast<unsigned>(data[i]) << 16) |
                               (two ? (static_cast<unsigned>(data[i + 1]) << 8) : 0u);
            out += A[(v >> 18) & 0x3F];
            out += A[(v >> 12) & 0x3F];
            out += two ? A[(v >> 6) & 0x3F] : '=';
            out += '=';
        }
        return out;
    }

    // Reverse lookup, built once. -1 marks "not part of the alphabet", which is
    // how whitespace and line breaks are skipped rather than rejected.
    inline const signed char *ReverseTable() {
        static signed char table[256];
        static bool built = false;
        if (!built) {
            for (int i = 0; i < 256; ++i) table[i] = -1;
            const char *A = Alphabet();
            for (int i = 0; i < 64; ++i)
                table[static_cast<unsigned char>(A[i])] = static_cast<signed char>(i);
            built = true;
        }
        return table;
    }

    // Returns false only on a truncated quartet — i.e. the body was cut short in
    // transit. Padding is optional; trailing '=' simply ends the group.
    inline bool Decode(const wchar_t *text, size_t n, std::vector<unsigned char> &out) {
        const signed char *R = ReverseTable();
        out.clear();
        out.reserve((n / 4) * 3);

        unsigned acc  = 0;
        int      bits = 0;
        for (size_t i = 0; i < n; ++i) {
            const wchar_t c = text[i];
            if (c == L'=') break;                 // padding: the payload ends here
            if (c > 127) continue;                // not alphabet — skip
            const signed char d = R[static_cast<unsigned char>(c)];
            if (d < 0) continue;                  // whitespace, CR, LF — skip

            acc = (acc << 6) | static_cast<unsigned>(d);
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                out.push_back(static_cast<unsigned char>((acc >> bits) & 0xFF));
            }
        }
        // bits < 6 is the normal remainder of a padded group. 6 leftover bits
        // means a quartet lost a character on the way here.
        return bits < 6;
    }

} // namespace Common::Base64
