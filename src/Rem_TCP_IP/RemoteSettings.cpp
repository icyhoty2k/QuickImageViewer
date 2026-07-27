#include "RemoteSettings.h"
#include "RemoteCrypto.h"

#include "Dedicated/DedicatedSettings.h"
#include "Persistence/RegistryManager.h" // ForEachSetting — the one settings walk
#include "AppState.h"
#include "Platform/Constants.h"
#include "Platform/ConstantsStrings.h"

#include <algorithm>
#include <cwctype>

extern AppState app;

namespace Remote {

namespace RT = Constants::RemoteTcpIp;

namespace {
    // Trims leading/trailing whitespace. List entries arrive from a hand-edited
    // text file, so "  192.168.1.10 , 192.168.1.11" must behave like the tidy form.
    std::wstring Trim(const std::wstring &s) {
        size_t b = 0, e = s.size();
        while (b < e && ::iswspace(s[b])) ++b;
        while (e > b && ::iswspace(s[e - 1])) --e;
        return s.substr(b, e - b);
    }

    // A stored list entry must at least look like an address literal. Anything
    // with a path separator, a space in the middle or an obviously illegal
    // character is dropped rather than silently compared against and never matched.
    bool LooksLikeAddress(const std::wstring &s) {
        if (s.empty() || s.size() > 64) return false;
        for (wchar_t c : s) {
            const bool ok = (c >= L'0' && c <= L'9') ||
                            (c >= L'a' && c <= L'f') ||
                            (c >= L'A' && c <= L'F') ||
                            c == L'.' || c == L':' || c == L'*';
            if (!ok) return false;
        }
        return true;
    }

