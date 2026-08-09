// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include "Input/Command.h"

// =============================================================================
// RemoteProtocol — the wire format, and the table of what is remotely reachable.
//
// FORMAT: newline-delimited UTF-8 text, one request per line.
//
//     <command> [payload]\n
//
// Deliberately plain. netcat, telnet, curl and a five-line Python script are all
// first-class clients, with nothing to marshal and nothing to generate.
//
//     next
//     goto 42
//     open C:\Pictures\holiday\IMG_0042.jpg
//     interval 5000
//
// Responses are one line, and a client only ever needs the first token:
//
//     OK
//     OK <text>
//     ERR <code> <human readable reason>
//
// -----------------------------------------------------------------------------
// NAMES ON THE WIRE, NEVER ORDINALS  —  this is a hard constraint.
//
// `enum class Command` renumbers whenever anyone inserts an enumerator, and
// people do: ToggleViewportLock was added mid-enum during the same session this
// protocol was designed, shifting every command after it by one. A script that
// sent the ordinal 47 would silently begin doing something different after the
// next rebuild, with no error raised anywhere — the worst possible failure mode.
//
// Names cost one lookup table. That table is below, and it is also the ONLY list
// of what can be driven remotely: a Command absent from it is unreachable. So
// exposing a new command is a deliberate act rather than a side effect of adding
// an enumerator somewhere else.
// -----------------------------------------------------------------------------
//
// Full design record: docs/REMOTE_TCP_IP_SPEC.md
// =============================================================================

namespace Remote {

    // Whether a command takes a value after its name.
    //
    // `Required` is not merely documentation — those commands currently raise UI
    // when driven without one (JumpToImage opens a panel, SlideshowSetInterval
    // prompts a dialog). A remote client that reached those paths would block
    // the connection until somebody physically dismissed the dialog, so the
    // parser refuses them rather than letting that happen.
    enum class PayloadRule {
        None,     // a payload is an error — catches typos rather than ignoring them
        Required, // absent payload is an error
    };

    struct CommandEntry {
        const wchar_t *name;
        Command        cmd;
        PayloadRule    payload;
        // A few words, for the Ctrl+F10 command picker and the `help` listing.
        //
        // OPTIONAL, and empty for most rows on purpose: "NextImage — next image"
        // is noise, and a table where two thirds of the descriptions restate the
        // name teaches the reader to stop reading them. Filled in only where the
        // name does NOT say it: what the payload means, what the units are, and
        // the handful of commands whose effect is not guessable.
        const wchar_t *desc = L"";
        // What the VALUE means, for the rows that take one — units, range, format.
        //
        // SEPARATE from `desc` because they answer different questions and the
        // Ctrl+F10 panel shows them in different places: the command's description
        // sits over the command box, the value's over the value box. While the two
        // shared one field, a payload row's only text described its value, so the
        // panel had nothing to say about what the command DID.
        //
        // Empty for every row that takes no value — there is nothing to describe.
        const wchar_t *valueDesc = L"";
    };

    // Why a line was rejected. Maps onto Constants::RemoteTcpIp::ERR_*.
    enum class ParseStatus {
        Ok,        // a Command to execute — see RemoteRequest::cmd
        Verb,      // a protocol-level verb, answered without touching the app
        EmptyLine,
        UnknownCommand,
        PayloadRequired,
        PayloadRejected,
        LineTooLong,
    };

