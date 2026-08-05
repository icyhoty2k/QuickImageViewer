// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

// winsock2.h MUST come before anything that pulls in windows.h. This project
// does not define WIN32_LEAN_AND_MEAN, so windows.h drags in the original
// winsock.h and every socket type is then redefined. RemoteServer.h includes
// windows.h, so these two lines stay pinned at the top of the file.
#include <winsock2.h>
#include <ws2tcpip.h>

#include "RemoteServer.h"
#include "RemoteSettings.h"
#include "RemoteBlacklist.h"   // gate 1, and where the brute-force guard writes
#include "RemoteTls.h"         // Schannel — mandatory on any non-loopback bind
#include "RemoteCrypto.h"
#include "RemoteExec.h"
#include "RemoteImageXfer.h" // BuildPreviewJpeg + ReadImageFile — both run on THIS thread
#include "RemoteLog.h"    // Ctrl+F12 — the inbound half of the wire record
#include "RemoteBeacon.h" // mDNS announcement — reconciled on every Start/Stop

#include "Platform/Constants.h"
#include "Platform/ConstantsStrings.h"
#include "Input/Command.h"

#include <algorithm>
#include <map>      // the failed-authentication table, keyed by peer address
#include <memory>   // shared_ptr — the per-connection send lock outlives its thread
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

    // --- Failed-authentication tracking -------------------------------------
    //
    // Keyed by peer ADDRESS, and touched from several client threads at once,
    // so every access is under the mutex. Deliberately in memory only: a ban
    // that survived a process restart would need a file, and a file recording
    // who tried to connect is a new thing to leak.
    //
    // It is NOT cleared by Stop(), so stopping and starting the listener from
    // the F9 panel does not hand an attacker a reset. Only exiting qIV clears
    // it, and an attacker cannot cause that.
    struct AuthFailures {
        int       count      = 0;   // failures inside the current window
        long long firstMs    = 0;   // when the window opened
        long long lastSeenMs = 0;   // for eviction at AUTH_TRACK_MAX
    };

    std::map<std::wstring, AuthFailures> g_authFails;
    std::mutex                           g_authFailMutex;

    long long NowMs() { return static_cast<long long>(GetTickCount64()); }

    // "<maxDim>[;<quality>]". Anything unparseable leaves the default in place
    // rather than failing the request — a preview that is the wrong size is a
    // far better answer than no preview, and the clamps in BuildPreviewJpeg
    // catch out-of-range values anyway.
    void ParsePreviewPayload(const std::wstring &payload, int &maxDim, int &quality) {
        const size_t sep = payload.find(L';');
        const std::wstring a = payload.substr(0, sep);
        const std::wstring b = (sep == std::wstring::npos) ? std::wstring()
                                                           : payload.substr(sep + 1);
        try { if (!a.empty()) maxDim  = std::stoi(a); } catch (...) {}
        try { if (!b.empty()) quality = std::stoi(b); } catch (...) {}
    }

    // One failed handshake. Opens or extends the window, and on the Nth failure
    // inside it hands the address to the BLACKLIST.
    //
    // The blacklist is where a ban lives now — a file, written with the time and
    // the reason, surviving restarts. The counter here is only the trigger: it
    // exists so a single mistyped password is not a permanent block, and so what
    // ends up in the file is a BURST rather than an accumulation of unrelated
    // mistakes months apart.
    void NoteAuthFailure(const std::wstring &peer) {
        // KEYED BY SCOPE, NOT BY ADDRESS. For v4 the two are the same string and
        // nothing below changes. For v6 this collapses the peer's whole /64 to
        // one key, which is what makes the counter reachable at all — see
        // BlockScope. It is also what goes into the blacklist, so the block
        // covers the prefix the guesses actually came from rather than the one
        // address that happened to send the fifth of them.
        const std::wstring key = BlockScope(peer);

        const long long now = NowMs();
        bool blacklist = false;

        {
            std::lock_guard<std::mutex> lk(g_authFailMutex);

            // Cap the table before inserting, so a flood of distinct sources
            // cannot grow it without bound. Evicts the least recently seen —
            // the entry least likely to be an attack in progress.
            if (g_authFails.size() >= RT::AUTH_TRACK_MAX && !g_authFails.count(key)) {
                auto oldest = g_authFails.begin();
                for (auto it = g_authFails.begin(); it != g_authFails.end(); ++it)
                    if (it->second.lastSeenMs < oldest->second.lastSeenMs) oldest = it;
                g_authFails.erase(oldest);
            }

            AuthFailures &f = g_authFails[key];
            f.lastSeenMs = now;

            // A stale window is a NEW window, not a continuation. Without this
            // an address that fails once a month is eventually blocked for it.
            if (f.count == 0 || now - f.firstMs > RT::AUTH_FAIL_WINDOW_MS) {
                f.count   = 1;
                f.firstMs = now;
            } else if (++f.count >= RT::AUTH_MAX_FAILURES) {
                blacklist = true;
                // Reset rather than erase: the address is about to be refused at
                // accept(), so this record has done its job, and leaving the
                // count at the threshold would re-trigger on any later attempt
                // that slipped through.
                f.count   = 0;
                f.firstMs = 0;
            }
        }

        // Outside the lock — this writes a file, and the accept path must not
        // wait behind disk IO.
        if (blacklist) {
            Blacklist::Add(key,
                           std::wstring(Constants::Messages::BLACKLIST_REASON_AUTH_PREFIX) +
                               std::to_wstring(RT::AUTH_MAX_FAILURES) +
                               Constants::Messages::BLACKLIST_REASON_AUTH_SUFFIX);
        }
    }

    // A completed handshake clears the record entirely — the address has proved
    // it holds the password, so its earlier typos are not evidence of anything.
    void ClearAuthFailures(const std::wstring &peer) {
        // The SAME key NoteAuthFailure used, or the record it wrote is never
        // found and a v6 client's earlier typos accumulate across successful
        // sessions until they blacklist their own prefix.
        std::lock_guard<std::mutex> lk(g_authFailMutex);
        g_authFails.erase(BlockScope(peer));
    }

    // --- Observers ----------------------------------------------------------
    // Connections that sent `observe 1`. Written by client threads (join/leave)
    // and read by the UI thread (emit), so every access is under this mutex.
    // The ConnId IS the socket — an observer cannot outlive its connection, so
    // there is nothing to reconcile and nothing to persist.
    // An observer, and whether it is on THIS machine.
    //
    // The locality matters for the same reason it does when driving: a playlist
    // POSITION only means something against the same set of files. An observer
    // on this box shares the folder and can follow the picture exactly; one
    // across the network has its own content, so an index would send it
    // somewhere arbitrary. It still receives the actions — zoom, rotate,
    // effects, view mode, slideshow — and follows what is being DONE without
    // following what is being SHOWN. That is the most that can honestly be
    // delivered, and it is silently wrong to pretend otherwise.
    struct Observer {
        SOCKET sock        = INVALID_SOCKET;
        bool   sameMachine = false;
        // Null on a plaintext (loopback) listener. Owned by the client thread's
        // stack; safe to hold because that thread removes itself from
        // g_observers before the session is destroyed — see ClientThread.
        Tls::Session *tls  = nullptr;
    };

    std::vector<Observer> g_observers;
    std::mutex            g_observerMutex;

    // ConnId → the TLS session for that connection.
    //
    // Needed because AddObserver is reached from the COMMAND path (`observe 1`
    // in RemoteExec), which knows only the connection id — while the session is
    // a local of the client thread. Registered for the life of that thread and
    // removed before it returns, so an entry here always outlives every lookup.
    std::map<ConnId, Tls::Session *> g_sessions;
    std::mutex                       g_sessionMutex;

    void RegisterSession(ConnId c, Tls::Session *t) {
        std::lock_guard<std::mutex> lk(g_sessionMutex);
        if (t) g_sessions[c] = t;
    }
    void UnregisterSession(ConnId c) {
        std::lock_guard<std::mutex> lk(g_sessionMutex);
        g_sessions.erase(c);
    }
    Tls::Session *SessionFor(ConnId c) {
        std::lock_guard<std::mutex> lk(g_sessionMutex);
        auto it = g_sessions.find(c);
        return it == g_sessions.end() ? nullptr : it->second;
    }

    // ConnId → that connection's SEND LOCK. For the PLAINTEXT path; the TLS one
    // already has an equivalent inside the session.
    //
    // TWO THREADS WRITE TO ONE SOCKET. The client thread answers commands; the
    // UI thread pushes EVENT lines from EmitToObservers, mid-command, to every
    // observer at once. Two sends running at the same time on one TCP stream
    // interleave wherever the send buffer forces a partial write, and half a
    // line inside another line desynchronises that peer's parser permanently —
    // on a wall streaming an image, a corrupted picture rather than a glitch.
    //
    // The TLS half was never exposed: Session::TrySend refuses outright when the
    // client thread holds the session, which is the same rule. This is that rule
    // for the other half — which only ever runs on loopback, and so was the easy
    // one to leave out.
    //
    // shared_ptr because the two sides do NOT share a lifetime: the UI thread
    // can still be holding the lock at the moment the client thread tears its
    // connection down and unregisters.
    std::map<ConnId, std::shared_ptr<std::mutex>> g_sendLocks;
    std::mutex                                    g_sendLockMutex;

    // ConnId → the name that connection asked to be called, from `hello <name>`.
    //
    // A LABEL, NEVER A CREDENTIAL. It is the only string on the wire a peer
    // chooses about itself, so nothing is decided by it: not the AllowList, not
    // the blacklist, not the failed-authentication table — all of those stay
    // keyed by the address, which a peer cannot choose. It reaches exactly one
    // place, the log, and it is always shown BESIDE the address rather than
    // instead of it, so a peer calling itself "127.0.0.1" or "Monitor2" cannot
    // make a log row lie about where it came from.
    std::map<ConnId, std::wstring> g_peerNames;
    std::mutex                     g_peerNameMutex;

    void SetPeerName(ConnId c, const std::wstring &name) {
        std::lock_guard<std::mutex> lk(g_peerNameMutex);
        if (name.empty()) g_peerNames.erase(c);
        else              g_peerNames[c] = name;
    }
    void ForgetPeerName(ConnId c) {
        std::lock_guard<std::mutex> lk(g_peerNameMutex);
        g_peerNames.erase(c);
    }
    std::wstring PeerNameFor(ConnId c) {
        std::lock_guard<std::mutex> lk(g_peerNameMutex);
        auto it = g_peerNames.find(c);
        return it == g_peerNames.end() ? std::wstring() : it->second;
    }

    // ConnId → what KIND of client it says it is: `win`, `android`, or empty for
    // one that never said. Shares the name mutex because the two are written by
    // the same thread at the same moment and read together by the same panel —
    // a second lock would buy nothing and add an ordering to get wrong.
    //
    // A HINT, exactly like the name: peer-chosen, used only to pick a glyph, and
    // never consulted by anything that decides what a connection may do.
    std::map<ConnId, AgentInfo> g_peerAgents;

    void SetPeerAgent(ConnId c, const AgentInfo &info) {
        std::lock_guard<std::mutex> lk(g_peerNameMutex);
        g_peerAgents[c] = info;
    }
    void ForgetPeerAgent(ConnId c) {
        std::lock_guard<std::mutex> lk(g_peerNameMutex);
        g_peerAgents.erase(c);
    }
    AgentInfo PeerAgentFor(ConnId c) {
        std::lock_guard<std::mutex> lk(g_peerNameMutex);
        auto it = g_peerAgents.find(c);
        return it == g_peerAgents.end() ? AgentInfo{} : it->second;
    }

    // ConnId → the FACTS about that connection, for the F9 panel's live list and
    // for Kick/Ban to act on a row the operator can actually see.
    //
    // A fourth map keyed by ConnId rather than fields bolted onto one of the
    // three above, because its lifetime is deliberately the WIDEST of them: a
    // connection appears here before the TLS handshake, so a peer stalling in
    // the handshake — the one an operator most wants to eject — is listed and
    // kickable. The session and send-lock maps cannot start that early, since
    // neither thing exists yet.
    //
    // `address` is the bare peer address, kept because Ban needs it AFTER the
    // socket is gone; ConnLabel's combined form cannot be taken apart again.
    struct LiveConn {
        std::wstring address;
        int          port        = 0;
        long long    sinceMs     = 0;
        bool         tls         = false;
        bool         sameMachine = false;
    };
    std::map<ConnId, LiveConn> g_liveConns;
    std::mutex                 g_liveConnMutex;

    void RegisterLiveConn(ConnId c, const LiveConn &info) {
        std::lock_guard<std::mutex> lk(g_liveConnMutex);
        g_liveConns[c] = info;
    }
    void UnregisterLiveConn(ConnId c) {
        std::lock_guard<std::mutex> lk(g_liveConnMutex);
        g_liveConns.erase(c);
    }

    void RegisterSendLock(ConnId c) {
        std::lock_guard<std::mutex> lk(g_sendLockMutex);
        g_sendLocks[c] = std::make_shared<std::mutex>();
    }
    void UnregisterSendLock(ConnId c) {
        std::lock_guard<std::mutex> lk(g_sendLockMutex);
        g_sendLocks.erase(c);
    }
    // Null for a socket no other thread can reach — the accept loop's
    // connection-cap refusal writes to one before any client thread exists.
    std::shared_ptr<std::mutex> SendLockFor(ConnId c) {
        std::lock_guard<std::mutex> lk(g_sendLockMutex);
        auto it = g_sendLocks.find(c);
        return it == g_sendLocks.end() ? nullptr : it->second;
    }

    // Shadow count, so the COMMON case costs an atomic load instead of a lock.
    //
    // HasObservers() is called on every command and every image change. A viewer
    // that nobody is watching — which is nearly all of them, nearly all the time
    // — would otherwise take and release a mutex on every keystroke to discover
    // an empty vector. Written only under g_observerMutex, so it cannot disagree
    // with the vector; read without it, because a stale "false" for the few
    // nanoseconds between adding an observer and publishing the count costs at
    // most one missed event on the connection that just asked to observe.
    std::atomic<int> g_observerCount{0};

    // Tell the UI thread the client count (or the running state) changed, so the
    // overlay's server indicator repaints. Posted, never sent: this is called
    // from socket threads and from Stop(), and none of them may block on the UI.
    void NotifyClientsChanged() {
        if (g_owner) PostMessageW(g_owner, Constants::WM_QIV_REMOTE_CLIENTS, 0, 0);
    }

    void SetEndpoint(const std::wstring &s) {
        std::lock_guard<std::mutex> lk(g_endpointMutex);
        g_endpoint = s;
    }

    Settings SnapshotCopy() {
        std::lock_guard<std::mutex> lk(g_snapshotMutex);
        return g_snapshot;
    }

    // Address matching (AddressMatches / InList) now lives in RemoteSettings, so
    // the AllowList here and the blacklist in its own translation unit cannot
    // drift apart on what "matches" means.

    // --- Socket helpers -----------------------------------------------------

    // `tls` is null on a loopback-bound listener, where the socket carries plain
    // bytes, and non-null everywhere else. Threading it through these three
    // functions rather than wrapping the SOCKET keeps every existing call site
    // unchanged and puts the branch in one place per direction.
    bool SendAll(SOCKET s, const std::string &bytes, Tls::Session *tls = nullptr) {
        if (tls) return tls->Send(s, bytes.data(), bytes.size());

        // HELD FOR THE WHOLE LINE, partial writes included — that is the point.
        // See g_sendLocks: the UI thread may be pushing an EVENT into this same
        // socket, and it only ever TRIES for this lock, so waiting here costs it
        // nothing.
        const std::shared_ptr<std::mutex> lock = SendLockFor(static_cast<ConnId>(s));
        std::unique_lock<std::mutex> guard;
        if (lock) guard = std::unique_lock<std::mutex>(*lock);

        size_t sent = 0;
        while (sent < bytes.size()) {
            const int n = send(s, bytes.data() + sent,
                               static_cast<int>(bytes.size() - sent), 0);
            if (n <= 0) return false;
            sent += static_cast<size_t>(n);
        }
        return true;
    }

    // Forward declarations — the log helpers need PeerLabel/NowUs, which are
    // defined below with the rest of the socket helpers.
    std::wstring PeerLabel(SOCKET s);
    long long    NowUs();

    // The handshake carries a salt, a nonce and an HMAC over that nonce. None of
    // it is reusable against the next connection — but it is still credential
    // material, and this log gets SAVED TO DISK and handed to whoever is helping
    // you debug. Redacted at CAPTURE, so it never enters the store at all: a
    // filter applied at save time would be one forgotten code path away from
    // writing the real thing.
    std::wstring RedactForLog(const std::wstring &line) {
        if (_wcsnicmp(line.c_str(), L"AUTH", 4) != 0) return line;
        return L"AUTH <redacted>";
    }

    // When THIS connection's last inbound line was read. thread_local because
    // every client gets its own thread, which makes this exactly per-connection
    // with no map and no lock. Lets the reply carry the handling time without
    // threading a timestamp through six call sites.
    thread_local long long t_inboundAtUs = 0;

    // EVERY line this server writes goes through SendLine, and every line it
    // reads goes through RecvLine. That is why the Ctrl+F12 record sits in these
    // two and not at the call sites: the banner, the auth challenge, the verb
    // replies (help/ping/version) and the parse-error refusals never reach the
    // UI thread at all, so instrumenting the command path alone left a log with
    // invisible holes in it.
    bool SendLine(SOCKET s, const std::wstring &line, Tls::Session *tls = nullptr) {
        if (Log::IsEnabled()) {
            // Delta is the time since the line being answered arrived — the
            // handling cost of this instance, which is the number that says
            // whether it is the reason a wall of screens is lagging.
            const long long d = t_inboundAtUs ? NowUs() - t_inboundAtUs : -1;
            t_inboundAtUs = 0;
            Log::Add(Log::Direction::Out, Log::SelfLabel(), L"(reply)",
                     PeerLabel(s), RedactForLog(line), d);
        }
        return SendAll(s, ToUtf8(line) + "\r\n", tls);
    }

    // Reads one \n-terminated line. Returns false on disconnect, error, or a
    // line that exceeds MAX_LINE_LEN — an unbounded line is the one input a
    // peer fully controls, so the connection is dropped rather than buffered.
    bool RecvLine(SOCKET s, std::string &accum, std::wstring &lineOut,
                  Tls::Session *tls = nullptr) {
        for (;;) {
            const size_t nl = accum.find('\n');
            if (nl != std::string::npos) {
                std::string raw = accum.substr(0, nl);
                accum.erase(0, nl + 1);
                if (!raw.empty() && raw.back() == '\r') raw.pop_back();
                lineOut = FromUtf8(raw.data(), raw.size());
                if (Log::IsEnabled() && !lineOut.empty()) {
                    t_inboundAtUs = NowUs();
                    Log::Add(Log::Direction::In, PeerLabel(s), RedactForLog(lineOut),
                             Log::SelfLabel(), L"(awaiting reply)", -1);
                }
                return true;
            }
            if (accum.size() > RT::MAX_LINE_LEN) return false;

            // The TLS session does its own buffering — it must, because a record
            // boundary and a line boundary have nothing to do with each other.
            // `accum` still holds the LINE remainder on top of that, so a reply
            // that arrives in one record but spans two lines still works.
            if (tls) {
                if (!tls->Recv(s, accum)) return false;
                continue;
            }

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

    // Microseconds from a monotonic source. Same body as the mirror's own NowUs
    // — duplicated rather than shared because the two modules have no header in
    // common and a four-line clock is not worth one.
    long long NowUs() {
        LARGE_INTEGER f, c;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&c);
        if (f.QuadPart == 0) return 0;
        return (c.QuadPart * 1000000LL) / f.QuadPart;
    }

    int PeerPort(const sockaddr_storage &ss) {
        if (ss.ss_family == AF_INET)
            return ntohs(reinterpret_cast<const sockaddr_in *>(&ss)->sin_port);
        if (ss.ss_family == AF_INET6)
            return ntohs(reinterpret_cast<const sockaddr_in6 *>(&ss)->sin6_port);
        return 0;
    }

    // "192.168.0.7:51423", or "[2001:db8::1]:51423".
    //
    // THE PORT IS WHAT MAKES TWO CONNECTIONS TELLABLE APART. An address alone
    // cannot: a phone that reconnects every few minutes, or a wall where two
    // instances on one box both dial in, produce rows that are identical in
    // every visible column — and an arrival then cannot be paired with its own
    // departure, which is most of what these lines are for.
    //
    // Brackets around a v6 address because its own colons would otherwise read
    // as the separator. Address alone when the port is unknown; a made-up 0 in
    // the label would be worse than an absent one.
    std::wstring ConnLabel(const std::wstring &addr, int port) {
        if (addr.empty()) return L"(unknown)";
        if (port <= 0) return addr;
        return FormatEndpoint(addr, port);
    }

    // "Ada's phone 192.168.0.7:51423" once that peer has said hello, and the
    // bare endpoint until it does.
    //
    // NAME FIRST, ADDRESS ALWAYS. The name is what a person recognises and the
    // address is what is true — a peer chooses the first and cannot choose the
    // second, so dropping the address for a nicer column would be handing the
    // peer control of what the log says about it.
    std::wstring NamedLabel(ConnId c, const std::wstring &endpoint) {
        const std::wstring name = PeerNameFor(c);
        return name.empty() ? endpoint : name + L" " + endpoint;
    }

    // Who is at the other end of `s`, for the Ctrl+F12 log's Sender column.
    //
    // An ADDRESS, not a name: the protocol has no "who am I" handshake — the
    // driving instance identifies the target, never itself — so an address is
    // the only thing this end actually knows. Adding a handshake to make the
    // column prettier would change the wire format for a diagnostic.
    //
    // With the source port, so a command line and the connection line that
    // opened it name the SAME thing. Two spellings of "who" across one log is a
    // correlation the reader has to do by hand, and cannot do at all when one
    // address holds two connections.
    std::wstring PeerLabel(SOCKET s) {
        if (s == INVALID_SOCKET) return L"(local)";
        sockaddr_storage ss{};
        int len = sizeof(ss);
        if (getpeername(s, reinterpret_cast<sockaddr *>(&ss), &len) != 0)
            return L"(unknown)";
        return NamedLabel(static_cast<ConnId>(s), ConnLabel(PeerAddress(ss), PeerPort(ss)));
    }

    // Is the peer on the other end of `s` this very machine?
    //
    // Not a comparison against "127.0.0.1": a second instance on this box may
    // have connected via the machine's LAN address or its name, and calling that
    // remote would needlessly stop it following positions when it shares the
    // folder and could follow them perfectly.
    bool PeerIsSameMachine(SOCKET s) {
        sockaddr_storage ss{};
        int len = sizeof(ss);
        if (getpeername(s, reinterpret_cast<sockaddr *>(&ss), &len) != 0) return false;

        if (ss.ss_family == AF_INET) {
            const auto *s4 = reinterpret_cast<const sockaddr_in *>(&ss);
            if ((ntohl(s4->sin_addr.s_addr) >> 24) == 127) return true;
        } else if (ss.ss_family == AF_INET6) {
            const auto *s6 = reinterpret_cast<const sockaddr_in6 *>(&ss);
            if (IN6_IS_ADDR_LOOPBACK(&s6->sin6_addr)) return true;
            // IPv4-mapped loopback arrives here on a dual-stack socket.
            if (IN6_IS_ADDR_V4MAPPED(&s6->sin6_addr) && s6->sin6_addr.s6_addr[12] == 127)
                return true;
        }

        // Not loopback — but it may still be an address this machine answers to.
        char self[256] = {};
        if (gethostname(self, sizeof(self)) != 0) return false;

        addrinfo hints{};
        hints.ai_family   = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        addrinfo *mine = nullptr;
        if (getaddrinfo(self, nullptr, &hints, &mine) != 0 || !mine) return false;

        bool same = false;
        for (addrinfo *b = mine; b && !same; b = b->ai_next) {
            if (b->ai_family != ss.ss_family) continue;
            if (b->ai_family == AF_INET) {
                const auto *m4 = reinterpret_cast<const sockaddr_in *>(b->ai_addr);
                const auto *p4 = reinterpret_cast<const sockaddr_in *>(&ss);
                same = (m4->sin_addr.s_addr == p4->sin_addr.s_addr);
            } else if (b->ai_family == AF_INET6) {
                const auto *m6 = reinterpret_cast<const sockaddr_in6 *>(b->ai_addr);
                const auto *p6 = reinterpret_cast<const sockaddr_in6 *>(&ss);
                same = (memcmp(&m6->sin6_addr, &p6->sin6_addr, sizeof(in6_addr)) == 0);
            }
        }
        freeaddrinfo(mine);
        return same;
    }

    // --- Authentication -----------------------------------------------------

    // Challenge-response. The password never crosses the wire: the server sends
    // a fresh random nonce, the client returns HMAC(secret, nonce), and the
    // secret is derived from the stored hash so the server never holds plaintext
    // either. A captured response is useless against the next nonce.
    bool Authenticate(SOCKET s, const Settings &cfg, std::string &accum,
                      const std::wstring &peer, Tls::Session *tls) {
        // No password configured. WhyCannotStart has already refused to start a
        // listener in this state on anything but loopback, so reaching here
        // means a local-only server and the openness is the intended one.
        //
        // SAID OUT LOUD, not signalled by silence. Exactly one line follows the
        // banner on every connection — "OK" here, an AUTH challenge below — so a
        // client blocks for it and reads what it says. Staying quiet was the v4
        // behaviour and it forced both clients to guess from a timeout, which
        // got the answer wrong whenever a challenge was slower than the probe.
        if (cfg.passwordHash.empty()) return SendLine(s, MakeOk(), tls);

        const std::vector<BYTE> secret = Crypto::SecretFromStored(cfg.passwordHash);
        const std::vector<BYTE> salt   = Crypto::SaltFromStored(cfg.passwordHash);
        const int         iterations   = Crypto::IterationsFromStored(cfg.passwordHash);
        const std::vector<BYTE> nonce  = Crypto::RandomBytes(NONCE_LEN);
        // A failed RNG must abort the connection. Falling back to anything
        // predictable would silently remove the replay protection.
        if (secret.empty() || salt.empty() || iterations <= 0 || nonce.empty()) {
            SendLine(s, MakeErr(RT::ERR_INTERNAL, L"server cannot generate a challenge"), tls);
            return false;
        }

        // "AUTH <iterations> <salt> <nonce>". All three have to travel: the
        // client holds only the plaintext password and cannot derive the shared
        // digest without the salt AND the work factor it was made with. Neither
        // is secret — a salt's job is uniqueness and an iteration count's is
        // cost, and hiding either would only prevent legitimate clients from
        // computing the same value.
        if (!SendLine(s, L"AUTH " + std::to_wstring(iterations) + L" " +
                             Crypto::ToHex(salt) + L" " + Crypto::ToHex(nonce), tls))
            return false;

        std::wstring line;
        if (!RecvLine(s, accum, line, tls)) return false;

        // Expected form: "AUTH <hex>"
        std::wstring got = line;
        if (_wcsnicmp(got.c_str(), L"AUTH ", 5) == 0) got = got.substr(5);
        else {
            // Counted as a failure like any other. A brute-forcer is free to
            // send a malformed line instead of a wrong one, and a rule that
            // only counted well-formed guesses would be trivially sidestepped.
            NoteAuthFailure(peer);
            Sleep(RT::AUTH_FAIL_DELAY_MS);
            SendLine(s, MakeErr(RT::ERR_NOT_AUTHENTICATED, L"expected AUTH <response>"), tls);
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
            NoteAuthFailure(peer);
            // BEFORE the reply, not after. The cost has to be paid inside the
            // attempt: a delay applied after the answer is sent is a delay the
            // attacker skips by dropping the connection and opening the next.
            Sleep(RT::AUTH_FAIL_DELAY_MS);
            SendLine(s, MakeErr(RT::ERR_AUTH_FAILED, L"authentication failed"), tls);
            return false;
        }

        // Proved. Its earlier typos are not evidence of anything.
        ClearAuthFailures(peer);
        // Tell the client it is authenticated — clients expect an OK line here.
        if (!SendLine(s, MakeOk(), tls)) return false;
        return true;

    }

    // --- Per-client conversation --------------------------------------------

    // The peer address IS passed now — the failed-authentication table is keyed
    // by it, and that table is written from here, after the accept gates have
    // finished with it.
    void ClientThread(SOCKET client, Settings cfg, HWND owner, std::wstring peer,
                      int peerPort) {
        std::string accum;

        // `peer` stays the BARE ADDRESS — it is what the AllowList, the
        // blacklist and the failed-authentication table are keyed by, and a port
        // in any of those would break every rule ever written. `who` is the
        // display form, and it exists only for the log.
        const std::wstring who = ConnLabel(peer, peerPort);

        // For the session duration on the departure line. Cheap enough to take
        // unconditionally: recording can be switched on mid-connection, and a
        // duration that only exists when the log happened to be running at the
        // moment of the accept would be missing from exactly the sessions
        // somebody turned the log on to watch.
        const long long startedUs = NowUs();

        // WHY this connection ended, recorded at the exit that ends it.
        //
        // The log has always shown arrivals and never departures, which leaves
        // the most common question about a wall of screens unanswerable: a row
        // that stopped responding could have gone away an hour ago or be sitting
        // there connected and idle, and the log looked identical either way.
        //
        // Assigned at each break rather than worked out at the bottom, because
        // by then they are indistinguishable — "the peer closed", "the read
        // timed out" and "we could not write to it" all arrive at the same line,
        // and telling them apart is the whole reason to look.
        const wchar_t *departure = L"(disconnected)";

        // WIC is COM, and SendDisplayedPreview decodes on THIS thread. MTA
        // rather than STA: nothing here pumps a message loop, and an STA
        // apartment without one deadlocks the moment a proxy is needed.
        //
        // Balanced at every exit below — there are two, and both are after this.
        const HRESULT comInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        // THE HANDSHAKE DEADLINE, and it goes on BEFORE the TLS handshake
        // because that is the first thing a silent peer can stall.
        //
        // Everything from here to a completed authentication is bounded: the
        // ClientHello, the AUTH answer, every recv in between. Without it this
        // thread blocks in recv() for ever on a peer that connects and says
        // nothing, and MAX_CONNECTIONS of those take the listener off the air at
        // the cost of one open socket each — no password, no protocol, nothing
        // the gates above can see. See HANDSHAKE_TIMEOUT_MS.
        //
        // The SEND bound stays on for the whole connection. It is generous
        // enough never to cut a real image transfer short and only bites on a
        // peer that has stopped reading altogether.
        {
            const DWORD rcv = RT::HANDSHAKE_TIMEOUT_MS;
            const DWORD snd = RT::SEND_TIMEOUT_MS;
            setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
                       reinterpret_cast<const char *>(&rcv), sizeof(rcv));
            setsockopt(client, SOL_SOCKET, SO_SNDTIMEO,
                       reinterpret_cast<const char *>(&snd), sizeof(snd));
        }

        // KEEPALIVE, and it is here rather than after authentication on purpose:
        // the deadline above covers an unauthenticated peer, but the moment that
        // deadline is LIFTED below there is nothing left watching the socket at
        // all. Setting it now means there is never a window without one of the
        // two. What it buys is the NAT case — an idle mirror across a home
        // router, where the mapping is dropped silently and this thread would
        // otherwise wait in recv() forever on a connection that is already gone.
        EnableKeepAlive(static_cast<UINT_PTR>(client));

        // LISTED FROM HERE, before the TLS handshake — see g_liveConns. Both
        // exits below unregister; there are exactly two, and the early one is
        // the failed handshake a few lines down.
        {
            LiveConn info;
            info.address     = peer;
            info.port        = peerPort;
            info.sinceMs     = NowMs();
            // The DECISION, which is known before the handshake runs and does
            // not depend on it succeeding — the same rule the peer is about to
            // be held to.
            info.tls         = Tls::RequiredForAddress(peer);
            info.sameMachine = PeerIsSameMachine(client);
            RegisterLiveConn(static_cast<ConnId>(client), info);
        }

        // TLS FIRST, before the banner — the banner is already application data
        // and must travel inside the tunnel, not in front of it.
        //
        // Whether to do this at all is decided by the BIND ADDRESS, not by
        // anything the client said, so there is nothing here for a peer to
        // influence. A client that opens the connection and speaks plaintext to
        // a TLS endpoint simply fails the handshake and is dropped.
        // PER CONNECTION, from the PEER's address — not from the bind address.
        //
        // A listener on 0.0.0.0 serves both: another copy on this machine gets
        // plaintext (nothing left the box, and the local multi-screen wall pays
        // no handshake), while anything from off-machine gets TLS.
        //
        // Safe because a peer address is not something an attacker can choose:
        // a TCP connection has to complete a handshake, so a packet claiming to
        // come from 127.0.0.1 cannot carry a conversation unless it really is
        // local. Deciding from the BIND address instead — which is what this
        // used to do — forced the whole listener into one mode and made
        // 0.0.0.0 break every loopback client.
        Tls::Session  tlsSession;
        Tls::Session *tls = nullptr;

        // Recorded because this decision is INVISIBLE otherwise, and getting it
        // wrong looks like nothing at all: the server waits for a ClientHello,
        // the client waits for a banner, and both sit there until somebody times
        // out. The peer address is the whole input to the decision, so the log
        // has to name it — "which address did you actually see me as" is the
        // question that cannot be answered from the client side.
        if (Log::IsEnabled())
            Log::Add(Log::Direction::In, who,
                     std::wstring(Tls::RequiredForAddress(peer)
                                      ? L"(accepted — expecting TLS"
                                      : L"(accepted — plaintext") +
                         // SAID OUT LOUD, because the address alone cannot say
                         // it. A second copy on this machine that dialled in
                         // over the LAN address rather than loopback shows up
                         // with the SAME address this instance listens on, and
                         // the log then looks like it is reporting itself.
                         (PeerIsSameMachine(client) ? L" — same machine)" : L")"),
                     Log::SelfLabel(), L"(connection)", -1);

        if (Tls::RequiredForAddress(peer)) {
            std::wstring err;
            if (!tlsSession.AcceptHandshake(client, err)) {
                // Silent TO THE PEER. A failed handshake is either a scanner, a
                // stale client, or an attacker; none of them are owed a
                // diagnostic, and the plaintext channel needed to deliver one is
                // precisely what this endpoint refuses to use.
                //
                // Recorded LOCALLY all the same — this is the single most
                // confusing failure the remote has, because both ends simply
                // wait, and the owner of the machine is not the attacker.
                if (Log::IsEnabled())
                    Log::Add(Log::Direction::In, who, L"(dropped — TLS handshake failed)",
                             Log::SelfLabel(), L"(connection)", NowUs() - startedUs);
                UnregisterLiveConn(static_cast<ConnId>(client));
                shutdown(client, SD_BOTH);
                closesocket(client);
                g_activeClients.fetch_sub(1, std::memory_order_acq_rel);
                NotifyClientsChanged();
                if (SUCCEEDED(comInit)) CoUninitialize();
                return;
            }
            tls = &tlsSession;
            RegisterSession(static_cast<ConnId>(client), tls);
        }

        // AFTER the handshake, BEFORE the first byte of application data — so
        // the banner below is already covered, and the early return above (a
        // failed handshake) has nothing to unregister. See g_sendLocks.
        RegisterSendLock(static_cast<ConnId>(client));

        SendLine(client, MakeOk(L"qIV " + std::wstring(Constants::APP_VERSION) +
                                L" remote v" + std::to_wstring(RT::PROTOCOL_VERSION) +
                                (cfg.name.empty() ? L"" : L" [" + cfg.name + L"]")), tls);

        if (Authenticate(client, cfg, accum, peer, tls)) {
            // DEADLINE LIFTED, now that this peer has proved it is allowed to be
            // here. An authenticated connection is expected to sit idle: a
            // mirrored screen waits for the next keystroke on the driving
            // instance, and that gap is minutes, not seconds. Keeping the
            // handshake bound in force would drop exactly the connections the
            // feature exists to hold open.
            //
            // What it cost the attacker is already paid — a slot can now only be
            // held by someone who knows the password, which is a different
            // problem with a different answer (MAX_CONNECTIONS, the AllowList).
            const DWORD noTimeout = 0;
            setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
                       reinterpret_cast<const char *>(&noTimeout), sizeof(noTimeout));

            for (;;) {
                if (g_stopRequested.load(std::memory_order_acquire)) {
                    SendLine(client, MakeErr(RT::ERR_INTERNAL, L"server shutting down"), tls);
                    departure = L"(disconnected — server shutting down)";
                    break;
                }

                std::wstring line;
                if (!RecvLine(client, accum, line, tls)) {
                    // One line covers three causes and they cannot be separated
                    // here: a clean close, a reset, and — for an unauthenticated
                    // peer — the handshake deadline all surface as a failed read.
                    // The command loop only runs AFTER authentication, so by this
                    // point there is no deadline left and it is genuinely the
                    // peer that went away.
                    departure = L"(disconnected — closed by peer)";
                    break;
                }

                const RemoteRequest req = ParseLine(line);

                if (req.status == ParseStatus::EmptyLine) continue;

                if (req.status == ParseStatus::Verb) {
                    switch (req.verb) {
                        case Verb::Help:
                            SendAll(client, ToUtf8(BuildHelpText()), tls);
                            SendLine(client, MakeOk(), tls);
                            break;
                        case Verb::Ping:
                            SendLine(client, MakeOk(L"pong"), tls);
                            break;
                        case Verb::Hello: {
                            // SANITISED HARD, because this is peer-chosen text
                            // that goes straight into a log column. Control
                            // characters and the log's own separators are out —
                            // a name carrying a tab would split a saved row into
                            // one that never happened — and the length is capped
                            // so a peer cannot push the address out of view.
                            std::wstring name;
                            for (wchar_t ch : req.payload) {
                                if (name.size() >= RT::PEER_NAME_MAX) break;
                                if (ch < 0x20 || ch == 0x7F) continue;
                                name += ch;
                            }
                            while (!name.empty() && name.back() == L' ') name.pop_back();

                            SetPeerName(static_cast<ConnId>(client), name);
                            // Answered so the client knows it was heard; the
                            // name is echoed so a log at THAT end records what
                            // this end actually stored after sanitising.
                            SendLine(client, MakeOk(name), tls);
                            break;
                        }
                        case Verb::Agent: {
                            // Parsed and sanitised by ParseAgent — including the
                            // closed platform vocabulary — so nothing peer-chosen
                            // reaches storage unfiltered.
                            SetPeerAgent(static_cast<ConnId>(client),
                                         ParseAgent(req.payload));

                            // ANSWERED WITH OUR OWN, which is what makes this a
                            // greeting rather than a report. The banner already
                            // told the client who we are in prose; this says the
                            // same thing in the form its parser reads, so both
                            // ends can label the other in their lists.
                            SendLine(client,
                                     MakeOk(BuildAgent(L"qIV",
                                                       Constants::APP_VERSION,
                                                       cfg.name)),
                                     tls);
                            break;
                        }
                        case Verb::Version:
                            SendLine(client, MakeOk(std::wstring(Constants::APP_VERSION) +
                                                    L" protocol " +
                                                    std::to_wstring(RT::PROTOCOL_VERSION)), tls);
                            break;
                        default:
                            SendLine(client, MakeErr(RT::ERR_INTERNAL, L"unhandled verb"), tls);
                            break;
                    }
                    continue;
                }

                if (req.status != ParseStatus::Ok) {
                    SendLine(client, MakeErrFor(req), tls);
                    continue;
                }

                // Hand the command to the UI thread and wait, bounded. See the
                // ownership note in RemoteServer.h — both sides hold a
                // shared_ptr, so a timeout here cannot corrupt anything.
                auto call = std::make_shared<RemoteCall>();
                call->req  = req;
                call->conn = static_cast<ConnId>(client);

                if (!call->doneEvent) {
                    SendLine(client, MakeErr(RT::ERR_INTERNAL, L"no event"), tls);
                    continue;
                }

                if (!PostMessageW(owner, Constants::WM_QIV_REMOTE_COMMAND, 0,
                                  reinterpret_cast<LPARAM>(
                                      new std::shared_ptr<RemoteCall>(call)))) {
                    SendLine(client, MakeErr(RT::ERR_INTERNAL, L"viewer not accepting commands"), tls);
                    continue;
                }

                const DWORD w = WaitForSingleObject(call->doneEvent, REPLY_TIMEOUT_MS);
                if (w == WAIT_OBJECT_0) {
                    // SendDisplayedPreview's second stage. The UI thread has
                    // returned only the PATH; the decode, downscale and JPEG
                    // encode happen HERE, on this socket thread, because they
                    // take far longer than REPLY_TIMEOUT_MS on a large image and
                    // would freeze the viewer for the duration.
                    // SendDisplayedImage's second stage, same shape as the
                    // preview one below. The UI thread returned only the PATH;
                    // reading the file and base64-encoding it happens HERE.
                    //
                    // This one is the LARGER of the two — it is the original
                    // file, not a downscaled JPEG — so it is the one that hurt
                    // most while it ran on the UI thread. It is also what a
                    // phone sends when the user saves the displayed picture,
                    // which made an ordinary action stall the viewer.
                    const std::wstring origMarker = ORIGINAL_PATH_MARKER;
                    if (call->result.rfind(origMarker, 0) == 0) {
                        const std::wstring path = call->result.substr(origMarker.size());
                        std::wstring reply;

                        // The OK line's shape is protocol — "<bytes>;<name>"
                        // after the '=' — because a client checks the size
                        // against what it received and reads the name to know
                        // what it is decoding. Unchanged from when this ran on
                        // the UI thread: no client can tell the difference.
                        if (path.empty()) {
                            reply = MakeOk(L"SendDisplayedImage=0;"); // showing nothing
                        } else {
                            std::vector<unsigned char> bytes;
                            std::wstring err;
                            if (Xfer::ReadImageFile(path, bytes, err)) {
                                const size_t slash = path.find_last_of(L"\\/");
                                const std::wstring name =
                                    (slash == std::wstring::npos) ? path : path.substr(slash + 1);
                                reply = Xfer::BuildDataReplyBody(bytes) +
                                        MakeOk(L"SendDisplayedImage=" +
                                               std::to_wstring(bytes.size()) + L";" + name);
                            } else {
                                reply = MakeErr(RT::ERR_BAD_PAYLOAD, err);
                            }
                        }

                        if (!SendAll(client, ToUtf8(reply) + "\r\n", tls)) {
                            departure = L"(dropped — send failed)";
                            break;
                        }
                        continue;
                    }

                    const std::wstring marker = PREVIEW_PATH_MARKER;
                    if (call->result.rfind(marker, 0) == 0) {
                        const std::wstring path = call->result.substr(marker.size());
                        std::wstring reply;

                        if (path.empty()) {
                            reply = MakeOk(L"SendDisplayedPreview=0;"); // showing nothing
                        } else {
                            int maxDim  = RT::PREVIEW_MAX_DIM_DEFAULT;
                            int quality = RT::PREVIEW_QUALITY_DEFAULT;
                            ParsePreviewPayload(req.payload, maxDim, quality);

                            std::vector<unsigned char> jpeg;
                            std::wstring err;
                            if (Xfer::BuildPreviewJpeg(path, maxDim, quality, jpeg, err)) {
                                const size_t slash = path.find_last_of(L"\\/");
                                const std::wstring name =
                                    (slash == std::wstring::npos) ? path : path.substr(slash + 1);
                                reply = Xfer::BuildDataReplyBody(jpeg) +
                                        MakeOk(L"SendDisplayedPreview=" +
                                               std::to_wstring(jpeg.size()) + L";" + name);
                            } else {
                                reply = MakeErr(RT::ERR_BAD_PAYLOAD, err);
                            }
                        }

                        if (!SendAll(client, ToUtf8(reply) + "\r\n", tls)) {
                            // Distinct from "closed by peer" on purpose: a write
                            // that fails mid-image usually means a client that
                            // stopped reading, not one that hung up, and the two
                            // want different things looked at.
                            departure = L"(dropped — send failed)";
                            break;
                        }
                        continue;
                    }

                    if (!SendLine(client, call->result, tls)) {
                        departure = L"(dropped — send failed)";
                        break;
                    }
                } else {
                    if (!SendLine(client, MakeErr(RT::ERR_INTERNAL, L"viewer did not respond in time"), tls)) {
                        departure = L"(dropped — send failed)";
                        break;
                    }
                }
            }
        } else {
            // Authenticate has already sent the peer its ERR. This records the
            // CONNECTION ENDING, which is a different fact and the one that
            // closes the pair opened by the accept line.
            departure = L"(disconnected — authentication failed)";
        }

        // ONE DEPARTURE FOR EVERY ARRIVAL. Every path that reaches this point
        // passed the accept line above, so the two always come in pairs and a
        // connection still open is exactly an arrival with nothing under it.
        // That is the property the log needs for "who is on this machine right
        // now" to be answerable by reading it.
        if (Log::IsEnabled())
            // HOW LONG IT LASTED goes in the Δ column, which is otherwise "—" on
            // a connection row. On a command row that column is a round trip; on
            // this one it is the session, and the two are never confused because
            // only these rows say "(connection)".
            Log::Add(Log::Direction::In,
                     NamedLabel(static_cast<ConnId>(client), who), departure,
                     Log::SelfLabel(), L"(connection)", NowUs() - startedUs);

        // Leave the observer list BEFORE the socket closes. The UI thread emits
        // into these handles directly, and a closed socket still sitting in the
        // list is a write to a dead descriptor on the next action.
        RemoveObserver(static_cast<ConnId>(client));
        // Same ordering rule, same reason: nothing may look this connection's
        // session up after the stack frame holding it is gone.
        UnregisterSession(static_cast<ConnId>(client));
        // Safe in either order with the push above — it holds a shared_ptr, so
        // a lock it already took stays alive after this drops the map's copy.
        UnregisterSendLock(static_cast<ConnId>(client));
        // After the departure line above, which is the last row that wants it.
        ForgetPeerName(static_cast<ConnId>(client));
        ForgetPeerAgent(static_cast<ConnId>(client));
        // Last of the four, and BEFORE the socket closes: a row the panel can
        // still see is a row Kick can still act on, and acting on it once the
        // descriptor is closed would be a shutdown() against a number the
        // system may already have handed to something else.
        UnregisterLiveConn(static_cast<ConnId>(client));

        if (tls) tls->Shutdown(client);
        shutdown(client, SD_BOTH);
        closesocket(client);
        g_activeClients.fetch_sub(1, std::memory_order_acq_rel);
        NotifyClientsChanged();
        if (SUCCEEDED(comInit)) CoUninitialize();
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

            // Address for the RULES, address+port for the LOG. Kept apart on
            // purpose: every list and table below is keyed by the bare address,
            // and a port reaching any of them would break every rule ever typed.
            const int          peerPort = PeerPort(ss);
            const std::wstring who      = ConnLabel(peer, peerPort);

            // Gates in the order the spec fixes, and the order matters:
            // an address that fails 1 or 2 is told NOTHING, not even that
            // something is listening. Only a peer that has already proved it is
            // permitted here gets a diagnostic.

            // 1. BlackList — deny always overrides allow.
            //
            // Read LIVE from the blacklist module rather than from `cfg`, which
            // is a snapshot taken at Start(). An address blacklisted mid-session
            // by the brute-force guard has to be refused from that moment, not
            // from the next restart.
            if (Blacklist::IsBlocked(peer)) {
                // LOGGED, though the peer is told nothing. A refused connection
                // never reaches ClientThread, so until now it left no trace at
                // all — the one category of arrival the owner of an exposed
                // listener most wants to see. The store is a fixed-size ring, so
                // a scanner hammering the port rotates it rather than growing it.
                if (Log::IsEnabled())
                    Log::Add(Log::Direction::In, who, L"(refused — blacklisted)",
                             Log::SelfLabel(), L"(connection)", -1);
                closesocket(client);
                continue;
            }
            // 2. AllowList — empty means deny everyone, by design.
            //
            // No exception for loopback. 127.0.0.1 is SEEDED into the default
            // list so a fresh instance is reachable without anyone discovering
            // the fail-closed rule the hard way — but it is an ordinary entry,
            // and removing it from the .ini really does lock this machine out.
            // A list that some addresses can bypass is not a list.
            if (cfg.allowList.empty() || !InList(cfg.allowList, peer)) {
                // Which of the two it was, because they need opposite fixes: an
                // empty list is a server nobody configured, a non-match is a
                // rule that does not cover the address shown right here — and
                // that address is the answer to "what should I have typed".
                if (Log::IsEnabled())
                    Log::Add(Log::Direction::In, who,
                             cfg.allowList.empty() ? L"(refused — AllowList empty)"
                                                   : L"(refused — not on the AllowList)",
                             Log::SelfLabel(), L"(connection)", -1);
                closesocket(client);
                continue;
            }
            // 3. Connection cap.
            //
            // A permitted peer is told WHY on a plaintext endpoint. On a TLS
            // endpoint it is not: the diagnostic would have to go out before the
            // handshake, in the clear, to a client that is expecting a TLS
            // record — so it would read as a protocol error rather than "busy".
            //
            // Handshaking first purely to deliver a refusal was the alternative,
            // and it is worse: it hands anyone a way to make this machine do
            // asymmetric crypto for a connection that was never going to be
            // served. The cap is not a security boundary, and a silent close is
            // honest enough.
            if (g_activeClients.load(std::memory_order_acquire) >= cfg.maxConnections) {
                if (Log::IsEnabled())
                    Log::Add(Log::Direction::In, who, L"(refused — connection limit reached)",
                             Log::SelfLabel(), L"(connection)", -1);
                if (!Tls::RequiredForAddress(peer))
                    SendLine(client, MakeErr(RT::ERR_TOO_MANY_CLIENTS,
                                             L"connection limit reached"));
                shutdown(client, SD_BOTH);
                closesocket(client);
                continue;
            }

            g_activeClients.fetch_add(1, std::memory_order_acq_rel);
            NotifyClientsChanged();
            // Detached: a client's lifetime is its socket's, and Stop() closes
            // the sockets rather than joining every conversation.
            std::thread(ClientThread, client, cfg, g_owner, peer, peerPort).detach();
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

    // UI thread, and the moment this instance's name starts appearing in other
    // people's logs as well as its own. The socket threads that will want it
    // cannot read Config() themselves — see RemoteLog.h.
    Log::SetSelfName(cfg.name);

    const std::wstring blocked = WhyCannotStart(cfg);
    // An empty AllowList is a warning, not a refusal: the server binds and
    // listens, it just denies every caller. The panel reports it so a fully
    // functional-looking listener that refuses everything is never a mystery.
    if (!blocked.empty() && blocked != Constants::Messages::REMOTE_WARN_EMPTY_ALLOWLIST) {
        errorOut = blocked;
        return false;
    }

    // Read the blacklist BEFORE the socket exists. A listener that binds first
    // and loads its block list a moment later has a window in which a blocked
    // address is admitted, and that window is exactly when an attacker who was
    // blocked last night is retrying.
    Blacklist::Reload();

    // Same ordering, same reasoning: obtain the TLS identity before binding.
    //
    // A FAILURE HERE REFUSES TO START. There is no fallback to plaintext, and
    // that is the single most important line in this function: the whole point
    // of deciding encryption from the bind address is that it cannot be
    // negotiated away, and a listener that quietly opened in the clear because a
    // certificate could not be generated would negotiate it away by accident.
    if (Tls::RequiredForAddress(cfg.bindAddress)) {
        std::wstring tlsErr;
        if (!Tls::EnsureServerCredentials(tlsErr)) {
            errorOut = std::wstring(Constants::Messages::REMOTE_BLOCKED_TLS_UNAVAILABLE) + tlsErr;
            return false;
        }
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

    // DUAL STACK, and only meaningful when the bind address is an IPv6 one.
    //
    // Windows defaults IPV6_V6ONLY to 1, so a listener on "::" accepts IPv6 and
    // REFUSES IPv4 — which on a machine whose LAN is v4 looks like a server that
    // started cleanly and cannot be reached by anything. Clearing it makes the
    // single socket serve both, with v4 peers arriving as "::ffff:a.b.c.d";
    // AcceptedPeerAddress already normalises that form back to plain dotted quad
    // before the AllowList sees it, so the access rules do not change shape.
    //
    // Best effort: a failure leaves the documented v6-only behaviour, which is
    // still a working listener, so it must not refuse to start.
    if (res->ai_family == AF_INET6) {
        const DWORD v6only = 0;
        setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY,
                   reinterpret_cast<const char *>(&v6only), sizeof(v6only));
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
    SetEndpoint(FormatEndpoint(cfg.bindAddress, cfg.port));
    // The saved log's preamble names it, so a reader can tell this instance's
    // own address from a peer's — which matters most when they are the same,
    // because a second copy on this box may dial in over the LAN address.
    Log::SetSelfEndpoint(FormatEndpoint(cfg.bindAddress, cfg.port));

    g_owner = hOwner;
    g_listenSocket.store(s, std::memory_order_release);
    g_stopRequested.store(false, std::memory_order_release);
    g_running.store(true, std::memory_order_release);
    g_activeClients.store(0, std::memory_order_release);

    g_listenThread = std::thread(ListenThread);
    NotifyClientsChanged();   // the overlay indicator appears

    // The beacon can only be announced once there is a listener behind it, so it
    // is reconciled HERE rather than when the setting is read. Refresh decides
    // for itself whether anything should be published — a beacon with the
    // setting off, or on a loopback-only bind, is silently nothing.
    Beacon::Refresh();

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
    Log::SetSelfEndpoint({});
    NotifyClientsChanged();   // the overlay indicator goes away

    // Releases the TLS credentials and deletes the key container importing the
    // PFX created. Deliberately AFTER the listener thread has joined and the
    // listening socket is closed, so no handshake can be in flight against
    // credentials that are being torn down.
    //
    // Detached client threads may still be running, but each holds its own
    // security CONTEXT, which stays valid after the credential handle is freed —
    // Schannel keeps a reference for as long as a context derived from it lives.
    Tls::ShutdownServerCredentials();

    if (g_wsaUp) {
        WSACleanup();
        g_wsaUp = false;
    }

    // The listener is gone, so the announcement is now a promise nobody can
    // keep. Withdrawn here rather than left to expire: a client browsing after a
    // Stop would otherwise still list this instance and fail on connect, and the
    // user blames the app rather than the stopped server.
    Beacon::Refresh();
}

