// winsock2.h before anything that pulls windows.h — see the note in
// RemoteServer.cpp. RemoteClient.h includes windows.h.
#include <winsock2.h>
#include <ws2tcpip.h>

#include "RemoteMirror.h"
#include "RemoteClient.h"
#include "RemoteProtocol.h"
#include "RemoteServer.h" // ActiveConnections — the other half of SessionActive
#include "RemotesFile.h"  // SplitStoredSecret — imported credentials
#include "RemoteLog.h"    // Ctrl+F12 — the record of what crossed the wire
#include "RemoteSettings.h" // Config().name, snapshotted for the log's Sender column
#include "RemoteImageXfer.h" // Alt+Enter / Ctrl+Alt+Enter — the image transfer

#include "AppState.h"
#include "Platform/Constants.h"
#include "Platform/ConstantsStrings.h"
#include "Platform/FileHandler.h"      // ReSortPlaylistAndRebuildMap
#include "Overlays/OverlayManager.h"
#include "Persistence/RegistryManager.h"

extern AppState app;
extern OverlayManager g_overlayManager;

#include <atomic>
#include <chrono>
#include <cstdlib>      // _wtoi — parsing the QueryState reply on the sender thread
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

namespace Remote::Mirror {

namespace RT = Constants::RemoteTcpIp;

namespace {

    // What the UI thread hands to a sender thread.
    //
    // `expectFile` is the file name the DRIVING instance landed on. The sender
    // compares it against the one the target names back and reports a mismatch,
    // which it can do without ever touching app.playlist — a socket thread may
    // not read that, and this keeps the comparison off the UI thread entirely.
    // Empty when the command has no position to verify.
    struct QueuedLine {
        std::wstring line;
        std::wstring expectFile;
        // Where to report the answer, or nullptr for "nobody is listening".
        //
        // Only hand-sent commands (Ctrl+F10) set this. A mirrored keystroke does
        // not: nobody reads the reply to "next image", and posting one message
        // per keystroke per target would be a storm on the one path that has to
        // stay cheap.
        HWND replyTo = nullptr;

        // A queued item is EITHER one ready-made line, or a Ctrl+Enter push the
        // sender thread has to negotiate (ask, then send only the lines that turn
        // out to be needed). ONE queue for both, so a push cannot overtake the
        // keystrokes queued ahead of it — they are actions on the same screen and
        // their order is the order the user made them in.
        //
        // LAST in the struct on purpose: PushTo brace-initialises the three
        // fields above, and a new member ahead of them would silently shift that
        // initialisation by one.
        // Line     one ready-made command
        // Position Ctrl+Enter — negotiate folder/sort/index (RunSendPosition)
        // StreamOut Alt+Enter — carry the image's BYTES there (RunStreamOut)
        // StreamIn Ctrl+Alt+Enter — fetch the bytes of what it is showing
        //
        // All four share ONE queue, so a transfer cannot overtake the keystrokes
        // queued ahead of it: they are actions on the same screen, in the order the
        // user made them.
        enum class Kind { Line, Position, StreamOut, StreamIn };
        Kind        kind = Kind::Line;
        PushRequest push;   // meaningful only when kind == Position
        // The file to stream out. Read on the SENDER thread, not the UI thread: a
        // 20 MB file read is not something to do on the thread that paints.
        std::wstring streamPath;
    };

    // One driven instance.
    //
    // OWNERSHIP: `client` belongs to the sender thread and to nothing else. The
    // UI thread only ever touches the queue (under `mtx`) and the atomics. That
    // division is what lets the UI thread mirror a keystroke without any chance
    // of blocking on a socket.
    struct Target {
        int          id = 0;
        std::wstring name;
        std::wstring host;
        int          port = 0;
        std::wstring password;
        std::wstring exePath;
        // TLS certificate fingerprint to pin. Empty for a loopback target, which
        // speaks plaintext and presents nothing to check.
        std::wstring pin;

        // Resolved when the target is added and re-resolved on demand — never
        // per keystroke, which would put a DNS lookup in the mirror path.
        //
        // ATOMIC because the re-resolve runs on the sender thread (DNS blocks,
        // and the UI thread must not) while the UI thread reads it to pick a
        // command set. One acquire load per broadcast — the same cost class as
        // the other flags on this hot path.
        //
        // Same machine  → the full command set, positions included: both ends
        //                 see the same files, so an index means the same picture.
        // Other machine → the portable subset. No indices, no paths, no folder.
        //                 That instance becomes a PARALLEL viewer running the
        //                 same actions over its own content, rather than a
        //                 mirror of this screen — which is the most that can
        //                 honestly be delivered when the folders differ.
        std::atomic<bool> sameMachine{false};

        // Set by the UI thread, serviced by the sender thread: "re-resolve your
        // host". The answer CAN change while the process runs — DHCP hands out a
        // new address, a laptop moves between networks, a name starts resolving
        // somewhere else — and a stale answer sends the wrong command set or
        // greys out a start button that would work.
        std::atomic<bool> resolveWanted{false};

        Client client;                      // sender thread only

        std::mutex               mtx;       // guards queue + lastError
        std::condition_variable  cv;
        std::deque<QueuedLine>   queue;
        std::wstring             lastError;

        std::atomic<bool>      stop{false};
        // Listed and idle vs listed and dialling. A saved list describes screens
        // that may be switched off, not yet built, or simply not wanted right
        // now — so being in the list cannot imply being connected to.
        std::atomic<bool>      wantConnect{true};
        // Does F11 drive THIS one? Connected and mirrored are different questions:
        // a screen can be joined so it can be polled, started, stopped or watched
        // from the console while the keystrokes go somewhere else. Session state,
        // deliberately not persisted — a narrowed selection is about what is on
        // the desk right now, and a saved one would silently leave screens out of
        // a later session for a reason nobody remembers.
        std::atomic<bool>      mirroring{true};
        std::atomic<bool>      connected{false};
        std::atomic<Down>      down{Down::None};
        std::atomic<bool>      observing{false};
        std::atomic<long long> lagUs{-1};

        std::thread th;
    };

    // UI thread only. unique_ptr because Target holds a thread, a mutex and a
    // condition_variable — none of which can be moved, so the vector must not
    // reallocate the objects themselves.
    std::vector<std::unique_ptr<Target>> g_targets;
    int  g_nextId = 1;
    HWND g_owner  = nullptr;

    // How many targets are CONNECTED right now — not how many are configured.
    //
    // The distinction decides two things that both matter. A copy that merely
    // has a qivRemoteServers.ini has targets from the moment it starts, and if that
    // counted as a session it would sit in restricted mode — delete and Find
    // refused — forever, whether or not anything ever answered. And a slave that
    // is switched off after ten minutes leaves its target configured (the sender
    // thread keeps retrying, which is correct) while nothing is actually joined
    // any more.
    //
    // So: configured is not connected. Restrictions and the mirror gate both
    // hang off THIS, and both fall away the moment the last link drops.
    //
    // Written by sender threads on every connect/disconnect, read on the
    // keystroke path — hence an atomic rather than a walk over g_targets, which
    // would also need a lock the UI thread does not otherwise take.
    std::atomic<int> g_connectedCount{0};

    // Keeps the counter in step with a target's connected flag. The two must
    // move together or the count drifts: a thread that reconnected without
    // publishing it would leave the viewer unrestricted while it was driving.
    // Where the F10 console listens, and its coalescing gate. Outside any mutex
    // because the producers are sender threads and the point of the gate is that
    // they never serialise on it.
    // SEVERAL panels, not one. It was a single HWND, which meant whichever of the
    // F10 console and the Ctrl+F10 Send Command panel registered last silently
    // stole the other's notifications — and the loser had to poll on a timer to
    // find out that a target had connected.
    //
    // A fixed array of slots rather than a vector: the producers are SENDER THREADS
    // on a per-connection path, so this must be readable with no lock and no
    // allocation. Two panels subscribe today; four slots means neither a
    // reallocation nor a rule about who may subscribe.
    //
    // EACH SLOT HAS ITS OWN GATE. One coalescing flag for all of them would let a
    // panel that is mid-rebuild suppress the message the other one has not had yet.
    struct PanelNotify {
        std::atomic<HWND> hwnd{nullptr};
        std::atomic<bool> pending{false};
    };
    constexpr size_t PANEL_NOTIFY_SLOTS = 4;
    PanelNotify g_panelNotify[PANEL_NOTIFY_SLOTS];

