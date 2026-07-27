// winsock2.h MUST come before anything that pulls in windows.h. This project
// does not define WIN32_LEAN_AND_MEAN, so windows.h drags in the original
// winsock.h and every socket type is then redefined. RemoteServer.h includes
// windows.h, so these two lines stay pinned at the top of the file.
#include <winsock2.h>
#include <ws2tcpip.h>

#include "RemoteServer.h"
#include "RemoteSettings.h"
#include "RemoteCrypto.h"
#include "RemoteExec.h"

#include "Platform/Constants.h"
#include "Platform/ConstantsStrings.h"
#include "Input/Command.h"

#include <algorithm>
#include <mutex>
#include <thread>
#include <vector>

namespace Remote {

namespace RT = Constants::RemoteTcpIp;

// =============================================================================
// Module state. Everything here is touched by the listener thread, the client
// threads and the UI thread, so every field is either atomic or guarded.
// =============================================================================
namespace {
    std::atomic<bool>   g_running{false};
    std::atomic<bool>   g_stopRequested{false};
    std::atomic<int>    g_activeClients{0};
    std::atomic<SOCKET> g_listenSocket{INVALID_SOCKET};

    std::thread  g_listenThread;
    HWND         g_owner = nullptr;
    std::mutex   g_endpointMutex;
    std::wstring g_endpoint;

    // A snapshot of the configuration taken at Start(). The socket threads read
    // THIS, never Remote::Config() — that lives on the UI thread and the user can
    // edit it in the panel while clients are connected.
    Settings   g_snapshot;
    std::mutex g_snapshotMutex;

    // WSAStartup is refcounted by Winsock, but calling it twice without matching
    // cleanups still leaks a reference. One flag keeps Start/Stop balanced.
    bool g_wsaUp = false;

    // How long a client waits for the UI thread before giving up. Generous: the
    // UI thread may legitimately be mid-decode. Bounded so a wedged UI thread
    // cannot pin a socket thread forever.
    constexpr DWORD REPLY_TIMEOUT_MS = 5000;

    // How long accept() blocks before the loop re-checks the stop flag.
    constexpr long ACCEPT_POLL_US = 250000; // 250 ms

    // Bytes of challenge. 32 is SHA-256's own width; a nonce shorter than the
    // digest would be the weakest link in the exchange.
    constexpr size_t NONCE_LEN = 32;

    void SetEndpoint(const std::wstring &s) {
        std::lock_guard<std::mutex> lk(g_endpointMutex);
        g_endpoint = s;
    }

    Settings SnapshotCopy() {
        std::lock_guard<std::mutex> lk(g_snapshotMutex);
        return g_snapshot;
    }

    // --- Address matching ---------------------------------------------------

    // Literal match, plus a trailing "*" wildcard so "192.168.1.*" covers a
    // subnet without spelling out 254 entries. Deliberately NOT a CIDR parser:
    // hand-edited text files get CIDR subtly wrong, and a rule that silently
    // matches more than the author intended is worse than no rule.
    bool AddressMatches(const std::wstring &pattern, const std::wstring &addr) {
        if (pattern.empty()) return false;
        if (pattern == L"*") return true;

        if (pattern.back() == L'*') {
            const std::wstring prefix = pattern.substr(0, pattern.size() - 1);
            return addr.size() >= prefix.size() &&
                   _wcsnicmp(addr.c_str(), prefix.c_str(), prefix.size()) == 0;
        }
        return _wcsicmp(pattern.c_str(), addr.c_str()) == 0;
    }

    bool InList(const std::vector<std::wstring> &list, const std::wstring &addr) {
        for (const std::wstring &p : list)
            if (AddressMatches(p, addr)) return true;
        return false;
    }

    // --- Socket helpers -----------------------------------------------------

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

    bool SendLine(SOCKET s, const std::wstring &line) {
        return SendAll(s, ToUtf8(line) + "\r\n");
    }

