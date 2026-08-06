// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

// winsock2.h before anything that pulls windows.h — see the note in
// RemoteServer.cpp. RemoteClient.h includes windows.h.
#include <winsock2.h>
#include <ws2tcpip.h>

#include "RemoteClient.h"
#include "RemoteProtocol.h"
#include "RemoteCrypto.h"
#include "RemoteSettings.h"   // Config().name — what this instance calls itself
#include "RemoteTls.h"
#include "RemoteLog.h"   // Ctrl+F12 — recorded HERE, at the wire boundary

#include "Platform/Constants.h"
#include "Platform/ConstantsStrings.h"

#include <vector>

namespace Remote {

namespace RT = Constants::RemoteTcpIp;

namespace {
    // An unreachable host must fail in a few seconds, not in however long the
    // TCP stack takes to give up (often ~20s on Windows).
    constexpr int CONNECT_TIMEOUT_MS = 4000;
    // Bounds every read and write once connected, so a peer that accepts and
    // then stops talking cannot pin the calling thread indefinitely.
    constexpr int IO_TIMEOUT_MS      = 5000;

    // Winsock is refcounted per process. The server may or may not have already
    // started it, so the client keeps its own balanced reference rather than
    // assuming either way.
    struct WsaRef {
        bool up = false;
        bool Acquire() {
            if (up) return true;
            WSADATA d{};
            up = (WSAStartup(MAKEWORD(2, 2), &d) == 0);
            return up;
        }
        ~WsaRef() { if (up) WSACleanup(); }
    };

    // --- Ctrl+F12 log helpers -------------------------------------------------
    // The record lives at the WIRE BOUNDARY (see RemoteClient.h) so no send or
    // receive can bypass it. All three short-circuit before building anything
    // when recording is off.
    long long LogNowUs() {
        if (!Log::IsCapturing()) return 0;
        LARGE_INTEGER f, c;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&c);
        return f.QuadPart ? (c.QuadPart * 1000000LL) / f.QuadPart : 0;
    }

    // A Client knows a socket; the owner supplies the readable name. Falls back
    // rather than leaving the column blank, which would read as a bug.
    std::wstring LogPeer(const std::wstring &label) {
        return label.empty() ? std::wstring(L"(peer)") : label;
    }

    // One unsolicited line from the far end. No delta: nothing was asked, so
    // there is no interval to report and a number would be invented.
    void LogInbound(const std::wstring &peer, const std::wstring &line) {
        if (!Log::IsCapturing() || line.empty()) return;
        Log::Add(Log::Direction::In, LogPeer(peer), line,
                 Log::SelfLabel(), L"(unsolicited)", -1);
    }

    // `tls` is null for a loopback target, non-null otherwise. Same shape as
    // the server side, and for the same reason: one branch per direction rather
    // than a wrapper around SOCKET that every call site would have to adopt.
    bool SendAll(SOCKET s, const std::string &bytes, Tls::Session *tls = nullptr) {
        if (tls) return tls->Send(s, bytes.data(), bytes.size());

        size_t sent = 0;
        while (sent < bytes.size()) {
            const int n = send(s, bytes.data() + sent,
                               static_cast<int>(bytes.size() - sent), 0);
            if (n <= 0) return false;
            sent += static_cast<size_t>(n);
        }
        return true;
    }

    bool RecvLine(SOCKET s, std::string &accum, std::wstring &lineOut,
                  Tls::Session *tls = nullptr) {
        for (;;) {
            const size_t nl = accum.find('\n');
            if (nl != std::string::npos) {
                std::string raw = accum.substr(0, nl);
                accum.erase(0, nl + 1);
                if (!raw.empty() && raw.back() == '\r') raw.pop_back();
                lineOut = FromUtf8(raw.data(), raw.size());
                return true;
            }
            if (accum.size() > RT::MAX_LINE_LEN) return false;

            // The session buffers records; `accum` buffers lines on top. A
            // record boundary and a line boundary have nothing to do with each
            // other, so both layers are needed.
            if (tls) {
                if (!tls->Recv(s, accum)) return false;
                continue;
            }

            char buf[1024];
            const int n = recv(s, buf, sizeof(buf), 0);
            if (n <= 0) return false; // closed, or SO_RCVTIMEO expired
            accum.append(buf, static_cast<size_t>(n));
        }
    }