    // Tell every subscribed panel that a CONNECTION changed. Cheap and silent when
    // none is open, which is the usual case: four acquire loads and no branch taken.
    void NotifyTargetsChanged() {
        for (PanelNotify &p : g_panelNotify) {
            HWND hwnd = p.hwnd.load(std::memory_order_acquire);
            if (!hwnd) continue;
            // Already told and not yet acknowledged — a batch of targets coming up
            // together costs one message per panel, not one per target.
            if (p.pending.exchange(true, std::memory_order_acq_rel)) continue;
            if (!PostMessageW(hwnd, Constants::WM_QIV_REMOTE_TARGETS_CHANGED, 0, 0))
                p.pending.store(false, std::memory_order_release);
        }
    }

    void SetConnected(Target &t, bool up) {
        const bool was = t.connected.exchange(up, std::memory_order_acq_rel);
        if (was == up) return;
        g_connectedCount.fetch_add(up ? 1 : -1, std::memory_order_acq_rel);
        // Only reached on a REAL transition — the early return above is the
        // filter, so a reconnect loop that keeps failing does not spam this.
        NotifyTargetsChanged();
    }

    // The other half of "connection state": WHY a row is down. Exchanged rather
    // than stored so an unchanged reason does not repaint — a target that is
    // switched off re-classifies as Offline on every retry.
    void SetDown(Target &t, Down d) {
        if (t.down.exchange(d, std::memory_order_acq_rel) != d) NotifyTargetsChanged();
    }

    // --- helpers ------------------------------------------------------------

    // Is `host` this very machine?
    //
    // NOT a string comparison against "127.0.0.1". A slave running on this box
    // is commonly addressed by its LAN address or its computer name, and
    // treating that as remote would needlessly drop it to the reduced command
    // set — for two instances that share a filesystem and would mirror
    // perfectly.
    //
    // So: resolve the target, resolve our own name, and look for an address in
    // common. Loopback short-circuits. Deliberately uses getaddrinfo rather than
    // GetAdaptersAddresses so no new library has to be linked; the own-name
    // lookup covers every address the machine answers to.
    bool IsSameMachine(const std::wstring &host) {
        if (host.empty()) return false;

        // getaddrinfo is a WINSOCK call and fails with WSANOTINITIALISED (10093)
        // if nothing in this process has called WSAStartup yet. This runs from
        // AddTarget on the UI thread, which happens at startup and on every
        // panel open — long before the F9 server binds or a sender thread dials,
        // so on a standalone viewer Winsock is usually DOWN right here. The
        // failure looked exactly like "not this machine", which marked every
        // local instance remote and disabled its start button.
        //
        // Winsock refcounts, so a balanced local reference is correct whether or
        // not the server already holds one.
        struct WsaScope {
            bool up = false;
            WsaScope()  { WSADATA d{}; up = (WSAStartup(MAKEWORD(2, 2), &d) == 0); }
            ~WsaScope() { if (up) WSACleanup(); }
        } wsa;
        if (!wsa.up) return false;

        const std::string node = ToUtf8(host);

        addrinfo hints{};
        hints.ai_family   = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        addrinfo *them = nullptr;
        if (getaddrinfo(node.c_str(), nullptr, &hints, &them) != 0 || !them)
            return false;

        // 1. Loopback in any family — unambiguous, and the common case.
        for (addrinfo *a = them; a; a = a->ai_next) {
            if (a->ai_family == AF_INET) {
                const auto *s4 = reinterpret_cast<const sockaddr_in *>(a->ai_addr);
                if ((ntohl(s4->sin_addr.s_addr) >> 24) == 127) { freeaddrinfo(them); return true; }
            } else if (a->ai_family == AF_INET6) {
                const auto *s6 = reinterpret_cast<const sockaddr_in6 *>(a->ai_addr);
                if (IN6_IS_ADDR_LOOPBACK(&s6->sin6_addr)) { freeaddrinfo(them); return true; }
            }
        }

        // 2. Any address this machine itself answers to.
        char self[256] = {};
        if (gethostname(self, sizeof(self)) != 0) { freeaddrinfo(them); return false; }

        addrinfo *mine = nullptr;
        if (getaddrinfo(self, nullptr, &hints, &mine) != 0 || !mine) {
            freeaddrinfo(them);
            return false;
        }

        // Compare the ADDRESS BYTES only. memcmp over the whole sockaddr also
        // compared sin_port (harmless, both zero here) and — the one that bit —
        // sin6_scope_id and sin6_flowinfo: the same IPv6 address resolved from a
        // literal carries scope 0 while the one resolved from our own host name
        // carries the interface index, so two spellings of one address never
        // matched.
        auto addrBytes = [](const addrinfo *a, int &len) -> const void * {
            if (a->ai_family == AF_INET) {
                len = sizeof(in_addr);
                return &reinterpret_cast<const sockaddr_in *>(a->ai_addr)->sin_addr;
            }
            if (a->ai_family == AF_INET6) {
                len = sizeof(in6_addr);
                return &reinterpret_cast<const sockaddr_in6 *>(a->ai_addr)->sin6_addr;
            }
            len = 0;
            return nullptr;
        };

        bool same = false;
        for (addrinfo *a = them; a && !same; a = a->ai_next) {
            int la = 0;
            const void *pa = addrBytes(a, la);
            if (!pa) continue;
            for (addrinfo *b = mine; b; b = b->ai_next) {
                if (a->ai_family != b->ai_family) continue;
                int lb = 0;
                const void *pb = addrBytes(b, lb);
                if (!pb || la != lb) continue;
                if (memcmp(pa, pb, static_cast<size_t>(la)) == 0) {
                    same = true;
                    break;
                }
            }
        }

        freeaddrinfo(mine);
        freeaddrinfo(them);
        return same;
    }

    long long NowUs() {
        LARGE_INTEGER f, c;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&c);
        if (f.QuadPart == 0) return 0;
        return (c.QuadPart * 1000000LL) / f.QuadPart;
    }

    // Pulls the file name out of an "OK goto=47/238 IMG_0042.jpg" reply.
    // Empty when the reply carries no name, which is every command that does
    // not move the playlist position.
    std::wstring FileNameFromReply(const std::wstring &reply) {
        const size_t sp = reply.find_last_of(L' ');
        if (sp == std::wstring::npos) return {};
        const std::wstring tail = reply.substr(sp + 1);
        // A position with no name ("47/238") is not a file name.
        return tail.find(L'/') == std::wstring::npos ? tail : std::wstring{};
    }

    void SetError(Target &t, const std::wstring &err) {
        std::lock_guard<std::mutex> lk(t.mtx);
        t.lastError = err;
    }

    // Which kind of failure, from the message Client::Connect produced.
    //
    // Matching on the message rather than a status code because those messages
    // are already the single place each failure is described (ConstantsStrings),
    // and inventing a parallel code enum beside them would be one more pair to
    // keep in step. The comparison is against the constants themselves, not
    // against literal text, so rewording a message cannot silently break this.
    Down Classify(const std::wstring &err) {
        namespace M = Constants::Messages;
        if (err == M::REMOTE_CLIENT_RESOLVE_FAILED)   return Down::Address;
        if (err == M::REMOTE_CLIENT_CONNECT_FAILED)   return Down::Offline;
        if (err == M::REMOTE_CLIENT_NO_BANNER)        return Down::Rejected;
        if (err == M::REMOTE_CLIENT_AUTH_FAILED ||
            err == M::REMOTE_CLIENT_PASSWORD_REQUIRED) return Down::Auth;
        if (err == M::REMOTE_CLIENT_PROTOCOL_ERROR)   return Down::Protocol;
        // Send/receive failures on an established link mean it went away
        // mid-session, which is the same situation as never having answered.
        return Down::Offline;
    }

