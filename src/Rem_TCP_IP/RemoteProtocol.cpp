#include "RemoteProtocol.h"
#include "RemoteMirror.h" // SessionActive — the switch BlockedNow hangs off
#include "Platform/Constants.h"

#include <algorithm>
#include <cwctype>

namespace Remote {

namespace RT = Constants::RemoteTcpIp;

// =============================================================================
// THE REACHABLE-COMMAND TABLE
//
// This is the whole remote API surface. A Command that is not listed here cannot
// be driven over the network, whatever a client sends.
//
// Several rows may map to the same Command — that is how the short scripting
// aliases work (`next` and `NextImage` are the same thing). The canonical enum
// name is always accepted; the short alias is listed first because that is what
// people actually type.
//
// DELIBERATELY ABSENT, and why:
//   SaveImage                  raises a Save dialog and writes files
//   CmdArgsExport/Import/      all raise file choosers; a blocked dialog would
//     GenerateShortcut/Test    hold the connection open indefinitely
//   ToggleDedicated            switches the persistence namespace at runtime
//   OpenFile without payload   the payload form is exposed as `open`; the bare
//                              command raises a file chooser
// =============================================================================
namespace {
    constexpr CommandEntry TABLE[] = {
        // --- Navigation ---
        { L"NextImage",             Command::NextImage,                       PayloadRule::None },
        { L"PrevImage",             Command::PrevImage,                       PayloadRule::None },
        { L"GoToFirstImage",        Command::GoToFirstImage,                  PayloadRule::None },
        { L"GoToLastImage",         Command::GoToLastImage,                   PayloadRule::None },
        { L"GoToLastImageInCurrentFolder",
                                    Command::GoToLastImageInCurrentFolder,    PayloadRule::None },
        { L"ToggleFirstLastImageInCurrentFolder",
                                    Command::ToggleFirstLastImageInCurrentFolder, PayloadRule::None },
        { L"ShowInExplorer",        Command::ShowInExplorer,                  PayloadRule::None },
        { L"ToggleLastDir",         Command::ToggleLastDir,                   PayloadRule::None },
        { L"ToggleLastImage",       Command::ToggleLastImage,                 PayloadRule::None },
        { L"PrevHistoryFolder",     Command::PrevHistoryFolder,               PayloadRule::None },
        { L"NextHistoryFolder",     Command::NextHistoryFolder,               PayloadRule::None },
        { L"NextFavoriteFolder",    Command::NextFavoriteFolder,              PayloadRule::None },
        { L"PrevFavoriteFolder",    Command::PrevFavoriteFolder,              PayloadRule::None },
        { L"ReloadCurrentDir",      Command::ReloadCurrentDir,                PayloadRule::None },

        // --- Commands carrying a value ---
        { L"JumpToImage",           Command::JumpToImage,                     PayloadRule::Required,
          L"image NUMBER in the playlist, 1-based" },
        { L"OpenFile",              Command::OpenFile,                        PayloadRule::Required,
          L"full path to a file or a folder" },
        { L"FindImage",             Command::FindImage,                       PayloadRule::Required,
          L"jump to the first file whose name matches this text" },
        { L"ZoomTo",                Command::ZoomTo,                          PayloadRule::Required,
          L"percent, e.g. 150. 0 returns to the view mode's natural fit" },
        { L"SlideshowSetInterval",  Command::SlideshowSetInterval,            PayloadRule::Required,
          L"slide duration in milliseconds" },

        // --- View modes ---
        { L"ViewMode1",             Command::ViewMode1,                       PayloadRule::None },
        { L"ViewMode2",             Command::ViewMode2,                       PayloadRule::None },
        { L"ViewMode3",             Command::ViewMode3,                       PayloadRule::None },
        { L"ViewMode4",             Command::ViewMode4,                       PayloadRule::None },
        { L"ViewMode5",             Command::ViewMode5,                       PayloadRule::None },

        // --- Zoom / viewport ---
        { L"ZoomIn",                Command::ZoomIn,                          PayloadRule::None },
        { L"ZoomOut",               Command::ZoomOut,                         PayloadRule::None },
        { L"ZoomReset",             Command::ZoomReset,                       PayloadRule::None },
        { L"ToggleViewportLock",    Command::ToggleViewportLock,              PayloadRule::None },
        { L"PanLeft",               Command::PanLeft,                         PayloadRule::None },
        { L"PanRight",              Command::PanRight,                        PayloadRule::None },
        { L"PanUp",                 Command::PanUp,                           PayloadRule::None },
        { L"PanDown",               Command::PanDown,                         PayloadRule::None },

        // --- Transform ---
        { L"RotateCW",              Command::RotateCW,                        PayloadRule::None },
        { L"RotateCCW",             Command::RotateCCW,                       PayloadRule::None },
        { L"FlipH",                 Command::FlipH,                           PayloadRule::None },
        { L"FlipV",                 Command::FlipV,                           PayloadRule::None },

        // --- Slideshow ---
        { L"SlideshowToggle",       Command::SlideshowToggle,                 PayloadRule::None },
        { L"SlideshowPauseResume",  Command::SlideshowPauseResume,            PayloadRule::None },
        { L"SlideshowToggleLoop",   Command::SlideshowToggleLoop,             PayloadRule::None },
        { L"SlideshowToggleShuffle",Command::SlideshowToggleShuffle,          PayloadRule::None },
        { L"SlideshowCycleTransition",
                                    Command::SlideshowCycleTransition,        PayloadRule::None },

        // --- Window / chrome ---
        { L"ToggleFullscreen",      Command::ToggleFullscreen,                PayloadRule::None },
        { L"ToggleAlwaysOnTop",     Command::ToggleAlwaysOnTop,               PayloadRule::None },
        { L"AutosizeToWorkArea",    Command::AutosizeToWorkArea,              PayloadRule::None },
        { L"ResizeWindowLarger",    Command::ResizeWindowLarger,              PayloadRule::None },
        { L"ResizeWindowSmaller",   Command::ResizeWindowSmaller,             PayloadRule::None },
        { L"MoveWindowLeft",        Command::MoveWindowLeft,                  PayloadRule::None },
        { L"MoveWindowRight",       Command::MoveWindowRight,                 PayloadRule::None },
        { L"MoveWindowUp",          Command::MoveWindowUp,                    PayloadRule::None },
        { L"MoveWindowDown",        Command::MoveWindowDown,                  PayloadRule::None },
        { L"SnapLeft",              Command::SnapLeft,                        PayloadRule::None },
        { L"SnapRight",             Command::SnapRight,                       PayloadRule::None },
        { L"SnapTop",               Command::SnapTop,                         PayloadRule::None },
        { L"SnapBottom",            Command::SnapBottom,                      PayloadRule::None },
        { L"SnapTopLeft",           Command::SnapTopLeft,                     PayloadRule::None },
        { L"SnapTopRight",          Command::SnapTopRight,                    PayloadRule::None },
        { L"SnapBottomLeft",        Command::SnapBottomLeft,                  PayloadRule::None },
        { L"SnapBottomRight",       Command::SnapBottomRight,                 PayloadRule::None },
        { L"ToggleCornerPreference",Command::ToggleCornerPreference,          PayloadRule::None },
        { L"CycleBackdropType",     Command::CycleBackdropType,               PayloadRule::None },

        // --- Panels / overlays ---
        { L"ToggleHelp",            Command::ToggleHelp,                      PayloadRule::None },
        { L"ToggleCache",           Command::ToggleCache,                     PayloadRule::None },
        { L"ClearCache",            Command::ClearCache,                      PayloadRule::None },
        { L"ToggleDir",             Command::ToggleDir,                       PayloadRule::None },
        { L"ToggleHistory",         Command::ToggleHistory,                   PayloadRule::None },
        { L"ToggleHistoryFull",     Command::ToggleHistoryFull,               PayloadRule::None },
        { L"ToggleOverlay",         Command::ToggleOverlay,                   PayloadRule::None },
        { L"CycleOverlayLayout",    Command::CycleOverlayLayout,              PayloadRule::None },
        { L"ToggleOverlayBackground",
                                    Command::ToggleOverlayBackground,         PayloadRule::None },
        { L"ShowInfo",              Command::ShowInfo,                        PayloadRule::None },
        { L"ToggleStats",           Command::ToggleStats,                     PayloadRule::None },
        { L"CloseAllPanels",        Command::CloseAllPanels,                  PayloadRule::None },
        { L"RestoreAllPanels",      Command::RestoreAllPanels,                PayloadRule::None },
        { L"ToggleAllPanels",       Command::ToggleAllPanels,                 PayloadRule::None },
        { L"ToggleDedicatedPanel",  Command::ToggleDedicatedPanel,            PayloadRule::None },
        { L"ToggleThumbnailWrapAround",
                                    Command::ToggleThumbnailWrapAround,       PayloadRule::None },
        { L"ToggleThumbnailEffects",Command::ToggleThumbnailEffects,          PayloadRule::None },

        // --- Colour effects ---
        { L"ToggleGrayscale",       Command::ToggleGrayscale,                 PayloadRule::None },
        { L"ToggleInvert",          Command::ToggleInvert,                    PayloadRule::None },
        { L"ToggleSepia",           Command::ToggleSepia,                     PayloadRule::None },
        { L"ToggleSolarize",        Command::ToggleSolarize,                  PayloadRule::None },
        { L"ToggleOutline",         Command::ToggleOutline,                   PayloadRule::None },
        { L"ToggleThreshold",       Command::ToggleThreshold,                 PayloadRule::None },
        { L"ToggleEffectPreview",   Command::ToggleEffectPreview,             PayloadRule::None },
        { L"ResetEffects",          Command::ResetEffects,                    PayloadRule::None },
        { L"GammaUp",               Command::GammaUp,                         PayloadRule::None },
        { L"GammaDown",             Command::GammaDown,                       PayloadRule::None },
        { L"BrightnessUp",          Command::BrightnessUp,                    PayloadRule::None },
        { L"BrightnessDown",        Command::BrightnessDown,                  PayloadRule::None },
        { L"ContrastUp",            Command::ContrastUp,                      PayloadRule::None },
        { L"ContrastDown",          Command::ContrastDown,                    PayloadRule::None },
        { L"SaturationUp",          Command::SaturationUp,                    PayloadRule::None },
        { L"SaturationDown",        Command::SaturationDown,                  PayloadRule::None },

        // --- Sort order ---
        { L"SortByName",            Command::SortByName,                      PayloadRule::None },
        { L"SortByDate",            Command::SortByDate,                      PayloadRule::None },
        { L"SortBySize",            Command::SortBySize,                      PayloadRule::None },
        { L"SortByType",            Command::SortByType,                      PayloadRule::None },
        { L"SortByDisk",            Command::SortByDisk,                      PayloadRule::None },

        // --- Wallpaper ---
        { L"SetWallpaperFill",      Command::SetWallpaperFill,                PayloadRule::None },
        { L"SetWallpaperFit",       Command::SetWallpaperFit,                 PayloadRule::None },
        { L"SetWallpaperStretch",   Command::SetWallpaperStretch,             PayloadRule::None },
        { L"SetWallpaperTile",      Command::SetWallpaperTile,                PayloadRule::None },
        { L"SetWallpaperCenter",    Command::SetWallpaperCenter,              PayloadRule::None },
        { L"SetWallpaperSpan",      Command::SetWallpaperSpan,                PayloadRule::None },

        // --- Theme ---
        { L"ThemeFactorUp",         Command::ThemeFactorUp,                   PayloadRule::None },
        { L"ThemeFactorDown",       Command::ThemeFactorDown,                 PayloadRule::None },
        { L"ThemeFactorReset",      Command::ThemeFactorReset,                PayloadRule::None },

        // --- Clipboard ---
        { L"CopyToClipboard",       Command::CopyToClipboard,                 PayloadRule::None },

        // --- Application control ---
        // HideToTray and quit are the two a screen-management script actually
        // needs; both are safe because the tray icon remains the way back.
        { L"HideToTray",            Command::HideToTray,                      PayloadRule::None },
        { L"NewWindow",             Command::NewWindow,                       PayloadRule::None },
        { L"ResetAll",              Command::ResetAll,                        PayloadRule::None },
        { L"ResetWindowLayout",     Command::ResetWindowLayout,               PayloadRule::None },
        { L"HardQuit",              Command::HardQuit,                        PayloadRule::None,
          L"exits that instance immediately, without saving its session" },

        // --- Window opacity ---
        { L"OpacityUp",             Command::OpacityUp,                       PayloadRule::None },
        { L"OpacityDown",           Command::OpacityDown,                     PayloadRule::None },

        // --- History walk, all rows (the horizontal-wheel scope) ---
        { L"PrevHistoryFolderAll",  Command::PrevHistoryFolderAll,            PayloadRule::None },
        { L"NextHistoryFolderAll",  Command::NextHistoryFolderAll,            PayloadRule::None },

        // --- Mirroring / observing ---
        // `observe` is how a CALLER asks to be fed this instance's actions: it
        // adds the calling connection to the observer list, and `observe 0`
        // removes it. There is no way to nominate a THIRD party — an observer can
        // only ever be the connection that asked, which is what stops this from
        // being a way to make one screen shout at another.
        { L"Observe",               Command::Observe,                         PayloadRule::Required,
          L"1 = start reporting my actions to you, 0 = stop" },
        // `sync` pushes the sender's whole view/effect state. Exists because
        // mirroring forwards TOGGLES: a toggle applied to a different starting
        // state diverges, and the effect CHAIN ORDER cannot be repaired by
        // resending toggles at all.
        { L"Sync",                  Command::Sync,                            PayloadRule::Required,
          L"adopt the sender's whole view state (built by the console, not typed)" },

        // --- Diagnostics ---
        // `enablelog 1` / `enablelog 0` — switch the Ctrl+F12 wire log on or off
        // on THIS instance. Payload-required rather than a toggle so a driving
        // instance can put a whole wall of screens into the same state; a toggle
        // sent to ends that already disagree only swaps which one is wrong.
        //
        // Reachable remotely on purpose: the screen whose behaviour you are
        // trying to explain is usually not the one you are sitting at.
        { L"EnableRemoteLog",       Command::EnableRemoteLog,                 PayloadRule::Required,
          L"1 = start recording the wire log, 0 = stop" },

        // `msg <text>` — say something on the RECEIVER's screen. The console's
        // Identify button sends each target its own name, which is how you tell
        // two otherwise identical viewers apart.
        //
        { L"msgRemote",             Command::msgRemote,                       PayloadRule::Required,
          L"show this text centre-screen on the receiver" },
    };

