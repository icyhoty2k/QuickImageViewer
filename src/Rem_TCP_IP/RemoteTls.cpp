// winsock2.h before anything that pulls in windows.h — see RemoteServer.cpp.
#include <winsock2.h>
#include <ws2tcpip.h>

#include "RemoteTls.h"
#include "RemoteSettings.h"        // IsLoopbackBind — the one rule for "is this local"
#include "RemoteCrypto.h"          // Sha256 / ToHex — the fingerprint
#include "Persistence/IniFile.h"   // PathBesideExe
#include "Platform/Constants.h"
#include "Platform/ConstantsStrings.h"

// SCH_CREDENTIALS and TLS_PARAMETERS — the modern credential structures, the
// only ones that can express "TLS 1.2 and 1.3, nothing older" — sit behind this
// macro in schannel.h. Without it the legacy SCHANNEL_CRED is all that exists,
// and with it the protocol set would be whatever system policy happens to allow.
//
// The SDK header states its own precondition: defining this also requires
// UNICODE_STRING, which windows.h does not provide. Hence winternl.h, and hence
// both of these BEFORE <schannel.h> rather than beside it.
#define SCHANNEL_USE_BLACKLISTS
#include <winternl.h>

#include <schannel.h>
#include <wincrypt.h>
#include <ncrypt.h>
#include <sddl.h>      // ConvertStringSecurityDescriptorToSecurityDescriptorW
#include <aclapi.h>    // SetNamedSecurityInfoW

#include <algorithm>
#include <mutex>

// secur32 / crypt32 / ncrypt are linked in CMakeLists.txt beside the other
// Windows libraries — deliberately not via #pragma comment(lib), so every
// dependency this project takes is visible in one place.

namespace Remote::Tls {

namespace RT = Constants::RemoteTcpIp;

namespace {

    // ── Process-wide credentials ────────────────────────────────────────────
    // Built once. A per-connection AcquireCredentialsHandle would re-read the
    // PFX and re-import the key for every client, which is slow and, worse,
    // would let the identity change under a running listener.
    std::mutex        g_credMutex;
    CredHandle        g_serverCred{};
    bool              g_haveServerCred = false;
    PCCERT_CONTEXT    g_serverCert     = nullptr;
    std::wstring      g_fingerprint;

    CredHandle        g_clientCred{};
    bool              g_haveClientCred = false;

    const std::wstring &CertPath() {
        static const std::wstring p =
            Persistence::Ini::PathBesideExe(RT::TLS_CERT_FILE_NAME);
        return p;
    }

    std::wstring SecErr(const wchar_t *what, SECURITY_STATUS st) {
        wchar_t buf[160];
        swprintf_s(buf, L"%s (0x%08X)", what, static_cast<unsigned>(st));
        return buf;
    }

    // SHA-256 of the certificate's DER bytes, lower-case hex. THE pinned value.
    //
    // Over the whole certificate rather than the public key: a certificate is
    // what Schannel hands back and what the user can verify with certutil or
    // openssl, so the number in the F9 panel is one anybody can reproduce.
    std::wstring FingerprintOf(PCCERT_CONTEXT cert) {
        if (!cert) return {};
        const std::vector<BYTE> h =
            Crypto::Sha256(cert->pbCertEncoded, cert->cbCertEncoded);
        return Crypto::ToHex(h);
    }

    // ── Certificate generation ──────────────────────────────────────────────

