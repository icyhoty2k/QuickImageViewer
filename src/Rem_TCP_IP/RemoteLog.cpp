// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#include "RemoteLog.h"

// windows.h FIRST. Constants.h uses COLORREF, BYTE, DWORD, UINT and WM_USER at
// namespace scope without including it itself — every other translation unit
// reaches it through a header that already pulled windows.h in.
#include <windows.h>

#include "Platform/Constants.h"   // REMOTE_LOG_DEFAULT — the one place off is decided
#include "Persistence/IniFile.h"        // PathBesideExe — the logs\ folder
#include "Persistence/RotatingLogFile.h" // the writer thread, shared with the app log

#include <algorithm>
#include <atomic>
#include <cstdio>   // swprintf_s
#include <cstdlib>  // _wcstoi64
#include <deque>
#include <mutex>
#include <string>

namespace Remote::Log {

namespace {

    std::mutex        g_mutex;
    std::deque<Entry> g_entries;
    long long         g_nextSeq = 1;
    std::wstring      g_selfName;   // guarded by g_mutex — see SetSelfName
    // What this instance is listening ON, for the saved file's preamble. A log
    // that names only the peer leaves "is that address me or them" unanswerable
    // — which is the one question a same-machine connection makes urgent, since
    // then both ends really do share an address.
    std::wstring      g_selfEndpoint;

    // OUTSIDE the mutex on purpose: the whole value of the switch is that a
    // viewer with logging off never touches the lock. Relaxed ordering, because
    // nothing is published through this flag — it gates a diagnostic, and an
    // exchange landing on either side of the flip is equally true.
    std::atomic<bool> g_enabled{Constants::RemoteTcpIp::REMOTE_LOG_DEFAULT};

    // Also outside the mutex: Add must be able to decide whether to post without
    // serialising against a Snapshot the panel is taking on the UI thread.
    std::atomic<HWND> g_notifyWnd{nullptr};
    std::atomic<bool> g_notifyPending{false};

    // The file's own version marker. ONE format is supported — a file whose
    // version does not match is refused rather than guessed at, because a log
    // read back with the sender and receiver swapped is worse than no log.
    //
    // No older version is accepted. These files are a diagnostic written and
    // read within one build; carrying readers for formats nobody has would be
    // dead code that can only rot.
    //
    // Everything in a row is written to be READ. The one raw value is deltaUs,
    // which is a measurement rather than a restatement of something already on
    // the line — the timestamp, and the gap beside it, are the loader's source
    // for the rest.
    constexpr const wchar_t *FILE_MAGIC = L"#qIV-remote-log";
    constexpr int            FILE_VERSION = 3;

    // Pipe-separated fields in a data row, counting the "<time> [<thread>]
    // <LEVEL> <dir>" prefix as the first:
    //   prefix | from | to | line | result | elapsed | #seq
    constexpr size_t FILE_COLUMNS = 7;

