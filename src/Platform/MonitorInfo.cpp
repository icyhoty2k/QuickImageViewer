// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#include "MonitorInfo.h"

#include <shellscalingapi.h>   // GetDpiForMonitor
#include <algorithm>

// NO #pragma comment(lib, "Shcore.lib") HERE — CMake already lists shcore, so
// this was a duplicate of the real dependency list rather than part of it.

namespace MonitorInfo {

namespace {

    // -------------------------------------------------------------------------
    // Friendly names, and why this is not one API call.
    //
    // Three sources, tried in order, each less useful than the last:
    //
    //   1. QueryDisplayConfig + DISPLAYCONFIG_TARGET_DEVICE_NAME
    //      The only call that yields "DELL G3223Q" — the string the monitor
    //      reports in its EDID. Windows 7+.
    //
    //   2. EnumDisplayDevices with EDD_GET_DEVICE_INTERFACE_NAME
    //      Usually "Generic PnP Monitor". Not useful for telling two screens
    //      apart, but better than a device path when (1) fails.
    //
    //   3. `\\.\DISPLAYn` itself.
    //
    // (1) fails more often than its documentation suggests — remote sessions,
    // some virtual displays, and drivers that report an empty EDID string all
    // land in (2) or (3). A monitor with no label is worse than an ugly one, so
    // the chain never returns empty.
    // -------------------------------------------------------------------------
    std::wstring FriendlyNameFor(const std::wstring &deviceName) {

        UINT32 pathCount = 0, modeCount = 0;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount)
                == ERROR_SUCCESS && pathCount > 0) {

            std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
            std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);

            if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(),
                                   &modeCount, modes.data(), nullptr) == ERROR_SUCCESS) {
                paths.resize(pathCount);

                for (const DISPLAYCONFIG_PATH_INFO &p : paths) {
                    // The SOURCE name is what maps a path back to `\\.\DISPLAYn`;
                    // the TARGET name is what carries the human label. Both have
                    // to be asked for separately, and matched through the path.
                    DISPLAYCONFIG_SOURCE_DEVICE_NAME src = {};
                    src.header.type      = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
                    src.header.size      = sizeof(src);
                    src.header.adapterId = p.sourceInfo.adapterId;
                    src.header.id        = p.sourceInfo.id;
                    if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS) continue;
                    if (deviceName != src.viewGdiDeviceName) continue;

                    DISPLAYCONFIG_TARGET_DEVICE_NAME tgt = {};
                    tgt.header.type      = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
                    tgt.header.size      = sizeof(tgt);
                    tgt.header.adapterId = p.targetInfo.adapterId;
                    tgt.header.id        = p.targetInfo.id;
                    if (DisplayConfigGetDeviceInfo(&tgt.header) != ERROR_SUCCESS) continue;

                    if (tgt.monitorFriendlyDeviceName[0] != L'\0')
                        return tgt.monitorFriendlyDeviceName;
                }
            }
        }

        // (2) — the adapter's own description of what is plugged in.
        DISPLAY_DEVICEW dd = {sizeof(dd)};
        if (EnumDisplayDevicesW(deviceName.c_str(), 0, &dd, 0) && dd.DeviceString[0] != L'\0')
            return dd.DeviceString;

        // (3) — always something.
        return deviceName;
    }

    int RefreshHzFor(const std::wstring &deviceName) {
        DEVMODEW dm = {};
        dm.dmSize = sizeof(dm);
        if (!EnumDisplaySettingsW(deviceName.c_str(), ENUM_CURRENT_SETTINGS, &dm)) return 0;

        // 0 and 1 are documented as "the adapter's default", not as a rate.
        // Reporting either as Hz would put "1 Hz" on screen.
        if (dm.dmDisplayFrequency <= 1) return 0;
        return static_cast<int>(dm.dmDisplayFrequency);
    }

    BOOL CALLBACK CollectProc(HMONITOR hMon, HDC, LPRECT, LPARAM lParam) {
        auto *out = reinterpret_cast<std::vector<Entry> *>(lParam);

        MONITORINFOEXW mi = {};
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(hMon, &mi)) return TRUE;   // skip it, keep enumerating

        Entry e;
        e.deviceName = mi.szDevice;
        e.name       = FriendlyNameFor(e.deviceName);
        e.rcMonitor  = mi.rcMonitor;
        e.rcWork     = mi.rcWork;
        e.primary    = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
        e.refreshHz  = RefreshHzFor(e.deviceName);

        UINT dpiX = 0, dpiY = 0;
        if (SUCCEEDED(GetDpiForMonitor(hMon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)))
            e.dpi = dpiX;

        out->push_back(std::move(e));
        return TRUE;
    }

} // namespace

std::vector<Entry> Enumerate() {
    std::vector<Entry> mons;
    if (!EnumDisplayMonitors(nullptr, nullptr, CollectProc,
                             reinterpret_cast<LPARAM>(&mons)))
        return mons;   // empty; every caller already handles that

    std::sort(mons.begin(), mons.end(), [](const Entry &a, const Entry &b) {
        if (a.rcMonitor.left != b.rcMonitor.left)
            return a.rcMonitor.left < b.rcMonitor.left;
        return a.rcMonitor.top < b.rcMonitor.top;
    });
    return mons;
}

int IndexOfWindow(HWND hWnd, const std::vector<Entry> &monitors) {
    if (monitors.empty()) return -1;

    MONITORINFOEXW cur = {};
    cur.cbSize = sizeof(cur);
    if (!GetMonitorInfoW(MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST), &cur))
        return -1;

    // Matched on the device name rather than the rect. Two monitors cannot share
    // a device name, but they CAN share a top-left corner while a display change
    // is being applied — during which a rect match briefly picks the wrong one.
    for (size_t i = 0; i < monitors.size(); ++i)
        if (monitors[i].deviceName == cur.szDevice)
            return static_cast<int>(i);

    // The name lookup can miss if the display set changed between the two calls.
    // Position is the weaker test but better than reporting "unknown".
    for (size_t i = 0; i < monitors.size(); ++i)
        if (monitors[i].rcMonitor.left == cur.rcMonitor.left &&
            monitors[i].rcMonitor.top  == cur.rcMonitor.top)
            return static_cast<int>(i);

    return -1;
}

std::wstring DescribeWindowMonitor(HWND hWnd) {
    const std::vector<Entry> mons = Enumerate();
    if (mons.empty()) return std::wstring();

    const int idx = IndexOfWindow(hWnd, mons);
    if (idx < 0) return std::wstring();

    // One screen: the count adds nothing. "(1 of 1)" reads like a defect.
    if (mons.size() == 1) return mons[0].name;

    return mons[idx].name + L" (" + std::to_wstring(idx + 1) + L" of " +
           std::to_wstring(mons.size()) + L")";
}

} // namespace MonitorInfo