    // Requests answered by the protocol layer itself rather than by the viewer.
    // They exist because they map to no `Command`: there is no enumerator for
    // "list what you accept" or "are you alive", and inventing one purely to
    // satisfy the table would put a fake entry into the app's input pipeline.
    enum class Verb {
        None,
        Help,     // the reachable-command listing, built from the table below
        Ping,     // liveness check that executes nothing
        Version,  // app version + protocol version
        // "hello <name>" — the client says what to CALL it. Optional, sent once
        // after authentication, and it changes nothing about what the connection
        // may do; it exists so the Ctrl+F12 log can name a peer instead of only
        // addressing it. An address is not an identity when three phones share a
        // router, and it is not even unique when a second instance on this very
        // machine dials in over the LAN address.
        //
        // The payload is a LABEL, never a credential. It is chosen by the peer,
        // so nothing may be decided by it — see how the server stores it.
        Hello,
        // "agent k=v;k=v;…" — who and what is at the other end.
        //
        //   agent app=qIV;ver=2.96.0.113;proto=5;platform=win;os=Windows 11;host=PCHOME
        //
        // SENT BY BOTH SIDES, once, immediately after authentication. The server
        // has always introduced itself in the banner; this is the client saying
        // the same kind of thing back, in a form that parses.
        //
        // KEY=VALUE, SEMICOLON-SEPARATED, because that is what this protocol
        // already speaks — QueryToggles and sync both answer in exactly this
        // shape. A new grammar for one message would be a second thing to write
        // a parser for.
        //
        // UNKNOWN KEYS ARE IGNORED, not refused. That is what lets a field be
        // added later without either end needing to know about it first.
        //
        // EVERY VALUE IS PEER-CHOSEN, therefore every value is a HINT. A phone
        // may claim `platform=win` and any client may claim any hostname. These
        // fields exist to label a row in a list. Nothing about access,
        // capability or routing may be decided from them — the AllowList, the
        // password and TLS are what govern this connection, and none of them
        // read this.
        Agent,
        // "bye" — the client is closing on purpose.
        //
        // WHY THIS HAS TO EXIST. A clean TCP close, a reset, a phone going out
        // of Wi-Fi range and a crashed client all arrive here as the same thing:
        // a failed read. The socket layer cannot tell them apart, so without a
        // word from the client there is no way to know whether a departure was
        // deliberate.
        //
        // Optional, and everything still works without it — a client that never
        // says it simply departs as "closed by peer", which is what every client
        // did before. It exists so the overlay can show a normal disconnect
        // differently from a screen that vanished, because on a wall those mean
        // opposite things: one is somebody finishing, the other is something to
        // go and look at.
        //
        // Carries no payload and grants nothing. The server answers, then the
        // client closes.
        Bye,
    };

    // The parsed contents of an `agent` line. Every field is optional and empty
    // when absent — a peer that sends none is simply a peer that said nothing,
    // which is a normal state and not an error.
    struct AgentInfo {
        std::wstring app;       // "qIV", "qIVRemote"
        std::wstring version;   // the app's own version
        std::wstring platform;  // "win", "android" — anything else is dropped
        std::wstring os;        // "Windows 11 26200", "Android 14"
        std::wstring host;      // machine or device name

        // The name the USER gave this instance — the server's configured Name on
        // the desktop, the one typed in Settings on the phone. Distinct from
        // `host`: that is what the machine calls itself, this is what a person
        // decided to call it, and on a wall of screens only the second is useful.
        //
        // Also carried by `hello`, which predates this and still works. A client
        // may send either or both; the server keeps the last one it was told.
        std::wstring name;
    };

    // Parses an `agent` payload. Sanitising is part of parsing rather than a
    // step a caller might forget: values are peer-chosen text that ends up in a
    // list and a log, so control characters and the separators themselves are
    // stripped here, once, for every field.
    AgentInfo ParseAgent(const std::wstring &payload);

    // Builds this instance's own agent line payload. One place, so the two ends
    // of this program cannot describe themselves differently.
    //
    // `instanceName` is the user's name for this instance — the server's
    // configured Name. EMPTY FIELDS ARE OMITTED rather than sent blank: `os=`
    // with nothing after it is not a fact, and a reader cannot tell it from a
    // value lost on the way. Absent means unknown.
    std::wstring BuildAgent(const std::wstring &appName, const std::wstring &appVersion,
                            const std::wstring &instanceName);

    // A field as it should APPEAR — the value, or "?" when it is unknown.
    //
    // The wire omits; the screen shows a question mark. Those are different jobs
    // and doing them in one place each is what stops a panel drawing a blank gap
    // that reads as "nothing" when it means "not told".
    std::wstring AgentField(const std::wstring &value);