    // Date AND time. FormatTime is the panel's form and omits the date, which is
    // right for a live column and wrong for a file that can span midnight.
    std::wstring FormatStamp(long long whenFt) {
        if (whenFt <= 0) return L"—";
        FILETIME utc;
        utc.dwLowDateTime  = static_cast<DWORD>(whenFt & 0xFFFFFFFF);
        utc.dwHighDateTime = static_cast<DWORD>((whenFt >> 32) & 0xFFFFFFFF);
        FILETIME local{};
        if (!FileTimeToLocalFileTime(&utc, &local)) return L"—";
        SYSTEMTIME st{};
        if (!FileTimeToSystemTime(&local, &st)) return L"—";
        wchar_t b[40];
        swprintf_s(b, L"%04u-%02u-%02u %02u:%02u:%02u.%03u",
                   st.wYear, st.wMonth, st.wDay,
                   st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        return b;
    }

    // The inverse of FormatStamp: "2026-08-06 04:52:38.825" back to the FILETIME
    // it was rendered from. Local time in, UTC out, mirroring FormatStamp
    // exactly — the two are only ever correct as a pair.
    //
    // 0 for anything that is not a stamp, which is what "—" becomes. That is the
    // same value an unset time already had, so a row that never carried one
    // reloads as one that still does not.
    long long ParseStamp(const std::wstring &s) {
        SYSTEMTIME st{};
        unsigned   ms = 0;
        if (swscanf_s(s.c_str(), L"%hu-%hu-%hu %hu:%hu:%hu.%u",
                      &st.wYear, &st.wMonth, &st.wDay,
                      &st.wHour, &st.wMinute, &st.wSecond, &ms) != 7)
            return 0;
        st.wMilliseconds = static_cast<WORD>(ms);

        FILETIME local{};
        if (!SystemTimeToFileTime(&st, &local)) return 0;
        FILETIME utc{};
        if (!LocalFileTimeToFileTime(&local, &utc)) return 0;
        return (static_cast<long long>(utc.dwHighDateTime) << 32) | utc.dwLowDateTime;
    }

    long long NowFt() {
        FILETIME ft{};
        GetSystemTimeAsFileTime(&ft);
        return (static_cast<long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    }

    // Tabs and newlines are the record separators, so a field carrying one would
    // split into a row that never happened. Replaced rather than escaped: this
    // is a log to read, and an escape sequence in a response column is harder to
    // read than the space it stands in for.
    // '|' goes too, and for the same reason the others do: it is the field
    // separator inside the message column, so a wire line carrying one would
    // split into a row that never happened. Replaced with '/' rather than
    // escaped — this is a log to read, and an escape sequence is harder to read
    // than the character it stands in for.
    std::wstring Flatten(const std::wstring &s) {
        std::wstring out = s;
        for (wchar_t &c : out)
            if (c == L'\t' || c == L'\r' || c == L'\n') c = L' ';
            else if (c == L'|') c = L'/';
        return out;
    }

    // --- UTF-8, without pulling in a codec ------------------------------------
    std::string ToUtf8(const std::wstring &w) {
        if (w.empty()) return {};
        const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                          nullptr, 0, nullptr, nullptr);
        if (n <= 0) return {};
        std::string out(static_cast<size_t>(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                            out.data(), n, nullptr, nullptr);
        return out;
    }

    std::wstring FromUtf8(const char *p, size_t len) {
        if (!p || len == 0) return {};
        const int n = MultiByteToWideChar(CP_UTF8, 0, p, static_cast<int>(len), nullptr, 0);
        if (n <= 0) return {};
        std::wstring out(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, p, static_cast<int>(len), out.data(), n);
        return out;
    }

    // Splits on tabs WITHOUT collapsing empties: an empty response is a real
    // value (a command that answered with a bare OK), and dropping it would
    // shift every column after it. Used for the preamble, whose lines are still
    // "# key<TAB>value".
    std::vector<std::wstring> SplitTabs(const std::wstring &line) {
        std::vector<std::wstring> out;
        size_t start = 0;
        for (;;) {
            const size_t tab = line.find(L'\t', start);
            if (tab == std::wstring::npos) { out.push_back(line.substr(start)); break; }
            out.push_back(line.substr(start, tab - start));
            start = tab + 1;
        }
        return out;
    }

    // The data rows' separator. Same rule about empty fields, and safe because
    // Flatten has already removed '|' from everything a peer can influence.
    std::vector<std::wstring> SplitPipes(const std::wstring &line) {
        std::vector<std::wstring> out;
        size_t start = 0;
        for (;;) {
            const size_t bar = line.find(L" | ", start);
            if (bar == std::wstring::npos) { out.push_back(line.substr(start)); break; }
            out.push_back(line.substr(start, bar - start));
            start = bar + 3;
        }
        return out;
    }

    // "1.8 ms" / "428 µs" / "2.30 s" / "14h 02m" back to microseconds, and -1
    // for the "—" FormatDelta writes when nothing was measured.
    //
    // LOSSY BY DESIGN, and that is the trade this format makes: the file carries
    // what a person reads, so a reloaded elapsed is the rounded value that was
    // on screen rather than the raw measurement. The panel renders it through
    // FormatDelta again, so what you see after a reload is identical to what you
    // saw before one.
    long long ParseDelta(const std::wstring &s) {
        double v = 0;
        if (swscanf_s(s.c_str(), L"%lf", &v) != 1) return -1;

        if (s.find(L"µs") != std::wstring::npos) return static_cast<long long>(v);
        if (s.find(L"ms") != std::wstring::npos) return static_cast<long long>(v * 1000.0);
        if (s.find(L'h')  != std::wstring::npos) {
            double m = 0;
            swscanf_s(s.c_str(), L"%lfh %lfm", &v, &m);
            return static_cast<long long>((v * 3600.0 + m * 60.0) * 1000000.0);
        }
        if (s.find(L'm')  != std::wstring::npos) {
            double sec = 0;
            swscanf_s(s.c_str(), L"%lfm %lfs", &v, &sec);
            return static_cast<long long>((v * 60.0 + sec) * 1000000.0);
        }
        if (s.find(L's')  != std::wstring::npos) return static_cast<long long>(v * 1000000.0);
        return -1;
    }

    long long ToLL(const std::wstring &s) {
        return _wcstoi64(s.c_str(), nullptr, 10);
    }

    // =========================================================================
    // The file sink
    // =========================================================================
    // Separate from g_enabled, and outside every mutex, for the same reason: a
    // producer decides whether to queue a row on one relaxed load.
    std::atomic<bool> g_fileOn{false};

    // The writer. One thread, owned entirely by this object — see
    // Persistence/RotatingLogFile.h for why the disk is never touched on a
    // caller's thread.
    Persistence::RotatingLogFile g_file;

    // WARN for a refusal, ERROR for a connection that ended badly, INFO for
    // everything else.
    //
    // A level column is the first thing a log viewer colours and the first thing
    // it filters on, so writing INFO on every row would throw away the one
    // feature that makes a wall of rows navigable. Derived rather than passed
    // in: every producer already says what happened in the text, and asking
    // seventeen record points each to classify themselves is seventeen chances
    // to disagree.
    Persistence::LogLevel LevelFor(const Entry &e) {
        // BOTH FIELDS, and that is the point of this function.
        //
        // A reply carries its text in `response` ("ERR 403 …"), but a CONNECTION
        // row carries it in `command` — "(refused — blacklisted)" with a
        // response of merely "(connection)". Reading only the response, as this
        // first did, classified every refusal as INFO: a bot hammering the port
        // all night produced a log in which filtering for WARN showed nothing,
        // which is precisely the case the level column exists to surface.
        auto has = [&](const wchar_t *needle) {
            return e.command.find(needle) != std::wstring::npos ||
                   e.response.find(needle) != std::wstring::npos;
        };

        // ERROR — somebody was BLOCKED, or blocked themselves. These are the
        // rows worth waking up for: a blacklist hit means an address is already
        // known bad, and a ban is this machine deciding so.
        if (has(L"blacklisted") || has(L"blocked") || has(L"banned"))
            return Persistence::LogLevel::Error;

        // WARN — refused, dropped, or ended in a way nobody asked for. A single
        // one is ordinary; a column of them is an attack or a broken screen, and
        // either way it is the pattern that carries the meaning.
        if (e.response.rfind(L"ERR", 0) == 0 ||
            has(L"refused") || has(L"dropped") ||
            has(L"authentication failed") || has(L"ABRUPTLY"))
            return Persistence::LogLevel::Warn;

        return Persistence::LogLevel::Info;
    }

    // ONE ROW, in the layout every standard log viewer already understands:
    //
    //   <time> [<thread>] <LEVEL> <message>
    //
    // That is the log4j/log4net shape, which is what LogViewPlus, lnav and the
    // rest parse out of the box — four columns, sortable and filterable, with no
    // per-file configuration. Everything specific to wire traffic lives inside
    // the message, because a viewer that does not know this program still has to
    // be able to show the row.
    //
    // The message is PIPE-DELIMITED so it can be parsed straight back for Ctrl+O
    // — see Flatten, which removes '|' from every peer-controlled field for
    // exactly the reason it already removed tabs.
    //
    // Shared by SaveTo and by the file sink so the two cannot drift: a rotated
    // file and a hand-saved one are the same format, and Ctrl+O opens either.
    //
    // NO TIME-SINCE-PREVIOUS FIELD. It was here briefly and came out again: a
    // log viewer derives it from two adjacent timestamps far better than this
    // can — it recomputes after a sort or a filter, which a stored value cannot.
    // Writing it would be shipping a cached subtraction that goes wrong the
    // moment the rows are reordered.
    std::wstring FormatRow(const Entry &e) {
        // The message: everything specific to wire traffic, which the shared
        // layout knows nothing about and does not need to.
        std::wstring m;
        m += (e.dir == Direction::Out) ? L"OUT" : L"IN";
        m += L" | ";  m += Flatten(e.sender);
        m += L" | ";  m += Flatten(e.receiver);
        m += L" | ";  m += Flatten(e.command);
        m += L" | ";  m += Flatten(e.response);
        m += L" | ";  m += FormatDelta(e.deltaUs);
        m += L" | #"; m += std::to_wstring(e.seq);

        return Persistence::BuildLogLine(FormatStamp(e.whenFt), e.threadId,
                                         LevelFor(e), m);
    }

    std::wstring ComputerName() {
        wchar_t  name[MAX_COMPUTERNAME_LENGTH + 1] = {};
        DWORD    n = MAX_COMPUTERNAME_LENGTH + 1;
        if (!GetComputerNameW(name, &n)) return L"(unknown)";
        return name;
    }

    // THE HEADER ON EVERY FILE, including each one a rotation opens.
    //
    // A rotated file is found months later, on its own, by someone who no longer
    // remembers the session — so it has to answer "what wrote this, which
    // machine, which build, and when" without reference to anything else. It
    // begins with the magic and version LoadFrom expects, and every other line
    // starts with '#', which that loader skips: the preamble can grow without
    // touching the parser, and the file still opens in Ctrl+F12.
    //
    // Runs ON THE WRITER THREAD. SelfName and SelfEndpoint take the store's
    // mutex and nothing else, and the writer holds no lock when this is called.
    std::wstring FilePreamble() {
        namespace RT = Constants::RemoteTcpIp;

        // WRAPPED BY HAND at roughly 78 columns, and that is not fussiness.
        // These files are opened in Notepad and in the Ctrl+F12 panel, neither
        // of which reflows: a 140-character note becomes one wrapped line that
        // looks like the writer emitted a broken record, which is exactly the
        // doubt a diagnostic must not create about itself. Every line below is
        // its own '#' row, and the loader skips all of them, so splitting a
        // sentence across two costs nothing.
        std::wstring h = std::wstring(FILE_MAGIC) + L'\t' +
                         std::to_wstring(FILE_VERSION) + L"\r\n";
        h += L"# log\tTCP/IP wire traffic\r\n";
        h += L"# instance\t"  + Flatten(SelfName()) + L"\r\n";
        h += L"# listening\t" + Flatten(SelfEndpoint()) + L"\r\n";
        h += L"# machine\t"   + Flatten(ComputerName()) + L"\r\n";
        h += L"# app\t"       + std::wstring(Constants::APP_NAME) + L" " +
                                std::wstring(Constants::APP_VERSION) + L"\r\n";
        h += L"# protocol\tv" + std::to_wstring(RT::PROTOCOL_VERSION) + L"\r\n";
        h += L"# started\t"   + FormatStamp(NowFt()) + L"\r\n";
        h += L"# rotation\t"  + std::to_wstring(Constants::Logging::MAX_ROWS) +
                                L" rows per file\r\n";
        h += L"#\r\n";
        h += L"# format\ttime [thread] LEVEL message\r\n";
        h += L"# message\tdir | from | to | line | result | elapsed | #seq\r\n";
        h += L"#\r\n";
        h += L"# note\telapsed is what THAT exchange cost. Time since the\r\n";
        h += L"# note\tprevious row is not written — a log viewer derives it\r\n";
        h += L"# note\tfrom the timestamps, and recomputes it after a sort.\r\n";
        h += L"#\r\n";
        h += L"# note\tfrom/to name a PEER as address:port, and this instance\r\n";
        h += L"# note\tby its name. A row with this instance's own name on\r\n";
        h += L"# note\tboth sides never happens.\r\n";
        h += L"#\r\n";
        h += L"# note\thandshake lines are redacted at capture. No password\r\n";
        h += L"# note\tor HMAC ever reaches this file.\r\n";
        h += L"#\r\n";
        return h;
    }

    std::wstring LastErrorSentence(const wchar_t *what, const std::wstring &path) {
        const DWORD e = GetLastError();
        wchar_t buf[64];
        swprintf_s(buf, L" (error %lu)", e);
        return std::wstring(what) + L":\r\n\r\n    " + path + buf;
    }

} // namespace

// =============================================================================
// Store
// =============================================================================
void Add(Direction dir,
         const std::wstring &sender,
         const std::wstring &command,
         const std::wstring &receiver,
         const std::wstring &response,
         long long deltaUs) {
    // TWO DESTINATIONS, decided independently and read once each so the rest of
    // this function cannot see them change underneath it.
    //
    // The second half of the switch. Callers check IsCapturing() first so they
    // build nothing; this catches the ones that did not, and the race between
    // the two checks is harmless — one extra entry either side of a flip.
    const bool toStore = g_enabled.load(std::memory_order_relaxed);
    const bool toFile  = g_fileOn.load(std::memory_order_relaxed);
    if (!toStore && !toFile) return;

    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);

    // TRUNCATED HERE, at the one point every producer passes through.
    //
    // An image stream puts ~128 KB of base64 on a single line, several lines per
    // picture. That is not a record of anything a reader can use, and kept whole
    // it would bloat both the in-memory store and the file it saves to. A
    // diagnostic log wants to know a chunk went past and how big it was.
    //
    // Applied to the two peer-controlled fields only; sender and receiver are this
    // program's own labels.
    // SANITISED as well as truncated, and for a reason that is not cosmetic.
    //
    // What arrives on this socket is not always text. A client that speaks TLS
    // to a plaintext listener sends a ClientHello, and those bytes land here as
    // a "command" full of control characters — including TAB and newline, which
    // are the FIELD and RECORD separators of the saved .tsv. One such line
    // silently corrupts the file's structure, so the log of the very failure you
    // are trying to diagnose is the log that cannot be read.
    //
    // Replaced rather than escaped: this is a diagnostic, and knowing a byte was
    // unprintable is all a reader can use. The count still comes from the
    // ORIGINAL length, so a clipped line reports what really went past.
    auto clip = [](const std::wstring &s) {
        namespace RT = Constants::RemoteTcpIp;

        std::wstring out;
        out.reserve(std::min(s.size(), RT::LOG_LINE_MAX));
        for (size_t i = 0; i < s.size() && i < RT::LOG_LINE_MAX; ++i) {
            const wchar_t c = s[i];
            // Everything below space, plus DEL — tab and newline among them.
            out += (c < 0x20 || c == 0x7F) ? L'·' : c;
        }

        if (s.size() > RT::LOG_LINE_MAX)
            out += L"… (" + std::to_wstring(s.size()) + L" chars)";
        return out;
    };

    Entry e;
    // Taken HERE, on the thread that actually recorded the exchange — the writer
    // thread that puts it in the file is a different one, and asking it would
    // stamp every row with the same useless number.
    e.threadId = GetCurrentThreadId();
    e.whenFt   = (static_cast<long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    e.deltaUs  = deltaUs;
    e.dir      = dir;
    e.sender   = sender;
    e.command  = clip(command);
    e.receiver = receiver;
    e.response = clip(response);

    // The row for the file, built while the entry is still in hand. Formatted
    // HERE rather than on the writer thread so the queue holds finished text: a
    // writer that had to reach back into the store for fields would be reading
    // a deque the producers are still trimming.
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        // NUMBERED WHETHER OR NOT IT IS KEPT, and the counter is shared by both
        // destinations. A file written while the panel is not recording still
        // gets consecutive sequence numbers, and switching the panel on
        // mid-session does not restart them — one number never means two
        // different exchanges, wherever they were written.
        e.seq = g_nextSeq++;

        // FORMATTED AND QUEUED WITHOUT LETTING GO OF THIS LOCK.
        //
        // Formatting here and queuing after releasing would let two producer
        // threads take their numbers in one order and reach the queue in the
        // other, putting seq 5 in the file above seq 4. A log whose rows are not
        // in the order they happened is a log that has to be sorted before it
        // can be believed.
        //
        // Safe: Write does nothing but push onto a deque and signal, and the
        // writer thread never holds ITS mutex while taking this one — it swaps
        // the queue out, releases, and only then formats a header. So the order
        // is always store-lock then writer-lock, and never the reverse.
        if (toFile) g_file.Write(FormatRow(e));

        if (toStore) {
            g_entries.push_back(std::move(e));
            // Trimmed from the FRONT, so what is kept is always the most recent
            // window.
            while (g_entries.size() > CAPACITY) g_entries.pop_front();
        }
    }

    // OUTSIDE the lock. PostMessage does not block, but holding a mutex across
    // any call into USER32 from a socket thread is how a deadlock gets written.
    //
    // Only when something was STORED: the panel paints the deque, so a row that
    // went to the file alone gives it nothing new to draw, and posting anyway
    // would wake the UI thread once per exchange to rebuild an unchanged list.
    if (toStore) NotifyChanged();
}

void NotifyChanged() {
    // Nothing to do when no panel is open, which is the usual case even with
    // recording on.
    HWND hwnd = g_notifyWnd.load(std::memory_order_acquire);
    if (!hwnd) return;

    // The coalescing gate: exchange returns the OLD value, so exactly one
    // producer in a burst gets false and posts.
    if (g_notifyPending.exchange(true, std::memory_order_acq_rel)) return;

    if (!PostMessageW(hwnd, Constants::WM_QIV_REMOTE_LOG_ADDED, 0, 0)) {
        // The window went away between the load and the post. Release the gate
        // or every later entry would see a pending notification that will never
        // arrive, and the panel would stop updating for the rest of the session.
        g_notifyPending.store(false, std::memory_order_release);
    }
}

std::vector<Entry> Snapshot() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return std::vector<Entry>(g_entries.begin(), g_entries.end());
}

size_t Count() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_entries.size();
}

