// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#pragma once
#include <winsock2.h>
#include <windows.h>

// CtxtHandle is an SSPI type and the Session below holds one by value, so the
// SSPI headers belong here rather than only in the .cpp. SECURITY_WIN32 selects
// the user-mode half of sspi.h — without it the header compiles to nothing
// usable and every member reads as an unknown type.
#ifndef SECURITY_WIN32
#define SECURITY_WIN32
#endif
#include <security.h>

#include <mutex>
#include <string>
#include <vector>

// =============================================================================
// RemoteTls — TLS for the remote-control socket, on Schannel.
//
// Windows' own TLS stack, for the same reason RemoteCrypto is BCrypt: this
// program ships no third-party crypto, and the one thing worse than no TLS is a
// bundled TLS library nobody updates when the next protocol flaw lands.
//
// -----------------------------------------------------------------------------
// WHEN IT IS ON
//
// The server decides PER CONNECTION, from the PEER's address:
//
//     peer on 127.0.0.0/8 or ::1   →  plaintext
//     peer anywhere else           →  TLS, always
//
// THIS IS NOT NEGOTIATION AND HAS NO DOWNGRADE HOLE. Nothing on the wire selects
// it, and a peer address is not something an attacker can choose: TCP requires a
// completed handshake, so a connection claiming to come from 127.0.0.1 cannot
// carry a conversation unless it really is local. A packet that left the machine
// is encrypted; one that never did is not.
//
// PER CONNECTION rather than per listener, because the two cases coexist. A
// server on 0.0.0.0 serves the local multi-screen wall in plaintext — no
// certificates to manage, no handshake per keystroke — while the phone on the
// LAN talking to the same port gets TLS. Deciding from the BIND address forced
// one mode on everybody and made 0.0.0.0 break every loopback client.
//
// The client learns which to speak from the address it DIALS, which is the same
// address the server will see it arrive from — so the two agree by construction.
// The one case needing care is an alias: the Android emulator's 10.0.2.2 reaches
// the host's loopback, so the client counts it as local, exactly as the server
// will when the connection turns up as 127.0.0.1.
//
// -----------------------------------------------------------------------------
// IDENTITY: A SELF-SIGNED CERTIFICATE AND A PINNED FINGERPRINT
//
// There is no CA, and there should not be: this server is reached by IP address
// on somebody's home connection, which no public CA will ever issue for. So the
// certificate is self-signed, generated once, and the client checks it by
// PINNING — comparing the SHA-256 of the certificate to a value the user
// carried across by hand, exactly once, out of band.
//
// That is stronger than the public CA system for this shape of problem, not
// weaker: a pin trusts precisely one key, where a CA chain trusts every
// authority in the store to have never mis-issued.
//
// The fingerprint is shown in the F9 panel. It goes into the phone's server
// profile. If it ever changes without the user regenerating it, the client
// refuses to connect — which is the entire point, and the one case where
// refusing is the helpful behaviour.
//
// -----------------------------------------------------------------------------
// WHERE THE KEY LIVES, AND WHAT THAT COSTS
//
// qivServerCert.pfx, beside the exe, unencrypted — chosen so a portable folder
// carries its own identity and moving the folder to another machine does not
// invalidate every pin.
//
// THE COST IS REAL AND IS NOT MITIGATED BY ANYTHING HERE: anyone who can read
// that file has the private key, and with it can impersonate this server to a
// client that has pinned it. Pinning protects the channel from strangers; it
// cannot protect it from someone who already has the folder. The file is created
// with a DACL granting only the current user, which stops a casual read by
// another account on the same machine and stops nothing else — a copied,
// synced or backed-up folder takes the key with it.
//
// Treat the exe folder as secret, or accept that anyone with a copy of it can
// be this server.
// =============================================================================

namespace Remote::Tls {

    // True when this address is NOT on this machine, and therefore that TLS is
    // mandatory. The single rule both ends apply — the server to each PEER it
    // accepts, the client to the address it dials.
    //
    // Also used with the BIND address in two places, where the question is
    // different but the answer is the same: whether this listener can be reached
    // from off-machine at all, which decides whether a certificate is needed and
    // whether a password is compulsory.
    bool RequiredForAddress(const std::wstring &address);

    // --- Server credentials -------------------------------------------------

    // Loads qivServerCert.pfx, generating a self-signed certificate and writing
    // the file when it is not there. Idempotent: the second call returns the
    // credentials the first one built, so the fingerprint is stable for the life
    // of the process AND across restarts.
    //
    // False on failure, with a reason. A listener that cannot obtain
    // credentials MUST NOT fall back to plaintext — see Start().
    bool EnsureServerCredentials(std::wstring &errorOut);

