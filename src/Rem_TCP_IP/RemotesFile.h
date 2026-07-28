#pragma once
#include <windows.h>
#include <string>
#include <vector>

// =============================================================================
// RemotesFile — the list of instances THIS copy drives, kept beside the exe.
//
//     [Remotes]
//     1=Monitor2,127.0.0.1,8771,hunter2,1,D:\qIV\qIV_dedicated_Mon2.exe
//     2=Monitor3,127.0.0.1,8772,hunter2,1,D:\qIV\qIV_dedicated_Mon3.exe
//
// Fields, in order: Name, IP, Port, Password, AutoConnect, ExePath.
//
// THE NAME IS THE IDENTITY. It is what the console shows, what a message names
// when something fails, and what makes a row the same row after its port or its
// folder changes — a screen called "Monitor2" stays Monitor2 when it moves. So
// names must be unique, and LoadRemotes drops a duplicate rather than keeping
// two rows that no message could tell apart.
//
// Host+port has to stay unique as well, for a different reason: two rows
// pointing at one instance would both drive it, and it would receive every
// mirrored command twice. That is checked when a row is added, not here — a
// hand-edited file with two names for one address is odd but harmless, while
// two rows with one name is genuinely ambiguous.
//
// -----------------------------------------------------------------------------
// A SEPARATE FILE, NOT A SECTION OF THE INSTANCE .ini — and that is load-bearing.
//
// Dedicated::DetectStartupMode decides where the WHOLE APPLICATION keeps its
// settings by looking for an .ini named after the exe (qIV.exe → qIV.ini). Put
// this list in there and a registry-backed master would silently become
// file-backed on its next launch, with every unrelated preference reverting to
// its default — which is the accident RemoteSettings::SaveToIniSeeded exists to
// contain.
//
// "qivRemotes.ini" is not the exe-derived name, so that check never sees it.
// Creating it changes nothing about how the rest of the app persists itself,
// whichever mode the copy is in.
//
// It also belongs to the DRIVING side only. RemoteWnd's peer fields were
// deliberately session-only on the grounds that "an address you are driving
// from this machine is not part of THIS instance's configuration" — true of a
// screen that is driven, wrong for the one doing the driving, which needs its
// list of screens to survive a restart. Slaves never read this file.
// -----------------------------------------------------------------------------
//
// THE PASSWORD IS STORED AS TYPED. Deliberate, and the reasoning is the setup:
// every instance is on one machine, the listeners bind 127.0.0.1, and anyone who
// can read a file next to the exe is already running as the user who owns every
// one of these viewers — a password would be protecting them from themselves.
//
// What that reasoning does NOT cover is the folder travelling: a portable exe
// directory copied to a USB stick, synced, or zipped and sent takes the file
// with it. Keeping the listeners on loopback is what makes that harmless, since
// a password for a port nothing off-machine can reach is worth nothing. The two
// decisions hold each other up; changing either one alone breaks the argument.
//
// Full design record: docs/REMOTE_TCP_IP_SPEC.md
// =============================================================================

namespace Remote {

    struct RemoteEntry {
        std::wstring name;
        std::wstring host;
        int          port = 0;
        std::wstring password;
        bool         autoConnect = false;
        // Full path to the exe to launch when the console's start button is
        // pressed on a target that is not running. Each slave is its own renamed
        // copy (*dedicated*.exe), so this is a distinct binary per row, and the
        // .ini beside it carries that screen's whole configuration.
        std::wstring exePath;
    };

    // Marks a password field that holds an already-derived SECRET rather than a
    // typed password: "secret:<salt-hex>$<digest-hex>", exactly the two halves
    // the target's own settings file stores.
    //
    // The prefix exists so the two forms are told apart without guessing. A
    // plaintext password could in principle look like "abc$def", and a wrong
    // guess would mean silently attempting the wrong authentication route.
    constexpr const wchar_t *SECRET_PREFIX = L"secret:";

    bool IsStoredSecret(const std::wstring &passwordField);

    // Splits a "secret:<salt>$<digest>" field. False when it is not one, or is
    // malformed.
    bool SplitStoredSecret(const std::wstring &passwordField,
                           std::wstring &saltHexOut, std::wstring &digestHexOut);

    // =========================================================================
    // IMPORT — read another instance's settings file and build a row from it.
    //
    // Point at a dedicated instance's .ini (or its .exe, and the .ini beside it
    // is found by swapping the extension, exactly as Dedicated::SettingsFilePath
    // derives it) and everything needed to drive it comes out: port, name, exe,
    // and the credentials.
    //
    // THE CREDENTIALS COME OUT TOO, and that is the point rather than an
    // oversight. A server stores "salt$digest" and never holds the plaintext;
    // the digest IS the shared secret. Anything that can read that file can
    // already authenticate to that instance — so importing it is a complete
    // setup with nothing to type, and it grants no access that reading the file
    // did not already grant.
    //
    // `problemOut` is a hard failure — nothing usable was found. `warningOut` is
    // something that WILL stop the connection working but is fixable at the
    // other end, and is worth saying before the attempt rather than after a
    // timeout: a listener that is disabled, or an AllowList that does not
    // include this machine. Those two are invisible from the outside — the
    // connection simply never completes — and are the most common reasons a
    // correctly-typed target does not answer.
    bool ImportFromInstanceFile(const std::wstring &chosenPath,
                                RemoteEntry &entryOut,
                                std::wstring &problemOut,
                                std::wstring &warningOut);

    // <exe folder>\qivRemotes.ini — resolvable whether or not the file exists.
    const std::wstring &RemotesFilePath();
    bool RemotesFileExists();

    // Reads every [Remotes] row. Returns empty when the file is absent, which is
    // the ordinary state for a copy that drives nothing.
    std::vector<RemoteEntry> LoadRemotes();

    // Rewrites the [Remotes] section from `entries`. Creates the file (UTF-16LE
    // + BOM — the Win32 profile API only writes Unicode into a file that is
    // already Unicode, and paths here can contain non-ASCII characters).
    //
    // Rewrites rather than merges: this file has exactly one section and one
    // owner, so there is nothing else in it to preserve.
    void SaveRemotes(const std::vector<RemoteEntry> &entries);

} // namespace Remote
