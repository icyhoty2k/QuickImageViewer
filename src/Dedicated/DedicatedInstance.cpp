#include "DedicatedInstance.h"
#include "DedicatedSettings.h"           // the .ini this instance persists into
#include "AppState.h"                    // app.isDedicated (legacy identity route)
#include "Persistence/RegistryManager.h" // GetExePathW
#include "Platform/Constants.h"
#include "Renderer/IRenderer.h"          // path-keyed cache probe / preload
#include "../../resources/resource.h"    // IDI_APP_ICON / IDI_APP_ICON_DEDICATED
#include <functional>
#include <algorithm>
#include <shlobj.h>
#include <shobjidl.h>

extern AppState app;

namespace Dedicated {

namespace {
    // Kept short so derived mutex / class / registry names stay well inside
    // their respective limits.
    constexpr size_t MAX_NAME_LEN = 48;

    void AppendFlag(std::wstring &s, bool on, const wchar_t *flag) {
        if (!on) return;
        if (!s.empty()) s += L' ';
        s += flag;
    }

    void AppendValue(std::wstring &s, const wchar_t *sw, const std::wstring &v,
                     const wchar_t *joiner = L"=") {
        if (!s.empty()) s += L' ';
        s += sw;
        s += joiner;
        s += v;
    }

    std::wstring Quote(const std::wstring &p) {
        if (p.find(L' ') == std::wstring::npos) return p;
        return L"\"" + p + L"\"";
    }
}

// =============================================================================
// SanitizeInstanceName
// =============================================================================
std::wstring SanitizeInstanceName(const std::wstring &raw) {
    std::wstring out;
    out.reserve(std::min(raw.size(), MAX_NAME_LEN));

    for (wchar_t c : raw) {
        if (out.size() >= MAX_NAME_LEN) break;
        // Letters, digits, dash and underscore only. Spaces become underscores
        // so a friendly name like "Lobby Screen" still yields a usable id.
        if ((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
            (c >= L'0' && c <= L'9') || c == L'-' || c == L'_')
            out += c;
        else if (c == L' ')
            out += L'_';
        // everything else (path separators, quotes, backslashes…) is dropped
    }

    // Collapse to empty when nothing but separators survived — callers treat an
    // empty name as "this is the main instance".
    if (out.find_first_not_of(L"_-") == std::wstring::npos) out.clear();
    return out;
}

// =============================================================================
// Derived identifiers
//
// An empty instance name must reproduce the pre-existing identifiers exactly,
// so a normal launch is byte-for-byte unchanged.
// =============================================================================
std::wstring MutexNameFor(const std::wstring &instance) {
    std::wstring name = L"QuickImageViewer_SingleInstanceMutex";
    if (!instance.empty()) name += L"_" + instance;
    return name;
}

std::wstring DefaultMutexName() {
    // Name alone is not enough: two copies with the SAME file name in DIFFERENT
    // folders would derive the same mutex and fight over one instance slot. The
    // folder is folded into a short hash so each copy on disk is distinct, while
    // the same copy still resolves to the same value on every launch.
    std::wstring full = Persistence::Registry::GetExePathW();
    std::transform(full.begin(), full.end(), full.begin(), ::towlower); // paths are case-insensitive

    const size_t h64 = std::hash<std::wstring>{}(full);
    const unsigned h32 = static_cast<unsigned>(h64 ^ (h64 >> 32));

    wchar_t suffix[16];
    swprintf_s(suffix, L"_%08X", h32);

    // Sanitised: a mutex name cannot contain a backslash — it would be read as
    // a namespace separator.
    return MutexNameFor(SanitizeInstanceName(ExeStemName()) + suffix);
}

std::wstring ResolveMutexName() {
    // 1. Explicit override wins — lets two copies be forced to share or split a
    //    slot, and survives renaming the exe.
    const std::wstring override_ = ReadInstanceMutex();
    if (!override_.empty()) return override_;

    // 2. Otherwise derive it from this exe.
    return DefaultMutexName();
}

std::wstring ResolveWindowClassName() {
    if (!SettingsUseFile()) return Constants::WINDOW_CLASS_NAME; // main app, unchanged

    std::wstring key = State().config.name;
    if (key.empty()) key = SanitizeInstanceName(ExeStemName());
    return WindowClassNameFor(key);
}

std::wstring WindowClassNameFor(const std::wstring &instance) {
    std::wstring name = Constants::WINDOW_CLASS_NAME;
    // A DIFFERENT class per instance is what stops FindWindowW() — used by the
    // single-instance shell-forwarding path — from ever handing a double-clicked
    // file to a dedicated slideshow instead of the main window.
    if (!instance.empty()) name += L"_" + instance;
    return name;
}

std::wstring RegistryPrefixFor(const std::wstring &instance) {
    if (instance.empty()) return L"";
    return std::wstring(Constants::DedicatedMode::DEDICATED_MODE_GLOBAL_PREFIX) +
           instance + L"_";
}

std::wstring FilePrefixFor(const std::wstring &instance) {
    if (instance.empty()) return L"";
    return std::wstring(Constants::DedicatedMode::DEDICATED_MODE_GLOBAL_PREFIX) +
           instance + L"_";
}

std::wstring RunValueNameFor(const std::wstring &instance) {
    if (instance.empty()) return Constants::Registry::RUN_VALUE_NAME;
    return std::wstring(Constants::DedicatedMode::DEDICATED_MODE_GLOBAL_PREFIX) +
           instance + L"_" + Constants::Registry::RUN_VALUE_NAME;
}

// =============================================================================
// BuildCommandLine
// =============================================================================
std::wstring BuildCommandLine(const InstanceConfig &cfg) {
    namespace D = Constants::Dedicated;
    std::wstring s;

    AppendValue(s, L"-instance", Quote(cfg.name));
    if (!cfg.description.empty())
        AppendValue(s, L"-instanceDesc", Quote(cfg.description));

    if (cfg.monitorNum >= 1)
        AppendValue(s, L"-monitorNum", std::to_wstring(cfg.monitorNum), L"#");

    AppendFlag(s, cfg.fullscreen, L"-fullscreen");
    AppendFlag(s, cfg.hideMouse,  L"-hideMouse");

    if (!cfg.imageFolder.empty())
        AppendValue(s, L"-startFolder", Quote(cfg.imageFolder), L" ");

    AppendFlag(s, cfg.slideshow, L"-slideshow");
    AppendFlag(s, cfg.loop,      L"-repeat");
    if (cfg.intervalSeconds > 0)
        AppendValue(s, L"-slideshowInterval", std::to_wstring(cfg.intervalSeconds), L" ");

    if (cfg.HasPromotions()) {
        AppendValue(s, L"-promoFolder", Quote(cfg.promotionFolder), L" ");
        AppendValue(s, L"-promoOrder",
                    cfg.promoOrder == D::PromoOrder::SEQUENTIAL ? L"sequential" : L"weighted");
        // Each trigger is emitted only when armed. "5-0" is a strict cadence,
        // "5-15" a random one — the same encoding the config uses.
        if (cfg.promoImagesFrom > 0)
            AppendValue(s, L"-promoEveryImages",
                        std::to_wstring(cfg.promoImagesFrom) + L"-" +
                        std::to_wstring(cfg.promoImagesTo));
        if (cfg.promoTimeFrom > 0)
            AppendValue(s, L"-promoEverySeconds",
                        std::to_wstring(cfg.promoTimeFrom) + L"-" +
                        std::to_wstring(cfg.promoTimeTo));
    }
    return s;
}

// =============================================================================
// Config persistence
//
// Stored alongside the mirrored app settings in the same .ini. Keys are
// prefixed so they cannot clash with the Constants::Registry::* names the main
// settings use.
// =============================================================================
namespace {
    constexpr const wchar_t *K_IMAGE_FOLDER = L"qivDedImageFolder";
    constexpr const wchar_t *K_PROMO_FOLDER = L"qivDedPromoFolder";
    constexpr const wchar_t *K_PROMO_ORDER  = L"qivDedPromoOrder";
    constexpr const wchar_t *K_PROMO_IMG_FROM = L"qivDedPromoImagesFrom";
    constexpr const wchar_t *K_PROMO_IMG_TO   = L"qivDedPromoImagesTo";
    constexpr const wchar_t *K_PROMO_SEC_FROM = L"qivDedPromoTimeFrom";
    constexpr const wchar_t *K_PROMO_SEC_TO   = L"qivDedPromoTimeTo";
    constexpr const wchar_t *K_MONITOR    = L"qivDedMonitor";
    constexpr const wchar_t *K_FULLSCREEN = L"qivDedFullscreen";
    constexpr const wchar_t *K_SLIDESHOW  = L"qivDedSlideshow";
    constexpr const wchar_t *K_LOOP       = L"qivDedLoop";
    constexpr const wchar_t *K_HIDEMOUSE  = L"qivDedHideMouse";
    constexpr const wchar_t *K_INTERVAL   = L"qivDedIntervalSec";
}

void SaveConfig(const InstanceConfig &cfg) {
    EnsureSettingsFile(cfg.name, cfg.description);
    // Pin the mutex explicitly so this copy's single-instance identity survives
    // even if the exe is later renamed.
    WriteInstanceMutex(DefaultMutexName());

    WriteString(K_IMAGE_FOLDER, cfg.imageFolder);
    WriteString(K_PROMO_FOLDER, cfg.promotionFolder);
    WriteDword(K_PROMO_ORDER,    static_cast<DWORD>(cfg.promoOrder));
    WriteDword(K_PROMO_IMG_FROM, static_cast<DWORD>(cfg.promoImagesFrom));
    WriteDword(K_PROMO_IMG_TO,   static_cast<DWORD>(cfg.promoImagesTo));
    WriteDword(K_PROMO_SEC_FROM, static_cast<DWORD>(cfg.promoTimeFrom));
    WriteDword(K_PROMO_SEC_TO,   static_cast<DWORD>(cfg.promoTimeTo));
    WriteDword(K_MONITOR,    static_cast<DWORD>(cfg.monitorNum));
    WriteDword(K_FULLSCREEN, cfg.fullscreen ? 1u : 0u);
    WriteDword(K_SLIDESHOW,  cfg.slideshow  ? 1u : 0u);
    WriteDword(K_LOOP,       cfg.loop       ? 1u : 0u);
    WriteDword(K_HIDEMOUSE,  cfg.hideMouse  ? 1u : 0u);
    WriteDword(K_INTERVAL,   static_cast<DWORD>(cfg.intervalSeconds));
}

namespace {
    // Direct profile access against an explicit path — the DedicatedSettings
    // helpers are bound to THIS process's .ini, which is exactly what we are
    // not targeting here.
    void PutStr(const std::wstring &ini, const wchar_t *sec, const wchar_t *key,
                const std::wstring &v) {
        WritePrivateProfileStringW(sec, key, v.c_str(), ini.c_str());
    }
    void PutInt(const std::wstring &ini, const wchar_t *sec, const wchar_t *key, int v) {
        PutStr(ini, sec, key, std::to_wstring(v));
    }
    int GetInt(const std::wstring &ini, const wchar_t *sec, const wchar_t *key, int def) {
        return GetPrivateProfileIntW(sec, key, def, ini.c_str());
    }
    std::wstring GetStr(const std::wstring &ini, const wchar_t *sec, const wchar_t *key) {
        std::wstring buf(Constants::MAX_FILE_PATH, L'\0');
        const DWORD n = GetPrivateProfileStringW(sec, key, L"", buf.data(),
                                                 static_cast<DWORD>(buf.size()), ini.c_str());
        buf.resize(n);
        return buf;
    }

    // A new file must start as UTF-16LE or the profile API silently writes ANSI
    // and mangles non-ASCII folder paths.
    void CreateIniIfMissing(const std::wstring &ini) {
        HANDLE h = CreateFileW(ini.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                               CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        std::wstring head;
        head += static_cast<wchar_t>(0xFEFF);
        head += L"; QuickImageViewer - dedicated instance settings\r\n";
        head += L"; This instance never reads or writes the registry.\r\n\r\n";
        head += L"[Instance]\r\nName=\r\nDescription=\r\nVersion=\r\nDedicated=\r\nMutex=\r\n\r\n";
        head += L"[Settings]\r\n";
        DWORD w = 0;
        WriteFile(h, head.data(), static_cast<DWORD>(head.size() * sizeof(wchar_t)), &w, nullptr);
        CloseHandle(h);
    }

    constexpr const wchar_t *SEC_INSTANCE = L"Instance";
    constexpr const wchar_t *SEC_SETTINGS = L"Settings";
}

void WriteConfigTo(const std::wstring &ini, const InstanceConfig &cfg) {
    if (ini.empty()) return;
    CreateIniIfMissing(ini);

    PutStr(ini, SEC_INSTANCE, L"Name",        cfg.name);
    PutStr(ini, SEC_INSTANCE, L"Description", cfg.description);
    PutStr(ini, SEC_INSTANCE, L"Version",     Constants::APP_VERSION);
    PutStr(ini, SEC_INSTANCE, L"Dedicated",   L"1");
    PutStr(ini, SEC_INSTANCE, L"Mutex",
           MutexNameFor(SanitizeInstanceName(cfg.name)));

    PutStr(ini, SEC_SETTINGS, K_IMAGE_FOLDER, cfg.imageFolder);
    PutStr(ini, SEC_SETTINGS, K_PROMO_FOLDER, cfg.promotionFolder);
    PutInt(ini, SEC_SETTINGS, K_PROMO_ORDER,    cfg.promoOrder);
    PutInt(ini, SEC_SETTINGS, K_PROMO_IMG_FROM, cfg.promoImagesFrom);
    PutInt(ini, SEC_SETTINGS, K_PROMO_IMG_TO,   cfg.promoImagesTo);
    PutInt(ini, SEC_SETTINGS, K_PROMO_SEC_FROM, cfg.promoTimeFrom);
    PutInt(ini, SEC_SETTINGS, K_PROMO_SEC_TO,   cfg.promoTimeTo);
    PutInt(ini, SEC_SETTINGS, K_MONITOR,    cfg.monitorNum);
    PutInt(ini, SEC_SETTINGS, K_FULLSCREEN, cfg.fullscreen ? 1 : 0);
    PutInt(ini, SEC_SETTINGS, K_SLIDESHOW,  cfg.slideshow  ? 1 : 0);
    PutInt(ini, SEC_SETTINGS, K_LOOP,       cfg.loop       ? 1 : 0);
    PutInt(ini, SEC_SETTINGS, K_HIDEMOUSE,  cfg.hideMouse  ? 1 : 0);
    PutInt(ini, SEC_SETTINGS, K_INTERVAL,   cfg.intervalSeconds);
}

bool ReadConfigFrom(const std::wstring &ini, InstanceConfig &cfg) {
    if (ini.empty()) return false;
    const DWORD attr = GetFileAttributesW(ini.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) return false;

    namespace D = Constants::Dedicated;
    cfg.name        = GetStr(ini, SEC_INSTANCE, L"Name");
    cfg.description = GetStr(ini, SEC_INSTANCE, L"Description");

    cfg.imageFolder     = GetStr(ini, SEC_SETTINGS, K_IMAGE_FOLDER);
    cfg.promotionFolder = GetStr(ini, SEC_SETTINGS, K_PROMO_FOLDER);
    cfg.promoOrder      = GetInt(ini, SEC_SETTINGS, K_PROMO_ORDER, D::PromoOrder::WEIGHTED);
    cfg.promoImagesFrom = GetInt(ini, SEC_SETTINGS, K_PROMO_IMG_FROM, D::PROMO_IMAGES_EVERY_DEFAULT);
    cfg.promoImagesTo   = GetInt(ini, SEC_SETTINGS, K_PROMO_IMG_TO,   D::PROMO_IMAGES_UPTO_DEFAULT);
    cfg.promoTimeFrom   = GetInt(ini, SEC_SETTINGS, K_PROMO_SEC_FROM, 0);
    cfg.promoTimeTo     = GetInt(ini, SEC_SETTINGS, K_PROMO_SEC_TO,   0);
    cfg.monitorNum      = GetInt(ini, SEC_SETTINGS, K_MONITOR, 0);
    cfg.fullscreen      = GetInt(ini, SEC_SETTINGS, K_FULLSCREEN, 1) != 0;
    cfg.slideshow       = GetInt(ini, SEC_SETTINGS, K_SLIDESHOW,  1) != 0;
    cfg.loop            = GetInt(ini, SEC_SETTINGS, K_LOOP,       1) != 0;
    cfg.hideMouse       = GetInt(ini, SEC_SETTINGS, K_HIDEMOUSE,  1) != 0;
    cfg.intervalSeconds = GetInt(ini, SEC_SETTINGS, K_INTERVAL,   0);
    return true;
}

void LoadConfig(InstanceConfig &cfg) {
    namespace D = Constants::Dedicated;

    cfg.name        = ReadInstanceName();
    cfg.description = ReadInstanceDescription();

    cfg.imageFolder     = ReadString(K_IMAGE_FOLDER);
    cfg.promotionFolder = ReadString(K_PROMO_FOLDER);

    cfg.promoOrder = static_cast<int>(ReadDword(K_PROMO_ORDER,
                                                static_cast<DWORD>(D::PromoOrder::WEIGHTED)));
    cfg.promoImagesFrom = static_cast<int>(ReadDword(K_PROMO_IMG_FROM, D::PROMO_IMAGES_EVERY_DEFAULT));
    cfg.promoImagesTo   = static_cast<int>(ReadDword(K_PROMO_IMG_TO,   D::PROMO_IMAGES_UPTO_DEFAULT));
    cfg.promoTimeFrom   = static_cast<int>(ReadDword(K_PROMO_SEC_FROM, D::PROMO_SECONDS_EVERY_DEFAULT));
    cfg.promoTimeTo     = static_cast<int>(ReadDword(K_PROMO_SEC_TO,   D::PROMO_SECONDS_UPTO_DEFAULT));

    cfg.monitorNum      = static_cast<int>(ReadDword(K_MONITOR, 0));
    cfg.fullscreen      = ReadDword(K_FULLSCREEN, 1) != 0;
    cfg.slideshow       = ReadDword(K_SLIDESHOW,  1) != 0;
    cfg.loop            = ReadDword(K_LOOP,       1) != 0;
    cfg.hideMouse       = ReadDword(K_HIDEMOUSE,  1) != 0;
    cfg.intervalSeconds = static_cast<int>(ReadDword(K_INTERVAL, 0));
}

// =============================================================================
// CreateInstanceShortcut
// =============================================================================
bool CreateInstanceShortcut(const InstanceConfig &cfg, const std::wstring &targetDir,
                            std::wstring &outPath) {
    outPath.clear();
    if (!cfg.IsValid() || targetDir.empty()) return false;

    const std::wstring exe = Persistence::Registry::GetExePathW();
    if (exe.empty()) return false;

    // Named after the instance so several shortcuts coexist in one folder —
    // dropping them all into shell:startup is the intended workflow.
    std::wstring lnk = targetDir;
    if (!lnk.empty() && lnk.back() != L'\\' && lnk.back() != L'/') lnk += L'\\';
    lnk += L"qIV - " + cfg.name + L".lnk";

    IShellLinkW *link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&link))) || !link)
        return false;

    std::wstring workDir = exe;
    const size_t slash = workDir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) workDir.resize(slash);

    const std::wstring args = BuildCommandLine(cfg);
    link->SetPath(exe.c_str());
    link->SetArguments(args.c_str());
    link->SetWorkingDirectory(workDir.c_str());
    link->SetIconLocation(exe.c_str(), 0);
    link->SetDescription(cfg.description.empty() ? cfg.name.c_str()
                                                 : cfg.description.c_str());

    IPersistFile *pf = nullptr;
    HRESULT hr = link->QueryInterface(IID_PPV_ARGS(&pf));
    if (SUCCEEDED(hr)) {
        hr = pf->Save(lnk.c_str(), TRUE);
        pf->Release();
    }
    link->Release();

    if (FAILED(hr)) return false;
    outPath = lnk;
    return true;
}

