// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#pragma once
#include <intsafe.h>
#include <string>
#include <windows.h>
#include "../Platform/Constants.h"

struct AppState;

namespace Persistence::Registry {
    // Infrastructure
    void RegisterAppForOpenWith();

    void EnableRunOnStartup(bool isEnabledRunOnStartup);

    // Integer/Flag persistence (DWORD)
    void SaveSetting(const wchar_t *valueName, DWORD value);

    DWORD LoadSetting(const wchar_t *valueName, DWORD defaultValue);

    // Text/Path persistence (String)
    void SaveStringSetting(const wchar_t *valueName, const std::wstring &value);

    // NOTE: the old raw-buffer overload was removed — it had no callers and left
    // the caller's buffer UNTOUCHED when the value was missing, so a stack buffer
    // would have been read as garbage. Use the wstring version below.

    // Returns the registry string value as std::wstring; empty on missing/error.
    // Handles paths of any length (no MAX_PATH limit).
    std::wstring LoadStringSetting(const wchar_t *valueName);

    // Load all persisted user settings from the registry into app.
    // Mirrors the startup load block in wWinMain — call this instead of duplicating it.
    void LoadAllSettings(AppState &app);

    // Walks every persistable setting as a (registry key name, current value)
    // pair, in one place.
    //
    // WHY IT EXISTS: this list had been written out inline by the tray's Export
    // Settings, and the remote panel's .ini seeding needs exactly the same set.
    // Two copies would drift the first time somebody added a setting to one and
    // not the other — and the failure would be silent, an .ini quietly missing a
    // value that then reverts to its default. Both callers walk THIS.
    //
    // Every value fits a DWORD; the float ones are already stored scaled (theme
    // factor ×100, click zoom via Converters::toZoomInt) exactly as the registry
    // holds them, so a consumer writes what it is given without reinterpreting.
    void ForEachSetting(const AppState &app,
                        void (*fn)(const wchar_t *key, DWORD value, void *ctx),
                        void *ctx);

    // Returns the full path of this exe as std::wstring.
    // Handles paths of any length — never truncates.
    inline std::wstring GetExePathW() {
        std::wstring buf(Constants::MAX_FILE_PATH, L'\0');
        DWORD len = GetModuleFileNameW(nullptr, buf.data(), Constants::MAX_FILE_PATH);
        if (len == 0 || len >= Constants::MAX_FILE_PATH) return {};
        buf.resize(len);
        return buf;
    }
}