    // Creates the self-signed certificate and writes qivServerCert.pfx.
    //
    // The CNG key is created PERSISTED because CertCreateSelfSignCertificate
    // needs a key it can name, and is DELETED again once the PFX holds it — so
    // the file really is the only copy, which is what makes the folder portable.
    // Leaving it behind would also leak a key container per regeneration.
    bool CreateCertificateFile(std::wstring &errorOut) {
        NCRYPT_PROV_HANDLE prov = 0;
        NCRYPT_KEY_HANDLE  key  = 0;
        PCCERT_CONTEXT     cert = nullptr;
        HCERTSTORE         store = nullptr;
        bool               ok   = false;

        // A unique container name, so two copies generating at once cannot
        // collide and so a stale container from a crash is never reused.
        GUID g{};
        CoCreateGuid(&g);
        wchar_t container[64];
        swprintf_s(container, L"qIV-tls-%08X%04X%04X",
                   g.Data1, g.Data2, g.Data3);

        SECURITY_STATUS st =
            NCryptOpenStorageProvider(&prov, MS_KEY_STORAGE_PROVIDER, 0);
        if (st != ERROR_SUCCESS) {
            errorOut = SecErr(L"Cannot open the key storage provider", st);
            return false;
        }

        st = NCryptCreatePersistedKey(prov, &key, BCRYPT_RSA_ALGORITHM,
                                      container, 0, NCRYPT_OVERWRITE_KEY_FLAG);
        if (st != ERROR_SUCCESS) {
            errorOut = SecErr(L"Cannot create the TLS key", st);
            goto done;
        }

        {
            DWORD bits = RT::TLS_KEY_BITS;
            st = NCryptSetProperty(key, NCRYPT_LENGTH_PROPERTY,
                                   reinterpret_cast<PBYTE>(&bits), sizeof(bits), 0);
            if (st != ERROR_SUCCESS) {
                errorOut = SecErr(L"Cannot set the TLS key length", st);
                goto done;
            }
            st = NCryptFinalizeKey(key, 0);
            if (st != ERROR_SUCCESS) {
                errorOut = SecErr(L"Cannot finalize the TLS key", st);
                goto done;
            }
        }

        {
            // Subject name → encoded blob.
            DWORD nameLen = 0;
            if (!CertStrToNameW(X509_ASN_ENCODING, RT::TLS_CERT_SUBJECT,
                                CERT_X500_NAME_STR, nullptr, nullptr, &nameLen, nullptr)) {
                errorOut = L"Cannot encode the certificate subject name.";
                goto done;
            }
            std::vector<BYTE> nameBytes(nameLen);
            if (!CertStrToNameW(X509_ASN_ENCODING, RT::TLS_CERT_SUBJECT,
                                CERT_X500_NAME_STR, nullptr, nameBytes.data(),
                                &nameLen, nullptr)) {
                errorOut = L"Cannot encode the certificate subject name.";
                goto done;
            }
            CERT_NAME_BLOB subject{nameLen, nameBytes.data()};

            CRYPT_KEY_PROV_INFO kpi{};
            kpi.pwszContainerName = container;
            kpi.pwszProvName      = const_cast<LPWSTR>(MS_KEY_STORAGE_PROVIDER);
            kpi.dwProvType        = 0;              // 0 = CNG, not a legacy CSP
            kpi.dwKeySpec         = 0;

            CRYPT_ALGORITHM_IDENTIFIER sig{};
            sig.pszObjId = const_cast<LPSTR>(szOID_RSA_SHA256RSA);

            SYSTEMTIME start{}, end{};
            GetSystemTime(&start);
            end = start;
            end.wYear = static_cast<WORD>(end.wYear + RT::TLS_CERT_VALID_YEARS);
            // 29 February plus N years is not a date. Nudging to the 28th costs
            // one day of validity out of ten years and cannot fail.
            if (end.wMonth == 2 && end.wDay == 29) end.wDay = 28;

            cert = CertCreateSelfSignCertificate(key, &subject, 0, &kpi, &sig,
                                                 &start, &end, nullptr);
            if (!cert) {
                errorOut = L"Cannot create the self-signed certificate.";
                goto done;
            }
        }

        // Memory store → PFX blob → file.
        store = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0,
                              CERT_STORE_CREATE_NEW_FLAG, nullptr);
        if (!store) {
            errorOut = L"Cannot create a temporary certificate store.";
            goto done;
        }
        if (!CertAddCertificateContextToStore(store, cert,
                                              CERT_STORE_ADD_REPLACE_EXISTING, nullptr)) {
            errorOut = L"Cannot stage the certificate for export.";
            goto done;
        }

        {
            CRYPT_DATA_BLOB pfx{};
            // Empty password. The file is protected by where it sits and by its
            // DACL, not by a passphrase — a passphrase this program had to store
            // beside the file it protects would be decoration.
            if (!PFXExportCertStoreEx(store, &pfx, L"", nullptr,
                                      EXPORT_PRIVATE_KEYS | REPORT_NO_PRIVATE_KEY)) {
                errorOut = L"Cannot export the certificate.";
                goto done;
            }
            std::vector<BYTE> blob(pfx.cbData);
            pfx.pbData = blob.data();
            if (!PFXExportCertStoreEx(store, &pfx, L"", nullptr,
                                      EXPORT_PRIVATE_KEYS | REPORT_NO_PRIVATE_KEY)) {
                errorOut = L"Cannot export the certificate.";
                goto done;
            }

            // CREATE_NEW: never overwrite an existing identity by accident.
            // Regenerating is a deliberate act that deletes the file first.
            HANDLE h = CreateFileW(CertPath().c_str(), GENERIC_WRITE, 0, nullptr,
                                   CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE) {
                errorOut = L"Cannot write " + CertPath();
                goto done;
            }
            DWORD written = 0;
            const bool wrote = WriteFile(h, blob.data(),
                                         static_cast<DWORD>(pfx.cbData), &written, nullptr) &&
                               written == pfx.cbData;
            CloseHandle(h);
            if (!wrote) {
                DeleteFileW(CertPath().c_str());   // a truncated PFX is worse than none
                errorOut = L"Cannot write " + CertPath();
                goto done;
            }
        }

