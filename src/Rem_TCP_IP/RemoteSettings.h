#pragma once
#include <windows.h>
#include <string>
#include <vector>

// =============================================================================
// RemoteSettings — configuration for qIV's TCP/IP remote control.
//
// THE LISTENER IS OFF UNLESS EXPLICITLY ENABLED. Two ways in, and only two:
//
//   1. command-line switches   (-remote, -remotePort=…, …)
//   2. a [REMOTE_TCP_IP] section in the instance .ini
//
// There is deliberately no registry default and no "sensible" fallback port. A
// viewer nobody configured for remote control never binds a socket.
//
// FILE LAYOUT — qivLocalServer.ini, beside the exe and owned entirely by this
// subsystem:
//
//     [REMOTE_TCP_IP]
//     Enable=false
//     Name=Lobby-Screen
//     IpAddress=127.0.0.1
//     PortNo=8770
//     AllowList=192.168.1.10,192.168.1.11
//     Password=<hash>
//     MaxConnections=1
//
// The BLACKLIST is not in here. It is qivRemoteServerBlacklist.ini — see
// RemoteBlacklist.h for why the one list qIV writes to by itself is kept apart
// from the settings a person edits.
//
// ITS OWN FILE, and this used to be a section of the instance .ini. That was a
// real hazard rather than an untidiness: an .ini named after the exe existing at
// all is what makes the WHOLE application file-backed
// (Dedicated::DetectStartupMode), so saving a port number from the F9 panel
// could silently move every unrelated setting off the registry onto a file that
// contained none of them. A whole seeding mechanism existed to make that
// survivable; a separate file removes the problem instead of managing it.
//
// A fixed name is invisible to DetectStartupMode, so nothing here can affect
// where anything else is stored — same arrangement as the remote-servers list.
//
// SECURITY POSTURE — fail closed at every step:
//   • Enable defaults false
//   • bind address defaults to loopback, not 0.0.0.0
//   • an EMPTY AllowList denies everyone rather than allowing everyone
//   • the blacklist always beats AllowList — it is gate 1, AllowList is gate 2
//   • MaxConnections defaults to 1
//   • no password refuses to start at all off loopback
//
// 127.0.0.1 is SEEDED into a new AllowList so a fresh instance is reachable
// from the copy beside it without anyone meeting the fail-closed rule as a
// mystery. It is an ordinary entry with no special status: delete it and this
// machine is locked out like any other, which is the point — a list with
// built-in exceptions is not a list.
//
// Full design record: docs/REMOTE_TCP_IP_SPEC.md
// =============================================================================

namespace Remote {

    struct Settings {
        // Master switch. Nothing binds a socket while this is false.
        bool enable = false;

        // How this instance identifies itself to a connecting client.
        std::wstring name;

        // BIND address — which local interfaces the listener accepts on.
        // "127.0.0.1" = this machine only. "0.0.0.0" = every interface.
        // NOT the same field as the client's connect-to target.
        std::wstring bindAddress;

        // 0 = not configured. The server refuses to start without a real port.
        int port = 0;

        // IPs permitted to connect. EMPTY MEANS DENY EVERYONE — fail closed.
        // Seeded with 127.0.0.1, which is an ordinary entry and can be removed;
        // removing it locks this machine out, exactly as removing any other
        // entry locks that one out.
        std::vector<std::wstring> allowList;

        // NOTE: the blacklist is NOT here. It lives in its own file and its own
        // module (RemoteBlacklist.h), because it is the one list the PROGRAM
        // writes — the brute-force guard appends to it unattended — and because
        // a snapshot of it taken at Start() would mean an address blocked at
        // 04:11 was still admitted until the next restart.

        // Stored hashed, never plaintext. Empty = no password configured.
        std::wstring passwordHash;

        // Simultaneous clients, clamped to [MAX_CONNECTIONS_MIN, MAX].
        int maxConnections = 1;
    };

    // The process-wide remote configuration. Owned and mutated by the UI thread
    // only — the socket thread reads a snapshot taken at Start().
    Settings &Config();