    // Reads one \n-terminated line. Returns false on disconnect, error, or a
    // line that exceeds MAX_LINE_LEN — an unbounded line is the one input a
    // peer fully controls, so the connection is dropped rather than buffered.
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
            if (n <= 0) return false;
            accum.append(buf, static_cast<size_t>(n));
        }
    }

    std::wstring PeerAddress(const sockaddr_storage &ss) {
        wchar_t host[NI_MAXHOST] = {};
        if (GetNameInfoW(reinterpret_cast<const sockaddr *>(&ss),
                         sizeof(sockaddr_storage), host, NI_MAXHOST,
                         nullptr, 0, NI_NUMERICHOST) != 0)
            return {};
        // IPv4-mapped IPv6 ("::ffff:192.168.1.5") is normalised so an AllowList
        // written in plain IPv4 still matches when the socket is dual-stack.
        const std::wstring s = host;
        const size_t mapped = s.rfind(L':');
        if (s.rfind(L"::ffff:", 0) == 0 && mapped != std::wstring::npos)
            return s.substr(mapped + 1);
        return s;
    }

    // --- Authentication -----------------------------------------------------

    // Challenge-response. The password never crosses the wire: the server sends
    // a fresh random nonce, the client returns HMAC(secret, nonce), and the
    // secret is derived from the stored hash so the server never holds plaintext
    // either. A captured response is useless against the next nonce.
    bool Authenticate(SOCKET s, const Settings &cfg, std::string &accum) {
        if (cfg.passwordHash.empty()) return true; // no password configured

        const std::vector<BYTE> secret = Crypto::SecretFromStored(cfg.passwordHash);
        const std::vector<BYTE> salt   = Crypto::SaltFromStored(cfg.passwordHash);
        const std::vector<BYTE> nonce  = Crypto::RandomBytes(NONCE_LEN);
        // A failed RNG must abort the connection. Falling back to anything
        // predictable would silently remove the replay protection.
        if (secret.empty() || salt.empty() || nonce.empty()) {
            SendLine(s, MakeErr(RT::ERR_INTERNAL, L"server cannot generate a challenge"));
            return false;
        }

        // "AUTH <salt> <nonce>". The salt has to travel: the client holds only
        // the plaintext password, and cannot derive the shared digest without it.
        // Sending it costs nothing — a salt's job is uniqueness, not secrecy.
        if (!SendLine(s, L"AUTH " + Crypto::ToHex(salt) + L" " + Crypto::ToHex(nonce)))
            return false;

        std::wstring line;
        if (!RecvLine(s, accum, line)) return false;

        // Expected form: "AUTH <hex>"
        std::wstring got = line;
        if (_wcsnicmp(got.c_str(), L"AUTH ", 5) == 0) got = got.substr(5);
        else {
            SendLine(s, MakeErr(RT::ERR_NOT_AUTHENTICATED, L"expected AUTH <response>"));
            return false;
        }

        const std::vector<BYTE> expect =
            Crypto::HmacSha256(secret, nonce.data(), nonce.size());
        const std::vector<BYTE> actual = Crypto::FromHex(got);

        bool ok = !expect.empty() && actual.size() == expect.size();
        if (ok) {
            BYTE diff = 0;
            for (size_t i = 0; i < expect.size(); ++i)
                diff |= static_cast<BYTE>(expect[i] ^ actual[i]);
            ok = (diff == 0);
        }

        if (!ok) {
            SendLine(s, MakeErr(RT::ERR_AUTH_FAILED, L"authentication failed"));
            return false;
        }
        return true;
    }

    // --- Per-client conversation --------------------------------------------

    // The peer address is deliberately NOT passed in: it is used by the accept
    // gates and has no consumer once a connection is admitted. Re-add it when
    // something actually reports it — a connected-client list, or a log.
    void ClientThread(SOCKET client, Settings cfg, HWND owner) {
        std::string accum;

        SendLine(client, MakeOk(L"qIV " + std::wstring(Constants::APP_VERSION) +
                                L" remote v" + std::to_wstring(RT::PROTOCOL_VERSION) +
                                (cfg.name.empty() ? L"" : L" [" + cfg.name + L"]")));

        if (Authenticate(client, cfg, accum)) {
            for (;;) {
                if (g_stopRequested.load(std::memory_order_acquire)) {
                    SendLine(client, MakeErr(RT::ERR_INTERNAL, L"server shutting down"));
                    break;
                }

                std::wstring line;
                if (!RecvLine(client, accum, line)) break;

                const RemoteRequest req = ParseLine(line);

                if (req.status == ParseStatus::EmptyLine) continue;

                if (req.status == ParseStatus::Verb) {
                    switch (req.verb) {
                        case Verb::Help:
                            SendAll(client, ToUtf8(BuildHelpText()));
                            SendLine(client, MakeOk());
                            break;
                        case Verb::Ping:
                            SendLine(client, MakeOk(L"pong"));
                            break;
                        case Verb::Version:
                            SendLine(client, MakeOk(std::wstring(Constants::APP_VERSION) +
                                                    L" protocol " +
                                                    std::to_wstring(RT::PROTOCOL_VERSION)));
                            break;
                        default:
                            SendLine(client, MakeErr(RT::ERR_INTERNAL, L"unhandled verb"));
                            break;
                    }
                    continue;
                }

                if (req.status != ParseStatus::Ok) {
                    SendLine(client, MakeErrFor(req));
                    continue;
                }

                // Hand the command to the UI thread and wait, bounded. See the
                // ownership note in RemoteServer.h — both sides hold a
                // shared_ptr, so a timeout here cannot corrupt anything.
                auto call = std::make_shared<RemoteCall>();
                call->req = req;

                if (!call->doneEvent) {
                    SendLine(client, MakeErr(RT::ERR_INTERNAL, L"no event"));
                    continue;
                }

                if (!PostMessageW(owner, Constants::WM_QIV_REMOTE_COMMAND, 0,
                                  reinterpret_cast<LPARAM>(
                                      new std::shared_ptr<RemoteCall>(call)))) {
                    SendLine(client, MakeErr(RT::ERR_INTERNAL, L"viewer not accepting commands"));
                    continue;
                }

                const DWORD w = WaitForSingleObject(call->doneEvent, REPLY_TIMEOUT_MS);
                if (w == WAIT_OBJECT_0) {
                    if (!SendLine(client, call->result)) break;
                } else {
                    if (!SendLine(client, MakeErr(RT::ERR_INTERNAL, L"viewer did not respond in time")))
                        break;
                }
            }
        }

        shutdown(client, SD_BOTH);
        closesocket(client);
        g_activeClients.fetch_sub(1, std::memory_order_acq_rel);
    }

    // --- Listener -----------------------------------------------------------

    void ListenThread() {
        const SOCKET listenSock = g_listenSocket.load(std::memory_order_acquire);

        while (!g_stopRequested.load(std::memory_order_acquire)) {
            // select() rather than a blocking accept(), so the stop flag is
            // re-checked several times a second and Stop() does not depend on
            // yanking the socket out from under a blocked call.
            fd_set rd;
            FD_ZERO(&rd);
            FD_SET(listenSock, &rd);
            timeval tv{0, ACCEPT_POLL_US};

            const int sel = select(0, &rd, nullptr, nullptr, &tv);
            if (sel == SOCKET_ERROR) break;
            if (sel == 0) continue; // timed out; loop re-checks the stop flag

            sockaddr_storage ss{};
            int sslen = sizeof(ss);
            const SOCKET client = accept(listenSock,
                                         reinterpret_cast<sockaddr *>(&ss), &sslen);
            if (client == INVALID_SOCKET) continue;

            const std::wstring peer = PeerAddress(ss);
            const Settings cfg = SnapshotCopy();

            // Gates in the order the spec fixes, and the order matters:
            // an address that fails 1 or 2 is told NOTHING, not even that
            // something is listening. Only a peer that has already proved it is
            // permitted here gets a diagnostic.

            // 1. BlackList — deny always overrides allow.
            if (InList(cfg.blackList, peer)) {
                closesocket(client);
                continue;
            }
            // 2. AllowList — empty means deny everyone, by design.
            if (cfg.allowList.empty() || !InList(cfg.allowList, peer)) {
                closesocket(client);
                continue;
            }
            // 3. Connection cap — this peer is trusted, so it is told why.
            if (g_activeClients.load(std::memory_order_acquire) >= cfg.maxConnections) {
                SendLine(client, MakeErr(RT::ERR_TOO_MANY_CLIENTS,
                                         L"connection limit reached"));
                shutdown(client, SD_BOTH);
                closesocket(client);
                continue;
            }

            g_activeClients.fetch_add(1, std::memory_order_acq_rel);
            // Detached: a client's lifetime is its socket's, and Stop() closes
            // the sockets rather than joining every conversation.
            std::thread(ClientThread, client, cfg, g_owner).detach();
        }

        g_running.store(false, std::memory_order_release);
        if (g_owner) PostMessageW(g_owner, Constants::WM_QIV_REMOTE_STOPPED, 0, 0);
    }
}

