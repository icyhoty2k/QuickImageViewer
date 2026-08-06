// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#include "RemoteBeacon.h"
#include "RemoteLog.h"      // the announcement is a network event, so it goes in that log
#include "RemoteServer.h"
#include "RemoteSettings.h"
#include "../AppState.h"
#include "../Platform/Constants.h"

#include <windows.h>
#include <windns.h>
#include <mutex>

// NO #pragma comment(lib, "Dnsapi.lib") HERE. CMake owns library configuration
// — see the note beside dxguid/comctl32 in CMakeLists.txt, which were moved for
// this exact reason. A pragma here makes the link list in CMake a lie.

extern AppState app;

namespace Remote::Beacon {

namespace {

    // The DNS-SD service type. `_qiv` is the application protocol, `_tcp` the
    // transport, and both are part of the name every client searches for — the
    // Android side asks NsdManager for exactly this string, so it is a wire
    // constant and not a label. Changing it makes every existing client blind.
    constexpr const wchar_t *SERVICE_TYPE = L"_qiv._tcp.local";

    // One mutex for the whole module. Refresh() is reachable from the UI thread
    // (menu toggle, server start/stop) and from the registration callback, and
    // the two must not interleave while deciding what is registered.
    std::mutex               g_mutex;
    PDNS_SERVICE_INSTANCE    g_instance   = nullptr;  // what we handed to Windows
    bool                     g_advertised = false;
    std::wstring             g_reason;                // why not, when not
    int                      g_advertisedPort = 0;    // to detect a port change
    std::wstring             g_advertisedName;        // ditto for the name

    // mDNS instance labels are shown to a person choosing a screen, so this
    // keeps them readable rather than strictly legal: dots would split the label
    // and control characters have no business on a network.
    std::wstring SanitiseLabel(const std::wstring &raw) {
        std::wstring out;
        for (wchar_t ch : raw) {
            if (out.size() >= 40) break;          // a phone list, not an essay
            if (ch < 0x20 || ch == 0x7F) continue;
            if (ch == L'.') { out += L'-'; continue; }
            out += ch;
        }
        while (!out.empty() && out.back() == L' ') out.pop_back();
        return out;
    }

    std::wstring HostLabel() {
        wchar_t buf[MAX_COMPUTERNAME_LENGTH + 1] = {};
        DWORD len = MAX_COMPUTERNAME_LENGTH + 1;
        if (GetComputerNameW(buf, &len) && buf[0] != L'\0') return buf;
        return L"qiv";
    }

    // Windows calls this when the registration finishes. It owns nothing we need
    // afterwards — the instance we constructed is ours to free — so this exists
    // mainly to record that the announcement really went out rather than merely
    // being asked for.
    VOID WINAPI RegisterComplete(DWORD status, PVOID, PDNS_SERVICE_INSTANCE pInstance) {
        std::wstring what;
        int          port = 0;
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            if (status == ERROR_SUCCESS) {
                g_advertised = true;
                g_reason.clear();
            } else {
                g_advertised = false;
                g_reason     = L"the network refused the announcement";
            }
            what = g_advertisedName;
            port = g_advertisedPort;
        }

        // RECORDED HERE RATHER THAN IN Refresh(), because this is where it
        // becomes true: registration is asynchronous, and Refresh only asks. A
        // line written at the request would claim the instance is findable
        // before the responder had agreed.
        //
        // "The phone cannot see the PC" is the support question this feature
        // exists to create, and its answer is always whether these two lines
        // are in the file. Outside the lock — Log::Add takes the log store's
        // own mutex, and there is no reason to hold both.
        if (Log::IsCapturing()) {
            Log::Add(Log::Direction::Out, Log::SelfLabel(),
                     status == ERROR_SUCCESS
                         ? L"(beacon published as \"" + what + L"\" on " + SERVICE_TYPE + L")"
                         : L"(beacon refused by the network — nothing is discoverable)",
                     L"(local network)",
                     status == ERROR_SUCCESS ? L"port " + std::to_wstring(port)
                                             : L"(beacon)",
                     -1);
        }

        // The callback's copy is separate from the one we constructed.
        if (pInstance) DnsServiceFreeInstance(pInstance);
    }

    void UnregisterLocked() {
        if (!g_instance) return;

        // The early return above is what keeps this honest: Refresh() calls
        // this on every start, stop and toggle, and only a registration that
        // actually existed gets a withdrawal line. Written before the fields
        // are cleared below, so it can still name what stopped being announced.
        if (Log::IsCapturing())
            Log::Add(Log::Direction::Out, Log::SelfLabel(),
                     L"(beacon withdrawn — \"" + g_advertisedName + L"\" is no longer discoverable)",
                     L"(local network)", L"(beacon)", -1);

        DNS_SERVICE_REGISTER_REQUEST req = {};
        req.Version           = DNS_QUERY_REQUEST_VERSION1;
        req.InterfaceIndex    = 0;
        req.pServiceInstance  = g_instance;
        req.pRegisterCompletionCallback = nullptr;   // fire and forget on the way out
        req.pQueryContext     = nullptr;
        req.unicastEnabled    = FALSE;

        DnsServiceDeRegister(&req, nullptr);

        DnsServiceFreeInstance(g_instance);
        g_instance       = nullptr;
        g_advertised     = false;
        g_advertisedPort = 0;
        g_advertisedName.clear();
    }

