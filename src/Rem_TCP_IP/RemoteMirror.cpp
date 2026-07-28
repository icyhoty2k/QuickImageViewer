// winsock2.h before anything that pulls windows.h — see the note in
// RemoteServer.cpp. RemoteClient.h includes windows.h.
#include <winsock2.h>
#include <ws2tcpip.h>

#include "RemoteMirror.h"
#include "RemoteClient.h"
#include "RemoteProtocol.h"
#include "RemoteServer.h" // ActiveConnections — the other half of SessionActive

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

        // Decided ONCE, when the target is added — the answer cannot change
        // while the process runs, and resolving it per keystroke would put a DNS
        // lookup in the mirror path.
        //
        // Same machine  → the full command set, positions included: both ends
        //                 see the same files, so an index means the same picture.
        // Other machine → the portable subset. No indices, no paths, no folder.
        //                 That instance becomes a PARALLEL viewer running the
        //                 same actions over its own content, rather than a
        //                 mirror of this screen — which is the most that can
        //                 honestly be delivered when the folders differ.
        bool sameMachine = false;

        Client client;                      // sender thread only

        std::mutex               mtx;       // guards queue + lastError
        std::condition_variable  cv;
        std::deque<QueuedLine>   queue;
        std::wstring             lastError;

        std::atomic<bool>      stop{false};
        std::atomic<bool>      connected{false};
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
    // has a qivRemotes.ini has targets from the moment it starts, and if that
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
    void SetConnected(Target &t, bool up) {
        const bool was = t.connected.exchange(up, std::memory_order_acq_rel);
        if (was == up) return;
        g_connectedCount.fetch_add(up ? 1 : -1, std::memory_order_acq_rel);
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

        bool same = false;
        for (addrinfo *a = them; a && !same; a = a->ai_next) {
            for (addrinfo *b = mine; b; b = b->ai_next) {
                if (a->ai_family != b->ai_family) continue;
                if (a->ai_addrlen == b->ai_addrlen &&
                    memcmp(a->ai_addr, b->ai_addr, a->ai_addrlen) == 0) {
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

    void PostEventLine(const std::wstring &line) {
        if (!g_owner) return;
        PostMessageW(g_owner, Constants::WM_QIV_REMOTE_EVENT, 0,
                     reinterpret_cast<LPARAM>(new std::wstring(line)));
    }

    // --- the sender thread --------------------------------------------------
    //
    // The ONLY thread that touches t.client. Connects, drains the queue, and
    // while idle listens for the unsolicited EVENT lines an observed target
    // pushes.
    void SenderLoop(Target *t) {
        while (!t->stop.load(std::memory_order_acquire)) {

            // ── Connect / reconnect ─────────────────────────────────────────
            if (!t->client.IsConnected()) {
                std::wstring err;
                if (!t->client.Connect(t->host, t->port, t->password, err)) {
                    SetConnected(*t, false);
                    SetError(*t, err);

                    // Back off rather than spin: a slave that is switched off is
                    // an ordinary state, not an error to retry as fast as the
                    // CPU allows. Waiting on the cv (not sleeping) so that
                    // Shutdown still stops this thread promptly.
                    std::unique_lock<std::mutex> lk(t->mtx);
                    t->cv.wait_for(lk, std::chrono::milliseconds(RT::MIRROR_RECONNECT_MS),
                                   [t] { return t->stop.load(std::memory_order_acquire); });
                    continue;
                }
                SetConnected(*t, true);
                SetError(*t, {});

                // Re-arm observation across a reconnect. The far end drops its
                // observer entry when the connection dies, so a target that was
                // being watched before a restart would silently stop reporting.
                if (t->observing.load(std::memory_order_acquire)) {
                    std::wstring reply, err2;
                    (void) t->client.Send(L"observe 1", reply, err2);
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
                        return t->stop.load(std::memory_order_acquire) || !t->queue.empty();
                    });
                    continue;
                }
            }

            if (have) {
                std::wstring reply, err;
                std::vector<std::wstring> events;

                const long long t0 = NowUs();
                const bool ok = t->client.Send(item.line, reply, err, &events);
                const long long t1 = NowUs();

                if (!ok) {
                    SetConnected(*t, false);
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
    void PushTo(Target &t, const std::wstring &line, const std::wstring &expectFile) {
        {
            std::lock_guard<std::mutex> lk(t.mtx);
            // Drop the OLDEST on overflow. A backlog for a machine that stopped
            // answering would replay stale navigation when it came back, and a
            // mirrored keystroke is only meaningful at the moment it is made.
            while (t.queue.size() >= RT::MIRROR_QUEUE_MAX) t.queue.pop_front();
            t.queue.push_back(QueuedLine{line, expectFile});
        }
        t.cv.notify_one();
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

bool SessionActive() {
    // CONNECTED targets, not configured ones — see g_connectedCount. Two atomic
    // loads, nothing else: this is asked on every command.
    return g_connectedCount.load(std::memory_order_acquire) > 0 ||
           ActiveConnections() > 0;
}

bool HasLiveTargets() {
    return g_connectedCount.load(std::memory_order_acquire) > 0;
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
              const std::wstring &password, const std::wstring &exePath) {
    // Before the first target exists: entering a session with disk order still
    // selected would misalign the two playlists from the start.
    if (g_targets.empty()) LeaveDiskOrderForSession(g_owner);

    auto t = std::make_unique<Target>();
    t->id          = g_nextId++;
    t->name        = name;
    t->host        = host;
    t->port        = port;
    t->password    = password;
    t->exePath     = exePath;
    t->sameMachine = IsSameMachine(host);

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
        v.sameMachine = t->sameMachine;
        v.name        = t->name;
        v.host      = t->host;
        v.port      = t->port;
        v.exePath   = t->exePath;
        v.connected = t->connected.load(std::memory_order_acquire);
        v.observing = t->observing.load(std::memory_order_acquire);
        v.lagUs     = t->lagUs.load(std::memory_order_acquire);
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
    for (std::unique_ptr<Target> &t : g_targets) PushTo(*t, line, {});
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
        if (t->sameMachine) PushTo(*t, line, expectFile);
}

void BroadcastSync(const std::wstring &full, const std::wstring &portable) {
    // Same state, two spellings. `full` carries folder=; `portable` does not,
    // because a drive letter means nothing on another machine and applying it
    // would send that instance to a folder it cannot open.
    for (std::unique_ptr<Target> &t : g_targets)
        PushTo(*t, t->sameMachine ? full : portable, {});
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
        if (t->sameMachine ? okLocal : okRemote) PushTo(*t, name, {});
}

void SendTo(int id, const std::wstring &line) {
    if (Target *t = Find(id)) PushTo(*t, line, {});
}

void PingAll() {
    // Down the LIVE connection: MaxConnections defaults to 1, so a second
    // connection to a target this master already drives would simply be refused
    // — and the round trip on the channel actually in use is the more honest
    // number anyway.
    for (std::unique_ptr<Target> &t : g_targets) PushTo(*t, L"ping", {});
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
            PushTo(*other, L"observe 0", {});
        }
    }

    t->observing.store(on, std::memory_order_release);
    PushTo(*t, on ? L"observe 1" : L"observe 0", {});
}

} // namespace Remote::Mirror