// =============================================================================
// State
// =============================================================================
RuntimeState &State() {
    static RuntimeState s;
    return s;
}

bool IsDedicatedProcess() {
    // Either identity route counts: a named -instance, or the legacy -dedicated
    // flag that predates named instances.
    return State().active || app.isDedicated;
}

UINT AppIconId() {
    return IsDedicatedProcess() ? IDI_APP_ICON_DEDICATED : IDI_APP_ICON;
}

// =============================================================================
// Promotions runtime
// =============================================================================
void InitPromotions() {
    RuntimeState &st = State();
    if (!st.active) return;

    const InstanceConfig &cfg = st.config;
    if (cfg.promotionFolder.empty()) {
        st.promotions.Clear();
        return;
    }

    st.promotions.SetOrder(cfg.promoOrder);
    st.promotions.SetImageTrigger(cfg.promoImagesFrom, cfg.promoImagesTo);
    st.promotions.SetTimeTrigger(cfg.promoTimeFrom, cfg.promoTimeTo);
    st.promotions.Scan(cfg.promotionFolder); // Scan re-arms and decides the first pick
}

void PreloadUpcomingPromotion(HWND /*hWnd*/) {
    RuntimeState &st = State();
    if (!st.active || !app.renderer) return;

    const PromotionEntry *next = st.promotions.PeekNext();
    if (!next) return;

    // Guarded by the playlist INDEX (the neighbour-preload contract), not by
    // wantedPathHash: the main-image guard would cancel this immediately, since
    // a promotion is deliberately never the "wanted" playlist image.
    (void) app.renderer->PreloadBitmap(next->path, app.currentIndex, app.currentIndex);
}

bool ShowNextPromotion(HWND hWnd) {
    RuntimeState &st = State();
    if (!st.active || !app.renderer) return false;

    const PromotionEntry *entry = st.promotions.TakeNext();
    if (!entry) return false;
    const std::wstring path = entry->path; // copy: TakeNext already moved the cursor

    // Cache probe. A hit makes this path the active bitmap — the same mechanism
    // LoadImageIndex uses on a cache hit, just keyed by path instead of index.
    if (FAILED(app.renderer->LoadBitmap(nullptr, 0, 0, path))) {
        // Not decoded yet. Warm it and skip this round rather than stalling the
        // slideshow; it will be ready the next time one is due.
        (void) app.renderer->PreloadBitmap(path, app.currentIndex, app.currentIndex);
        return false;
    }

    st.showingPromotion = true;
    st.promoPath = path;

    // A promotion is a full-frame message: show it clean, ignoring any pan/zoom
    // the images were using.
    app.viewport = ViewportState{};
    InvalidateRect(hWnd, nullptr, FALSE);

    PreloadUpcomingPromotion(hWnd); // keep exactly one ready
    return true;
}

} // namespace Dedicated