    // WHAT THE PEER IS to this instance.
    //
    // Named for the peer's role rather than for the direction of the socket,
    // because that is the question a person is actually asking. "Inbound" is a
    // fact about a TCP connection; "Client" is what the thing on the other end
    // IS — something using this viewer's listener. Ctrl+F9 lists clients,
    // Ctrl+F11 lists servers, and the word in the row should be the word in the
    // panel's own title.
    //
    // NOT A FIELD OF AgentInfo, and the distinction matters. Everything in that
    // struct is what the PEER SAID about itself; this is what THIS MACHINE
    // KNOWS, because it either accepted the connection or made it. A peer cannot
    // be wrong about this and cannot lie about it, so it does not belong in the
    // same container as the things it chooses.
    enum class AgentRole {
        Client,   // it dialled us — something using our listener      (Ctrl+F9)
        Server,   // we dialled it — a listener this instance drives   (Ctrl+F11)
    };

    // One line describing a peer, for any panel that shows one:
    //
    //   🙋 Client  ·  App qIVRemote 1.0.0  ·  OS Android 14  ·  Host Pixel 8
    //   📡 Server  ·  App qIV 2.96.0.113  ·  OS Windows 11 26200  ·  Host PCHOME
    //
    // SHARED BY BOTH DIRECTIONS ON PURPOSE. My Clients (Ctrl+F9) lists the
    // connections that came in; Mirroring (Ctrl+F11) lists the ones this
    // instance dialled out. They are different lists of different things, but a
    // peer is described the same way in both — and two panels formatting the
    // same struct by hand is exactly how one of them ends up saying "unknown"
    // where the other says "?".
    //
    // The ROLE leads the line because it is the one part that is certain.
    // Everything after it is hearsay, and reading it in that order is the point.
    //
    // The WORD carries the meaning; the glyph is decoration beside it. That is
    // the opposite of the scope column in My Clients, where the glyph IS the
    // information — worth knowing, because it means this pair can be swapped for
    // taste without anything becoming unreadable.
    std::wstring DescribeAgent(const AgentInfo &info, AgentRole role);

    struct RemoteRequest {
        Command      cmd     = Command::None;
        Verb         verb    = Verb::None;   // set when status == Verb
        std::wstring payload;                // empty unless the command takes one
        ParseStatus  status  = ParseStatus::EmptyLine;
        std::wstring rawName;                // as received, for the error message
    };

    // Parses one received line. Does NOT execute anything and does not touch
    // `app` — safe to call from the socket thread, which is the point.
    // Leading/trailing whitespace and a trailing \r (telnet, CRLF clients) are
    // tolerated. Command names are case-insensitive.
    RemoteRequest ParseLine(const std::wstring &line);

    // The reachable-command table and its size. Exposed so the panel and the
    // `help` verb can list exactly what this build accepts, with no second list
    // to fall out of step.
    const CommandEntry *CommandTable(size_t &countOut);

    // Name → Command, case-insensitive. Returns false when unreachable.
    bool LookupCommand(const std::wstring &name, const CommandEntry *&entryOut);

    // Command → the name to put on the wire. The FIRST matching row wins, which
    // is why the short alias is listed before the canonical enum name: a mirrored
    // session sends "next", not "NextImage". Returns false for a Command that has
    // no row, i.e. one that is not remotely reachable at all.
    bool NameForCommand(Command cmd, std::wstring &nameOut);

    // The wire word for a blank-screen state lives in AppState.h, beside the
    // enum it spells — FolderOverlayWireWord. It belongs to the protocol, but
    // the enum is nested and cannot be forward-declared, so declaring it here
    // would mean this header pulling AppState.h (and IRenderer.h, and wincodec)
    // into every file that only wanted the command table.

