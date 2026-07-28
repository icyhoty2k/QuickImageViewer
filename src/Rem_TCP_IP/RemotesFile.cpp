#include "RemotesFile.h"

#include "Platform/Constants.h"

#include <cwctype>

namespace Remote {

namespace RT = Constants::RemoteTcpIp;

namespace {

    std::wstring Trim(const std::wstring &s) {
        size_t b = 0, e = s.size();
        while (b < e && ::iswspace(s[b])) ++b;
        while (e > b && ::iswspace(s[e - 1])) --e;
        return s.substr(b, e - b);
    }

    // The list file sits beside the EXE, not beside the instance .ini — see the
    // header on why the two must not be the same file.
    std::wstring ResolvePath() {
        wchar_t exe[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, exe, MAX_PATH) == 0) return {};

        std::wstring path(exe);
        const size_t slash = path.find_last_of(L"\\/");
        if (slash == std::wstring::npos) return {};
        path.erase(slash + 1);
        path += RT::REMOTES_FILE_NAME;
        return path;
    }

    // A row is "Name,IP,Port,Password,AutoConnect,ExePath".
    //
    // Split on the first five commas ONLY, so the sixth field keeps whatever it
    // contains. A Windows path cannot hold a comma, but it is the field most
    // likely to be hand-edited and it costs nothing to make it the safe one.
    bool ParseRow(const std::wstring &raw, RemoteEntry &out) {
        std::wstring f[6];
        size_t start = 0;
        int    i     = 0;
        for (; i < 5; ++i) {
            const size_t c = raw.find(L',', start);
            if (c == std::wstring::npos) return false; // too few fields — skip the row
            f[i]  = Trim(raw.substr(start, c - start));
            start = c + 1;
        }
        f[5] = Trim(raw.substr(start));

        if (f[0].empty() || f[1].empty()) return false;

        int port = 0;
        try {
            port = std::stoi(f[2]);
        } catch (...) {
            return false;
        }
        if (port < RT::PORT_MIN || port > RT::PORT_MAX) return false;

        out.name        = f[0];
        out.host        = f[1];
        out.port        = port;
        out.password    = f[3];
        out.autoConnect = (f[4] == L"1" || _wcsicmp(f[4].c_str(), L"true") == 0);
        out.exePath     = f[5];
        return true;
    }

    std::wstring BuildRow(const RemoteEntry &e) {
        return e.name + L"," + e.host + L"," + std::to_wstring(e.port) + L"," +
               e.password + L"," + (e.autoConnect ? L"1" : L"0") + L"," + e.exePath;
    }

} // namespace

const std::wstring &RemotesFilePath() {
    static const std::wstring path = ResolvePath();
    return path;
}

bool RemotesFileExists() {
    const std::wstring &p = RemotesFilePath();
    if (p.empty()) return false;
    const DWORD attr = GetFileAttributesW(p.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

std::vector<RemoteEntry> LoadRemotes() {
    std::vector<RemoteEntry> out;
    if (!RemotesFileExists()) return out; // drives nothing — the ordinary state

    const std::wstring &path = RemotesFilePath();

    // Numbered keys, read until one is missing. A gap ends the list rather than
    // being skipped: renumbering on every save keeps them contiguous, so a hole
    // means the file was hand-edited and stopping is the predictable answer.
    for (int i = 1; i <= RT::REMOTES_MAX; ++i) {
        wchar_t buf[1024] = {};
        const DWORD n = GetPrivateProfileStringW(
            RT::REMOTES_SECTION, std::to_wstring(i).c_str(), L"",
            buf, static_cast<DWORD>(std::size(buf)), path.c_str());
        if (n == 0) break;

        RemoteEntry e;
        if (ParseRow(buf, e)) out.push_back(std::move(e));
    }
    return out;
}

void SaveRemotes(const std::vector<RemoteEntry> &entries) {
    const std::wstring &path = RemotesFilePath();
    if (path.empty()) return;

    if (!RemotesFileExists()) {
        // UTF-16LE + BOM, written by hand. WritePrivateProfileStringW only
        // stores Unicode into a file that is ALREADY Unicode — create it as
        // ANSI and every non-ASCII character in a path or a name is silently
        // mangled from the first write onwards.
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            const wchar_t header[] = L"\xFEFF"
                L"; QuickImageViewer - instances this copy drives\r\n"
                L"; Name,IP,Port,Password,AutoConnect,ExePath\r\n"
                L"[Remotes]\r\n";
            DWORD written = 0;
            WriteFile(h, header, static_cast<DWORD>(sizeof(header) - sizeof(wchar_t)),
                      &written, nullptr);
            CloseHandle(h);
        }
    }

    // Drop the whole section first, then write the rows back numbered from 1.
    // Without the wipe, shrinking the list would leave the removed rows behind
    // and they would reappear on the next load.
    WritePrivateProfileStringW(RT::REMOTES_SECTION, nullptr, nullptr, path.c_str());

    int i = 1;
    for (const RemoteEntry &e : entries) {
        if (i > RT::REMOTES_MAX) break;
        WritePrivateProfileStringW(RT::REMOTES_SECTION, std::to_wstring(i).c_str(),
                                   BuildRow(e).c_str(), path.c_str());
        ++i;
    }
}

} // namespace Remote
