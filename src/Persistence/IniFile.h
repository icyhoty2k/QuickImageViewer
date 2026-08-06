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
// IniFile — a plain .ini sitting beside the exe.
//
// WHAT THIS IS NOT: it is not the instance settings file. That one is
// Dedicated::SettingsFilePath(), it is named after the exe, and its mere
// EXISTENCE flips the whole application from registry-backed to file-backed
// (Dedicated::DetectStartupMode). Anything written through this header is
// invisible to that check, because the file names are fixed and never
// exe-derived — the same reasoning that already keeps the remote-servers list
// in a file of its own.
//
// WHY SEPARATE FILES AT ALL. Two reasons, and both are practical rather than
// tidy:
//
//   • WRITE AMPLIFICATION. WritePrivateProfileString rewrites the WHOLE file
//     for one key. Parking a value that changes on every exit — the last image
//     viewed — inside the settings file means rewriting every setting the
//     application has, every time the application closes, for one line. On an
//     SSD that is wasted write endurance for nothing.
//
//   • BLAST RADIUS. A file that holds one subsystem's configuration can be
//     deleted, hand-edited or copied between machines without touching
//     anything else. The listener configuration and the resume position have
//     nothing to do with each other and nothing to do with view settings.
//
// Every file created here is UTF-16LE with a BOM, because
// WritePrivateProfileStringW only writes Unicode into a file that is ALREADY
// Unicode — without the BOM every value would be narrowed to ANSI and any
// non-ASCII path would be mangled on the way in.
// =============================================================================

namespace Persistence::Ini {

    // <exe folder>\<fileName>. Empty when the module path cannot be resolved.
    // Full path, always: WritePrivateProfileStringW given a bare name writes
    // into the Windows directory rather than failing.
    std::wstring PathBesideExe(const wchar_t *fileName);

    bool Exists(const std::wstring &path);

    // Creates the file with a BOM and a comment header if it is not there.
    // No-op when it already exists. `headerComment` is written as one ';' line,
    // so a user opening the file finds out what wrote it.
    void CreateWithHeaderIfMissing(const std::wstring &path, const wchar_t *headerComment);

    // "YYYY-MM-DD HH:MM:SS", local time — these files are read by a person.
    std::wstring TimeStampNow();

    // The two header lines every generated file carries. `Generated` is written
    // once, in the creation body, and never touched again; `Updated` is rewritten
    // by TouchUpdatedStamp on each save.
    //
    // Two lines rather than one because they answer different questions, and a
    // single line relabelled on every write could answer neither honestly.
    std::wstring GeneratedStampLines();

    // Rewrites the "; Updated:" line, inserting it after "; Generated:" if it is
    // missing. Reads and rewrites the whole file — which costs nothing here,
    // because WritePrivateProfileString already does exactly that for one key.
    //
    // No-op when the file does not exist: a stamp is not worth creating a file for.
    void TouchUpdatedStamp(const std::wstring &path);

    // Creates the file with a BOM and `text` verbatim. No-op if it exists.
    //
    // For writing a fully ANNOTATED file — comments above each key — in one go.
    // WritePrivateProfileString cannot produce that: it appends bare keys. It
    // does PRESERVE surrounding comment lines when it later updates a value, so
    // laying the file out once at creation is what makes the annotations stick.
    void CreateWithTextIfMissing(const std::wstring &path, const std::wstring &text);

    // Reads grow their buffer until the value fits, so a long path is never
    // silently truncated. Empty when absent.
    std::wstring ReadString(const std::wstring &path, const wchar_t *section,
                            const wchar_t *key);

    // Creates the file (with header) on first write. Only the named key is
    // touched; every other section and key in the file survives.
    void WriteString(const std::wstring &path, const wchar_t *section,
                     const wchar_t *key, const std::wstring &value,
                     const wchar_t *headerComment);

    // Stored as decimal TEXT rather than through GetPrivateProfileInt, so the
    // file stays hand-editable and a malformed value is distinguishable from a
    // legitimate zero.
    // Removes the key outright, rather than leaving it with an empty value.
    //
    // WriteString with L"" writes `Key=`, which is NOT the same thing: it leaves
    // a line behind in a file the user is invited to read, and every reader then
    // has to treat "present but blank" and "absent" as the same case. Deleting
    // is what the caller almost always means when it clears a value.
    //
    // Does nothing when the file or the key is already gone.
    void DeleteKey(const std::wstring &path, const wchar_t *section,
                   const wchar_t *key);

    DWORD ReadDword(const std::wstring &path, const wchar_t *section,
                    const wchar_t *key, DWORD defaultValue);
    void  WriteDword(const std::wstring &path, const wchar_t *section,
                     const wchar_t *key, DWORD value, const wchar_t *headerComment);

    // The shared truthiness rule: 1/true/on/yes (or any non-zero number) is
    // true, 0/false/off/no is false, anything else falls back to `def` so a typo
    // never silently flips a setting. Whitespace and case are ignored.
    //
    // Deliberately identical to Dedicated::ParseBoolValue. Duplicated rather
    // than shared because that one is bound to the instance settings file and
    // this one must work on any file, including another machine's.
    bool ParseBool(const std::wstring &raw, bool def);

} // namespace Persistence::Ini
