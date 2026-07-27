#pragma once
#include <windows.h>
#include <string>
#include <vector>

// =============================================================================
// RemoteCrypto — hashing for the remote-control password.
//
// WHY THIS EXISTS AT ALL: the remote password is written into a plain text file
// sitting next to the exe, and travels over an unencrypted socket. Both are
// solved without TLS, but only if the password is never handled in plaintext
// anywhere durable:
//
//   • AT REST — the .ini stores SHA-256(salt + password), never the password.
//   • ON THE WIRE — authentication is challenge-response. The server sends a
//     random nonce; the client returns HMAC-SHA256(secret, nonce). The password
//     itself never crosses the network, and a captured response cannot be
//     replayed against a different nonce.
//
// Everything here is built on Windows' own BCrypt (CNG). No third-party crypto,
// no hand-rolled primitives.
//
// Full design record: docs/REMOTE_TCP_IP_SPEC.md
// =============================================================================

namespace Remote::Crypto {

    // Raw SHA-256 of arbitrary bytes. Returns 32 bytes, or empty on failure —
    // every BCrypt call is checked, and a failed hash must never be treated as
    // a successful one by a caller comparing against a stored value.
    std::vector<BYTE> Sha256(const void *data, size_t len);

    // HMAC-SHA256(key, message). Returns 32 bytes, or empty on failure.
    // Used for the connect-time challenge-response.
    std::vector<BYTE> HmacSha256(const std::vector<BYTE> &key,
                                 const void *msg, size_t msgLen);

    // Lower-case hex of a byte buffer — the form stored in the .ini and sent on
    // the wire, so both are inspectable in a text editor and a packet log.
    std::wstring ToHex(const std::vector<BYTE> &bytes);

    // Parses lower/upper-case hex back to bytes. Empty on malformed input.
    std::vector<BYTE> FromHex(const std::wstring &hex);

    // Cryptographically random bytes from BCryptGenRandom. Empty on failure —
    // callers must abort rather than fall back to a weak source, because a
    // predictable nonce defeats the replay protection entirely.
    std::vector<BYTE> RandomBytes(size_t count);

    // --- Stored-password format -------------------------------------------------
    // The .ini value is "<salt-hex>$<hash-hex>". The salt is per-password and
    // random, so two instances configured with the same password do not produce
    // the same stored string, and a precomputed table is useless against it.

    // Hashes a plaintext password with a fresh random salt, returning the full
    // stored form. Empty on failure. The plaintext is not retained anywhere.
    std::wstring HashPassword(const std::wstring &plaintext);

    // Constant-time-ish verification of a plaintext against a stored value.
    // Returns false for malformed stored values rather than throwing.
    bool VerifyPassword(const std::wstring &plaintext, const std::wstring &stored);

    // --- Challenge-response secret ---------------------------------------------
    // Both ends must arrive at the SAME secret by different routes:
    //
    //   server — has the stored "salt$digest" and no plaintext  → SecretFromStored
    //   client — has the plaintext and no stored value          → SecretFromPassword
    //
    // The shared secret is the digest itself, SHA-256(salt + password). That is
    // why the server's AUTH challenge must carry the SALT as well as the nonce:
    // without it a client holding the correct password still could not derive
    // the digest, and no exchange would be possible. The salt is not secret —
    // its job is to make each stored value unique, not to stay hidden.

    // Server side: pull the digest half out of a stored "salt$digest" value.
    std::vector<BYTE> SecretFromStored(const std::wstring &stored);

    // Server side: pull the salt half out, to send in the challenge.
    std::vector<BYTE> SaltFromStored(const std::wstring &stored);

    // Client side: recompute the same digest from the plaintext and the salt the
    // server just sent. Matches HashPassword's arithmetic exactly.
    std::vector<BYTE> SecretFromPassword(const std::wstring &plaintext,
                                         const std::vector<BYTE> &salt);

} // namespace Remote::Crypto