    // =========================================================================
    // Ctrl+Enter push — the negotiation (sender thread only)
    // =========================================================================

    // Disk order, as CommandExecuter's SortByDisk case writes it. Named here
    // because the push has to recognise it: it is the one order that is NOT
    // reproducible on another drive, so an index taken from it means nothing at
    // the far end — and the command that would set it is refused there anyway
    // while a session is live (SESSION_BLOCKED).
    constexpr int SORT_ORDER_DISK = 4;

    // What the far end answered to `QueryState`.
    struct FarState {
        bool valid = false;          // did it understand the question at all?
        int  count = 0;              // playlist length
        int  index = 0;              // 1-based current position, 0 = nothing loaded
        int  sortOrder = -1;
        bool sortRev = false;
        std::wstring fileName;       // what it is showing
        std::wstring folder;         // the folder that picture lives in
    };

    // Trailing-separator- and case-insensitive path compare. Two instances reach
    // the same folder by different spellings all the time — one opened
    // `D:\Pics`, the other `d:\pics\` — and treating those as different would
    // rescan a folder the far end is already sitting in.
    bool SameFolder(const std::wstring &a, const std::wstring &b) {
        if (a.empty() || b.empty()) return false;
        auto trim = [](const std::wstring &s) {
            size_t n = s.size();
            while (n > 1 && (s[n - 1] == L'\\' || s[n - 1] == L'/')) --n;
            return s.substr(0, n);
        };
        const std::wstring ta = trim(a), tb = trim(b);
        return _wcsicmp(ta.c_str(), tb.c_str()) == 0;
    }

    // "OK QueryState=count=238;index=47;sort=0;sortrev=0;name=A.jpg;folder=D:\p"
    //
    // Anything that is not that — an ERR, or an OK from a build that predates the
    // command — leaves `valid` false, and the caller falls back to the form that
    // needs no agreement at all (a full path). Unknown keys are ignored, the same
    // rule `sync` follows in both directions.
    FarState ParseQueryState(const std::wstring &reply) {
        FarState st;
        const size_t eq = reply.find(L'=');
        if (_wcsnicmp(reply.c_str(), RT::RESP_OK, 2) != 0 || eq == std::wstring::npos)
            return st;

        const std::wstring body = reply.substr(eq + 1);
        size_t start = 0;
        while (start <= body.size()) {
            const size_t semi = body.find(L';', start);
            const std::wstring tok = body.substr(
                start, semi == std::wstring::npos ? std::wstring::npos : semi - start);
            start = (semi == std::wstring::npos) ? body.size() + 1 : semi + 1;
            if (tok.empty()) continue;

            const size_t e = tok.find(L'=');
            if (e == std::wstring::npos) continue;
            const std::wstring k = tok.substr(0, e);
            const std::wstring v = tok.substr(e + 1);

            // A path or a file name containing ';' truncates its own value and
            // nothing after it, because those two keys are sent LAST. The result
            // is a folder that compares unequal — which costs an extra rescan and
            // is still correct, rather than a wrong index.
            if      (k == L"count")   st.count     = _wtoi(v.c_str());
            else if (k == L"index")   st.index     = _wtoi(v.c_str());
            else if (k == L"sort")    st.sortOrder = _wtoi(v.c_str());
            else if (k == L"sortrev") st.sortRev   = (_wtoi(v.c_str()) != 0);
            else if (k == L"name")    st.fileName  = v;
            else if (k == L"folder")  st.folder    = v;
        }

        // `sort` is the one key that cannot be defaulted: without it there is no
        // way to know whether the orders agree, and guessing "they do" is the
        // mistake that puts a wrong picture on the screen.
        st.valid = (st.sortOrder >= 0);
        return st;
    }

    // The file the far end landed on, from "OK JumpToImage=47/238 my photo.jpg".
    //
    // The WHOLE remainder after the position, not the last token — file names
    // contain spaces, and taking the last word would make "my photo.jpg" compare
    // as "photo.jpg" and send every push down the repair path. FileNameFromReply
    // (the desync check) takes the last token on purpose, because it also has to
    // read replies that carry no position at all; this one knows there is one.
    std::wstring LandedName(const std::wstring &reply) {
        const size_t eq = reply.find(L'=');
        if (eq == std::wstring::npos) return {};
        const size_t sp = reply.find(L' ', eq);
        if (sp == std::wstring::npos) return {};
        return reply.substr(sp + 1);
    }

    // The command that SETS a given sort order, by the numbering
    // CommandExecuter's SortBy* cases write into fileHandlerDefaultSortOrder.
    // The wire spelling comes from NameForCommand so there is one source for it.
    bool SortCommandFor(int order, std::wstring &name) {
        switch (order) {
            case 0:  return NameForCommand(Command::SortByName, name);
            case 1:  return NameForCommand(Command::SortByDate, name);
            case 2:  return NameForCommand(Command::SortBySize, name);
            case 3:  return NameForCommand(Command::SortByType, name);
            default: return false;  // 4 = disk, and anything a newer build adds
        }
    }

    void PostEventLine(const std::wstring &line) {
        if (!g_owner) return;
        PostMessageW(g_owner, Constants::WM_QIV_REMOTE_EVENT, 0,
                     reinterpret_cast<LPARAM>(new std::wstring(line)));
    }

    // Bounded pause between `QueryState` polls while the far end's scan runs.
    // On the cv rather than a bare sleep, so Shutdown and Disconnect still stop
    // this thread at once instead of at the end of the current wait.
    bool PushPause(Target &t) {
        std::unique_lock<std::mutex> lk(t.mtx);
        t.cv.wait_for(lk, std::chrono::milliseconds(RT::PUSH_SETTLE_MS), [&t] {
            return t.stop.load(std::memory_order_acquire) ||
                   !t.wantConnect.load(std::memory_order_acquire);
        });
        return !t.stop.load(std::memory_order_acquire) &&
               t.wantConnect.load(std::memory_order_acquire);
    }