    // Loopback-only binds cannot be reached from the network, so announcing one
    // promises something nobody who hears it can use. "0.0.0.0" and a specific
    // LAN address are both fine; only the loopback forms are refused.
    bool BindIsReachable(const std::wstring &bindAddress) {
        if (bindAddress.empty())          return false;
        if (bindAddress == L"127.0.0.1")  return false;
        if (bindAddress == L"::1")        return false;
        if (bindAddress == L"localhost")  return false;
        return true;
    }

} // namespace

void Refresh() {
    std::lock_guard<std::mutex> lk(g_mutex);

    // --- Should there be a beacon at all? ------------------------------------
    // Three rules, checked in the order a user would ask them, so the reason
    // reported back is the one they can act on first.
    if (!app.remoteBeacon) {
        g_reason.clear();               // not asked for — not a fault
        UnregisterLocked();
        return;
    }
    if (!Remote::IsRunning()) {
        g_reason = L"the Local Server is not running";
        UnregisterLocked();
        return;
    }

    const Remote::Settings &cfg = Remote::Config();
    if (!BindIsReachable(cfg.bindAddress)) {
        g_reason = L"the server is bound to this machine only";
        UnregisterLocked();
        return;
    }
    if (cfg.port <= 0) {
        g_reason = L"no port is configured";
        UnregisterLocked();
        return;
    }

    // --- Already saying exactly this? ----------------------------------------
    // Refresh is called on every start, stop and toggle, and re-registering an
    // unchanged service would drop the record off the network for the moment
    // between the two calls — a client browsing at that instant sees it vanish.
    std::wstring label = SanitiseLabel(cfg.name.empty() ? HostLabel() : cfg.name);
    if (label.empty()) label = L"qIV";

    if (g_instance && g_advertisedPort == cfg.port && g_advertisedName == label)
        return;

    // Name or port changed: the old record is wrong, so it goes first.
    UnregisterLocked();

    // --- Announce ------------------------------------------------------------
    const std::wstring instanceName = label + L"." + SERVICE_TYPE;
    const std::wstring hostName     = HostLabel() + L".local";

    // NOTHING BUT NAME AND PORT. No TXT properties are attached on purpose —
    // every field here is readable by anyone on the network, and the only things
    // worth saying are already in the name and the port. See the header.
    g_instance = DnsServiceConstructInstance(
        instanceName.c_str(),
        hostName.c_str(),
        nullptr,                      // IPv4 — let the responder fill it in
        nullptr,                      // IPv6 — likewise
        static_cast<WORD>(cfg.port),
        0,                            // priority
        0,                            // weight
        0,                            // property count
        nullptr,                      // property keys
        nullptr);                     // property values

    if (!g_instance) {
        g_advertised = false;
        g_reason     = L"could not build the announcement";
        return;
    }

    DNS_SERVICE_REGISTER_REQUEST req = {};
    req.Version           = DNS_QUERY_REQUEST_VERSION1;
    req.InterfaceIndex    = 0;
    req.pServiceInstance  = g_instance;
    req.pRegisterCompletionCallback = RegisterComplete;
    req.pQueryContext     = nullptr;
    req.unicastEnabled    = FALSE;

    const DWORD rc = DnsServiceRegister(&req, nullptr);

    // DNS_REQUEST_PENDING is the SUCCESS path: registration is asynchronous and
    // RegisterComplete decides the outcome. Treating "pending" as failure here
    // would tear down a registration that was about to succeed.
    if (rc != DNS_REQUEST_PENDING && rc != ERROR_SUCCESS) {
        DnsServiceFreeInstance(g_instance);
        g_instance   = nullptr;
        g_advertised = false;
        g_reason     = L"the system refused to publish the service";
        return;
    }

    g_advertisedPort = cfg.port;
    g_advertisedName = label;
    // g_advertised stays false until the callback says otherwise — this reports
    // what is true, not what was attempted.
}

bool IsAdvertising() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_advertised;
}

std::wstring InactiveReason() {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!app.remoteBeacon) return std::wstring();
    if (g_advertised)      return std::wstring();
    return g_reason;
}

void Shutdown() {
    std::lock_guard<std::mutex> lk(g_mutex);
    UnregisterLocked();
    g_reason.clear();
}

} // namespace Remote::Beacon