bool IsRunning() { return g_running.load(std::memory_order_acquire); }

int ActiveConnections() { return g_activeClients.load(std::memory_order_acquire); }

// =============================================================================
// Live connections — Kick and Ban. UI thread only; see the header.
// =============================================================================

std::vector<ClientInfo> Connections() {
    std::vector<ClientInfo> out;
    {
        std::lock_guard<std::mutex> lk(g_liveConnMutex);
        out.reserve(g_liveConns.size());
        for (const auto &[id, c] : g_liveConns) {
            ClientInfo info;
            info.id          = id;
            info.address     = c.address;
            info.port        = c.port;
            info.sinceMs     = c.sinceMs;
            info.tls         = c.tls;
            info.sameMachine = c.sameMachine;
            out.push_back(std::move(info));
        }
    }

    // The NAME is filled outside the connection lock, because PeerNameFor takes
    // a different mutex and holding two of them in an order nothing else agrees
    // on is how a deadlock is built. A name that arrives between the two loops
    // simply shows up on the next refresh.
    for (ClientInfo &info : out) {
        info.name     = PeerNameFor(info.id);
        const AgentInfo ag = PeerAgentFor(info.id);
        info.platform     = ag.platform;
        info.agentApp     = ag.app;
        info.agentVersion = ag.version;
        info.agentOs      = ag.os;
        info.agentHost    = ag.host;
        // `hello` still wins for the display name when it was sent — it is the
        // older path and some clients use only that. The agent's name fills in
        // for those that greet but never said hello.
        if (info.name.empty()) info.name = ag.name;
    }

    // OBSERVING is filled here for the same reason the name is: it needs
    // g_observerMutex, and taking that while still holding the connection lock
    // would introduce a second lock order for no gain. One pass over a list that
    // is a handful of entries.
    //
    // Matched on the socket because Observer::sock IS the ConnId — the observer
    // list needs no identity of its own, and giving it one would create a second
    // place for a name to go stale.
    {
        std::lock_guard<std::mutex> lk(g_observerMutex);
        for (ClientInfo &info : out) {
            const SOCKET s = static_cast<SOCKET>(info.id);
            for (const Observer &o : g_observers) {
                if (o.sock == s) { info.observing = true; break; }
            }
        }
    }

    // OLDEST FIRST, so the list does not reorder under the operator's cursor
    // every time somebody connects. The map is keyed by socket, and socket
    // numbers are reused — they are not an arrival order.
    std::sort(out.begin(), out.end(),
              [](const ClientInfo &a, const ClientInfo &b) { return a.sinceMs < b.sinceMs; });
    return out;
}

