// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#include "SessionFile.h"

#include "IniFile.h"
#include "Platform/Constants.h"
#include "Dedicated/DedicatedSettings.h"

#include <windows.h>
#include <cstdlib> // wcstol — parsing the stored "x,y,width,height"

namespace Persistence::Session {

namespace {
    namespace CS = Constants::Session;

    // Resolved once — the exe cannot move while the process runs.
    const std::wstring &Path() {
        static const std::wstring path = Ini::PathBesideExe(CS::FILE_NAME);
        return path;
    }

    // =========================================================================
    // WHERE SESSION STATE LIVES — the settings store decides, not this file.
    //
    // A copy configured through a settings .ini keeps its session state in
    // qivSession.ini beside it; a copy configured through the registry keeps it
    // in the registry. Same rule the settings themselves follow
    // (Dedicated::SettingsUseFile), so a portable copy stays entirely portable
    // and a registry copy leaves no stray file next to the exe.
    //
    // The separate FILE still matters in file mode, and for the original
    // reason: an .ini write rewrites the whole file, so session state parked in
    // the settings .ini would make every close rewrite every setting. In
    // registry mode that cost does not exist — a value is a value — so the
    // session keys simply sit beside the settings.
    //
    // Registry writes here are DIRECT, not queued through g_writeQueue. The
    // crash mark has to be on disk before a crash, and the exit-path values
    // have to land before teardown; a queue that is flushed afterwards is no
    // use to either.
    // =========================================================================
    struct Key {
        const wchar_t *ini; // key name inside [Session] in qivSession.ini
        const wchar_t *reg; // value name under the app's registry key
    };

    constexpr Key K_LAST_IMAGE  {CS::KEY_LAST_IMAGE,  CS::REG_LAST_IMAGE};
    constexpr Key K_LAST_FOLDER {CS::KEY_LAST_FOLDER, CS::REG_LAST_FOLDER};
    constexpr Key K_RUNNING     {CS::KEY_RUNNING,     CS::REG_RUNNING};
    constexpr Key K_CRASH_DUMP  {CS::KEY_CRASH_DUMP,  CS::REG_CRASH_DUMP};
    constexpr Key K_WINDOW_RECT    {CS::KEY_WINDOW_RECT,    CS::REG_WINDOW_RECT};
    constexpr Key K_WINDOW_MONITOR {CS::KEY_WINDOW_MONITOR, CS::REG_WINDOW_MONITOR};

    bool UseFile() { return Dedicated::SettingsUseFile(); }

    std::wstring ReadValue(const Key &k) {
        if (UseFile())
            return Ini::ReadString(Path(), CS::SECTION, k.ini);

        wchar_t buf[1024] = {};
        DWORD size = sizeof(buf);
        DWORD type = 0;
        const LSTATUS rc = RegGetValueW(Constants::Registry::ROOT_HIVE,
                                        Constants::Registry::ROOT_KEY,
                                        k.reg, RRF_RT_REG_SZ, &type, buf, &size);
        if (rc != ERROR_SUCCESS) return std::wstring();
        return std::wstring(buf);
    }

    // An empty value DELETES rather than writing a blank, in both stores. An
    // absent key and an empty one mean the same thing to every reader here, and
    // the absent form leaves a tidy file for something the user is invited to
    // open and delete.
    void WriteValue(const Key &k, const wchar_t *value) {
        const bool clearing = (!value || !*value);

        if (UseFile()) {
            if (clearing)
                Ini::DeleteKey(Path(), CS::SECTION, k.ini);
            else
                Ini::WriteString(Path(), CS::SECTION, k.ini, value, CS::FILE_HEADER);
            return;
        }

        HKEY hKey = nullptr;
        if (RegCreateKeyExW(Constants::Registry::ROOT_HIVE, Constants::Registry::ROOT_KEY,
                            0, nullptr, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE,
                            nullptr, &hKey, nullptr) != ERROR_SUCCESS)
            return;

        if (clearing) {
            RegDeleteValueW(hKey, k.reg);
        } else {
            const DWORD bytes = static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t));
            RegSetValueExW(hKey, k.reg, 0, REG_SZ,
                           reinterpret_cast<const BYTE *>(value), bytes);
        }
        RegCloseKey(hKey);
    }

    void WriteValue(const Key &k, const std::wstring &value) {
        WriteValue(k, value.c_str());
    }
}