    constexpr size_t TABLE_COUNT = sizeof(TABLE) / sizeof(TABLE[0]);

    // =========================================================================
    // ONE NAME PER COMMAND, and it is the enumerator's own spelling.
    //
    // The table used to carry a short alias beside the long one — `goto` for
    // JumpToImage, `quit` for HardQuit, `msg` for msgRemote. That is gone. A
    // command now has exactly one wire name, identical to its Command.h
    // enumerator, so there is one spelling to learn and no question about which
    // of two names a log line or a script is using.
    //
    // The one command that cannot follow the rule is JumpToImage: `goto` is a
    // C++ keyword, so `Command::goto` could never have existed. The long name is
    // the only name there, which is exactly the rule anyway.
    //
    // The two asserts below enforce it from the table alone:
    constexpr bool TableHasOneRowPerCommand() {
        for (size_t i = 0; i < TABLE_COUNT; ++i)
            for (size_t j = i + 1; j < TABLE_COUNT; ++j)
                if (TABLE[i].cmd == TABLE[j].cmd) return false;
        return true;
    }

    constexpr bool ConstEqualI(const wchar_t *a, const wchar_t *b) {
        for (;; ++a, ++b) {
            const wchar_t ca = (*a >= L'A' && *a <= L'Z') ? *a + 32 : *a;
            const wchar_t cb = (*b >= L'A' && *b <= L'Z') ? *b + 32 : *b;
            if (ca != cb) return false;
            if (ca == 0) return true;
        }
    }

