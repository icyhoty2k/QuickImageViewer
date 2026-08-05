// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

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
    // Answers for BOTH lists — the permanent file-backed one and the timed
    // blocks below. One question, one answer, one place the accept path calls.
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

    // =========================================================================
    // TIMED BLOCKS — a kick that keeps the peer out for a while.
    //
    // IN MEMORY ONLY, AND DELIBERATELY SO. Nothing here is written to
    // qivRemoteServerBlacklist.ini and nothing survives a restart. A temporary
    // block that outlived the process would be neither temporary nor visible —
    // the file is the permanent list, the thing you read to find out who is
    // barred, and salting it with entries that expire would make it answer that
    // question wrongly. Restarting qIV clears every timed block, which is the
    // documented escape hatch when you have shut yourself out.
    //
    // Kept in this module rather than beside the socket because IsBlocked is the
    // ONE question the accept path asks, and it must have ONE answer. A second
    // list consulted from a second place is how an address ends up blocked by
    // one rule and admitted by another.
    // =========================================================================

    struct TimedEntry {
        std::wstring address;      // literal or pattern, exactly like Entry
        std::wstring reason;
        long long    untilMs = 0;  // GetTickCount64 base; past means expired
    };

    // Blocks `address` for `minutes`, replacing any timed block already on it
    // (the new expiry wins, whether it is longer or shorter — the operator's
    // most recent decision is the current one).
    //
    // Bounded by Constants::RemoteTcpIp::TIMED_BLOCK_MAX so a script driving the
    // panel cannot grow the table without limit; the soonest-expiring entry is
    // dropped when it is full, since that is the one closest to being irrelevant.
    void AddTimed(const std::wstring &address, int minutes, const std::wstring &reason);

    // Lifts a timed block early. False when that address had none.
    bool ClearTimed(const std::wstring &address);

    // Live timed blocks, expired ones removed. For the panel's list.
    std::vector<TimedEntry> TimedSnapshot();

} // namespace Remote::Blacklist