    // --- .ini access -----------------------------------------------------------

    // True when qivLocalServer.ini actually carries a configured section.
    // Distinguishes "configured off" from "never configured", which the panel
    // shows differently.
    bool SectionExists();

    // Reads qivLocalServer.ini into Config(). Absent keys keep their defaults;
    // out-of-range or unparseable values fall back to the default rather than
    // failing the load, so one bad line never stops the app from starting.
    // No-op when the file is not there.
    void LoadFromIni();

    // Writes every field back, one key at a time, creating the file (UTF-16LE +
    // BOM) if needed. Creating it has NO effect on how the application persists
    // anything else — which is the entire point of it being a separate file.
    void SaveToIni();

    // True when qivLocalServer.ini exists.
    bool IniExists();

    // --- Command-line overrides ------------------------------------------------
    // Filled by CMDArgs and layered on top of whatever LoadFromIni produced, so
    // a configured screen can be launched without repeating its configuration.
    // Its own struct rather than the CmdArgs one, so Rem_TCP_IP stays independent
    // of the command-line parser.
    //
    // "Absent" is encoded per field: empty string, or -1 for the two numbers.
    // An absent switch NEVER overwrites a stored value — that is what lets
    // `-remote` alone start a fully .ini-configured listener.
    struct Overrides {
        bool         enable = false;   // -remote; only ever turns it ON
        std::wstring name;
        std::wstring bindAddress;
        std::wstring allowList;        // raw, comma/semicolon separated
        std::wstring blackList;        // raw, comma/semicolon separated — appended
                                       // to qivRemoteServerBlacklist.ini, since
                                       // that file is now the only blacklist
        std::wstring plainPassword;    // hashed here; never stored as given
        int          port           = -1;
        int          maxConnections = -1;
    };

    // Applies the overrides to Config() and normalizes the result.
    // Does NOT write to disk — a command line describes one launch, not a
    // permanent reconfiguration of the instance.
    void ApplyOverrides(const Overrides &o);

    // --- List helpers ----------------------------------------------------------

    // Splits a comma/semicolon separated list into trimmed, non-empty entries.
    // Tolerates whitespace and trailing separators from hand-edited files.
    std::vector<std::wstring> ParseList(const std::wstring &raw);

    // Literal match, plus a trailing "*" wildcard so "192.168.1.*" covers a
    // subnet without spelling out 254 entries. Deliberately NOT a CIDR parser:
    // hand-edited text files get CIDR subtly wrong, and a rule that silently
    // matches more than the author intended is worse than no rule.
    //
    // Here rather than inside the server because the AllowList and the blacklist
    // must agree on what "matches" means, and they now live in different files
    // and different translation units. Two copies of this rule is two chances
    // for an address to be allowed by one and not blocked by the other.
    bool AddressMatches(const std::wstring &pattern, const std::wstring &addr);
    bool InList(const std::vector<std::wstring> &list, const std::wstring &addr);

    // True when an entry could plausibly be an address literal — digits, hex,
    // dots, colons and the trailing star. Anything else is dropped rather than
    // silently compared against and never matched.
    bool LooksLikeAddress(const std::wstring &s);

    // Rejoins for storage. Comma-separated, no spaces.
    std::wstring JoinList(const std::vector<std::wstring> &items);

    // --- Validation ------------------------------------------------------------

    // Clamps port and maxConnections into their legal ranges and drops malformed
    // list entries. Called by LoadFromIni and before any Start.
    void Normalize(Settings &s);

    // True for 127.0.0.0/8, ::1 and "localhost" — a bind address only this
    // machine can reach. Exposed because the password requirement hangs off it:
    // an unauthenticated listener is a local convenience and a remote hole, and
    // the bind address is what separates the two.
    bool IsLoopbackBind(const std::wstring &addr);

    // Human-readable reason the server cannot start, or empty when it can.
    // Drives the panel's status line so a refusing server never looks like a bug:
    // an enabled server with an empty AllowList denies every connection, which
    // must be stated rather than left to be discovered.
    std::wstring WhyCannotStart(const Settings &s);

} // namespace Remote
