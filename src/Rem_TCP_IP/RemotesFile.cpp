#include "RemotesFile.h"

#include "Platform/Constants.h"

#include <cwctype>
#include <iterator>
#include <string>

namespace Remote {

namespace RT = Constants::RemoteTcpIp;

namespace {

    // The .ini truthiness rule, matching Dedicated::ParseBoolValue: 1/true/on/yes
    // (or any non-zero number) is true. Duplicated rather than reached for
    // because that function reads THIS instance's file, and the import reads
    // somebody else's.
    bool Dedicated_ParseBool(const std::wstring &raw) {
        std::wstring v;
        for (wchar_t c : raw) if (!::iswspace(c)) v += static_cast<wchar_t>(::towlower(c));
        if (v.empty()) return false;
        if (v == L"1" || v == L"true" || v == L"on" || v == L"yes") return true;
        if (v == L"0" || v == L"false" || v == L"off" || v == L"no") return false;
        try { return std::stoi(v) != 0; } catch (...) { return false; }
    }

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

// =============================================================================
// Stored secrets
// =============================================================================

bool IsStoredSecret(const std::wstring &f) {
    return f.rfind(SECRET_PREFIX, 0) == 0;
}

bool SplitStoredSecret(const std::wstring &f,
                       std::wstring &saltHexOut, std::wstring &digestHexOut) {
    if (!IsStoredSecret(f)) return false;
    const std::wstring body = f.substr(wcslen(SECRET_PREFIX));
    const size_t sep = body.find(L'$');
    if (sep == std::wstring::npos || sep == 0 || sep + 1 >= body.size()) return false;
    saltHexOut   = body.substr(0, sep);
    digestHexOut = body.substr(sep + 1);
    return true;
}

// =============================================================================
// Import
// =============================================================================

bool ImportFromInstanceFile(const std::wstring &chosenPath,
                            RemoteEntry &entryOut,
                            std::wstring &problemOut,
                            std::wstring &warningOut) {
    problemOut.clear();
    warningOut.clear();
    if (chosenPath.empty()) { problemOut = L"No file chosen."; return false; }

    // Work out which of the pair was picked. An instance's settings file is its
    // exe path with the extension swapped — the same derivation
    // Dedicated::SettingsFilePath uses, so either half finds the other.
    std::wstring iniPath = chosenPath;
    std::wstring exePath;

    const size_t dot   = chosenPath.find_last_of(L'.');
    const size_t slash = chosenPath.find_last_of(L"\\/");
    const bool hasExt  = dot != std::wstring::npos &&
                         (slash == std::wstring::npos || dot > slash);

    if (hasExt && _wcsicmp(chosenPath.c_str() + dot, L".exe") == 0) {
        exePath = chosenPath;
        iniPath = chosenPath.substr(0, dot) + L".ini";

        if (GetFileAttributesW(iniPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
            // The most likely reason to land here: that copy has never been
            // configured, or it is launched with -config pointing somewhere
            // else entirely. Neither is something this can guess its way out of.
            problemOut = L"That exe has no settings file beside it.\r\n\r\nExpected:\r\n    " +
                         iniPath +
                         L"\r\n\r\nEither the instance has never been configured (open it and "
                         L"use F9 → Save to INI), or it is started with -config pointing at a "
                         L"file elsewhere — in which case pick that file instead.";
            return false;
        }
    } else {
        // An .ini was picked. Its exe is the same derivation in reverse, and is
        // optional: without it the console simply cannot start that instance.
        if (hasExt) {
            const std::wstring candidate = chosenPath.substr(0, dot) + L".exe";
            if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES)
                exePath = candidate;
        }
    }

    if (GetFileAttributesW(iniPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        problemOut = L"That file no longer exists:\r\n\r\n    " + iniPath;
        return false;
    }

    // Read the section directly rather than through Dedicated::ReadSectionString,
    // which is bound to THIS instance's own settings file.
    auto readKey = [&](const wchar_t *key) {
        wchar_t buf[1024] = {};
        GetPrivateProfileStringW(RT::INI_SECTION, key, L"", buf,
                                 static_cast<DWORD>(std::size(buf)), iniPath.c_str());
        return Trim(buf);
    };

    const std::wstring rawEnable = readKey(RT::KEY_ENABLE);
    const std::wstring rawPort   = readKey(RT::KEY_PORT_NO);
    const std::wstring rawName   = readKey(RT::KEY_NAME);
    const std::wstring rawAllow  = readKey(RT::KEY_ALLOW_LIST);
    const std::wstring rawPass   = readKey(RT::KEY_PASSWORD);
    const std::wstring rawBind   = readKey(RT::KEY_IP_ADDRESS);

    // No section at all. Every key comes back empty, and the file is somebody
    // else's .ini or an instance that was never given remote settings.
    if (rawEnable.empty() && rawPort.empty() && rawName.empty()) {
        problemOut = L"That settings file has no [" + std::wstring(RT::INI_SECTION) +
                     L"] section.\r\n\r\nThat instance has never been configured for "
                     L"remote control. Open it, fill in F9 and press Save to INI.";
        return false;
    }

    int port = 0;
    try {
        port = std::stoi(rawPort);
    } catch (...) {
        port = 0;
    }
    if (port < RT::PORT_MIN || port > RT::PORT_MAX) {
        problemOut = L"That instance has no valid listen port configured (PortNo = \"" +
                     rawPort + L"\").\r\n\r\nSet one in its F9 panel first — there is no "
                     L"default port to fall back on.";
        return false;
    }

    // Warnings: both mean the connection will not complete, and neither is
    // visible from outside — the attempt just times out or hangs up.
    if (rawName.empty()) {
        // A nameless instance refuses to start, so importing one records a row
        // that can never connect. Named first because it is the newest of the
        // three requirements and the least likely to be expected.
        warningOut = L"It has no Name set, and a listener will not start without one — "
                     L"a driving instance identifies it by name. Set one in its F9 panel.";
    } else if (!Dedicated_ParseBool(rawEnable)) {
        warningOut = L"Its listener is disabled (Enable=false), so it will not accept "
                     L"anything until that is switched on in its F9 panel.";
    } else if (rawAllow.empty()) {
        warningOut = L"Its AllowList is empty, which denies EVERY connection — that is "
                     L"the fail-closed default. Add 127.0.0.1 to it in its F9 panel.";
    } else if (rawAllow.find(L"127.0.0.1") == std::wstring::npos &&
               rawAllow.find(L'*') == std::wstring::npos) {
        warningOut = L"Its AllowList does not include 127.0.0.1, so it will accept the "
                     L"connection and immediately hang up on this machine.";
    }

    entryOut = RemoteEntry{};
    entryOut.name = rawName;
    // Named after its file when it has no name of its own — better than an
    // address, and the file name is what the user just picked, so they will
    // recognise it.
    if (entryOut.name.empty()) {
        const std::wstring stem = iniPath.substr(slash == std::wstring::npos ? 0 : slash + 1);
        const size_t sdot = stem.find_last_of(L'.');
        entryOut.name = (sdot == std::wstring::npos) ? stem : stem.substr(0, sdot);
    }

    // ALWAYS loopback. IpAddress in that file is the address the instance BINDS
    // to, which is not an address to connect to — 0.0.0.0 means "every
    // interface" and cannot be dialled. Reading its file means it is on this
    // machine, so loopback is both correct and the one address its default
    // AllowList admits.
    (void) rawBind;
    entryOut.host        = L"127.0.0.1";
    entryOut.port        = port;
    entryOut.exePath     = exePath;
    entryOut.autoConnect = false;

    // The stored "salt$digest" IS the shared secret — see the header. Carried
    // across with a marker so the connect path knows not to treat it as a typed
    // password. An instance with no password needs none.
    if (!rawPass.empty()) {
        if (rawPass.find(L'$') == std::wstring::npos) {
            problemOut = L"That instance's stored password is malformed — it should be "
                         L"\"salt$hash\". Re-set it in its F9 panel.";
            return false;
        }
        entryOut.password = std::wstring(SECRET_PREFIX) + rawPass;
    }

    return true;
}

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
        if (!ParseRow(buf, e)) continue;

        // The name is the identity — see the header. A file that somehow holds
        // two rows with one name is ambiguous in every message that would name
        // it, so the first wins and the rest are dropped. First rather than last
        // because the file is written in list order: the earlier row is the one
        // that was there before whatever went wrong.
        bool duplicate = false;
        for (const RemoteEntry &seen : out) {
            if (_wcsicmp(seen.name.c_str(), e.name.c_str()) == 0) { duplicate = true; break; }
        }
        if (duplicate) continue;

        out.push_back(std::move(e));
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