void SetEnabled(bool on) { g_enabled.store(on, std::memory_order_relaxed); }
bool IsEnabled()         { return g_enabled.load(std::memory_order_relaxed); }

// Two relaxed loads on the keystroke path, which is the same cost the single
// load used to be for any purpose that matters. Ordered cheap-first only by
// habit; both are plain atomics and neither can block.
bool IsCapturing() {
    return g_enabled.load(std::memory_order_relaxed) ||
           g_fileOn.load(std::memory_order_relaxed);
}

// =============================================================================
// The file sink
// =============================================================================
std::wstring LogDirectory() {
    // "<exe folder>\logs\network". Resolved every call rather than cached: it is
    // asked for by a menu handler and a status line, never on the recording
    // path, and a static here would be one more thing initialised before main
    // for no gain.
    return Persistence::Ini::PathBesideExe(Constants::Logging::DIR_NAME) +
           L"\\" + Constants::Logging::SUBDIR_NETWORK;
}

void SetFileLogging(bool on) {
    if (on == g_fileOn.load(std::memory_order_relaxed)) return;

    if (!on) {
        // FLAG DOWN FIRST, so nothing new is queued while the drain runs, and
        // Stop then writes out what is already waiting before closing.
        g_fileOn.store(false, std::memory_order_relaxed);
        g_file.Stop();
        return;
    }

    Persistence::RotatingLogFile::Config cfg;
    cfg.dir      = LogDirectory();
    cfg.baseName = std::wstring(Constants::APP_NAME) + L"_" +
                   Constants::Logging::KIND_TCP_IP;
    cfg.ext      = Constants::Logging::EXT;
    cfg.maxRows  = Constants::Logging::MAX_ROWS;
    cfg.header   = &FilePreamble;

    g_file.Start(cfg);
    // LAST, so the writer is running before the first producer can queue to it.
    g_fileOn.store(true, std::memory_order_relaxed);
}

