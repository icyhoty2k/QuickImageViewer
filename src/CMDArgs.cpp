// CMDArgs.cpp — Command-line argument parsing and application for QIV.
#include "CMDArgs.h"
#include "AppState.h"
#include "SlideshowTransitions.h"
#include "Input/AppCommands.h"
#include "Dedicated/DedicatedSettings.h" // IsDedicatedFlag — never prompt on a screen
#include "Platform/Constants.h"
#include "Platform/FileHandler.h"
#include <numeric>
#include <random>
#include <algorithm>

extern AppState app;

// =============================================================================
// Transition switch helpers
// =============================================================================

// "none" | "all" | "list"  →  Constants::Slideshow::TransitionSource, or -1.
int ParseTransitionSource(const std::wstring &s) {
    namespace TS = Constants::Slideshow::TransitionSource;
    if (_wcsicmp(s.c_str(), L"none") == 0) return TS::NONE;
    if (_wcsicmp(s.c_str(), L"all")  == 0) return TS::ALL;
    if (_wcsicmp(s.c_str(), L"list") == 0) return TS::LIST;
    return -1; // unrecognised → leave the saved setting alone
}

// "sequential"|"seq" | "random"|"rand"  →  TransitionOrder, or -1.
int ParseTransitionOrder(const std::wstring &s) {
    namespace TO = Constants::Slideshow::TransitionOrder;
    if (_wcsicmp(s.c_str(), L"sequential") == 0 || _wcsicmp(s.c_str(), L"seq")  == 0)
        return TO::SEQUENTIAL;
    if (_wcsicmp(s.c_str(), L"random")     == 0 || _wcsicmp(s.c_str(), L"rand") == 0)
        return TO::RANDOM;
    return -1;
}

