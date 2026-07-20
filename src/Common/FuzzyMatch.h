#pragma once
#include <cwctype>
#include <windows.h>

// =============================================================================
// Common/FuzzyMatch.h  —  Shared fuzzy / wildcard matching for QIV panels.
//
// Two match modes, selected automatically by IsWildcardQuery():
//
// ─────────────────────────────────────────────────────────────────────────────
//   FUZZY  — order-strict subsequence match.  Every query character must appear
//             in the candidate in order, but need not be adjacent.
//             Gaps are penalised; consecutive runs, word-boundary hits
//             (after \, /, ., -, _, space), and starting at position 0 are
//             rewarded.  Returns scored positions for highlighting.
//
//   Matching:
//     "doc"   matches  "C:\My Documents\notes.txt"  — d, o, c appear in order
//     "myfld" matches  "C:\Work\MyFolder"            — m-y-f-l-d subsequence
//     "qiv"   matches  "C:\QuickImageViewer"         — q at boundary, i at 'I', v at 'V'
//     "abc"   does NOT match "C:\cab"                — wrong order (c before a)
//     "xyz"   does NOT match "C:\Work\photo.jpg"     — x not present at all
//     "ba"    does NOT match "C:\ab"                 — b comes after a, not before
//
//   Scoring breakdown (higher = better, used for sort order):
//     +8   if first query char matches at position 0 of the candidate
//     +10  for each query char that immediately follows the previous match
//          (consecutive run bonus)
//     +4   for each query char that immediately follows \, /, ., -, _, or space
//          (word-boundary bonus)
//     -(positions[last] - positions[first])   spread penalty (wide gaps hurt)
//
//   Scoring examples — same query, different candidates:
//     "doc"  vs  "doc.jpg"             → high   (start bonus + full run + zero spread)
//     "doc"  vs  "C:\documents"        → medium (boundary bonus after \, some spread)
//     "doc"  vs  "C:\My Documents\"   → medium (boundary after space in "Documents")
//     "doc"  vs  "adobe_color.dng"    → low    (no boundaries, large spread)
//
// ─────────────────────────────────────────────────────────────────────────────
//   WILDCARD — classic glob patterns.  Activated when the query contains * or ?.
//             Both * and ? are tested case-insensitively (lower both sides first).
//
//     *   matches any sequence of characters, including none (zero or more).
//     ?   matches exactly one character — any single character.
//
//   Simple * patterns:
//     *.jpg          — any path ending in ".jpg"
//     *.png          — any path ending in ".png"
//     *photo*        — any path containing "photo" anywhere
//     *2024*         — any path containing "2024" anywhere
//     vacation*      — path that STARTS with "vacation"  (no leading * = anchored)
//     *_final        — path that ENDS with "_final"      (no trailing * = anchored)
//     *\2024\*       — path that has a "2024" folder segment  (\\ is a literal backslash)
//
//   ? patterns  (? = exactly one character, no more, no less):
//     ?ork           — any 4-char word: "work", "fork", "cork", "york" …
//     ?ork*          — path whose content starts with any char + "ork"
//     report?        — "report1", "reportA", "reportX" … (one trailing char)
//     ???.jpg        — any 3-character filename with .jpg extension
//     img_????.jpg   — "img_" then exactly 4 chars then ".jpg"
//     ?.?            — single char, dot, single char  (e.g. "a.b", "x.y")
//
//   Multi-wildcard patterns (stars and questions can be combined freely):
//     *back*up*      — path with "back" appearing anywhere before "up"
//     *2024*trip*    — path containing "2024" then later "trip"
//     *\photos\*     — path with a "photos" directory anywhere in the middle
//     *.2024-??-??*  — path containing a date-like segment yyyy-mm-dd (fixed year)
//
//   Anchoring — the ENTIRE candidate path is matched, so leading/trailing * matter:
//     photos         — fuzzy mode (no * or ?), not a wildcard at all
//     *photos*       — wildcard: "photos" anywhere in the full path  ← most common
//     photos*        — wildcard: path STARTS with "photos"
//     *photos        — wildcard: path ENDS with "photos"
//     *\photos\*     — wildcard: "photos" is a folder segment (has \ on both sides)
//
//   Edge cases:
//     *              — matches everything, including an empty path
//     **             — same as * (consecutive stars are handled correctly)
//     ""  (empty)    — IsWildcardQuery returns false → treated as fuzzy, matches all
//
// ─────────────────────────────────────────────────────────────────────────────
//   Case handling:
//     Both algorithms work on lowercased strings only.
//     Call LowerCopy() on the query once; lowercase each candidate before matching.
//     This makes matching fully case-insensitive with no extra logic.
//     Example: typing "*.JPG" lowercases to "*.jpg" before WildcardMatch is called.
//
// ─────────────────────────────────────────────────────────────────────────────
// Usage:
//   1. Lowercase your query with LowerCopy().
//   2. Check IsWildcardQuery() to pick the right matcher.
//   3. For each candidate, lowercase it and call FuzzyMatch() or WildcardMatch().
//   4. Sort fuzzy results by FuzzyMatchResult::score descending (wildcards: no sort).
//   5. Use FuzzyMatchResult::positions[0..posCount) to highlight matched chars.
// =============================================================================

