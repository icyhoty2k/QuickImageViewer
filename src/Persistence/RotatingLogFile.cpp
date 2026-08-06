// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#include "RotatingLogFile.h"

#include <windows.h>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace Persistence {

namespace {

    // How many rows may wait for the disk before the oldest are dropped.
    //
    // Generous enough that an ordinary burst — a wall of screens all answering
    // at once — never reaches it, and small enough that a writer stuck on a
    // dead network share cannot eat memory while the viewer carries on. At
    // roughly 200 bytes a row this is a few megabytes at the very worst.
    constexpr size_t QUEUE_MAX = 20000;

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

    // "20260806_041418" — sorts as a string in the order it happened, which is
    // the whole job of a name in a folder of these.
    std::wstring StampNow() {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        wchar_t b[32];
        swprintf_s(b, L"%04u%02u%02u_%02u%02u%02u",
                   st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        return b;
    }

    // Creates the folder and any missing parent. Returns false only when the
    // path cannot be made — an existing folder is success.
    bool EnsureDirectory(const std::wstring &dir) {
        if (dir.empty()) return false;

        const DWORD attr = GetFileAttributesW(dir.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES)
            return (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;

        // One parent at a time, top down. SHCreateDirectoryEx would do this in
        // one call but drags in shell32 for a job that is six lines.
        size_t pos = dir.find_first_of(L"\\/", 3);   // past "C:\" or "\\?\"
        while (pos != std::wstring::npos) {
            const std::wstring part = dir.substr(0, pos);
            if (!part.empty()) CreateDirectoryW(part.c_str(), nullptr);
            pos = dir.find_first_of(L"\\/", pos + 1);
        }
        CreateDirectoryW(dir.c_str(), nullptr);

        const DWORD made = GetFileAttributesW(dir.c_str());
        return made != INVALID_FILE_ATTRIBUTES && (made & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }

} // namespace

// =============================================================================
// The shared line layout
// =============================================================================
const wchar_t *LogLevelName(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return L"TRACE";
        case LogLevel::Debug: return L"DEBUG";
        case LogLevel::Info:  return L"INFO ";
        case LogLevel::Warn:  return L"WARN ";
        case LogLevel::Error: return L"ERROR";
        case LogLevel::Fatal: return L"FATAL";
    }
    return L"INFO ";
}

std::wstring BuildLogLine(const std::wstring &stamp,
                          unsigned long      threadId,
                          LogLevel           level,
                          const std::wstring &message) {
    std::wstring line;
    line.reserve(stamp.size() + message.size() + 24);
    line += stamp;
    line += L" [";
    line += std::to_wstring(threadId);
    line += L"] ";
    line += LogLevelName(level);
    line += L' ';
    line += message;
    return line;
}

// =============================================================================
// Impl — everything the header refuses to expose
// =============================================================================
struct RotatingLogFile::Impl {
    Config                  cfg;

    std::mutex              mutex;
    std::condition_variable cv;
    std::deque<std::wstring> queue;
    bool                    stop    = false;
    // Rows thrown away since the last time the file was told about it. Reported
    // into the file itself, so a gap in the sequence numbers has an explanation
    // sitting next to it rather than looking like lost data.
    long long               dropped = 0;

    std::thread             thread;

    // Writer-thread state. Nothing else may touch these — no lock guards them
    // because no other thread has any business reading them.
    HANDLE                  handle  = INVALID_HANDLE_VALUE;
    int                     rows    = 0;
    std::wstring            path;

    // Guards `path` alone, because CurrentPath() is called from the UI thread
    // for the overlay and the panel's status line.
    mutable std::mutex      pathMutex;

    void SetPath(const std::wstring &p) {
        std::lock_guard<std::mutex> lk(pathMutex);
        path = p;
    }
    std::wstring GetPath() const {
        std::lock_guard<std::mutex> lk(pathMutex);
        return path;
    }

    void CloseFile() {
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
            handle = INVALID_HANDLE_VALUE;
        }
        rows = 0;
        SetPath({});
    }

    void WriteRaw(const std::string &utf8) {
        if (handle == INVALID_HANDLE_VALUE || utf8.empty()) return;
        DWORD written = 0;
        WriteFile(handle, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    }

    // FIRST file of this run only. Cleared once a file has been chosen, so a
    // rotation always makes a new one rather than re-adopting.
    bool firstOpen = true;

    // How many DATA rows an existing file already holds. Lines beginning with
    // '#' are the preamble and any resume markers, and are not counted — the
    // rotation limit is a count of records, so the header must not shorten a
    // file's useful life.
    //
    // -1 when the file cannot be read at all, which is treated as "do not touch
    // it" rather than "it is empty".
    int CountRows(const std::wstring &p) const {
        HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return -1;

        LARGE_INTEGER size{};
        if (!GetFileSizeEx(h, &size) || size.QuadPart > (64LL << 20)) {
            CloseHandle(h);
            return -1;
        }

        std::string raw(static_cast<size_t>(size.QuadPart), '\0');
        DWORD read = 0;
        const bool ok = raw.empty() ||
                        ReadFile(h, raw.data(), static_cast<DWORD>(raw.size()), &read, nullptr) != 0;
        CloseHandle(h);
        if (!ok) return -1;
        raw.resize(read);

        // Counted on the UTF-8 bytes directly: '\n' and '#' are single-byte in
        // UTF-8 and can never appear inside a multi-byte sequence, so decoding
        // the whole file to count its lines would be work for nothing.
        int  rowCount = 0;
        bool atLineStart = true;
        for (const char c : raw) {
            if (c == '\n')      { atLineStart = true;  continue; }
            if (c == '\r')      { continue; }
            if (!atLineStart)   { continue; }
            atLineStart = false;
            if (c != '#') ++rowCount;   // a data row begins with anything else
        }
        return rowCount;
    }

    // The newest file this configuration wrote, or empty when there is none.
    //
    // NEWEST BY NAME, not by timestamp: the names end in a sortable stamp this
    // component chose itself, so the greatest name is the latest file. A
    // modification time would be the wrong answer the moment somebody copies the
    // folder or a backup tool touches it.
    std::wstring NewestExisting() const {
        std::wstring pattern = cfg.dir;
        if (!pattern.empty() && pattern.back() != L'\\' && pattern.back() != L'/')
            pattern += L'\\';
        pattern += cfg.baseName;
        pattern += L"_*";
        pattern += cfg.ext;

        WIN32_FIND_DATAW fd{};
        HANDLE find = FindFirstFileW(pattern.c_str(), &fd);
        if (find == INVALID_HANDLE_VALUE) return {};

        std::wstring best;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (best.empty() || best.compare(fd.cFileName) < 0) best = fd.cFileName;
        } while (FindNextFileW(find, &fd));
        FindClose(find);

        if (best.empty()) return {};

        std::wstring p = cfg.dir;
        if (!p.empty() && p.back() != L'\\' && p.back() != L'/') p += L'\\';
        return p + best;
    }

    // CONTINUES THE LAST FILE instead of starting a new one.
    //
    // Switching a log off and on again — which is one menu click, and easy to do
    // twice — used to close the file and open a fresh one on the next line. A
    // folder of near-empty stubs is the result, and the rotation limit stops
    // meaning anything: a "5000 rows per file" rule that produces files of 3
    // rows is not a rule.
    //
    // So the newest file is adopted and appended to, up to the SAME limit. Only
    // on the first open of a run: a rotation has filled the file it just closed,
    // and must never come back to it.
    bool AdoptNewest() {
        const std::wstring p = NewestExisting();
        if (p.empty()) return false;

        const int existing = CountRows(p);
        if (existing < 0 || existing >= cfg.maxRows) return false;

        HANDLE h = CreateFileW(p.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;

        handle = h;
        rows   = existing;
        SetPath(p);

        // A MARKER, so the seam is visible. Rows either side of it come from
        // different runs of the program — or from the same run with logging
        // switched off in between — and a reader who cannot see that would
        // otherwise read one continuous session that never happened.
        WriteRaw(ToUtf8(L"# resumed\t" + StampNow() + L"\r\n"));
        return true;
    }

    // Opens the next file and writes its preamble. Failure leaves the handle
    // invalid, and every later write is then a no-op — deliberately silent.
    // The alternative is a message box from a background thread about a
    // diagnostic nobody is watching, which is worse than a missing log.
    bool OpenNext() {
        CloseFile();

        if (!EnsureDirectory(cfg.dir)) return false;

        std::wstring p = cfg.dir;
        if (!p.empty() && p.back() != L'\\' && p.back() != L'/') p += L'\\';
        p += cfg.baseName;
        p += L'_';
        p += StampNow();

        // A SECOND FILE IN THE SAME SECOND is possible — a burst that rotates
        // twice inside one tick — and CREATE_ALWAYS would silently truncate the
        // first one. Suffixed until the name is free, so no file can ever be
        // destroyed by the rotation that follows it.
        std::wstring candidate = p + cfg.ext;
        for (int n = 2; n < 100; ++n) {
            const DWORD attr = GetFileAttributesW(candidate.c_str());
            if (attr == INVALID_FILE_ATTRIBUTES) break;
            candidate = p + L"_" + std::to_wstring(n) + cfg.ext;
        }

        handle = CreateFileW(candidate.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) return false;

        SetPath(candidate);
        rows = 0;

        // BOM first, so Notepad and Excel read UTF-8 rather than guessing at a
        // code page — a log full of machine names is exactly where that bites.
        static const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
        DWORD written = 0;
        WriteFile(handle, bom, 3, &written, nullptr);

        if (cfg.header) WriteRaw(ToUtf8(cfg.header()));
        return true;
    }

    // Picks the file this row belongs in, opening or rotating as needed.
    //
    // THE FIRST OPEN OF A RUN LOOKS FOR AN EXISTING FILE; every later one is a
    // rotation and must not. That is the whole difference between "continue
    // where we left off" and "come back to a file we just filled".
    bool EnsureOpen() {
        if (handle != INVALID_HANDLE_VALUE && rows < cfg.maxRows) return true;

        if (handle != INVALID_HANDLE_VALUE) CloseFile();   // full — rotate

        if (firstOpen) {
            firstOpen = false;
            if (!EnsureDirectory(cfg.dir)) return false;
            if (AdoptNewest()) return true;
        }
        return OpenNext();
    }

    void WriteRow(const std::wstring &row) {
        if (!EnsureOpen()) return;
        WriteRaw(ToUtf8(row) + "\r\n");
        ++rows;
    }

    // The writer. Drains under the lock, writes outside it — so a producer
    // queuing a row never waits for a disk that is busy.
    void Run() {
        // THE FOLDER IS MADE AT ONCE, before anything is queued, even though the
        // FILE is still not created until the first row arrives.
        //
        // The two are deliberately different. An empty file per quiet session is
        // clutter nobody wants; a missing FOLDER is indistinguishable from a
        // broken setting — switch logging on, look for logs\, find nothing, and
        // the only honest conclusion available to the user is that it does not
        // work. The folder existing and being empty says "on, nothing has
        // happened yet", which is the true state.
        //
        // On this thread rather than in Start(), so a slow or absent disk cannot
        // stall the menu click that switched it on.
        EnsureDirectory(cfg.dir);

        for (;;) {
            std::deque<std::wstring> batch;
            long long lost = 0;

            {
                std::unique_lock<std::mutex> lk(mutex);
                cv.wait(lk, [this] { return stop || !queue.empty(); });
                if (stop && queue.empty()) break;
                batch.swap(queue);
                lost = dropped;
                dropped = 0;
            }

            // REPORTED BEFORE the rows that followed the loss, so its position
            // in the file is where the gap actually is.
            if (lost > 0)
                WriteRow(L"# ---- " + std::to_wstring(lost) +
                         L" row(s) dropped: the writer could not keep up ----");

            for (const std::wstring &row : batch) WriteRow(row);

            FlushFileBuffers(handle == INVALID_HANDLE_VALUE ? nullptr : handle);
        }

        CloseFile();
    }
};

// =============================================================================
// Public surface
// =============================================================================
RotatingLogFile::~RotatingLogFile() {
    Stop();
}

void RotatingLogFile::Start(const Config &cfg) {
    if (m_impl) return;   // already running — a setting flipped twice is not two threads

    m_impl      = new Impl();
    m_impl->cfg = cfg;
    if (m_impl->cfg.maxRows < 1) m_impl->cfg.maxRows = 1;

    m_impl->thread = std::thread([impl = m_impl] { impl->Run(); });
}

void RotatingLogFile::Stop() {
    if (!m_impl) return;

    {
        std::lock_guard<std::mutex> lk(m_impl->mutex);
        m_impl->stop = true;
    }
    m_impl->cv.notify_all();

    if (m_impl->thread.joinable()) m_impl->thread.join();

    delete m_impl;
    m_impl = nullptr;
}

bool RotatingLogFile::IsRunning() const {
    return m_impl != nullptr;
}

void RotatingLogFile::Write(const std::wstring &row) {
    if (!m_impl) return;

    {
        std::lock_guard<std::mutex> lk(m_impl->mutex);
        // OLDEST FIRST when full. The newest rows are the ones being waited for
        // — a log that discards what just happened in order to keep what
        // happened a minute ago has the priority backwards.
        while (m_impl->queue.size() >= QUEUE_MAX) {
            m_impl->queue.pop_front();
            ++m_impl->dropped;
        }
        m_impl->queue.push_back(row);
    }
    m_impl->cv.notify_one();
}

std::wstring RotatingLogFile::CurrentPath() const {
    return m_impl ? m_impl->GetPath() : std::wstring();
}

} // namespace Persistence