std::wstring LoadLastImage() {
    return ReadValue(K_LAST_IMAGE);
}

namespace {
    // Annotated on creation, so the file explains itself — see
    // RemoteSettings::SaveToIni for why this has to happen at creation time.
    //
    // Shared because either writer can be the first to run: an empty folder
    // records a folder and no image, so SaveLastFolder cannot rely on
    // SaveLastImage having created the file already.
    void EnsureAnnotatedFile() {
        if (!UseFile()) return; // registry mode — there is no file to annotate
        if (Ini::Exists(Path())) return;

        std::wstring body = L"; QuickImageViewer\r\n; ";
        body += CS::FILE_HEADER;
        body += L"\r\n";
        body += Ini::GeneratedStampLines();
        body += L";\r\n"
                L"; Not a settings file. Nothing here is worth keeping: delete it and the\r\n"
                L"; next launch simply opens from the folder history instead.\r\n"
                L"; It is separate so that closing qIV does not rewrite every setting the\r\n"
                L"; application has just to record one line.\r\n\r\n";
        body += L"["; body += CS::SECTION; body += L"]\r\n\r\n";
        body += L"; Full path of the image on screen at the last exit. Reopened on the\r\n"
                L"; next launch. Ignored if the file no longer exists.\r\n";
        body += CS::KEY_LAST_IMAGE; body += L"=\r\n\r\n";
        body += L"; The folder that was open at the last exit. Used when the image above\r\n"
                L"; is gone, or when there was no image to record at all.\r\n";
        body += CS::KEY_LAST_FOLDER; body += L"=\r\n\r\n";
        body += L"; Main window placement at the last exit, as x,y,width,height.\r\n"
                L"; Only written and only honoured while Remember Window Position is on.\r\n";
        body += CS::KEY_WINDOW_RECT; body += L"=\r\n\r\n";
        body += L"; The display that window was on, as its device name. Used when the\r\n"
                L"; screens have been rearranged since: the window goes back to that\r\n"
                L"; SCREEN rather than to those coordinates. Ignored if it is gone.\r\n";
        body += CS::KEY_WINDOW_MONITOR; body += L"=\r\n";
        Ini::CreateWithTextIfMissing(Path(), body);
    }
}

void SaveLastImage(const std::wstring &fullPath) {
    EnsureAnnotatedFile();

    // Written straight through rather than queued. Called from the exit path,
    // where the point is that it reaches disk before teardown.
    WriteValue(K_LAST_IMAGE, fullPath);

    // Doubles as a record of when qIV last exited. File mode only — the stamp is
    // a comment line in the .ini, and there is nothing equivalent to touch in
    // the registry.
    if (UseFile())
        Ini::TouchUpdatedStamp(Path());
}

std::wstring LoadLastFolder() {
    return ReadValue(K_LAST_FOLDER);
}

void SaveLastFolder(const std::wstring &folderPath) {
    EnsureAnnotatedFile();
    WriteValue(K_LAST_FOLDER, folderPath);
}

// =============================================================================
// Main window placement
// =============================================================================
bool LoadWindowRect(int &x, int &y, int &width, int &height) {
    const std::wstring raw = ReadValue(K_WINDOW_RECT);
    if (raw.empty()) return false;

    // "x,y,width,height". Parsed by hand rather than with a stream: four
    // integers do not justify pulling <sstream> into the startup path.
    int values[4] = {0, 0, 0, 0};
    int index = 0;
    size_t pos = 0;
    while (index < 4) {
        size_t comma = raw.find(L',', pos);
        const std::wstring field =
                raw.substr(pos, comma == std::wstring::npos ? std::wstring::npos : comma - pos);
        if (field.empty()) return false;

        wchar_t *end = nullptr;
        const long parsed = wcstol(field.c_str(), &end, 10);
        if (!end || *end != L'\0') return false;
        values[index] = static_cast<int>(parsed);
        ++index;

        if (comma == std::wstring::npos) break;
        pos = comma + 1;
    }
    if (index != 4) return false;

    // A zero or negative size is not a window. Guards a hand-edited file and a
    // rect saved while minimised.
    if (values[2] <= 0 || values[3] <= 0) return false;

    x = values[0];
    y = values[1];
    width = values[2];
    height = values[3];
    return true;
}