bool KickConnection(ConnId id) {
    {
        std::lock_guard<std::mutex> lk(g_liveConnMutex);
        if (!g_liveConns.count(id)) return false;
    }

    // OUTSIDE the lock, and see the header for why this is shutdown() rather
    // than closesocket(). The client thread notices its read failing, unwinds
    // through the ordinary departure path, and unregisters itself — so nothing
    // here erases the map entry.
    shutdown(static_cast<SOCKET>(id), SD_BOTH);
    return true;
}

bool TimedKickConnection(ConnId id, int minutes, std::wstring &scopeOut) {
    std::wstring address;
    {
        std::lock_guard<std::mutex> lk(g_liveConnMutex);
        auto it = g_liveConns.find(id);
        if (it == g_liveConns.end()) return false;
        address = it->second.address;
    }
    if (address.empty()) return false;

    scopeOut = BlockScope(address);

    // BLOCK FIRST, THEN KICK — the same race Ban has. A peer whose socket is
    // closed before the block is recorded reconnects into the gap, and a timed
    // kick that admits the peer it just ejected is worse than none: it looks
    // like it worked.
    Blacklist::AddTimed(scopeOut, minutes, Constants::Messages::BLACKLIST_REASON_TIMED);
    KickConnection(id);
    return true;
}