// =============================================================================
// RemoteCall
// =============================================================================
RemoteCall::RemoteCall() {
    // Manual-reset: the UI thread signals once and the client may wake late.
    doneEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

RemoteCall::~RemoteCall() {
    if (doneEvent) CloseHandle(doneEvent);
}

// =============================================================================
// Start / Stop
// =============================================================================

bool Start(HWND hOwner, std::wstring &errorOut) {
    if (g_running.load(std::memory_order_acquire)) {
        errorOut = Constants::Messages::REMOTE_ALREADY_RUNNING;
        return false;
    }

    Settings cfg = Config();
    Normalize(cfg);

    const std::wstring blocked = WhyCannotStart(cfg);
    // An empty AllowList is a warning, not a refusal: the server binds and
    // listens, it just denies every caller. The panel reports it so a fully
    // functional-looking listener that refuses everything is never a mystery.
    if (!blocked.empty() && blocked != Constants::Messages::REMOTE_WARN_EMPTY_ALLOWLIST) {
        errorOut = blocked;
        return false;
    }

    if (!g_wsaUp) {
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            errorOut = Constants::Messages::REMOTE_WSA_FAILED;
            return false;
        }
        g_wsaUp = true;
    }

    // getaddrinfo rather than inet_addr, so the bind address field accepts an
    // IPv6 literal as readily as an IPv4 one.
    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags    = AI_PASSIVE | AI_NUMERICHOST;

    const std::string node = ToUtf8(cfg.bindAddress);
    const std::string port = ToUtf8(std::to_wstring(cfg.port));

    addrinfo *res = nullptr;
    if (getaddrinfo(node.c_str(), port.c_str(), &hints, &res) != 0 || !res) {
        errorOut = Constants::Messages::REMOTE_BAD_BIND_ADDRESS;
        return false;
    }

    SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == INVALID_SOCKET) {
        freeaddrinfo(res);
        errorOut = Constants::Messages::REMOTE_SOCKET_FAILED;
        return false;
    }

    // Deliberately NOT setting SO_REUSEADDR. On Windows it allows two processes
    // to bind the same port and silently steal each other's connections, which
    // for a control channel would be a security problem rather than a
    // convenience. A port already in use must fail loudly.
    if (bind(s, res->ai_addr, static_cast<int>(res->ai_addrlen)) == SOCKET_ERROR) {
        const int err = WSAGetLastError();
        freeaddrinfo(res);
        closesocket(s);
        errorOut = (err == WSAEADDRINUSE)
                       ? Constants::Messages::REMOTE_PORT_IN_USE
                       : Constants::Messages::REMOTE_BIND_FAILED;
        return false;
    }
    freeaddrinfo(res);

    if (listen(s, SOMAXCONN) == SOCKET_ERROR) {
        closesocket(s);
        errorOut = Constants::Messages::REMOTE_LISTEN_FAILED;
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(g_snapshotMutex);
        g_snapshot = cfg;
    }
    SetEndpoint(cfg.bindAddress + L":" + std::to_wstring(cfg.port));

    g_owner = hOwner;
    g_listenSocket.store(s, std::memory_order_release);
    g_stopRequested.store(false, std::memory_order_release);
    g_running.store(true, std::memory_order_release);
    g_activeClients.store(0, std::memory_order_release);

    g_listenThread = std::thread(ListenThread);
    errorOut.clear();
    return true;
}