    // Ctrl+Enter, for ONE target. Runs the steps described in RemoteMirror.h.
    //
    // Returns false only for a CONNECTION fault, which the caller then treats
    // exactly like a failed ordinary send (mark down, classify, reconnect). A far
    // end that answers ERR to a step is not a connection fault: the push does
    // what it can and stops, because retrying a refusal gets the same refusal.
    bool RunSendPosition(Target &t, const PushRequest &p, std::wstring &err,
                 std::vector<std::wstring> *events) {
        std::wstring reply;

        // ── 1. What is it doing right now? ──────────────────────────────────
        if (!t.client.Send(L"QueryState", reply, err, events)) return false;
        // NOT named `far`: windef.h still defines `far` and `near` as empty macros
        // for 16-bit compatibility, so that identifier vanishes at preprocess time.
        const FarState there = ParseQueryState(reply);

        // ── The path-only form ──────────────────────────────────────────────
        // Used when an index cannot be trusted at all: the far end did not
        // understand the query (an older build), or this viewer is sorted by
        // DISK order, which is a property of the drive and reproduces nowhere.
        //
        // `OpenFile <full path>` is the whole answer in one line — it opens the
        // folder and lands on that file through the far end's own scan, so there
        // is no index and nothing to race.
        if (!there.valid || p.sortOrder == SORT_ORDER_DISK)
            return t.client.Send(L"OpenFile " + p.imagePath, reply, err, events);

        const bool folderMatches = SameFolder(there.folder, p.folder);
        const bool orderMatches  = (there.sortOrder == p.sortOrder) &&
                                   (there.sortRev   == p.sortRev);

        // ── 2. Already in the right folder, in the right order ──────────────
        // One line, no rescan. This is the case that makes Ctrl+Enter usable as
        // a repeated act — walk the folder here, push each picture as you reach
        // it, and the other screen never rebuilds its playlist.
        if (folderMatches && orderMatches && there.count >= p.index) {
            if (!t.client.Send(L"JumpToImage " + std::to_wstring(p.index), reply, err, events))
                return false;
            // Landed somewhere else? Then the two playlists hold different files
            // — one end has a picture the other does not — and every index from
            // here on is off by the difference. Repaired by naming the file
            // outright, which needs no agreement about order or count.
            const std::wstring got = LandedName(reply);
            if (!got.empty() && !p.fileName.empty() &&
                _wcsicmp(got.c_str(), p.fileName.c_str()) != 0)
                return t.client.Send(L"OpenFile " + p.imagePath, reply, err, events);
            return true;
        }

        // ── 3. Send only what differs, index LAST ───────────────────────────
        if (!folderMatches) {
            if (!t.client.Send(L"OpenFile " + p.folder, reply, err, events)) return false;
        }

        if (!orderMatches) {
            // SortBy* are TOGGLES, not setters: the command for the order you are
            // ALREADY in flips ascending/descending instead. So the presses are
            // computed from the state observed in step 1 — one press to move to a
            // different order (which lands ascending), a second only if reverse
            // is wanted; a single flip when the order is right and the direction
            // is not.
            std::wstring sortCmd;
            if (SortCommandFor(p.sortOrder, sortCmd)) {
                if (there.sortOrder != p.sortOrder) {
                    if (!t.client.Send(sortCmd, reply, err, events)) return false;
                    if (p.sortRev && !t.client.Send(sortCmd, reply, err, events)) return false;
                } else if (there.sortRev != p.sortRev) {
                    if (!t.client.Send(sortCmd, reply, err, events)) return false;
                }
            }
        }

        // ── 4. Wait for the far end's playlist to exist ─────────────────────
        bool settled;
        if (folderMatches) {
            // Nothing is rescanning: only the order changed, and a sort command
            // re-sorts and answers on the far end's UI thread, so its list is
            // already in the new order by the time that reply arrives.
            //
            // A list SHORTER than the index is therefore not a timing problem —
            // the two ends hold different files, and no amount of waiting fixes
            // that. It falls through to the path form below.
            settled = (there.count >= p.index);
        } else {
            // Opening a folder there starts an ASYNC scan and answers
            // immediately. An index sent into that gap indexes the OLD playlist.
            // So ask until the folder it reports is the one it was told to open
            // and its list is long enough — bounded, because an unbounded wait is
            // a hung sender thread. See PUSH_SETTLE_* in Constants.h.
            settled = false;
            for (int i = 0; !settled && i < RT::PUSH_SETTLE_TRIES; ++i) {
                if (!PushPause(t)) return true;  // asked to stop or disconnect
                if (!t.client.Send(L"QueryState", reply, err, events)) return false;
                const FarState now = ParseQueryState(reply);
                settled = now.valid && SameFolder(now.folder, p.folder) &&
                          now.count >= p.index;
            }
        }

        // ── 5. The index, or the path if the wait ran out ───────────────────
        // A far end still not settled is not necessarily broken — a very large
        // folder simply takes longer than the budget. Naming the file outright
        // still lands correctly there, and its own scan finishes underneath it.
        if (!settled)
            return t.client.Send(L"OpenFile " + p.imagePath, reply, err, events);

        if (!t.client.Send(L"JumpToImage " + std::to_wstring(p.index), reply, err, events))
            return false;

        const std::wstring got = LandedName(reply);
        if (!got.empty() && !p.fileName.empty() &&
            _wcsicmp(got.c_str(), p.fileName.c_str()) != 0)
            return t.client.Send(L"OpenFile " + p.imagePath, reply, err, events);
        return true;
    }

    // =========================================================================
    // Streaming a picture, out and in (sender thread only)
    //
    // The image's own FILE BYTES travel — never a path — so both of these work to
    // an instance on another machine, or to a phone. See RemoteImageXfer.h.
    // =========================================================================

    // Alt+Enter, for ONE target: StreamImageBegin, the chunks, StreamImageShow.
    //
    // Returns false only for a CONNECTION fault. A far end that answers ERR to a
    // step is a refusal, not a broken link: the sequence stops there, because every
    // later chunk would be refused for the same reason.
    bool RunStreamOut(Target &t, const std::wstring &imagePath, std::wstring &err,
                      std::vector<std::wstring> *events) {
        // CAPABILITY FIRST. A v1 instance buffers 4 KB per line and drops the
        // connection on a chunk it cannot hold — so sending one would report itself
        // as "the link died", which points the reader at the network instead of at
        // the version. Refused here with the actual reason, and the connection is
        // left alone.
        if (t.client.PeerProtocol() < 2) {
            err = Constants::Messages::STREAM_ERR_PEER_TOO_OLD;
            return true;
        }

        std::vector<unsigned char> bytes;
        if (!Xfer::ReadImageFile(imagePath, bytes, err)) {
            // OUR file, not the connection's fault — reported through err so the
            // console footer says so, and the target is left alone.
            return true;
        }

        const std::vector<std::wstring> lines =
            Xfer::BuildStreamCommands(imagePath, bytes);

        std::wstring reply;
        for (const std::wstring &line : lines) {
            if (!t.client.Send(line, reply, err, events)) return false;
            // ERR from the far end: out of space, too large for its ceiling, or it
            // does not know these commands at all (an older build). Stopping is
            // right, and StreamImageBegin at the far end is what clears the partial
            // transfer — there is nothing to tidy from here.
            if (_wcsnicmp(reply.c_str(), RT::RESP_ERR, 3) == 0) return true;
        }
        return true;
    }

    // Ctrl+Alt+Enter, for ONE target: ask what it is displaying, decode the answer,
    // write it to a temp file HERE, and hand the UI thread the path.
    //
    // The decode and the file write happen on this thread on purpose — several
    // megabytes of base64 is not work for the thread that paints. The UI thread
    // gets one message carrying one path.
    bool RunStreamIn(Target &t, std::wstring &err, std::vector<std::wstring> *events) {
        // Same gate, other direction: a v1 instance does not know the command, and
        // its ERR would be reported as "showing nothing" — which is a different fact.
        if (t.client.PeerProtocol() < 2) {
            err = Constants::Messages::STREAM_ERR_PEER_TOO_OLD;
            if (g_owner) {
                // The UI thread is waiting for an answer either way; an empty path is
                // how it is told there is nothing to show.
                auto *p = new std::wstring();
                if (!PostMessageW(g_owner, Constants::WM_QIV_REMOTE_PULLED, 0,
                                  reinterpret_cast<LPARAM>(p)))
                    delete p;
            }
            return true;
        }

        std::wstring reply;
        if (!t.client.Send(L"SendDisplayedImage", reply, err, events)) return false;

        // Refused, or showing nothing: an EMPTY path tells the UI thread which
        // message to put up. Reported rather than swallowed — the user pressed a
        // key and is owed an answer either way.
        std::wstring tempPath;
        std::vector<unsigned char> bytes;
        if (_wcsnicmp(reply.c_str(), RT::RESP_ERR, 3) != 0 &&
            Xfer::ParseDataReply(reply, bytes)) {
            std::wstring name = Xfer::FileNameFromDataReply(reply);
            if (name.empty()) name = L"remote.img";  // extension picks the decoder
            (void) Xfer::WriteTempImage(name, bytes, tempPath);
        }

        if (g_owner) {
            auto *p = new std::wstring(tempPath);
            if (!PostMessageW(g_owner, Constants::WM_QIV_REMOTE_PULLED, 0,
                              reinterpret_cast<LPARAM>(p)))
                delete p;   // the window went away between ask and answer
        }
        return true;
    }