        ok = true;

    done:
        if (cert)  CertFreeCertificateContext(cert);
        if (store) CertCloseStore(store, 0);
        // Delete the persisted key whether or not this succeeded: on success the
        // PFX is the only copy by design, and on failure it is litter.
        if (key) {
            NCryptDeleteKey(key, NCRYPT_SILENT_FLAG);
            NCryptFreeObject(key);
        }
        if (prov) NCryptFreeObject(prov);
        return ok;
    }

    // Reads the PFX and returns the certificate, key attached.
    //
    // PKCS12_NO_PERSIST_KEY keeps the imported private key EPHEMERAL. Without
    // it every load would deposit another key container in the user's profile —
    // a slow leak, and copies of the key in the very place the file-based model
    // was chosen to avoid.
    PCCERT_CONTEXT LoadCertificate(std::wstring &errorOut) {
        HANDLE h = CreateFileW(CertPath().c_str(), GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            errorOut = L"Cannot open " + CertPath();
            return nullptr;
        }

        LARGE_INTEGER size{};
        if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0 || size.QuadPart > 1024 * 1024) {
            CloseHandle(h);
            errorOut = L"The certificate file is missing or not a certificate.";
            return nullptr;
        }

        std::vector<BYTE> bytes(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        const bool read = ReadFile(h, bytes.data(), static_cast<DWORD>(bytes.size()),
                                   &got, nullptr) != 0;
        CloseHandle(h);
        if (!read || got != bytes.size()) {
            errorOut = L"Cannot read " + CertPath();
            return nullptr;
        }

        CRYPT_DATA_BLOB blob{static_cast<DWORD>(bytes.size()), bytes.data()};
        HCERTSTORE store = PFXImportCertStore(
            &blob, L"", PKCS12_NO_PERSIST_KEY | PKCS12_ALWAYS_CNG_KSP);
        if (!store) {
            errorOut = L"The certificate file could not be read — delete " +
                       CertPath() + L" to have a new one generated.";
            return nullptr;
        }

        PCCERT_CONTEXT cert = CertFindCertificateInStore(
            store, X509_ASN_ENCODING, 0, CERT_FIND_ANY, nullptr, nullptr);
        // Duplicated so it outlives the store, which is closed immediately —
        // the context holds its own reference to the key.
        PCCERT_CONTEXT kept = cert ? CertDuplicateCertificateContext(cert) : nullptr;
        if (cert) CertFreeCertificateContext(cert);
        CertCloseStore(store, 0);

        if (!kept) errorOut = L"The certificate file contains no certificate.";
        return kept;
    }

