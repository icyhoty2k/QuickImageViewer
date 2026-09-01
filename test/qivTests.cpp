// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

//
// qivTests — unit tests for the parts of qIV that are pure logic.
//
// WHY THERE IS NO TEST FRAMEWORK HERE. Catch2 and GoogleTest are both fine, and
// both would be a new dependency, a build-system change and a pile of macro
// machinery to read through before you can see what a failing test means. The
// harness below is thirty lines, has no magic in it, and prints the file and
// line of anything that fails. If the suite ever grows past a few hundred cases
// that trade stops being worth it — until then it is not.
//
// WHAT IS TESTABLE AND WHAT IS NOT. Everything here is a free function over
// plain values. The rest of qIV is windows, Direct2D, the registry and a global
// AppState, none of which can be exercised without standing the whole program
// up — so this suite deliberately covers the small, sharp-edged pieces where a
// silent behaviour change is both plausible and expensive:
//
//   * Base64      — the wire format. A rounding error here corrupts every image
//                   the phone sends or receives, and does it silently.
//   * Converters  — zoom is stored as an integer percent and read back as a
//                   float. That round trip is a settings value; if it drifts,
//                   the user's zoom quietly changes on every restart.
//   * FuzzyMatch  — the Find dialog's wildcard rules, which are exactly the kind
//                   of thing that "obviously" works until an anchor is dropped.
//
//   * AllowList    — DONE 2026-08-06, and it was the reason the note that used
//                    to sit here existed. The matcher decides who may open a
//                    socket to this machine, and its failure mode is silent:
//                    too permissive produces no crash and no log line. It could
//                    not be linked while it lived in RemoteSettings.cpp, which
//                    also pulls in AppState, RemoteBlacklist, RemoteCrypto and
//                    DedicatedSettings — so it was split into AddressMatch.cpp,
//                    declarations unchanged, and covered here.
//   * Logging      — the rotating writer and the wire log's save/load pair.
//                    Rotation and format round-tripping both fail silently, and
//                    the very first run caught a real one: the UTF-8 BOM was
//                    being counted as a data row, so every adopted file rotated
//                    one row early.
//
// STILL NOT COVERED, and worth saying out loud: anything that needs a window, a
// message pump or a socket. The two use-after-frees fixed on 2026-08-06 —
// RemoteClientsWnd::DoKick and RemotesWnd::DoRemoveTarget, both a reference into
// a vector held across a modal dialog — are not reachable from here. They were
// found by reading a minidump and then grepping for the same shape. Different
// tool, different bug; this suite is not the whole answer.
//

// windows.h FIRST: Constants.h, which the log headers pull in, uses DWORD and
// COLORREF at namespace scope without including it itself.
#include <windows.h>

#include "Common/Base64.h"
#include "Common/Converters.h"
#include "Common/FuzzyMatch.h"
#include "Common/Utf8.h"           // the history files' encoding
#include "Common/DuplicateFinder.h" // what counts as the same picture
#include "Common/PreviewStrip.h"    // which thumbnail a click landed on
#include "Platform/FolderIndex.h"   // where a file name starts inside a path
#include "Persistence/HistoryFoldersManager.h" // HistoryPath - untrusted line hygiene
#include "Persistence/IniFile.h"         // every persisted setting travels through this
#include "Persistence/RotatingLogFile.h"
#include "Rem_TCP_IP/RemoteLog.h"
#include "Rem_TCP_IP/RemoteSettings.h"   // AddressMatches / InList — the accept gate
#include "Rem_TCP_IP/RemoteCrypto.h"     // the password hash and handshake secret
#include "Rem_TCP_IP/AuthFailPolicy.h"   // what a wrong password costs
#include "Platform/Constants.h"          // APP_NAME, for the banner
#include "UI/AppMenu/AppMenuIds.h"       // the id space the menu tests check

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cctype>    // isdigit / isalnum — parsing AppMenuIds.h
#include <cstdlib>   // atoi — the same
#include <cstring>   // strlen — the HMAC test vector's message length
#include <string>
#include <vector>

// ── harness ─────────────────────────────────────────────────────────────────

namespace {
    int  g_checks       = 0;   // whole run
    int  g_failed       = 0;
    int  g_groups       = 0;   // counted rather than hardcoded — a group added
                               // below must not need the summary line edited too
    int  g_groupChecks  = 0;   // current group only
    int  g_groupFailed  = 0;
    bool g_verbose      = false;

    // What a check is CURRENTLY proving, set by NOTE(). Printed with a failure
    // so the output says what broke in English, not only which expression
    // returned false — "TWFuT" being rejected means nothing on its own.
    const char *g_note = "";

    // --- Colour ---------------------------------------------------------------
    //
    // ANSI escapes, but only once the console has been asked to interpret them
    // and has agreed. Redirect the output to a file or pipe it and the request
    // fails, g_colour stays false, and what lands in the file is plain text
    // rather than a mess of escape sequences — which is what makes this safe to
    // run from ctest and from CI.
    bool g_colour = false;

    void EnableColour() {
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD  mode = 0;
        if (h == INVALID_HANDLE_VALUE || !GetConsoleMode(h, &mode)) return;
        if (SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING))
            g_colour = true;
    }

    const char *Green() { return g_colour ? "\x1b[32m" : ""; }
    const char *Red()   { return g_colour ? "\x1b[31m" : ""; }
    const char *Dim()   { return g_colour ? "\x1b[90m" : ""; }
    const char *Bold()  { return g_colour ? "\x1b[1m"  : ""; }
    const char *Off()   { return g_colour ? "\x1b[0m"  : ""; }

    void Report(bool ok, const char *expr, const char *file, int line) {
        ++g_checks;
        ++g_groupChecks;
        if (ok) {
            if (g_verbose) std::printf("      %s·%s %s%s%s\n", Green(), Off(), Dim(), expr, Off());
            return;
        }
        ++g_failed;
        ++g_groupFailed;
        // The failure block is the one thing here that must be readable at a
        // glance in a CI log, so it is indented, labelled and never coloured
        // into invisibility.
        std::printf("\n      %s✗ FAILED%s  %s\n", Red(), Off(), expr);
        if (*g_note) std::printf("        %swhile checking:%s %s\n", Dim(), Off(), g_note);
        std::printf("        %s%s:%d%s\n", Dim(), file, line, Off());
    }

    // Wall time per group. Cheap, and it turns the table into something that
    // also answers "what is slow" — a group that suddenly costs 200 ms has
    // usually started doing IO somebody did not intend.
    std::chrono::steady_clock::time_point g_groupStart;

    // Set by a group that prints its own lines — the benchmarks. Its title
    // cannot share a line with a summary that arrives several lines later.
    bool g_groupMultiline = false;

    void BeginGroup(const char *title, bool multiline = false) {
        ++g_groups;
        g_groupChecks    = 0;
        g_groupFailed    = 0;
        g_note           = "";
        g_groupMultiline = multiline;
        g_groupStart     = std::chrono::steady_clock::now();
        std::printf("  %-44s", title);
        if (g_verbose || multiline) std::printf("\n");
    }

    void EndGroup() {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - g_groupStart).count();

        // A failure, a verbose run and a self-printing group all leave the
        // cursor at the start of a line, so the summary needs re-indenting to
        // land back under the column.
        if (g_verbose || g_groupFailed || g_groupMultiline) std::printf("  %-44s", "");

        if (g_groupFailed)
            std::printf("%s%4d✗%s %3d checks %s%5lldms%s\n",
                        Red(), g_groupFailed, Off(), g_groupChecks, Dim(), ms, Off());
        else
            std::printf("%s   ✓%s %3d checks %s%5lldms%s\n",
                        Green(), Off(), g_groupChecks, Dim(), ms, Off());
    }

    void Rule() {
        std::printf("  %s", Dim());
        for (int i = 0; i < 62; ++i) std::printf("─");
        std::printf("%s\n", Off());
    }
}

#define CHECK(expr) Report((expr), #expr, __FILE__, __LINE__)

// Labels the checks that follow. Costs nothing when everything passes and turns
// a failure from "an expression was false" into a sentence.
#define NOTE(text) (g_note = (text))

// Floats are compared with a tolerance rather than ==, because every value here
// has been through a divide by 100 and back.
#define CHECK_NEAR(a, b) Report(std::fabs((a) - (b)) < 1e-4f, #a " ~= " #b, __FILE__, __LINE__)

// ── Base64 ──────────────────────────────────────────────────────────────────

namespace {

    // Decode takes wchar_t because the wire is read as wide text; Encode returns
    // narrow. Widening here is what a caller does, so the round trip is tested
    // the way it is actually used rather than in a shape nothing performs.
    std::wstring Widen(const std::string &s) {
        return std::wstring(s.begin(), s.end());
    }

    bool RoundTrips(const std::vector<unsigned char> &in) {
        const std::string encoded = Common::Base64::Encode(in.data(), in.size());
        const std::wstring wide   = Widen(encoded);
        std::vector<unsigned char> out;
        if (!Common::Base64::Decode(wide.c_str(), wide.size(), out)) return false;
        return out == in;
    }

    void TestBase64() {
        NOTE("padding: 1 byte -> '==', 2 -> '=', 3 -> none");
        // The three padding cases. One input byte produces two '=' , two produce
        // one, three produce none — get this wrong and every transfer whose size
        // is not a multiple of three is corrupt at the tail.
        CHECK(Common::Base64::Encode(reinterpret_cast<const unsigned char *>("M"), 1) == "TQ==");
        CHECK(Common::Base64::Encode(reinterpret_cast<const unsigned char *>("Ma"), 2) == "TWE=");
        CHECK(Common::Base64::Encode(reinterpret_cast<const unsigned char *>("Man"), 3) == "TWFu");

        NOTE("empty input encodes to an empty string, not a stray pad");
        // Empty in, empty out — not a crash and not a stray pad.
        CHECK(Common::Base64::Encode(nullptr, 0).empty());

        NOTE("all 256 byte values survive a round trip (signed-char bugs)");
        // Every byte value, so a signed-char bug in the shifts cannot hide in the
        // half of the range that ordinary text never reaches. This is the check
        // that catches "works on ASCII, corrupts JPEGs".
        std::vector<unsigned char> all;
        all.reserve(256);
        for (int i = 0; i < 256; ++i) all.push_back(static_cast<unsigned char>(i));
        CHECK(RoundTrips(all));

        NOTE("lengths 0..9 round trip across the 3-byte group boundary");
        // Lengths either side of the 3-byte group boundary.
        for (size_t n = 0; n <= 9; ++n) {
            std::vector<unsigned char> v;
            for (size_t i = 0; i < n; ++i) v.push_back(static_cast<unsigned char>(0xF0 + i));
            CHECK(RoundTrips(v));
        }

        NOTE("decoder skips non-alphabet chars; only a cut quartet is an error");
        // The decoder's contract, which is narrower than "rejects rubbish":
        // characters outside the alphabet are SKIPPED, so whitespace and line
        // breaks in a wire body cost nothing. Only a truncated quartet — six
        // leftover bits, meaning a character was lost in transit — is an error.
        std::vector<unsigned char> out;
        CHECK(Common::Base64::Decode(L"TWF!u", 5, out));   // '!' skipped -> "Man"
        CHECK(out.size() == 3 && out[0] == 'M' && out[1] == 'a' && out[2] == 'n');
        CHECK(Common::Base64::Decode(L"TW Fu", 5, out));   // embedded space, fine
        CHECK(!Common::Base64::Decode(L"TWFuT", 5, out));  // quartet cut short
    }

    // ── Converters ──────────────────────────────────────────────────────────

    void TestConverters() {
        NOTE("percent <-> ratio, the single conversion factor");
        // Percent <-> ratio, the one factor in the codebase.
        CHECK_NEAR(Converters::PercentToRatio(100.0f), 1.0f);
        CHECK_NEAR(Converters::RatioToPercent(1.0f), 100.0f);
        CHECK_NEAR(Converters::PercentToRatio(250.0f), 2.5f);

        NOTE("zoom survives registry storage: int percent -> float -> int");
        // Storage round trip. This is a SETTINGS value: zoom is written to the
        // registry as an integer percent and read back as a float, so a drift
        // here changes the user's zoom slightly on every restart.
        for (int stored : {1, 25, 50, 100, 150, 400, 800, 6400}) {
            CHECK(Converters::toZoomInt(Converters::toZoomFloat(stored)) == stored);
        }

        NOTE("toZoomInt rounds, it does not floor");
        // toZoomInt ROUNDS (it adds 0.5 before truncating) rather than flooring.
        CHECK(Converters::toZoomInt(1.004f) == 100);
        CHECK(Converters::toZoomInt(1.006f) == 101);

        NOTE("FormatPercentCompact trims trailing zeros at both extremes");
        // Trailing zeros are trimmed, so one format serves both ends of the
        // range — the reason a hardcoded "%.1f" was wrong here.
        CHECK(Converters::FormatPercentCompact(99999.0f) == L"99999");
        CHECK(Converters::FormatPercentCompact(0.1f) == L"0.1");
        CHECK(Converters::FormatPercentCompact(0.01f) == L"0.01");

        NOTE("a non-positive zoom prints \"0%\", never \"-nan%\"");
        // A non-positive or non-finite zoom must never reach the overlay as
        // "-nan%". The guard returns a printable answer instead.
        CHECK(Converters::FormatZoomPercent(0.0f) == L"0%");
        CHECK(Converters::FormatZoomPercent(-1.0f) == L"0%");
        CHECK(Converters::FormatZoomPercent(1.0f) == L"100%");
    }

    // ── FuzzyMatch / WildcardMatch ──────────────────────────────────────────