    // --- the sender thread --------------------------------------------------
    //
    // The ONLY thread that touches t.client. Connects, drains the queue, and
    // while idle listens for the unsolicited EVENT lines an observed target
    // pushes.
    void SenderLoop(Target *t) {
        while (!t->stop.load(std::memory_order_acquire)) {

            // ── Re-resolve "is this host us?" ───────────────────────────────
            // HERE, not on the UI thread: getaddrinfo blocks for as long as DNS
            // takes, and this thread is the one already allowed to wait on the
            // network for this target. `t->host` is written once in AddTarget
            // and never mutated, so reading it unlocked is safe; RemoveTarget
            // joins this thread before destroying the Target, so `t` cannot
            // dangle underneath us.
            if (t->resolveWanted.exchange(false, std::memory_order_acq_rel))
                t->sameMachine.store(IsSameMachine(t->host), std::memory_order_release);

            // ── Idle: listed, but not asked to connect ──────────────────────
            // Park rather than dial. Nothing is queued to a target in this
            // state (PushTo drops it), so there is no backlog to replay when it
            // is switched on.
            if (!t->wantConnect.load(std::memory_order_acquire)) {
                if (t->client.IsConnected()) {
                    t->client.Disconnect();
                    SetConnected(*t, false);
                }
                SetDown(*t, Down::None);
                SetError(*t, {});

                std::unique_lock<std::mutex> lk(t->mtx);
                t->cv.wait(lk, [t] {
                    return t->stop.load(std::memory_order_acquire) ||
                           t->wantConnect.load(std::memory_order_acquire) ||
                           // A listed-but-idle row still shows a start button,
                           // so it still has to notice the address moving.
                           t->resolveWanted.load(std::memory_order_acquire);
                });
                continue;
            }

            // ── Connect / reconnect ─────────────────────────────────────────
            if (!t->client.IsConnected()) {
                // The password field carries EITHER a typed password OR an
                // already-derived secret imported from the target's own settings
                // file. Both end at the same shared value; the marker is what
                // says which route to take, rather than guessing from the shape
                // of the text.
                std::wstring err;
                std::wstring saltHex, digestHex;
                const bool ok = SplitStoredSecret(t->password, saltHex, digestHex)
                    ? t->client.ConnectWithSecret(t->host, t->port, digestHex, saltHex, err)
                    : t->client.Connect(t->host, t->port, t->password, err);

                if (!ok) {
                    SetConnected(*t, false);
                    SetDown(*t, Classify(err));
                    SetError(*t, err);

                    // Back off rather than spin: a slave that is switched off is
                    // an ordinary state, not an error to retry as fast as the
                    // CPU allows. Waiting on the cv (not sleeping) so that
                    // Shutdown still stops this thread promptly.
                    std::unique_lock<std::mutex> lk(t->mtx);
                    t->cv.wait_for(lk, std::chrono::milliseconds(RT::MIRROR_RECONNECT_MS),
                                   [t] {
                                       return t->stop.load(std::memory_order_acquire) ||
                                              // Pressing Disconnect on a row that
                                              // is still retrying should stop the
                                              // retrying NOW, not at the end of
                                              // the current back-off.
                                              !t->wantConnect.load(std::memory_order_acquire);
                                   });
                    continue;
                }
                SetConnected(*t, true);
                SetDown(*t, Down::None);
                SetError(*t, {});

                // Re-arm observation across a reconnect. The far end drops its
                // observer entry when the connection dies, so a target that was
                // being watched before a restart would silently stop reporting.
                if (t->observing.load(std::memory_order_acquire)) {
                    std::wstring reply, err2;
                    (void) t->client.Send(L"Observe 1", reply, err2);
                }
            }

            // ── Take one queued line ────────────────────────────────────────
            QueuedLine item;
            bool have = false;
            {
                std::unique_lock<std::mutex> lk(t->mtx);
                if (!t->queue.empty()) {
                    item = std::move(t->queue.front());
                    t->queue.pop_front();
                    have = true;
                } else if (!t->observing.load(std::memory_order_acquire)) {
                    // Nothing queued and nobody to listen to: block until there
                    // IS something. A cv wait wakes the instant the UI thread
                    // pushes, so an ordinary mirrored keystroke goes out with no
                    // added latency at all.
                    t->cv.wait(lk, [t] {
                        return t->stop.load(std::memory_order_acquire) ||
                               !t->queue.empty() ||
                               // DISCONNECT. Without this the thread parks here
                               // holding a live socket and never looks at
                               // wantConnect again: SetConnecting(id, false)
                               // notifies, the predicate is re-evaluated, none
                               // of the other terms is true, and it goes
                               // straight back to sleep still connected. The
                               // idle branch at the top of the loop is the only
                               // thing that tears the socket down, and it is
                               // unreachable until something else wakes this.
                               !t->wantConnect.load(std::memory_order_acquire) ||
                               t->resolveWanted.load(std::memory_order_acquire);
                    });
                    continue;
                }
            }

            if (have) {
                std::wstring reply, err;
                std::vector<std::wstring> events;

                const long long t0 = NowUs();
                // A push is SEVERAL exchanges, negotiated here rather than by the
                // UI thread — this is the only thread allowed to wait for a
                // reply. It reads the same event sink, so an observed target's
                // EVENT lines arriving mid-negotiation are not lost.
                bool ok = false;
                switch (item.kind) {
                    case QueuedLine::Kind::Position:
                        ok = RunSendPosition(*t, item.push, err, &events);
                        break;
                    case QueuedLine::Kind::StreamOut:
                        ok = RunStreamOut(*t, item.streamPath, err, &events);
                        break;
                    case QueuedLine::Kind::StreamIn:
                        ok = RunStreamIn(*t, err, &events);
                        break;
                    case QueuedLine::Kind::Line:
                        ok = t->client.Send(item.line, reply, err, &events);
                        break;
                }
                const long long t1 = NowUs();

                // NOT logged here. The record lives inside Client::Send, at the
                // wire boundary — logging at call sites meant every other path
                // that sent a line (the observe re-arm, the handshake, the
                // standalone ping, the whole inbound EVENT stream) was silently
                // missing from a log that looked complete. See RemoteClient.h.

                // Somebody typed this one and is waiting for the answer. Posted,
                // not called: this is a sender thread, and the panel that reads
                // it lives on the UI thread. Ownership passes with the pointer.
                if (item.replyTo) {
                    auto *r = new CmdReply{t->name, item.line, ok ? reply : err,
                                           ok, t1 - t0};
                    if (!PostMessageW(item.replyTo, Constants::WM_QIV_REMOTE_CMD_REPLY,
                                      0, reinterpret_cast<LPARAM>(r)))
                        delete r;   // the panel closed between send and answer
                }

                if (!ok) {
                    SetConnected(*t, false);
                    SetDown(*t, Classify(err));
                    SetError(*t, err);
                } else if (!err.empty()) {
                    // The link is fine and the JOB failed — a stream whose source
                    // file could not be read, for instance. Recorded so the F10
                    // console's footer can say what happened, without marking a
                    // healthy target as down.
                    SetError(*t, err);
                } else {
                    t->lagUs.store(t1 - t0, std::memory_order_release);

                    // Divergence check. We sent an INDEX; the target named the
                    // file it actually landed on. Different names mean the two
                    // playlists no longer line up — same sort order, different
                    // file set — and every further index would land wrong.
                    if (!item.expectFile.empty()) {
                        const std::wstring got = FileNameFromReply(reply);
                        if (!got.empty() && _wcsicmp(got.c_str(), item.expectFile.c_str()) != 0) {
                            // Only the UI thread may read app.playlist to build
                            // the repair, so ask it to.
                            if (g_owner)
                                PostMessageW(g_owner, Constants::WM_QIV_REMOTE_DESYNC,
                                             static_cast<WPARAM>(t->id), 0);
                        }
                    }
                }

                // Events that arrived interleaved with our exchange.
                for (const std::wstring &e : events) PostEventLine(e);
                continue;
            }

            // ── Idle, and observing: listen ─────────────────────────────────
            // An observed target talks when IT acts, so somebody has to be
            // reading the socket even with nothing to send. Bounded, so the
            // queue and the stop flag are still re-checked several times a second.
            std::wstring line;
            if (t->client.PollLine(line, RT::MIRROR_IDLE_POLL_MS)) {
                if (_wcsnicmp(line.c_str(), RT::RESP_EVENT, 5) == 0)
                    PostEventLine(line);
                // Anything else on an idle connection is a stray reply to a
                // request that already timed out. Dropping it is correct — it
                // answers a question nobody is still asking.
            }
        }

        t->client.Disconnect();
        SetConnected(*t, false);
    }