    // What this instance calls itself to a server, for that server's log.
    //
    // The F9 name when there is one - it is what the user already calls this
    // machine everywhere else in the remote UI, so it needs no explaining at the
    // other end. The COMPUTER NAME when there is not, and that fallback is the
    // point rather than a nicety: an instance that only DRIVES others never
    // needed an F9 name, has no listener of its own, and is therefore exactly
    // the case where the far end has nothing but an address to go on.
    //
    // Never empty, so the announcement is never skipped for want of a label.
    std::wstring SelfAnnounceName() {
        if (const std::wstring configured = Config().name; !configured.empty())
            return configured;

        wchar_t host[MAX_COMPUTERNAME_LENGTH + 1] = {};
        DWORD   n = MAX_COMPUTERNAME_LENGTH + 1;
        if (GetComputerNameW(host, &n) && host[0]) return host;
        return L"qIV";
    }

    // Non-blocking connect with an explicit deadline. This is the whole reason
    // the client does not simply call connect() and hope.
    bool ConnectWithTimeout(SOCKET s, const sockaddr *addr, int addrLen, int timeoutMs) {
        u_long nonBlocking = 1;
        if (ioctlsocket(s, FIONBIO, &nonBlocking) != 0) return false;

        bool ok = false;
        if (connect(s, addr, addrLen) == 0) {
            ok = true; // connected immediately (loopback usually does)
        } else if (WSAGetLastError() == WSAEWOULDBLOCK) {
            fd_set wr, ex;
            FD_ZERO(&wr); FD_SET(s, &wr);
            FD_ZERO(&ex); FD_SET(s, &ex);
            timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};

            if (select(0, nullptr, &wr, &ex, &tv) > 0 && FD_ISSET(s, &wr)) {
                // Writable is not sufficient on its own — a refused connection
                // also selects writable. SO_ERROR carries the real outcome.
                int soErr = 0;
                int len = sizeof(soErr);
                if (getsockopt(s, SOL_SOCKET, SO_ERROR,
                               reinterpret_cast<char *>(&soErr), &len) == 0 && soErr == 0)
                    ok = true;
            }
        }

        u_long blocking = 0;
        ioctlsocket(s, FIONBIO, &blocking);
        return ok;
    }

    // Is the address we ACTUALLY CONNECTED TO a loopback one?
    //
    // The rule is the server's, but it has to be applied to the same KIND of
    // thing the server applies it to. The server decides from the peer address
    // it was handed by accept(); this end used to decide from the host STRING
    // the user typed, and the two disagree for any name that resolves to
    // loopback — a hosts-file entry, an mDNS alias, a machine name pointing at
    // 127.0.0.1. "lobby-pc" is exactly the case the resolve above exists to
    // support, so it is not a hypothetical.
    //
    // WHEN THIS IS WRONG IT IS INVISIBLE: one end waits for a ClientHello, the
    // other waits for a banner, and both sit silent until a timeout with
    // nothing on screen to say why. Deciding after the connect costs nothing —
    // the resolved address is already in hand — and removes the disagreement.
    bool AddressIsLoopback(const sockaddr *sa) {
        if (!sa) return false;
        if (sa->sa_family == AF_INET) {
            // The whole 127.0.0.0/8 block, which is what IsLoopbackBind means by
            // its "127." prefix — the range is defined by its first octet.
            const auto *a = reinterpret_cast<const sockaddr_in *>(sa);
            return (ntohl(a->sin_addr.s_addr) >> 24) == 127;
        }
        if (sa->sa_family == AF_INET6) {
            const auto *a = reinterpret_cast<const sockaddr_in6 *>(sa);
            if (IN6_IS_ADDR_LOOPBACK(&a->sin6_addr)) return true;
            // IPv4-mapped ("::ffff:127.0.0.1"), which is what a dual-stack
            // resolve of a loopback name can produce. The server normalises
            // these in PeerAddress before applying its own rule, so this end
            // has to see them the same way or the two ends disagree on
            // precisely the addresses that normalisation exists for.
            if (IN6_IS_ADDR_V4MAPPED(&a->sin6_addr))
                return reinterpret_cast<const BYTE *>(&a->sin6_addr)[12] == 127;
        }
        return false;
    }
}

// Defined here, where Tls::Session is complete. See the header.
Client::Client() = default;

Client::~Client() { Disconnect(); }

bool Client::IsConnected() const { return m_connected; }

