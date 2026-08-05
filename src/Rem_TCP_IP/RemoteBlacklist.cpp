// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#include "RemoteBlacklist.h"

#include "RemoteSettings.h"          // AddressMatches / LooksLikeAddress — one rule
#include "Persistence/IniFile.h"     // PathBesideExe
#include "Platform/Constants.h"

#include <windows.h>

#include <cwctype>
#include <mutex>

namespace Remote::Blacklist {

namespace RT = Constants::RemoteTcpIp;

namespace {
    std::vector<Entry> g_entries;
    std::mutex         g_mutex;
    bool               g_loaded = false;

    std::wstring Trim(const std::wstring &s) {
        size_t b = 0, e = s.size();
        while (b < e && ::iswspace(s[b])) ++b;
        while (e > b && ::iswspace(s[e - 1])) --e;
        return s.substr(b, e - b);
    }

    std::wstring NowStamp() {
        SYSTEMTIME st{};
        GetLocalTime(&st);   // LOCAL: this file is read by a person, not a machine
        wchar_t buf[32];
        swprintf_s(buf, L"%04u-%02u-%02u %02u:%02u:%02u",
                   st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        return buf;
    }

    // Reads the whole file as text. UTF-16LE when it carries the BOM this module
    // writes, UTF-8 otherwise — a file created by hand in Notepad or by a script
    // is far more likely to be UTF-8, and refusing to read it would look like
    // the blacklist silently not working.
    std::wstring ReadWholeFile(const std::wstring &path) {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return {};

        LARGE_INTEGER size{};
        if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0 ||
            size.QuadPart > 8 * 1024 * 1024) {   // a blacklist is never megabytes
            CloseHandle(h);
            return {};
        }

        std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
        DWORD got = 0;
        const bool ok = ReadFile(h, bytes.data(), static_cast<DWORD>(bytes.size()),
                                 &got, nullptr) != 0;
        CloseHandle(h);
        if (!ok) return {};
        bytes.resize(got);

        // UTF-16LE BOM
        if (bytes.size() >= 2 &&
            static_cast<unsigned char>(bytes[0]) == 0xFF &&
            static_cast<unsigned char>(bytes[1]) == 0xFE) {
            const size_t chars = (bytes.size() - 2) / sizeof(wchar_t);
            return std::wstring(reinterpret_cast<const wchar_t *>(bytes.data() + 2), chars);
        }

        size_t offset = 0;
        if (bytes.size() >= 3 &&
            static_cast<unsigned char>(bytes[0]) == 0xEF &&
            static_cast<unsigned char>(bytes[1]) == 0xBB &&
            static_cast<unsigned char>(bytes[2]) == 0xBF)
            offset = 3;   // UTF-8 BOM

        const int n = MultiByteToWideChar(CP_UTF8, 0, bytes.data() + offset,
                                          static_cast<int>(bytes.size() - offset),
                                          nullptr, 0);
        if (n <= 0) return {};
        std::wstring out(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, bytes.data() + offset,
                            static_cast<int>(bytes.size() - offset), out.data(), n);
        return out;
    }

    // "<address>[;<when>[;<reason>]]". False for a comment, a blank line, or an
    // address that could not be one — a malformed line is SKIPPED rather than
    // failing the load, because one bad line must not disable the whole
    // blacklist. That failure direction matters here more than anywhere else in
    // the program: the consequence is admitting everyone it was meant to refuse.
    bool ParseLine(const std::wstring &raw, Entry &out) {
        const std::wstring line = Trim(raw);
        if (line.empty()) return false;
        if (line[0] == RT::BLACKLIST_FIELD_SEP ||
            line[0] == RT::BLACKLIST_COMMENT_ALT) return false;   // comment

        const size_t a = line.find(RT::BLACKLIST_FIELD_SEP);
        out.address = Trim(a == std::wstring::npos ? line : line.substr(0, a));
        if (!Remote::LooksLikeAddress(out.address)) return false;

        if (a == std::wstring::npos) return true;   // bare address — hand written

        const size_t b = line.find(RT::BLACKLIST_FIELD_SEP, a + 1);
        if (b == std::wstring::npos) {
            out.when = Trim(line.substr(a + 1));
            return true;
        }

        out.when = Trim(line.substr(a + 1, b - a - 1));
        // Everything after the second separator is the reason, semicolons and
        // all — it is free text and the last field, so there is nothing after it
        // to be confused with.
        out.reason = Trim(line.substr(b + 1));
        return true;
    }

    // Appends one line, creating the file with its explanatory header first.
    // Written UTF-16LE to match every other file this program creates and so a
    // reason with non-ASCII characters survives.
    void AppendLine(const std::wstring &path, const std::wstring &line) {
        const bool creating = !Persistence::Ini::Exists(path);

        HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                               nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;

        std::wstring text;
        if (creating) {
            text += static_cast<wchar_t>(0xFEFF);   // UTF-16LE BOM — must be first
            text += RT::BLACKLIST_FILE_HEADER;
            text += L";\r\n";
            text += Persistence::Ini::GeneratedStampLines();
            text += L"\r\n";
        }
        text += line;
        text += L"\r\n";

        DWORD written = 0;
        WriteFile(h, text.data(), static_cast<DWORD>(text.size() * sizeof(wchar_t)),
                  &written, nullptr);
        CloseHandle(h);

        // After the append, so the stamp records when the last address was
        // added — which for this file is the interesting question.
        if (!creating) Persistence::Ini::TouchUpdatedStamp(path);
    }

    // Caller holds g_mutex.
    bool IsBlockedLocked(const std::wstring &address) {
        for (const Entry &e : g_entries)
            if (Remote::AddressMatches(e.address, address)) return true;
        return false;
    }