bool BanConnection(ConnId id, std::wstring &scopeOut) {
    std::wstring address;
    {
        std::lock_guard<std::mutex> lk(g_liveConnMutex);
        auto it = g_liveConns.find(id);
        if (it == g_liveConns.end()) return false;
        address = it->second.address;
    }
    if (address.empty()) return false;

    scopeOut = BlockScope(address);

    // BLACKLIST FIRST, THEN KICK. The other order leaves a gap: the peer's
    // socket closes, it reconnects immediately, and the accept gate has not been
    // told about it yet — so the ban silently fails to take on exactly the peer
    // it was aimed at. Written before the connection is torn down, that race
    // does not exist.
    Blacklist::Add(scopeOut, Constants::Messages::BLACKLIST_REASON_OPERATOR);
    KickConnection(id);
    return true;
}

bool IsEncrypted() {
    if (!g_running.load(std::memory_order_acquire)) return false;
    // The SNAPSHOT's bind address — what the socket was actually opened with.
    return Tls::RequiredForAddress(SnapshotCopy().bindAddress);
}

std::wstring BoundEndpoint() {
    std::lock_guard<std::mutex> lk(g_endpointMutex);
    return g_endpoint;
}

// =============================================================================
// UI-thread execution
// =============================================================================
// The body. Split out so ExecuteOnUiThread can time and log EVERY exit —
// including the refusals, which are the ones you most want a record of — without
// a log call before each of five returns, where the sixth would be forgotten.
static std::wstring ExecuteOnUiThreadBody(HWND hWnd, const RemoteRequest &req, ConnId from) {
    // THE WIRE BOUNDARY. Unconditional — not gated on the session, not on the
    // password, not on the allow-list.
    //
    // A static_assert already proves no file-altering command has a row in the
    // command table, so nothing should ever reach here. That is the point: this
    // is the second lock. If a row is ever added by a route the assert does not
    // see — a hand-built RemoteRequest, a future parser change — the answer must
    // still be no, because the cost of being wrong once is deleted files.
    if (IsNeverRemote(req.cmd))
        return MakeErr(RT::ERR_UNKNOWN_COMMAND, L"not available remotely");

    // Everything below runs as an INBOUND command: it must not be forwarded on
    // to this instance's own targets, and must not be echoed back to the
    // connection that sent it. Without this a master mirroring to a slave that
    // is also observing the master trades one keystroke between them forever.
    // Scoped, so an early return cannot leave the flag set.
    InboundGuard guard(from);

    // The payload-carrying commands go to their headless paths in RemoteExec.
    // They must NOT reach the bare ExecuteCommand, where each raises a panel or
    // a dialog and would hold the client's connection open until somebody
    // dismissed a window on the machine.
    std::wstring reply;
    if (ExecutePayloadCommand(hWnd, req, reply))
        return reply;

    // The shared sink. Keyboard, mouse, tray and panels all funnel through
    // here, so a remote command behaves identically to the same command given
    // locally.
    //
    // The kiosk lock is deliberately NOT consulted: isLocked exists to stop
    // someone at the keyboard of an unattended screen, while a remote caller has
    // already passed the address gates and the password.
    InputManager::ExecuteCommand(hWnd, req.cmd);

    // Report the resulting state, not a bare OK. A driving instance uses this
    // to confirm that what it sent actually took effect — and for the commands
    // that move the playlist, to notice that the two ends have diverged. See
    // InputManager::GetCommandValue.
    std::wstring name;
    if (!NameForCommand(req.cmd, name)) return MakeOk();
    return MakeOk(name + L"=" + InputManager::GetCommandValue(hWnd, req.cmd));
}