void Client::Disconnect() {
    const SOCKET s = static_cast<SOCKET>(m_sock);
    if (s != INVALID_SOCKET) {
        shutdown(s, SD_BOTH);
        closesocket(s);
    }
    m_sock = static_cast<UINT_PTR>(INVALID_SOCKET);
    m_connected = false;
    m_accum.clear();
    // After the socket is closed: the session's close_notify has nowhere to go
    // once the handle is gone, and holding a context for a dead socket is a
    // trap for the next Connect.
    m_tls.reset();
}

bool Client::Connect(const std::wstring &host, int port,
                     const std::wstring &password, std::wstring &errorOut) {
    return DoConnect(host, port, password, {}, {}, errorOut);
}

bool Client::ConnectWithSecret(const std::wstring &host, int port,
                               const std::wstring &storedSecret,
                               const std::wstring &storedSalt,
                               std::wstring &errorOut) {
    const std::vector<BYTE> secret = Crypto::FromHex(storedSecret);
    const std::vector<BYTE> salt   = Crypto::FromHex(storedSalt);
    if (secret.empty() || salt.empty()) {
        errorOut = Constants::Messages::REMOTE_CLIENT_BAD_SECRET;
        return false;
    }
    return DoConnect(host, port, {}, secret, salt, errorOut);
}

