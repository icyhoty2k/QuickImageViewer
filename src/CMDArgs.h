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

    // --- Behavior ---
    bool         hideMouse     = false; // -hideMouse     : hide cursor at startup
    bool         lock          = false; // -lock          : KIOSK — no keyboard or mouse input accepted
    bool         dedicated       = false; // -dedicated       : separate registry/history/favorites, unique mutex
    bool         runOnStartup    = false; // -runOnStartup    : write/refresh the startup registry entry (handles exe relocation)
    bool         restoreDefaults = false; // -RestoreDefaults : delete all registry settings, show confirmation, and exit
};

// Parse argc/argv into a CmdArgs struct.  Call immediately after CommandLineToArgvW.
CmdArgs ParseCmdArgs(int argc, LPWSTR* argv);

// Apply parsed args to the already-created window.
// Replaces the old inline arg-handling block in wWinMain.
void ApplyCmdArgs(HWND hWnd, const CmdArgs& args, int nCmdShow);