void SaveWindowRect(int x, int y, int width, int height) {
    if (width <= 0 || height <= 0) return;

    EnsureAnnotatedFile();

    std::wstring value = std::to_wstring(x);
    value += L','; value += std::to_wstring(y);
    value += L','; value += std::to_wstring(width);
    value += L','; value += std::to_wstring(height);
    WriteValue(K_WINDOW_RECT, value);
}

void RemoveObsoleteValues() {
    // Both stores. LastFolder was written through SaveStringSetting, which
    // routes exactly the way everything else does, so a file-backed copy has it
    // sitting in its settings .ini — a dead line in a file the user is invited
    // to open and read.
    if (UseFile()) {
        Dedicated::DeleteValue(CS::REG_OBSOLETE_LAST_IMAGE);
        Dedicated::DeleteValue(CS::REG_OBSOLETE_LAST_FOLDER);
        return;
    }

    HKEY hKey = nullptr;
    if (RegOpenKeyExW(Constants::Registry::ROOT_HIVE, Constants::Registry::ROOT_KEY,
                      0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS)
        return;

    // Absent is the normal case and not an error — this runs on every launch and
    // succeeds exactly once per machine.
    RegDeleteValueW(hKey, CS::REG_OBSOLETE_LAST_IMAGE);
    RegDeleteValueW(hKey, CS::REG_OBSOLETE_LAST_FOLDER);
    RegCloseKey(hKey);
}

std::wstring LoadWindowMonitor() {
    return ReadValue(K_WINDOW_MONITOR);
}

void SaveWindowMonitor(const std::wstring &deviceName) {
    EnsureAnnotatedFile();
    WriteValue(K_WINDOW_MONITOR, deviceName);
}

void ClearWindowRect() {
    // Both, always. The monitor name is only ever meaningful together with the
    // rect it was recorded beside; leaving one behind would have the restore
    // reasoning about half a placement.
    WriteValue(K_WINDOW_RECT, L"");
    WriteValue(K_WINDOW_MONITOR, L"");
}

// =============================================================================
// Did the last run end properly?
// =============================================================================
PreviousRun TakePreviousRun() {
    PreviousRun out;
    out.crashed  = ReadValue(K_RUNNING) == L"1";
    out.dumpPath = ReadValue(K_CRASH_DUMP);

    // CLEARED as they are read, so one abnormal exit is reported once. Leaving
    // them would make every launch after a single crash claim a fresh one, and
    // a log that cries wolf is one nobody reads.
    if (!out.dumpPath.empty())
        WriteValue(K_CRASH_DUMP, L"");

    return out;
}

void MarkRunning(bool running) {
    // "1" while a run is under way; the key is REMOVED rather than set to "0" on
    // a clean exit. An absent key and a zero mean the same thing to the reader,
    // and an absent one leaves a tidy file — which matters for something a user
    // is invited to open and delete.
    WriteValue(K_RUNNING, running ? L"1" : L"");
}

void RecordCrashDump(const wchar_t *dumpPath) {
    // ONE WRITE AND NOTHING ELSE. Called from the unhandled-exception filter,
    // where the heap may be corrupt and any allocation is a gamble — this is far
    // less than the minidump write it sits beside, and the reporting it enables
    // happens next launch where everything is healthy. The pointer is passed
    // straight through to the store for that reason: no wstring is built here.
    if (!dumpPath || !*dumpPath) return;
    WriteValue(K_CRASH_DUMP, dumpPath);
}

} // namespace Persistence::Session