// The body. Split out so DoConnect can record EVERY exit — and it has eight,
// which is exactly why a log call before each of them would eventually miss one.
bool Client::DoConnectBody(const std::wstring &host, int port,
                           const std::wstring &password,
                           const std::vector<BYTE> &presetSecret,
                           const std::vector<BYTE> &presetSalt,
                           std::wstring &errorOut) {
    Disconnect();

    if (host.empty()) {
        errorOut = Constants::Messages::REMOTE_CLIENT_NO_HOST;
        return false;
    }
    if (port < RT::PORT_MIN || port > RT::PORT_MAX) {
        errorOut = Constants::Messages::REMOTE_CLIENT_BAD_PORT;
        return false;
    }

    static WsaRef wsa;
    if (!wsa.Acquire()) {
        errorOut = Constants::Messages::REMOTE_WSA_FAILED;
        return false;
    }

    // A host NAME is allowed here, unlike the server's bind address which must
    // be a literal — connecting to "lobby-pc" is a reasonable thing to want.
    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    const std::string node = ToUtf8(host);
    const std::string svc  = ToUtf8(std::to_wstring(port));

    addrinfo *res = nullptr;
    if (getaddrinfo(node.c_str(), svc.c_str(), &hints, &res) != 0 || !res) {
        errorOut = Constants::Messages::REMOTE_CLIENT_RESOLVE_FAILED;
        return false;
    }

    SOCKET s = INVALID_SOCKET;
    bool connected = false;
    // The address the connect actually landed on. Kept because it — not the
    // typed host — decides TLS below, and `ai` does not outlive freeaddrinfo.
    sockaddr_storage connectedTo{};
    for (addrinfo *ai = res; ai && !connected; ai = ai->ai_next) {
        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        if (ConnectWithTimeout(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen),
                               CONNECT_TIMEOUT_MS)) {
            const size_t n = static_cast<size_t>(ai->ai_addrlen);
            CopyMemory(&connectedTo, ai->ai_addr,
                       n < sizeof(connectedTo) ? n : sizeof(connectedTo));
            connected = true;
            break;
        }
        closesocket(s);
        s = INVALID_SOCKET;
    }
    freeaddrinfo(res);

    if (!connected) {
        errorOut = Constants::Messages::REMOTE_CLIENT_CONNECT_FAILED;
        return false;
    }

    const DWORD io = IO_TIMEOUT_MS;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&io), sizeof(io));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&io), sizeof(io));

    // The other half of the NAT problem. This end has IO_TIMEOUT_MS on every
    // read, so a dead connection surfaces here within seconds of anything being
    // SENT — but a mirror sender that has nothing to send sits between two
    // sends for minutes, which is exactly the gap a router closes the mapping
    // in. Keepalive is what turns that into a detected disconnect and a
    // reconnect attempt, rather than a send that fails much later.
    EnableKeepAlive(static_cast<UINT_PTR>(s));

    m_sock = static_cast<UINT_PTR>(s);
    m_accum.clear();
    m_tls.reset();
    m_lastFingerprint.clear();

    // TLS BEFORE THE BANNER — the banner is application data and travels inside
    // the tunnel, exactly as the server sends it.
    //
    // Whether to do this is decided from the RESOLVED ADDRESS THIS SOCKET IS
    // CONNECTED TO — see AddressIsLoopback — by the same rule the server
    // applies to the peer address it accepted. Both ends therefore judge the
    // same value. Nothing on the wire selects it, so the two cannot be talked
    // into disagreeing; they can only be misconfigured, and then the handshake
    // fails loudly instead of quietly falling back to plaintext.
    if (!AddressIsLoopback(reinterpret_cast<const sockaddr *>(&connectedTo))) {
        auto session = std::make_unique<Tls::Session>();
        std::wstring tlsErr;
        if (!session->ConnectHandshake(s, m_pin, m_lastFingerprint, tlsErr)) {
            Disconnect();
            // The reason travels verbatim: "no pin stored" and "certificate
            // mismatch" are different problems with different remedies, and
            // collapsing them into "connection failed" would hide which.
            errorOut = tlsErr;
            return false;
        }
        m_tls = std::move(session);
    }

    Tls::Session *tls = m_tls.get();

    // Banner first — the server always sends one before anything else.
    std::wstring line;
    if (!RecvLine(s, m_accum, line, tls)) {
        Disconnect();
        errorOut = Constants::Messages::REMOTE_CLIENT_NO_BANNER;
        return false;
    }
    m_banner = line;

    // A REFUSAL CAN ARRIVE IN THE BANNER'S PLACE. The accept loop answers a
    // connection over the cap with "ERR 8 …" and closes, before any banner. Read
    // as a banner it leaves m_peerProtocol at 0 and the handshake then fails
    // further down for the wrong reason, reporting a protocol problem for a
    // server that was simply busy.
    if (_wcsnicmp(m_banner.c_str(), RT::RESP_ERR, 3) == 0) {
        Disconnect();
        errorOut = m_banner;
        return false;
    }

    // "OK qIV 2.96.0.113 remote v2 [Name]" → 2. Parsed here, once, because this is
    // the only moment it is known and a caller that had to re-read the banner would
    // re-parse it per send. A banner without the marker leaves 0, which every
    // capability check reads as "too old", the safe way round.
    m_peerProtocol = 0;
    if (const size_t at = m_banner.find(L" remote v"); at != std::wstring::npos) {
        const wchar_t *p = m_banner.c_str() + at + 9;   // past " remote v"
        int v = 0;
        while (*p >= L'0' && *p <= L'9') { v = v * 10 + (*p - L'0'); ++p; }
        m_peerProtocol = v;
    }

    // EXACTLY ONE LINE FOLLOWS THE BANNER, always — protocol v5. "AUTH <iter>
    // <salt> <nonce>" when the server wants a password, a bare "OK" when it does
    // not. So this read BLOCKS on the ordinary IO timeout and the answer is read
    // rather than inferred.
    //
    // It used to be a 750 ms probe whose EXPIRY meant "no password", because a
    // v4 server signalled that by saying nothing. Two ways for that to be wrong,
    // and both were silent: a challenge slower than the probe read as "no
    // password", after which this client entered command mode unauthenticated
    // and every command failed for no stated reason; and every genuinely
    // passwordless connect paid the full 750 ms to learn nothing.
    std::wstring challenge;
    if (!RecvLine(s, m_accum, challenge, tls)) {
        Disconnect();
        errorOut = Constants::Messages::REMOTE_CLIENT_PROTOCOL_ERROR;
        return false;
    }

    // A refusal can land here too — the same "ERR …" the banner check above
    // guards against, one line later.
    if (_wcsnicmp(challenge.c_str(), RT::RESP_ERR, 3) == 0) {
        Disconnect();
        errorOut = challenge;
        return false;
    }

    if (_wcsnicmp(challenge.c_str(), L"AUTH ", 5) != 0) {
        // Not a challenge, so it must be the passwordless "OK". Anything
        // else is a server that did not follow v5, and guessing at it is
        // how the old probe got things wrong.
        if (_wcsnicmp(challenge.c_str(), RT::RESP_OK, 2) != 0) {
            Disconnect();
            errorOut = Constants::Messages::REMOTE_CLIENT_PROTOCOL_ERROR;
            return false;
        }
        // SAY WHO WE ARE, once, before anything else. The server has no way to know
        // otherwise — it sees an address, and an address is not an identity when two
        // instances share a machine or three phones share a router. Its Ctrl+F12 log
        // then names this instance instead of only addressing it.
        //
        // BEST EFFORT: a server that does not know the verb answers ERR and the
        // connection carries on unaffected, because nothing here depends on the
        // reply. The name is a label, not a credential, and this is deliberately
        // sent AFTER authentication — nothing may be decided by a string a peer
        // chooses about itself.
        {
            SendAll(s, ToUtf8(L"hello " + SelfAnnounceName()) + "\r\n", tls);
            std::wstring ack;
            RecvLine(s, m_accum, ack, tls);   // consumed so it cannot be read as a reply
        }

        m_connected = true;
        errorOut.clear();
        return true;
    }

    // "AUTH <iterations> <salt-hex> <nonce-hex>"
    //
    // Three fields since protocol v3. A v2 server sends two and lands in
    // the parse failure below — correct, and deliberately not worked
    // around: its passwords are derived by a method this build refuses
    // to use, so connecting to it anyway would be the one outcome worth
    // preventing.
    const std::wstring rest = challenge.substr(5);
    const size_t sp1 = rest.find(L' ');
    const size_t sp2 = (sp1 == std::wstring::npos)
                           ? std::wstring::npos
                           : rest.find(L' ', sp1 + 1);
    if (sp1 == std::wstring::npos || sp2 == std::wstring::npos) {
        Disconnect();
        errorOut = Constants::Messages::REMOTE_CLIENT_PROTOCOL_ERROR;
        return false;
    }

    const std::wstring iterText = rest.substr(0, sp1);
    const int iterations =
        (iterText.find_first_not_of(L"0123456789") == std::wstring::npos &&
         !iterText.empty())
            ? static_cast<int>(wcstol(iterText.c_str(), nullptr, 10))
            : 0;

    const std::vector<BYTE> salt  = Crypto::FromHex(rest.substr(sp1 + 1, sp2 - sp1 - 1));
    const std::vector<BYTE> nonce = Crypto::FromHex(rest.substr(sp2 + 1));
    if (iterations <= 0 || salt.empty() || nonce.empty()) {
        Disconnect();
        errorOut = Constants::Messages::REMOTE_CLIENT_PROTOCOL_ERROR;
        return false;
    }
    if (password.empty() && presetSecret.empty()) {
        Disconnect();
        errorOut = Constants::Messages::REMOTE_CLIENT_PASSWORD_REQUIRED;
        return false;
    }

    // Two routes to one value. With a password we recompute the digest
    // the server stores; with a secret imported from that server's own
    // settings file we already have it.
    //
    // The imported form checks the SALT first. A different salt means
    // the target's password has been changed since the import, so the
    // stored secret is for a value that no longer exists — worth saying
    // so, because the exchange would otherwise fail as an ordinary
    // authentication error and look like the wrong password was typed.
    std::vector<BYTE> secret;
    if (!presetSecret.empty()) {
        if (presetSalt != salt) {
            Disconnect();
            errorOut = Constants::Messages::REMOTE_CLIENT_SECRET_STALE;
            return false;
        }
        secret = presetSecret;
    } else {
        // The expensive step, and the only one: PBKDF2 at the server's
        // stated work factor. It runs once per connect on this thread —
        // never on the UI thread, which is why DoConnect is where it is.
        secret = Crypto::SecretFromPassword(password, salt, iterations);
    }

    // Whichever route produced it, the password itself never goes on
    // the wire — the answer is an HMAC of the server's own nonce.
    const std::vector<BYTE> answer =
        Crypto::HmacSha256(secret, nonce.data(), nonce.size());
    if (answer.empty()) {
        Disconnect();
        errorOut = Constants::Messages::REMOTE_CLIENT_PROTOCOL_ERROR;
        return false;
    }

    if (!SendAll(s, ToUtf8(L"AUTH " + Crypto::ToHex(answer)) + "\r\n", tls)) {
        Disconnect();
        errorOut = Constants::Messages::REMOTE_CLIENT_CONNECT_FAILED;
        return false;
    }

    // ONE LINE, ALWAYS — "OK" or "ERR". A blocking read, for the same reason as
    // the challenge above: under v4 success was silence, so this was a second
    // 750 ms probe whose expiry was read as "we are in". A rejection slower than
    // the probe was therefore taken for an acceptance, and this client went on
    // to run commands against a server that had already refused it.
    std::wstring authReply;
    if (!RecvLine(s, m_accum, authReply, tls)) {
        Disconnect();
        errorOut = Constants::Messages::REMOTE_CLIENT_CONNECT_FAILED;
        return false;
    }

    if (_wcsnicmp(authReply.c_str(), RT::RESP_ERR, 3) == 0) {
        Disconnect();
        errorOut = Constants::Messages::REMOTE_CLIENT_AUTH_FAILED;
        return false;
    }
    // Neither OK nor ERR is a server that is not speaking v5. Refused rather
    // than assumed — an unread line here is the one that desynchronises every
    // reply that follows.
    if (_wcsnicmp(authReply.c_str(), RT::RESP_OK, 2) != 0) {
        Disconnect();
        errorOut = Constants::Messages::REMOTE_CLIENT_PROTOCOL_ERROR;
        return false;
    }

    // SAY WHO WE ARE, once, before anything else. The server has no way to know
    // otherwise — it sees an address, and an address is not an identity when two
    // instances share a machine or three phones share a router. Its Ctrl+F12 log
    // then names this instance instead of only addressing it.
    //
    // BEST EFFORT: a server that does not know the verb answers ERR and the
    // connection carries on unaffected, because nothing here depends on the
    // reply. The name is a label, not a credential, and this is deliberately
    // sent AFTER authentication — nothing may be decided by a string a peer
    // chooses about itself.
    {
        SendAll(s, ToUtf8(L"hello " + SelfAnnounceName()) + "\r\n", tls);
        std::wstring ack;
        RecvLine(s, m_accum, ack, tls);   // consumed so it cannot be read as a reply
    }

    // WHO AND WHAT is at this end, for the other side's client list. Same
    // contract as hello above: best effort, sent after authentication, reply
    // consumed so it cannot be mistaken for an answer to something else, and
    // nothing here depends on it being understood.
    //
    // The reply is the server's OWN agent line — this is a greeting, not a
    // report — and is parsed so a target can be labelled in Mirroring the
    // same way a client is labelled in My Clients.
    {
        SendAll(s, ToUtf8(L"agent " + BuildAgent(L"qIV", Constants::APP_VERSION,
                                                 SelfAnnounceName())) + "\r\n", tls);
        std::wstring ack;
        if (RecvLine(s, m_accum, ack, tls)) {
            // "OK app=qIV;ver=…" — drop the status word, keep the pairs.
            const size_t sp = ack.find(L' ');
            if (sp != std::wstring::npos)
                m_peerAgent = ParseAgent(ack.substr(sp + 1));
        }
    }

    m_connected = true;
    errorOut.clear();
    return true;
}

