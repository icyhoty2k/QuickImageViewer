// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#include "DedicatedSettings.h"
#include "DedicatedInstance.h"
#include "Persistence/RegistryManager.h" // GetExePathW
#include "Persistence/IniFile.h"         // Generated / Updated stamps
#include "Platform/Constants.h"
#include <algorithm>
#include <cwctype>

namespace Dedicated {

namespace {
    constexpr const wchar_t *SECTION          = L"Settings";
    constexpr const wchar_t *SECTION_INSTANCE = L"Instance";
    constexpr const wchar_t *KEY_NAME         = L"Name";
    constexpr const wchar_t *KEY_DESCRIPTION  = L"Description";
    constexpr const wchar_t *KEY_VERSION      = L"Version";
    constexpr const wchar_t *KEY_MUTEX        = L"Mutex";
    constexpr const wchar_t *KEY_DEDICATED    = L"Dedicated";

    void WriteDefaultIni(); // defined once the file writers below are available

    // <exe path>.ini — resolved once. Derived from the EXE rather than from a
    // configured name, so the file is unique by construction: the filesystem
    // already guarantees no two exes in one folder share a name.
    // WritePrivateProfileStringW needs the FULL path, else it writes into the
    // Windows directory.
    std::wstring g_override; // set by -config, consumed on first resolve

    const std::wstring &ResolvePath() {
        static const std::wstring path = [] {
            if (!g_override.empty()) return g_override;
            std::wstring exe = Persistence::Registry::GetExePathW();
            if (exe.empty()) return std::wstring();
            const size_t dot   = exe.find_last_of(L'.');
            const size_t slash = exe.find_last_of(L"\\/");
            if (dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash))
                exe.resize(dot);
            return exe + Constants::Dedicated::SETTINGS_FILE_EXT;
        }();
        return path;
    }

    // The exe's file name without folder or extension.
    std::wstring ExeStem() {
        std::wstring exe = Persistence::Registry::GetExePathW();
        const size_t slash = exe.find_last_of(L"\\/");
        if (slash != std::wstring::npos) exe.erase(0, slash + 1);
        const size_t dot = exe.find_last_of(L'.');
        if (dot != std::wstring::npos) exe.resize(dot);
        return exe;
    }

    // Case-insensitive: the exe name is lower-cased, and EXE_DEDICATED_MARKER is
    // required to be lower case, so any casing in the file name matches.
    bool ExeNameSaysDedicated() {
        std::wstring stem = ExeStem();
        std::transform(stem.begin(), stem.end(), stem.begin(), ::towlower);
        return stem.find(Constants::Dedicated::EXE_DEDICATED_MARKER) != std::wstring::npos;
    }

    bool FileExists(const std::wstring &p) {
        if (p.empty()) return false;
        const DWORD attr = GetFileAttributesW(p.c_str());
        return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
    }

    StartupMode  g_mode      = StartupMode::Registry;
    bool         g_resolved  = false;
    bool         g_dedicated = false;
    std::wstring g_name;

    // "1" / "true" / "on" / "yes" — or any non-zero number — mean yes.
    // "0" / "false" / "off" / "no" mean no. Anything else falls back to `def`,
    // so a typo never silently flips an instance's identity.
    bool ParseFlag(const std::wstring &raw, bool def) {
        if (raw.empty()) return def;

        std::wstring v;
        for (wchar_t c : raw)
            if (c != L' ' && c != L'\t') v += static_cast<wchar_t>(::towlower(c));
        if (v.empty()) return def;

        if (v == L"1" || v == L"true" || v == L"on"  || v == L"yes") return true;
        if (v == L"0" || v == L"false"|| v == L"off" || v == L"no")  return false;

        // Any other number: non-zero = on. Lets "2", "99" behave sensibly
        // rather than being rejected.
        bool digits = true;
        for (wchar_t c : v)
            if (c < L'0' || c > L'9') { digits = false; break; }
        if (digits) return _wtoi64(v.c_str()) != 0;

        return def;
    }
}