    void TestWildcard() {
        NOTE("IsWildcardQuery detects * and ?");
        CHECK(Common::IsWildcardQuery(L"a*b", 3));
        CHECK(Common::IsWildcardQuery(L"a?b", 3));
        CHECK(!Common::IsWildcardQuery(L"abc", 3));
        CHECK(!Common::IsWildcardQuery(L"", 0));

        NOTE("wildcards are anchored: no implicit leading or trailing *");
        // ANCHORED AT BOTH ENDS — the documented rule is "matches the entire
        // text, no implicit leading or trailing *". Dropping either anchor is a
        // one-character edit that makes Find quietly match far too much, and
        // nothing else in the app would notice.
        CHECK(Common::WildcardMatch(L"photo", L"photo"));
        CHECK(!Common::WildcardMatch(L"photo", L"photograph"));
        CHECK(!Common::WildcardMatch(L"graph", L"photograph"));
        CHECK(Common::WildcardMatch(L"photo*", L"photograph"));
        CHECK(Common::WildcardMatch(L"*graph", L"photograph"));

        NOTE("? matches exactly one character, never zero");
        // ? is exactly one character, never zero.
        CHECK(Common::WildcardMatch(L"im?.jpg", L"img.jpg"));
        CHECK(!Common::WildcardMatch(L"im?.jpg", L"im.jpg"));

        NOTE("* spans any run of characters, including an empty one");
        // * spans any run, including none.
        CHECK(Common::WildcardMatch(L"*.jpg", L"holiday.jpg"));
        CHECK(Common::WildcardMatch(L"*", L"anything"));
        CHECK(Common::WildcardMatch(L"a*b", L"ab"));
        CHECK(Common::WildcardMatch(L"a*b", L"axxxb"));
        CHECK(!Common::WildcardMatch(L"a*b", L"axxxc"));

        NOTE("matching is case-SENSITIVE; the caller must lowercase both sides");
        // Both sides must already be lowercased by the caller — asserted so the
        // contract is visible in a test rather than only in a comment.
        CHECK(!Common::WildcardMatch(L"photo", L"PHOTO"));
    }
    // ── FuzzyMatch scoring ──────────────────────────────────────────────────
    //
    // The Find dialog's non-wildcard path, and until now completely untested.
    // It is subsequence matching: every query character must appear in order,
    // not necessarily adjacently.

    void TestFuzzy() {
        Common::FuzzyMatchResult r;

        NOTE("subsequence: characters must appear in order, not adjacently");
        CHECK(Common::FuzzyMatch(L"hld", 3, L"holiday", 7, r));
        CHECK(Common::FuzzyMatch(L"holiday", 7, L"holiday", 7, r));
        CHECK(!Common::FuzzyMatch(L"dloh", 4, L"holiday", 7, r));   // wrong order
        CHECK(!Common::FuzzyMatch(L"holidayx", 8, L"holiday", 7, r));

        NOTE("an empty query is REFUSED here, not treated as matching everything: "
             "the scoring reads positions[0] and positions[pi-1] unguarded, so a "
             "zero-length match would be an out-of-bounds read. Callers filter it "
             "out first. Empty text matches nothing either");
        CHECK(!Common::FuzzyMatch(L"", 0, L"holiday", 7, r));
        CHECK(!Common::FuzzyMatch(L"a", 1, L"", 0, r));

        NOTE("positions point at the matched characters, for highlighting");
        CHECK(Common::FuzzyMatch(L"hd", 2, L"holiday", 7, r));
        CHECK(r.posCount == 2);
        CHECK(r.positions[0] == 0);           // 'h' at index 0
        CHECK(r.positions[1] == 4);           // first 'd' at index 4
        CHECK(r.positions[0] < r.positions[1]);

        NOTE("a tighter match scores higher than a scattered one");
        Common::FuzzyMatchResult tight, loose;
        CHECK(Common::FuzzyMatch(L"ho", 2, L"holiday", 7, tight));
        CHECK(Common::FuzzyMatch(L"hy", 2, L"holiday", 7, loose));
        CHECK(tight.score >= loose.score);
    }

    // ── benchmarks ──────────────────────────────────────────────────────────
    //
    // THESE REPORT NUMBERS; THEY BARELY ASSERT. A timing assertion tight enough
    // to catch a 10% regression fails randomly on a loaded machine, and a suite
    // that cries wolf is a suite that gets ignored — which costs more than the
    // regression would have. The ceilings below are set to catch catastrophes
    // (an accidental O(n^2), a per-call allocation added to a hot loop), not
    // drift. Read the printed numbers; compare them build to build yourself.

    double MillisSince(std::chrono::steady_clock::time_point t0) {
        const auto dt = std::chrono::steady_clock::now() - t0;
        return std::chrono::duration<double, std::milli>(dt).count();
    }

    void Benchmarks() {
        // --- Base64, the wire path -----------------------------------------
        //
        // Worth measuring for a concrete reason: every image the phone saves
        // goes through Encode, and a 32 MB transfer is qIV's documented ceiling.
        // This is also the code that moved off the UI thread today, so its cost
        // is now paid on a socket thread — but it is still paid.
        {
            const size_t bytes = 8u * 1024u * 1024u;
            std::vector<unsigned char> data(bytes);
            for (size_t i = 0; i < bytes; ++i)
                data[i] = static_cast<unsigned char>(i * 31u + (i >> 7));

            auto t0 = std::chrono::steady_clock::now();
            const std::string encoded = Common::Base64::Encode(data.data(), data.size());
            const double encMs = MillisSince(t0);

            const std::wstring wide(encoded.begin(), encoded.end());
            std::vector<unsigned char> out;
            t0 = std::chrono::steady_clock::now();
            const bool ok = Common::Base64::Decode(wide.c_str(), wide.size(), out);
            const double decMs = MillisSince(t0);

            std::printf("\n      Base64 encode  %7.1f ms  (%6.1f MB/s)\n",
                        encMs, (bytes / 1048576.0) / (encMs / 1000.0));
            std::printf("      Base64 decode  %7.1f ms  (%6.1f MB/s)\n",
                        decMs, (bytes / 1048576.0) / (decMs / 1000.0));

            NOTE("Base64 round trip over 8 MB is correct and not pathologically slow");
            CHECK(ok);
            CHECK(out == data);
            CHECK(encMs < 5000.0);   // catastrophe ceiling, not a target
            CHECK(decMs < 5000.0);
        }

        // --- Find, over a folder far larger than any real one ---------------
        //
        // 20k names is well past what a photo folder holds, deliberately: the
        // Find dialog filters on every keystroke, so a per-call allocation or an
        // accidental quadratic here is felt as typing lag rather than as a slow
        // operation somebody would think to profile.
        {
            std::vector<std::wstring> names;
            names.reserve(20000);
            for (int i = 0; i < 20000; ++i)
                names.push_back(L"img_" + std::to_wstring(i) + L"_holiday_beach.jpg");

            Common::FuzzyMatchResult r;
            int hits = 0;

            auto t0 = std::chrono::steady_clock::now();
            for (const std::wstring &n : names)
                if (Common::WildcardMatch(L"*holiday*.jpg", n.c_str())) ++hits;
            const double wildMs = MillisSince(t0);

            t0 = std::chrono::steady_clock::now();
            for (const std::wstring &n : names)
                if (Common::FuzzyMatch(L"hlday", 5, n.c_str(),
                                       static_cast<int>(n.size()), r)) ++hits;
            const double fuzzMs = MillisSince(t0);

            std::printf("      WildcardMatch  %7.1f ms  (%6.0f ns each, 20k names)\n",
                        wildMs, wildMs * 1e6 / 20000.0);
            std::printf("      FuzzyMatch     %7.1f ms  (%6.0f ns each, 20k names)\n",
                        fuzzMs, fuzzMs * 1e6 / 20000.0);

            NOTE("Find scales linearly over 20k names and matches what it should");
            CHECK(hits == 40000);       // every name matches both patterns
            CHECK(wildMs < 2000.0);
            CHECK(fuzzMs < 2000.0);
        }
        std::printf("\n");
    }
} // namespace

// ── Logging ─────────────────────────────────────────────────────────────────
//
// The only tests here that touch the disk, and they earn it: rotation and
// continuation are FILE-SYSTEM behaviour, and both fail silently. A rotation
// that never fires produces one enormous file nobody notices until it will not
// open; a continuation that never fires produces a folder of three-line stubs.
// Neither shows up in a build, and neither is visible from a code review.
//
// Everything runs in a fresh directory under %TEMP% and is deleted afterwards,
// so the suite leaves nothing behind and cannot collide with a real logs\.

namespace {

    std::wstring MakeTempDir() {
        wchar_t base[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, base);

        // Process id AND tick count: two runs of the suite in the same second,
        // which is what a CI matrix does, must not share a directory.
        std::wstring d = std::wstring(base) + L"qivTests_" +
                         std::to_wstring(GetCurrentProcessId()) + L"_" +
                         std::to_wstring(GetTickCount64());
        CreateDirectoryW(d.c_str(), nullptr);
        return d;
    }