    // Restricts the PFX to the current user. Stops another account on this
    // machine reading the key; stops nothing at all once the folder is copied,
    // which is the trade this storage model makes knowingly.
    void RestrictCertFileAccess() {
        // "Protected DACL, one ACE: the OWNER gets full access." PROTECTED is
        // the load-bearing part — without it the folder's inherited permissions
        // are merged back in and the restriction achieves nothing.
        PSECURITY_DESCRIPTOR sd = nullptr;
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"D:P(A;;FA;;;OW)", SDDL_REVISION_1, &sd, nullptr)) {
            BOOL present = FALSE, defaulted = FALSE;
            PACL acl = nullptr;
            if (GetSecurityDescriptorDacl(sd, &present, &acl, &defaulted) && present) {
                SetNamedSecurityInfoW(const_cast<LPWSTR>(CertPath().c_str()),
                                      SE_FILE_OBJECT,
                                      DACL_SECURITY_INFORMATION |
                                          PROTECTED_DACL_SECURITY_INFORMATION,
                                      nullptr, nullptr, acl, nullptr);
            }
            LocalFree(sd);
        }
    }

    // ── Schannel credentials ────────────────────────────────────────────────

    // TLS 1.2 and 1.3 only. Everything older is disabled explicitly rather than
    // left to the system default, because the default is a moving target set by
    // policy elsewhere and this is one of the few places where inheriting
    // somebody else's idea of "compatible" is a security decision.
    TLS_PARAMETERS MakeTlsParameters() {
        TLS_PARAMETERS p{};
        p.grbitDisabledProtocols = static_cast<DWORD>(
            ~(SP_PROT_TLS1_2 | SP_PROT_TLS1_3));
        return p;
    }

    bool AcquireServerCred(PCCERT_CONTEXT cert, std::wstring &errorOut) {
        TLS_PARAMETERS params = MakeTlsParameters();

        SCH_CREDENTIALS creds{};
        creds.dwVersion       = SCH_CREDENTIALS_VERSION;
        creds.cCreds          = 1;
        creds.paCred          = const_cast<PCCERT_CONTEXT *>(&cert);
        creds.cTlsParameters  = 1;
        creds.pTlsParameters  = &params;

        const SECURITY_STATUS st = AcquireCredentialsHandleW(
            nullptr, const_cast<LPWSTR>(UNISP_NAME_W), SECPKG_CRED_INBOUND,
            nullptr, &creds, nullptr, nullptr, &g_serverCred, nullptr);
        if (st != SEC_E_OK) {
            errorOut = SecErr(L"Cannot acquire TLS server credentials", st);
            return false;
        }
        g_haveServerCred = true;
        return true;
    }

    bool EnsureClientCred(std::wstring &errorOut) {
        std::lock_guard<std::mutex> lk(g_credMutex);
        if (g_haveClientCred) return true;

        TLS_PARAMETERS params = MakeTlsParameters();

        SCH_CREDENTIALS creds{};
        creds.dwVersion      = SCH_CREDENTIALS_VERSION;
        creds.cTlsParameters = 1;
        creds.pTlsParameters = &params;
        // MANUAL validation, and this is the crux of the pinning model: Schannel
        // must NOT apply chain validation, because a self-signed certificate
        // fails it by definition. The check that replaces it is the fingerprint
        // comparison in ConnectHandshake — which is stricter, not laxer, since
        // it trusts exactly one key rather than every CA in the store.
        //
        // NO_DEFAULT_CREDS so this never offers a client certificate it happens
        // to find; there is no client-certificate scheme here.
        creds.dwFlags = SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS;

        const SECURITY_STATUS st = AcquireCredentialsHandleW(
            nullptr, const_cast<LPWSTR>(UNISP_NAME_W), SECPKG_CRED_OUTBOUND,
            nullptr, &creds, nullptr, nullptr, &g_clientCred, nullptr);
        if (st != SEC_E_OK) {
            errorOut = SecErr(L"Cannot acquire TLS client credentials", st);
            return false;
        }
        g_haveClientCred = true;
        return true;
    }

    // ── Socket helpers, local so the TLS layer owns its own IO ──────────────

    bool SendRaw(SOCKET s, const char *data, size_t len) {
        size_t sent = 0;
        while (sent < len) {
            const int n = send(s, data + sent, static_cast<int>(len - sent), 0);
            if (n <= 0) return false;
            sent += static_cast<size_t>(n);
        }
        return true;
    }

    // Appends one read. False on close or error.
    bool RecvRaw(SOCKET s, std::string &into) {
        char buf[8192];
        const int n = recv(s, buf, static_cast<int>(sizeof(buf)), 0);
        if (n <= 0) return false;
        into.append(buf, static_cast<size_t>(n));
        return true;
    }
}

// =============================================================================
// Public surface
// =============================================================================

bool RequiredForAddress(const std::wstring &address) {
    // The ONE rule, and it is deliberately the inverse of "is this loopback".
    // Anything reachable from off this machine is encrypted; nothing else is.
    return !Remote::IsLoopbackBind(address);
}

bool EnsureServerCredentials(std::wstring &errorOut) {
    std::lock_guard<std::mutex> lk(g_credMutex);
    if (g_haveServerCred) return true;

    if (!Persistence::Ini::Exists(CertPath())) {
        if (!CreateCertificateFile(errorOut)) return false;
        RestrictCertFileAccess();
    }

    PCCERT_CONTEXT cert = LoadCertificate(errorOut);
    if (!cert) return false;

    if (!AcquireServerCred(cert, errorOut)) {
        CertFreeCertificateContext(cert);
        return false;
    }

    g_serverCert  = cert;                 // held: the credentials reference it
    g_fingerprint = FingerprintOf(cert);
    return true;
}

