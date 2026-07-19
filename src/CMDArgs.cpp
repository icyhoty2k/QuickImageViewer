// CMDArgs.cpp — Command-line argument parsing and application for QIV.
#include "CMDArgs.h"
#include "AppState.h"
#include "SlideshowTransitions.h"
#include "Input/AppCommands.h"
#include "Platform/Constants.h"
#include "Platform/FileHandler.h"
#include <numeric>
#include <random>
#include <algorithm>

extern AppState app;

// =============================================================================
// ParseCmdArgs
// =============================================================================
// Argument reference:
//   -background              Start hidden in system tray (service mode)
//   -fullscreen              Start in fullscreen
//   -windowedView            Start in windowed mode (default; useful as explicit override)
//   -awaysOnTop              Window always on top
//   -runOnStartup            Write/refresh the startup registry entry (mode-aware: dedicated writes its own Run key entry)
//   -monitorNum#N            Open on monitor N (1-based)
//   -startFolder <path>      Open folder at startup (slideshow or normal browse)
//   -slideshow               Auto-start slideshow after loading content
//   -repeat                  Enable slideshow loop
//   -shuffle                 Enable slideshow shuffle
//   -slideshowInterval N     Seconds between slides (integer)
//   -hideMouse               Hide mouse cursor at startup
//   -lock                    KIOSK mode: no keyboard or mouse input
//   -RestoreDefaults         Wipe all registry settings, confirm, and exit (recovery fallback)
//   <path>                   Positional: open this image file
// =============================================================================
CmdArgs ParseCmdArgs(int argc, LPWSTR *argv) {
    CmdArgs args;

    for (int i = 1; i < argc; ++i) {
        const std::wstring arg(argv[i]);

        // --- Display ---
        if (arg == L"-background") args.background = true;
        else if (arg == L"-fullscreen") args.fullscreen = true;
        else if (arg == L"-windowedView") args.windowedView = true;
        else if (arg == L"-awaysOnTop" ||
                 arg == L"-alwaysOnTop")
            args.alwaysOnTop = true;

            // -monitorNum#N  (e.g. -monitorNum#2)
        else if (arg.size() > 11 &&
                 arg.substr(0, 11) == L"-monitorNum") {
            size_t hash = arg.find(L'#');
            if (hash != std::wstring::npos && hash + 1 < arg.size()) {
                try {
                    args.monitorNum = std::stoi(arg.substr(hash + 1));
                } catch (...) {}
            }
        }

        // -startFolder <path>
        else if (arg == L"-startFolder" && i + 1 < argc)
            args.startFolder = argv[++i];

            // --- Slideshow ---
        else if (arg == L"-slideshow") args.slideshow = true;
        else if (arg == L"-repeat") args.repeat = true;
        else if (arg == L"-shuffle" || arg == L"-Shuffle") args.shuffle = true;

            // -slideshowInterval N  (seconds)
        else if (arg == L"-slideshowInterval" && i + 1 < argc) {
            try {
                int secs = std::stoi(std::wstring(argv[++i]));
                if (secs > 0) args.slideshowIntervalMs = secs * 1000;
            } catch (...) {}
        }

        // --- Behavior ---
        else if (arg == L"-hideMouse") args.hideMouse = true;
        else if (arg == L"-lock") args.lock = true;

            // -dedicated (separate registry/history/favorites namespace, unique mutex)
        else if (arg == L"-dedicated") args.dedicated = true;
        else if (arg == L"-runOnStartup") args.runOnStartup = true;

            // -slideshowTransition=<type>  (Cut/Fade/Dissolve/Ripple/Push/Zoom)
        else if (arg.size() > 20 &&
                 _wcsnicmp(arg.c_str(), L"-slideshowTransition=", 21) == 0)
            args.slideshowTransition = ParseTransitionType(arg.substr(21));

            // -slideshowTransitionShuffle
        else if (_wcsicmp(arg.c_str(), L"-slideshowTransitionShuffle") == 0)
            args.transitionShuffle = true;

            // Positional: first non-flag token is the image file
        else if (_wcsicmp(arg.c_str(), L"-RestoreDefaults") == 0)
            args.restoreDefaults = true;

            // Positional: first non-flag token is the image file
        else if (!arg.empty() && arg[0] != L'-' && args.imageFile.empty())
            args.imageFile = arg;
    }

    return args;
}

// =============================================================================
// ApplyCmdArgs
// =============================================================================
void ApplyCmdArgs(HWND hWnd, const CmdArgs &args, int nCmdShow) {
    // 1. Slideshow parameters (set before starting so toggleSlideshow picks them up)
    if (args.repeat) app.slideshow.loop = true;
    if (args.shuffle) app.slideshow.shuffle = true;
    if (args.slideshowIntervalMs > 0) app.slideshow.intervalMs = args.slideshowIntervalMs;
    app.slideshow.transition.type = args.slideshowTransition;
    app.slideshow.transition.shuffle = args.transitionShuffle;

    // 1a. Dedicated mode (affects history file and registry — must be set before any of that)
    if (args.dedicated) app.isDedicated = true;

    // 2. KIOSK lock
    if (args.lock) app.isLocked = true;

    // 3. Position on a specific monitor before the window is shown
    if (args.monitorNum >= 1) {
        struct MonitorInfo {
            int target, current;
            RECT rc;
            bool found;
        };
        MonitorInfo mi{args.monitorNum, 0, {}, false};
        EnumDisplayMonitors(nullptr, nullptr,
                            [](HMONITOR, HDC, RECT *pRc, LPARAM lp) -> BOOL {
                                auto *m = reinterpret_cast<MonitorInfo *>(lp);
                                if (++m->current == m->target) {
                                    m->rc = *pRc;
                                    m->found = true;
                                    return FALSE;
                                }
                                return TRUE;
                            },
                            reinterpret_cast<LPARAM>(&mi));

        if (mi.found) {
            int w = static_cast<int>(app.baseWidth  * app.dpiScale);
            int h = static_cast<int>(app.baseHeight * app.dpiScale);
            int x = mi.rc.left + (mi.rc.right - mi.rc.left - w) / 2;
            int y = mi.rc.top + (mi.rc.bottom - mi.rc.top - h) / 2;
            SetWindowPos(hWnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }

    // 4. Always on top (set before show so it takes effect immediately)
    if (args.alwaysOnTop)
        SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);

    // 5. Background / service mode — hide window, add tray icon, done
    if (args.background) {
        ShowWindow(hWnd, SW_HIDE);
        AppCommands::AddTrayIcon(hWnd);
        return;
    }

    // 6. Show window normally
    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    // Tray icon is always present — dedicated mode requires it for kiosk control,
    // and normal mode uses it as the "hide to tray on close" target.
    AppCommands::AddTrayIcon(hWnd);

    // 7. Load content
    if (!args.startFolder.empty())
        OpenDirectory(hWnd, args.startFolder);
    else if (!args.imageFile.empty())
        OpenSpecificImage(hWnd, args.imageFile);
    else
        OpenInitialImage(hWnd);

    // 8. Fullscreen (after show + content so renderer has correct dimensions)
    if (args.fullscreen)
        AppCommands::ToggleFullscreen(hWnd);

    // 9. Auto-start slideshow (after content is loaded so playlist exists)
    if (args.slideshow)
        AppCommands::toggleSlideshow(hWnd);

    // 10. Hide mouse cursor
    if (args.hideMouse)
        ShowCursor(FALSE);
}