    void RemoveTempDir(const std::wstring &dir) {
        WIN32_FIND_DATAW fd{};
        HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (find != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
            } while (FindNextFileW(find, &fd));
            FindClose(find);
        }
        RemoveDirectoryW(dir.c_str());
    }

    int CountFiles(const std::wstring &dir, const std::wstring &pattern) {
        WIN32_FIND_DATAW fd{};
        HANDLE find = FindFirstFileW((dir + L"\\" + pattern).c_str(), &fd);
        if (find == INVALID_HANDLE_VALUE) return 0;
        int n = 0;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) ++n;
        } while (FindNextFileW(find, &fd));
        FindClose(find);
        return n;
    }

    std::string ReadWhole(const std::wstring &path) {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return {};
        LARGE_INTEGER size{};
        GetFileSizeEx(h, &size);
        std::string raw(static_cast<size_t>(size.QuadPart), '\0');
        DWORD read = 0;
        if (!raw.empty()) ReadFile(h, raw.data(), static_cast<DWORD>(raw.size()), &read, nullptr);
        CloseHandle(h);
        raw.resize(read);
        return raw;
    }

    // Reads a file out of the source tree. Several tests below check the SOURCE
    // rather than the built behaviour, so this has to live above all of them —
    // it used to sit further down and only compiled by accident.
    std::string ReadSourceFile(const char *relativePath) {
        std::string full = QIV_SOURCE_ROOT;
        full += "/";
        full += relativePath;

        // fopen_s rather than fopen: the harness builds at /W4 now, and the
        // deprecation warning is the compiler being right - a suppression here
        // would be the first of many.
        std::FILE *f = nullptr;
        if (fopen_s(&f, full.c_str(), "rb") != 0 || !f) return std::string();

        std::string out;
        char buf[4096];
        size_t n = 0;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
            out.append(buf, n);
        std::fclose(f);
        return out;
    }

    // Data rows only — the same rule the component itself counts by, so a test
    // that disagrees with it is testing the wrong number.
    //
    // Skipping the BOM matters: without it the first byte, 0xEF, is not '#' and
    // reads as the start of a data row. That is precisely the bug this test
    // found in RotatingLogFile::CountRows, so the helper has to be right about
    // it or the test would go on agreeing with the defect.
    int DataRowsIn(const std::string &text) {
        size_t start = 0;
        if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
            static_cast<unsigned char>(text[1]) == 0xBB &&
            static_cast<unsigned char>(text[2]) == 0xBF)
            start = 3;

        int  rows        = 0;
        bool atLineStart = true;
        for (size_t i = start; i < text.size(); ++i) {
            const char c = text[i];
            if (c == '\n')    { atLineStart = true; continue; }
            if (c == '\r')    continue;
            if (!atLineStart) continue;
            atLineStart = false;
            if (c != '#') ++rows;
        }
        return rows;
    }

    bool Contains(const std::string &haystack, const char *needle) {
        return haystack.find(needle) != std::string::npos;
    }

    std::wstring TestPreamble() { return L"# unit test header\r\n"; }

    void TestLogLayout() {
        using Persistence::LogLevel;

        NOTE("level names are padded to five, so the message column lines up");
        // A viewer splitting on whitespace does not care, but a person reading
        // the raw file does — a ragged left edge reads as noise.
        CHECK(std::wstring(Persistence::LogLevelName(LogLevel::Info))  == L"INFO ");
        CHECK(std::wstring(Persistence::LogLevelName(LogLevel::Warn))  == L"WARN ");
        CHECK(std::wstring(Persistence::LogLevelName(LogLevel::Error)) == L"ERROR");
        CHECK(std::wstring(Persistence::LogLevelName(LogLevel::Trace)) == L"TRACE");

        NOTE("BuildLogLine emits time [thread] LEVEL message, in that order");
        // The shape every general-purpose log viewer parses without being
        // configured. Both logs go through this one function precisely so a
        // viewer set up for one file works on the other.
        const std::wstring line =
            Persistence::BuildLogLine(L"2026-08-06 04:52:39.735", 7412,
                                      LogLevel::Info, L"startup | hello");
        CHECK(line == L"2026-08-06 04:52:39.735 [7412] INFO  startup | hello");
    }

    void TestLogRotation() {
        const std::wstring dir = MakeTempDir();

        Persistence::RotatingLogFile::Config cfg;
        cfg.dir      = dir;
        cfg.baseName = L"unit";
        cfg.ext      = L".log";
        cfg.maxRows  = 5;
        cfg.header   = &TestPreamble;

        NOTE("first write creates the folder's first file, header and all");
        {
            Persistence::RotatingLogFile f;
            f.Start(cfg);
            for (int i = 0; i < 3; ++i) f.Write(L"row " + std::to_wstring(i));
            f.Stop();   // drains and joins, so the file is complete below
        }
        CHECK(CountFiles(dir, L"unit_*.log") == 1);

        NOTE("stop then start CONTINUES that file rather than opening a new one");
        // The bug this exists for: a menu toggle is one click and easy to do
        // twice, and a fresh file per toggle makes "5000 rows per file" mean
        // nothing.
        {
            Persistence::RotatingLogFile f;
            f.Start(cfg);
            f.Write(L"row 3");
            f.Stop();
        }
        CHECK(CountFiles(dir, L"unit_*.log") == 1);

        NOTE("the resumed marker records the seam between two runs");
        // Rows either side came from different runs. A reader who cannot see
        // that reads one continuous session that never happened.
        std::wstring only;
        {
            WIN32_FIND_DATAW fd{};
            HANDLE find = FindFirstFileW((dir + L"\\unit_*.log").c_str(), &fd);
            if (find != INVALID_HANDLE_VALUE) { only = dir + L"\\" + fd.cFileName; FindClose(find); }
        }
        const std::string body = ReadWhole(only);
        CHECK(Contains(body, "# resumed"));

        NOTE("the preamble is not counted against the rotation limit");
        // Four data rows so far, and the header lines must not have eaten any of
        // the five — otherwise a growing header silently shortens every file.
        CHECK(DataRowsIn(body) == 4);

        NOTE("passing maxRows rotates, and the new file starts empty of rows");
        {
            Persistence::RotatingLogFile f;
            f.Start(cfg);
            f.Write(L"row 4");   // fills the adopted file to 5
            f.Write(L"row 5");   // rotates
            f.Write(L"row 6");
            f.Stop();
        }
        CHECK(CountFiles(dir, L"unit_*.log") == 2);

        NOTE("a full newest file is never adopted — a new one is started");
        {
            Persistence::RotatingLogFile f;
            f.Start(cfg);
            for (int i = 0; i < 4; ++i) f.Write(L"more " + std::to_wstring(i));
            f.Stop();
        }
        // The second file held 2 rows; adopting it and adding 4 gives 6, which
        // passes 5 and rotates once. Two files before, one more after.
        CHECK(CountFiles(dir, L"unit_*.log") == 3);

        NOTE("writing while stopped is a no-op, not a crash");
        // Every producer calls Write without knowing whether logging is on.
        {
            Persistence::RotatingLogFile f;
            f.Write(L"nobody is listening");
            CHECK(!f.IsRunning());
            CHECK(f.CurrentPath().empty());
        }

        RemoveTempDir(dir);
    }

    void TestWireLogRoundTrip() {
        namespace RL = Remote::Log;

        const std::wstring dir  = MakeTempDir();
        const std::wstring file = dir + L"\\roundtrip.log";

        RL::Clear();
        RL::SetEnabled(true);

        // Deltas under a millisecond survive exactly; the format writes what a
        // person reads, so anything larger comes back rounded. These are chosen
        // to make the round trip exact rather than to hide that.
        RL::Add(RL::Direction::In,  L"peer 192.168.0.84:34659", L"Observe 1",
                L"qIV 0.0.0.0:8770", L"OK observing", 428);
        RL::Add(RL::Direction::Out, L"qIV 0.0.0.0:8770", L"(reply)",
                L"peer 192.168.0.84:34659", L"OK", 26);
        // -1 is "never measured", which renders as an em dash and must come back
        // as -1 rather than as zero.
        RL::Add(RL::Direction::In,  L"peer", L"hello someone", L"qIV", L"", -1);

        const std::vector<RL::Entry> before = RL::Snapshot();
        CHECK(before.size() == 3);

        std::wstring err;
        NOTE("a saved log writes without error");
        CHECK(RL::SaveTo(file, err));

        RL::Clear();
        CHECK(RL::Count() == 0);

        NOTE("and loads back — the writer and the reader agree on the columns");
        // The check that matters. FormatRow and LoadFrom are edited together and
        // drift silently: a field added on one side shifts every index on the
        // other, and the symptom is a log that reads back with the sender and
        // the response swapped.
        CHECK(RL::LoadFrom(file, err));

        const std::vector<RL::Entry> after = RL::Snapshot();
        CHECK(after.size() == before.size());

        if (after.size() == before.size()) {
            for (size_t i = 0; i < before.size(); ++i) {
                NOTE("every text field survives the round trip in its own column");
                CHECK(after[i].seq      == before[i].seq);
                CHECK(after[i].sender   == before[i].sender);
                CHECK(after[i].command  == before[i].command);
                CHECK(after[i].receiver == before[i].receiver);
                CHECK(after[i].response == before[i].response);
                CHECK(after[i].dir      == before[i].dir);

                NOTE("sub-millisecond deltas are exact; -1 stays -1");
                CHECK(after[i].deltaUs  == before[i].deltaUs);

                NOTE("timestamps survive to the millisecond the file records");
                // The raw FILETIME is no longer written — the readable stamp is
                // the source — so equality is to the millisecond, which is all
                // the panel has ever displayed.
                CHECK(after[i].whenFt / 10000 == before[i].whenFt / 10000);
            }
        }

        NOTE("a pipe in a wire line cannot split the row it is written on");
        // '|' separates the fields inside the message, so a peer sending one
        // would otherwise forge a column boundary. Flattened to '/' at capture,
        // exactly as tabs and newlines already were.
        RL::Clear();
        RL::Add(RL::Direction::In, L"peer", L"say a|b|c", L"qIV", L"OK", 5);
        CHECK(RL::SaveTo(file, err));
        RL::Clear();
        CHECK(RL::LoadFrom(file, err));
        const std::vector<RL::Entry> piped = RL::Snapshot();
        CHECK(piped.size() == 1);
        if (piped.size() == 1) CHECK(piped[0].command == L"say a/b/c");

        NOTE("a file that is not a qIV log is refused, not half-read");
        {
            HANDLE h = CreateFileW((dir + L"\\junk.log").c_str(), GENERIC_WRITE, 0, nullptr,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            const char *junk = "this is not a log\r\n";
            DWORD written = 0;
            WriteFile(h, junk, static_cast<DWORD>(strlen(junk)), &written, nullptr);
            CloseHandle(h);
        }
        CHECK(!RL::LoadFrom(dir + L"\\junk.log", err));
        CHECK(!err.empty());

        RL::SetEnabled(false);
        RL::Clear();
        RemoveTempDir(dir);
    }

    // ── AllowList — who is allowed to connect ───────────────────────────────
    //
    // THE MOST SECURITY-RELEVANT PURE LOGIC IN THE PROGRAM. `InList` is what the
    // accept gate asks before a peer may speak, and its failure mode is silent:
    // a matcher that is too permissive produces no crash, no error and no log
    // line — just a machine that admits somebody it should not have.
    //
    // Everything below is written against CURRENT behaviour, including the sharp
    // edges the implementation documents on purpose. A test that "fixed" one of
    // those by asserting what feels right would break real users' lists.

    // -------------------------------------------------------------------------
    //  The brute-force guard's escalation.
    //
    //  This is the half of the guard that decides what a wrong password costs,
    //  and until v3 it was unreachable from a test - the decision sat inside a
    //  function holding a mutex, a map and a file write. It is pure now, so the
    //  rule can be stated as checks instead of as a comment nobody can verify.
    // -------------------------------------------------------------------------
    // -------------------------------------------------------------------------
    //  UTF-8 for the history and favourites files.
    //
    //  These files were written through std::wofstream with no imbue, and the
    //  default C locale cannot represent anything above one byte. Writing such a
    //  character put the stream into fail+bad and DISCARDED EVERY LINE AFTER IT -
    //  so a Cyrillic folder name silently truncated the file. The conversion is
    //  explicit now, and these checks are what stop it regressing.
    // -------------------------------------------------------------------------
    // -------------------------------------------------------------------------
    //  HistoryPath - the hygiene every line of a hand-editable file passes.
    //
    //  Both history files are documented as editable by hand, so every line is a
    //  string somebody may have typed. Normalize is what decides whether one is
    //  a usable folder path, and until this was split out of
    //  HistoryFoldersManager.cpp nothing could reach it from a test.
    // -------------------------------------------------------------------------
    // -------------------------------------------------------------------------
    //  Duplicate detection - the rule, with no filesystem behind it.
    //
    //  This is the operation where being wrong is worst: somebody acting on a
    //  wrong answer deletes a photograph. So the rule is pure, and every way it
    //  could be wrong is a check here.
    // -------------------------------------------------------------------------
    // -------------------------------------------------------------------------
    //  THE PREVIEW STRIP - which thumbnail a click landed on.
    //
    //  The duplicate list draws every copy in a group side by side, and clicking
    //  one selects that copy. Get this wrong by one cell and the panel selects
    //  the picture NEXT to the one under the finger - and the next thing that
    //  happens is a Recycle Bin entry for the wrong file.
    //
    //  Boxes are `box` wide, `cell` apart, so cell - box is the gap between them.
    // -------------------------------------------------------------------------
    // -------------------------------------------------------------------------
    //  WHERE THE FILE NAME STARTS INSIDE A PATH.
    //
    //  Three lines inside a directory walk, and they were WRONG for a whole
    //  release: the separator set was written L"\/", which is not a valid
    //  escape. MSVC folds it to "/" alone, so on a Windows path nothing ever
    //  matched and every offset came back 0.
    //
    //  It went unnoticed because the value was computed and never read. Pulling
    //  it out is what makes it reachable from here at all.
    // -------------------------------------------------------------------------
    // -------------------------------------------------------------------------
    //  NO INVALID ESCAPE SEQUENCES IN ANY STRING LITERAL.
    //
    //  FolderIndex.cpp shipped L"\/" in 3.0. That is not a valid escape:
    //  the compiler folds it to "/" alone, so the separator set lost its
    //  backslash and every Windows path came back with a name offset of 0.
    //
    //  ⚠ THE COMPILER DID WARN, AND NOBODY READ IT. MSVC emits C4129,
    //  "unrecognized character escape sequence", five times over - once per
    //  translation unit that includes the header. It shipped anyway, because a
    //  warning in a build log that scrolls past is not a signal anyone acts on.
    //
    //  That is the argument for a test rather than for reading warnings harder:
    //  this FAILS, and a failure stops the run. Verified by injecting L"a\q b"
    //  into DuplicateScan.cpp - the compiler warned, the suite went red, and
    //  only the red one is impossible to walk past.
    //
    //  The files listed are the ones that actually spell out path separators.
    //  A new file carrying one has to be added here; that is the cost of the
    //  harness having no directory walk, and it is cheaper than the bug.
    // -------------------------------------------------------------------------
    void TestNoInvalidEscapes() {
        // Everything C++ defines, plus the numeric and universal forms.
        const std::string valid = "abfnrtv\\\'\"?01234567xuU";

        const char *files[] = {
            "src/Platform/FolderIndex.h",
            "src/Platform/FolderIndex.cpp",
            "src/Platform/DuplicateScan.cpp",
            "src/Common/DuplicateFinder.cpp",
            "src/UI/FloatingPanels/FindWnd.cpp",
            "src/Persistence/HistoryPath.cpp",
            "src/Platform/JumpList.cpp",
        };

        for (const char *rel : files) {
            const std::string src = ReadSourceFile(rel);
            NOTE(rel);
            CHECK(!src.empty());

            // COMMENTS ARE SKIPPED, and that is not a convenience.
            //
            // FolderIndex.h documents the bug this test exists for, and quotes
            // the broken literal to do it. A scan that cannot tell code from
            // prose flags that comment forever - and a test which fails on its
            // own documentation is one somebody deletes rather than obeys.
            int  offenders = 0;
            enum { CODE, LINE_COMMENT, BLOCK_COMMENT, STRING } state = CODE;

            for (size_t i = 0; i < src.size(); ++i) {
                const char c = src[i];
                const char n = (i + 1 < src.size()) ? src[i + 1] : '\0';

                if (state == LINE_COMMENT) {
                    if (c == '\n') state = CODE;
                } else if (state == BLOCK_COMMENT) {
                    if (c == '*' && n == '/') { state = CODE; ++i; }
                } else if (state == STRING) {
                    if (c == '\\') {
                        // An escaped backslash consumes the next character, so
                        // the pair after it is not itself an escape. Without the
                        // ++i, every "\\" would read as a backslash and a stray
                        // quote, and the scan would report the opposite of the truth.
                        if (valid.find(n) == std::string::npos) ++offenders;
                        ++i;
                    } else if (c == '\"') {
                        state = CODE;
                    }
                } else {
                    if (c == '/' && n == '/')       { state = LINE_COMMENT;  ++i; }
                    else if (c == '/' && n == '*') { state = BLOCK_COMMENT; ++i; }
                    else if (c == '\"')                { state = STRING; }
                }
            }
            CHECK(offenders == 0);
        }
    }

    void TestNameOffset() {
        using Platform::FolderIndex::NameOffsetOf;

        NOTE("a Windows path - the case that was broken");
        {
            const std::wstring p = L"C:\\Users\\me\\holiday.jpg";
            CHECK(NameOffsetOf(p) == 12); // the last separator sits at index 11
            CHECK(std::wstring(p.c_str() + NameOffsetOf(p)) == L"holiday.jpg");
        }

        NOTE("a forward-slash path - what the broken literal happened to handle");
        {
            const std::wstring p = L"C:/pics/holiday.jpg";
            CHECK(std::wstring(p.c_str() + NameOffsetOf(p)) == L"holiday.jpg");
        }

        NOTE("mixed separators - the LAST one wins, whichever kind it is");
        {
            CHECK(std::wstring(L"C:/pics\\a.png" + NameOffsetOf(L"C:/pics\\a.png")) == L"a.png");
            CHECK(std::wstring(L"C:\\pics/a.png" + NameOffsetOf(L"C:\\pics/a.png")) == L"a.png");
        }

        NOTE("a bare name has no separator, so the offset is 0");
        CHECK(NameOffsetOf(L"holiday.jpg") == 0);

        NOTE("an empty path is 0 rather than a crash");
        CHECK(NameOffsetOf(L"") == 0);

        NOTE("a trailing separator points one past the end, which is the empty name");
        // Not a case the walk produces - directory_iterator never yields one -
        // but the offset must stay INSIDE the string, because the caller adds it
        // to c_str() and reads from there.
        {
            const std::wstring p = L"C:\\pics\\";
            const int off = NameOffsetOf(p);
            CHECK(off == static_cast<int>(p.size()));
            CHECK(std::wstring(p.c_str() + off).empty());
        }

        NOTE("a UNC path keeps its share, and names the file after the last slash");
        {
            const std::wstring p = L"\\\\nas\\photos\\a.png";
            CHECK(std::wstring(p.c_str() + NameOffsetOf(p)) == L"a.png");
        }
    }

    void TestPreviewStrip() {
        using Common::PreviewStrip::SlotAt;

        // Three 100-wide boxes at x = 10, 120, 230; top 50; gap 10.
        const int L = 10, T = 50, BOX = 100, CELL = 110, N = 3;

        NOTE("a point inside a box selects THAT box");
        CHECK(SlotAt( 10, 50, L, T, BOX, CELL, N) == 0);   // exact top-left
        CHECK(SlotAt( 60, 100, L, T, BOX, CELL, N) == 0);  // middle of the first
        CHECK(SlotAt(170, 100, L, T, BOX, CELL, N) == 1);
        CHECK(SlotAt(280, 100, L, T, BOX, CELL, N) == 2);

        NOTE("the LAST pixel of a box still belongs to it");
        CHECK(SlotAt(109, 149, L, T, BOX, CELL, N) == 0);
        CHECK(SlotAt(329, 149, L, T, BOX, CELL, N) == 2);

        NOTE("the GAP between two boxes belongs to neither");
        // A click here is visibly in empty space; selecting the box on its left
        // would read as the click landing somewhere it did not.
        CHECK(SlotAt(110, 100, L, T, BOX, CELL, N) == -1);
        CHECK(SlotAt(119, 100, L, T, BOX, CELL, N) == -1);
        CHECK(SlotAt(120, 100, L, T, BOX, CELL, N) ==  1); // first pixel of the next

        NOTE("outside the strip is a miss, in every direction");
        CHECK(SlotAt(  9, 100, L, T, BOX, CELL, N) == -1); // left of the first
        CHECK(SlotAt(340, 100, L, T, BOX, CELL, N) == -1); // past the last
        CHECK(SlotAt( 60,  49, L, T, BOX, CELL, N) == -1); // above
        CHECK(SlotAt( 60, 150, L, T, BOX, CELL, N) == -1); // below

        NOTE("the gap AFTER the last box is a miss, not slot 2");
        CHECK(SlotAt(330, 100, L, T, BOX, CELL, N) == -1);

        NOTE("nothing on show means nothing to hit");
        // The strip is not painted when the count is zero, and the stored
        // geometry is stale - so the count has to be the thing that decides.
        CHECK(SlotAt( 60, 100, L, T, BOX, CELL, 0) == -1);
        CHECK(SlotAt( 60, 100, L, T,   0, CELL, N) == -1);
        CHECK(SlotAt( 60, 100, L, T, BOX,    0, N) == -1);

        NOTE("a fourth box is not hittable when only three are on show");
        // The list can hold more copies than the strip shows - PREVIEW_MAX caps
        // it - so a click past the last drawn one must miss rather than index
        // a picture that is not there.
        CHECK(SlotAt(390, 100, L, T, BOX, CELL, N) == -1);
    }

    void TestDuplicateFinder() {
        using namespace Common::DuplicateFinder;
        auto C = [](const wchar_t *path, unsigned long long size,
                    std::uint64_t digest, bool hashed) {
            Candidate c;
            c.path = path; c.size = size; c.digest = digest; c.hashed = hashed;
            return c;
        };

        NOTE("only files SHARING a size are worth reading");
        {
            std::vector<Candidate> in {
                C(L"a.jpg", 100, 0, false),
                C(L"b.jpg", 100, 0, false),
                C(L"c.jpg", 999, 0, false),   // alone at its size - never read
            };
            const auto need = NeedHashing(in);
            CHECK(need.size() == 2);
            CHECK(need[0] == 0);
            CHECK(need[1] == 1);
        }

        NOTE("a size of 0 is UNKNOWN and is never grouped with other unknowns");
        // Grouping them would read every file whose size could not be read, to
        // compare them against each other for no reason.
        {
            std::vector<Candidate> in {
                C(L"a.jpg", 0, 0, false),
                C(L"b.jpg", 0, 0, false),
            };
            CHECK(NeedHashing(in).empty());
        }

        NOTE("same size AND same digest is a duplicate");
        {
            std::vector<Candidate> in {
                C(L"one.jpg", 2048, 0xABCD, true),
                C(L"two.jpg", 2048, 0xABCD, true),
            };
            const auto groups = FindGroups(in);
            CHECK(groups.size() == 1);
            CHECK(groups[0].paths.size() == 2);
            CHECK(groups[0].size == 2048);
        }

        NOTE("same size, DIFFERENT digest is not");
        {
            std::vector<Candidate> in {
                C(L"one.jpg", 2048, 0xABCD, true),
                C(L"two.jpg", 2048, 0x1234, true),
            };
            CHECK(FindGroups(in).empty());
        }

        NOTE("same digest, DIFFERENT size is not - both must agree");
        {
            std::vector<Candidate> in {
                C(L"one.jpg", 2048, 0xABCD, true),
                C(L"two.jpg", 4096, 0xABCD, true),
            };
            CHECK(FindGroups(in).empty());
        }

        NOTE("a file that could NOT be read proves nothing and is never matched");
        // The one failure that would cost somebody a photograph: treating an
        // unreadable file as equal to whatever it sits beside.
        //
        // ⚠ THE DIGESTS HERE ARE BOTH 0 ON PURPOSE. An unhashed candidate keeps
        // the default digest, so the only way this check can fail when the
        // hashed guard is removed is if the file it sits beside also digests to
        // 0. Written with different digests - as it first was - the pair is
        // separated by the digest comparison instead, and the check passes
        // whether the guard is there or not: a test that cannot fail the real
        // way. Removing the guard now turns the suite red, which was proved by
        // doing it.
        {
            std::vector<Candidate> in {
                C(L"good.jpg",   2048, 0, true),   // legitimately digests to 0
                C(L"locked.jpg", 2048, 0, false),  // never read, so 0 by default
            };
            CHECK(FindGroups(in).empty());
        }

        NOTE("one copy is not a duplicate");
        {
            std::vector<Candidate> in { C(L"only.jpg", 10, 0x1, true) };
            CHECK(FindGroups(in).empty());
        }

        NOTE("three copies are ONE group of three, not three pairs");
        {
            std::vector<Candidate> in {
                C(L"a.jpg", 50, 0x7, true),
                C(L"b.jpg", 50, 0x7, true),
                C(L"c.jpg", 50, 0x7, true),
            };
            const auto groups = FindGroups(in);
            CHECK(groups.size() == 1);
            CHECK(groups[0].paths.size() == 3);
        }

        NOTE("biggest waste first, and the order is STABLE across runs");
        // The backing map has no defined iteration order; without the sort the
        // list would reshuffle between runs and be unusable.
        {
            std::vector<Candidate> in {
                C(L"small1.jpg", 10, 0x1, true),
                C(L"small2.jpg", 10, 0x1, true),
                C(L"big1.jpg",   99, 0x2, true),
                C(L"big2.jpg",   99, 0x2, true),
                C(L"big3.jpg",   99, 0x2, true),
            };
            const auto groups = FindGroups(in);
            CHECK(groups.size() == 2);
            CHECK(groups[0].paths.size() == 3);   // more copies leads
            CHECK(groups[1].size == 10);

            const auto again = FindGroups(in);
            CHECK(again.size() == groups.size());
            CHECK(again[0].paths.front() == groups[0].paths.front());
        }

        NOTE("Partition splits a group that only LOOKED identical");
        // The hash said these three match. Bytes say the middle one does not.
        // This is the collision case: astronomically unlikely, and the whole
        // reason the comparison exists, because the consequence is a deleted
        // photograph.
        {
            auto equal = [](const std::wstring &a, const std::wstring &b) {
                const bool aOdd = a.find(L"odd") != std::wstring::npos;
                const bool bOdd = b.find(L"odd") != std::wstring::npos;
                return aOdd == bOdd;
            };
            const std::vector<std::wstring> paths { L"a.jpg", L"odd.jpg", L"b.jpg" };
            const auto sets = Partition(paths, equal);
            CHECK(sets.size() == 2);
            CHECK(sets[0].size() == 2);          // a and b
            CHECK(sets[1].size() == 1);          // odd, alone
            CHECK(sets[1][0] == std::wstring(L"odd.jpg"));
        }

        NOTE("a group that IS identical survives partitioning whole");
        {
            auto always = [](const std::wstring &, const std::wstring &) { return true; };
            const std::vector<std::wstring> paths { L"a.jpg", L"b.jpg", L"c.jpg" };
            const auto sets = Partition(paths, always);
            CHECK(sets.size() == 1);
            CHECK(sets[0].size() == 3);
        }

        NOTE("a file that cannot be READ ends up alone, never somebody's duplicate");
        // The comparison answers false when it cannot read, and false must mean
        // "not the same" rather than "assume the same".
        {
            auto never = [](const std::wstring &, const std::wstring &) { return false; };
            const std::vector<std::wstring> paths { L"a.jpg", L"b.jpg" };
            const auto sets = Partition(paths, never);
            CHECK(sets.size() == 2);
            CHECK(sets[0].size() == 1);
            CHECK(sets[1].size() == 1);
        }

        NOTE("partitioning keeps the given order inside each set");
        {
            auto always = [](const std::wstring &, const std::wstring &) { return true; };
            const std::vector<std::wstring> paths { L"keep.jpg", L"copy.jpg" };
            const auto sets = Partition(paths, always);
            CHECK(sets.size() == 1);
            CHECK(sets[0][0] == std::wstring(L"keep.jpg"));
        }

        NOTE("paths keep the order they were given, so the caller decides the original");
        {
            std::vector<Candidate> in {
                C(L"keep-me.jpg", 8, 0x9, true),
                C(L"copy.jpg",    8, 0x9, true),
            };
            const auto groups = FindGroups(in);
            CHECK(groups.size() == 1);
            CHECK(groups[0].paths[0] == std::wstring(L"keep-me.jpg"));
        }
    }

    void TestHistoryPath() {
        std::wstring out;

        NOTE("a plain absolute path survives, unchanged");
        CHECK(HistoryPath::Normalize(L"D:\\Pictures\\Holiday", out));
        CHECK(out == std::wstring(L"D:\\Pictures\\Holiday"));

        NOTE("forward slashes become backslashes and repeats collapse");
        // A user typing a path from a browser or a script writes it either way.
        CHECK(HistoryPath::Normalize(L"D:/Pictures//Holiday", out));
        CHECK(out == std::wstring(L"D:\\Pictures\\Holiday"));

        NOTE("surrounding whitespace and one wrapping pair of quotes are trimmed");
        // Copying a path from Explorer's address bar brings the quotes with it.
        CHECK(HistoryPath::Normalize(L"   \"D:\\Pictures\\Holiday\"  ", out));
        CHECK(out == std::wstring(L"D:\\Pictures\\Holiday"));

        NOTE("a trailing separator goes, EXCEPT on a drive root");
        CHECK(HistoryPath::Normalize(L"D:\\Pictures\\", out));
        CHECK(out == std::wstring(L"D:\\Pictures"));
        CHECK(HistoryPath::Normalize(L"D:\\", out));
        CHECK(out == std::wstring(L"D:\\"));   // a drive root is a real folder

        NOTE("a bare drive letter is NOT a folder");
        CHECK(!HistoryPath::Normalize(L"D:", out));

        NOTE("relative and empty are rejected - the list stores absolute paths only");
        CHECK(!HistoryPath::Normalize(L"", out));
        CHECK(!HistoryPath::Normalize(L"    ", out));
        CHECK(!HistoryPath::Normalize(L"Pictures\\Holiday", out));
        CHECK(!HistoryPath::Normalize(L"..\\Holiday", out));

        NOTE("a UNC share is accepted");
        CHECK(HistoryPath::Normalize(L"\\\\nas\\photos", out));

        NOTE("characters Win32 forbids in a path are rejected");
        // This is the check that matters for a file anybody can edit: a line
        // carrying these is corrupt, not a folder somebody has.
        CHECK(!HistoryPath::Normalize(L"D:\\Pic<ures", out));
        CHECK(!HistoryPath::Normalize(L"D:\\Pic>ures", out));
        CHECK(!HistoryPath::Normalize(L"D:\\Pic|ures", out));
        CHECK(!HistoryPath::Normalize(L"D:\\Pic?ures", out));
        CHECK(!HistoryPath::Normalize(L"D:\\Pic*ures", out));

        NOTE("a TRAILING CR is trimmed, not rejected - it is surrounding whitespace");
        // Clean() runs first and trims whitespace, and CR is whitespace. So a
        // line from a CRLF file that reached here unstripped still yields the
        // right path rather than being thrown away. This is the belt to the
        // reader's braces, and the reason the reader's CR removal is a
        // convenience rather than the only thing standing between a CRLF file
        // and an empty history.
        std::wstring withCr = L"D:\\Pictures";
        withCr += static_cast<wchar_t>(13);
        CHECK(HistoryPath::Normalize(withCr, out));
        CHECK(out == std::wstring(L"D:\\Pictures"));

        NOTE("a control character INSIDE the path is still rejected");

        std::wstring withTab = L"D:\\Pic";
        withTab += static_cast<wchar_t>(9);
        withTab += L"tures";
        CHECK(!HistoryPath::Normalize(withTab, out));

        NOTE("an absurd length is a corrupt line, not a path");
        CHECK(!HistoryPath::Normalize(L"D:\\" + std::wstring(40000, L'a'), out));

        NOTE("rejected input leaves 'out' untouched, so a caller cannot use half a result");
        std::wstring keep = L"UNTOUCHED";
        CHECK(!HistoryPath::Normalize(L"nonsense", keep));
        CHECK(keep == std::wstring(L"UNTOUCHED"));

        NOTE("IsBroken is exactly 'Normalize refused it'");
        CHECK(HistoryPath::IsBroken(L"nonsense"));
        CHECK(!HistoryPath::IsBroken(L"D:\\Pictures"));

        NOTE("Clean trims but never validates - a broken line stays visible");
        // A row the panel shows as broken must still be shown. Swallowing it
        // looks like data loss and hides the mistake the user needs to see.
        CHECK(HistoryPath::Clean(L"  D:\\Pic<ures  ") == std::wstring(L"D:\\Pic<ures"));

        NOTE("case-insensitive equality, so one folder is one entry");
        CHECK(HistoryPath::Equal(L"D:\\Pics", L"d:\\pics"));
        CHECK(!HistoryPath::Equal(L"D:\\Pics", L"D:\\Pics2"));

        NOTE("the hash AGREES with that equality, or the sets break");
        // FolderPathSet uses both. A hash that disagreed would put two spellings
        // of one folder in different buckets and silently duplicate entries.
        HistoryPath::HashCI h;
        CHECK(h(L"D:\\Pics") == h(L"d:\\PICS"));
    }

    void TestUtf8() {
        using namespace Common::Utf8;

        NOTE("ASCII survives unchanged, so old files read back identically");
        // Every file written before this change is ASCII - non-ASCII could not be
        // written at all. Byte-identical round-tripping is what makes the switch
        // need no migration.
        const std::wstring ascii = L"D:/Pictures/Holiday 2026";
        CHECK(Encode(ascii) == std::string("D:/Pictures/Holiday 2026"));
        CHECK(Decode(Encode(ascii)) == ascii);

        NOTE("Cyrillic round-trips - the case that used to truncate the file");
        std::wstring cyr;
        cyr += L'D'; cyr += L':'; cyr += L'/';
        cyr += static_cast<wchar_t>(0x0421); // C
        cyr += static_cast<wchar_t>(0x043D); // n
        cyr += static_cast<wchar_t>(0x0438); // i
        cyr += static_cast<wchar_t>(0x043C); // m
        cyr += static_cast<wchar_t>(0x043A); // k
        cyr += static_cast<wchar_t>(0x0438); // i
        const std::string enc = Encode(cyr);
        CHECK(enc.size() == 3 + 12);        // 3 ASCII + 6 two-byte code points
        CHECK(Decode(enc) == cyr);

        NOTE("a character outside the BMP survives as its surrogate pair");
        // Four-byte UTF-8. wchar_t is 16 bits here, so this is two wchar_t in and
        // must come back as the same two - a converter that mishandled surrogates
        // would corrupt any path containing an emoji, which Windows allows.
        std::wstring astral;
        astral += static_cast<wchar_t>(0xD83D);
        astral += static_cast<wchar_t>(0xDCC1); // U+1F4C1 file folder
        CHECK(Encode(astral).size() == 4);
        CHECK(Decode(Encode(astral)) == astral);

        NOTE("empty in, empty out - never a partial result");
        CHECK(Encode(std::wstring()).empty());
        CHECK(Decode(std::string()).empty());

        NOTE("the BOM Notepad writes is stripped, and only at the front");
        std::string bom = "\xEF\xBB\xBF" "D:/x";
        StripBom(bom);
        CHECK(bom == std::string("D:/x"));

        std::string inner = "D:/x" "\xEF\xBB\xBF";   // not at the front: left alone
        StripBom(inner);
        CHECK(inner.size() == 7);

        std::string shortStr = "\xEF\xBB";            // too short to be a BOM
        StripBom(shortStr);
        CHECK(shortStr.size() == 2);

        NOTE("CR is dropped so a CRLF file does not yield unusable paths");
        // Reading in binary mode keeps the CR the CRT used to strip. Left in
        // place it becomes part of the path and every entry is rejected.
        std::wstring crlf = L"a\r\nb\r\n";
        StripCr(crlf);
        CHECK(crlf == std::wstring(L"a\nb\n"));

        std::wstring none = L"a\nb";
        StripCr(none);
        CHECK(none == std::wstring(L"a\nb"));

        std::wstring empty;
        StripCr(empty);
        CHECK(empty.empty());

        NOTE("ToCrlf is the way back out, and it is idempotent");
        // These files have always been CRLF on disk and must stay that way now
        // the writer is in binary mode. The guard against doubling matters
        // because no caller passes CRLF today and any caller could tomorrow -
        // and the corruption it would cause is silent.
        CHECK(ToCrlf(L"a\nb\n") == std::wstring(L"a\r\nb\r\n"));
        CHECK(ToCrlf(L"a\r\nb\r\n") == std::wstring(L"a\r\nb\r\n"));
        CHECK(ToCrlf(ToCrlf(L"a\nb")) == ToCrlf(L"a\nb"));
        CHECK(ToCrlf(L"no newlines here") == std::wstring(L"no newlines here"));
        CHECK(ToCrlf(std::wstring()).empty());

        NOTE("a lone CR is not touched, and does not swallow the next line");
        CHECK(ToCrlf(L"a\rb\n") == std::wstring(L"a\rb\r\n"));

        NOTE("StripCr then ToCrlf round-trips a CRLF file unchanged");
        std::wstring file = L"one\r\ntwo\r\n";
        StripCr(file);
        CHECK(file == std::wstring(L"one\ntwo\n"));
        CHECK(ToCrlf(file) == std::wstring(L"one\r\ntwo\r\n"));
    }

    void TestAuthFailPolicy() {
        using namespace Remote::AuthPolicy;
        constexpr int MAXF = 5;
        constexpr long long WINDOW = 10 * 60 * 1000;

        NOTE("failures below the threshold cost nothing");
        {
            FailRecord r;
            for (int i = 1; i < MAXF; ++i)
                CHECK(NoteFailure(r, 1000 + i, MAXF, WINDOW) == Response::Ignore);
        }

        NOTE("the FIRST crossing is temporary, not permanent");
        // The whole point of the v3 change: a mistyped password five times over
        // must not write a permanent, file-backed ban on a family phone.
        {
            FailRecord r;
            Response last = Response::Ignore;
            for (int i = 0; i < MAXF; ++i) last = NoteFailure(r, 1000 + i, MAXF, WINDOW);
            CHECK(last == Response::BlockTimed);
            CHECK(r.strikes == 1);
        }

        NOTE("the SECOND crossing is permanent");
        {
            FailRecord r;
            for (int i = 0; i < MAXF; ++i) NoteFailure(r, 1000 + i, MAXF, WINDOW);
            Response last = Response::Ignore;
            for (int i = 0; i < MAXF; ++i) last = NoteFailure(r, 2000 + i, MAXF, WINDOW);
            CHECK(last == Response::BlockPermanent);
            CHECK(r.strikes == 2);
        }

        NOTE("crossing RESETS the count, so the next attempt does not re-trigger");
        // Leaving the count at the threshold would make every later attempt
        // report a fresh block, and the second one would be permanent
        // immediately - collapsing the escalation to no escalation at all.
        {
            FailRecord r;
            for (int i = 0; i < MAXF; ++i) NoteFailure(r, 1000 + i, MAXF, WINDOW);
            CHECK(NoteFailure(r, 1100, MAXF, WINDOW) == Response::Ignore);
            CHECK(r.strikes == 1);
        }

        NOTE("a stale window is a NEW window - an occasional typo never blocks");
        {
            FailRecord r;
            for (int i = 0; i < 20; ++i)
                CHECK(NoteFailure(r, 1000 + i * (WINDOW + 1), MAXF, WINDOW) == Response::Ignore);
            CHECK(r.strikes == 0);
        }

        NOTE("STRIKES SURVIVE a window reset - waiting it out does not forgive");
        // An attacker who pauses between bursts must still reach the permanent
        // ban. If strikes reset with the window they would collect timed blocks
        // for ever and never earn the permanent one.
        {
            FailRecord r;
            for (int i = 0; i < MAXF; ++i) NoteFailure(r, 1000 + i, MAXF, WINDOW);
            CHECK(r.strikes == 1);

            const long long later = 1000 + WINDOW * 5;
            Response last = Response::Ignore;
            for (int i = 0; i < MAXF; ++i) last = NoteFailure(r, later + i, MAXF, WINDOW);
            CHECK(last == Response::BlockPermanent);
        }

        NOTE("lastSeenMs tracks every failure, so eviction picks the stalest");
        {
            FailRecord r;
            NoteFailure(r, 4242, MAXF, WINDOW);
            CHECK(r.lastSeenMs == 4242);
            NoteFailure(r, 9999, MAXF, WINDOW);
            CHECK(r.lastSeenMs == 9999);
        }
    }

    void TestAddressMatch() {
        using Remote::AddressMatches;
        using Remote::InList;

        NOTE("empty pattern matches nothing; '*' matches everything");
        // An empty entry must never be a wildcard. A blank line in a hand-edited
        // list would otherwise open the machine to the world.
        CHECK(!AddressMatches(L"", L"192.168.0.1"));
        CHECK(AddressMatches(L"*", L"192.168.0.1"));
        CHECK(AddressMatches(L"*", L"2001:db8::1"));

        NOTE("exact v4, and a near miss is a miss");
        CHECK(AddressMatches(L"192.168.0.10", L"192.168.0.10"));
        CHECK(!AddressMatches(L"192.168.0.10", L"192.168.0.11"));
        CHECK(!AddressMatches(L"192.168.0.10", L"192.168.0.100"));

        NOTE("v6 compares NUMERICALLY, so spelling cannot cause a lockout");
        // "2001:db8::1" and the fully expanded form are one host. A rule typed
        // the long way round matching nothing would lock somebody out silently.
        CHECK(AddressMatches(L"2001:0db8:0000:0000:0000:0000:0000:0001", L"2001:db8::1"));
        CHECK(AddressMatches(L"2001:db8::1", L"2001:0db8::1"));
        CHECK(!AddressMatches(L"2001:db8::1", L"2001:db8::2"));

        NOTE("a scope id on the peer does not defeat a link-local rule");
        // GetNameInfoW hands back "fe80::1%12" for a link-local peer; the %12 is
        // the interface, not the address.
        CHECK(AddressMatches(L"fe80::1", L"fe80::1%12"));

        NOTE("text prefix '*' keeps its documented sharp edge");
        // Compared as CHARACTERS. "192.168.1*" without the trailing dot also
        // matches 192.168.10.x — that is why /N exists beside it. Asserted so
        // nobody "fixes" it and silently narrows existing lists.
        CHECK(AddressMatches(L"192.168.1.*", L"192.168.1.55"));
        CHECK(!AddressMatches(L"192.168.1.*", L"192.168.2.55"));
        CHECK(AddressMatches(L"192.168.1*", L"192.168.10.5"));   // the sharp edge
        CHECK(AddressMatches(L"192.168.1*", L"192.168.100.5"));  // and again

        NOTE("v4 CIDR masks on bit boundaries and off them");
        CHECK(AddressMatches(L"192.168.0.0/24", L"192.168.0.1"));
        CHECK(AddressMatches(L"192.168.0.0/24", L"192.168.0.255"));
        CHECK(!AddressMatches(L"192.168.0.0/24", L"192.168.1.1"));
        CHECK(AddressMatches(L"10.0.0.0/8", L"10.255.255.255"));
        CHECK(!AddressMatches(L"10.0.0.0/8", L"11.0.0.1"));
        // /31 and /32 — the narrow end, where an off-by-one in the mask shows.
        CHECK(AddressMatches(L"192.168.0.10/32", L"192.168.0.10"));
        CHECK(!AddressMatches(L"192.168.0.10/32", L"192.168.0.11"));
        CHECK(AddressMatches(L"192.168.0.10/31", L"192.168.0.11"));
        CHECK(!AddressMatches(L"192.168.0.10/31", L"192.168.0.12"));

        NOTE("/0 is every address, and must not shift by 32 (UB)");
        // Shifting a 32-bit value by 32 is undefined, not zero. Handled as a
        // special case; this is the check that it still is.
        CHECK(AddressMatches(L"0.0.0.0/0", L"1.2.3.4"));
        CHECK(AddressMatches(L"0.0.0.0/0", L"255.255.255.255"));

        NOTE("v6 CIDR, including a prefix that is not a whole byte");
        CHECK(AddressMatches(L"2001:db8::/32", L"2001:db8:1234::1"));
        CHECK(!AddressMatches(L"2001:db8::/32", L"2001:db9::1"));
        CHECK(AddressMatches(L"2001:db8:abcd:1234::/64", L"2001:db8:abcd:1234::99"));
        CHECK(!AddressMatches(L"2001:db8:abcd:1234::/64", L"2001:db8:abcd:1235::1"));
        // /36 — four bits into the fifth byte, so the partial-byte mask is used.
        CHECK(AddressMatches(L"2001:db8:1000::/36", L"2001:db8:1fff::1"));
        CHECK(!AddressMatches(L"2001:db8:1000::/36", L"2001:db8:2000::1"));

        NOTE("FAMILIES MUST NOT CROSS — a v4 rule never admits a v6 peer");
        // The check that stops a /24 covering addresses that merely begin with
        // the same bytes in another notation.
        CHECK(!AddressMatches(L"192.168.0.0/24", L"2001:db8::1"));
        CHECK(!AddressMatches(L"2001:db8::/32", L"192.168.0.1"));
        CHECK(!AddressMatches(L"10.0.0.0/8", L"::ffff:10.0.0.1"));

        NOTE("ranges, both the full form and the last-octet shorthand");
        CHECK(AddressMatches(L"192.168.0.10-192.168.0.50", L"192.168.0.10"));
        CHECK(AddressMatches(L"192.168.0.10-192.168.0.50", L"192.168.0.50"));
        CHECK(!AddressMatches(L"192.168.0.10-192.168.0.50", L"192.168.0.51"));
        CHECK(!AddressMatches(L"192.168.0.10-192.168.0.50", L"192.168.0.9"));
        CHECK(AddressMatches(L"192.168.0.10-50", L"192.168.0.30"));
        CHECK(!AddressMatches(L"192.168.0.10-50", L"192.168.0.51"));

        NOTE("a backwards range is a typo and matches nothing");
        CHECK(!AddressMatches(L"192.168.0.50-192.168.0.10", L"192.168.0.30"));
        CHECK(!AddressMatches(L"192.168.0.50-10", L"192.168.0.30"));

        NOTE("malformed rules are refused, never treated as wildcards");
        // The dangerous failure would be a rule that cannot be parsed being
        // treated as "match anything". Each of these must match NOTHING.
        CHECK(!AddressMatches(L"192.168.0.0/33", L"192.168.0.1"));
        CHECK(!AddressMatches(L"2001:db8::/129", L"2001:db8::1"));
        CHECK(!AddressMatches(L"192.168.0.0/", L"192.168.0.1"));
        CHECK(!AddressMatches(L"/24", L"192.168.0.1"));
        CHECK(!AddressMatches(L"192.168.0.0/abc", L"192.168.0.1"));
        CHECK(!AddressMatches(L"not-an-address", L"192.168.0.1"));

        NOTE("an unparseable rule falls back to TEXT, which admits no real peer");
        // "192.168.0.256" is not an address, so neither ParseV4 nor ParseV6
        // accepts it and the comparison falls through to _wcsicmp — documented
        // behaviour, kept so entries that are not address literals go on
        // behaving as they always did.
        //
        // It is safe for a reason worth stating rather than assuming: a peer
        // address reaches the accept gate from GetNameInfoW with NI_NUMERICHOST,
        // so it is ALWAYS a valid numeric literal. A rule that can only match
        // itself as text therefore matches nothing that can ever connect.
        //
        // The first check below documents the fallback; the second is the one
        // that matters — it must not admit a real address.
        CHECK(AddressMatches(L"192.168.0.256", L"192.168.0.256"));   // text = text
        CHECK(!AddressMatches(L"192.168.0.256", L"192.168.0.25"));
        CHECK(!AddressMatches(L"192.168.0.256", L"192.168.0.1"));

        NOTE("InList is any-of, and an empty list admits nobody");
        // Empty means DENY EVERYONE, by design — the accept gate depends on it.
        const std::vector<std::wstring> empty;
        CHECK(!InList(empty, L"192.168.0.1"));

        const std::vector<std::wstring> list = {L"127.0.0.1", L"192.168.1.0/24", L"10.0.0.5-10"};
        CHECK(InList(list, L"127.0.0.1"));
        CHECK(InList(list, L"192.168.1.77"));
        CHECK(InList(list, L"10.0.0.7"));
        CHECK(!InList(list, L"192.168.2.1"));
        CHECK(!InList(list, L"10.0.0.11"));
        CHECK(!InList(list, L"8.8.8.8"));

        NOTE("one bad entry does not disable the good ones beside it");
        const std::vector<std::wstring> mixed = {L"garbage/99", L"192.168.1.0/24"};
        CHECK(InList(mixed, L"192.168.1.5"));
        CHECK(!InList(mixed, L"172.16.0.1"));
    }

    // ── LooksLikeAddress / BlockScope / SameHost ────────────────────────────

    void TestAddressHelpers() {
        using Remote::LooksLikeAddress;
        using Remote::BlockScope;
        using Remote::SameHost;

        NOTE("LooksLikeAddress accepts the real forms");
        CHECK(LooksLikeAddress(L"192.168.0.1"));
        CHECK(LooksLikeAddress(L"192.168.0.0/24"));
        CHECK(LooksLikeAddress(L"192.168.0.10-50"));
        CHECK(LooksLikeAddress(L"2001:db8::1"));
        CHECK(LooksLikeAddress(L"*"));

        NOTE("and drops entries that could never match — a typo is not a rule");
        CHECK(!LooksLikeAddress(L""));
        CHECK(!LooksLikeAddress(L"C:\\path\\thing"));
        CHECK(!LooksLikeAddress(L"192.168.0.1 and more"));
        CHECK(!LooksLikeAddress(L"192.168.0.0/99"));   // parses as text, not as CIDR
        CHECK(!LooksLikeAddress(L"10.0.0.5-1"));       // backwards range
        CHECK(!LooksLikeAddress(std::wstring(65, L'1')));  // over the length cap

        NOTE("BlockScope widens a v6 host to its /64");
        CHECK(BlockScope(L"2001:db8:abcd:1234::99") == L"2001:db8:abcd:1234::/64");

        NOTE("BlockScope leaves v4 ALONE — a /64 over v4 would be catastrophic");
        // An IPv4-mapped address's low bits carry a v4 address, so a /64 over one
        // reads as ::ffff:0:0/64 and would block the entire IPv4 internet from a
        // single wrong password.
        CHECK(BlockScope(L"192.168.0.1") == L"192.168.0.1");
        CHECK(BlockScope(L"::ffff:192.168.1.5") == L"::ffff:192.168.1.5");

        NOTE("BlockScope refuses to widen loopback and the all-zero prefix");
        CHECK(BlockScope(L"::1") == L"::1");

        NOTE("SameHost compares numerically within a family, never across one");
        CHECK(SameHost(L"192.168.0.1", L"192.168.0.1"));
        CHECK(SameHost(L"2001:db8::1", L"2001:0db8:0000:0000:0000:0000:0000:0001"));
        CHECK(!SameHost(L"192.168.0.1", L"192.168.0.2"));
        CHECK(!SameHost(L"192.168.0.1", L"::ffff:192.168.0.1"));  // different families
        CHECK(!SameHost(L"", L"192.168.0.1"));

        NOTE("two names compare case-insensitively, and nothing more");
        CHECK(SameHost(L"Monitor2", L"monitor2"));
        CHECK(!SameHost(L"monitor2", L"monitor2."));
    }

    // ── IniFile — what every persisted setting travels through ──────────────
    //
    // Every setting the application keeps is written and read back through these
    // functions, so a defect here is not one wrong value — it is any of them.
    // The failure modes are quiet: a key that fails to save looks exactly like a
    // key the user never set, and a mangled non-ASCII path looks like a file
    // that has gone missing.
    //
    // Runs in a temp directory and deletes it afterwards, like the log tests.

    void TestIniFile() {
        namespace Ini = Persistence::Ini;

        NOTE("ParseBool accepts every documented true form");
        // 1/true/on/yes, any non-zero number, case and whitespace ignored.
        CHECK(Ini::ParseBool(L"1", false));
        CHECK(Ini::ParseBool(L"true", false));
        CHECK(Ini::ParseBool(L"TRUE", false));
        CHECK(Ini::ParseBool(L"  yes  ", false));
        CHECK(Ini::ParseBool(L"On", false));
        CHECK(Ini::ParseBool(L"42", false));

        NOTE("and every documented false form");
        CHECK(!Ini::ParseBool(L"0", true));
        CHECK(!Ini::ParseBool(L"false", true));
        CHECK(!Ini::ParseBool(L"OFF", true));
        CHECK(!Ini::ParseBool(L" no ", true));

        NOTE("a typo falls back to the DEFAULT, never silently flipping a setting");
        // The property that matters: garbage must not read as either value by
        // accident. Both defaults are checked, because a rule that always
        // returned false would pass a one-sided test.
        CHECK(Ini::ParseBool(L"tru", true));
        CHECK(!Ini::ParseBool(L"tru", false));
        CHECK(Ini::ParseBool(L"", true));
        CHECK(!Ini::ParseBool(L"", false));
        CHECK(Ini::ParseBool(L"maybe", true));
        CHECK(!Ini::ParseBool(L"maybe", false));

        const std::wstring dir  = MakeTempDir();
        const std::wstring path = dir + L"\\test.ini";

        NOTE("reading a file that does not exist yields empty, not a crash");
        CHECK(Ini::ReadString(path, L"Sec", L"Key").empty());
        CHECK(!Ini::Exists(path));

        NOTE("a value written comes back identical");
        Ini::WriteString(path, L"Sec", L"Key", L"value", L"test");
        CHECK(Ini::Exists(path));
        CHECK(Ini::ReadString(path, L"Sec", L"Key") == L"value");

        NOTE("NON-ASCII survives the round trip — the UTF-16 BOM doing its job");
        // WritePrivateProfileStringW only writes Unicode into a file that is
        // ALREADY Unicode. Without the BOM these files are created with, every
        // value is narrowed to ANSI and any non-ASCII path is mangled on the way
        // in — which would show up as a folder that "went missing".
        Ini::WriteString(path, L"Sec", L"Path", L"C:\\Фото\\日本\\naïve", L"test");
        CHECK(Ini::ReadString(path, L"Sec", L"Path") == L"C:\\Фото\\日本\\naïve");

        NOTE("writing one key leaves every other key and section alone");
        Ini::WriteString(path, L"Other", L"Untouched", L"keepme", L"test");
        Ini::WriteString(path, L"Sec", L"Key", L"changed", L"test");
        CHECK(Ini::ReadString(path, L"Sec", L"Key") == L"changed");
        CHECK(Ini::ReadString(path, L"Other", L"Untouched") == L"keepme");
        CHECK(Ini::ReadString(path, L"Sec", L"Path") == L"C:\\Фото\\日本\\naïve");

        NOTE("an EMPTY value removes the key rather than storing a blank");
        // Relied on directly by Session::MarkRunning, which clears the
        // crash-detection mark on a clean exit by writing "". If this stored an
        // empty string instead of removing the key, every launch after the first
        // would report a crash that never happened.
        Ini::WriteString(path, L"Sec", L"Key", L"", L"test");
        CHECK(Ini::ReadString(path, L"Sec", L"Key").empty());
        CHECK(Ini::ReadString(path, L"Other", L"Untouched") == L"keepme");

        NOTE("DWORDs are stored as text, and garbage falls back to the default");
        Ini::WriteDword(path, L"Sec", L"Num", 4242, L"test");
        CHECK(Ini::ReadDword(path, L"Sec", L"Num", 7) == 4242);
        CHECK(Ini::ReadString(path, L"Sec", L"Num") == L"4242");   // hand-editable
        CHECK(Ini::ReadDword(path, L"Sec", L"Missing", 7) == 7);
        Ini::WriteString(path, L"Sec", L"Junk", L"not-a-number", L"test");
        CHECK(Ini::ReadDword(path, L"Sec", L"Junk", 7) == 7);

        NOTE("zero is a legitimate value, distinguishable from absent");
        // The reason these are stored as text at all — GetPrivateProfileInt
        // cannot tell "key present and 0" from "key missing".
        Ini::WriteDword(path, L"Sec", L"Zero", 0, L"test");
        CHECK(Ini::ReadDword(path, L"Sec", L"Zero", 7) == 0);

        NOTE("CreateWithHeaderIfMissing does not clobber an existing file");
        Ini::CreateWithHeaderIfMissing(path, L"a different header");
        CHECK(Ini::ReadDword(path, L"Sec", L"Num", 7) == 4242);

        NOTE("PathBesideExe returns an absolute path ending in the name given");
        // A bare name handed to WritePrivateProfileStringW writes into the
        // Windows directory rather than failing, so this must never be relative.
        {
            const std::wstring p = Ini::PathBesideExe(L"qivTestProbe.ini");
            CHECK(!p.empty());
            CHECK(p.find(L'\\') != std::wstring::npos);
            CHECK(p.size() > wcslen(L"qivTestProbe.ini"));
            CHECK(p.substr(p.size() - wcslen(L"qivTestProbe.ini")) == L"qivTestProbe.ini");
        }

        RemoveTempDir(dir);
    }

    // ── Crypto — the password hash and the handshake secret ─────────────────
    //
    // If this is wrong, AUTHENTICATION is wrong, and it fails silently in the
    // dangerous direction: a verifier that accepts too much produces no error.
    //
    // The property that matters most is the last group — the server derives the
    // shared secret from the STORED value, the client derives it from the
    // PLAINTEXT plus the salt and iteration count sent in the challenge, and the
    // two must agree exactly. If they ever diverge, every client is locked out;
    // if the derivation were weakened to make them agree, everyone gets in.

    void TestCrypto() {
        namespace C = Remote::Crypto;

        NOTE("SHA-256 against the published vector for the empty input");
        // A known-answer test, not a round trip: a round trip agrees with itself
        // even when both halves are wrong.
        CHECK(C::ToHex(C::Sha256("", 0)) ==
              L"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

        NOTE("and for \"abc\"");
        CHECK(C::ToHex(C::Sha256("abc", 3)) ==
              L"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

        NOTE("HMAC-SHA256 against RFC 4231 test case 2");
        // key = "Jefe", data = "what do ya want for nothing?"
        {
            const std::vector<BYTE> key = {'J', 'e', 'f', 'e'};
            const char *msg = "what do ya want for nothing?";
            CHECK(C::ToHex(C::HmacSha256(key, msg, strlen(msg))) ==
                  L"5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
        }

        NOTE("hex round trip, including case-insensitive input");
        {
            const std::vector<BYTE> raw = {0x00, 0x0F, 0xA5, 0xFF, 0x10};
            CHECK(C::ToHex(raw) == L"000fa5ff10");
            CHECK(C::FromHex(L"000fa5ff10") == raw);
            CHECK(C::FromHex(L"000FA5FF10") == raw);
        }

        NOTE("RandomBytes returns the length asked for, and is not constant");
        {
            const auto a = C::RandomBytes(16);
            const auto b = C::RandomBytes(16);
            CHECK(a.size() == 16);
            CHECK(b.size() == 16);
            // Two draws colliding would be a 1-in-2^128 event, so this failing
            // means the generator is stuck rather than unlucky.
            CHECK(a != b);
        }

        NOTE("a hashed password verifies, and a wrong one does not");
        const std::wstring stored = C::HashPassword(L"correct horse battery staple");
        CHECK(!stored.empty());
        CHECK(C::VerifyPassword(L"correct horse battery staple", stored));
        CHECK(!C::VerifyPassword(L"Correct horse battery staple", stored)); // case
        CHECK(!C::VerifyPassword(L"", stored));
        CHECK(!C::VerifyPassword(L"correct horse battery stapl", stored));

        NOTE("the SALT is per-password, so two hashes of one password differ");
        // Without this a precomputed table works against every instance
        // configured with the same password.
        const std::wstring again = C::HashPassword(L"correct horse battery staple");
        CHECK(again != stored);
        CHECK(C::VerifyPassword(L"correct horse battery staple", again));

        NOTE("a malformed stored value is refused, never accepted");
        // The dangerous direction: a value that cannot be parsed must not verify.
        CHECK(!C::StoredIsUsable(L""));
        CHECK(!C::StoredIsUsable(L"not-a-hash"));
        CHECK(!C::StoredIsUsable(L"210000$onlytwo"));
        CHECK(!C::StoredIsUsable(L"$$"));
        CHECK(!C::VerifyPassword(L"anything", L""));
        CHECK(!C::VerifyPassword(L"anything", L"not-a-hash"));
        CHECK(!C::VerifyPassword(L"anything", L"$$"));
        CHECK(C::StoredIsUsable(stored));

        NOTE("the stored value carries its own parameters back out");
        CHECK(C::IterationsFromStored(stored) > 0);
        CHECK(C::IterationsFromStored(L"garbage") == 0);
        CHECK(!C::SaltFromStored(stored).empty());
        CHECK(!C::SecretFromStored(stored).empty());

        NOTE("BOTH ENDS DERIVE THE SAME SECRET BY DIFFERENT ROUTES");
        // The server has the stored value and no plaintext; the client has the
        // plaintext plus the salt and iteration count from the challenge. These
        // must agree exactly or the handshake cannot succeed for anybody.
        {
            const auto serverSide = C::SecretFromStored(stored);
            const auto clientSide = C::SecretFromPassword(L"correct horse battery staple",
                                                          C::SaltFromStored(stored),
                                                          C::IterationsFromStored(stored));
            CHECK(!serverSide.empty());
            CHECK(serverSide == clientSide);

            // And a wrong password must NOT arrive at the same secret.
            const auto wrong = C::SecretFromPassword(L"wrong password",
                                                     C::SaltFromStored(stored),
                                                     C::IterationsFromStored(stored));
            CHECK(wrong != serverSide);
        }

        NOTE("the wrong salt or iteration count also fails to reproduce it");
        // Both travel in the clear in the challenge; neither is secret, but both
        // are load-bearing — a client fed the wrong one must not still match.
        {
            const auto serverSide = C::SecretFromStored(stored);
            const auto wrongSalt  = C::SecretFromPassword(L"correct horse battery staple",
                                                          C::RandomBytes(16),
                                                          C::IterationsFromStored(stored));
            CHECK(wrongSalt != serverSide);

            const auto wrongIters = C::SecretFromPassword(L"correct horse battery staple",
                                                          C::SaltFromStored(stored),
                                                          C::IterationsFromStored(stored) + 1);
            CHECK(wrongIters != serverSide);
        }
    }

    // =========================================================================
    // Persisted setting names
    //
    // Every value qIV stores must be spelled "qiv...". The prefix is what keeps
    // its values identifiable inside HKCU\Software\QuickImageViewer and inside
    // the [Settings] section of a portable copy's .ini — an unprefixed one is
    // invisible to any sweep that goes looking for them.
    //
    // This is not hypothetical tidiness. "LastFolder" shipped without the
    // prefix, outlived the code that wrote it, and was then read back in an
    // exported .reg and taken for live state. One missing prefix cost a real
    // misdiagnosis.
    //
    // Checked against the DECLARED STRING, not the C++ identifier: the
    // identifier is always upper-case, and it is the string that reaches the
    // registry.
    // =========================================================================
    void TestSettingNamesArePrefixed() {
        const std::string constants = ReadSourceFile("src/Platform/Constants.h");
        CHECK(!constants.empty());
        if (constants.empty()) return;

        // Only the Registry namespace. The rest of Constants.h is full of
        // strings that are not value names at all.
        const size_t nsStart = constants.find("namespace Registry {");
        NOTE("the Registry namespace is where value names are declared");
        CHECK(nsStart != std::string::npos);
        if (nsStart == std::string::npos) return;

        // ...and the scan has to STOP at its closing brace. What follows in
        // Constants.h is Session state, backup file naming and font defaults —
        // strings that are not value names and are deliberately unprefixed.
        // Reading past the brace reports every one of them as a violation.
        //
        // Matched by counting braces, skipping the three things that can hold a
        // brace without opening a scope: strings, // comments, /* */ comments.
        const size_t nsOpen = constants.find('{', nsStart);
        if (nsOpen == std::string::npos) return;

        size_t nsEnd = std::string::npos;
        int    depth = 0;
        for (size_t i = nsOpen; i < constants.size(); ++i) {
            const char c = constants[i];

            if (c == '/' && i + 1 < constants.size() && constants[i + 1] == '/') {
                i = constants.find('\n', i);
                if (i == std::string::npos) break;
                continue;
            }
            if (c == '/' && i + 1 < constants.size() && constants[i + 1] == '*') {
                i = constants.find("*/", i + 2);
                if (i == std::string::npos) break;
                ++i; // land on the '/', the loop's ++i steps past it
                continue;
            }
            if (c == '"') {
                ++i;
                while (i < constants.size() && constants[i] != '"') {
                    if (constants[i] == '\\') ++i; // \" does not close the string
                    ++i;
                }
                continue;
            }

            if (c == '{') {
                ++depth;
            } else if (c == '}') {
                --depth;
                if (depth == 0) { nsEnd = i; break; }
            }
        }

        NOTE("the Registry namespace closes, so the scan has an end");
        CHECK(nsEnd != std::string::npos);
        if (nsEnd == std::string::npos) return;

        // Registry KEY PATHS and the Run entry live here too and are exempt:
        // they name locations Windows defines, or — for the startup entry — the
        // text the user sees in Task Manager, which must read "QuickImageViewer"
        // rather than an internal prefix.
        const char *exempt[] = {
            "ROOT_KEY", "OPEN_WITH_COMMAND", "OPEN_WITH_ROOT", "OPEN_WITH_TYPES",
            "RUN_KEY", "RUN_VALUE_NAME"
        };

        // The prefix comes from the CONSTANT, not from a literal repeated here.
        // If it ever changes, this test follows it instead of failing the whole
        // codebase for disagreeing with a hard-coded "qiv".
        std::string prefix;
        for (const wchar_t *p = Constants::Registry::VALUE_PREFIX; *p; ++p)
            prefix.push_back(static_cast<char>(*p)); // ASCII by construction

        NOTE("the prefix constant is a non-empty ASCII string");
        CHECK(!prefix.empty());

        int checked = 0;
        int unprefixed = 0;

        size_t pos = nsStart;
        const std::string marker = "constexpr const wchar_t *";
        while ((pos = constants.find(marker, pos)) != std::string::npos && pos < nsEnd) {
            size_t p = pos + marker.size();
            while (p < constants.size() && std::isspace(static_cast<unsigned char>(constants[p]))) ++p;

            size_t nameStart = p;
            while (p < constants.size() &&
                   (std::isalnum(static_cast<unsigned char>(constants[p])) || constants[p] == '_'))
                ++p;
            const std::string name = constants.substr(nameStart, p - nameStart);

            // Value must be a plain L"..." on the same declaration.
            const size_t quote = constants.find(L'"', p);
            const size_t lineEnd = constants.find('\n', p);
            if (quote == std::string::npos || (lineEnd != std::string::npos && quote > lineEnd)) {
                pos = p;
                continue;
            }
            const size_t close = constants.find('"', quote + 1);
            if (close == std::string::npos) break;
            const std::string value = constants.substr(quote + 1, close - quote - 1);
            pos = close + 1;

            // Arrays of extensions and similar are not single value names.
            if (name.empty() || value.empty()) continue;

            bool isExempt = false;
            for (const char *e : exempt)
                if (name == e) { isExempt = true; break; }
            if (isExempt) continue;

            // A path, not a value name.
            if (value.find('\\') != std::string::npos) continue;

            // VALUE_PREFIX itself is the rule, not an instance of it.
            if (name == "VALUE_PREFIX") continue;

            ++checked;
            if (value.rfind(prefix, 0) != 0) {
                ++unprefixed;
                std::printf("      not prefixed: %s = \"%s\"\n", name.c_str(), value.c_str());
            }
        }

        NOTE("the Registry namespace parsed and yielded value names");
        CHECK(checked > 40);

        NOTE("every persisted value name starts with the prefix constant");
        CHECK(unprefixed == 0);
    }

    // -------------------------------------------------------------------------
    // A setting is stored under the SAME name whichever store it lands in.
    //
    // RegistryManager's four accessors each branch on Dedicated::SettingsUseFile
    // and then have to name the value. If one branch ever names it differently,
    // nothing breaks loudly: the app writes to one name and reads the other, and
    // the setting simply stops surviving a restart for half the users — the half
    // running whichever mode nobody tested.
    //
    // Both branches must therefore derive the name from the SAME `valueName`
    // parameter. The registry branch wraps it in PrefixedName(), which adds the
    // dedicated-mode prefix so a dedicated copy and a normal one can share one
    // hive without colliding. The FILE branch deliberately does not: each copy
    // already has its own .ini, so there is nothing to isolate it from.
    // -------------------------------------------------------------------------
    void TestSettingNamesMatchAcrossStores() {
        const std::string src = ReadSourceFile("src/Persistence/RegistryManager.cpp");
        CHECK(!src.empty());
        if (src.empty()) return;

        struct Accessor {
            const char *signature;   // where the function starts
            const char *fileCall;    // what the file branch must call it with
            const char *registryCall;// what the registry branch must call it with
        };
        const Accessor accessors[] = {
            {"void SaveSetting(",          "Dedicated::WriteDword(valueName",  "PrefixedName(valueName)"},
            {"DWORD LoadSetting(",         "Dedicated::ReadDword(valueName",   "PrefixedName(valueName)"},
            {"void SaveStringSetting(",    "Dedicated::WriteString(valueName", "PrefixedName(valueName)"},
            {"std::wstring LoadStringSetting(", "Dedicated::ReadString(valueName", "PrefixedName(valueName)"},
        };

        for (const Accessor &a : accessors) {
            const size_t start = src.find(a.signature);

            NOTE("the accessor is still where this test expects it");
            CHECK(start != std::string::npos);
            if (start == std::string::npos) continue;

            // Bounded to roughly one function so a match cannot be borrowed
            // from the next accessor down.
            const std::string body = src.substr(start, 1200);

            NOTE("the file branch names the value with the caller's valueName");
            const bool fileOk = body.find(a.fileCall) != std::string::npos;
            if (!fileOk) std::printf("      missing in %s: %s\n", a.signature, a.fileCall);
            CHECK(fileOk);

            NOTE("the registry branch names it with PrefixedName(valueName)");
            const bool regOk = body.find(a.registryCall) != std::string::npos;
            if (!regOk) std::printf("      missing in %s: %s\n", a.signature, a.registryCall);
            CHECK(regOk);
        }

        NOTE("PrefixedName adds only the dedicated prefix, nothing else");
        // If this grew a second transformation the two stores would diverge
        // again, which is precisely what the checks above are guarding.
        const size_t pn = src.find("PrefixedName(const wchar_t *valueName)");
        CHECK(pn != std::string::npos);
        if (pn != std::string::npos) {
            const std::string body = src.substr(pn, 400);
            CHECK(body.find("DEDICATED_MODE_GLOBAL_PREFIX") != std::string::npos);
            CHECK(body.find("return valueName;") != std::string::npos);
        }
    }

    // =========================================================================
    // Context menu ids
    //
    // WHY THIS EXISTS. Menu command ids are hand-assigned integers in
    // AppMenuIds.h, and a duplicate is silent: both items build, both appear,
    // and clicking one runs the other's code. It has already happened once —
    // the header still carries the comment "Moved off 67, which the overlay
    // 'Off' band also claimed".
    //
    // The trap is that the id space is not a flat list of scalars. Three of its
    // regions are RANGES that occupy ids no line in the file ever spells out:
    //
    //   * SET_SORT_FIRST(43) .. SET_SORT_LAST(47)   — 44, 45, 46 are taken
    //   * the overlay band, three runs of nine plus three layout modes
    //   * the font-family band, one id per entry in OVERLAY_FONT_FAMILIES
    //
    // So "grep for the number" is not a check. Reading the file and picking an
    // unused-LOOKING value is how 44 or 45 gets chosen and sorting quietly
    // breaks.
    //
    // The header is PARSED rather than mirrored here. A copied list would agree
    // with itself forever while the header drifted, which is the failure this
    // is meant to catch.
    // =========================================================================

    // One id, and where it came from — the name is what a failure has to print,
    // because "200 is used twice" is useless without both culprits.
    struct MenuId {
        std::string name;
        int         value = 0;
    };

    // Values are written as a literal, or as NAME + literal. Resolved in
    // repeated passes so a definition may refer to one that appears later; a
    // pass that resolves nothing means whatever is left is a form this parser
    // does not understand, which is reported rather than skipped.
    bool ParseMenuIds(const std::string &src, std::vector<MenuId> &out,
                      std::vector<std::string> &unresolved) {
        struct Pending { std::string name, expr; };
        std::vector<Pending> pending;

        size_t pos = 0;
        while (true) {
            const size_t eq = src.find('=', pos);
            if (eq == std::string::npos) break;

            // Not an assignment: ==, !=, <=, >=. The header is full of
            // static_assert(A == B), and reading those as definitions is how a
            // parser invents unresolvable names and fails on a healthy file.
            if (eq + 1 < src.size() && src[eq + 1] == '=') { pos = eq + 2; continue; }
            if (eq > 0 && (src[eq - 1] == '=' || src[eq - 1] == '!' ||
                           src[eq - 1] == '<' || src[eq - 1] == '>')) {
                pos = eq + 1;
                continue;
            }

            // Anything on a static_assert line is a claim ABOUT ids, not a
            // definition of one. The == guard above already catches most of
            // them; this catches the rest without depending on their spelling.
            {
                const size_t lineStart = src.rfind('\n', eq);
                const size_t from = (lineStart == std::string::npos) ? 0 : lineStart + 1;
                const std::string line = src.substr(from, eq - from);
                if (line.find("static_assert") != std::string::npos) {
                    pos = eq + 1;
                    continue;
                }
            }

            // Walk back over the name.
            size_t nameEnd = eq;
            while (nameEnd > 0 && std::isspace(static_cast<unsigned char>(src[nameEnd - 1]))) --nameEnd;
            size_t nameStart = nameEnd;
            while (nameStart > 0 &&
                   (std::isalnum(static_cast<unsigned char>(src[nameStart - 1])) || src[nameStart - 1] == '_'))
                --nameStart;

            const std::string name = src.substr(nameStart, nameEnd - nameStart);

            // Value runs to the comma, semicolon or closing brace that ends it.
            size_t valEnd = eq + 1;
            while (valEnd < src.size() && src[valEnd] != ',' && src[valEnd] != ';' &&
                   src[valEnd] != '\n' && src[valEnd] != '}')
                ++valEnd;
            std::string expr = src.substr(eq + 1, valEnd - (eq + 1));

            // Strip a trailing comment.
            const size_t slash = expr.find("//");
            if (slash != std::string::npos) expr = expr.substr(0, slash);

            pos = valEnd + 1;

            if (name.rfind("SET_", 0) != 0 && name != "VIEWER_BASE" &&
                name != "OVERLAY_SLOT_COUNT" && name != "OVERLAY_BAND_FIRST" &&
                name != "OVERLAY_BAND_LAST")
                continue;

            pending.push_back({name, expr});
        }

        std::vector<MenuId> resolved;
        bool progress = true;
        while (progress && !pending.empty()) {
            progress = false;
            std::vector<Pending> stillPending;

            for (const Pending &p : pending) {
                // Trim.
                size_t a = 0, b = p.expr.size();
                while (a < b && std::isspace(static_cast<unsigned char>(p.expr[a]))) ++a;
                while (b > a && std::isspace(static_cast<unsigned char>(p.expr[b - 1]))) --b;
                const std::string e = p.expr.substr(a, b - a);
                if (e.empty()) continue;

                // Plain integer?
                if (std::isdigit(static_cast<unsigned char>(e[0]))) {
                    resolved.push_back({p.name, std::atoi(e.c_str())});
                    progress = true;
                    continue;
                }

                // NAME, or NAME + literal.
                size_t plus = e.find('+');
                std::string base = (plus == std::string::npos) ? e : e.substr(0, plus);
                int addend = 0;
                if (plus != std::string::npos) addend = std::atoi(e.c_str() + plus + 1);

                while (!base.empty() && std::isspace(static_cast<unsigned char>(base.back())))
                    base.pop_back();

                bool found = false;
                for (const MenuId &r : resolved) {
                    if (r.name == base) {
                        resolved.push_back({p.name, r.value + addend});
                        found = true;
                        progress = true;
                        break;
                    }
                }
                if (!found) stillPending.push_back(p);
            }
            pending.swap(stillPending);
        }

        for (const Pending &p : pending) unresolved.push_back(p.name);
        out.swap(resolved);
        return unresolved.empty();
    }

    int ValueOf(const std::vector<MenuId> &ids, const char *name) {
        for (const MenuId &m : ids)
            if (m.name == name) return m.value;
        return -1;
    }

    void TestMenuIdsUnique() {
        const std::string src = ReadSourceFile("src/UI/AppMenu/AppMenuIds.h");
        CHECK(!src.empty());
        if (src.empty()) return;

        std::vector<MenuId>     ids;
        std::vector<std::string> unresolved;
        const bool parsedAll = ParseMenuIds(src, ids, unresolved);

        NOTE("every enumerator in the header parses");
        // A value this test cannot evaluate is a value it cannot check, so an
        // unparsed one fails rather than passing quietly.
        if (!parsedAll) {
            for (const std::string &n : unresolved)
                std::printf("      unresolved: %s\n", n.c_str());
        }
        CHECK(parsedAll);
        CHECK(ids.size() > 40);

        // ── Build the occupancy map ─────────────────────────────────────────
        // Every id claimed by anything, with the name that claimed it. Ranges
        // are expanded, which is the whole point: the gaps inside them are not
        // free.
        struct Claim { int value; std::string owner; };
        std::vector<Claim> claims;

        const int sortFirst   = ValueOf(ids, "SET_SORT_FIRST");
        const int sortLast    = ValueOf(ids, "SET_SORT_LAST");
        const int bandFirst   = ValueOf(ids, "SET_OVERLAY_OFF_BASE");
        const int layoutFirst = ValueOf(ids, "SET_LAYOUT_GRID");
        const int fontBase    = ValueOf(ids, "SET_OVERLAY_FONT_FAMILY_BASE");

        NOTE("the header still declares the ranges this test expands");
        CHECK(sortFirst > 0 && sortLast >= sortFirst);
        CHECK(bandFirst > 0 && layoutFirst > bandFirst);
        CHECK(fontBase > 0);

        for (const MenuId &m : ids) {
            // The range endpoints and the band bases are accounted for by the
            // expansions below; counting them twice would report a collision
            // with themselves.
            if (m.name == "SET_SORT_FIRST" || m.name == "SET_SORT_LAST") continue;
            // A boundary ALIAS, not an item: SET_SCALAR_LAST is defined as
            // SET_LOCATION so the band asserts have something to compare
            // against. Counting it would report the id colliding with itself.
            if (m.name == "SET_SCALAR_LAST") continue;
            if (m.name.rfind("SET_OVERLAY_OFF_BASE", 0) == 0) continue;
            if (m.name.rfind("SET_OVERLAY_FULL_BASE", 0) == 0) continue;
            if (m.name.rfind("SET_OVERLAY_COMPACT_BASE", 0) == 0) continue;
            if (m.name.rfind("SET_LAYOUT_", 0) == 0) continue;
            if (m.name == "SET_OVERLAY_BASE") continue;
            if (m.name == "SET_OVERLAY_FONT_FAMILY_BASE") continue;
            if (m.name == "VIEWER_BASE" || m.name == "OVERLAY_SLOT_COUNT" ||
                m.name == "OVERLAY_BAND_FIRST" || m.name == "OVERLAY_BAND_LAST")
                continue;
            claims.push_back({m.value, m.name});
        }

        for (int v = sortFirst; v <= sortLast; ++v)
            claims.push_back({v, "sort order run (SET_SORT_FIRST..SET_SORT_LAST)"});

        const int slots = UI::AppMenu::Ids::OVERLAY_SLOT_COUNT;
        for (int v = bandFirst; v < bandFirst + slots * 3; ++v)
            claims.push_back({v, "overlay slot band (Off/Full/Compact runs)"});
        for (int v = layoutFirst; v <= layoutFirst + 2; ++v)
            claims.push_back({v, "overlay layout modes"});
        for (int v = fontBase;
             v < fontBase + static_cast<int>(Constants::Overlay::OVERLAY_FONT_FAMILY_COUNT); ++v)
            claims.push_back({v, "overlay font-family band"});

        // ── The check ───────────────────────────────────────────────────────
        NOTE("no menu id is claimed twice");
        int collisions = 0;
        for (size_t i = 0; i < claims.size(); ++i) {
            for (size_t j = i + 1; j < claims.size(); ++j) {
                if (claims[i].value != claims[j].value) continue;
                ++collisions;
                std::printf("      id %d claimed by %s AND %s\n",
                            claims[i].value, claims[i].owner.c_str(), claims[j].owner.c_str());
            }
        }
        CHECK(collisions == 0);

        NOTE("no settings id reaches into the viewer-command space");
        // Below VIEWER_BASE everything dispatches as a setting; at or above it,
        // as a viewer command. An id that crossed would run the wrong handler.
        const int viewerBase = ValueOf(ids, "VIEWER_BASE");
        CHECK(viewerBase > 0);
        int crossings = 0;
        for (const Claim &c : claims)
            if (c.value >= viewerBase) ++crossings;
        CHECK(crossings == 0);
    }

    // -------------------------------------------------------------------------
    // The band decode, exercised against the real constants.
    //
    // AppMenuSettings.cpp recovers a slot with (id - OFF_BASE) % 9 and the state
    // with (id - OFF_BASE) / 9. The header asserts the layout that makes that
    // correct; this walks every id the decoder will ever see and confirms it
    // comes back as the slot and state it went in as.
    // -------------------------------------------------------------------------
    void TestOverlayBandDecode() {
        namespace Id = UI::AppMenu::Ids;

        NOTE("every slot/state id decodes back to the slot and state it encodes");
        int wrong = 0;
        for (int band = 0; band < 3; ++band) {
            for (int slot = 0; slot < Id::OVERLAY_SLOT_COUNT; ++slot) {
                const int id = Id::SET_OVERLAY_OFF_BASE + band * Id::OVERLAY_SLOT_COUNT + slot;

                const int decodedBand = (id - Id::SET_OVERLAY_OFF_BASE) / Id::OVERLAY_SLOT_COUNT;
                const int decodedSlot = (id - Id::SET_OVERLAY_OFF_BASE) % Id::OVERLAY_SLOT_COUNT;

                if (decodedBand != band || decodedSlot != slot) ++wrong;
            }
        }
        CHECK(wrong == 0);

        NOTE("the slot runs stop before the layout ids begin");
        // The decoder tells "a slot" from "a layout" with one comparison, so a
        // gap or an overlap here misroutes every layout click into a slot.
        CHECK(Id::SET_LAYOUT_GRID ==
              Id::SET_OVERLAY_COMPACT_BASE + Id::OVERLAY_SLOT_COUNT);
        CHECK(Id::SET_LAYOUT_SUMMARY == Id::SET_LAYOUT_GRID + 2);

        NOTE("the declared band bounds match the ids actually in the band");
        CHECK(Id::OVERLAY_BAND_FIRST == Id::SET_OVERLAY_OFF_BASE);
        CHECK(Id::OVERLAY_BAND_LAST == Id::SET_LAYOUT_SUMMARY);

        NOTE("the overlay scalars sit OUTSIDE the arithmetic band");
        // They dispatch as ordinary cases. Inside the band they would be
        // decoded as a slot instead and never reach their own handler.
        CHECK(Id::SET_OVERLAY_EFFECTS_LIST > Id::OVERLAY_BAND_LAST);
        CHECK(Id::SET_OVERLAY_DIR_NAME     > Id::OVERLAY_BAND_LAST);
        CHECK(Id::SET_OVERLAY_FONT_SIZE    > Id::OVERLAY_BAND_LAST);
        CHECK(Id::SET_OVERLAY_FONT_COLOR   > Id::OVERLAY_BAND_LAST);
        CHECK(Id::SET_OVERLAY_FONT_FAMILY_BASE > Id::OVERLAY_BAND_LAST);
    }

    // -------------------------------------------------------------------------
    // Every menu item must actually do something.
    //
    // An item built with an id that no dispatch case handles looks completely
    // normal — it draws, it highlights, it closes the menu, and nothing happens.
    // Nothing in the compiler notices, because both halves are valid on their
    // own.
    // -------------------------------------------------------------------------
    void TestMenuItemsAreHandled() {
        const std::string builders = ReadSourceFile("src/UI/AppMenu/AppMenuBuilders.cpp");
        const std::string settings = ReadSourceFile("src/UI/AppMenu/AppMenuSettings.cpp");
        const std::string io       = ReadSourceFile("src/UI/AppMenu/AppMenuIO.cpp");
        const std::string menu     = ReadSourceFile("src/UI/AppMenu/AppMenu.cpp");

        CHECK(!builders.empty());
        CHECK(!settings.empty());
        if (builders.empty() || settings.empty()) return;

        const std::string handlers = settings + io + menu;

        // Collect Id::SET_* referenced by the builders.
        std::vector<std::string> used;
        size_t pos = 0;
        while ((pos = builders.find("Id::SET_", pos)) != std::string::npos) {
            size_t start = pos + 4; // past "Id::"
            size_t end = start;
            while (end < builders.size() &&
                   (std::isalnum(static_cast<unsigned char>(builders[end])) || builders[end] == '_'))
                ++end;
            const std::string name = builders.substr(start, end - start);

            bool already = false;
            for (const std::string &u : used)
                if (u == name) { already = true; break; }
            if (!already) used.push_back(name);
            pos = end;
        }

        NOTE("the builders were read and do reference menu ids");
        CHECK(used.size() > 20);

        NOTE("every id the menu builds is handled somewhere");
        int orphans = 0;
        for (const std::string &name : used) {
            // Band members are decoded arithmetically rather than by name, so a
            // literal "case Id::X" for them will never exist.
            if (name.rfind("SET_OVERLAY_OFF_BASE", 0) == 0) continue;
            if (name.rfind("SET_OVERLAY_FULL_BASE", 0) == 0) continue;
            if (name.rfind("SET_OVERLAY_COMPACT_BASE", 0) == 0) continue;
            if (name.rfind("SET_OVERLAY_FONT_FAMILY_BASE", 0) == 0) continue;
            if (name.rfind("SET_SORT_", 0) == 0) continue;
            // Layout ids sit inside the arithmetic band as well — they are the
            // three values immediately after the slot runs, and the decoder
            // reaches them by subtraction, not by a case label.
            if (name.rfind("SET_LAYOUT_", 0) == 0) continue;

            const std::string wanted = "case Id::" + name;
            if (handlers.find(wanted) == std::string::npos) {
                ++orphans;
                std::printf("      built but never handled: Id::%s\n", name.c_str());
            }
        }
        CHECK(orphans == 0);
    }

} // namespace

int main(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (a[0] == '-' && (a[1] == 'v' || (a[1] == '-' && a[2] == 'v')))
            g_verbose = true;
    }

    EnableColour();
    const auto runStart = std::chrono::steady_clock::now();

    std::printf("\n  %sqIV unit tests%s%s%s\n", Bold(), Off(),
                Dim(), g_verbose ? "   verbose" : "");
    // %ls — APP_NAME is a wide string and the rest of this line is narrow.
    std::printf("  %s%ls%s\n", Dim(), Constants::APP_NAME, Off());
    std::printf("\n");

    std::printf("  %sWIRE AND STORAGE%s\n", Dim(), Off());
    BeginGroup("Base64 - the wire format");
    TestBase64();
    EndGroup();

    BeginGroup("Converters - zoom storage round trip");
    TestConverters();
    EndGroup();

    std::printf("\n  %sFIND%s\n", Dim(), Off());
    BeginGroup("WildcardMatch - Find dialog rules");
    TestWildcard();
    EndGroup();

    BeginGroup("FuzzyMatch - Find subsequence scoring");
    TestFuzzy();
    EndGroup();

    std::printf("\n  %sPERSISTENCE%s\n", Dim(), Off());
    BeginGroup("IniFile - how every setting is stored");
    TestIniFile();
    EndGroup();

    std::printf("\n  %sLOGGING%s\n", Dim(), Off());
    BeginGroup("Log layout - the shared line shape");
    TestLogLayout();
    EndGroup();

    BeginGroup("Log files - rotation and continuation");
    TestLogRotation();
    EndGroup();

    BeginGroup("Wire log - save/load round trip");
    TestWireLogRoundTrip();
    EndGroup();

    BeginGroup("Setting names - the qiv prefix");
    TestSettingNamesArePrefixed();
    EndGroup();

    BeginGroup("Setting names - file and registry agree");
    TestSettingNamesMatchAcrossStores();
    EndGroup();

    std::printf("\n  %sCONTEXT MENU%s\n", Dim(), Off());
    BeginGroup("Menu ids - one id, one meaning");
    TestMenuIdsUnique();
    EndGroup();

    BeginGroup("Overlay band - the arithmetic decode");
    TestOverlayBandDecode();
    EndGroup();

    BeginGroup("Menu items - built and handled");
    TestMenuItemsAreHandled();
    EndGroup();

    std::printf("\n  %sSECURITY  %s— who may connect, and how they prove it%s\n",
                Dim(), Dim(), Off());
    BeginGroup("AllowList - who may connect");
    TestAddressMatch();
    EndGroup();

    BeginGroup("Brute-force guard - what a wrong password costs");
    TestAuthFailPolicy();
    EndGroup();

    BeginGroup("UTF-8 - the history files' encoding");
    TestUtf8();
    EndGroup();

    BeginGroup("HistoryPath - hygiene on every hand-edited line");
    TestHistoryPath();
    EndGroup();

    BeginGroup("Duplicates - what counts as the same picture");
    TestDuplicateFinder();
    EndGroup();

    BeginGroup("Preview strip - which thumbnail a click landed on");
    TestPreviewStrip();
    EndGroup();

    BeginGroup("FolderIndex - where a file name starts inside a path");
    TestNameOffset();
    EndGroup();

    BeginGroup("Source hygiene - no invalid escape sequences");
    TestNoInvalidEscapes();
    EndGroup();

    BeginGroup("AllowList - entry validation and scope");
    TestAddressHelpers();
    EndGroup();

    BeginGroup("Crypto - password hash and handshake");
    TestCrypto();
    EndGroup();

    // Prints timings, so it always breaks the column layout. Last, and after a
    // blank line, so the table above stays readable.
    std::printf("\n  %sBENCHMARKS%s\n", Dim(), Off());
    BeginGroup("Throughput", /*multiline=*/true);
    Benchmarks();
    EndGroup();

    const auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - runStart).count();

    std::printf("\n");
    Rule();
    if (g_failed == 0) {
        std::printf("  %s✓ ALL PASSED%s   %s%d checks · %d groups · %lldms%s\n",
                    Green(), Off(), Dim(), g_checks, g_groups, totalMs, Off());
    } else {
        std::printf("  %s✗ %d FAILED%s   %s%d checks · %d groups · %lldms%s\n",
                    Red(), g_failed, Off(), Dim(), g_checks, g_groups, totalMs, Off());
    }
    Rule();

    if (g_failed == 0 && !g_verbose)
        std::printf("  %sRun with -v to list every check by name.%s\n", Dim(), Off());
    std::printf("\n");

    // Non-zero on failure is what makes ctest and CI report this as a failed
    // test rather than a passing one that happened to print the word FAIL.
    return g_failed == 0 ? 0 : 1;
}
