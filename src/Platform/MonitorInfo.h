// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#pragma once
#include <windows.h>
#include <string>
#include <vector>

// =============================================================================
// MonitorInfo — the display list, in the order a person sees them.
//
// WHY THIS EXISTS AS A MODULE. Ctrl+M already enumerated and sorted monitors
// inside CommandExecuter.cpp, and the Statistics panel needs the same list. Two
// copies of "collect, then sort by virtual-desktop position" drift the moment
// one of them gains a tie-break the other does not — and they would disagree
// about which screen is "2 of 3", which is exactly the number both of them show
// the user.
//
// NAMES ARE THE POINT. `MONITORINFOEX::szDevice` is `\\.\DISPLAY1` — correct,
// unique, and useless in an overlay. What a person recognises is "DELL G3223Q",
// and getting that requires the DisplayConfig API rather than any of the older
// monitor calls. See the fallback chain in the .cpp: the friendly name is not
// always available, so this never returns an empty label.
// =============================================================================

namespace MonitorInfo {

    struct Entry {
        // What to show a person. Never empty — falls back through the device
        // string to `\\.\DISPLAYn` when nothing better can be read.
        std::wstring name;

        // `\\.\DISPLAYn`. Kept because it is the stable identifier the rest of
        // Windows uses, and because it disambiguates two identical panels that
        // report the same friendly name.
        std::wstring deviceName;

        RECT rcMonitor{};   // full bounds, virtual-desktop coordinates
        RECT rcWork{};      // minus taskbar and any appbars
        bool primary = false;

        // 0 when unknown. Per-monitor DPI, not the process-wide value: two
        // screens at different scaling is the normal case on a laptop plus an
        // external, and the app's own dpiScale describes only the one it is on.
        UINT dpi = 0;

        // 0 when unknown. Worth showing because a 60 Hz secondary beside a
        // 144 Hz primary explains a difference in feel that nothing else does.
        int refreshHz = 0;

        int Width()  const { return rcMonitor.right  - rcMonitor.left; }
        int Height() const { return rcMonitor.bottom - rcMonitor.top;  }
    };

    // All monitors, SORTED left-to-right then top-to-bottom by their
    // virtual-desktop coordinates.
    //
    // The sort is the whole contract. EnumDisplayMonitors returns whatever the
    // display driver reports, which bears no relation to the physical
    // arrangement, so "next monitor" built on raw enumeration order jumps around
    // unpredictably on a three-screen desk. Sorted, "next" means "the one to the
    // right", which is what Ctrl+M appears to promise.
    //
    // Empty only if enumeration itself fails.
    std::vector<Entry> Enumerate();

    // Index into Enumerate()'s result for the monitor a window is on, or -1.
    // Takes the list rather than re-enumerating so a caller that shows "2 of 3"
    // cannot report a position from one enumeration and a count from another.
    int IndexOfWindow(HWND hWnd, const std::vector<Entry> &monitors);

    // "DELL G3223Q (2 of 3)" — the one-line form for the centre overlay.
    // Returns just the name when there is only one monitor: "(1 of 1)" is noise
    // on a single-screen desk.
    std::wstring DescribeWindowMonitor(HWND hWnd);

} // namespace MonitorInfo