    // The .ini stores decimal text. GetPrivateProfileIntW cannot distinguish
    // "key absent" from "key present but zero", so ranged values are read as
    // strings and parsed here — that is what lets a garbage value fall back to
    // the default instead of silently becoming 0.
    int ReadIntKey(const wchar_t *key, int defaultValue) {
        const std::wstring raw = Trim(
            Dedicated::ReadSectionString(RT::INI_SECTION, key));
        if (raw.empty()) return defaultValue;
        for (wchar_t c : raw)
            if (c < L'0' || c > L'9') return defaultValue;
        try {
            return std::stoi(raw);
        } catch (...) {
            return defaultValue;
        }
    }
}

Settings &Config() {
    static Settings s = [] {
        Settings d;
        d.enable         = RT::ENABLE_DEFAULT;
        d.bindAddress    = RT::BIND_ADDRESS_DEFAULT;
        d.port           = RT::PORT_UNSET;
        d.maxConnections = RT::MAX_CONNECTIONS_DEFAULT;
        return d;
    }();
    return s;
}

std::vector<std::wstring> ParseList(const std::wstring &raw) {
    std::vector<std::wstring> out;
    const std::wstring seps = RT::LIST_SEPARATORS;

    size_t start = 0;
    while (start <= raw.size()) {
        const size_t hit = raw.find_first_of(seps, start);
        const std::wstring tok =
            Trim(raw.substr(start, hit == std::wstring::npos ? std::wstring::npos
                                                             : hit - start));
        if (!tok.empty()) out.push_back(tok);
        if (hit == std::wstring::npos) break;
        start = hit + 1;
    }
    return out;
}

std::wstring JoinList(const std::vector<std::wstring> &items) {
    std::wstring out;
    for (const std::wstring &s : items) {
        if (!out.empty()) out += L',';
        out += s;
    }
    return out;
}

void Normalize(Settings &s) {
    // Port: 0 stays 0 (means "not configured"); anything else must be legal.
    if (s.port != RT::PORT_UNSET &&
        (s.port < RT::PORT_MIN || s.port > RT::PORT_MAX))
        s.port = RT::PORT_UNSET;

    if (s.maxConnections < RT::MAX_CONNECTIONS_MIN ||
        s.maxConnections > RT::MAX_CONNECTIONS_MAX)
        s.maxConnections = RT::MAX_CONNECTIONS_DEFAULT;

    if (s.bindAddress.empty()) s.bindAddress = RT::BIND_ADDRESS_DEFAULT;

    // Drop entries that could never match a real peer address. Leaving them in
    // would make a typo look like a working rule.
    auto prune = [](std::vector<std::wstring> &v) {
        v.erase(std::remove_if(v.begin(), v.end(),
                               [](const std::wstring &e) { return !LooksLikeAddress(e); }),
                v.end());
    };
    prune(s.allowList);
    prune(s.blackList);
}

bool SectionExists() {
    if (!Dedicated::SettingsUseFile()) return false;
    // Enable is written by every save, so its presence is a reliable marker that
    // the section has been configured at least once.
    return !Dedicated::ReadSectionString(RT::INI_SECTION, RT::KEY_ENABLE).empty();
}

void LoadFromIni() {
    // Registry-backed installs have no .ini at all. Remote control is then
    // reachable only through command-line switches, by design.
    if (!Dedicated::SettingsUseFile()) return;

    Settings &s = Config();

    s.enable = Dedicated::ParseBoolValue(
        Dedicated::ReadSectionString(RT::INI_SECTION, RT::KEY_ENABLE),
        RT::ENABLE_DEFAULT);

    s.name = Trim(Dedicated::ReadSectionString(RT::INI_SECTION, RT::KEY_NAME));

    const std::wstring bind =
        Trim(Dedicated::ReadSectionString(RT::INI_SECTION, RT::KEY_IP_ADDRESS));
    s.bindAddress = bind.empty() ? RT::BIND_ADDRESS_DEFAULT : bind;

    s.port           = ReadIntKey(RT::KEY_PORT_NO,   RT::PORT_UNSET);
    s.maxConnections = ReadIntKey(RT::KEY_MAX_CONNS, RT::MAX_CONNECTIONS_DEFAULT);

    s.allowList = ParseList(Dedicated::ReadSectionString(RT::INI_SECTION, RT::KEY_ALLOW_LIST));
    s.blackList = ParseList(Dedicated::ReadSectionString(RT::INI_SECTION, RT::KEY_BLACK_LIST));

    // Stored already-hashed. Nothing here ever sees or writes a plaintext
    // password — hashing happens where the user types it.
    s.passwordHash =
        Trim(Dedicated::ReadSectionString(RT::INI_SECTION, RT::KEY_PASSWORD));

    Normalize(s);
}

void SaveToIni() {
    const Settings &s = Config();

    Dedicated::WriteSectionString(RT::INI_SECTION, RT::KEY_ENABLE,
                                  s.enable ? L"true" : L"false");
    Dedicated::WriteSectionString(RT::INI_SECTION, RT::KEY_NAME,       s.name);
    Dedicated::WriteSectionString(RT::INI_SECTION, RT::KEY_IP_ADDRESS, s.bindAddress);
    Dedicated::WriteSectionDword (RT::INI_SECTION, RT::KEY_PORT_NO,
                                  static_cast<DWORD>(s.port));
    Dedicated::WriteSectionString(RT::INI_SECTION, RT::KEY_ALLOW_LIST,
                                  JoinList(s.allowList));
    Dedicated::WriteSectionString(RT::INI_SECTION, RT::KEY_PASSWORD,   s.passwordHash);
    Dedicated::WriteSectionString(RT::INI_SECTION, RT::KEY_BLACK_LIST,
                                  JoinList(s.blackList));
    Dedicated::WriteSectionDword (RT::INI_SECTION, RT::KEY_MAX_CONNS,
                                  static_cast<DWORD>(s.maxConnections));
}

void ApplyOverrides(const Overrides &o) {
    Settings &s = Config();

    // -remote only ever switches the listener ON. There is no command-line way
    // to switch it off, because the absence of a switch already means "leave the
    // stored value alone" — a -remote=false would be indistinguishable from it.
    if (o.enable) s.enable = true;

    if (!o.name.empty())        s.name        = o.name;
    if (!o.bindAddress.empty()) s.bindAddress = o.bindAddress;
    if (!o.allowList.empty())   s.allowList   = ParseList(o.allowList);
    if (!o.blackList.empty())   s.blackList   = ParseList(o.blackList);

    if (o.port >= 0)           s.port           = o.port;
    if (o.maxConnections >= 0) s.maxConnections = o.maxConnections;

    // Hashed immediately. The plaintext came from a command line — already the
    // worst place for it, visible in Task Manager and the parent shell's history
    // — so it must not also be retained in memory or written anywhere as given.
    if (!o.plainPassword.empty()) {
        const std::wstring hashed = Crypto::HashPassword(o.plainPassword);
        // A failed hash must not silently leave the previous password in place
        // while the user believes they set a new one.
        if (!hashed.empty()) s.passwordHash = hashed;
    }

    Normalize(s);
}

bool IniExists() {
    const std::wstring &path = Dedicated::SettingsFilePath();
    if (path.empty()) return false;
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

void SaveToIniSeeded(bool &createdFileOut) {
    createdFileOut = !IniExists();

    if (createdFileOut) {
        // Seed [Settings] BEFORE writing anything else. From the next launch the
        // .ini is authoritative for every setting, so a file that holds only a
        // port number would silently reset the user's entire configuration.
        //
        // The key list comes from Persistence::Registry::ForEachSetting — the
        // same walk the tray's Export uses — so a setting added later is seeded
        // automatically rather than being quietly forgotten here.
        Persistence::Registry::ForEachSetting(
            app,
            [](const wchar_t *key, DWORD value, void *) {
                Dedicated::WriteDword(key, value); // [Settings]
            },
            nullptr);

        // Identity, so a generated file says what it belongs to rather than
        // appearing as an anonymous block of keys. Dedicated=0: an .ini created
        // to hold remote settings makes the copy file-backed, but it does NOT
        // make it a dedicated appliance.
        if (Dedicated::ReadInstanceName().empty())
            Dedicated::WriteInstanceString(L"Name", Dedicated::ExeStemName());
        if (Dedicated::ReadInstanceString(L"Dedicated").empty())
            Dedicated::WriteInstanceString(L"Dedicated", L"0");
        Dedicated::WriteInstanceString(L"Version", Constants::APP_VERSION);
    }

    SaveToIni();
}

std::wstring WhyCannotStart(const Settings &s) {
    if (!s.enable)
        return Constants::Messages::REMOTE_BLOCKED_DISABLED;
    if (s.port == RT::PORT_UNSET)
        return Constants::Messages::REMOTE_BLOCKED_NO_PORT;
    // Not a hard stop — the server binds and listens — but every connection will
    // be refused, so saying nothing here would look exactly like a broken server.
    if (s.allowList.empty())
        return Constants::Messages::REMOTE_WARN_EMPTY_ALLOWLIST;
    return {};
}

} // namespace Remote