    // Queue one line to one target. UI thread. Never blocks.
    void PushTo(Target &t, const std::wstring &line, const std::wstring &expectFile,
                HWND replyTo = nullptr) {
        // A listed-but-idle target is not being driven. Queuing for it would
        // build a backlog that replayed the moment somebody pressed Connect —
        // minutes of stale navigation arriving at once.
        if (!t.wantConnect.load(std::memory_order_acquire)) return;
        {
            std::lock_guard<std::mutex> lk(t.mtx);
            // Drop the OLDEST on overflow. A backlog for a machine that stopped
            // answering would replay stale navigation when it came back, and a
            // mirrored keystroke is only meaningful at the moment it is made.
            while (t.queue.size() >= RT::MIRROR_QUEUE_MAX) t.queue.pop_front();
            t.queue.push_back(QueuedLine{line, expectFile, replyTo});
        }
        t.cv.notify_one();
    }

    // Queue an already-built job (a Ctrl+Enter push). Same guards as PushTo —
    // idle targets are skipped, and the oldest goes on overflow — so a push
    // cannot build a backlog for a screen that is switched off either.
    void PushJobTo(Target &t, QueuedLine &&job) {
        if (!t.wantConnect.load(std::memory_order_acquire)) return;
        {
            std::lock_guard<std::mutex> lk(t.mtx);
            while (t.queue.size() >= RT::MIRROR_QUEUE_MAX) t.queue.pop_front();
            t.queue.push_back(std::move(job));
        }
        t.cv.notify_one();
    }

    // Does a BROADCAST reach this target? Only the fan-out asks: SendTo and
    // PingAll are per-row console actions on a named target, and a row left out
    // of the mirror selection is still a row you can poll, start or stop.
    bool Mirrored(const Target &t) {
        return t.mirroring.load(std::memory_order_acquire);
    }

    Target *Find(int id) {
        for (std::unique_ptr<Target> &t : g_targets)
            if (t->id == id) return t.get();
        return nullptr;
    }

} // namespace

// =============================================================================
// Target list — UI thread only
// =============================================================================

void SetOwner(HWND hOwner) { g_owner = hOwner; }

const wchar_t *DownLabel(Down d) {
    switch (d) {
        case Down::Offline:  return L"offline";
        case Down::Address:  return L"bad address";
        case Down::Rejected: return L"rejected";
        case Down::Auth:     return L"password";
        case Down::Protocol: return L"not qIV";
        default:             return L"—";
    }
}

const wchar_t *DownRemedy(Down d) {
    switch (d) {
        case Down::Offline:
            return L"Nothing is listening there. Click the red dot to start it, "
                   L"if it is on this machine and has an exe recorded.";
        case Down::Address:
            return L"That address does not resolve — the row is wrong, not the target.";
        case Down::Rejected:
            return L"It accepted and hung up without speaking. Its AllowList "
                   L"almost certainly does not include this machine.";
        case Down::Auth:
            return L"It refused the credentials — its password changed since this "
                   L"row was saved. Remove the row and add it again.";
        case Down::Protocol:
            return L"Something is listening on that port, but it is not a qIV "
                   L"remote. Wrong port?";
        default:
            return L"";
    }
}

bool SessionActive() {
    // CONNECTED targets, not configured ones — see g_connectedCount. Two atomic
    // loads, nothing else: this is asked on every command.
    return g_connectedCount.load(std::memory_order_acquire) > 0 ||
           ActiveConnections() > 0;
}

bool HasLiveTargets() {
    return g_connectedCount.load(std::memory_order_acquire) > 0;
}

int ConnectedCount() {
    return g_connectedCount.load(std::memory_order_acquire);
}

// Disk order is refused while connected, but blocking the COMMAND does not undo
// having been in that mode already. Connecting while sorted by disk would leave
// two instances quietly ordering their playlists differently, which is the exact
// failure the block exists to prevent — so the connection fixes it rather than
// merely forbidding the next press.
//
// Sort by name, because it is the default and the one order every machine agrees
// on. Announced, because a sort order changing by itself is otherwise alarming.
static void LeaveDiskOrderForSession(HWND hOwner) {
    if (app.fileHandlerDefaultSortOrder != 4) return;

    app.fileHandlerDefaultSortOrder   = 0;
    app.fileHandlerIsReverseSortOrder = false;
    Persistence::Registry::SaveSetting(Constants::Registry::SORT_ORDER, 0u);
    Persistence::Registry::SaveSetting(Constants::Registry::SORT_REVERSE, 0u);

    if (hOwner) {
        ReSortPlaylistAndRebuildMap(hOwner);
        g_overlayManager.PostCenterMessage(
            hOwner, Constants::Messages::REMOTE_SORT_LEFT_DISK_ORDER);
    }
}

int AddTarget(const std::wstring &name, const std::wstring &host, int port,
              const std::wstring &password, const std::wstring &exePath,
              const std::wstring &pin, bool connectNow) {
    // Before the first LIVE target: entering a session with disk order still
    // selected would misalign the two playlists from the start. A row that is
    // only being listed changes nothing, so it does not trigger this.
    if (connectNow && !HasLiveTargets()) LeaveDiskOrderForSession(g_owner);

    auto t = std::make_unique<Target>();
    t->id          = g_nextId++;
    t->name        = name;
    t->host        = host;
    t->port        = port;
    t->password    = password;
    t->exePath     = exePath;
    t->pin         = pin;
    // Resolved here, synchronously, so the row is right the moment it appears.
    // The periodic refresh below only has to catch LATER changes.
    t->sameMachine.store(IsSameMachine(host), std::memory_order_release);

    // UI thread, and the last moment before a sender thread exists that will
    // want this name and may not read Config() itself. See RemoteLog.h.
    Log::SetSelfName(Remote::Config().name);

    // How the log names the far end. Set before the sender thread starts, so
    // even the first handshake is recorded against a readable name rather than
    // an address.
    t->client.SetPeerLabel(name.empty() ? (host + L":" + std::to_wstring(port)) : name);
    // Set before any connect attempt: the pin is checked during the TLS
    // handshake, which happens before the banner, so supplying it afterwards
    // would be supplying it too late.
    t->client.SetPin(pin);
    t->wantConnect.store(connectNow, std::memory_order_release);

    Target *raw = t.get();
    g_targets.push_back(std::move(t));

    // Connecting happens ON the thread, so this returns immediately even when
    // the target is switched off.
    raw->th = std::thread(SenderLoop, raw);
    return raw->id;
}