std::wstring ServerFingerprint() {
    std::lock_guard<std::mutex> lk(g_credMutex);
    return g_fingerprint;
}

bool RegenerateServerCertificate(std::wstring &errorOut) {
    {
        std::lock_guard<std::mutex> lk(g_credMutex);
        if (g_haveServerCred) {
            FreeCredentialsHandle(&g_serverCred);
            g_haveServerCred = false;
        }
        if (g_serverCert) {
            CertFreeCertificateContext(g_serverCert);
            g_serverCert = nullptr;
        }
        g_fingerprint.clear();

        if (Persistence::Ini::Exists(CertPath()) &&
            !DeleteFileW(CertPath().c_str())) {
            errorOut = L"Cannot delete " + CertPath() +
                       L" — stop the server and close anything holding the file.";
            return false;
        }
    }
    return EnsureServerCredentials(errorOut);
}

// =============================================================================
// Session
// =============================================================================

Session::~Session() { Release(); }

void Session::Release() {
    if (m_haveCtx) {
        DeleteSecurityContext(&m_ctx);
        m_haveCtx = false;
    }
    m_active = false;
    m_incoming.clear();
    m_decrypted.clear();
}

bool Session::AcceptHandshake(SOCKET s, std::wstring &errorOut) {
    {
        std::lock_guard<std::mutex> lk(g_credMutex);
        if (!g_haveServerCred) {
            errorOut = L"TLS credentials are not available.";
            return false;
        }
    }

    // A handshake that never completes must not pin a client thread. Restored
    // to the caller's timeout on the way out, whichever way this returns.
    DWORD prev = 0;
    int   prevLen = sizeof(prev);
    getsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char *>(&prev), &prevLen);
    DWORD hs = RT::TLS_HANDSHAKE_TIMEOUT_MS;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&hs), sizeof(hs));

    m_isServer = true;

    bool ok = false;
    for (int step = 0; step < RT::TLS_HANDSHAKE_MAX_STEPS; ++step) {
        if (m_incoming.empty() && !RecvRaw(s, m_incoming)) {
            errorOut = Constants::Messages::REMOTE_TLS_HANDSHAKE_FAILED;
            break;
        }

        SecBuffer in[2]{};
        in[0].BufferType = SECBUFFER_TOKEN;
        in[0].pvBuffer   = m_incoming.data();
        in[0].cbBuffer   = static_cast<unsigned long>(m_incoming.size());
        in[1].BufferType = SECBUFFER_EMPTY;
        SecBufferDesc inDesc{SECBUFFER_VERSION, 2, in};

        SecBuffer out[1]{};
        out[0].BufferType = SECBUFFER_TOKEN;
        SecBufferDesc outDesc{SECBUFFER_VERSION, 1, out};

        unsigned long attrs = 0;
        const SECURITY_STATUS st = AcceptSecurityContext(
            &g_serverCred,
            m_haveCtx ? &m_ctx : nullptr,
            &inDesc,
            ASC_REQ_SEQUENCE_DETECT | ASC_REQ_REPLAY_DETECT |
                ASC_REQ_CONFIDENTIALITY | ASC_REQ_EXTENDED_ERROR |
                ASC_REQ_ALLOCATE_MEMORY | ASC_REQ_STREAM,
            0,
            m_haveCtx ? nullptr : &m_ctx,
            &outDesc, &attrs, nullptr);

        if (st == SEC_E_OK || st == SEC_I_CONTINUE_NEEDED ||
            (FAILED(st) && (attrs & ASC_RET_EXTENDED_ERROR)))
            m_haveCtx = true;

        // A token to send back, even on failure — that is what carries the TLS
        // alert telling the peer WHY, instead of an unexplained disconnect.
        if (out[0].cbBuffer && out[0].pvBuffer) {
            SendRaw(s, static_cast<const char *>(out[0].pvBuffer), out[0].cbBuffer);
            FreeContextBuffer(out[0].pvBuffer);
        }

        if (st == SEC_E_INCOMPLETE_MESSAGE) {
            if (!RecvRaw(s, m_incoming)) {
                errorOut = Constants::Messages::REMOTE_TLS_HANDSHAKE_FAILED;
                break;
            }
            continue;
        }

        // Anything the handshake did not consume is either the next token or
        // the first application record. Keeping it is what makes a client that
        // sends its first line immediately work.
        if (in[1].BufferType == SECBUFFER_EXTRA)
            m_incoming.erase(0, m_incoming.size() - in[1].cbBuffer);
        else
            m_incoming.clear();

        if (st == SEC_E_OK) { ok = true; break; }
        if (st != SEC_I_CONTINUE_NEEDED) {
            errorOut = SecErr(Constants::Messages::REMOTE_TLS_HANDSHAKE_FAILED, st);
            break;
        }
    }

    if (ok) {
        SecPkgContext_StreamSizes sizes{};
        if (QueryContextAttributes(&m_ctx, SECPKG_ATTR_STREAM_SIZES, &sizes) != SEC_E_OK) {
            errorOut = L"TLS stream sizes unavailable.";
            ok = false;
        } else {
            m_header     = sizes.cbHeader;
            m_trailer    = sizes.cbTrailer;
            m_maxMessage = sizes.cbMaximumMessage;
            m_active     = true;
        }
    }

    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&prev), sizeof(prev));
    if (!ok) Release();
    return ok;
}

