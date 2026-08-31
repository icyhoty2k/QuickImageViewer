// SPDX-License-Identifier: AGPL-3.0-or-later
//
// HistoryPath — cleaning and validating one line of the history files.
//
// SPLIT OUT OF HistoryFoldersManager.cpp so the test binary can link it. It is
// the pure half of that file: no AppState, no registry, no disk. It is also the
// half that reads UNTRUSTED input — both files are documented as hand-editable,
// so every line arriving from them is a string somebody may have typed.
//
// The contract is in HistoryFoldersManager.h, beside the declarations.

#include "HistoryFoldersManager.h"

#include <windows.h>
#include <cwctype>

// ---------------------------------------------------------------------------
// PATH HYGIENE  —  see the header for why this exists
// ---------------------------------------------------------------------------
namespace HistoryPath {

    // Longest path Win32 accepts with the \\?\ prefix. Anything past this is a
    // corrupt line, not a path — two concatenated entries, say.
    static constexpr size_t MAX_PATH_CHARS = 32767;

    std::wstring Clean(const std::wstring &raw) {
        std::wstring s = raw;

        // --- trim whitespace (covers \r, \n, tabs, the trailing spaces a hand
        //     edit leaves behind) ---
        auto trim = [](std::wstring &t) {
            size_t b = 0, e = t.size();
            while (b < e && iswspace(t[b])) ++b;
            while (e > b && iswspace(t[e - 1])) --e;
            t = t.substr(b, e - b);
        };
        trim(s);

        // --- unwrap one pair of quotes: Explorer's "Copy as path" produces them
        if (s.size() >= 2 && s.front() == L'"' && s.back() == L'"') {
            s = s.substr(1, s.size() - 2);
            trim(s);
        }
        return s;
    }

    bool Normalize(const std::wstring &raw, std::wstring &out) {
        std::wstring s = Clean(raw);
        if (s.empty() || s.size() > MAX_PATH_CHARS) return false;

        // --- reject characters that cannot appear in a Win32 path ---
        // ':' is allowed only as the drive separator at index 1, checked below.
        for (size_t i = 0; i < s.size(); ++i) {
            const wchar_t c = s[i];
            if (c < 0x20) return false; // control character
            if (c == L'<' || c == L'>' || c == L'"' || c == L'|' ||
                c == L'?' || c == L'*')
                return false;
            if (c == L':' && i != 1) return false;
        }

        // --- separators: '/' -> '\', then collapse runs ---
        for (auto &c: s)
            if (c == L'/') c = L'\\';

        const bool isUnc = (s.size() >= 2 && s[0] == L'\\' && s[1] == L'\\');
        std::wstring collapsed;
        collapsed.reserve(s.size());
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == L'\\' && !collapsed.empty() && collapsed.back() == L'\\')
                continue; // skip the repeat
            collapsed += s[i];
        }
        if (isUnc) collapsed.insert(collapsed.begin(), L'\\'); // restore the UNC pair
        s.swap(collapsed);

        // --- must be absolute: "X:\..." or "\\server\share" ---
        const bool isDrive = (s.size() >= 3 && iswalpha(s[0]) && s[1] == L':' && s[2] == L'\\');
        if (!isDrive && !isUnc) return false;
        if (isUnc && s.size() <= 2) return false; // bare "\\"

        // --- drop a trailing separator, but keep the drive root "D:\" ---
        while (s.size() > 3 && s.back() == L'\\')
            s.pop_back();
        if (isUnc && s.size() > 2 && s.back() == L'\\')
            s.pop_back();

        // A drive root alone ("D:\") is a legitimate folder; a bare "D:" is not.
        if (s.size() < 3) return false;

        out.swap(s);
        return true;
    }

    bool IsBroken(const std::wstring &entry) {
        std::wstring ignored;
        return !Normalize(entry, ignored);
    }

    bool Equal(const std::wstring &a, const std::wstring &b) {
        return a.size() == b.size() && _wcsicmp(a.c_str(), b.c_str()) == 0;
    }

    size_t HashCI::operator()(const std::wstring &s) const {
        // FNV-1a over the lowercased characters — must agree with Equal().
        size_t h = 1469598103934665603ULL;
        for (wchar_t c: s) {
            h ^= static_cast<size_t>(towlower(c));
            h *= 1099511628211ULL;
        }
        return h;
    }

} // namespace HistoryPath