bool FileLoggingIsOn() { return g_fileOn.load(std::memory_order_relaxed); }

std::wstring CurrentFilePath() { return g_file.CurrentPath(); }

void ShutdownFileLogging() {
    g_fileOn.store(false, std::memory_order_relaxed);
    g_file.Stop();
}

void SetNotifyWindow(HWND hwnd) {
    g_notifyWnd.store(hwnd, std::memory_order_release);
    // Unregistering leaves the gate closed otherwise, and the next panel to
    // open would never be told about anything.
    if (!hwnd) g_notifyPending.store(false, std::memory_order_release);
}

void ClearNotifyPending() {
    g_notifyPending.store(false, std::memory_order_release);
}

void SetSelfName(const std::wstring &name) {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_selfName = name;
}

std::wstring SelfName() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_selfName.empty() ? std::wstring(L"(this)") : g_selfName;
}

// NAME THEN ADDRESS, the same shape a peer gets, so both sides of a row read
// alike and neither has to be guessed at. The address is dropped when there is
// none — a driving instance with no listener of its own has no endpoint to give,
// and "(not listening)" on every row it writes would be noise, not information.
std::wstring SelfLabel() {
    std::lock_guard<std::mutex> lk(g_mutex);
    const std::wstring n = g_selfName.empty() ? std::wstring(L"(this)") : g_selfName;
    return g_selfEndpoint.empty() ? n : n + L" " + g_selfEndpoint;
}

