// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#pragma once
#include <cstdint>

// =============================================================================
// RemoteInbound — "this command arrived from the wire, do not send it back out".
//
// THE PROBLEM THIS SOLVES IS A LOOP, and it is not hypothetical: it is the
// normal configuration.
//
// A master with mirroring on (F11) forwards its commands to a slave. Press the
// eye in the F10 console and that slave also echoes what it does back to the
// master. Now:
//
//     master presses Space
//       → forwards "next" to the slave
//         → slave executes it
//           → slave echoes "EVENT next" back to the master
//             → master executes it
//               → master forwards "next" to the slave …
//
// One keystroke, forever, at socket speed. Two instances observing each other
// do the same thing with no master at all.
//
// The cut is simple and has to be in exactly one place: while a command that
// CAME FROM the wire is executing, that same execution must neither forward nor
// echo. Both suppressions read this one flag — a command is either something
// this instance decided to do, or something it was told to do, and only the
// first kind travels.
//
// `Source()` carries WHICH connection it came from, because suppression is not
// all-or-nothing on the slave side: a command from observer A must still reach
// observers B and C, just not bounce back to A.
//
// UI THREAD ONLY. Every command executes there, so plain statics are correct
// and an atomic would only cost something. Scoped rather than set/clear, so an
// early return inside ExecuteCommand cannot leave the flag stuck on — which
// would silently disable mirroring for the rest of the session.
// =============================================================================

namespace Remote {

    // 0 means "no connection" — a local keypress, or the far end of an echo we
    // do not need to exclude.
    using ConnId = uint64_t;
    inline constexpr ConnId CONN_NONE = 0;

    namespace detail {
        inline int    g_inboundDepth  = 0;
        inline ConnId g_inboundSource = CONN_NONE;
    }

    class InboundGuard {
    public:
        explicit InboundGuard(ConnId from) {
            // Nested is possible — a `sync` handler running an inner command —
            // and the OUTERMOST source is the one that matters, so an inner
            // guard must not overwrite it.
            if (detail::g_inboundDepth == 0) detail::g_inboundSource = from;
            ++detail::g_inboundDepth;
        }
        ~InboundGuard() {
            if (--detail::g_inboundDepth == 0) detail::g_inboundSource = CONN_NONE;
        }
        InboundGuard(const InboundGuard &)            = delete;
        InboundGuard &operator=(const InboundGuard &) = delete;
    };

    // True while a command that arrived from the wire is executing.
    inline bool InboundActive() { return detail::g_inboundDepth > 0; }

    // The connection it came from, or CONN_NONE.
    inline ConnId InboundSource() { return detail::g_inboundSource; }

    // =========================================================================
    // "May this instance echo what it is doing to its observers right now?"
    //
    // NOT the same question as `!InboundActive()`, and treating it as one is
    // what broke the two-phone case: phone A presses Next, the server executes
    // it under the inbound guard, and every observer echo was suppressed — so
    // phone B, watching the same viewer with `observe 1`, was told NOTHING and
    // sat frozen on the previous picture. The only client that ever learned
    // anything was the one that already knew, from its own reply.
    //
    // The suppression that IS needed is narrower, and the `except` argument of
    // EmitToObservers already expresses it: do not bounce the line back to the
    // connection that sent the command. Observers B and C still get it.
    //
    // The one case that must still be silenced wholesale is the OTHER inbound
    // route — WM_QIV_REMOTE_EVENT in AppMain, where this instance is replaying
    // what an observed PEER did. That guard carries CONN_NONE, so there is no
    // connection to exclude, and echoing there is exactly the mutual-observe
    // loop this header exists to cut: two instances watching each other would
    // trade one keystroke forever. Hence the source test rather than a bare
    // depth test — a real ConnId means "a client asked for this", CONN_NONE
    // while inbound means "a peer's event is being replayed".
    //
    // Callers still pass InboundSource() as `except`; this only decides whether
    // to emit at all.
    // =========================================================================
    inline bool ObserverEchoAllowed() {
        return !InboundActive() || InboundSource() != CONN_NONE;
    }

    // =========================================================================
    // "This dispatch already forwarded a command — do not also forward what it
    // did."
    //
    // Two things forward: the gate in ExecuteCommand (a COMMAND, e.g. `next`)
    // and LoadImageIndex (an INDEX, `goto 47`). Both are needed — most picture
    // changes are not commands at all, and the slideshow timer never goes near
    // ExecuteCommand — but for a navigation command they would BOTH fire on one
    // keypress.
    //
    // Sending both is worse than merely redundant. `next` means "advance in
    // YOUR playlist", which is the point of forwarding commands rather than
    // positions; a `goto 47` arriving behind it would overwrite that with a
    // position computed from a DIFFERENT list. The far end would end up on our
    // index instead of its own next picture.
    //
    // So: whichever fires first wins, and the gate is first. Scoped for the
    // remainder of the dispatch.
    // =========================================================================
    namespace detail { inline int g_forwardDepth = 0; }

    class ForwardGuard {
    public:
        explicit ForwardGuard(bool active) : m_active(active) {
            if (m_active) ++detail::g_forwardDepth;
        }
        ~ForwardGuard() { if (m_active) --detail::g_forwardDepth; }
        ForwardGuard(const ForwardGuard &)            = delete;
        ForwardGuard &operator=(const ForwardGuard &) = delete;
    private:
        bool m_active;
    };

    inline bool ForwardInFlight() { return detail::g_forwardDepth > 0; }

} // namespace Remote
