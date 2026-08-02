#include "RemoteCrypto.h"

#include "Platform/Constants.h"   // PBKDF2_ITERATIONS / PBKDF2_SALT_LEN

#include <bcrypt.h>
#include <algorithm>

// bcrypt is linked explicitly in CMakeLists.txt alongside the other Windows
// libraries — deliberately not via a #pragma comment(lib), so every dependency
// this project takes is visible in one place.

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
#endif

namespace Remote::Crypto {

namespace {
    namespace RT = Constants::RemoteTcpIp;

    constexpr size_t SHA256_LEN = 32;
    constexpr wchar_t STORED_SEPARATOR = L'$';

    // RAII for a BCrypt algorithm handle. Every early return below would
    // otherwise leak it — CNG handles are process-global resources, not memory
    // the allocator eventually reclaims.
    struct AlgHandle {
        BCRYPT_ALG_HANDLE h = nullptr;
        ~AlgHandle() {
            if (h) BCryptCloseAlgorithmProvider(h, 0);
        }
        AlgHandle() = default;
        AlgHandle(const AlgHandle &) = delete;
        AlgHandle &operator=(const AlgHandle &) = delete;
    };

    // Shared body for SHA-256 and HMAC-SHA256 — the only difference is whether
    // the provider is opened with the HMAC flag and a key is supplied.
    std::vector<BYTE> HashCore(const void *data, size_t len,
                               const std::vector<BYTE> *key) {
        AlgHandle alg;
        const DWORD flags = key ? BCRYPT_ALG_HANDLE_HMAC_FLAG : 0;
        if (BCryptOpenAlgorithmProvider(&alg.h, BCRYPT_SHA256_ALGORITHM,
                                        nullptr, flags) != STATUS_SUCCESS)
            return {};

        BCRYPT_HASH_HANDLE hash = nullptr;
        PUCHAR keyPtr = nullptr;
        ULONG  keyLen = 0;
        if (key && !key->empty()) {
            keyPtr = const_cast<PUCHAR>(key->data());
            keyLen = static_cast<ULONG>(key->size());
        }

        if (BCryptCreateHash(alg.h, &hash, nullptr, 0, keyPtr, keyLen, 0) != STATUS_SUCCESS)
            return {};

        std::vector<BYTE> out(SHA256_LEN);
        bool ok =
            BCryptHashData(hash, static_cast<PUCHAR>(const_cast<void *>(data)),
                           static_cast<ULONG>(len), 0) == STATUS_SUCCESS &&
            BCryptFinishHash(hash, out.data(),
                             static_cast<ULONG>(out.size()), 0) == STATUS_SUCCESS;

        BCryptDestroyHash(hash);
        if (!ok) return {};
        return out;
    }

    // UTF-8 so the same password produces the same hash regardless of how the
    // caller's wide string happens to be stored, and so non-ASCII passwords are
    // handled byte-identically on both ends of a connection.
    std::vector<BYTE> ToUtf8(const std::wstring &s) {
        if (s.empty()) return {};
        const int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(),
                                          static_cast<int>(s.size()),
                                          nullptr, 0, nullptr, nullptr);
        if (n <= 0) return {};
        std::vector<BYTE> out(static_cast<size_t>(n));
        WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                            reinterpret_cast<LPSTR>(out.data()), n, nullptr, nullptr);
        return out;
    }
}

std::vector<BYTE> Sha256(const void *data, size_t len) {
    return HashCore(data, len, nullptr);
}

std::vector<BYTE> HmacSha256(const std::vector<BYTE> &key,
                             const void *msg, size_t msgLen) {
    if (key.empty()) return {};
    return HashCore(msg, msgLen, &key);
}

std::wstring ToHex(const std::vector<BYTE> &bytes) {
    static const wchar_t *digits = L"0123456789abcdef";
    std::wstring out;
    out.reserve(bytes.size() * 2);
    for (BYTE b : bytes) {
        out += digits[(b >> 4) & 0x0F];
        out += digits[b & 0x0F];
    }
    return out;
}

std::vector<BYTE> FromHex(const std::wstring &hex) {
    if (hex.empty() || (hex.size() % 2) != 0) return {};

    auto nibble = [](wchar_t c) -> int {
        if (c >= L'0' && c <= L'9') return c - L'0';
        if (c >= L'a' && c <= L'f') return c - L'a' + 10;
        if (c >= L'A' && c <= L'F') return c - L'A' + 10;
        return -1;
    };

    std::vector<BYTE> out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        const int hi = nibble(hex[i]);
        const int lo = nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) return {};
        out.push_back(static_cast<BYTE>((hi << 4) | lo));
    }
    return out;
}

