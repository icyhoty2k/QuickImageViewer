// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#pragma once
#include <string>

// =============================================================================
// RemoteBeacon — "there is a qIV here", said on the local network.
//
// THE PROBLEM IT SOLVES. Every client of this protocol currently begins by being
// told an address to type. That is the single step most likely to end with the
// user giving up: it needs them to find their PC's LAN address, type it without
// a typo on a phone keyboard, and know what a port is. The server already knows
// all of it. This says so.
//
// mDNS / DNS-SD, service type `_qiv._tcp`, over Windows' own DnsServiceRegister
// (Windows 10 1703+). No Bonjour install, no vendored library, and nothing new
// in the build — and on the Android side it is answered by NsdManager, which is
// part of the framework. Both ends get discovery with no dependency, which is
// why this and not a hand-rolled UDP beacon.
//
// -----------------------------------------------------------------------------
// WHAT IT ANNOUNCES, AND WHAT IT DELIBERATELY DOES NOT
//
//   instance name — the server's configured Name, or the host name
//   port          — the TCP port the listener is on
//
// That is all. NEVER the password, never whether one is set, never a path, never
// a file name, never the AllowList. A beacon is a signpost, and a signpost that
// describes the lock on the door is a worse signpost.
//
// DISCOVERY IS NOT ACCESS. Being findable changes nothing about who may connect:
// the AllowList, the password challenge and TLS are all untouched by this module
// and all still apply. Someone who discovers this instance and is not on the
// allow list gets exactly as far as someone who guessed the address.
// -----------------------------------------------------------------------------

namespace Remote::Beacon {

    // Reconciles the announcement with reality. Idempotent, and the ONLY entry
    // point that decides whether a beacon exists.
    //
    // The beacon is live when ALL of these hold:
    //
    //   1. app.remoteBeacon is on          — the user asked for it
    //   2. the Local Server is listening   — there is something to connect TO
    //   3. the bind address is not loopback-only
    //
    // Rule 2 is the one worth stating twice: a beacon without a listener behind
    // it advertises a service that refuses every connection, which is worse than
    // no beacon at all — the client finds an entry, tries it, and fails, and the
    // user blames the app rather than the setting.
    //
    // Rule 3 follows from rule 2 and is not fussiness. On 127.0.0.1 nothing off
    // this machine can reach the listener, so announcing it to the network is a
    // promise that cannot be kept by anyone who hears it.
    //
    // Call this after ANY of the three change: server started, server stopped,
    // the setting toggled, the bind address or port edited. Calling it when
    // nothing changed is free — it compares against what is currently registered
    // and returns.
    void Refresh();

    // Whether an announcement is on the network right now. For the panel — the
    // setting says what was asked for, this says what is true.
    bool IsAdvertising();

    // A short reason the beacon is not live while the setting is on, for the F9
    // panel to show. Empty when it is live, or when the user never asked for it.
    //
    // Exists because "I ticked the box and nothing happened" is the support
    // question this feature will generate, and the answer is always one of the
    // three rules above. The panel should be able to say which.
    std::wstring InactiveReason();

    // Tears down any registration. Called on shutdown; safe when nothing is
    // registered.
    void Shutdown();

} // namespace Remote::Beacon