// Pushed in rather than read out: this module is a leaf and knows nothing about
// the server, which is what keeps a diagnostic from becoming a dependency.
void SetSelfEndpoint(const std::wstring &endpoint) {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_selfEndpoint = endpoint;
}

std::wstring SelfEndpoint() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_selfEndpoint.empty() ? std::wstring(L"(not listening)") : g_selfEndpoint;
}

void Clear() {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_entries.clear();
}

// =============================================================================
// Formatting
// =============================================================================
std::wstring FormatTime(long long whenFt) {
    if (whenFt <= 0) return L"—";

    FILETIME utc;
    utc.dwLowDateTime  = static_cast<DWORD>(whenFt & 0xFFFFFFFF);
    utc.dwHighDateTime = static_cast<DWORD>((whenFt >> 32) & 0xFFFFFFFF);

    FILETIME local{};
    if (!FileTimeToLocalFileTime(&utc, &local)) return L"—";

    SYSTEMTIME st{};
    if (!FileTimeToSystemTime(&local, &st)) return L"—";

    wchar_t b[32];
    swprintf_s(b, L"%02u:%02u:%02u.%03u", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return b;
}

std::wstring FormatDelta(long long us) {
    if (us < 0) return L"—";
    wchar_t b[32];
    // Three bands, because the interesting range spans five orders of
    // magnitude: loopback is sub-millisecond, a LAN screen is single-digit
    // milliseconds, and a target that is swapping is seconds.
    if (us < 1000)          swprintf_s(b, L"%lld µs", us);
    else if (us < 1000000)  swprintf_s(b, L"%.1f ms", static_cast<double>(us) / 1000.0);
    else if (us < 60000000) swprintf_s(b, L"%.2f s",  static_cast<double>(us) / 1000000.0);
    else {
        // A FOURTH BAND, for the connection rows. A round trip never reaches a
        // minute — REPLY_TIMEOUT_MS bounds it at five seconds — but a SESSION
        // does, and a wall left running overnight rendered as "50400.00 s",
        // which is a number nobody reads as fourteen hours.
        const long long total = us / 1000000;
        const long long h = total / 3600, m = (total % 3600) / 60, sec = total % 60;
        if (h > 0) swprintf_s(b, L"%lldh %02lldm", h, m);
        else       swprintf_s(b, L"%lldm %02llds", m, sec);
    }
    return b;
}

// =============================================================================
// Disk
// =============================================================================
bool SaveTo(const std::wstring &path, std::wstring &errorOut) {
    const std::vector<Entry> rows = Snapshot();

    // Built whole, then written in one go: a partially written log that was
    // interrupted mid-flush is a file that loads as garbage, and the whole
    // buffer is a few megabytes at CAPACITY.
    // A PREAMBLE, because the first question asked of any log is "whose is
    // this". Every line starts with '#' and the loader skips them all, so this
    // block can grow without touching the parser.
    std::wstring text = std::wstring(FILE_MAGIC) + L'\t' +
                        std::to_wstring(FILE_VERSION) + L"\r\n";
    text += L"# instance\t"  + Flatten(SelfName()) + L"\r\n";
    text += L"# listening\t" + Flatten(SelfEndpoint()) + L"\r\n";
    text += L"# saved\t"     + FormatStamp(NowFt()) + L"\r\n";
    text += L"# rows\t"      + std::to_wstring(rows.size()) + L"\r\n";
    text += L"# format\ttime [thread] LEVEL message\r\n";
    text += L"# message\tdir | from | to | line | result | elapsed | #seq\r\n";
    text += L"#\r\n";
    text += L"# note\tfrom/to name a PEER as address:port, and this instance\r\n";
    text += L"# note\tby its name. A row with this instance's own name on\r\n";
    text += L"# note\tboth sides never happens.\r\n";
    text += L"#\r\n";

    // THE SAME FormatRow THE FILE SINK USES. One definition, so a file this
    // saves and a file the rotation writes are the same format — which is what
    // lets Ctrl+O open either of them.
    //
    for (const Entry &e : rows) {
        text += FormatRow(e);
        text += L"\r\n";
    }

    const std::string utf8 = ToUtf8(text);

    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        errorOut = LastErrorSentence(L"Could not create the log file", path);
        return false;
    }

    // BOM, so Notepad and Excel both read it as UTF-8 rather than guessing at
    // the code page — a log full of machine names is exactly where that bites.
    static const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
    DWORD written = 0;
    bool ok = WriteFile(h, bom, 3, &written, nullptr) != 0;
    if (ok && !utf8.empty())
        ok = WriteFile(h, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr) != 0 &&
             written == static_cast<DWORD>(utf8.size());

    if (!ok) errorOut = LastErrorSentence(L"Could not write the log file", path);
    CloseHandle(h);
    return ok;
}