    // --- Timed blocks -------------------------------------------------------
    //
    // Its own mutex, not g_mutex: this list is written from the UI thread (the
    // panel's buttons) and read on the accept path, while g_mutex additionally
    // guards file IO. Sharing one lock would put a panel click behind a disk
    // write, on the same mutex every incoming connection takes.
    std::vector<TimedEntry> g_timed;
    std::mutex              g_timedMutex;

    long long NowTicks() { return static_cast<long long>(GetTickCount64()); }

    // Caller holds g_timedMutex. Drops what has run out, so the list is pruned
    // by ordinary use rather than by a timer — the accept path and the panel
    // between them touch it often enough, and an expired entry that nobody has
    // looked at yet has no effect on anything.
    void PruneTimedLocked() {
        const long long now = NowTicks();
        std::erase_if(g_timed, [now](const TimedEntry &t) { return t.untilMs <= now; });
    }

    // Caller holds g_timedMutex.
    bool IsTimedBlockedLocked(const std::wstring &address) {
        for (const TimedEntry &t : g_timed)
            if (Remote::AddressMatches(t.address, address)) return true;
        return false;
    }
}

const std::wstring &FilePath() {
    static const std::wstring path =
        Persistence::Ini::PathBesideExe(RT::BLACKLIST_FILE_NAME);
    return path;
}

bool FileExists() { return Persistence::Ini::Exists(FilePath()); }

void Reload() {
    const std::wstring text = ReadWholeFile(FilePath());

    std::vector<Entry> parsed;
    size_t start = 0;
    while (start <= text.size() && parsed.size() < RT::BLACKLIST_MAX) {
        size_t nl = text.find(L'\n', start);
        if (nl == std::wstring::npos) nl = text.size();

        Entry e;
        if (ParseLine(text.substr(start, nl - start), e)) parsed.push_back(std::move(e));

        if (nl >= text.size()) break;
        start = nl + 1;
    }

    std::lock_guard<std::mutex> lk(g_mutex);
    g_entries = std::move(parsed);
    g_loaded  = true;
}

bool IsBlocked(const std::wstring &address) {
    // Never answer "not blocked" merely because nobody loaded the file. A
    // blacklist that fails open is worse than no blacklist, because it is
    // believed. Checked and loaded OUTSIDE the lock — Reload takes the same
    // mutex, so holding it here would deadlock.
    bool needLoad;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        needLoad = !g_loaded;
    }
    if (needLoad) Reload();

    // TIMED FIRST, because it is the cheap one: an in-memory vector that is
    // empty in every ordinary install, with no file behind it. The permanent
    // list is checked second and is equally empty most of the time — neither
    // costs anything real, and the order only matters for saying which.
    {
        std::lock_guard<std::mutex> lk(g_timedMutex);
        PruneTimedLocked();
        if (IsTimedBlockedLocked(address)) return true;
    }

    std::lock_guard<std::mutex> lk(g_mutex);
    return IsBlockedLocked(address);
}

void AddTimed(const std::wstring &address, int minutes, const std::wstring &reason) {
    if (address.empty() || !Remote::LooksLikeAddress(address)) return;
    if (minutes <= 0) return;

    const long long until = NowTicks() + static_cast<long long>(minutes) * 60 * 1000;

    std::lock_guard<std::mutex> lk(g_timedMutex);
    PruneTimedLocked();

    // REPLACE rather than accumulate. Two timed blocks on one address would
    // both have to expire before it came back, which makes "blocked for ten
    // minutes" mean something different depending on what was pressed before.
    for (TimedEntry &t : g_timed) {
        if (t.address == address) {
            t.untilMs = until;
            t.reason  = reason;
            return;
        }
    }

    // Full: drop the entry closest to expiring, which is the one whose loss
    // changes the least. Not a refusal like the permanent list's cap — that one
    // fails closed on a file it cannot grow, while this is a live decision an
    // operator just made and is entitled to see take effect.
    if (g_timed.size() >= RT::TIMED_BLOCK_MAX) {
        auto soonest = g_timed.begin();
        for (auto it = g_timed.begin(); it != g_timed.end(); ++it)
            if (it->untilMs < soonest->untilMs) soonest = it;
        g_timed.erase(soonest);
    }

    TimedEntry t;
    t.address = address;
    t.reason  = reason;
    t.untilMs = until;
    g_timed.push_back(std::move(t));
}

bool ClearTimed(const std::wstring &address) {
    std::lock_guard<std::mutex> lk(g_timedMutex);
    const size_t before = g_timed.size();
    std::erase_if(g_timed, [&address](const TimedEntry &t) { return t.address == address; });
    return g_timed.size() != before;
}

std::vector<TimedEntry> TimedSnapshot() {
    std::lock_guard<std::mutex> lk(g_timedMutex);
    PruneTimedLocked();
    return g_timed;
}

void Add(const std::wstring &address, const std::wstring &reason) {
    if (address.empty() || !Remote::LooksLikeAddress(address)) return;

    Entry e;
    e.address = address;
    e.when    = NowStamp();
    e.reason  = reason;

    {
        std::lock_guard<std::mutex> lk(g_mutex);
        if (IsBlockedLocked(address)) return;   // already covered — no duplicate line
        if (g_entries.size() >= RT::BLACKLIST_MAX) return;
        g_entries.push_back(e);
    }

    // Outside the lock: file IO must not hold a mutex that the accept path takes
    // on every connection.
    const wchar_t sep = RT::BLACKLIST_FIELD_SEP;
    AppendLine(FilePath(), e.address + sep + e.when + sep + e.reason);
}

std::vector<Entry> Snapshot() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_entries;
}

size_t Count() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_entries.size();
}

} // namespace Remote::Blacklist
