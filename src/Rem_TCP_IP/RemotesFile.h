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