void Stop() {
    if (!g_running.load(std::memory_order_acquire) && !g_listenThread.joinable())
        return;

    g_stopRequested.store(true, std::memory_order_release);

    // The listener wakes within ACCEPT_POLL_US and exits on its own, so the
    // socket is closed only after the thread has left the select/accept pair.
    if (g_listenThread.joinable()) g_listenThread.join();

    const SOCKET s = g_listenSocket.exchange(INVALID_SOCKET, std::memory_order_acq_rel);
    if (s != INVALID_SOCKET) closesocket(s);

    // Client threads are detached and end when their sockets die or when they
    // next observe g_stopRequested. Nothing here waits on them: a client parked
    // in recv() must not be able to hold up the UI thread's Stop().

    g_running.store(false, std::memory_order_release);
    SetEndpoint({});

    if (g_wsaUp) {
        WSACleanup();
        g_wsaUp = false;
    }
}

bool IsRunning() { return g_running.load(std::memory_order_acquire); }

int ActiveConnections() { return g_activeClients.load(std::memory_order_acquire); }

std::wstring BoundEndpoint() {
    std::lock_guard<std::mutex> lk(g_endpointMutex);
    return g_endpoint;
}

// =============================================================================
// UI-thread execution
// =============================================================================
std::wstring ExecuteOnUiThread(HWND hWnd, const RemoteRequest &req) {
    // The five payload-carrying commands go to their headless paths in
    // RemoteExec. They must NOT reach ExecuteCommand, where each raises a panel
    // or a dialog and would hold the client's connection open until somebody
    // dismissed a window on the machine.
    std::wstring reply;
    if (ExecutePayloadCommand(hWnd, req, reply))
        return reply;

    // The shared sink. Keyboard, mouse and tray all funnel through here, so a
    // remote command behaves identically to the same command typed locally.
    //
    // The kiosk lock is deliberately NOT consulted: isLocked exists to stop
    // someone at the keyboard of an unattended screen, while a remote caller has
    // already passed the address gates and the password.
    InputManager::ExecuteCommand(hWnd, req.cmd);
    return MakeOk();
}

} // namespace Remote