    // --- Mirroring ------------------------------------------------------------
    // Whether a command should be forwarded to the connected targets when
    // mirroring is on.
    //
    // THIS IS NOT THE SAME QUESTION AS "is it in the table above". The table is
    // the SCRIPTING surface and is deliberately permissive: a screen-management
    // script legitimately wants `quit`, `HideToTray`, `NewWindow` and the window
    // geometry commands. Mirroring is a different contract — a keystroke fanned
    // out to every connected screen at once — and the same commands are wrong
    // there. One Ctrl+Q would kill every slave; one HideToTray would send them
    // all to a tray nobody is sitting in front of; a Snap would knock a screen
    // off the monitor it was placed on.
    //
    // Anything refused here stays reachable individually through the F9 panel's
    // Send box. Refusing to fan it out is not refusing to allow it.
    bool IsMirrorable(Command cmd);

    // Narrower still, for a target on ANOTHER MACHINE. Drops everything that
    // depends on the two ends holding the same files: an index means nothing
    // against a different playlist, and a path means nothing against a different
    // drive. What remains — navigation, zoom, effects, view mode, slideshow —
    // each instance applies to its own content, which is the useful behaviour
    // when the folders differ anyway.
    bool IsMirrorableRemote(Command cmd);

    // --- Observing --------------------------------------------------------
    // Whether a command is worth PUSHING TO A WATCHER once this instance has
    // done it. A third question over the same enum, and not the mirroring one:
    // fan-out sends a bare name before the user has typed the value, while an
    // echo reports something already done and holds the value in its hand.
    //
    // The observer echo used to ask IsMirrorable, which refuses every payload
    // row for that fan-out reason — so `SlideshowSetInterval 5000` and
    // `OpenFile <path>` told observers nothing at all. Two phones on one viewer
    // is where that shows: the one that acted knew from its own reply, and the
    // other went stale with nothing to tell it.
    //
    // It is NOT more permissive about anything else. An observer executes what
    // it receives, so every safety exclusion mirroring makes — HardQuit, window
    // geometry, the panel toggles — applies here unchanged.
    //
    // `hasValue` says the caller holds a payload. It admits the commands that
    // are denied ONLY because their bare form raises a panel but that mean a
    // plain state change once a value is attached — `ZoomTo` today. Left false
    // by the bare echo, so a payload-less verb is never put on the wire for
    // every observer to refuse.
    bool IsAnnounceable(Command cmd, bool hasValue = false);

    // Whether that command's VALUE only means something on this machine — a
    // path, an index. Such an announcement goes to same-machine observers only,
    // via the `positional` argument of EmitToObservers, because a path that
    // resolves on the far end names a DIFFERENT picture. See §5.
    bool IsMachineSpecificPayload(Command cmd);

    // =========================================================================
    // TWO KINDS OF "NO", and they are not the same kind.
    //
    // IsNeverRemote — a structural rule. These alter files, so they must not be
    //   reachable over a socket under ANY configuration: no password, no
    //   allow-list entry, no future protocol row makes them available. Enforced
    //   twice over — a static_assert proves they have no row in the command
    //   table (adding one fails the build), and the wire path refuses them again
    //   at run time in case the table is ever bypassed.
    //
    //   This matters more than it looks. Authentication here happens ONCE, at
    //   connection time; the lines after it carry no per-message signature. On
    //   loopback that is irrelevant. Reachable from a network it means anyone
    //   who can inject into an established session speaks as the authenticated
    //   caller — and the answer to that is not stronger authentication, it is
    //   that destructive operations were never on the menu.
    //
    // IsBlockedInSession — a policy. These are perfectly fine on an instance
    //   that is on its own, and unsafe only while it is connected to another:
    //   they either change the file set both ends are counting on, or they mean
    //   something different on each end. Refused with a reason while any
    //   connection is live, allowed again when the last one drops.
    // =========================================================================

    bool IsNeverRemote(Command cmd);

    // `reasonOut` receives a short human-readable why, for the overlay message.
    // A dropped command must always say what happened — silence reads as a bug.
    bool IsBlockedInSession(Command cmd, const wchar_t *&reasonOut);