bool Client::DoConnect(const std::wstring &host, int port,
                       const std::wstring &password,
                       const std::vector<BYTE> &presetSecret,
                       const std::vector<BYTE> &presetSalt,
                       std::wstring &errorOut) {
    const long long t0 = LogNowUs();
    const bool ok = DoConnectBody(host, port, password, presetSecret, presetSalt, errorOut);

    // The handshake is a wire exchange like any other and belongs in the log:
    // "connect" with the banner as the reply, or with the reason it failed. A
    // target that will not come up is the single most common thing this log is
    // opened to explain, and it used to leave no trace at all.
    //
    // The label may not be set yet on the very first attempt, so the address is
    // used — it is what was dialled, which is the useful thing here anyway.
    if (Log::IsCapturing()) {
        const std::wstring endpoint = FormatEndpoint(host, port);
        const std::wstring peer     = m_peerLabel.empty() ? endpoint : m_peerLabel;
        Log::Add(Log::Direction::Out, Log::SelfLabel(),
                 L"connect " + endpoint,
                 peer, ok ? (m_banner.empty() ? L"OK connected" : m_banner) : errorOut,
                 LogNowUs() - t0);
    }
    return ok;
}

bool Client::Send(const std::wstring &commandLine,
                  std::wstring &replyOut, std::wstring &errorOut,
                  std::vector<std::wstring> *eventsOut) {
    // Every exit from this function is recorded, including the three failures
    // below. Doing it here rather than at the call sites is the whole point:
    // `observe 1`, `ping`, `sync` and the reconnect re-arm all come through
    // here, and only one of them used to be logged.
    const long long t0 = LogNowUs();
    const auto record = [&](const std::wstring &response) {
        if (!Log::IsCapturing()) return;
        Log::Add(Log::Direction::Out, Log::SelfLabel(), commandLine,
                 LogPeer(m_peerLabel), response, LogNowUs() - t0);
    };

    if (!m_connected) {
        errorOut = Constants::Messages::REMOTE_CLIENT_NOT_CONNECTED;
        record(errorOut);
        return false;
    }
    const SOCKET s = static_cast<SOCKET>(m_sock);
    Tls::Session *tls = m_tls.get();

    if (!SendAll(s, ToUtf8(commandLine) + "\r\n", tls)) {
        Disconnect();
        errorOut = Constants::Messages::REMOTE_CLIENT_SEND_FAILED;
        record(errorOut);
        return false;
    }

    std::wstring line;
    if (!RecvLine(s, m_accum, line, tls)) {
        Disconnect();
        errorOut = Constants::Messages::REMOTE_CLIENT_NO_REPLY;
        record(errorOut);
        return false;
    }

    // `help` answers with a listing before its OK, so keep reading until the
    // line that actually starts with OK or ERR — that one is the reply.
    while (_wcsnicmp(line.c_str(), RT::RESP_OK, 2) != 0 &&
           _wcsnicmp(line.c_str(), RT::RESP_ERR, 3) != 0) {
        // An observed peer pushes EVENT lines when IT acts, which can land in
        // the middle of our exchange. They are not part of the answer — folding
        // one into replyOut here would corrupt the reply AND lose the event.
        if (_wcsnicmp(line.c_str(), RT::RESP_EVENT, 5) == 0) {
            if (eventsOut) eventsOut->push_back(line);
            // An EVENT is a line the WATCHED instance sent us of its own accord.
            // Its own entry, in the IN direction — it is not our reply and it
            // did not take us any time, so a delta would be a fiction.
            if (Log::IsCapturing())
                Log::Add(Log::Direction::In, LogPeer(m_peerLabel), line,
                         Log::SelfLabel(), L"(unsolicited)", -1);
        } else {
            replyOut += line + L"\r\n";
        }
        if (!RecvLine(s, m_accum, line, tls)) {
            Disconnect();
            errorOut = Constants::Messages::REMOTE_CLIENT_NO_REPLY;
            record(errorOut);
            return false;
        }
    }

    replyOut += line;
    errorOut.clear();
    record(replyOut);
    return true;
}

