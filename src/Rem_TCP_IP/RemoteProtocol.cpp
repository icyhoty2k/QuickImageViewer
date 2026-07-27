#include "RemoteProtocol.h"
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
        { L"next",                  Command::NextImage,                       PayloadRule::None },
        { L"NextImage",             Command::NextImage,                       PayloadRule::None },
        { L"prev",                  Command::PrevImage,                       PayloadRule::None },
        { L"PrevImage",             Command::PrevImage,                       PayloadRule::None },
        { L"first",                 Command::GoToFirstImage,                  PayloadRule::None },
        { L"GoToFirstImage",        Command::GoToFirstImage,                  PayloadRule::None },
        { L"last",                  Command::GoToLastImage,                   PayloadRule::None },
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
        { L"reload",                Command::ReloadCurrentDir,                PayloadRule::None },
        { L"ReloadCurrentDir",      Command::ReloadCurrentDir,                PayloadRule::None },

        // --- Commands carrying a value ---
        { L"goto",                  Command::JumpToImage,                     PayloadRule::Required },
        { L"JumpToImage",           Command::JumpToImage,                     PayloadRule::Required },
        { L"open",                  Command::OpenFile,                        PayloadRule::Required },
        { L"OpenFile",              Command::OpenFile,                        PayloadRule::Required },
        { L"find",                  Command::FindImage,                       PayloadRule::Required },
        { L"FindImage",             Command::FindImage,                       PayloadRule::Required },
        { L"zoom",                  Command::ZoomTo,                          PayloadRule::Required },
        { L"ZoomTo",                Command::ZoomTo,                          PayloadRule::Required },
        { L"interval",              Command::SlideshowSetInterval,            PayloadRule::Required },
        { L"SlideshowSetInterval",  Command::SlideshowSetInterval,            PayloadRule::Required },

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
        { L"slideshow",             Command::SlideshowToggle,                 PayloadRule::None },
        { L"SlideshowToggle",       Command::SlideshowToggle,                 PayloadRule::None },
        { L"pause",                 Command::SlideshowPauseResume,            PayloadRule::None },
        { L"SlideshowPauseResume",  Command::SlideshowPauseResume,            PayloadRule::None },
        { L"SlideshowToggleLoop",   Command::SlideshowToggleLoop,             PayloadRule::None },
        { L"SlideshowToggleShuffle",Command::SlideshowToggleShuffle,          PayloadRule::None },
        { L"SlideshowCycleTransition",
                                    Command::SlideshowCycleTransition,        PayloadRule::None },

        // --- Window / chrome ---
        { L"fullscreen",            Command::ToggleFullscreen,                PayloadRule::None },
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
        { L"quit",                  Command::HardQuit,                        PayloadRule::None },
        { L"HardQuit",              Command::HardQuit,                        PayloadRule::None },
    };

    constexpr size_t TABLE_COUNT = sizeof(TABLE) / sizeof(TABLE[0]);

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
