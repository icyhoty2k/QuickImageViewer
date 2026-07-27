// CMDArgs.h — Command-line argument parsing for QIV.
// All parsing and application logic lives here + CMDArgs.cpp.
// Call ParseCmdArgs() early in wWinMain, then ApplyCmdArgs() after the window is ready.
#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include "SlideshowTransitions.h"

struct CmdArgs {
    // --- Display ---
    bool         background    = false; // -background      : start hidden in tray (service mode)
    bool         fullscreen    = false; // -fullscreen       : start in fullscreen
    bool         windowedView  = false; // -windowedView     : explicit windowed (default)
    bool         alwaysOnTop   = false; // -awaysOnTop       : keep window above all others
    int          monitorNum    = -1;    // -monitorNum#N     : 1-based monitor index (-1 = current)

    // --- Content ---
    std::wstring startFolder;           // -startFolder <path> : open this folder at startup
    std::wstring imageFile;             // positional arg      : open this specific file

    // --- Slideshow ---
    bool         slideshow            = false; // -slideshow                    : auto-start slideshow
    bool         repeat               = false; // -repeat                       : loop slideshow
    bool         shuffle              = false; // -shuffle                      : random order
    int          slideshowIntervalMs  = -1;    // -slideshowInterval N          : N seconds between slides
    // Transition control — mirrors Slideshow › Transition in the menus.
    //   -slideshowTransition=<name>              pick one (implies source none)
    //   -slideshowTransitions=<a,b,c|1,5,9>      custom list (implies source list);
    //                                            accepts names OR the menu's numbers
    //   -slideshowTransitionSource=none|all|list which transitions are in play
    //   -slideshowTransitionOrder=sequential|random  how the next is drawn
    //   -slideshowTransitionShuffle              legacy: source all + order random
    // The *Specified/*Given flags exist so an absent switch leaves the SAVED
    // setting alone instead of silently resetting it.
    TransitionType slideshowTransition  = TransitionType::Cut; // -slideshowTransition=<type>
    bool           transitionSpecified  = false;
    uint32_t       transitionList       = 0;     // bitmask of TransitionType values
    bool           transitionListGiven  = false;
    int            transitionSource     = -1;    // -1 = not supplied
    int            transitionOrder      = -1;    // -1 = not supplied
    bool           transitionShuffle    = false; // -slideshowTransitionShuffle

    // --- Dedicated instance (src/Dedicated) ---
    // A NAME is what makes several dedicated copies able to coexist: mutex,
    // window class, registry namespace and history file all derive from it.
    // Empty name = the main instance, whose identifiers are unchanged.
    std::wstring instanceName;   // -instance=<name>
    std::wstring instanceDesc;   // -instanceDesc=<text>
    // -config <path> : use THIS .ini instead of the one derived from the exe
    // name. Generated startup shortcuts pass it, so one exe can serve several
    // configured screens and the config can live anywhere.
    std::wstring configPath;

    // Promotions — the second playlist. -1 means "switch absent, leave default".
    std::wstring promoFolder;    // -promoFolder <path>
    int promoOrder      = -1;    // -promoOrder=sequential|weighted
    int promoImagesFrom = -1;    // -promoEveryImages=<from>-<to>   (0 = off)
    int promoImagesTo   = -1;
    int promoTimeFrom   = -1;    // -promoEverySeconds=<from>-<to>  (0 = off)
    int promoTimeTo     = -1;

    // --- Remote control over TCP/IP (src/Rem_TCP_IP) ---
    // One of the only two ways the listener can be switched on; the other is a
    // [REMOTE_TCP_IP] section in the .ini. Absent switches leave the .ini's
    // values alone, so a configured screen can be launched without repeating
    // its whole configuration on the command line.
    //   -remote                       enable the listener
    //   -remoteName=<name>            how this instance identifies itself
    //   -remoteIp=<addr>              BIND address (0.0.0.0 = every interface)
    //   -remotePort=<n>               listen port
    //   -remoteAllow=<a,b,c>          IPs permitted to connect (empty = deny all)
    //   -remoteBlock=<a,b,c>          IPs always refused; beats the allow list
    //   -remotePassword=<text>        hashed before storage, never kept plaintext
    //   -remoteMaxConn=<n>            simultaneous clients
    bool         remoteEnable   = false;
    std::wstring remoteName;
    std::wstring remoteIp;
    std::wstring remoteAllow;
    std::wstring remoteBlock;
    std::wstring remotePassword;
    int          remotePort     = -1;   // -1 = switch absent, keep the stored value
    int          remoteMaxConn  = -1;   // -1 = switch absent, keep the stored value

    // --- Behavior ---
    bool         hideMouse     = false; // -hideMouse     : hide cursor at startup
    bool         lock          = false; // -lock          : KIOSK — no keyboard or mouse input accepted
    bool         keepAwake     = false; // -keepDisplayAwake : block screensaver / display sleep
    bool         dedicated       = false; // -dedicated       : separate registry/history/favorites, unique mutex
    bool         runOnStartup    = false; // -runOnStartup    : write/refresh the startup registry entry (handles exe relocation)
    bool         restoreDefaults = false; // -RestoreDefaults : delete all registry settings, show confirmation, and exit
};

// Parse argc/argv into a CmdArgs struct.  Call immediately after CommandLineToArgvW.
CmdArgs ParseCmdArgs(int argc, LPWSTR* argv);

// Apply parsed args to the already-created window.
// Replaces the old inline arg-handling block in wWinMain.
void ApplyCmdArgs(HWND hWnd, const CmdArgs& args, int nCmdShow);

// --- Switch-value parsers, shared with the cmdArgs file support below ---
// Return Constants::Slideshow::TransitionSource / TransitionOrder, or -1 when
// the text is not recognised.
int      ParseTransitionSource(const std::wstring& s);
int      ParseTransitionOrder(const std::wstring& s);
// Comma/semicolon list of transition names or menu numbers → membership bitmask.
uint32_t ParseTransitionList(const std::wstring& spec);

// =============================================================================
// cmdArgs file / shortcut support  (context menu › CmdArgs)
//
// Distinct from Settings Export/Import, which round-trips the REGISTRY. These
// emit a command line: the switches that reproduce the current session's
// toggles when passed to the exe. Settings = how the app persists itself,
// cmdArgs = how you launch it a particular way.
// =============================================================================

// Current app state → a command-line string ("-dedicated -repeat …").
std::wstring BuildCmdArgsFromState();

// Write that string to a user-chosen .txt (defaults to Constants::CmdArgsFile).
void ExportCmdArgsFile(HWND hWnd);

// Read a cmdArgs file, apply it to the running app.
void ImportCmdArgsFile(HWND hWnd);

// Create a desktop shortcut to this exe carrying the generated switches.
void CreateCmdArgsShortcut(HWND hWnd);

// Validate a chosen file's switches; report the result in a dialog.
void TestCmdArgsFile(HWND hWnd);
