#pragma once
#include <string>
#include <vector>

// =============================================================================
// RemoteBlacklist — addresses the listener refuses, and the only configuration
// file qIV writes to BY ITSELF.
//
//     qivRemoteServerBlacklist.ini, beside the exe, one entry per line:
//
//         <address>;<date time>;<reason>
//
//     203.0.113.7;2026-08-03 04:11:52;5 failed authentications in 10 min
//     192.0.2.44
//
// Only the address is required. A bare one is a hand-written block and needs
// nothing else; every entry the PROGRAM writes carries all three fields,
// because an automatic entry that does not say when or why is one nobody can
// safely decide to delete six months later.
//
// WHY IT IS SEPARATE FROM qivLocalServer.ini. That file is what the user
// decided; this one is what the machine decided, unattended, while a brute-force
// attempt was in progress. Keeping them apart means an automatic write can never
// race a hand edit of the port or the password, and it means the blacklist can
// be deleted wholesale — which is exactly what you want to do after locking
// yourself out — without touching the configuration.
//
// LIVE, NOT SNAPSHOTTED. The accept loop reads a Settings snapshot taken at
// Start(), which is right for configuration and wrong for this: an address
// blacklisted at 04:11 has to be refused at 04:11, not after the next restart.
// So the list lives here, behind its own mutex, and is consulted directly.
//
// The corollary is that a HAND EDIT of the file is not noticed while the
// listener runs — the in-memory list is authoritative for the session and the
// file is its persistence. Reload() exists for the panel to call; there is no
// file watcher, because a watcher on a file this process also writes is a race
// looking for somewhere to happen.
// =============================================================================

namespace Remote::Blacklist {

    struct Entry {
        std::wstring address;   // literal, or a "192.168.1.*" prefix pattern
        std::wstring when;      // "YYYY-MM-DD HH:MM:SS", empty for a hand entry
        std::wstring reason;    // free text, empty for a hand entry
    };

    // <exe folder>\qivRemoteServerBlacklist.ini — resolvable whether or not the
    // file exists.
    const std::wstring &FilePath();
    bool FileExists();

    // Reads the file into memory, replacing whatever was held. Called once at
    // Start(); safe to call again to pick up hand edits.
    void Reload();

    // Is this peer refused? Matches with Remote::AddressMatches, so a trailing
    // "*" covers a subnet exactly as it does in the AllowList.
    //
    // ON THE ACCEPT PATH: one mutex and a linear scan of a list that is empty in
    // any ordinary install. Deliberately not an index — an ordered container
    // cannot answer a wildcard match, and building one to serve the empty case
    // faster would be work for nothing.
    bool IsBlocked(const std::wstring &address);

    // Adds an address with the current local time and a reason, and appends the
    // line to the file. No-op when it is already blocked, so a repeat offender
    // does not accumulate duplicate lines.
    //
    // APPENDS rather than rewriting: the file is only ever added to, so there is
    // nothing to merge and no window in which a crash mid-write loses entries
    // that were already there.
    void Add(const std::wstring &address, const std::wstring &reason);

    // A copy, for the panel to display. Not a reference — the list is mutated
    // from socket threads.
    std::vector<Entry> Snapshot();
    size_t Count();

} // namespace Remote::Blacklist