bool LoadFrom(const std::wstring &path, std::wstring &errorOut) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        errorOut = LastErrorSentence(L"Could not open that file", path);
        return false;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0 || size.QuadPart > (64LL << 20)) {
        CloseHandle(h);
        errorOut = L"That file is empty, or too large to be a qIV remote log "
                   L"(the limit is 64 MB).";
        return false;
    }

    std::string raw(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    const bool ok = ReadFile(h, raw.data(), static_cast<DWORD>(raw.size()), &read, nullptr) != 0;
    CloseHandle(h);
    if (!ok) {
        errorOut = LastErrorSentence(L"Could not read that file", path);
        return false;
    }
    raw.resize(read);

    size_t off = 0;
    if (raw.size() >= 3 && static_cast<unsigned char>(raw[0]) == 0xEF &&
        static_cast<unsigned char>(raw[1]) == 0xBB &&
        static_cast<unsigned char>(raw[2]) == 0xBF)
        off = 3;

    const std::wstring text = FromUtf8(raw.data() + off, raw.size() - off);

    std::vector<Entry> loaded;
    bool headerSeen = false;

    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find(L'\n', start);
        if (nl == std::wstring::npos) nl = text.size();
        std::wstring line = text.substr(start, nl - start);
        start = nl + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (line.empty()) { if (start > text.size()) break; continue; }

        if (!headerSeen) {
            // Magic and a number, nothing more — which is what lets the preamble
            // below it grow without ever touching this check.
            const std::vector<std::wstring> header = SplitTabs(line);
            if (header.size() < 2 || header[0] != FILE_MAGIC) {
                errorOut = L"That is not a qIV remote log.\r\n\r\nThe first line "
                           L"must be the header qIV writes when it saves one.";
                return false;
            }
            if (ToLL(header[1]) != FILE_VERSION) {
                errorOut = L"That log was written by a different version of qIV "
                           L"and this build cannot read its columns.";
                return false;
            }
            headerSeen = true;
            continue;
        }

        // The whole preamble, and any comment somebody added by hand. A data row
        // always begins with its sequence number, so '#' can never start one.
        if (line[0] == L'#') continue;

        const std::vector<std::wstring> f = SplitPipes(line);
        if (f.size() < FILE_COLUMNS) continue; // a truncated tail line, not a failure

        // f[0] is the shared prefix plus the first word of the message:
        // "<time> [<thread>] <LEVEL> <IN|OUT>". The level is NOT read back — it
        // is derived from the response by LevelFor, and a second stored copy is
        // just something that can come to disagree with the first.
        Entry e;
        e.whenFt = ParseStamp(f[0]);

        const size_t open = f[0].find(L'[');
        if (open != std::wstring::npos)
            e.threadId = static_cast<unsigned long>(ToLL(f[0].substr(open + 1)));

        // The direction is the last whitespace-separated token of that field.
        // Reliable because both the level and IN/OUT are fixed words this
        // program writes — nothing peer-controlled reaches here.
        const size_t sp = f[0].find_last_of(L' ');
        e.dir = (sp != std::wstring::npos && f[0].substr(sp + 1) == L"IN")
                    ? Direction::In
                    : Direction::Out;

        e.sender   = f[1];
        e.receiver = f[2];
        e.command  = f[3];
        e.response = f[4];
        e.deltaUs  = ParseDelta(f[5]);
        e.seq      = ToLL(f[6].substr(f[6].find(L'#') + 1));
        loaded.push_back(std::move(e));
    }

    if (!headerSeen) {
        errorOut = L"That file has no qIV remote-log header.";
        return false;
    }

    // REPLACE, not merge — see the header. The sequence counter is pushed past
    // whatever was loaded, so a live exchange arriving afterwards cannot be
    // given a number the file already used.
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_entries.assign(loaded.begin(), loaded.end());
        while (g_entries.size() > CAPACITY) g_entries.pop_front();
        for (const Entry &e : g_entries)
            if (e.seq >= g_nextSeq) g_nextSeq = e.seq + 1;
    }
    return true;
}

} // namespace Remote::Log