void RemoveTarget(int id) {
    for (auto it = g_targets.begin(); it != g_targets.end(); ++it) {
        if ((*it)->id != id) continue;

        Target *t = it->get();
        t->stop.store(true, std::memory_order_release);
        t->cv.notify_all();
        // Joined before erasing, so the thread cannot still be adjusting the
        // connected count for an object that is about to be destroyed.
        if (t->th.joinable()) t->th.join();
        SetConnected(*t, false); // idempotent — the loop's exit already did it

        g_targets.erase(it);
        return;
    }
}

void Shutdown() {
    // Signal everything first, then join: stopping five targets one at a time
    // would serialise five idle-poll timeouts.
    for (std::unique_ptr<Target> &t : g_targets) {
        t->stop.store(true, std::memory_order_release);
        t->cv.notify_all();
    }
    for (std::unique_ptr<Target> &t : g_targets)
        if (t->th.joinable()) t->th.join();

    g_targets.clear();
    g_connectedCount.store(0, std::memory_order_release); // every thread is joined
    g_owner = nullptr;
}

int TargetCount() { return static_cast<int>(g_targets.size()); }

std::vector<TargetView> Targets() {
    std::vector<TargetView> out;
    out.reserve(g_targets.size());
    for (std::unique_ptr<Target> &t : g_targets) {
        TargetView v;
        v.id          = t->id;
        v.sameMachine = t->sameMachine.load(std::memory_order_acquire);
        v.name        = t->name;
        v.host      = t->host;
        v.port      = t->port;
        v.exePath   = t->exePath;
        v.connecting = t->wantConnect.load(std::memory_order_acquire);
        v.mirroring  = t->mirroring.load(std::memory_order_acquire);
        v.connected  = t->connected.load(std::memory_order_acquire);
        v.observing = t->observing.load(std::memory_order_acquire);
        v.lagUs     = t->lagUs.load(std::memory_order_acquire);
        v.down      = t->down.load(std::memory_order_acquire);
        {
            std::lock_guard<std::mutex> lk(t->mtx);
            v.lastError = t->lastError;
        }
        out.push_back(std::move(v));
    }
    return out;
}

// =============================================================================
// Sending — UI thread, never blocks
// =============================================================================

void BroadcastLine(const std::wstring &line) {
    for (std::unique_ptr<Target> &t : g_targets)
        if (Mirrored(*t)) PushTo(*t, line, {});
}

void BroadcastPosition(const std::wstring &line, const std::wstring &expectFile) {
    // SAME-MACHINE TARGETS ONLY.
    //
    // A position is an index into a playlist. Two instances on this box see the
    // same folder and the same files, so index 47 is the same picture on both.
    // An instance on another machine has its own content, where 47 means
    // something unrelated — sending it would jump that screen to an arbitrary
    // image, which is worse than leaving it where it was.
    //
    // The expected file name goes only to those same-machine targets too. It is
    // there to detect the two playlists drifting apart, which is a fault worth
    // repairing; across machines the names differing is the normal condition,
    // and checking would report a divergence on every single navigation and push
    // a `sync` each time — a repair loop for a problem that is not one.
    for (std::unique_ptr<Target> &t : g_targets)
        if (t->sameMachine.load(std::memory_order_acquire) && Mirrored(*t))
            PushTo(*t, line, expectFile);
}

int SendImagePosition(const PushRequest &req, int *skippedRemote) {
    int sent = 0, skipped = 0;

    // SAME-MACHINE TARGETS ONLY, for the reason positions are: the job carries a
    // folder path and an index, and neither survives the trip to a box with its
    // own files at its own paths. Counted rather than dropped in silence — a
    // caller that reported "pushed to 3" when two of them were never asked would
    // be lying about the screens the user is looking at.
    //
    // The CONTROL ticks decide the rest (Mirrored), so pushing and mirroring
    // cannot reach different screens. F11 itself is NOT consulted: this is an
    // explicit act, and a viewer with mirroring off is the case it exists for.
    for (std::unique_ptr<Target> &t : g_targets) {
        if (!Mirrored(*t)) continue;
        if (!t->connected.load(std::memory_order_acquire)) continue;
        if (!t->sameMachine.load(std::memory_order_acquire)) { ++skipped; continue; }

        QueuedLine job;
        job.kind = QueuedLine::Kind::Position;
        job.push = req;
        PushJobTo(*t, std::move(job));
        ++sent;
    }

    if (skippedRemote) *skippedRemote = skipped;
    return sent;
}

int StreamImageToTargets(const std::wstring &imagePath) {
    int sent = 0;

    // EVERY controlled, connected target — this machine or any other. That is the
    // whole reason this carries bytes instead of a path: locality stops mattering,
    // so there is nothing to skip and no second command set to choose between.
    for (std::unique_ptr<Target> &t : g_targets) {
        if (!Mirrored(*t)) continue;
        if (!t->connected.load(std::memory_order_acquire)) continue;

        QueuedLine job;
        job.kind       = QueuedLine::Kind::StreamOut;
        job.streamPath = imagePath;
        PushJobTo(*t, std::move(job));
        ++sent;
    }
    return sent;
}

int RequestDisplayedImage(std::wstring &fromName) {
    fromName.clear();

    // ONE target, because the answer is a picture and this screen shows one at a
    // time. Asking three would decode three images and throw two away.
    //
    // The choice, in order: the instance being WATCHED (Ctrl+F11's ◉) if there is
    // one — you are already following that screen, so it is the one you mean —
    // otherwise the first controlled, connected row. Named back to the caller so
    // the overlay can say WHICH screen answered; with several ticked, a silent
    // pick would be indistinguishable from picking the wrong one.
    Target *chosen = nullptr;
    for (std::unique_ptr<Target> &t : g_targets) {
        if (!t->connected.load(std::memory_order_acquire)) continue;
        if (t->observing.load(std::memory_order_acquire)) { chosen = t.get(); break; }
        if (!Mirrored(*t)) continue;
        if (!chosen) chosen = t.get();
    }
    if (!chosen) return 0;

    fromName = chosen->name.empty()
                   ? (chosen->host + L":" + std::to_wstring(chosen->port))
                   : chosen->name;

    QueuedLine job;
    job.kind = QueuedLine::Kind::StreamIn;
    PushJobTo(*chosen, std::move(job));
    return 1;
}

void BroadcastSync(const std::wstring &full, const std::wstring &portable) {
    // EVERY target, mirror selection included — this is the console's Sync All
    // button, an explicit "line all of them up with me", not a mirrored
    // keystroke. Skipping the rows F11 currently leaves out would make that
    // button do less than its label says for no reason the user can see.
    //
    // Same state, two spellings. `full` carries folder=; `portable` does not,
    // because a drive letter means nothing on another machine and applying it
    // would send that instance to a folder it cannot open.
    for (std::unique_ptr<Target> &t : g_targets)
        PushTo(*t, t->sameMachine.load(std::memory_order_acquire) ? full : portable, {});
}

void Broadcast(Command cmd) {
    // Nothing joined: leave before walking the command table for a name that
    // would go nowhere. F11 stays on across a target dropping — it reconnects
    // and resumes — so without this the table walk would happen on every
    // keystroke for as long as the screens stayed off.
    if (g_connectedCount.load(std::memory_order_acquire) == 0) return;

    // Filtered per TARGET, not once for the whole fan-out: a mixed list — one
    // instance on this box, one on the screen in the next room — needs a
    // different answer for each, and filtering once would apply the stricter or
    // the looser rule to both.
    std::wstring name;
    if (!NameForCommand(cmd, name)) return; // unreachable command — nothing to send

    const bool okLocal  = IsMirrorable(cmd);
    const bool okRemote = IsMirrorableRemote(cmd);
    if (!okLocal && !okRemote) return;

    for (std::unique_ptr<Target> &t : g_targets)
        if (Mirrored(*t) &&
            (t->sameMachine.load(std::memory_order_acquire) ? okLocal : okRemote))
            PushTo(*t, name, {});
}

