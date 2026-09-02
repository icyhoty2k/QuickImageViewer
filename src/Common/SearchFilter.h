// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#pragma once
#include <string>
#include <cstdint>

// NARROWING A SEARCH BY SIZE AND DATE.
//
// Typing in the Find panel matches file names. On a library of tens of
// thousands of pictures a name is often not enough - "the big ones from last
// summer" is a real question and a name cannot answer it.
//
// PARSED HERE, PURE, so the rules are reachable from a test. What the panel
// does with the answer is the panel's business; whether ">5mb" means what a
// person expects is decided in this file.
//
// ⚠ THE OVERWHELMING CONSTRAINT IS NOT BREAKING PLAIN TEXT. People already
// type "photo_2024" and "IMG_20240817" into this box, and those must go on
// matching names exactly as before. So a token becomes a filter ONLY when it
// is unambiguous:
//
//     >5mb  <500kb  >=2gb        a comparison, digits, and a SIZE UNIT
//     >2024  <2024-06  >=2024-06-01   a comparison and a DATE SHAPE
//
// A bare four-digit year counts only inside 1900-2999. ">1000" is four digits
// too, and is far likelier to be a size with the unit left off.
//
// Anything else stays text. A bare 2024 is a name fragment, not a year; a bare
// >1000 is text too, because "digits after an operator" alone cannot say
// whether bytes or a year was meant. Refusing to guess is what keeps the box
// predictable.
namespace Common::SearchFilter {

    enum class Op { None, Less, LessEqual, Greater, GreaterEqual };

    struct Filter {
        Op                 sizeOp    = Op::None;
        unsigned long long sizeBytes = 0;

        // Days since 1970-01-01, so the comparison is integer and the parsing
        // is testable without a clock. The caller converts a file time once.
        Op      dateOp   = Op::None;
        long long dateDay = 0;

        // What is left after the filter tokens are removed - the text that
        // still has to match a file name. Empty means "every file that passes
        // the filters", which is the useful reading of a query that is nothing
        // but filters.
        std::wstring text;

        [[nodiscard]] bool Any() const { return sizeOp != Op::None || dateOp != Op::None; }
    };

    namespace detail {

        // Days from the civil date, Howard Hinnant's algorithm. Correct for any
        // proleptic Gregorian date and needs no clock, which is what makes the
        // date rules testable.
        inline long long DaysFromCivil(long long y, unsigned m, unsigned d) {
            y -= m <= 2;
            const long long era = (y >= 0 ? y : y - 399) / 400;
            const unsigned yoe = static_cast<unsigned>(y - era * 400);
            const unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2u) / 5u + d - 1u;
            const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
            return era * 146097LL + static_cast<long long>(doe) - 719468LL;
        }

        inline bool IsDigit(wchar_t c) { return c >= L'0' && c <= L'9'; }

        // The last day of a month, so ">2024-02" can mean "after February".
        inline unsigned LastDayOf(long long y, unsigned m) {
            static const unsigned len[] = {31,28,31,30,31,30,31,31,30,31,30,31};
            if (m < 1 || m > 12) return 31;
            if (m == 2) {
                const bool leap = (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
                return leap ? 29u : 28u;
            }
            return len[m - 1];
        }

    } // namespace detail

