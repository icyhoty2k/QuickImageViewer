// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#include "IniFile.h"

#include "Platform/Constants.h"

#include <cwctype>
#include <vector>

namespace Persistence::Ini {

std::wstring PathBesideExe(const wchar_t *fileName) {
    if (!fileName || !*fileName) return {};

    wchar_t exe[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH) == 0) return {};

    std::wstring path(exe);
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};

    path.erase(slash + 1);
    path += fileName;
    return path;
}

bool Exists(const std::wstring &path) {
    if (path.empty()) return false;
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

void CreateWithHeaderIfMissing(const std::wstring &path, const wchar_t *headerComment) {
    if (path.empty()) return;

    // CREATE_NEW rather than a Exists() test followed by a create: the test-then-
    // act pair has a window in which another instance creates the file, and the
    // second writer would then truncate the first one's header.
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return; // already exists, or unwritable

    std::wstring head;
    head += static_cast<wchar_t>(0xFEFF); // UTF-16LE BOM — must be first
    head += L"; QuickImageViewer\r\n";
    if (headerComment && *headerComment) {
        head += L"; ";
        head += headerComment;
        head += L"\r\n";
    }
    head += L"\r\n";

    DWORD written = 0;
    WriteFile(h, head.data(), static_cast<DWORD>(head.size() * sizeof(wchar_t)),
              &written, nullptr);
    CloseHandle(h);
}

namespace {
    constexpr const wchar_t *STAMP_GENERATED = L"; Generated: ";
    constexpr const wchar_t *STAMP_UPDATED   = L"; Updated:   ";
}

std::wstring TimeStampNow() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t buf[32];
    swprintf_s(buf, L"%04u-%02u-%02u %02u:%02u:%02u",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

std::wstring GeneratedStampLines() {
    const std::wstring now = TimeStampNow();
    return std::wstring(STAMP_GENERATED) + now + L"\r\n" +
           STAMP_UPDATED + now + L"\r\n";
}

void TouchUpdatedStamp(const std::wstring &path) {
    if (!Exists(path)) return;

    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart < 2 || size.QuadPart > 8 * 1024 * 1024) {
        CloseHandle(h);
        return;
    }

    std::wstring text(static_cast<size_t>(size.QuadPart) / sizeof(wchar_t), L'\0');
    DWORD got = 0;
    if (!ReadFile(h, text.data(), static_cast<DWORD>(text.size() * sizeof(wchar_t)),
                  &got, nullptr) || got == 0) {
        CloseHandle(h);
        return;
    }
    text.resize(got / sizeof(wchar_t));

    // Only ever touch a file this module wrote. A missing BOM means somebody
    // replaced it with an ANSI file, and rewriting that as UTF-16 would corrupt
    // every value in it.
    if (text.empty() || text[0] != static_cast<wchar_t>(0xFEFF)) {
        CloseHandle(h);
        return;
    }

    const std::wstring line = std::wstring(STAMP_UPDATED) + TimeStampNow();

    size_t at = text.find(STAMP_UPDATED);
    if (at != std::wstring::npos) {
        size_t end = text.find(L'\n', at);
        if (end == std::wstring::npos) end = text.size(); else ++end;
        text.replace(at, end - at, line + L"\r\n");
    } else {
        // No stamp yet — a file from an older build. Put it after Generated if
        // that exists, otherwise immediately after the BOM.
        size_t ins = text.find(STAMP_GENERATED);
        if (ins != std::wstring::npos) {
            ins = text.find(L'\n', ins);
            ins = (ins == std::wstring::npos) ? text.size() : ins + 1;
        } else {
            ins = 1;   // straight after the BOM
        }
        text.insert(ins, line + L"\r\n");
    }

    // Truncate as well as rewrite: the new text may be shorter than the old.
    SetFilePointer(h, 0, nullptr, FILE_BEGIN);
    DWORD written = 0;
    WriteFile(h, text.data(), static_cast<DWORD>(text.size() * sizeof(wchar_t)),
              &written, nullptr);
    SetEndOfFile(h);
    CloseHandle(h);
}

void CreateWithTextIfMissing(const std::wstring &path, const std::wstring &text) {
    if (path.empty()) return;

    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;

    std::wstring out(1, static_cast<wchar_t>(0xFEFF));   // UTF-16LE BOM first
    out += text;

    DWORD written = 0;
    WriteFile(h, out.data(), static_cast<DWORD>(out.size() * sizeof(wchar_t)),
              &written, nullptr);
    CloseHandle(h);
}

std::wstring ReadString(const std::wstring &path, const wchar_t *section,
                        const wchar_t *key) {
    if (path.empty()) return {};

    // Grows until the value fits. GetPrivateProfileString reports a length of
    // size-1 when it truncated, which is indistinguishable from a value that
    // exactly fills the buffer — so both cases retry with a bigger one.
    std::wstring buf(256, L'\0');
    for (;;) {
        const DWORD n = GetPrivateProfileStringW(section, key, L"", buf.data(),
                                                 static_cast<DWORD>(buf.size()),
                                                 path.c_str());
        if (n < buf.size() - 1 || buf.size() >= Constants::MAX_FILE_PATH) {
            buf.resize(n);
            return buf;
        }
        buf.assign(buf.size() * 2, L'\0');
    }
}

void WriteString(const std::wstring &path, const wchar_t *section,
                 const wchar_t *key, const std::wstring &value,
                 const wchar_t *headerComment) {
    if (path.empty()) return;
    CreateWithHeaderIfMissing(path, headerComment);
    WritePrivateProfileStringW(section, key, value.c_str(), path.c_str());
}

void DeleteKey(const std::wstring &path, const wchar_t *section, const wchar_t *key) {
    if (path.empty() || !section || !key) return;
    // No CreateWithHeaderIfMissing: there is nothing to delete from a file that
    // does not exist, and creating one to remove a key from it would be absurd.
    // A null value is what tells the API to remove the key rather than blank it.
    WritePrivateProfileStringW(section, key, nullptr, path.c_str());
}

DWORD ReadDword(const std::wstring &path, const wchar_t *section,
                const wchar_t *key, DWORD defaultValue) {
    const std::wstring raw = ReadString(path, section, key);
    if (raw.empty()) return defaultValue;
    try {
        return static_cast<DWORD>(std::stoul(raw));
    } catch (...) {
        // A malformed value falls back rather than throwing: one bad line in a
        // hand-edited file must not stop the application from starting.
        return defaultValue;
    }
}

void WriteDword(const std::wstring &path, const wchar_t *section,
                const wchar_t *key, DWORD value, const wchar_t *headerComment) {
    WriteString(path, section, key, std::to_wstring(value), headerComment);
}

bool ParseBool(const std::wstring &raw, bool def) {
    std::wstring v;
    for (wchar_t c : raw)
        if (!::iswspace(c)) v += static_cast<wchar_t>(::towlower(c));

    if (v.empty()) return def;
    if (v == L"1" || v == L"true"  || v == L"on"  || v == L"yes") return true;
    if (v == L"0" || v == L"false" || v == L"off" || v == L"no")  return false;
    try { return std::stoi(v) != 0; } catch (...) { return def; }
}

} // namespace Persistence::Ini