void SetMirroring(int id, bool on) {
    if (Target *t = Find(id)) t->mirroring.store(on, std::memory_order_release);
}

void SetMirroringAll(bool on) {
    for (std::unique_ptr<Target> &t : g_targets)
        t->mirroring.store(on, std::memory_order_release);
}

int MirroredLiveCount() {
    // Walks the list rather than keeping a counter: this is asked when F11 is
    // pressed and when the picker is drawn, never on the keystroke path — the
    // gate there is HasLiveTargets, which is one atomic load.
    int n = 0;
    for (std::unique_ptr<Target> &t : g_targets)
        if (t->connected.load(std::memory_order_acquire) && Mirrored(*t)) ++n;
    return n;
}

std::wstring SelectionSummary() {
    int live = 0, picked = 0;
    std::wstring names;
    for (std::unique_ptr<Target> &t : g_targets) {
        if (!t->connected.load(std::memory_order_acquire)) continue;
        ++live;
        if (!Mirrored(*t)) continue;
        ++picked;
        if (!names.empty()) names += L", ";
        names += t->name.empty() ? t->host : t->name;
    }

    // Nothing narrowed: the count is the whole story, and naming three screens
    // that are all following along is noise on top of an overlay that is only
    // up for a second.
    if (picked == live)
        return std::to_wstring(picked) + (picked == 1 ? L" target" : L" targets");

    // Narrowed: which ones, and out of how many — the count alone would leave
    // the user counting screens to work out who was left out.
    return names + L" (" + std::to_wstring(picked) + L" of " +
           std::to_wstring(live) + L")";
}

void SendTo(int id, const std::wstring &line) {
    if (Target *t = Find(id)) PushTo(*t, line, {});
}

int SendToIds(const std::wstring &line, const std::vector<int> &ids, HWND replyTo) {
    int n = 0;
    // NAMED targets, so the mirror selection is deliberately not consulted: the
    // caller has already said which instances it means. Only CONNECTED ones are
    // counted, because the number is reported to a user as "sent to n", and a row
    // that is merely listed would never answer.
    for (const int id : ids) {
        Target *t = Find(id);
        if (!t) continue;                                    // removed meanwhile
        if (!t->connected.load(std::memory_order_acquire)) continue;
        PushTo(*t, line, {}, replyTo);
        ++n;
    }
    return n;
}

// SendToControlled — the "every ☑ row in Ctrl+F11" form — is GONE, not merely
// unused: the Send Command panel now names its recipients explicitly (SendToIds),
// and keeping a second, differently-scoped send around would be the obvious thing
// for the next caller to reach for by mistake. Where keystrokes go and where one
// typed line goes are separate questions now.

void PingAll() {
    // Down the LIVE connection: MaxConnections defaults to 1, so a second
    // connection to a target this master already drives would simply be refused
    // — and the round trip on the channel actually in use is the more honest
    // number anyway.
    for (std::unique_ptr<Target> &t : g_targets) PushTo(*t, L"ping", {});
}

void BroadcastEnableLog(bool on) {
    // The STATE, not a toggle — see Command::EnableRemoteLog. Sent as one line
    // to every target, the same spelling both ways, because `enablelog` carries
    // no paths and no indices and so needs no same-machine variant.
    //
    // EVERY connected target, not just the mirrored selection: the log is a
    // diagnostic about the whole session, and a screen left recording because it
    // happened to be unticked in Remotes Control is a file nobody will think to
    // look at, still growing.
    const std::wstring line = on ? L"EnableRemoteLog 1" : L"EnableRemoteLog 0";
    for (std::unique_ptr<Target> &t : g_targets) PushTo(*t, line, {});
}

void AddPanelNotify(HWND hwnd) {
    if (!hwnd) return;

    // IDEMPOTENT. A panel may be shown twice without an intervening hide (the
    // cross-panel buttons do exactly that), and a second slot for the same window
    // would double every message it receives.
    for (PanelNotify &p : g_panelNotify)
        if (p.hwnd.load(std::memory_order_acquire) == hwnd) return;

    for (PanelNotify &p : g_panelNotify) {
        HWND expected = nullptr;
        if (p.hwnd.compare_exchange_strong(expected, hwnd,
                                           std::memory_order_acq_rel,
                                           std::memory_order_acquire)) {
            // Fresh subscriber, open gate — otherwise a slot left `pending` by the
            // panel that used it before would swallow this one's first message.
            p.pending.store(false, std::memory_order_release);
            return;
        }
    }
    // Every slot taken. Silently not subscribed: the panel keeps working and
    // simply refreshes when something else touches it, which is what it did before
    // this notification existed. Raising the count is the fix if that ever happens.
}

void RemovePanelNotify(HWND hwnd) {
    if (!hwnd) return;
    for (PanelNotify &p : g_panelNotify) {
        if (p.hwnd.load(std::memory_order_acquire) != hwnd) continue;
        // Window first, gate second: between the two a sender thread can only find
        // an empty slot and skip it. The other order could leave `pending` true on a
        // slot that is about to be handed to another panel.
        p.hwnd.store(nullptr, std::memory_order_release);
        p.pending.store(false, std::memory_order_release);
        return;
    }
}

void ClearPanelNotifyPending(HWND hwnd) {
    for (PanelNotify &p : g_panelNotify)
        if (p.hwnd.load(std::memory_order_acquire) == hwnd) {
            p.pending.store(false, std::memory_order_release);
            return;
        }
}

void RefreshSameMachine() {
    // Only ASKS. The resolving happens on each sender thread, so a console poll
    // never stalls the UI on DNS — the answers are picked up by the repaint the
    // poll timer was already going to do.
    for (std::unique_ptr<Target> &t : g_targets) {
        t->resolveWanted.store(true, std::memory_order_release);
        t->cv.notify_all();
    }
}

void SetConnecting(int id, bool on) {
    Target *t = Find(id);
    if (!t) return;

    // Same guard as adding a live target: going from "nothing connected" to
    // "something connected" is what starts a session, and disk order cannot
    // survive one.
    if (on && !HasLiveTargets()) LeaveDiskOrderForSession(g_owner);

    t->wantConnect.store(on, std::memory_order_release);
    // Wakes the sender thread out of whichever wait it is in — the idle park
    // when switching on, the reconnect back-off when switching off.
    t->cv.notify_all();
}

void SetConnectingAll(bool on) {
    // The disk-order check runs once for the whole batch rather than per target
    // — it is about entering a session at all, not about each connection.
    if (on && !HasLiveTargets()) LeaveDiskOrderForSession(g_owner);

    for (std::unique_ptr<Target> &t : g_targets) {
        t->wantConnect.store(on, std::memory_order_release);
        t->cv.notify_all();
    }
}

void SetObserving(int id, bool on) {
    Target *t = Find(id);
    if (!t) return;

    // EXCLUSIVE — the eye is a radio button, not a set of checkboxes.
    //
    // Observing means this viewer executes what the other one does. Two at once
    // would interleave two independent streams of actions into one screen: the
    // master would jump between whatever each was doing, following neither. The
    // result is not "watching two screens", it is watching nothing coherently.
    //
    // Enforced HERE rather than in the console, so the invariant holds for any
    // caller — a second panel, a future shortcut, a scripted setup — instead of
    // depending on one window remembering to clear the others.
    if (on) {
        for (std::unique_ptr<Target> &other : g_targets) {
            if (other.get() == t) continue;
            if (!other->observing.exchange(false, std::memory_order_acq_rel)) continue;
            // Told to stop, not merely forgotten: an instance left believing it
            // has an observer keeps pushing events down a connection that is no
            // longer acting on them.
            PushTo(*other, L"Observe 0", {});
        }
    }

    t->observing.store(on, std::memory_order_release);
    PushTo(*t, on ? L"Observe 1" : L"Observe 0", {});
}

} // namespace Remote::Mirror