    // Parses one token. Returns true when it IS a filter and fills `f`.
    //
    // ⚠ AN OPERATOR IS REQUIRED. Without one there is no way to tell a filter
    // from a name, and guessing wrong silently hides files the user asked for.
    inline bool ParseToken(const std::wstring &tok, Filter &f) {
        if (tok.size() < 2) return false;

        size_t i = 0;
        Op op = Op::None;
        if (tok[0] == L'>') { op = Op::Greater; i = 1; }
        else if (tok[0] == L'<') { op = Op::Less; i = 1; }
        else return false;
        if (i < tok.size() && tok[i] == L'=') {
            op = (op == Op::Greater) ? Op::GreaterEqual : Op::LessEqual;
            ++i;
        }
        if (i >= tok.size()) return false;

        // --- digits ---------------------------------------------------------
        size_t d0 = i;
        while (i < tok.size() && detail::IsDigit(tok[i])) ++i;
        if (i == d0) return false;                       // an operator and no number
        const std::wstring digits = tok.substr(d0, i - d0);
        if (digits.size() > 18) return false;            // absurd; treat as text

        unsigned long long n = 0;
        for (wchar_t c : digits) n = n * 10u + static_cast<unsigned>(c - L'0');

        // --- a size unit? ---------------------------------------------------
        std::wstring rest = tok.substr(i);
        for (wchar_t &c : rest) if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');

        unsigned long long mult = 0;
        if      (rest == L"b")                     mult = 1ULL;
        else if (rest == L"k" || rest == L"kb")    mult = 1024ULL;
        else if (rest == L"m" || rest == L"mb")    mult = 1024ULL * 1024ULL;
        else if (rest == L"g" || rest == L"gb")    mult = 1024ULL * 1024ULL * 1024ULL;

        if (mult) {
            f.sizeOp = op;
            f.sizeBytes = n * mult;
            return true;
        }

        // --- a date shape? --------------------------------------------------
        // yyyy, yyyy-mm or yyyy-mm-dd, and nothing else. Four digits is the
        // giveaway: a size without a unit never reaches here.
        // ⚠ FOUR DIGITS IS NOT ENOUGH ON ITS OWN. ">1000" is four digits and a
        // plausible year, and it is far more likely to be somebody reaching for
        // a size and forgetting the unit. There is no way to tell, so the
        // window is narrowed to years a photograph can actually carry - and
        // anything outside it stays text, which is the safe reading.
        //
        // Caught by the test that asserts ">1000" is refused: it was parsed as
        // the year 1000 on the first run.
        if (digits.size() != 4) return false;
        const long long year = static_cast<long long>(n);
        if (year < 1900 || year > 2999) return false;

        unsigned month = 0, day = 0;
        if (!rest.empty()) {
            // -mm or -mm-dd
            if (rest[0] != L'-') return false;
            size_t p = 1;
            size_t m0 = p;
            while (p < rest.size() && detail::IsDigit(rest[p])) ++p;
            if (p - m0 < 1 || p - m0 > 2) return false;
            month = 0;
            for (size_t q = m0; q < p; ++q) month = month * 10u + static_cast<unsigned>(rest[q] - L'0');
            if (month < 1 || month > 12) return false;

            if (p < rest.size()) {
                if (rest[p] != L'-') return false;
                ++p;
                size_t dd0 = p;
                while (p < rest.size() && detail::IsDigit(rest[p])) ++p;
                if (p != rest.size() || p - dd0 < 1 || p - dd0 > 2) return false;
                day = 0;
                for (size_t q = dd0; q < p; ++q) day = day * 10u + static_cast<unsigned>(rest[q] - L'0');
                if (day < 1 || day > detail::LastDayOf(year, month)) return false;
            }
        }

        // ⚠ A PARTIAL DATE IS A RANGE, NOT A POINT, and the operator decides
        // which end of it to use. ">2024" has to mean "after 2024 ended", or a
        // picture from December 2024 would match "newer than 2024". "<2024"
        // means "before 2024 began" for the same reason.
        const bool upper = (op == Op::Greater) || (op == Op::LessEqual);
        const unsigned m = month ? month : (upper ? 12u : 1u);
        const unsigned d = day   ? day   : (upper ? detail::LastDayOf(year, m) : 1u);

        f.dateOp  = op;
        f.dateDay = detail::DaysFromCivil(year, m, d);
        return true;
    }

    // Splits a query into filter tokens and the text that remains.
    inline Filter Parse(const std::wstring &query) {
        Filter f;
        std::wstring text;
        size_t i = 0;
        while (i < query.size()) {
            while (i < query.size() && query[i] == L' ') ++i;
            const size_t start = i;
            while (i < query.size() && query[i] != L' ') ++i;
            if (i == start) break;
            const std::wstring tok = query.substr(start, i - start);

            if (!ParseToken(tok, f)) {
                if (!text.empty()) text += L' ';
                text += tok;
            }
        }
        f.text = text;
        return f;
    }

    inline bool Compare(Op op, long long lhs, long long rhs) {
        switch (op) {
            case Op::Less:         return lhs <  rhs;
            case Op::LessEqual:    return lhs <= rhs;
            case Op::Greater:      return lhs >  rhs;
            case Op::GreaterEqual: return lhs >= rhs;
            case Op::None:         return true;
        }
        return true;
    }

    // `sizeBytes` and `day` describe one candidate. A size of 0 means unknown -
    // the same convention FolderIndex uses - and an unknown never passes a size
    // filter, because reporting a file the user cannot have asked for is worse
    // than omitting one.
    inline bool Passes(const Filter &f, unsigned long long sizeBytes, long long day) {
        if (f.sizeOp != Op::None) {
            if (sizeBytes == 0) return false;
            if (!Compare(f.sizeOp, static_cast<long long>(sizeBytes),
                                   static_cast<long long>(f.sizeBytes))) return false;
        }
        if (f.dateOp != Op::None) {
            if (day == 0) return false;   // unknown timestamp, same rule
            if (!Compare(f.dateOp, day, f.dateDay)) return false;
        }
        return true;
    }

} // namespace Common::SearchFilter