    constexpr bool TableNamesAreUnique() {
        for (size_t i = 0; i < TABLE_COUNT; ++i)
            for (size_t j = i + 1; j < TABLE_COUNT; ++j)
                // Case-insensitive, because LookupCommand is: two rows differing
                // only in case would be one name with two meanings, and which
                // one won would depend on table order.
                if (ConstEqualI(TABLE[i].name, TABLE[j].name)) return false;
        return true;
    }

    static_assert(TableHasOneRowPerCommand(),
                  "two rows map to the same Command — aliases were removed on purpose, "
                  "give the command one name and make it the enumerator's spelling");
    static_assert(TableNamesAreUnique(),
                  "two rows share a wire name (lookup is case-insensitive), so which one "
                  "answers would depend on table order");

    // NOT checked, and it cannot be from here: that every remotely-reachable
    // Command HAS a row. Proving that needs a second list of what ought to be
    // reachable, which is the thing this table already is. A missing row means a
    // command is silently unreachable — an inconvenience. An extra row could
    // mean deleted files, and THAT is what the NEVER_REMOTE assert below covers.

    // =========================================================================
    // NEVER_REMOTE — commands that alter files.
    //
    // Not "not currently exposed". Not "we remembered to leave them out". These
    // must be impossible to drive over a socket, and the static_assert below
    // turns that from a convention into a build error: give any of them a row in
    // TABLE and the project stops compiling.
    //
    // Why the strength: authentication happens once, when the connection opens
    // (RemoteServer::Authenticate). Every line after that is trusted with no
    // per-message signature, so on a routable network an attacker who can inject
    // into an established session issues commands as the authenticated caller.
    // Better authentication would narrow that; keeping `delete` off the menu
    // entirely removes it.
    // =========================================================================
    constexpr Command NEVER_REMOTE[] = {
        Command::FileDeleteSelection,
        Command::FileMoveSelection,
        Command::FilePasteIntoFolder,
        Command::FileCopySelection,
        Command::SaveImage,
    };

