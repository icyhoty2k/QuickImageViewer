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
// NOT COVERED, and worth saying out loud: the AllowList matcher in
// RemoteSettings, which is the single most security-relevant piece of pure logic
// in the codebase — it decides who may connect. It cannot be linked here because
// RemoteSettings.cpp pulls in AppState, IniFile, RemoteBlacklist and
// RemoteCrypto. Extracting the matcher into its own translation unit would make
// it testable, and it is the first thing worth doing after this.
//

#include "Common/Base64.h"
#include "Common/Converters.h"
#include "Common/FuzzyMatch.h"

#include <chrono>
#include <cmath>
#include <cstdio>
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

    void Report(bool ok, const char *expr, const char *file, int line) {
        ++g_checks;
        ++g_groupChecks;
        if (ok) {
            if (g_verbose) std::printf("      ok    %s\n", expr);
            return;
        }
        ++g_failed;
        ++g_groupFailed;
        std::printf("      FAIL  %s\n            %s:%d\n", expr, file, line);
        if (*g_note) std::printf("            while checking: %s\n", g_note);
    }

    void BeginGroup(const char *title) {
        ++g_groups;
        g_groupChecks = 0;
        g_groupFailed = 0;
        g_note        = "";
        std::printf("  %-46s", title);
        if (g_verbose) std::printf("\n");
    }

    void EndGroup() {
        if (g_verbose || g_groupFailed) std::printf("  %-46s", "");
        if (g_groupFailed)
            std::printf("%3d checks   %d FAILED\n", g_groupChecks, g_groupFailed);
        else
            std::printf("%3d checks   ok\n", g_groupChecks);
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

        NOTE("an empty query matches anything; empty text matches nothing but empty");
        CHECK(Common::FuzzyMatch(L"", 0, L"holiday", 7, r));
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

int main(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (a[0] == '-' && (a[1] == 'v' || (a[1] == '-' && a[2] == 'v')))
            g_verbose = true;
    }

    std::printf("\nqIV unit tests%s\n\n", g_verbose ? "  (verbose)" : "");

    BeginGroup("Base64 - the wire format");
    TestBase64();
    EndGroup();

    BeginGroup("Converters - zoom storage round trip");
    TestConverters();
    EndGroup();

    BeginGroup("WildcardMatch - Find dialog rules");
    TestWildcard();
    EndGroup();

    BeginGroup("FuzzyMatch - Find subsequence scoring");
    TestFuzzy();
    EndGroup();

    // Prints timings, so it always breaks the column layout. Last, and after a
    // blank line, so the table above stays readable.
    BeginGroup("Benchmarks");
    Benchmarks();
    EndGroup();

    std::printf("\n  %d checks in %d groups, %d failed\n\n", g_checks, g_groups, g_failed);

    if (g_failed == 0 && !g_verbose)
        std::printf("  Run with -v to list every check by name.\n\n");

    // Non-zero on failure is what makes ctest and CI report this as a failed
    // test rather than a passing one that happened to print the word FAIL.
    return g_failed == 0 ? 0 : 1;
}
