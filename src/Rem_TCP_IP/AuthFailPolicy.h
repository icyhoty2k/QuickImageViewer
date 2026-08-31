// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <algorithm>

// =============================================================================
//  AuthFailPolicy — what a failed password attempt should cost, as pure logic
//
//  WHY THIS IS ITS OWN HEADER. The decision used to live inline inside
//  RemoteServer::NoteAuthFailure, tangled with a mutex, a map, an eviction pass
//  and a file write. None of that can be reached from a test, so the one part
//  worth testing — how many wrong passwords earn what — was the one part
//  nothing could check. AddressMatch.cpp was split out of RemoteSettings.cpp for
//  exactly this reason; this is the same move, and header-only because there is
//  no state and nothing to link.
//
//  THE POLICY, AND WHY IT CHANGED IN v3.
//
//  It used to be one rule: five failures inside ten minutes wrote a PERMANENT
//  blacklist line. That was written when the only client was a second copy of
//  qIV on the same desk, driven by somebody who knew the password. With the
//  phone app public, the same rule now says that a family member mistyping the
//  password five times bans their phone's whole /64 from the machine for good,
//  and the only cure is finding and hand-editing an .ini file.
//
//  So the first crossing is now a TIMED block and the second is permanent:
//
//      crossing 1  ->  BlockTimed      (minutes, in memory, forgiven by a restart)
//      crossing 2+ ->  BlockPermanent  (the old behaviour, written to the file)
//
//  ⚠ THIS IS NOT A WEAKENING. An attacker guessing passwords does not stop
//  after one timed block, so they reach the permanent one a few minutes later;
//  what they lose is the ability to get themselves banned on a fat-fingered
//  first try, which was never the point of the guard. The rate limiting that
//  actually slows guessing is the per-attempt delay, not the ban.
// =============================================================================

namespace Remote::AuthPolicy {

    // What one failed attempt did to the record.
    enum class Response {
        Ignore,          // counted, nothing more
        BlockTimed,      // first threshold crossing
        BlockPermanent   // a repeat offender
    };

    // Per-key state. One of these per blocked SCOPE, not per address — see
    // BlockScope in RemoteServer.cpp for why a v6 peer is tracked as its /64.
    struct FailRecord {
        int       count      = 0; // failures inside the current window
        long long firstMs    = 0; // when the window opened
        long long lastSeenMs = 0; // for eviction when the table is full
        int       strikes    = 0; // threshold crossings, ever — drives escalation
    };

    // Records one failure and says what it earned.
    //
    // `nowMs` is a monotonic tick count, not a wall clock: the caller passes
    // GetTickCount64. A wall clock would let a system time change forgive or
    // manufacture a ban.
    inline Response NoteFailure(FailRecord &r, long long nowMs,
                                int maxFailures, long long windowMs) {
        r.lastSeenMs = nowMs;

        // A stale window is a NEW window, not a continuation. Without this an
        // address that fails once a month is eventually blocked for it.
        //
        // ⚠ STRIKES DELIBERATELY SURVIVE THE WINDOW RESET. The window measures
        // "is this a burst"; strikes measure "have we been here before". If
        // strikes reset with the window, an attacker who waits out ten minutes
        // between bursts would get a fresh timed block every time and never
        // reach the permanent one — which is precisely the behaviour the
        // permanent ban exists to stop.
        if (r.count == 0 || nowMs - r.firstMs > windowMs) {
            r.count   = 1;
            r.firstMs = nowMs;
            return Response::Ignore;
        }

        if (++r.count < maxFailures) return Response::Ignore;

        // Reset rather than erase: the address is about to be refused at
        // accept(), so this record has done its job, and leaving the count at
        // the threshold would re-trigger on any later attempt that slipped
        // through.
        r.count   = 0;
        r.firstMs = 0;
        ++r.strikes;

        return (r.strikes >= 2) ? Response::BlockPermanent : Response::BlockTimed;
    }

} // namespace Remote::AuthPolicy