bool Session::ConnectHandshake(SOCKET s, const std::wstring &expectedFingerprint,
                               std::wstring &actualFingerprintOut,
                               std::wstring &errorOut) {
    if (!EnsureClientCred(errorOut)) return false;

    m_isServer = false;

    DWORD prev = 0;
    int   prevLen = sizeof(prev);
    getsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char *>(&prev), &prevLen);
    DWORD hs = RT::TLS_HANDSHAKE_TIMEOUT_MS;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&hs), sizeof(hs));

    bool ok    = false;
    bool first = true;

    for (int step = 0; step < RT::TLS_HANDSHAKE_MAX_STEPS; ++step) {
        SecBuffer in[2]{};
        in[0].BufferType = SECBUFFER_TOKEN;
        in[0].pvBuffer   = m_incoming.data();
        in[0].cbBuffer   = static_cast<unsigned long>(m_incoming.size());
        in[1].BufferType = SECBUFFER_EMPTY;
        SecBufferDesc inDesc{SECBUFFER_VERSION, 2, in};

        SecBuffer out[1]{};
        out[0].BufferType = SECBUFFER_TOKEN;
        SecBufferDesc outDesc{SECBUFFER_VERSION, 1, out};

        unsigned long attrs = 0;
        // No target name is supplied, and none would mean anything: the server
        // is identified by a PINNED KEY, not by a name in a certificate that
        // nobody authoritative signed. Passing a hostname would only invite
        // Schannel to check something that carries no weight here.
        const SECURITY_STATUS st = InitializeSecurityContextW(
            &g_clientCred,
            first ? nullptr : &m_ctx,
            nullptr,
            ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT |
                ISC_REQ_CONFIDENTIALITY | ISC_REQ_EXTENDED_ERROR |
                ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM |
                ISC_REQ_MANUAL_CRED_VALIDATION,
            0, 0,
            first ? nullptr : &inDesc,
            0,
            first ? &m_ctx : nullptr,
            &outDesc, &attrs, nullptr);

        if (st == SEC_E_OK || st == SEC_I_CONTINUE_NEEDED ||
            st == SEC_E_INCOMPLETE_MESSAGE || first)
            m_haveCtx = true;
        first = false;

        if (out[0].cbBuffer && out[0].pvBuffer) {
            const bool sent = SendRaw(s, static_cast<const char *>(out[0].pvBuffer),
                                      out[0].cbBuffer);
            FreeContextBuffer(out[0].pvBuffer);
            if (!sent) {
                errorOut = Constants::Messages::REMOTE_TLS_HANDSHAKE_FAILED;
                break;
            }
        }

        if (st == SEC_E_INCOMPLETE_MESSAGE || st == SEC_I_CONTINUE_NEEDED) {
            if (st == SEC_I_CONTINUE_NEEDED && in[1].BufferType == SECBUFFER_EXTRA)
                m_incoming.erase(0, m_incoming.size() - in[1].cbBuffer);
            else if (st == SEC_I_CONTINUE_NEEDED)
                m_incoming.clear();

            if (!RecvRaw(s, m_incoming)) {
                errorOut = Constants::Messages::REMOTE_TLS_HANDSHAKE_FAILED;
                break;
            }
            continue;
        }

        if (st == SEC_E_OK) {
            if (in[1].BufferType == SECBUFFER_EXTRA)
                m_incoming.erase(0, m_incoming.size() - in[1].cbBuffer);
            else
                m_incoming.clear();
            ok = true;
            break;
        }

        errorOut = SecErr(Constants::Messages::REMOTE_TLS_HANDSHAKE_FAILED, st);
        break;
    }

    // ── The pin check. The whole security of this connection is this block. ──
    if (ok) {
        PCCERT_CONTEXT peer = nullptr;
        if (QueryContextAttributes(&m_ctx, SECPKG_ATTR_REMOTE_CERT_CONTEXT,
                                   &peer) != SEC_E_OK || !peer) {
            errorOut = L"The server presented no certificate.";
            ok = false;
        } else {
            actualFingerprintOut = FingerprintOf(peer);
            CertFreeCertificateContext(peer);

            if (expectedFingerprint.empty()) {
                // No pin stored yet. REFUSED, and the fingerprint reported so
                // the user can accept it deliberately. Trusting it here would
                // make the first connection — the only one an attacker needs to
                // be present for — the unprotected one.
                errorOut = Constants::Messages::REMOTE_TLS_NO_PIN;
                ok = false;
            } else if (_wcsicmp(expectedFingerprint.c_str(),
                                actualFingerprintOut.c_str()) != 0) {
                errorOut = Constants::Messages::REMOTE_TLS_PIN_MISMATCH;
                ok = false;
            }
        }
    }

    if (ok) {
        SecPkgContext_StreamSizes sizes{};
        if (QueryContextAttributes(&m_ctx, SECPKG_ATTR_STREAM_SIZES, &sizes) != SEC_E_OK) {
            errorOut = L"TLS stream sizes unavailable.";
            ok = false;
        } else {
            m_header     = sizes.cbHeader;
            m_trailer    = sizes.cbTrailer;
            m_maxMessage = sizes.cbMaximumMessage;
            m_active     = true;
        }
    }

    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&prev), sizeof(prev));
    if (!ok) Release();
    return ok;
}