// =============================================================================
// DetectStartupMode
//
// The whole point of deciding this from the filesystem is that it cannot be got
// wrong at launch time: an .ini beside the exe means "this copy is self-
// contained", full stop. A missing .ini on an exe NAMED dedicated means the copy
// was prepared but never configured — that must open the setup panel, not
// silently fall back to the registry and start fighting the main app.
// =============================================================================
void SetSettingsFileOverride(const std::wstring &iniPath) {
    g_override = iniPath; // must land before the first ResolvePath() call
}

StartupMode DetectStartupMode() {
    if (g_resolved) return g_mode;
    g_resolved = true;

    const std::wstring &ini      = ResolvePath();
    const bool nameSaysDedicated = ExeNameSaysDedicated();
    bool generated = false;

    // 1. The exe NAME is only a trigger: no .ini beside a *dedicated* exe means
    //    this copy was prepared but never configured, so write a default file
    //    rather than falling back to the registry and fighting the main app.
    if (!FileExists(ini) && nameSaysDedicated) {
        WriteDefaultIni();
        generated = FileExists(ini);
    }

    // 2. From here the FILE decides, not the name. An .ini beside any copy —
    //    however it is named — means settings live in it.
    if (FileExists(ini)) {
        g_mode = generated ? StartupMode::NeedsSetup : StartupMode::File;
        g_name = ReadInstanceName();            // authoritative when present
        if (g_name.empty()) g_name = ExeStem(); // else the exe's own name

        // 3. The flag inside decides whether this behaves as a dedicated
        //    appliance. Absent → assume yes only if the exe name says so, which
        //    lets an .ini serve as plain portable settings on a normal copy.
        g_dedicated = ParseFlag(ReadInstanceString(KEY_DEDICATED), nameSaysDedicated);
    } else {
        g_mode = StartupMode::Registry;
        g_dedicated = false;
        g_name.clear();
    }

    // Keep the shared runtime state in step so the identity helpers, the icon
    // and the promotions engine all agree without re-deriving any of this.
    State().active = g_dedicated;
    if (g_dedicated && State().config.name.empty())
        State().config.name = SanitizeInstanceName(g_name);

    return g_mode;
}

bool IsDedicatedFlag() {
    CurrentMode();
    return g_dedicated;
}

StartupMode CurrentMode() {
    return g_resolved ? g_mode : DetectStartupMode();
}

const std::wstring &InstanceName() {
    CurrentMode();
    return g_name;
}

namespace {
    // Creates the file with its [Instance] header when it does not exist yet.
    //
    // Written by hand rather than through the profile API for two reasons:
    // the header comment and section ORDER can be controlled (identity first),
    // and the file is stamped UTF-16LE with a BOM — WritePrivateProfileStringW
    // only writes Unicode into a file that is already Unicode, so without this
    // every subsequent value would be narrowed to ANSI and non-ASCII folder
    // paths would be mangled.
    void CreateWithHeaderIfMissing(const std::wstring &path) {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                               CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return; // already exists, or unwritable

        std::wstring head;
        head += static_cast<wchar_t>(0xFEFF); // UTF-16LE BOM — must be first
        head += L"; QuickImageViewer - instance settings.\r\n"
                L";\r\n"
                L"; THIS FILE EXISTING is what makes the copy beside it file-backed:\r\n"
                L"; it then reads and writes every setting here and never touches the\r\n"
                L"; registry. Delete it and that copy goes back to the registry, losing\r\n"
                L"; whatever is stored here.\r\n"
                L";\r\n"
                L"; Remote control is NOT configured here - see qivLocalServer.ini.\r\n"
                L";\r\n";
        head += Persistence::Ini::GeneratedStampLines();
        head += L"\r\n";
        head += L"["; head += SECTION_INSTANCE; head += L"]\r\n";
        head += L"; Name         what this instance calls itself\r\n"
                L"; Description  free text, for your own reference\r\n"
                L"; Version      the qIV build that last wrote this file\r\n"
                L"; Dedicated    1 = isolated appliance (own history/favourites, own icon)\r\n"
                L";              0 = ordinary copy that still keeps its settings here\r\n"
                L"; Mutex        single-instance identity; pins it across an exe rename\r\n";
        // Placeholders so the identity keys keep their position at the top; the
        // writers below fill them in immediately after.
        head += L"Name=\r\nDescription=\r\nVersion=\r\nDedicated=\r\nMutex=\r\n";
        head += L"\r\n";
        head += L"["; head += SECTION; head += L"]\r\n";
        head += L"; Application settings, written by qIV. Same names as the registry\r\n"
                L"; values they replace. Edit with the app closed.\r\n";

        DWORD written = 0;
        WriteFile(h, head.data(),
                  static_cast<DWORD>(head.size() * sizeof(wchar_t)), &written, nullptr);
        CloseHandle(h);
    }