    // SHA-256 of the server certificate's DER bytes, lower-case hex, no
    // separators. Empty before EnsureServerCredentials has succeeded.
    //
    // THE VALUE A CLIENT PINS. Shown in the F9 panel so it can be read off and
    // typed into a phone.
    std::wstring ServerFingerprint();

    // Releases the credentials AND deletes the key container that importing the
    // PFX created in the user's keyset.
    //
    // The deletion is the point. Schannel's server side cannot use an ephemeral
    // key, so the private key has to be persisted on import — which means one
    // container per launch would accumulate in the profile forever if nobody
    // cleaned up. The PFX beside the exe stays the durable copy; the keyset
    // entry is meant to live exactly as long as the listener.
    //
    // Called from Remote::Stop(). Safe to call when nothing was ever acquired.
    void ShutdownServerCredentials();

    // SHA-256 of the certificate inside a .pfx at `pfxPath`, lower-case hex.
    // Empty when the file is missing or unreadable.
    //
    // For IMPORT: pointing at another instance's folder yields its pin without
    // anyone reading a fingerprint off a screen and typing it back. Reads only
    // the certificate — the private key is never touched, so this works on a
    // file whose key it could not use.
    std::wstring FingerprintOfCertFile(const std::wstring &pfxPath);

    // Deletes the certificate and forgets the credentials, so the next
    // EnsureServerCredentials mints a fresh one. Every existing pin is
    // invalidated — which is the point when a key is believed compromised, and
    // is why this is never automatic.
    bool RegenerateServerCertificate(std::wstring &errorOut);

    // --- A TLS-wrapped connection -------------------------------------------
    //
    // Owns the security context for one socket. The socket itself stays with the
    // caller, which still closes it — this wraps the bytes, not the lifetime.
    //
    // NOT copyable. Handshake and Recv belong to ONE client thread.
    //
    // Send is the exception, and it has to be: EmitToObservers pushes from the
    // UI thread down a connection whose client thread is sitting in Recv. A
    // Schannel context must not be driven from two threads at once, so the
    // session carries a mutex held across EncryptMessage and DecryptMessage —
    // but deliberately NOT across the blocking socket read, which would
    // otherwise let one idle observer freeze every write in the process.
    //
    // TrySend exists for that push specifically: it gives up rather than
    // waiting, preserving the existing rule that an observer is a courtesy and
    // a stalled UI thread is a defect.
    class Session {
    public:
        Session() = default;
        ~Session();
        Session(const Session &)            = delete;
        Session &operator=(const Session &) = delete;

        // Server side. Runs the handshake to completion. False on any failure,
        // including a client that spoke plaintext at an endpoint that requires
        // TLS — that is a refusal, never a fallback.
        bool AcceptHandshake(SOCKET s, std::wstring &errorOut);

        // Client side. `expectedFingerprint` is the pinned SHA-256 hex; the
        // handshake is failed when the server's certificate does not match it.
        //
        // An EMPTY pin means "report what you found and refuse", filling
        // `actualFingerprintOut` — how the user is shown a fingerprint to accept
        // the first time, without ever trusting one silently.
        bool ConnectHandshake(SOCKET s, const std::wstring &expectedFingerprint,
                              std::wstring &actualFingerprintOut,
                              std::wstring &errorOut);

        bool Active() const { return m_active; }

        // Encrypts and writes everything, or fails. Mirrors SendAll.
        bool Send(SOCKET s, const char *data, size_t len);

        // The same, but returns false immediately when another thread holds the
        // session — never waits. For the observer push, which runs on the UI
        // thread and must drop rather than block.
        bool TrySend(SOCKET s, const char *data, size_t len);

        // Reads and decrypts at least one byte, appending to `out`. False on a
        // clean close or an error — the caller cannot tell them apart and does
        // not need to, exactly as with recv() returning 0.
        //
        // TLS is a RECORD protocol, not a byte stream: one recv() may deliver
        // half a record or three of them. The leftover is held inside the
        // session, which is why reads must go through the same object every
        // time and why a stray recv() on the socket corrupts it permanently.
        bool Recv(SOCKET s, std::string &out);

        // Sends the close_notify alert. Best effort — a peer that has already
        // gone gets no complaint.
        void Shutdown(SOCKET s);

    private:
        void Release();

        bool              m_active   = false;
        bool              m_isServer = false;   // which credential Shutdown uses
        CtxtHandle        m_ctx{};
        bool              m_haveCtx = false;
        unsigned long     m_header = 0, m_trailer = 0, m_maxMessage = 0;
        std::string       m_incoming;   // raw bytes read, not yet decrypted
        std::string       m_decrypted;  // decrypted, not yet handed to the caller

        // Guards use of the security context. Never held across a blocking read.
        std::mutex        m_ctxMutex;

        bool SendLocked(SOCKET s, const char *data, size_t len);
    };

} // namespace Remote::Tls
