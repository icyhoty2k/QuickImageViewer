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

} // namespace Remote
