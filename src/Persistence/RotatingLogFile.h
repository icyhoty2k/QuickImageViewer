// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#pragma once
#include <string>

// =============================================================================
// RotatingLogFile — text rows to timestamped files, written off the caller's
// thread.
//
// TWO CONSUMERS, which is why this is a component and not private to either of
// them: the TCP/IP wire log (Rem_TCP_IP/RemoteLog.h) and the application log.
// They share the folder, the rotation rule, the naming and the writer thread,
// and they share nothing else — each supplies its own header and formats its
// own rows. Writing this twice would mean two rotation bugs to fix instead of
// one.
//
// WHY A THREAD. The producers here are not tidy: the wire log is fed by socket
// threads, mirror sender threads, and the UI thread. Writing to disk on the
// caller would put a file write on the message pump, and a slow disk, a network
// share or an antivirus scan would then stop the viewer repainting. Write()
// therefore only appends to a queue and returns; one thread owns the handle and
// performs every CreateFile, WriteFile and rotation.
//
// THE QUEUE IS BOUNDED, and overflow DROPS rather than blocks. A producer that
// waited for the disk would be a diagnostic that stalls the thing it is meant to
// observe — the log would change the behaviour it was opened to explain. What is
// dropped is counted and reported in the file, because a gap you cannot see is
// worse than one you can.
//
// FILES ARE UTF-8 WITH A BOM and CRLF line endings, matching what
// RemoteLog::SaveTo writes, so a rotated wire-log file loads straight back into
// the Ctrl+F12 panel.
// =============================================================================

namespace Persistence {

// =============================================================================
// The line layout both logs share
// =============================================================================
//
//   2026-08-06 04:52:39.735 [7412] INFO  <message>
//
// This is the log4j / log4net shape, which is what LogViewPlus, lnav and every
// other general-purpose viewer parses with no per-file configuration: four
// columns — time, thread, level, message — sortable and filterable on arrival.
//
// It lives HERE rather than in either log because it is the one thing the two
// must agree on exactly. A viewer configured for one file has to work on the
// other; two builders would drift the first time somebody widened a column.
enum class LogLevel {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Fatal,
};

// "INFO", "WARN"… PADDED TO FIVE, so the message column starts at the same
// place on every line. A viewer that splits on whitespace does not care, but a
// person scanning the raw file in Notepad reads a ragged left edge as noise.
const wchar_t *LogLevelName(LogLevel level);

// Assembles the prefix and appends the message. `stamp` is passed in rather than
// taken here: the wire log timestamps an exchange at the moment it happened, not
// at the moment it reached the writer, and those are different instants.
std::wstring BuildLogLine(const std::wstring &stamp,
                          unsigned long      threadId,
                          LogLevel           level,
                          const std::wstring &message);

class RotatingLogFile {
    public:
        // Produces the preamble written at the top of EVERY file, including each
        // one a rotation opens. A plain function pointer rather than a
        // std::function: there is no state to capture, and the call site should
        // be something a reader can follow to one definition.
        //
        // Called ON THE WRITER THREAD. It must not touch UI state, and it must
        // not call back into whatever owns this object.
        using HeaderFn = std::wstring (*)();


        struct Config {
            // Full path to the folder. Created if missing, including parents.
            std::wstring dir;
            // Leading part of the file name — an instance or app name, ALREADY
            // sanitised by the caller, which knows what its own name may contain.
            std::wstring baseName;
            std::wstring ext = L".log";
            // Rows, not bytes, and not counting the preamble — so every file
            // holds the same number of records however the header grows.
            int          maxRows = 5000;
            HeaderFn     header  = nullptr;
        };

        RotatingLogFile() = default;
        ~RotatingLogFile();

        RotatingLogFile(const RotatingLogFile &)            = delete;
        RotatingLogFile &operator=(const RotatingLogFile &) = delete;

        // Starts the writer thread. Calling it while already running is a no-op,
        // so a setting flipped twice cannot open two threads on one file.
        //
        // Does NOT create a file yet: the first Write does — and when it does it
        // CONTINUES the newest file already in the folder, appending until that
        // one reaches maxRows.
        //
        // Two reasons. A logger switched on during a quiet hour should not
        // litter the folder with empty files; and switching one off and on again
        // is a single menu click, easy to do twice, which would otherwise leave
        // a trail of three-line stubs and make "5000 rows per file" mean
        // nothing. A rotation never adopts — it has just filled the file it
        // closed.
        void Start(const Config &cfg);

        // Drains what is queued, closes the file and JOINS the thread. Safe to
        // call when not running.
        //
        // Joined rather than detached on purpose: a detached writer holding a
        // handle while the process exits loses its last rows, and those are the
        // ones written immediately before whatever you are investigating.
        void Stop();

        bool IsRunning() const;

        // Queues one row. Never blocks, never touches the disk, safe from any
        // thread. A trailing newline is added by the writer — callers pass the
        // row's text and nothing else.
        void Write(const std::wstring &row);

        // The file currently open, or empty when nothing has been written yet.
        std::wstring CurrentPath() const;

    private:
        struct Impl;
        // Heap-allocated so this header pulls in no <thread>, <mutex> or
        // <deque> — it is included by UI code that has no business seeing them.
        Impl *m_impl = nullptr;
};

} // namespace Persistence
