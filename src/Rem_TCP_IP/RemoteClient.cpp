// winsock2.h before anything that pulls windows.h — see the note in
// RemoteServer.cpp. RemoteClient.h includes windows.h.
#include <winsock2.h>
#include <ws2tcpip.h>

#include "RemoteClient.h"
#include "RemoteProtocol.h"
#include "RemoteCrypto.h"

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

    bool SendAll(SOCKET s, const std::string &bytes) {
        size_t sent = 0;
        while (sent < bytes.size()) {
            const int n = send(s, bytes.data() + sent,
                               static_cast<int>(bytes.size() - sent), 0);
            if (n <= 0) return false;
            sent += static_cast<size_t>(n);
        }
        return true;
    }

    bool RecvLine(SOCKET s, std::string &accum, std::wstring &lineOut) {
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

            char buf[1024];
            const int n = recv(s, buf, sizeof(buf), 0);
            if (n <= 0) return false; // closed, or SO_RCVTIMEO expired
            accum.append(buf, static_cast<size_t>(n));
        }
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
}

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
}

bool Client::Connect(const std::wstring &host, int port,
                     const std::wstring &password, std::wstring &errorOut) {
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
    for (addrinfo *ai = res; ai && !connected; ai = ai->ai_next) {
        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        if (ConnectWithTimeout(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen),
                               CONNECT_TIMEOUT_MS)) {
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

    m_sock = static_cast<UINT_PTR>(s);
    m_accum.clear();

    // Banner first — the server always sends one before anything else.
    std::wstring line;
    if (!RecvLine(s, m_accum, line)) {
        Disconnect();
        errorOut = Constants::Messages::REMOTE_CLIENT_NO_BANNER;
        return false;
    }
    m_banner = line;

    // Then either an AUTH challenge, or nothing more until we speak.
    // Peek only if the server actually challenged: a passwordless server sends
    // no AUTH line at all, so blocking for one would stall until IO_TIMEOUT.
    // The banner tells us nothing about that, so the challenge is detected by
    // trying a read that is allowed to time out — the reply to our first real
    // command would otherwise be mistaken for a challenge.
    //
    // Simpler and deterministic: the server sends AUTH immediately after the
    // banner when it wants one, so a short read decides it.
    {
        DWORD shortTimeout = 750;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char *>(&shortTimeout), sizeof(shortTimeout));
        std::wstring maybeAuth;
        const bool got = RecvLine(s, m_accum, maybeAuth);
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char *>(&io), sizeof(io));

        if (got) {
            if (_wcsnicmp(maybeAuth.c_str(), L"AUTH ", 5) != 0) {
                Disconnect();
                errorOut = Constants::Messages::REMOTE_CLIENT_PROTOCOL_ERROR;
                return false;
            }

            // "AUTH <salt-hex> <nonce-hex>"
            const std::wstring rest = maybeAuth.substr(5);
            const size_t sp = rest.find(L' ');
            if (sp == std::wstring::npos) {
                Disconnect();
                errorOut = Constants::Messages::REMOTE_CLIENT_PROTOCOL_ERROR;
                return false;
            }

            const std::vector<BYTE> salt  = Crypto::FromHex(rest.substr(0, sp));
            const std::vector<BYTE> nonce = Crypto::FromHex(rest.substr(sp + 1));
            if (salt.empty() || nonce.empty()) {
                Disconnect();
                errorOut = Constants::Messages::REMOTE_CLIENT_PROTOCOL_ERROR;
                return false;
            }
            if (password.empty()) {
                Disconnect();
                errorOut = Constants::Messages::REMOTE_CLIENT_PASSWORD_REQUIRED;
                return false;
            }

            // Recompute the same digest the server stores, then HMAC the nonce.
            // The password itself never goes on the wire.
            const std::vector<BYTE> secret = Crypto::SecretFromPassword(password, salt);
            const std::vector<BYTE> answer =
                Crypto::HmacSha256(secret, nonce.data(), nonce.size());
            if (answer.empty()) {
                Disconnect();
                errorOut = Constants::Messages::REMOTE_CLIENT_PROTOCOL_ERROR;
                return false;
            }

            if (!SendAll(s, ToUtf8(L"AUTH " + Crypto::ToHex(answer)) + "\r\n")) {
                Disconnect();
                errorOut = Constants::Messages::REMOTE_CLIENT_CONNECT_FAILED;
                return false;
            }

            // The server stays silent on success and sends ERR on failure, so a
            // short read that times out means we are in.
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
                       reinterpret_cast<const char *>(&shortTimeout), sizeof(shortTimeout));
            std::wstring authReply;
            const bool rejected = RecvLine(s, m_accum, authReply);
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
                       reinterpret_cast<const char *>(&io), sizeof(io));

            if (rejected && _wcsnicmp(authReply.c_str(), RT::RESP_ERR, 3) == 0) {
                Disconnect();
                errorOut = Constants::Messages::REMOTE_CLIENT_AUTH_FAILED;
                return false;
            }
        }
    }

    m_connected = true;
    errorOut.clear();
    return true;
}

bool Client::Send(const std::wstring &commandLine,
                  std::wstring &replyOut, std::wstring &errorOut,
                  std::vector<std::wstring> *eventsOut) {
    if (!m_connected) {
        errorOut = Constants::Messages::REMOTE_CLIENT_NOT_CONNECTED;
        return false;
    }
    const SOCKET s = static_cast<SOCKET>(m_sock);

    if (!SendAll(s, ToUtf8(commandLine) + "\r\n")) {
        Disconnect();
        errorOut = Constants::Messages::REMOTE_CLIENT_SEND_FAILED;
        return false;
    }

    std::wstring line;
    if (!RecvLine(s, m_accum, line)) {
        Disconnect();
        errorOut = Constants::Messages::REMOTE_CLIENT_NO_REPLY;
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
        } else {
            replyOut += line + L"\r\n";
        }
        if (!RecvLine(s, m_accum, line)) {
            Disconnect();
            errorOut = Constants::Messages::REMOTE_CLIENT_NO_REPLY;
            return false;
        }
    }

    replyOut += line;
    errorOut.clear();
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
            return true;
        }
    }

    const DWORD shortTo = static_cast<DWORD>(timeoutMs);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char *>(&shortTo), sizeof(shortTo));

    const bool got = RecvLine(s, m_accum, lineOut);

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
    if (!c.Connect(host, port, password, errorOut)) return false;

    std::wstring reply;
    if (!c.Send(L"ping", reply, errorOut)) return false;

    infoOut = c.Banner();
    c.Disconnect();
    return true;
}

} // namespace Remote
