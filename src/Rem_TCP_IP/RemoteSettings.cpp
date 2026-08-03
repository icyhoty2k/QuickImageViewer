#include "RemoteSettings.h"
#include "RemoteBlacklist.h"  // -remoteBlock writes straight into the file
#include "RemoteCrypto.h"

#include "Dedicated/DedicatedSettings.h"
#include "Persistence/IniFile.h"  // qivLocalServer.ini — this subsystem's own file
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

    // The .ini stores decimal text. GetPrivateProfileIntW cannot distinguish
    // "key absent" from "key present but zero", so ranged values are read as
    // strings and parsed here — that is what lets a garbage value fall back to
    // the default instead of silently becoming 0.
    // The listener's own file, beside the exe. Resolved once — the exe cannot
    // move while the process runs.
    const std::wstring &LocalServerPath() {
        static const std::wstring path =
            Persistence::Ini::PathBesideExe(RT::LOCAL_SERVER_FILE_NAME);
        return path;
    }

    int ReadIntKey(const wchar_t *key, int defaultValue) {
        const std::wstring raw = Trim(
            Persistence::Ini::ReadString(LocalServerPath(), RT::INI_SECTION, key));
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
        d.autostart      = RT::AUTOSTART_DEFAULT;
        d.name           = RT::NAME_DEFAULT;
        d.bindAddress    = RT::BIND_ADDRESS_DEFAULT;
        d.port           = RT::PORT_DEFAULT;
        d.maxConnections = RT::MAX_CONNECTIONS_DEFAULT;
        // Seeded with this machine, so a freshly configured instance is
        // reachable from the copy beside it without anyone having to discover
        // that an empty list means "refuse everything".
        //
        // A SEED, not a rule: the accept gate gives 127.0.0.1 no special status,
        // so deleting this entry from the .ini really does lock this machine
        // out. That is deliberate — a list some addresses could bypass would not
        // be a list.
        d.allowList      = { RT::BIND_ADDRESS_DEFAULT };
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

// A stored list entry must at least look like an address literal. Anything with
// a path separator, a space in the middle or an obviously illegal character is
// dropped rather than silently compared against and never matched.
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

bool AddressMatches(const std::wstring &pattern, const std::wstring &addr) {
    if (pattern.empty()) return false;
    if (pattern == L"*") return true;

    if (pattern.back() == L'*') {
        const std::wstring prefix = pattern.substr(0, pattern.size() - 1);
        return addr.size() >= prefix.size() &&
               _wcsnicmp(addr.c_str(), prefix.c_str(), prefix.size()) == 0;
    }
    return _wcsicmp(pattern.c_str(), addr.c_str()) == 0;
}

bool InList(const std::vector<std::wstring> &list, const std::wstring &addr) {
    for (const std::wstring &p : list)
        if (AddressMatches(p, addr)) return true;
    return false;
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
}

bool SectionExists() {
    if (!IniExists()) return false;
    // Autostart is written by every save, so its presence is a reliable marker that
    // the file has been configured at least once rather than merely created.
    return !Persistence::Ini::ReadString(LocalServerPath(),
                                         RT::INI_SECTION, RT::KEY_AUTOSTART).empty();
}

void LoadFromIni() {
    // Gated on the FILE EXISTING, and on nothing else.
    //
    // It used to be entangled with SettingsUseFile() — "where does this process
    // keep its settings" — which is decided once at startup from what was on
    // disk then. That produced a genuinely baffling bug: configure the listener,
    // reopen the panel, and everything typed in came back blank, because at
    // startup there had been no file and the process stayed registry-backed for
    // the rest of its life.
    //
    // Now that the listener owns a file of its own, the two questions cannot be
    // conflated again: this one has no bearing on where anything else lives.
    if (!IniExists()) return;

    const std::wstring &path = LocalServerPath();
    Settings &s = Config();

    s.autostart = Persistence::Ini::ParseBool(
        Persistence::Ini::ReadString(path, RT::INI_SECTION, RT::KEY_AUTOSTART),
        RT::AUTOSTART_DEFAULT);

    // An ABSENT key keeps the default; it does not blank the field. A file that
    // simply omits Name must not leave an instance nameless — which is a state
    // that refuses to start.
    const std::wstring nm =
        Trim(Persistence::Ini::ReadString(path, RT::INI_SECTION, RT::KEY_NAME));
    if (!nm.empty()) s.name = nm;

    const std::wstring bind =
        Trim(Persistence::Ini::ReadString(path, RT::INI_SECTION, RT::KEY_IP_ADDRESS));
    s.bindAddress = bind.empty() ? RT::BIND_ADDRESS_DEFAULT : bind;

    s.port           = ReadIntKey(RT::KEY_PORT_NO,   RT::PORT_DEFAULT);
    s.maxConnections = ReadIntKey(RT::KEY_MAX_CONNS, RT::MAX_CONNECTIONS_DEFAULT);

    s.allowList = ParseList(
        Persistence::Ini::ReadString(path, RT::INI_SECTION, RT::KEY_ALLOW_LIST));

    // Stored already-hashed. Nothing here ever sees or writes a plaintext
    // password — hashing happens where the user types it.
    s.passwordHash =
        Trim(Persistence::Ini::ReadString(path, RT::INI_SECTION, RT::KEY_PASSWORD));

    Normalize(s);
}

void SaveToIni() {
    const std::wstring &path = LocalServerPath();
    const wchar_t *hdr = RT::LOCAL_SERVER_FILE_HEADER;
    const Settings &s = Config();

    // On CREATION, lay the whole file out annotated — a comment block above each
    // key. WritePrivateProfileString can only append bare keys, but it does keep
    // surrounding comments when it later updates a value, so doing it once here
    // is what makes the documentation survive every subsequent save.
    //
    // Creating this file has NO side effect on how the application persists
    // anything else — the reason it is a separate file at all.
    if (!Persistence::Ini::Exists(path)) {
        auto entry = [](const wchar_t *doc, const wchar_t *key,
                        const std::wstring &value) {
            return std::wstring(doc) + key + L"=" + value + L"\r\n\r\n";
        };

        std::wstring body;
        body += L"; QuickImageViewer\r\n; ";
        body += hdr;
        body += L"\r\n";
        body += Persistence::Ini::GeneratedStampLines();
        body += L";\r\n";
        body += L"; Values are applied when the listener STARTS. Edit while it is\r\n"
                L"; running and press Stop then Start in F9, or restart qIV.\r\n\r\n";
        body += L"["; body += RT::INI_SECTION; body += L"]\r\n\r\n";

        body += entry(RT::DOC_AUTOSTART,  RT::KEY_AUTOSTART,
                      s.autostart ? L"true" : L"false");
        body += entry(RT::DOC_NAME,       RT::KEY_NAME,       s.name);
        body += entry(RT::DOC_IP_ADDRESS, RT::KEY_IP_ADDRESS, s.bindAddress);
        body += entry(RT::DOC_PORT_NO,    RT::KEY_PORT_NO,    std::to_wstring(s.port));
        body += entry(RT::DOC_ALLOW_LIST, RT::KEY_ALLOW_LIST, JoinList(s.allowList));
        body += entry(RT::DOC_PASSWORD,   RT::KEY_PASSWORD,   s.passwordHash);
        body += entry(RT::DOC_MAX_CONNS,  RT::KEY_MAX_CONNS,
                      std::to_wstring(s.maxConnections));

        Persistence::Ini::CreateWithTextIfMissing(path, body);
    }

    // Still written key by key: this is also the UPDATE path, and the values
    // above are only correct for the launch that created the file.
    Persistence::Ini::WriteString(path, RT::INI_SECTION, RT::KEY_AUTOSTART,
                                  s.autostart ? L"true" : L"false", hdr);
    Persistence::Ini::WriteString(path, RT::INI_SECTION, RT::KEY_NAME,       s.name, hdr);
    Persistence::Ini::WriteString(path, RT::INI_SECTION, RT::KEY_IP_ADDRESS, s.bindAddress, hdr);
    Persistence::Ini::WriteDword (path, RT::INI_SECTION, RT::KEY_PORT_NO,
                                  static_cast<DWORD>(s.port), hdr);
    Persistence::Ini::WriteString(path, RT::INI_SECTION, RT::KEY_ALLOW_LIST,
                                  JoinList(s.allowList), hdr);
    Persistence::Ini::WriteString(path, RT::INI_SECTION, RT::KEY_PASSWORD,   s.passwordHash, hdr);
    Persistence::Ini::WriteDword (path, RT::INI_SECTION, RT::KEY_MAX_CONNS,
                                  static_cast<DWORD>(s.maxConnections), hdr);

    // Last, so the stamp reflects a completed save rather than a started one.
    Persistence::Ini::TouchUpdatedStamp(path);
}

void ApplyOverrides(const Overrides &o) {
    Settings &s = Config();

    // -remote only ever switches the listener ON. There is no command-line way
    // to switch it off, because the absence of a switch already means "leave the
    // stored value alone" — a -remote=false would be indistinguishable from it.
    if (o.autostart) s.autostart = true;

    if (!o.name.empty())        s.name        = o.name;
    if (!o.bindAddress.empty()) s.bindAddress = o.bindAddress;
    if (!o.allowList.empty())   s.allowList   = ParseList(o.allowList);
    // -remoteBlock goes STRAIGHT INTO THE BLACKLIST FILE rather than into a
    // Settings field, because there is no longer a blacklist anywhere else.
    // Written rather than held for the session on purpose: blocking an address
    // is the one command-line action whose whole point is that it outlives the
    // launch that asked for it.
    for (const std::wstring &addr : ParseList(o.blackList))
        Blacklist::Add(addr, Constants::Messages::BLACKLIST_REASON_CMDLINE);

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
    return Persistence::Ini::Exists(LocalServerPath());
}

bool IsLoopbackBind(const std::wstring &addr) {
    if (addr.empty()) return false;
    if (_wcsicmp(addr.c_str(), L"localhost") == 0) return true;
    if (_wcsicmp(addr.c_str(), L"::1") == 0)       return true;
    // The whole 127.0.0.0/8 block, not just 127.0.0.1 — every address in it is
    // loopback, and a prefix test is exact here because the range is defined by
    // its first octet. Matched on "127." with the dot so "1270.x" cannot pass.
    return addr.rfind(L"127.", 0) == 0;
}

std::wstring WhyCannotStart(const Settings &s) {
    if (s.port == RT::PORT_UNSET)
        return Constants::Messages::REMOTE_BLOCKED_NO_PORT;
    // A NAME IS MANDATORY, and not merely for tidiness.
    //
    // It is what this instance calls itself in its banner, which is what a
    // driving instance records as the row's identity — names are how remotes are
    // told apart, matched across a port change, and referred to in every message
    // about them. An unnamed instance produces a row nobody can name, so the
    // requirement belongs here, where it stops the listener from starting,
    // rather than being discovered at the far end later.
    if (s.name.empty())
        return Constants::Messages::REMOTE_BLOCKED_NO_NAME;
    // A PASSWORD IS MANDATORY off loopback. See the message's own comment for
    // why this refuses rather than warns.
    //
    // The test is the BIND ADDRESS, not the AllowList: what decides whether a
    // stranger can reach the socket at all is which interfaces it is on. An
    // AllowList is checked after the connection exists, and its entries are
    // addresses a caller asserts by connecting from — a second line, not the
    // first one.
    if (s.passwordHash.empty() && !IsLoopbackBind(s.bindAddress))
        return Constants::Messages::REMOTE_BLOCKED_NO_PASSWORD;
    // A password that is SET but unparseable. Checked separately from the empty
    // case and never folded into it: "cannot read the password" must never
    // resolve to "there is no password".
    if (!s.passwordHash.empty() && !Crypto::StoredIsUsable(s.passwordHash))
        return Constants::Messages::REMOTE_BLOCKED_BAD_PASSWORD;
    // Not a hard stop — the server binds and listens — but every connection will
    // be refused, so saying nothing here would look exactly like a broken server.
    if (s.allowList.empty())
        return Constants::Messages::REMOTE_WARN_EMPTY_ALLOWLIST;
    return {};
}

} // namespace Remote