bool Session::Send(SOCKET s, const char *data, size_t len) {
    std::lock_guard<std::mutex> lk(m_ctxMutex);
    return SendLocked(s, data, len);
}

bool Session::TrySend(SOCKET s, const char *data, size_t len) {
    std::unique_lock<std::mutex> lk(m_ctxMutex, std::try_to_lock);
    if (!lk.owns_lock()) return false;   // busy — the caller drops the event
    return SendLocked(s, data, len);
}

bool Session::SendLocked(SOCKET s, const char *data, size_t len) {
    if (!m_active) return false;

    size_t off = 0;
    std::vector<char> rec;
    while (off < len) {
        const unsigned long chunk =
            static_cast<unsigned long>(std::min<size_t>(len - off, m_maxMessage));

        rec.assign(m_header + chunk + m_trailer, 0);
        memcpy(rec.data() + m_header, data + off, chunk);

        SecBuffer b[3]{};
        b[0].BufferType = SECBUFFER_STREAM_HEADER;
        b[0].pvBuffer   = rec.data();
        b[0].cbBuffer   = m_header;
        b[1].BufferType = SECBUFFER_DATA;
        b[1].pvBuffer   = rec.data() + m_header;
        b[1].cbBuffer   = chunk;
        b[2].BufferType = SECBUFFER_STREAM_TRAILER;
        b[2].pvBuffer   = rec.data() + m_header + chunk;
        b[2].cbBuffer   = m_trailer;
        SecBufferDesc desc{SECBUFFER_VERSION, 3, b};

        if (EncryptMessage(&m_ctx, 0, &desc, 0) != SEC_E_OK) return false;

        // EncryptMessage may produce fewer bytes than the buffers reserved, so
        // the LENGTHS IT REPORTS are what goes on the wire — sending the whole
        // reservation would put padding on the stream and desynchronise it.
        const size_t total = b[0].cbBuffer + b[1].cbBuffer + b[2].cbBuffer;
        if (!SendRaw(s, rec.data(), total)) return false;

        off += chunk;
    }
    return true;
}

