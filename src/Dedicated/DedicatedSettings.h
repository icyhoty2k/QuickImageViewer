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

// =============================================================================
// DedicatedSettings — per-instance settings file, replacing the registry.
//
// RULE: a dedicated instance NEVER touches the registry. Not its settings, not
// the Run key, not file associations. Everything it persists goes into
//
//     <exe folder>\qivDedicated<Name>.ini
//
// Why: dedicated copies are meant to be dropped on a machine, pointed at a
// monitor and forgotten — often several at once. Registry writes would make
// them collide with each other and with the main app, leave residue behind, and
// need permissions that a locked-down display machine may not grant. A file
// next to the exe keeps each instance self-contained and portable, which is the
// same reason the app itself ships as a single portable EXE.
//
// The interception lives inside Persistence::Registry: SaveSetting / LoadSetting
// and friends check SettingsUseFile() and route here instead. Every existing
// call site is unchanged — they keep calling the same functions.
//
// FILE LAYOUT — identity first, so the file says what it is at a glance:
//
//     ; QuickImageViewer - dedicated instance settings
//     [Instance]
//     Name=Lobby-Screen
//     Description=Reception, left wall
//     Version=2.12.2.0
//
//     [Settings]
//     qivViewMode=1
//     qivSlideshowIntervalMs=5000
//     ...            (the same keys the main app persists to the registry)
//
// Version records the build that last wrote the file, so an old INI found next
// to a newer exe can be recognised.
//
// The file is created as UTF-16LE with a BOM. That is deliberate: the Win32
// profile API only writes Unicode into a file that is ALREADY Unicode, and
// folder paths stored here can contain non-ASCII characters.
// =============================================================================

namespace Dedicated {

// How this process persists its settings, decided ONCE at startup from the
// filesystem — never from a flag that could be forgotten or mistyped.
// Three independent questions, resolved in this order:
//
//   1. Does an .ini exist beside the exe?
//        The EXE NAME is only a trigger: a name containing "dedicated" with no
//        .ini causes a DEFAULT ONE TO BE GENERATED. After that the name no
//        longer matters — the file does.
//   2. WHERE do settings live?
//        .ini present → the file, registry untouched. Absent → registry.
//   3. Is this a DEDICATED instance?
//        The [Instance]Dedicated key inside the .ini decides. On/true/yes or
//        any non-zero number = yes; 0/off/false/no = no. So an .ini can also be
//        used purely for portable, file-backed settings on a normal copy.
enum class StartupMode {
    Registry,   // no .ini → registry, exactly as before
    File,       // .ini present → settings come from it
    NeedsSetup, // .ini was generated THIS launch → File-backed, needs configuring
};

// Points this process at an explicit .ini instead of the exe-derived one.
// Must be called BEFORE DetectStartupMode (i.e. straight after parsing
// -config). Lets one exe serve several configured screens.
void SetSettingsFileOverride(const std::wstring &iniPath);

// Resolves the mode. Call once, before anything reads a setting. Idempotent.
StartupMode DetectStartupMode();
StartupMode CurrentMode();

// True whenever an .ini backs this process — the registry is then off limits.
// Independent of the Dedicated flag: a portable copy may be file-backed while
// still behaving like the normal viewer.
bool SettingsUseFile();

// The [Instance]Dedicated flag. True → isolation, no history/favorites,
// promotions, dedicated icon. Defaults to whether the exe name says "dedicated"
// when the key is absent.
bool IsDedicatedFlag();

// <exe path with .ini extension>. Always resolvable, even in Registry mode,
// because the setup panel needs somewhere to write the file it creates.
const std::wstring &SettingsFilePath();

// Name this instance identifies itself by: [Instance]Name from the .ini when
// present, otherwise the exe's own file name. Empty for a normal copy.
const std::wstring &InstanceName();

// DWORD values, stored as decimal text under a single [Settings] section.
void  WriteDword(const wchar_t *valueName, DWORD value);
DWORD ReadDword(const wchar_t *valueName, DWORD defaultValue);

// String values (paths and similar).
void         WriteString(const wchar_t *valueName, const std::wstring &value);
std::wstring ReadString(const wchar_t *valueName);

// Removes a value from [Settings] outright — used to drop keys that older
// builds wrote and nothing reads any more. Writing an empty string would leave
// the dead line visible in a file the user is invited to open.
void         DeleteValue(const wchar_t *valueName);

// --- Arbitrary section access ----------------------------------------------
// The four functions above are [Settings]-only, and the [Instance] block has its
// own pair. A subsystem that owns a whole section of the file — [REMOTE_TCP_IP]
// is the first — goes through these instead of growing yet another hardcoded
// pair. Same file, same UTF-16LE+BOM guarantee, same per-key write semantics:
// only the named key is touched, every other section survives untouched.
std::wstring ReadSectionString(const wchar_t *section, const wchar_t *key);
void         WriteSectionString(const wchar_t *section, const wchar_t *key,
                                const std::wstring &value);
DWORD        ReadSectionDword(const wchar_t *section, const wchar_t *key,
                              DWORD defaultValue);
void         WriteSectionDword(const wchar_t *section, const wchar_t *key, DWORD value);

// The .ini's shared truthiness rule, exposed so every section parses flags the
// same way: 1/true/on/yes (or any non-zero number) = true, 0/false/off/no =
// false, anything else falls back to `def` so a typo never silently flips a
// setting. Whitespace and case are ignored.
bool ParseBoolValue(const std::wstring &raw, bool def);

// --- [Instance] identity block ---------------------------------------------
// Creates the file with its header if absent, then writes name/description and
// stamps the current app version. Safe to call on every launch.
void EnsureSettingsFile(const std::wstring &name, const std::wstring &description);

std::wstring ReadInstanceName();
std::wstring ReadInstanceDescription();
std::wstring ReadInstanceVersion();

// Generic [Instance] access, for keys that describe the instance itself rather
// than a mirrored app setting (mutex override, list file names, …).
std::wstring ReadInstanceString(const wchar_t *key);
void         WriteInstanceString(const wchar_t *key, const std::wstring &value);

// Optional [Instance]Mutex override. Empty when absent — callers then derive
// the mutex from the exe name instead. Writing it makes the single-instance
// identity explicit and hand-editable.
std::wstring ReadInstanceMutex();
void         WriteInstanceMutex(const std::wstring &mutexName);

// This exe's file name without folder or extension. The fallback identity for
// every copy, dedicated or not.
std::wstring ExeStemName();

} // namespace Dedicated
