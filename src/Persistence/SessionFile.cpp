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

namespace Persistence::Session {

namespace {
    namespace CS = Constants::Session;

    // Resolved once — the exe cannot move while the process runs.
    const std::wstring &Path() {
        static const std::wstring path = Ini::PathBesideExe(CS::FILE_NAME);
        return path;
    }
}

std::wstring LoadLastImage() {
    return Ini::ReadString(Path(), CS::SECTION, CS::KEY_LAST_IMAGE);
}

void SaveLastImage(const std::wstring &fullPath) {
    // Annotated on creation, so the file explains itself — see
    // RemoteSettings::SaveToIni for why this has to happen at creation time.
    if (!Ini::Exists(Path())) {
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
        body += CS::KEY_LAST_IMAGE; body += L"="; body += fullPath; body += L"\r\n";
        Ini::CreateWithTextIfMissing(Path(), body);
    }

    // Written straight through rather than queued. Called from the exit path,
    // where the point is that it reaches disk before teardown.
    Ini::WriteString(Path(), CS::SECTION, CS::KEY_LAST_IMAGE, fullPath,
                     CS::FILE_HEADER);
    // Doubles as a record of when qIV last exited.
    Ini::TouchUpdatedStamp(Path());
}

// =============================================================================
// Did the last run end properly?
// =============================================================================
PreviousRun TakePreviousRun() {
    PreviousRun out;
    out.crashed  = Ini::ReadString(Path(), CS::SECTION, CS::KEY_RUNNING) == L"1";
    out.dumpPath = Ini::ReadString(Path(), CS::SECTION, CS::KEY_CRASH_DUMP);

    // CLEARED as they are read, so one abnormal exit is reported once. Leaving
    // them would make every launch after a single crash claim a fresh one, and
    // a log that cries wolf is one nobody reads.
    if (!out.dumpPath.empty())
        Ini::WriteString(Path(), CS::SECTION, CS::KEY_CRASH_DUMP, L"", CS::FILE_HEADER);

    return out;
}

void MarkRunning(bool running) {
    // "1" while a run is under way; the key is REMOVED rather than set to "0" on
    // a clean exit. An absent key and a zero mean the same thing to the reader,
    // and an absent one leaves a tidy file — which matters for something a user
    // is invited to open and delete.
    Ini::WriteString(Path(), CS::SECTION, CS::KEY_RUNNING,
                     running ? L"1" : L"", CS::FILE_HEADER);
}

void RecordCrashDump(const wchar_t *dumpPath) {
    // ONE .ini WRITE AND NOTHING ELSE. Called from the unhandled-exception
    // filter, where the heap may be corrupt and any allocation is a gamble —
    // this is far less than the minidump write it sits beside, and the reporting
    // it enables happens next launch where everything is healthy.
    if (!dumpPath || !*dumpPath) return;
    Ini::WriteString(Path(), CS::SECTION, CS::KEY_CRASH_DUMP, dumpPath,
                     CS::FILE_HEADER);
}

} // namespace Persistence::Session