    // IsBlockedInSession AND a session is actually live. The form every caller
    // wants, and the reason it exists rather than each site writing the two
    // halves itself: with two conditions spelled out in six places, one of them
    // eventually gets only half the test.
    //
    // TWO callers by design. ExecuteCommand covers every command path — which
    // is most of the application. The thumbnail panel's file operations are the
    // exception: they act on the hovered item or the selection, carry cut/paste
    // state between calls, and are reachable by drag-and-drop as well as by
    // menu, so they are not commands and cannot be made into them without moving
    // that state out of the panel that owns it. They ask this instead, at the
    // gates they already have. One table, one question, two places that ask it.
    bool BlockedNow(Command cmd, const wchar_t *&reasonOut);

    // --- Response construction ---------------------------------------------
    std::wstring MakeOk();
    std::wstring MakeOk(const std::wstring &text);
    std::wstring MakeErr(int code, const std::wstring &reason);

    // ParseStatus → the ERR line a client should receive.
    std::wstring MakeErrFor(const RemoteRequest &req);

    // Multi-line listing of every reachable command, for the `help` verb.
    std::wstring BuildHelpText();

    // --- Encoding -----------------------------------------------------------
    // The wire is UTF-8; everything inside qIV is UTF-16. Conversions live here
    // so the socket layer never open-codes them.
    std::string  ToUtf8(const std::wstring &s);
    std::wstring FromUtf8(const char *data, size_t len);

    // --- Socket options -----------------------------------------------------
    // Turns on TCP keepalive with this program's intervals. Called by BOTH ends
    // — the server on each accepted socket, the client on each connected one —
    // because a NAT mapping is dropped for both and whichever side notices first
    // is the one that recovers. See KEEPALIVE_IDLE_MS.
    //
    // Best effort by design: the socket is perfectly usable without it (that is
    // the state everything was in before), so a failure is not worth failing a
    // connection over and there is nothing a caller could do about it anyway.
    // SOCKET is spelled UINT_PTR here so this header stays free of winsock2.h,
    // whose include ORDER is load-bearing in this project.
    void EnableKeepAlive(UINT_PTR sock);

    // --- Addresses ----------------------------------------------------------
    // "host:port" for display, with IPv6 literals BRACKETED: "[fe80::1]:5555".
    //
    // Not cosmetic. An IPv6 literal is full of colons, so gluing one to a port
    // with another colon produces "fe80::1:5555" — which is itself a valid IPv6
    // address, and the reader cannot tell where the address stopped. "::" is
    // worse still: it renders as ":::5555". Brackets are the RFC 3986 answer and
    // the one every other tool prints, so a person reading a qIV panel sees the
    // same shape they see everywhere else.
    //
    // DISPLAY ONLY. Nothing parses this back, and getaddrinfo does not accept
    // brackets — host and port are stored as separate fields precisely so this
    // string never has to be taken apart again.
    std::wstring FormatEndpoint(const std::wstring &host, int port);

    // Removes one surrounding pair of brackets from a typed address, so pasting
    // "[fe80::1]" back out of a panel into a host field connects rather than
    // failing to resolve. A no-op on everything else.
    std::wstring StripAddressBrackets(const std::wstring &host);

    // HOW FAR AWAY an address is, as one glyph: 🏠 this machine, 🖧 your LAN,
    // 🌐 somewhere public.
    //
    // SHARED BY EVERY PANEL THAT LISTS A PEER — My Clients (Ctrl+F9) and Servers
    // (F10) both show one, and they must agree. It began as a private helper in
    // one of them, which is exactly how this codebase ended up with three copies
    // of the monitor enumeration.
    //
    // READ FROM THE ADDRESS, never from anything the peer said about itself.
    // That is what makes it trustworthy at a glance: a peer chooses its `hello`
    // name and its `agent` fields, but it cannot choose the address the socket
    // reports — and for an outbound server, the address is one the user typed.
    //
    // The third case is the one that earns the glyph. A public address means
    // this connection leaves the network, which is either deliberate
    // port-forwarding or something the operator very much wants to notice.
    //
    // `sameMachine` short-circuits it where the caller already knows — the
    // server records it per connection, and a hostname that resolves to loopback
    // is not something this function can see for itself.
    const wchar_t *ScopeIcon(const std::wstring &address, bool sameMachine = false);

} // namespace Remote