std::wstring ExecuteOnUiThread(HWND hWnd, const RemoteRequest &req, ConnId from) {
    // NOT logged here any more. The record moved down to SendLine/RecvLine, the
    // two functions every byte actually passes through — this only sees the
    // commands that got as far as the UI thread, which is a subset, and a log
    // that quietly omits the refusals is a log that misleads.
    return ExecuteOnUiThreadBody(hWnd, req, from);
}

// =============================================================================
// Observers
// =============================================================================

void AddObserver(ConnId conn) {
    const SOCKET s = static_cast<SOCKET>(conn);
    if (s == INVALID_SOCKET) return;

    // Resolved ONCE, when the observer joins. It cannot change while the
    // connection lives, and doing it per emitted event would put a name lookup
    // on the image-change path.
    const bool local = PeerIsSameMachine(s);

    std::lock_guard<std::mutex> lk(g_observerMutex);
    for (const Observer &o : g_observers)
        if (o.sock == s) return; // `observe 1` twice is not two observers
    g_observers.push_back(Observer{s, local, SessionFor(conn)});
    g_observerCount.store(static_cast<int>(g_observers.size()), std::memory_order_release);
}

void RemoveObserver(ConnId conn) {
    const SOCKET s = static_cast<SOCKET>(conn);
    std::lock_guard<std::mutex> lk(g_observerMutex);
    g_observers.erase(std::remove_if(g_observers.begin(), g_observers.end(),
                                     [s](const Observer &o) { return o.sock == s; }),
                      g_observers.end());
    g_observerCount.store(static_cast<int>(g_observers.size()), std::memory_order_release);
}