    consteval bool NeverRemoteHasNoTableRow() {
        for (const Command c : NEVER_REMOTE)
            for (const CommandEntry &e : TABLE)
                if (e.cmd == c) return false;
        return true;
    }
    static_assert(NeverRemoteHasNoTableRow(),
                  "A file-altering command was given a row in the remote command "
                  "table. Remove it: these must never be reachable over a socket, "
                  "whatever the authentication state.");

    // =========================================================================
    // SESSION_BLOCKED — fine alone, unsafe while connected.
    //
    // Two failure modes, both silent without this: a command that changes the
    // FILE SET (every index after the change then points at a different
    // picture), and a command that means something different on each end.
    //
    // The reason travels with the entry because a keypress that quietly does
    // nothing is indistinguishable from a bug.
    // =========================================================================
    struct BlockedEntry {
        Command        cmd;
        const wchar_t *reason;
    };

    constexpr BlockedEntry SESSION_BLOCKED[] = {
        // Physical disk order is a property of the drive, so two instances both
        // "sorting by disk" genuinely produce different orders — and every index
        // exchanged after that lands on the wrong file.
        { Command::SortByDisk,
          L"physical disk order differs per drive" },

        // Searches this playlist only, so it lands somewhere the other end is
        // not. Navigation covers the same ground while connected.
        { Command::FindImage,
          L"searches this playlist only" },

        // The four that change what is in the folder. A file added or removed
        // shifts every index after it, which is exactly what the two ends are
        // relying on to stay aligned.
        { Command::FileDeleteSelection,
          L"changing the folder shifts every index after it" },
        { Command::FileMoveSelection,
          L"changing the folder shifts every index after it" },
        { Command::FilePasteIntoFolder,
          L"changing the folder shifts every index after it" },
        { Command::SaveImage,
          L"writing a file shifts every index after it" },

        // NOTE: FileCopySelection is deliberately absent. Copying to the
        // clipboard reads the file and writes nothing, so it changes no index
        // and there is no reason to take it away.
    };

