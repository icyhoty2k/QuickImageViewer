// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#pragma once
#include <windows.h>
#include <memory>
#include <string>
#include <vector>

// AgentInfo is held BY VALUE below, so the definition has to be here rather than
// forward declared. Safe to include: RemoteProtocol.h pulls windows.h and
// Input/Command.h and no winsock header, so it cannot disturb the winsock2.h
// ordering the .cpp files in this folder depend on.
#include "RemoteProtocol.h"

// =============================================================================
// RemoteClient — the "connect to another instance" half.
//
// One qIV driving another. The panel fills in a target address, port and
// password, and this opens the conversation the server side already speaks.
//
// -----------------------------------------------------------------------------
// BLOCKING, BUT ALWAYS BOUNDED — and never on the UI thread.
//
// Every call here can block for up to its timeout: connecting to a host that is
// switched off takes as long as the TCP stack wants unless something stops it.
// So every wait is explicitly capped:
//
//   • connect() is issued on a NON-BLOCKING socket and waited on with select(),
//     so an unreachable host fails in CONNECT_TIMEOUT_MS instead of ~20 seconds
//   • SO_RCVTIMEO/SO_SNDTIMEO bound every later read and write
//
// Even so, the caller must not be the UI thread — a bounded stall is still a
// stall, and freezing the viewer for several seconds because a wall screen is
// unplugged would be unacceptable. Slice 6's panel runs these on a short-lived
// worker and posts the result back.
// -----------------------------------------------------------------------------
//
// Full design record: docs/REMOTE_TCP_IP_SPEC.md
// =============================================================================

namespace Remote {

    // Forward-declared so this header stays free of winsock2.h — RemoteTls.h
    // pulls it in, and this header exists partly to keep that ordering trap out
    // of every consumer. Held by unique_ptr; ~Client is already out-of-line,
    // which is what makes an incomplete type legal here.
    namespace Tls { class Session; }

    class Client {
    public:
        // BOTH out-of-line, and the constructor is not an oversight: a defaulted
        // inline constructor still instantiates ~unique_ptr<Tls::Session> for
        // unwinding, and Session is incomplete here on purpose.
        Client();
        ~Client();
        Client(const Client &)            = delete;
        Client &operator=(const Client &) = delete;

        // Opens the connection, reads the server banner and completes the
        // challenge-response if the peer asks for one. Returns false with a
        // human-readable reason in `errorOut`.
        //
        // `password` is the PLAINTEXT the user typed. It is used to answer the
        // challenge and is not retained after Connect returns.
        bool Connect(const std::wstring &host, int port,
                     const std::wstring &password, std::wstring &errorOut);

        // Same handshake, but with the shared secret already in hand instead of
        // a password to derive it from.
        //
        // WHY THIS EXISTS: the digest IS the secret. A server stores
        // "salt$digest" and never holds the plaintext; a client normally
        // recomputes that digest from the password and the salt the server
        // sends. Anything that can read the server's own .ini therefore already
        // has everything needed to authenticate — which is what makes importing
        // a local instance's settings file a complete setup rather than a
        // partial one, with no password to type.
        //
        // `storedSecret` and `storedSalt` are the two halves of that value, hex
        // as they appear in the file. The salt is checked against the one the
        // server offers: a mismatch means its password has been changed since
        // the import, which is worth saying plainly rather than letting the
        // exchange fail as a generic auth error.
        bool ConnectWithSecret(const std::wstring &host, int port,
                               const std::wstring &storedSecret,
                               const std::wstring &storedSalt,
                               std::wstring &errorOut);

        // Sends one command line and returns the peer's reply verbatim
        // (including its "OK"/"ERR" prefix, which the caller may want to show).
        //
        // EVENT lines arriving mid-exchange are NOT part of the reply. An
        // observed peer pushes them whenever it feels like it, including between
        // our request and its answer, so they are pulled out here and appended
        // to `eventsOut` instead of being folded into `replyOut` — which is what
        // the multi-line `help` accumulation would otherwise do to them.
        bool Send(const std::wstring &commandLine,
                  std::wstring &replyOut, std::wstring &errorOut,
                  std::vector<std::wstring> *eventsOut = nullptr);

        // Bounded read for an unsolicited line while no request is outstanding.
        // Returns true when one arrived, false on timeout (not an error) or on
        // a dead connection — check IsConnected() to tell those apart.
        //
        // Needed because an observed peer talks when IT acts, not when we ask.
        // Without this the socket would only be read during a Send, and a slave
        // whose slideshow advanced while we were idle would go unheard until we
        // happened to send something.
        bool PollLine(std::wstring &lineOut, int timeoutMs);