    // Reads one key, growing the buffer until the value fits — a stored folder
    // path can exceed the usual 512-char guess.
    std::wstring ReadStringFrom(const wchar_t *section, const wchar_t *key) {
        const std::wstring &path = ResolvePath();
        if (path.empty() || !section || !key) return {};

        std::wstring buf(512, L'\0');
        for (;;) {
            const DWORD n = GetPrivateProfileStringW(section, key, L"", buf.data(),
                                                     static_cast<DWORD>(buf.size()),
                                                     path.c_str());
            // A result of size-1 means the value was truncated to fit.
            if (n < buf.size() - 1 || buf.size() >= Constants::MAX_FILE_PATH) {
                buf.resize(n);
                return buf;
            }
            buf.assign(buf.size() * 2, L'\0');
        }
    }

    // Writes a usable default .ini for a copy whose exe name says "dedicated"
    // but which has never been configured. Deliberately minimal: identity only.
    // The content folders are left blank so the F8 panel opens with something to
    // fill in, rather than the instance silently running on invented paths.
    void WriteDefaultIni() {
        const std::wstring &path = ResolvePath();
        if (path.empty()) return;

        CreateWithHeaderIfMissing(path);
        const std::wstring stem = ExeStem();
        WritePrivateProfileStringW(SECTION_INSTANCE, KEY_NAME, stem.c_str(), path.c_str());
        WritePrivateProfileStringW(SECTION_INSTANCE, KEY_DESCRIPTION,
                                   L"", path.c_str());
        WritePrivateProfileStringW(SECTION_INSTANCE, KEY_VERSION,
                                   Constants::APP_VERSION, path.c_str());
        WritePrivateProfileStringW(SECTION_INSTANCE, KEY_DEDICATED, L"1", path.c_str());
        // Pin the single-instance identity now, so a later rename of the exe
        // does not silently move this instance into a different slot.
        // DefaultMutexName() folds in the folder, so two identically-named
        // copies in different folders still get distinct slots.
        WritePrivateProfileStringW(SECTION_INSTANCE, KEY_MUTEX,
                                   DefaultMutexName().c_str(), path.c_str());
    }
}

bool SettingsUseFile() {
    // NeedsSetup counts as file-backed on purpose: an unconfigured dedicated
    // copy must stay off the registry entirely rather than writing to it while
    // the user is still filling in the setup panel.
    return CurrentMode() != StartupMode::Registry && !ResolvePath().empty();
}

const std::wstring &SettingsFilePath() {
    return ResolvePath();
}

void EnsureSettingsFile(const std::wstring &name, const std::wstring &description) {
    const std::wstring &path = ResolvePath();
    if (path.empty()) return;

    CreateWithHeaderIfMissing(path);
    WritePrivateProfileStringW(SECTION_INSTANCE, KEY_NAME, name.c_str(), path.c_str());
    WritePrivateProfileStringW(SECTION_INSTANCE, KEY_DESCRIPTION, description.c_str(),
                               path.c_str());
    // Configuring through the panel means this IS a dedicated instance.
    WritePrivateProfileStringW(SECTION_INSTANCE, KEY_DEDICATED, L"1", path.c_str());
    // Stamped every launch, so the file records the build that last wrote it.
    WritePrivateProfileStringW(SECTION_INSTANCE, KEY_VERSION,
                               Constants::APP_VERSION, path.c_str());
}

