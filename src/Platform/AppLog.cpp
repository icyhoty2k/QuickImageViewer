// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#include "AppLog.h"

#include <windows.h>

#include "Platform/Constants.h"
#include "Persistence/IniFile.h"   // PathBesideExe — the logs\ folder

#include <atomic>
#include <cstdio>   // swprintf_s

namespace AppLog {

namespace {

    // Outside every lock: with the log off, a call site costs one relaxed load
    // and returns. That is what makes it acceptable to put a Write on a path
    // that runs per image.
    std::atomic<bool> g_on{false};

    Persistence::RotatingLogFile g_file;

    // "2026-08-06 04:52:39.735", local time. The same shape the TCP/IP log
    // writes, because one viewer configuration has to parse both files.
    std::wstring StampNow() {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        wchar_t b[40];
        swprintf_s(b, L"%04u-%02u-%02u %02u:%02u:%02u.%03u",
                   st.wYear, st.wMonth, st.wDay,
                   st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        return b;
    }

    std::wstring ComputerName() {
        wchar_t name[MAX_COMPUTERNAME_LENGTH + 1] = {};
        DWORD   n = MAX_COMPUTERNAME_LENGTH + 1;
        if (!GetComputerNameW(name, &n)) return L"(unknown)";
        return name;
    }

    // Tabs, newlines and '|' out — '|' is the separator inside the message, and
    // a path or an error string carrying one would split a row that never
    // happened. Same rule, and the same reasoning, as RemoteLog::Flatten.
    std::wstring Flatten(const std::wstring &s) {
        std::wstring out = s;
        for (wchar_t &c : out)
            if (c == L'\t' || c == L'\r' || c == L'\n') c = L' ';
            else if (c == L'|') c = L'/';
        return out;
    }

    // THE HEADER ON EVERY FILE, including each one a rotation opens.
    //
    // A rotated file is found on its own months later by somebody who does not
    // remember the session, so it answers "what wrote this, which machine, which
    // build, when" without reference to anything else. Every line starts with
    // '#', which is what a log viewer shows verbatim and what this program's own
    // reader skips.
    //
    // Runs ON THE WRITER THREAD and touches nothing but Win32.
    std::wstring FilePreamble() {
        std::wstring h;
        h += L"# log\tGeneral — what this instance did\r\n";
        h += L"# machine\t" + Flatten(ComputerName()) + L"\r\n";
        h += L"# app\t"     + std::wstring(Constants::APP_NAME) + L" " +
                              std::wstring(Constants::APP_VERSION) + L"\r\n";
        h += L"# started\t" + StampNow() + L"\r\n";
        h += L"# rotation\t" + std::to_wstring(Constants::Logging::MAX_ROWS) +
                               L" rows per file\r\n";
        h += L"#\r\n";
        h += L"# format\ttime [thread] LEVEL message\r\n";
        h += L"# message\tcomponent | text\r\n";
        h += L"#\r\n";
        return h;
    }

} // namespace

std::wstring LogDirectory() {
    // "<exe folder>\logs\general". RotatingLogFile creates the whole chain, so
    // neither level has to exist first.
    return Persistence::Ini::PathBesideExe(Constants::Logging::DIR_NAME) +
           L"\\" + Constants::Logging::SUBDIR_GENERAL;
}

void SetEnabled(bool on) {
    if (on == g_on.load(std::memory_order_relaxed)) return;

    if (!on) {
        // FLAG DOWN FIRST, so nothing new is queued while the drain runs; Stop
        // then writes out what is already waiting before closing.
        g_on.store(false, std::memory_order_relaxed);
        g_file.Stop();
        return;
    }

    Persistence::RotatingLogFile::Config cfg;
    cfg.dir      = LogDirectory();
    cfg.baseName = std::wstring(Constants::APP_NAME) + L"_" +
                   Constants::Logging::KIND_GENERAL;
    cfg.ext      = Constants::Logging::EXT;
    cfg.maxRows  = Constants::Logging::MAX_ROWS;
    cfg.header   = &FilePreamble;

    g_file.Start(cfg);
    // LAST, so the writer is running before any producer can queue to it.
    g_on.store(true, std::memory_order_relaxed);
}

bool IsEnabled() { return g_on.load(std::memory_order_relaxed); }

std::wstring CurrentFilePath() { return g_file.CurrentPath(); }

void Shutdown() {
    g_on.store(false, std::memory_order_relaxed);
    g_file.Stop();
}

void Write(Level level, const wchar_t *component, const std::wstring &message) {
    if (!g_on.load(std::memory_order_relaxed)) return;

    // The component leads the message so a viewer filtering on text catches it
    // at a fixed position, and so a person scanning the raw file reads a column
    // rather than hunting for the subject in prose.
    std::wstring m = component ? component : L"?";
    m += L" | ";
    m += Flatten(message);

    g_file.Write(Persistence::BuildLogLine(StampNow(), GetCurrentThreadId(),
                                           level, m));
}

void Info (const wchar_t *component, const std::wstring &message) { Write(Level::Info,  component, message); }
void Warn (const wchar_t *component, const std::wstring &message) { Write(Level::Warn,  component, message); }
void Error(const wchar_t *component, const std::wstring &message) { Write(Level::Error, component, message); }

} // namespace AppLog
