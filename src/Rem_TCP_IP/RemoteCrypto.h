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
//   • AT REST — the .ini stores PBKDF2-HMAC-SHA256(password, salt, iterations),
//     never the password. A single SHA-256 was cheap enough that a leaked .ini
//     gave up any human-chosen password in minutes on a GPU; the derivation is
//     deliberately slow so that one guess costs an attacker what it costs us.
//   • ON THE WIRE — authentication is challenge-response. The server sends a
//     random nonce; the client returns HMAC-SHA256(secret, nonce). The password
//     itself never crosses the network, and a captured response cannot be
//     replayed against a different nonce.
//
// WHAT THIS DOES NOT DO, stated because it is easy to assume otherwise: the
// stored digest IS the shared secret, so anyone who can read the .ini can
// authenticate without ever knowing the password. PBKDF2 raises the cost of
// RECOVERING the password from that file; it does not make the file safe to
// hand out. Protect the .ini accordingly.
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

    // PBKDF2-HMAC-SHA256 → 32 bytes. Empty on failure or on a non-positive
    // iteration count. Exposed because the CLIENT needs exactly this arithmetic
    // and must not reimplement it.
    std::vector<BYTE> Pbkdf2Sha256(const std::wstring &plaintext,
                                   const std::vector<BYTE> &salt,
                                   int iterations);

    // --- Stored-password format -------------------------------------------------
    // The .ini value is "<iterations>$<salt-hex>$<digest-hex>".
    //
    // The salt is per-password and random, so two instances configured with the
    // same password do not produce the same stored string and a precomputed
    // table is useless against it. The ITERATION COUNT is stored beside it so
    // the cost can be raised in a later build without invalidating passwords
    // already set — a value carries the parameters it was made with.

    // Hashes a plaintext password with a fresh random salt at the current
    // iteration count, returning the full stored form. Empty on failure. The
    // plaintext is not retained anywhere.
    std::wstring HashPassword(const std::wstring &plaintext);

    // Constant-time-ish verification of a plaintext against a stored value.
    // Returns false for malformed stored values rather than throwing.
    bool VerifyPassword(const std::wstring &plaintext, const std::wstring &stored);

    // True when a stored value parses into all three fields with sane contents.
    // The listener checks this BEFORE binding: a password it cannot parse is a
    // password it cannot check, and starting anyway would be either a lockout
    // presented as a bug or — worse, if the empty case were ever conflated with
    // it — an unauthenticated socket.
    bool StoredIsUsable(const std::wstring &stored);

    // --- Challenge-response secret ---------------------------------------------
    // Both ends must arrive at the SAME secret by different routes:
    //
    //   server — has the stored "salt$digest" and no plaintext  → SecretFromStored
    //   client — has the plaintext and no stored value          → SecretFromPassword
    //
    // The shared secret is the digest itself. That is why the server's AUTH
    // challenge must carry the SALT and the ITERATION COUNT as well as the
    // nonce: without both, a client holding the correct password still could not
    // derive the digest, and no exchange would be possible. Neither is secret —
    // their job is to make each stored value unique and expensive, not hidden.

    // Server side: pull the digest field out of a stored value.
    std::vector<BYTE> SecretFromStored(const std::wstring &stored);

    // Server side: pull the salt field out, to send in the challenge.
    std::vector<BYTE> SaltFromStored(const std::wstring &stored);

    // Server side: pull the iteration count out, to send in the challenge.
    // 0 for a malformed stored value.
    int IterationsFromStored(const std::wstring &stored);

    // Client side: recompute the same digest from the plaintext and the salt and
    // iteration count the server just sent. Matches HashPassword exactly.
    std::vector<BYTE> SecretFromPassword(const std::wstring &plaintext,
                                         const std::vector<BYTE> &salt,
                                         int iterations);

} // namespace Remote::Crypto