    std::wstring TrimWs(const std::wstring &s) {
        size_t b = 0, e = s.size();
        while (b < e && ::iswspace(s[b])) ++b;
        while (e > b && ::iswspace(s[e - 1])) --e;
        return s.substr(b, e - b);
    }
}

const CommandEntry *CommandTable(size_t &countOut) {
    countOut = TABLE_COUNT;
    return TABLE;
}

bool LookupCommand(const std::wstring &name, const CommandEntry *&entryOut) {
    for (size_t i = 0; i < TABLE_COUNT; ++i) {
        if (_wcsicmp(TABLE[i].name, name.c_str()) == 0) {
            entryOut = &TABLE[i];
            return true;
        }
    }
    return false;
}

bool NameForCommand(Command cmd, std::wstring &nameOut) {
    // First row wins. The table lists the short alias before the canonical enum
    // name for exactly this reason — a mirrored session that a human may be
    // watching with netcat should read "next", not "NextImage".
    for (size_t i = 0; i < TABLE_COUNT; ++i) {
        if (TABLE[i].cmd == cmd) {
            nameOut = TABLE[i].name;
            return true;
        }
    }
    return false;
}

bool IsMirrorable(Command cmd) {
    switch (cmd) {
        // ── Never fanned out ────────────────────────────────────────────────
        // Each of these is reachable on purpose as a single deliberate act (the
        // F9 Send box, a script) and wrong as a broadcast.

        // Would end or hide every connected screen at once.
        case Command::HardQuit:
        case Command::HideToTray:
        case Command::NewWindow:
            return false;

        // Opens Explorer on a machine nobody is sitting at.
        case Command::ShowInExplorer:
            return false;

        // Writes files / raises a save dialog.
        case Command::SaveImage:
            return false;

        // Clipboard is per-machine; copying on a wall screen achieves nothing.
        case Command::CopyToClipboard:
            return false;

        // Panels raise a window on the far screen that then has to be closed
        // from here. The driving instance wants its OWN panels, not theirs.
        case Command::ToggleHelp:
        case Command::ToggleHistory:
        case Command::ToggleHistoryFull:
        case Command::ToggleCache:
        case Command::ToggleDir:
        case Command::ShowInfo:
        case Command::ToggleStats:
        case Command::FindImage:
        case Command::JumpToImage:
        case Command::ZoomTo:
        case Command::CloseAllPanels:
        case Command::RestoreAllPanels:
        case Command::ToggleAllPanels:
        case Command::ToggleDedicatedPanel:
        case Command::ToggleRemotePanel:
        case Command::ToggleRemotesConsole:
            return false;

        // Geometry. Slaves are placed on fixed monitors, usually fullscreen;
        // forwarding a snap or a move knocks one off the screen it was put on.
        case Command::SnapLeft:      case Command::SnapRight:
        case Command::SnapTop:       case Command::SnapBottom:
        case Command::SnapTopLeft:   case Command::SnapTopRight:
        case Command::SnapBottomLeft:case Command::SnapBottomRight:
        case Command::MoveWindowLeft:case Command::MoveWindowRight:
        case Command::MoveWindowUp:  case Command::MoveWindowDown:
        case Command::ResizeWindowLarger:
        case Command::ResizeWindowSmaller:
        case Command::AutosizeToWorkArea:
        case Command::ResetWindowLayout:
        case Command::ResetAll:
            return false;

        // Configuration and identity — a dedicated instance's whole point is
        // that these are ITS OWN and nobody else writes them.
        case Command::ToggleDedicated:
        case Command::CmdArgsExport:
        case Command::CmdArgsImport:
        case Command::CmdArgsGenerateShortcut:
        case Command::CmdArgsTest:
            return false;

        // The mirroring controls themselves. Forwarding MirrorToggle would have
        // every slave start mirroring to ITS targets — and where two of them
        // point at each other, that is an infinite exchange. Observe/Sync are
        // remote-only verbs and are never something a local keypress fans out.
        case Command::MirrorToggle:
        case Command::MirrorPick:
        case Command::MirrorLocalToggle:
        case Command::Observe:
        case Command::Sync:
            return false;

        // Fanned out by BroadcastEnableLog, which sends the STATE (`enablelog 1`)
        // — not by the generic mirror path, which would send the bare verb and
        // have every target reject it for a missing payload. Excluded here so
        // the two routes cannot both fire and send it twice.
        case Command::EnableRemoteLog:
            return false;

        // Aimed, never fanned out. `msg` is sent to ONE target on purpose — the
        // Identify button gives each screen a DIFFERENT text, its own name, so a
        // broadcast would be meaningless. And a slave that re-forwarded it to
        // its own targets would put one instance's name on somebody else's
        // screen.
        case Command::msgRemote:
            return false;

        default:
            break;
    }

    // Everything else mirrors — but only if it is on the wire at all. A command
    // with no table row has no name to send, so reachability is still the outer
    // bound: this function narrows that set, it never widens it.
    std::wstring unused;
    return NameForCommand(cmd, unused);
}

bool IsMirrorableRemote(Command cmd) {
    if (!IsMirrorable(cmd)) return false;

    switch (cmd) {
        // Disk order is a property of the drive. Both ends "sort by disk" and
        // arrive at different orders, which then quietly poisons every index.
        // Also refused locally while connected — this is the belt to that brace.
        case Command::SortByDisk:
            return false;

        // Nothing else needs excluding HERE, because the two things that do not
        // survive the trip are not commands: the `goto <n>` that LoadImageIndex
        // emits, and the `folder=` field inside a `sync`. Both are filtered
        // where they are produced, since a bare Command carries neither.
        default:
            return true;
    }
}

bool IsNeverRemote(Command cmd) {
    for (const Command c : NEVER_REMOTE)
        if (c == cmd) return true;
    return false;
}

bool BlockedNow(Command cmd, const wchar_t *&reasonOut) {
    if (!Mirror::SessionActive()) return false;
    return IsBlockedInSession(cmd, reasonOut);
}

bool IsBlockedInSession(Command cmd, const wchar_t *&reasonOut) {
    // Linear scan, deliberately. The table is a handful of entries that fit in
    // a cache line; hashing one Command costs more than comparing all of them,
    // and this runs once per keypress.
    for (const BlockedEntry &e : SESSION_BLOCKED) {
        if (e.cmd == cmd) {
            reasonOut = e.reason;
            return true;
        }
    }
    return false;
}

RemoteRequest ParseLine(const std::wstring &line) {
    RemoteRequest req;

    // Bound first, before any other work. An unbounded line is the one input a
    // socket fully controls, so it is rejected on length alone.
    if (line.size() > RT::MAX_LINE_LEN) {
        req.status = ParseStatus::LineTooLong;
        return req;
    }

    // Tolerate CRLF and stray indentation — telnet and a hand-typed session both
    // arrive slightly untidy, and rejecting that would be pointless pedantry.
    const std::wstring trimmed = TrimWs(line);
    if (trimmed.empty()) {
        req.status = ParseStatus::EmptyLine;
        return req;
    }

    // Split on the FIRST run of whitespace only: everything after it is payload,
    // verbatim. A path with spaces must survive intact and must not be quoted,
    // because quoting would then have to be unquoted and paths legitimately
    // contain quote characters on Windows.
    const size_t sp = trimmed.find_first_of(L" \t");
    std::wstring name, payload;
    if (sp == std::wstring::npos) {
        name = trimmed;
    } else {
        name    = trimmed.substr(0, sp);
        payload = TrimWs(trimmed.substr(sp + 1));
    }

    req.rawName = name;

    // Protocol verbs are checked before the command table. They answer from the
    // socket thread without ever reaching the viewer, which is what makes `ping`
    // a true liveness check: a reply proves the listener is up, and says nothing
    // about whether the UI thread happens to be busy.
    if (_wcsicmp(name.c_str(), L"help") == 0 ||
        _wcsicmp(name.c_str(), L"?")    == 0) {
        req.verb   = Verb::Help;
        req.status = ParseStatus::Verb;
        return req;
    }
    if (_wcsicmp(name.c_str(), L"ping") == 0) {
        req.verb   = Verb::Ping;
        req.status = ParseStatus::Verb;
        return req;
    }
    if (_wcsicmp(name.c_str(), L"version") == 0) {
        req.verb   = Verb::Version;
        req.status = ParseStatus::Verb;
        return req;
    }

    const CommandEntry *entry = nullptr;
    if (!LookupCommand(name, entry)) {
        req.status = ParseStatus::UnknownCommand;
        return req;
    }

    if (entry->payload == PayloadRule::Required && payload.empty()) {
        req.status = ParseStatus::PayloadRequired;
        return req;
    }
    // A payload on a command that takes none is an error rather than something
    // to discard: silently ignoring it would hide a typo that the sender
    // believes is doing something.
    if (entry->payload == PayloadRule::None && !payload.empty()) {
        req.status = ParseStatus::PayloadRejected;
        return req;
    }

    req.cmd     = entry->cmd;
    req.payload = payload;
    req.status  = ParseStatus::Ok;
    return req;
}

// --- Responses -------------------------------------------------------------

std::wstring MakeOk() { return RT::RESP_OK; }

std::wstring MakeOk(const std::wstring &text) {
    if (text.empty()) return MakeOk();
    return std::wstring(RT::RESP_OK) + L" " + text;
}

std::wstring MakeErr(int code, const std::wstring &reason) {
    return std::wstring(RT::RESP_ERR) + L" " + std::to_wstring(code) + L" " + reason;
}

std::wstring MakeErrFor(const RemoteRequest &req) {
    switch (req.status) {
        case ParseStatus::UnknownCommand:
            return MakeErr(RT::ERR_UNKNOWN_COMMAND,
                           L"unknown command '" + req.rawName + L"' - send 'help' for the list");
        case ParseStatus::PayloadRequired:
            return MakeErr(RT::ERR_PAYLOAD_REQUIRED,
                           L"'" + req.rawName + L"' requires a value");
        case ParseStatus::PayloadRejected:
            return MakeErr(RT::ERR_PAYLOAD_REJECTED,
                           L"'" + req.rawName + L"' takes no value");
        case ParseStatus::LineTooLong:
            return MakeErr(RT::ERR_LINE_TOO_LONG, L"line too long");
        default:
            return MakeErr(RT::ERR_INTERNAL, L"unhandled parse state");
    }
}

std::wstring BuildHelpText() {
    // Built from the same table the parser uses, so the listing can never drift
    // from what is actually accepted.
    std::wstring out = L"qIV remote protocol v" +
                       std::to_wstring(RT::PROTOCOL_VERSION) + L"\r\n";
    out += L"  help | ?                  this listing\r\n";
    out += L"  ping                      liveness check\r\n";
    out += L"  version                   app and protocol version\r\n";
    for (size_t i = 0; i < TABLE_COUNT; ++i) {
        out += L"  ";
        out += TABLE[i].name;
        if (TABLE[i].payload == PayloadRule::Required) out += L" <value>";
        out += L"\r\n";
    }
    return out;
}

// --- Encoding ---------------------------------------------------------------

std::string ToUtf8(const std::wstring &s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                                      nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring FromUtf8(const char *data, size_t len) {
    if (!data || len == 0) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, data, static_cast<int>(len), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, data, static_cast<int>(len), out.data(), n);
    return out;
}

} // namespace Remote