// Comma/semicolon-separated transition list → membership bitmask.
// Each token is either a NAME ("Fade", "Slide Left", "SlideLeft") or the NUMBER
// shown beside it in the menu (1-based, alphabetical) so the switch can mirror
// exactly what the menu displays. Unrecognised tokens are skipped rather than
// silently collapsing to Cut, which is what ParseTransitionType alone would do.
uint32_t ParseTransitionList(const std::wstring &spec) {
    uint32_t mask = 0;
    const int *order = TransitionDisplayOrder();
    size_t pos = 0;

    while (pos <= spec.size()) {
        size_t end = spec.find_first_of(L",;", pos);
        if (end == std::wstring::npos) end = spec.size();

        std::wstring tok = spec.substr(pos, end - pos);
        pos = end + 1;

        // Trim surrounding blanks so "a, b" works as well as "a,b".
        const size_t b = tok.find_first_not_of(L" \t");
        const size_t e = tok.find_last_not_of(L" \t");
        if (b == std::wstring::npos) continue;
        tok = tok.substr(b, e - b + 1);

        const bool numeric = std::all_of(tok.begin(), tok.end(),
                                         [](wchar_t c) { return c >= L'0' && c <= L'9'; });
        if (numeric) {
            const int n = _wtoi(tok.c_str()); // 1-based menu position
            if (n >= 1 && n <= Constants::Slideshow::TRANSITION_COUNT)
                mask |= (1u << order[n - 1]);
            continue;
        }

        // Names: only accept a Cut match when the token really says "Cut",
        // otherwise a typo would quietly enable Cut.
        const TransitionType t = ParseTransitionType(tok);
        if (t != TransitionType::Cut || _wcsicmp(tok.c_str(), L"Cut") == 0)
            mask |= (1u << static_cast<int>(t));
    }
    return mask;
}

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
//   -slideshowTransition=<name>          Use exactly this transition
//   -slideshowTransitions=<a,b,c>        Custom list; names or the menu's numbers
//                                        (e.g. "Fade,Iris,Spin" or "6,8,17")
//   -slideshowTransitionSource=none|all|list
//   -slideshowTransitionOrder=sequential|random
//   -slideshowTransitionShuffle          Legacy: same as source=all order=random
//   -hideMouse               Hide mouse cursor at startup
//   -lock                    KIOSK mode: no keyboard or mouse input
//   -keepDisplayAwake        Block the screensaver and display sleep
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
        else if (arg == L"-keepDisplayAwake") args.keepAwake = true;

            // -dedicated (separate registry/history/favorites namespace, unique mutex)
        else if (arg == L"-dedicated") args.dedicated = true;
        else if (arg == L"-runOnStartup") args.runOnStartup = true;

            // -config <path>  or  -config=<path>
        else if (_wcsicmp(arg.c_str(), L"-config") == 0 && i + 1 < argc)
            args.configPath = argv[++i];
        else if (_wcsnicmp(arg.c_str(), L"-config=", 8) == 0)
            args.configPath = arg.substr(8);

            // -slideshowTransitions=<a,b,c>  — custom list. Must be tested BEFORE
            // -slideshowTransition= or the shorter prefix would swallow it.
        else if (_wcsnicmp(arg.c_str(), L"-slideshowTransitions=", 22) == 0) {
            args.transitionList = ParseTransitionList(arg.substr(22));
            args.transitionListGiven = true;
        }

            // -slideshowTransitionSource=none|all|list
        else if (_wcsnicmp(arg.c_str(), L"-slideshowTransitionSource=", 27) == 0)
            args.transitionSource = ParseTransitionSource(arg.substr(27));

            // -slideshowTransitionOrder=sequential|random
        else if (_wcsnicmp(arg.c_str(), L"-slideshowTransitionOrder=", 26) == 0)
            args.transitionOrder = ParseTransitionOrder(arg.substr(26));

            // -slideshowTransitionShuffle
        else if (_wcsicmp(arg.c_str(), L"-slideshowTransitionShuffle") == 0)
            args.transitionShuffle = true;

            // -slideshowTransition=<name>  — single transition
        else if (_wcsnicmp(arg.c_str(), L"-slideshowTransition=", 21) == 0) {
            args.slideshowTransition = ParseTransitionType(arg.substr(21));
            args.transitionSpecified = true;
        }

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
    // Transition switches — each one only touches state it was actually given,
    // so an absent switch leaves the SAVED setting intact. (Assigning the type
    // unconditionally here used to reset every launch to Cut, because that is
    // the struct's default when no -slideshowTransition= was passed.)
    {
        namespace TS = Constants::Slideshow::TransitionSource;
        namespace TO = Constants::Slideshow::TransitionOrder;
        auto &tr = app.slideshow.transition;

        if (args.transitionSpecified) {
            tr.type   = args.slideshowTransition;
            tr.source = TS::NONE; // "use exactly this one"
        }
        if (args.transitionShuffle) { // legacy switch
            tr.source = TS::ALL;
            tr.order  = TO::RANDOM;
        }
        if (args.transitionListGiven) {
            tr.listMask = args.transitionList;
            tr.source   = TS::LIST;
        }
        // Explicit source/order come last so they win over the implications above.
        if (args.transitionSource >= 0) tr.source = args.transitionSource;
        if (args.transitionOrder  >= 0) tr.order  = args.transitionOrder;
        tr.seqIndex = 0;
    }

    // 1a. Dedicated mode (affects history file and registry — must be set before any of that)
    if (args.dedicated) app.isDedicated = true;

    // 2. KIOSK lock. app.isLocked already carries the STORED value at this point
    // (RegistryManager loaded it, or the .ini did for a dedicated screen), so the
    // switch can only force it on for this launch — it never clears a configured
    // lock, and never writes one back.
    if (args.lock) app.isLocked = true;
    // Same one-way rule. The request itself is armed further down, once it is
    // known whether the window is being shown at all.
    if (args.keepAwake) app.keepDisplayAwake = true;

    // 3. Position on a specific monitor before the window is shown
    if (args.monitorNum >= 1) {
        struct MonitorInfo {
            int target, current;
            RECT rcWork;
            bool found;
        };
        MonitorInfo mi{args.monitorNum, 0, {}, false};
        EnumDisplayMonitors(nullptr, nullptr,
                            [](HMONITOR hMon, HDC, RECT *, LPARAM lp) -> BOOL {
                                auto *m = reinterpret_cast<MonitorInfo *>(lp);
                                if (++m->current == m->target) {
                                    MONITORINFO miMon = {sizeof(miMon)};
                                    GetMonitorInfoW(hMon, &miMon);
                                    m->rcWork = miMon.rcWork;
                                    m->found = true;
                                    return FALSE;
                                }
                                return TRUE;
                            },
                            reinterpret_cast<LPARAM>(&mi));

        if (mi.found) {
            int w = static_cast<int>(app.baseWidth  * app.dpiScale);
            int h = static_cast<int>(app.baseHeight * app.dpiScale);
            int x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - w) / 2;
            int y = mi.rcWork.top  + (mi.rcWork.bottom - mi.rcWork.top  - h) / 2;
            SetWindowPos(hWnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }

    // 4. Always on top (set before show so it takes effect immediately).
    // Same rule as the lock: the stored value is already in app.isAlwaysOnTop and
    // the switch only forces it on. Setting the FLAG as well as the z-order
    // matters — Ctrl+T reads it to decide which way to toggle, so without this a
    // window started with -awaysOnTop would need two presses to come back down.
    if (args.alwaysOnTop) app.isAlwaysOnTop = true;
    if (app.isAlwaysOnTop)
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

    // WM_SHOWWINDOW normally arms this, but ShowWindow does not send it when the
    // window is already in the requested state — so arm it once explicitly here
    // rather than depend on that. The call is idempotent.
    AppCommands::ApplyDisplayAwake(hWnd);

    // Tray icon is always present — dedicated mode requires it for kiosk control,
    // and normal mode uses it as the "hide to tray on close" target.
    AppCommands::AddTrayIcon(hWnd);

    // 7. Load content
    if (!args.startFolder.empty())
        OpenDirectory(hWnd, args.startFolder);
    else if (!args.imageFile.empty())
        OpenSpecificImage(hWnd, args.imageFile);
    else if (!Dedicated::IsDedicatedFlag())
        OpenStartupTarget(hWnd); // last image → history → chooser, in that order
    // A dedicated instance NEVER opens the file chooser. It is an unattended
    // screen — nobody is there to answer a dialog, and a modal window would sit
    // on the display forever. With no folder resolved it simply shows its
    // empty-folder overlay, which is at least diagnosable from across a room.

    // 8. Fullscreen (after show + content so renderer has correct dimensions)
    if (args.fullscreen)
        AppCommands::ToggleFullscreen(hWnd);

    // 9. Auto-start slideshow (after content is loaded so playlist exists)
    if (args.slideshow)
        AppCommands::toggleSlideshow(hWnd);

    // 10. Hide mouse cursor. Recorded so stopping the slideshow can undo it —
    // ShowCursor is a counter, and an unhidden cursor is the only way to
    // reconfigure a screen that was started with -hideMouse.
    if (args.hideMouse) {
        ShowCursor(FALSE);
        app.cursorHiddenAtStartup = true;
    }
    // Ensure window has keyboard focus — ShowWindow + WM_NCACTIVATE
    // interaction doesn't always set it reliably with WS_POPUP.
    SetForegroundWindow(hWnd);
    SetFocus(hWnd);
}
