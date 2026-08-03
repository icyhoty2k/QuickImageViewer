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
    };

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

} // namespace Remote