std::vector<BYTE> RandomBytes(size_t count) {
    if (count == 0) return {};
    std::vector<BYTE> out(count);
    if (BCryptGenRandom(nullptr, out.data(), static_cast<ULONG>(out.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != STATUS_SUCCESS)
        return {}; // caller must abort — never fall back to a weak source
    return out;
}

std::vector<BYTE> Pbkdf2Sha256(const std::wstring &plaintext,
                               const std::vector<BYTE> &salt,
                               int iterations) {
    if (plaintext.empty() || salt.empty() || iterations <= 0) return {};

    // The PRF handle must be opened WITH the HMAC flag — BCryptDeriveKeyPBKDF2
    // takes a keyed hash provider, and passing a plain SHA-256 handle fails
    // rather than silently computing something else.
    AlgHandle prf;
    if (BCryptOpenAlgorithmProvider(&prf.h, BCRYPT_SHA256_ALGORITHM, nullptr,
                                    BCRYPT_ALG_HANDLE_HMAC_FLAG) != STATUS_SUCCESS)
        return {};

    std::vector<BYTE> pw = ToUtf8(plaintext);
    std::vector<BYTE> out(SHA256_LEN);

    const NTSTATUS st = BCryptDeriveKeyPBKDF2(
        prf.h,
        pw.data(), static_cast<ULONG>(pw.size()),
        const_cast<PUCHAR>(salt.data()), static_cast<ULONG>(salt.size()),
        static_cast<ULONGLONG>(iterations),
        out.data(), static_cast<ULONG>(out.size()),
        0);

    // The password bytes are wiped rather than left in a freed heap block. Not
    // a defence against anything sophisticated — the plaintext is in a wstring
    // the caller still owns — but this copy has no reason to outlive the call.
    SecureZeroMemory(pw.data(), pw.size());

    if (st != STATUS_SUCCESS) return {};
    return out;
}

std::vector<BYTE> SecretFromPassword(const std::wstring &plaintext,
                                     const std::vector<BYTE> &salt,
                                     int iterations) {
    // THE one definition of the arithmetic. HashPassword, VerifyPassword and
    // both clients go through it, so the two ends of a connection cannot drift
    // apart on how the digest is formed.
    return Pbkdf2Sha256(plaintext, salt, iterations);
}

std::wstring HashPassword(const std::wstring &plaintext) {
    if (plaintext.empty()) return {};

    const std::vector<BYTE> salt = RandomBytes(RT::PBKDF2_SALT_LEN);
    if (salt.empty()) return {};

    const std::vector<BYTE> digest =
        SecretFromPassword(plaintext, salt, RT::PBKDF2_ITERATIONS);
    if (digest.empty()) return {};

    // "<iterations>$<salt>$<digest>". The cost parameter is stored WITH the
    // value it produced, so raising PBKDF2_ITERATIONS later leaves every
    // existing password verifiable instead of silently breaking it.
    return std::to_wstring(RT::PBKDF2_ITERATIONS) + STORED_SEPARATOR +
           ToHex(salt) + STORED_SEPARATOR + ToHex(digest);
}

namespace {
    // Splits "<iterations>$<salt>$<digest>". Returns false for anything else —
    // including the two-field form earlier builds wrote, which cannot be
    // upgraded in place because upgrading needs the plaintext and the whole
    // point of the stored form is that it does not have it.
    bool SplitStored(const std::wstring &stored,
                     int &iterOut, std::wstring &saltHexOut, std::wstring &digestHexOut) {
        const size_t a = stored.find(STORED_SEPARATOR);
        if (a == std::wstring::npos) return false;
        const size_t b = stored.find(STORED_SEPARATOR, a + 1);
        if (b == std::wstring::npos) return false;

        const std::wstring iterText = stored.substr(0, a);
        if (iterText.empty() ||
            iterText.find_first_not_of(L"0123456789") != std::wstring::npos)
            return false;

        // wcstol rather than stoi: a value too large to fit must be rejected,
        // not thrown out of a function every caller would have to guard.
        const long v = wcstol(iterText.c_str(), nullptr, 10);
        if (v <= 0 || v > 100000000L) return false;

        iterOut      = static_cast<int>(v);
        saltHexOut   = stored.substr(a + 1, b - a - 1);
        digestHexOut = stored.substr(b + 1);
        return true;
    }
}

bool VerifyPassword(const std::wstring &plaintext, const std::wstring &stored) {
    int iterations = 0;
    std::wstring saltHex, digestHex;
    if (!SplitStored(stored, iterations, saltHex, digestHex)) return false;

    const std::vector<BYTE> salt   = FromHex(saltHex);
    const std::vector<BYTE> expect = FromHex(digestHex);
    if (salt.empty() || expect.size() != SHA256_LEN) return false;

    const std::vector<BYTE> actual = SecretFromPassword(plaintext, salt, iterations);
    if (actual.size() != expect.size()) return false;

    // Compare every byte regardless of where the first mismatch is, so the time
    // taken does not leak how much of the hash matched.
    BYTE diff = 0;
    for (size_t i = 0; i < actual.size(); ++i)
        diff |= static_cast<BYTE>(actual[i] ^ expect[i]);
    return diff == 0;
}

bool StoredIsUsable(const std::wstring &stored) {
    int iterations = 0;
    std::wstring saltHex, digestHex;
    if (!SplitStored(stored, iterations, saltHex, digestHex)) return false;
    return !FromHex(saltHex).empty() && FromHex(digestHex).size() == SHA256_LEN;
}

std::vector<BYTE> SecretFromStored(const std::wstring &stored) {
    // The stored digest IS the shared secret for the challenge-response. The
    // server holds it without ever knowing the plaintext; a client that knows
    // the password derives the same value from the salt and iteration count the
    // server sends in the challenge.
    int iterations = 0;
    std::wstring saltHex, digestHex;
    if (!SplitStored(stored, iterations, saltHex, digestHex)) return {};
    return FromHex(digestHex);
}

std::vector<BYTE> SaltFromStored(const std::wstring &stored) {
    int iterations = 0;
    std::wstring saltHex, digestHex;
    if (!SplitStored(stored, iterations, saltHex, digestHex)) return {};
    return FromHex(saltHex);
}

int IterationsFromStored(const std::wstring &stored) {
    int iterations = 0;
    std::wstring saltHex, digestHex;
    if (!SplitStored(stored, iterations, saltHex, digestHex)) return 0;
    return iterations;
}

} // namespace Remote::Crypto