bool Session::Recv(SOCKET s, std::string &out) {
    if (!m_active) return false;

    for (;;) {
        // The decrypt half runs UNDER THE LOCK; the blocking read below does
        // not. That split is the whole point: a concurrent Send must never
        // overlap DecryptMessage on the same context, and must never have to
        // wait for a peer that is simply quiet.
        {
            std::lock_guard<std::mutex> lk(m_ctxMutex);

        if (!m_decrypted.empty()) {
            out.append(m_decrypted);
            m_decrypted.clear();
            return true;
        }

        if (!m_incoming.empty()) {
            std::string work = m_incoming;

            SecBuffer b[4]{};
            b[0].BufferType = SECBUFFER_DATA;
            b[0].pvBuffer   = work.data();
            b[0].cbBuffer   = static_cast<unsigned long>(work.size());
            b[1].BufferType = SECBUFFER_EMPTY;
            b[2].BufferType = SECBUFFER_EMPTY;
            b[3].BufferType = SECBUFFER_EMPTY;
            SecBufferDesc desc{SECBUFFER_VERSION, 4, b};

            const SECURITY_STATUS st = DecryptMessage(&m_ctx, &desc, 0, nullptr);

            if (st == SEC_E_OK || st == SEC_I_RENEGOTIATE) {
                for (int i = 0; i < 4; ++i)
                    if (b[i].BufferType == SECBUFFER_DATA && b[i].cbBuffer)
                        m_decrypted.append(static_cast<const char *>(b[i].pvBuffer),
                                           b[i].cbBuffer);

                std::string extra;
                for (int i = 0; i < 4; ++i)
                    if (b[i].BufferType == SECBUFFER_EXTRA && b[i].cbBuffer)
                        extra.assign(static_cast<const char *>(b[i].pvBuffer),
                                     b[i].cbBuffer);
                m_incoming = extra;

                // TLS 1.3 key update, or a 1.2 renegotiation request. Refused
                // rather than serviced: this protocol's connections are short
                // and low-volume, so a rekey is never needed, and accepting one
                // means running a second handshake mid-stream — the most
                // error-prone path in any TLS integration, kept out of a code
                // path that guards a remote-control channel.
                if (st == SEC_I_RENEGOTIATE) return false;

                if (!m_decrypted.empty()) continue;
                // A record carrying no application data (an alert Schannel
                // handled, a 1.3 session ticket). Read on rather than reporting
                // a close.
                if (!m_incoming.empty()) continue;
            } else if (st == SEC_I_CONTEXT_EXPIRED) {
                return false;                       // peer sent close_notify
            } else if (st != SEC_E_INCOMPLETE_MESSAGE) {
                return false;
            }
        }
        }   // ← lock released HERE, before the blocking read below

        // Outside the lock, and only this thread ever touches m_incoming: Recv
        // belongs to one client thread and Send never reads it.
        if (!RecvRaw(s, m_incoming)) return false;
    }
}

void Session::Shutdown(SOCKET s) {
    if (!m_haveCtx) return;

    DWORD type = SCHANNEL_SHUTDOWN;
    SecBuffer b{sizeof(type), SECBUFFER_TOKEN, &type};
    SecBufferDesc desc{SECBUFFER_VERSION, 1, &b};
    if (ApplyControlToken(&m_ctx, &desc) != SEC_E_OK) return;

    SecBuffer out{};
    out.BufferType = SECBUFFER_TOKEN;
    SecBufferDesc outDesc{SECBUFFER_VERSION, 1, &out};
    unsigned long attrs = 0;

    // The close_notify alert is produced by the SAME call that drives the
    // handshake, so which one to use depends on which end this is. Using the
    // server call on a client context silently produces nothing, and the peer
    // then sees an abrupt disconnect instead of a clean close.
    const SECURITY_STATUS st =
        m_isServer
            ? AcceptSecurityContext(&g_serverCred, &m_ctx, nullptr,
                                    ASC_REQ_ALLOCATE_MEMORY | ASC_REQ_STREAM, 0,
                                    nullptr, &outDesc, &attrs, nullptr)
            : InitializeSecurityContextW(&g_clientCred, &m_ctx, nullptr,
                                         ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM,
                                         0, 0, nullptr, 0, nullptr,
                                         &outDesc, &attrs, nullptr);

    if ((st == SEC_E_OK || st == SEC_I_CONTINUE_NEEDED) &&
        out.cbBuffer && out.pvBuffer) {
        SendRaw(s, static_cast<const char *>(out.pvBuffer), out.cbBuffer);
        FreeContextBuffer(out.pvBuffer);
    }
}

} // namespace Remote::Tls
