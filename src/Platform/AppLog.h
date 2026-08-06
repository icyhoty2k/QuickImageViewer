// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#pragma once
#include <string>

#include "Persistence/RotatingLogFile.h"   // LogLevel + the shared line layout

// =============================================================================
// AppLog — the General log: this program talking about itself.
//
// The other log (Rem_TCP_IP/RemoteLog.h) is a transcript between machines. This
// one records what THIS instance did: started, stopped, failed to decode a
// picture, lost its renderer, had its monitor taken away.
//
// WHO IT IS FOR. A dedicated instance launched with Windows, showing a
// fullscreen locked slideshow on one monitor, with nobody in front of it. There
// is no panel to look at and no operator to ask, so the file is the only way to
// find out whether that screen is doing what it was set up to do. Every event
// below was chosen against that: would it explain a screen that is blank, stuck
// or showing the wrong thing tomorrow morning?
//
// NOTHING POLLS. There is no timer, no heartbeat and no watchdog anywhere in
// here — every line is written by something that was already happening. A
// diagnostic that runs work of its own is a diagnostic that changes the
// behaviour it was added to observe, and this app has already had to hunt down
// three UI-thread blockers.
//
// Off by default and persisted — Constants::IS_GENERAL_LOG. Files land beside
// the TCP/IP log in logs\, share its rotation, and use the same
// "time [thread] LEVEL message" layout, so one viewer configuration reads both.
//
// SAFE FROM ANY THREAD and safe before the window exists: writing only formats
// a line and hands it to the writer's queue. With the log off, every call below
// returns on one relaxed atomic load.
// =============================================================================

namespace AppLog {

    using Level = Persistence::LogLevel;

    // Starts or stops the writer. Opens no file by itself — the first line does.
    void SetEnabled(bool on);
    bool IsEnabled();

    // "<exe folder>\logs", whether or not it exists yet.
    std::wstring LogDirectory();

    // The file being written, empty when nothing has been written yet.
    std::wstring CurrentFilePath();

    // Drains the queue and joins the writer. Called once, late in shutdown —
    // after the last line anybody wants recorded.
    void Shutdown();

    // ONE ENTRY POINT, plus three shorthands for the levels that get used.
    //
    // `component` is a short fixed tag — "startup", "render", "decode",
    // "slideshow" — written as the first field of the message so a viewer can
    // filter on it. Fixed strings only: it is a category, not a sentence, and a
    // category nobody can predict cannot be filtered.
    void Write(Level level, const wchar_t *component, const std::wstring &message);

    void Info (const wchar_t *component, const std::wstring &message);
    void Warn (const wchar_t *component, const std::wstring &message);
    void Error(const wchar_t *component, const std::wstring &message);

    // --- The components, so a typo cannot invent a new category --------------
    constexpr const wchar_t *COMP_STARTUP   = L"startup";
    constexpr const wchar_t *COMP_SHUTDOWN  = L"shutdown";
    constexpr const wchar_t *COMP_CRASH     = L"crash";
    constexpr const wchar_t *COMP_SLIDESHOW = L"slideshow";
    constexpr const wchar_t *COMP_RENDER    = L"render";
    constexpr const wchar_t *COMP_DECODE    = L"decode";
    constexpr const wchar_t *COMP_SETTINGS  = L"settings";
    constexpr const wchar_t *COMP_DISPLAY   = L"display";

} // namespace AppLog