        void Disconnect();
        bool IsConnected() const;

        // The banner the server sent on connect — app version, protocol version
        // and the instance Name when it has one.
        const std::wstring &Banner() const { return m_banner; }

        // The far end's PROTOCOL version, parsed out of that banner ("… remote v2
        // [Name]"), or 0 when it did not say. What a caller checks before using
        // anything the older protocol had no idea about — chiefly the image stream,
        // whose chunk lines a v1 peer cannot buffer and answers by dropping the
        // connection. Refusing on the number is the difference between a clear
        // message and a link that dies mid-transfer.
        int PeerProtocol() const { return m_peerProtocol; }

        // What to call the far end in the Ctrl+F12 log.
        //
        // THE LOG IS WRITTEN IN HERE, at the wire boundary — inside Send,
        // PollLine and the handshake — rather than by the callers. It was done
        // by callers first and that was wrong: every path that sent a line by
        // some other route was silently absent from the log, and a log with
        // invisible gaps is worse than no log, because it is believed. The
        // observe re-arm, the handshake, the standalone ping and the whole
        // inbound EVENT stream from a watched instance were all missing.
        //
        // A Client knows only a socket, so the readable name has to be handed to
        // it. Set once, right after construction; falls back to host:port.
        void SetPeerLabel(const std::wstring &label) { m_peerLabel = label; }

        // The certificate fingerprint this target must present, lower-case hex.
        //
        // Set BEFORE Connect. Only consulted when the dialled address requires
        // TLS — a loopback target has no certificate and needs no pin.
        //
        // EMPTY MEANS REFUSE, not "trust anything". A connection with no pin
        // fails and reports what the server offered, so the first connection —
        // the only one an attacker has to be present for — is never the
        // unprotected one.
        void SetPin(const std::wstring &sha256Hex) { m_pin = sha256Hex; }

        // What the last attempt actually saw, whether it matched or not. How the
        // panel shows a fingerprint to accept, and how a mismatch can be
        // compared against the value on the server's own F9 panel.
        const std::wstring &LastFingerprint() const { return m_lastFingerprint; }

    private:
        // Both public entry points land here. `password` is used when
        // `presetSecret` is empty, and ignored when it is not — the two are the
        // two routes to the same shared secret, never both at once.
        bool DoConnect(const std::wstring &host, int port,
                       const std::wstring &password,
                       const std::vector<BYTE> &presetSecret,
                       const std::vector<BYTE> &presetSalt,
                       std::wstring &errorOut);

        // The handshake itself. DoConnect wraps it to time and log every exit —
        // there are eight, which is why the record is not written inline.
        bool DoConnectBody(const std::wstring &host, int port,
                           const std::wstring &password,
                           const std::vector<BYTE> &presetSecret,
                           const std::vector<BYTE> &presetSalt,
                           std::wstring &errorOut);

        // UINT_PTR rather than SOCKET so this header does not have to drag
        // winsock2.h into everything that includes it — and winsock2.h must
        // precede windows.h, which would put an ordering trap in every consumer.
        // How the far end is named in the log. Written once by the owner; read
        // on the sender thread only, which is the same thread that does the I/O.
        std::wstring m_peerLabel;

        // Non-null only while a TLS connection is up. Every read and write goes
        // through it when present; the socket carries plain bytes when not.
        std::unique_ptr<Tls::Session> m_tls;
        std::wstring m_pin;             // expected fingerprint, from the caller
        std::wstring m_lastFingerprint; // what the peer actually presented

        UINT_PTR     m_sock = static_cast<UINT_PTR>(~0ull); // INVALID_SOCKET
        std::string  m_accum;                                // partial line buffer
        std::wstring m_banner;

        // What the SERVER said about itself, from its reply to our `agent` line.
        // Empty against a server that does not answer it. Display only — same
        // rule as every other peer-supplied field: it labels a row, it never
        // decides anything.
        AgentInfo    m_peerAgent;
        int          m_peerProtocol = 0;   // 0 = the banner did not say
        bool         m_connected = false;
    };

    // One-shot reachability probe for the panel's "Check Connection" button:
    // connect, authenticate, `ping`, disconnect. Leaves nothing open.
    // On success `infoOut` carries the peer's banner.
    bool Probe(const std::wstring &host, int port, const std::wstring &password,
               std::wstring &infoOut, std::wstring &errorOut);

} // namespace Remote