namespace Common {

static constexpr int FUZZY_MAX_QUERY = 200;

struct FuzzyMatchResult {
    int score                    = 0;
    int posCount                 = 0;
    int positions[FUZZY_MAX_QUERY] = {};
};

// True when query contains * or ? — use WildcardMatch instead of FuzzyMatch.
inline bool IsWildcardQuery(const wchar_t *query, int len) {
    for (int i = 0; i < len; ++i)
        if (query[i] == L'*' || query[i] == L'?') return true;
    return false;
}

// Glob-style wildcard match.  Both pat and text must already be lowercased.
// Matches the entire text string — no implicit leading/trailing *.
// Pass a non-null out to receive the positions of every character matched by a
// literal or ? token (positions into text); * segments are not recorded.
// These positions can be fed directly to DrawMatchText for highlighting.
inline bool WildcardMatch(const wchar_t *pat, const wchar_t *text,
                           FuzzyMatchResult *out = nullptr)
{
    const wchar_t *star     = nullptr;
    const wchar_t *s        = text;
    const wchar_t *textBase = text;

    int tempPos[FUZZY_MAX_QUERY];
    int tempCount = 0;  // positions written since last *
    int starCount = 0;  // tempCount at the most recent *

    while (*text) {
        if (*pat == *text || *pat == L'?') {
            if (tempCount < FUZZY_MAX_QUERY)
                tempPos[tempCount] = static_cast<int>(text - textBase);
            ++tempCount;
            ++pat; ++text;
        }
        else if (*pat == L'*') {
            star      = pat++;
            s         = text;
            starCount = tempCount;
        }
        else if (star) {
            pat       = star + 1;
            text      = ++s;
            tempCount = starCount; // discard positions from failed attempt
        }
        else return false;
    }
    while (*pat == L'*') ++pat;
    if (*pat) return false;

    if (out) {
        out->posCount = tempCount;
        for (int i = 0; i < tempCount; ++i)
            out->positions[i] = tempPos[i];
    }
    return true;
}

// Fuzzy subsequence match with scoring.
//   query/queryLen : lowercased query string.
//   text/textLen   : lowercased candidate string.
//   out            : populated on match; untouched on no-match.
// Returns true if every query character appears in text in order.
inline bool FuzzyMatch(const wchar_t *query, int queryLen,
                       const wchar_t *text,  int textLen,
                       FuzzyMatchResult &out) {
    int positions[FUZZY_MAX_QUERY];
    int ni = 0, qi = 0, pi = 0;
    while (ni < textLen && qi < queryLen) {
        if (text[ni] == query[qi]) { positions[pi++] = ni; ++qi; }
        ++ni;
    }
    if (qi < queryLen) return false;

    int score = 0;
    if (positions[0] == 0) score += 8;
    for (int k = 0; k < pi; ++k) {
        if (k > 0 && positions[k] == positions[k - 1] + 1) score += 10;
        if (positions[k] > 0) {
            wchar_t prev = text[positions[k] - 1];
            if (prev == L'_' || prev == L'-' || prev == L'.' ||
                prev == L' ' || prev == L'\\' || prev == L'/')
                score += 4;
        }
    }
    score -= (positions[pi - 1] - positions[0]);

    out.score    = score;
    out.posCount = pi;
    for (int i = 0; i < pi; ++i) out.positions[i] = positions[i];
    return true;
}

// Lowercases src[0..len) into dst, which must have capacity for len+1 wchar_t.
inline void LowerCopy(const wchar_t *src, int len, wchar_t *dst) {
    for (int i = 0; i < len; ++i)
        dst[i] = static_cast<wchar_t>(towlower(src[i]));
    dst[len] = L'\0';
}

// Draw text with per-character fuzzy-match highlighting.
//
// text/len     — string to draw
// isHL         — bool[len]; true = highlight that character
// x, y         — top-left origin in hdc coordinates (TA_TOP|TA_LEFT)
// clip         — clipping rect
// clrBase      — color for normal characters
// clrHighlight — color for matched characters
//
// Font must already be selected into hdc.
// Batches consecutive same-color characters into a single ExtTextOut call.
inline void DrawMatchText(HDC hdc,
                          const wchar_t* text, int len,
                          const bool* isHL,
                          int x, int y, const RECT& clip,
                          COLORREF clrBase, COLORREF clrHighlight)
{
    if (len <= 0 || !text) return;
    int  segStart = 0;
    bool segHL    = isHL[0];
    for (int ci = 1; ci <= len; ++ci) {
        bool curHL = (ci < len) && isHL[ci];
        if (curHL != segHL || ci == len) {
            int segLen = ci - segStart;
            SetTextColor(hdc, segHL ? clrHighlight : clrBase);
            ExtTextOutW(hdc, x, y, ETO_CLIPPED, &clip,
                        text + segStart, segLen, nullptr);
            SIZE sz;
            GetTextExtentPoint32W(hdc, text + segStart, segLen, &sz);
            x        += sz.cx;
            segStart  = ci;
            segHL     = curHL;
        }
    }
}

} // namespace Common