bool HasObservers() {
    // Lock-free by design — see g_observerCount. This is on the keystroke path.
    return g_observerCount.load(std::memory_order_acquire) > 0;
}

void EmitToObservers(const std::wstring &line, ConnId except, bool positional) {
    // Cheap test first: no lock, and no string built, when nobody is watching.
    if (g_observerCount.load(std::memory_order_acquire) == 0) return;

    const std::string bytes = ToUtf8(std::wstring(RT::RESP_EVENT) + L" " + line) + "\r\n";

    std::lock_guard<std::mutex> lk(g_observerMutex);
    for (const Observer &o : g_observers) {
        const SOCKET s = o.sock;
        if (static_cast<ConnId>(s) == except) continue;

        // A POSITION goes only to an observer that shares this folder. Across
        // machines an index names a different picture, so sending one would
        // jump that viewer somewhere arbitrary — worse than leaving it where it
        // is. Such an observer still receives every ACTION, and follows what is
        // being done rather than what is being shown.
        if (positional && !o.sameMachine) continue;

        // NON-BLOCKING, and dropped rather than retried.
        //
        // This runs on the UI thread, mid-command. A default send() blocks once
        // the peer's receive window fills, so an observer that stopped reading
        // — paused in a debugger, hung, on a saturated link — would freeze the
        // viewer it is watching. That trade is never worth making: the event is
        // a courtesy, the responsiveness is not.
        //
        // One shot, no loop: a partial write would put half a line on the wire
        // and desynchronise that observer's parser for good, so a short count is
        // treated exactly like a refusal.
        int sent = 0;
        if (o.tls) {
            // TLS cannot be written non-blockingly the same way — a record is
            // all-or-nothing, and half of one on the wire is unrecoverable. So
            // the "never stall the UI thread" rule is kept a different way:
            // TrySend gives up immediately if the client thread holds the
            // session, and the socket keeps its blocking mode so a record is
            // never torn. The remaining exposure is a full send buffer on one
            // observer, bounded by the socket's own send timeout.
            sent = o.tls->TrySend(s, bytes.data(), bytes.size())
                       ? static_cast<int>(bytes.size())
                       : 0;
        } else {
            // Same two rules as the TLS branch, reached differently.
            //
            // 1. NEVER WAIT FOR THE CLIENT THREAD. try_lock, and a failure drops
            //    the event — blocking here to take a lock that thread holds for
            //    the length of an image transfer would freeze the viewer.
            //
            // 2. WRITABILITY IS TESTED, NOT IMPOSED. This used to flip the
            //    socket to non-blocking and back around the send. That mode is a
            //    property of the SOCKET, not of the call: the client thread is
            //    sitting in recv() on the same handle, and a recv entering
            //    during the flip returns WSAEWOULDBLOCK instead of blocking. Its
            //    RecvLine reads n <= 0 as "the peer is gone" and tears the
            //    connection down — an observer dropped at random the moment
            //    somebody presses a key was this race. select() answers the same
            //    question without touching state the other thread depends on.
            const std::shared_ptr<std::mutex> lock = SendLockFor(static_cast<ConnId>(s));
            std::unique_lock<std::mutex> guard;
            if (lock) guard = std::unique_lock<std::mutex>(*lock, std::try_to_lock);

            if (!lock || guard.owns_lock()) {
                fd_set wr;
                FD_ZERO(&wr);
                FD_SET(s, &wr);
                timeval zero{0, 0};
                // Writable means the send buffer has room, and one EVENT line
                // fits in whatever is left of 64 KB — which is what lets the
                // all-or-nothing check below stand in for a partial write.
                if (select(0, nullptr, &wr, nullptr, &zero) > 0 && FD_ISSET(s, &wr))
                    sent = send(s, bytes.data(), static_cast<int>(bytes.size()), 0);
            }
        }

        // The OTHER outbound path — this one deliberately bypasses SendLine to
        // stay non-blocking, so it needs its own record or every event this
        // instance pushes to a watcher would be missing from the log. The result
        // is reported honestly: a dropped event is exactly the thing you would
        // open the log to discover.
        if (Log::IsEnabled())
            Log::Add(Log::Direction::Out, Log::SelfLabel(),
                     std::wstring(RT::RESP_EVENT) + L" " + line, PeerLabel(s),
                     sent == static_cast<int>(bytes.size()) ? L"(pushed)"
                                                            : L"(dropped — observer not reading)",
                     -1);
    }
}

} // namespace Remote
