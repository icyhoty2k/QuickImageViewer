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
// FILE LAYOUT — a section of the same .ini the Dedicated subsystem already owns:
//
//     [REMOTE_TCP_IP]
//     Enable=false
//     Name=Lobby-Screen
//     IpAddress=127.0.0.1
//     PortNo=8770
//     AllowList=192.168.1.10,192.168.1.11
//     Password=<hash>
//     BlackList=
//     MaxConnections=1
//
// Reads and writes go through Dedicated::ReadSectionString / WriteSectionString,
// which write ONE KEY AT A TIME. That matters: the same file holds [Instance] and
// [Settings], and a wholesale rewrite would silently eat them.
//
// SECURITY POSTURE — fail closed at every step:
//   • Enable defaults false
//   • bind address defaults to loopback, not 0.0.0.0
//   • an EMPTY AllowList denies everyone rather than allowing everyone
//   • BlackList always beats AllowList
//   • MaxConnections defaults to 1
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
        std::vector<std::wstring> allowList;

        // IPs always refused, checked BEFORE allowList. Deny overrides allow.
        std::vector<std::wstring> blackList;

        // Stored hashed, never plaintext. Empty = no password configured.
        std::wstring passwordHash;

        // Simultaneous clients, clamped to [MAX_CONNECTIONS_MIN, MAX].
        int maxConnections = 1;
    };

    // The process-wide remote configuration. Owned and mutated by the UI thread
    // only — the socket thread reads a snapshot taken at Start().
    Settings &Config();

    // --- .ini access -----------------------------------------------------------

    // True when the .ini actually carries a [REMOTE_TCP_IP] section. Distinguishes
    // "configured off" from "never configured", which the panel shows differently.
    bool SectionExists();

    // Reads [REMOTE_TCP_IP] into Config(). Absent keys keep their defaults;
    // out-of-range or unparseable values fall back to the default rather than
    // failing the load, so one bad line never stops the app from starting.
    // No-op when the app is registry-backed (no .ini exists).
    void LoadFromIni();

    // Writes every field back to [REMOTE_TCP_IP], one key at a time. Creates the
    // file (UTF-16LE + BOM, [Instance] header first) when it does not exist.
    //
    // CAUTION: creating an .ini where none existed flips the WHOLE APP from
    // registry-backed to file-backed on the next launch — see
    // Dedicated::DetectStartupMode(). Callers that may be creating the file must
    // seed [Settings] with the current values first, or every unrelated setting
    // silently reverts to its default. SaveToIniSeeded() does this correctly.
    void SaveToIni();

    // The safe way for the panel to persist. Does what SaveToIni does, but when
    // the .ini does NOT already exist it first writes every current app setting
    // into [Settings].
    //
    // Why that matters: an .ini beside the exe is what makes the app file-backed
    // (Dedicated::DetectStartupMode). Creating one to hold a port number would
    // otherwise silently move the entire app off the registry onto a file that
    // contains none of its settings, and every unrelated preference would revert
    // to its default on the next launch. Seeding makes the switch lossless
    // instead of merely disclosed.
    //
    // `createdFileOut` reports whether the file was created by this call, so the
    // caller can tell the user the persistence model just changed.
    void SaveToIniSeeded(bool &createdFileOut);

    // True when an .ini already exists for this instance. The panel uses it to
    // decide whether saving is an ordinary write or a first-time creation with
    // the consequence above.
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
        std::wstring blackList;        // raw, comma/semicolon separated
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

    // Rejoins for storage. Comma-separated, no spaces.
    std::wstring JoinList(const std::vector<std::wstring> &items);

    // --- Validation ------------------------------------------------------------

    // Clamps port and maxConnections into their legal ranges and drops malformed
    // list entries. Called by LoadFromIni and before any Start.
    void Normalize(Settings &s);

    // Human-readable reason the server cannot start, or empty when it can.
    // Drives the panel's status line so a refusing server never looks like a bug:
    // an enabled server with an empty AllowList denies every connection, which
    // must be stated rather than left to be discovered.
    std::wstring WhyCannotStart(const Settings &s);

} // namespace Remote
