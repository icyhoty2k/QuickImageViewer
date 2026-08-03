#pragma once
#include <windows.h>
#include <string>
#include <vector>

// =============================================================================
// RemoteSettings — configuration for qIV's TCP/IP remote control.
//
// THE LISTENER NEVER STARTS BY ITSELF UNLESS ASKED. Three ways it comes up:
//
//   1. command-line switches   (-remote, -remotePort=…, …)
//   2. Autostart=true in qivLocalServer.ini
//   3. the Start button in the F9 panel — always available, and deliberately
//      not gated on Autostart, which describes launch behaviour and nothing else
//
// There is deliberately no registry default. Name, port and connection cap DO
// have defaults now — nothing binds a socket without Autostart or an explicit
// Start, so pre-filling them costs no safety and saves filling in fields with
// one obvious answer.
//
// FILE LAYOUT — qivLocalServer.ini, beside the exe and owned entirely by this
// subsystem:
//
//     [REMOTE_TCP_IP]
//     Autostart=false
//     Name=qIV
//     IpAddress=127.0.0.1
//     PortNo=8770
//     AllowList=192.168.1.10,192.168.1.11
//     Password=<hash>
//     MaxConnections=4
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
//   • Autostart defaults false, and the file is usually absent entirely
//   • bind address defaults to loopback, not 0.0.0.0
//   • an EMPTY AllowList denies everyone rather than allowing everyone
//   • the blacklist always beats AllowList — it is gate 1, AllowList is gate 2
//   • MaxConnections defaults to 4
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
        // Start the listener automatically at launch. NOT a master switch: the
        // F9 panel's Start button works regardless, and always did the work —
        // it was only refused because this flag doubled as a gate, so pressing
        // Start could answer "Remote server disabled", which is a sentence with
        // no useful action in it.
        //
        // False by default, and the file is usually absent entirely, so a viewer
        // nobody configured still never opens a socket on its own.
        bool autostart = false;

        // How this instance identifies itself to a connecting client.
        // Defaults to NAME_DEFAULT so F9 opens ready to run; distinct names
        // still matter once several instances are driven at once.
        std::wstring name;

        // BIND address — which local interfaces the listener accepts on.
        // "127.0.0.1" = this machine only. "0.0.0.0" = every interface.
        // NOT the same field as the client's connect-to target.
        std::wstring bindAddress;

        // 0 = not configured, and still refused — a hand-edited file can say
        // PortNo=0. Config() seeds PORT_DEFAULT.
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
        // Literal rather than MAX_CONNECTIONS_DEFAULT: this header does not
        // include Constants.h, and Config() overwrites it from the constant
        // anyway — this only covers a default-constructed Settings.
        int maxConnections = 4;
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
        bool         autostart = false; // -remote; only ever turns it ON
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

    // Five pattern forms, in the order they are tried:
    //
    //   *                          everything
    //   192.168.1.*                TEXT prefix — see the warning below
    //   192.168.0.0/24             CIDR, either family (2001:db8::/32)
    //   192.168.0.10-50            range, IPv4 only; also 10-192.168.0.50
    //   192.168.0.5                exact, compared NUMERICALLY when both sides
    //                              parse, so "::1" and its expanded spelling are
    //                              one address and a "%scope" suffix on the peer
    //                              does not defeat the rule
    //
    // THE STAR IS A STRING PREFIX, not an octet boundary. "192.168.1.*" is what
    // it looks like, but "192.168.1*" — no trailing dot — also covers
    // 192.168.10.x and 192.168.100.x, and "1*" covers most of the internet. It
    // stays that way because existing lists mean it; /N is the form to reach for
    // when the boundary matters.
    //
    // Here rather than inside the server because the AllowList and the blacklist
    // must agree on what "matches" means, and they now live in different files
    // and different translation units. Two copies of this rule is two chances
    // for an address to be allowed by one and not blocked by the other.
    bool AddressMatches(const std::wstring &pattern, const std::wstring &addr);
    bool InList(const std::vector<std::wstring> &list, const std::wstring &addr);

    // True when an entry could plausibly be an address literal — digits, hex,
    // dots, colons, the trailing star, and the '/' and '-' of the two ranged
    // forms. Those two are additionally required to PARSE: "192.168.0.0/99" is
    // spelled out of legal characters and can never match anything, which is the
    // sort of entry this exists to drop. Anything rejected is pruned by
    // Normalize and reported by the panel rather than kept and disbelieved.
    //
    // DOMAIN NAMES ARE NOT ADDRESSES and are refused here. A rule is checked
    // against the address a connection actually arrived from; resolving a name
    // per connection would put DNS — and whoever answers it — inside the access
    // decision.
    bool LooksLikeAddress(const std::wstring &s);

    // THE UNIT A PUNISHMENT APPLIES TO, which is not always a single address.
    //
    // Counting failed passwords per exact address is the obvious rule and it is
    // useless against IPv6. A residential v6 connection is handed a /64 — 2^64
    // addresses, all equally usable, and changing between them costs nothing.
    // An attacker rotates after every fifth guess and no counter ever reaches
    // its threshold; worse, the rotation evicts the table's real entries and
    // fills the blacklist with rows that each block one address out of
    // eighteen quintillion.
    //
    // So a v6 peer is tracked and blocked as its /64, returned as a CIDR string
    // ("2001:db8:abcd:1234::/64") that AddressMatches already understands — the
    // blacklist stores it verbatim and matches the whole prefix with no changes
    // of its own.
    //
    // v4 is returned UNCHANGED. There the address is the smallest unit a peer
    // actually controls, and widening to a prefix would punish a whole ISP —
    // and under CGNAT, thousands of unrelated subscribers — for one attacker.
    //
    // THE COST, stated plainly: a /64 is one household, so this blocks the
    // attacker's whole connection rather than one address of it. That is the
    // intent. It also means a legitimate client sharing that /64 is blocked
    // with them, which is the correct trade when the alternative is a guard
    // that does not work at all.
    std::wstring BlockScope(const std::wstring &address);

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
