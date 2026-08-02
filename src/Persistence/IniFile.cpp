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