std::wstring ReadInstanceName()        { return ReadStringFrom(SECTION_INSTANCE, KEY_NAME); }
std::wstring ReadInstanceDescription() { return ReadStringFrom(SECTION_INSTANCE, KEY_DESCRIPTION); }
std::wstring ReadInstanceVersion()     { return ReadStringFrom(SECTION_INSTANCE, KEY_VERSION); }
std::wstring ReadInstanceString(const wchar_t *key) {
    return ReadStringFrom(SECTION_INSTANCE, key);
}

void WriteInstanceString(const wchar_t *key, const std::wstring &value) {
    const std::wstring &path = ResolvePath();
    if (path.empty() || !key) return;
    CreateWithHeaderIfMissing(path);
    WritePrivateProfileStringW(SECTION_INSTANCE, key, value.c_str(), path.c_str());
}

std::wstring ReadInstanceMutex() { return ReadInstanceString(KEY_MUTEX); }

void WriteInstanceMutex(const std::wstring &mutexName) {
    WriteInstanceString(KEY_MUTEX, mutexName);
}

std::wstring ExeStemName() { return ExeStem(); }

void WriteDword(const wchar_t *valueName, DWORD value) {
    const std::wstring &path = ResolvePath();
    if (path.empty() || !valueName) return;
    CreateWithHeaderIfMissing(path); // keep [Instance] above [Settings]
    WritePrivateProfileStringW(SECTION, valueName,
                               std::to_wstring(value).c_str(), path.c_str());
}

DWORD ReadDword(const wchar_t *valueName, DWORD defaultValue) {
    const std::wstring &path = ResolvePath();
    if (path.empty() || !valueName) return defaultValue;
    // GetPrivateProfileIntW is unsigned-safe for the value range we store and
    // returns the default when the key is missing.
    return static_cast<DWORD>(
        GetPrivateProfileIntW(SECTION, valueName, static_cast<INT>(defaultValue),
                              path.c_str()));
}

void WriteString(const wchar_t *valueName, const std::wstring &value) {
    const std::wstring &path = ResolvePath();
    if (path.empty() || !valueName) return;
    WritePrivateProfileStringW(SECTION, valueName, value.c_str(), path.c_str());
}

std::wstring ReadString(const wchar_t *valueName) {
    return ReadStringFrom(SECTION, valueName);
}

// --- Arbitrary section access ----------------------------------------------
// CreateWithHeaderIfMissing runs on every write so a subsystem that configures
// itself before anything else has touched the file still gets a valid UTF-16LE
// file with [Instance] at the top, rather than an ANSI one the profile API
// would then narrow every later value into.

std::wstring ReadSectionString(const wchar_t *section, const wchar_t *key) {
    return ReadStringFrom(section, key);
}

void WriteSectionString(const wchar_t *section, const wchar_t *key,
                        const std::wstring &value) {
    const std::wstring &path = ResolvePath();
    if (path.empty() || !section || !key) return;
    CreateWithHeaderIfMissing(path);
    WritePrivateProfileStringW(section, key, value.c_str(), path.c_str());
}

DWORD ReadSectionDword(const wchar_t *section, const wchar_t *key, DWORD defaultValue) {
    const std::wstring &path = ResolvePath();
    if (path.empty() || !section || !key) return defaultValue;
    return static_cast<DWORD>(
        GetPrivateProfileIntW(section, key, static_cast<INT>(defaultValue), path.c_str()));
}

void WriteSectionDword(const wchar_t *section, const wchar_t *key, DWORD value) {
    WriteSectionString(section, key, std::to_wstring(value));
}

bool ParseBoolValue(const std::wstring &raw, bool def) {
    return ParseFlag(raw, def);
}

} // namespace Dedicated