bool Client::PollLine(std::wstring &lineOut, int timeoutMs) {
    if (!m_connected) return false;
    const SOCKET s = static_cast<SOCKET>(m_sock);

    // A previous Send may have left a complete line buffered; return it without
    // going near the socket, or a peer that talks in bursts would appear to
    // stall for one timeout per line.
    {
        const size_t nl = m_accum.find('\n');
        if (nl != std::string::npos) {
            std::string raw = m_accum.substr(0, nl);
            m_accum.erase(0, nl + 1);
            if (!raw.empty() && raw.back() == '\r') raw.pop_back();
            lineOut = FromUtf8(raw.data(), raw.size());
            LogInbound(m_peerLabel, lineOut);
            return true;
        }
    }

    const DWORD shortTo = static_cast<DWORD>(timeoutMs);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char *>(&shortTo), sizeof(shortTo));

    const bool got = RecvLine(s, m_accum, lineOut, m_tls.get());
    // THE watched-instance stream. This is the path that carries what a target
    // does on its own — the reason F10's ◉ exists — and it never went through
    // Send, which is why none of it appeared in the log before.
    if (got) LogInbound(m_peerLabel, lineOut);

    // A timeout and a dead peer are indistinguishable at RecvLine (both return
    // false), so the socket must NOT be torn down here: an idle connection is
    // the normal case for a target nobody is driving. The caller learns about a
    // real death from the next Send instead.
    const DWORD restore = IO_TIMEOUT_MS;
    if (m_connected)
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char *>(&restore), sizeof(restore));

    return got;
}

bool Probe(const std::wstring &host, int port, const std::wstring &password,
           std::wstring &infoOut, std::wstring &errorOut) {
    Client c;
    // A short-lived client with no target behind it, so the address IS the name.
    // Labelled anyway — an unlabelled row in the log reads as a bug.
    c.SetPeerLabel(FormatEndpoint(host, port));
    if (!c.Connect(host, port, password, errorOut)) return false;

    std::wstring reply;
    if (!c.Send(L"ping", reply, errorOut)) return false;

    infoOut = c.Banner();
    c.Disconnect();
    return true;
}

} // namespace Remote
