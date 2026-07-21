#include "DedicatedWnd.h"
#include "DedicatedSettings.h"
#include "DedicatedLists.h"
#include "AppState.h"
#include "Persistence/RegistryManager.h" // GetExePathW
#include "Platform/Constants.h"
#include "Platform/ConstantsStrings.h"
#include "UI/ThemedDialog.h"
#include <shlobj.h>
#include <shobjidl.h>
#include <algorithm>
#include <windowsx.h>

extern AppState app;

namespace UI {

namespace {
    constexpr int PANEL_W  = 720;
    constexpr int PANEL_H  = 640;
    constexpr int PAD      = 14;
    constexpr int ROW_H    = 28;
    constexpr int HDR_H    = 30;
    constexpr int BTN_H    = 34;
    constexpr int BTN_GAP  = 8;
    constexpr int TITLE_H  = 44;
    constexpr int FOOTER_H = 24;

    enum ButtonId { BTN_GENERATE = 1, BTN_ADD_STARTUP, BTN_REMOVE_STARTUP, BTN_TEST };

    // Row ids — the edit dispatch switches on these.
    enum RowId {
        R_NONE = 0,
        // Instance
        R_NAME, R_DESC, R_DEDICATED_DIR,
        // Content
        R_IMAGE_FOLDER, R_PROMO_FOLDER,
        // Promotions
        R_PROMO_ORDER, R_PROMO_IMAGES, R_PROMO_SECONDS,
        // Presentation
        R_MONITOR, R_FULLSCREEN, R_SLIDESHOW, R_LOOP, R_SHUFFLE, R_HIDEMOUSE, R_INTERVAL,
        // Slideshow detail
        R_TRANS_TYPE, R_TRANS_SOURCE, R_TRANS_ORDER,
        // View / window
        R_VIEWMODE, R_BASE_W, R_BASE_H, R_START_FULLSCREEN, R_ALWAYS_TOP,
        // Overlays
        R_OVERLAY_VISIBLE, R_OVERLAY_BG, R_MSG_MS,
        // Sorting
        R_SORT_ORDER, R_SORT_REVERSE,
        // Performance
        R_VRAM, R_LOOKASIDE, R_THUMB_CACHE,
        // Input
        R_SWAP_MOUSE, R_WHEEL_INV, R_WHEEL_INV_H, R_ZOOM_CLICK, R_CARET, R_CTRL_C,
        R_CONTEXT_MENU,
        // Misc
        R_KEEP_BG, R_THUMB_FX, R_OPEN_DIR_ON_START, R_THEME,
    };

    bool BgIsDark(COLORREF bg) {
        const int lum = (GetRValue(bg) * 299 + GetGValue(bg) * 587 + GetBValue(bg) * 114) / 1000;
        return lum < 128;
    }

    std::wstring OnOff(bool b) { return b ? L"On" : L"Off"; }

    std::wstring RangeText(int from, int to, const wchar_t *unit) {
        if (from <= 0) return L"Off";
        if (to <= from) return L"every " + std::to_wstring(from) + L" " + unit;
        return std::to_wstring(from) + L"-" + std::to_wstring(to) + L" " + unit;
    }

    std::wstring Tail(const std::wstring &p, size_t maxLen = 46) {
        if (p.empty()) return L"(not set)";
        if (p.size() <= maxLen) return p;
        return L"…" + p.substr(p.size() - maxLen);
    }

    std::wstring KnownFolder(REFKNOWNFOLDERID id) {
        PWSTR p = nullptr;
        std::wstring out;
        if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p)) && p) out = p;
        if (p) CoTaskMemFree(p);
        return out;
    }

    bool FileExists(const std::wstring &p) {
        if (p.empty()) return false;
        const DWORD a = GetFileAttributesW(p.c_str());
        return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
    }
}

// =============================================================================
// Init / Show
// =============================================================================
void DedicatedWnd::Init(HINSTANCE hInstance, HWND hParent) {
    const float s = app.dpiScale;
    InitFloating(hInstance, hParent, L"qIVDedicatedWnd", L"Dedicated",
                 static_cast<int>(PANEL_W * s), static_cast<int>(PANEL_H * s));
    m_edit.SetMaxLength(160);
    BuildRows();
}

void DedicatedWnd::Init(HINSTANCE hInstance, HWND hParent, int8_t) { Init(hInstance, hParent); }

void DedicatedWnd::Show() {
    // Re-read on EVERY open, not just the first: the panel must always describe
    // the configuration this instance is actually running, so stopping the
    // slideshow and pressing F8 shows the live values rather than whatever was
    // last typed here.
    if (Dedicated::SettingsUseFile()) {
        Dedicated::LoadConfig(m_cfg);
        if (m_cfg.name.empty())
            m_cfg.name = Dedicated::SanitizeInstanceName(Dedicated::InstanceName());

        // Default the target to this exe's own folder, so Generate / Add Startup
        // act on the running instance without asking where it lives.
        if (m_targetFolder.empty()) {
            std::wstring exe = Persistence::Registry::GetExePathW();
            const size_t slash = exe.find_last_of(L"\\/");
            if (slash != std::wstring::npos) {
                exe.resize(slash);
                m_targetFolder = exe;
            }
        }
    }
    CancelTextEdit();
    m_scrollY = 0;
    BuildRows();
    IPanelWindow::Show();
    Repaint();
}

// =============================================================================
// Dialog helpers — the panel is topmost, themed dialogs are not, so without
// dropping topmost first every dialog opens BEHIND the panel.
// =============================================================================
void DedicatedWnd::PushTopmostOff() {
    if (GetHwnd())
        SetWindowPos(GetHwnd(), HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void DedicatedWnd::PopTopmost() {
    if (GetHwnd())
        SetWindowPos(GetHwnd(), HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void DedicatedWnd::DialogMessage(const std::wstring &text, const wchar_t *caption) {
    PushTopmostOff();
    ThemedDialog::Message(GetHwnd(), text.c_str(), caption);
    PopTopmost();
}

bool DedicatedWnd::DialogConfirm(const std::wstring &text, const wchar_t *caption) {
    PushTopmostOff();
    const bool r = ThemedDialog::Confirm(GetHwnd(), text.c_str(), caption);
    PopTopmost();
    return r;
}

int DedicatedWnd::DialogPromptInt(const wchar_t *caption, const wchar_t *label,
                                  int cur, int lo, int hi, int def) {
    PushTopmostOff();
    const int v = ThemedDialog::PromptInt(GetHwnd(), caption, label, cur, lo, hi, def);
    PopTopmost();
    return v;
}

// =============================================================================
// Paths of the copy being managed
// =============================================================================
std::wstring DedicatedWnd::TargetExePath() const {
    if (m_targetFolder.empty() || m_cfg.name.empty()) return {};
    std::wstring dir = m_targetFolder;
    if (dir.back() != L'\\' && dir.back() != L'/') dir += L'\\';
    // The name carries the trigger word, so the copy self-identifies even
    // without a -config switch.
    return dir + L"qIV_dedicated_" + m_cfg.name + L".exe";
}

std::wstring DedicatedWnd::TargetIniPath() const {
    std::wstring exe = TargetExePath();
    if (exe.empty()) return {};
    const size_t dot = exe.find_last_of(L'.');
    if (dot != std::wstring::npos) exe.resize(dot);
    return exe + Constants::Dedicated::SETTINGS_FILE_EXT;
}

std::wstring DedicatedWnd::StartupLinkPath() const {
    const std::wstring startup = KnownFolder(FOLDERID_Startup);
    if (startup.empty() || m_cfg.name.empty()) return {};
    return startup + L"\\qIV - " + m_cfg.name + L".lnk";
}

// =============================================================================
// Actions
// =============================================================================
void DedicatedWnd::DoGenerate() {
    if (m_cfg.name.empty()) {
        DialogMessage(L"Give the instance a name first — the copy, its .ini and its "
                      L"shortcut are all named after it.", L"Dedicated");
        return;
    }
    if (m_targetFolder.empty() &&
        !PickFolder(m_targetFolder, L"Where should the dedicated copy be created?"))
        return;

    const std::wstring srcExe = Persistence::Registry::GetExePathW();
    const std::wstring dstExe = TargetExePath();
    const std::wstring dstIni = TargetIniPath();
    if (srcExe.empty() || dstExe.empty()) {
        DialogMessage(L"Could not resolve the target paths.", L"Generate");
        return;
    }

    if (FileExists(dstExe) &&
        !DialogConfirm(L"A copy already exists there:\n\n" + dstExe +
                       L"\n\nOverwrite it?", L"Generate"))
        return;

    if (!CopyFileW(srcExe.c_str(), dstExe.c_str(), FALSE)) {
        DialogMessage(L"Could not copy the executable to:\n\n" + dstExe +
                      L"\n\nCheck the folder is writable.", L"Generate");
        return;
    }

    // Write the .ini beside the copy. SaveConfig targets THIS process's file, so
    // the config is written directly to the new location instead.
    Dedicated::WriteConfigTo(dstIni, m_cfg);

    DialogMessage(L"Dedicated copy created:\n\n" + dstExe + L"\n\nConfig:\n" + dstIni +
                  L"\n\nUse \"Add Startup\" to have it launch with Windows.",
                  L"Generate");
    BuildRows();
    Repaint();
}

void DedicatedWnd::DoAddStartup() {
    const std::wstring exe = TargetExePath();
    const std::wstring ini = TargetIniPath();
    const std::wstring lnk = StartupLinkPath();

    if (exe.empty() || lnk.empty()) {
        DialogMessage(L"Set a name and generate the copy first.", L"Add Startup");
        return;
    }
    if (!FileExists(exe)) {
        DialogMessage(L"The dedicated copy does not exist yet:\n\n" + exe +
                      L"\n\nRun Generate first.", L"Add Startup");
        return;
    }

    // Target:  <copy>.exe -dedicated -config "<copy>.ini"
    // -config is what lets the shortcut point at a config living anywhere.
    const std::wstring args = L"-dedicated -config \"" + ini + L"\"";

    IShellLinkW *link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&link))) || !link) {
        DialogMessage(L"Could not create the shortcut object.", L"Add Startup");
        return;
    }
    std::wstring workDir = exe;
    const size_t slash = workDir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) workDir.resize(slash);

    link->SetPath(exe.c_str());
    link->SetArguments(args.c_str());
    link->SetWorkingDirectory(workDir.c_str());
    link->SetIconLocation(exe.c_str(), 0);
    link->SetDescription(m_cfg.description.empty() ? m_cfg.name.c_str()
                                                   : m_cfg.description.c_str());

    IPersistFile *pf = nullptr;
    HRESULT hr = link->QueryInterface(IID_PPV_ARGS(&pf));
    if (SUCCEEDED(hr)) { hr = pf->Save(lnk.c_str(), TRUE); pf->Release(); }
    link->Release();

    if (SUCCEEDED(hr))
        DialogMessage(L"Startup shortcut created:\n\n" + lnk + L"\n\nTarget:\n" +
                      exe + L"\n" + args, L"Add Startup");
    else
        DialogMessage(L"Failed to write the shortcut.", L"Add Startup");
    BuildRows();
    Repaint();
}

void DedicatedWnd::DoRemoveStartup() {
    const std::wstring lnk = StartupLinkPath();
    if (lnk.empty()) {
        DialogMessage(L"Set a name first — the shortcut is named after it.",
                      L"Remove Startup");
        return;
    }
    if (!FileExists(lnk)) {
        DialogMessage(L"No startup shortcut found for this instance:\n\n" + lnk,
                      L"Remove Startup");
        return;
    }
    if (DeleteFileW(lnk.c_str()))
        DialogMessage(L"Startup shortcut removed:\n\n" + lnk, L"Remove Startup");
    else
        DialogMessage(L"Could not delete:\n\n" + lnk, L"Remove Startup");
    BuildRows();
    Repaint();
}

void DedicatedWnd::DoTest() {
    const std::wstring ini = PickIniFile(false);
    if (ini.empty()) return;

    std::wstring problems;
    Dedicated::InstanceConfig cfg;
    if (!Dedicated::ReadConfigFrom(ini, cfg))
        problems += L"\n  • the file could not be read";

    if (cfg.name.empty())
        problems += L"\n  • [Instance]Name is empty — identity comes from it";
    if (cfg.imageFolder.empty())
        problems += L"\n  • no images folder set";
    else if (GetFileAttributesW(cfg.imageFolder.c_str()) == INVALID_FILE_ATTRIBUTES)
        problems += L"\n  • images folder does not exist: " + cfg.imageFolder;

    if (!cfg.promotionFolder.empty() &&
        GetFileAttributesW(cfg.promotionFolder.c_str()) == INVALID_FILE_ATTRIBUTES)
        problems += L"\n  • promotions folder does not exist: " + cfg.promotionFolder;

    if (!cfg.promotionFolder.empty() && cfg.promoImagesFrom <= 0 && cfg.promoTimeFrom <= 0)
        problems += L"\n  • a promotions folder is set but both triggers are off — "
                    L"no promotion will ever show";

    if (cfg.promoImagesTo > 0 && cfg.promoImagesTo < cfg.promoImagesFrom)
        problems += L"\n  • promotion image range is reversed";
    if (cfg.promoTimeTo > 0 && cfg.promoTimeTo < cfg.promoTimeFrom)
        problems += L"\n  • promotion time range is reversed";

    if (problems.empty()) {
        DialogMessage(L"✔ This config looks valid.\n\n" + ini, L"Test Config");
        return;
    }

    if (DialogConfirm(L"⚠ Problems found in:\n" + ini + L"\n" + problems +
                      L"\n\nReplace it with a working config built from the settings "
                      L"currently shown in this panel?", L"Test Config")) {
        Dedicated::WriteConfigTo(ini, m_cfg);
        DialogMessage(L"Rebuilt:\n\n" + ini, L"Test Config");
    }
}

// =============================================================================
// Rows
// =============================================================================
void DedicatedWnd::BuildRows() {
    namespace D  = Constants::Dedicated;
    namespace SS = Constants::Slideshow;
    m_rows.clear();

    auto hdr = [&](const wchar_t *t) {
        m_rows.push_back({Kind::Header, t, L"", R_NONE, {}});
    };
    auto row = [&](Kind k, const wchar_t *label, std::wstring value, int id) {
        m_rows.push_back({k, label, std::move(value), id, {}});
    };

    hdr(L"INSTANCE");
    row(Kind::Text,   L"Name",              m_cfg.name.empty() ? L"(required)" : m_cfg.name, R_NAME);
    row(Kind::Text,   L"Description",       m_cfg.description.empty() ? L"(optional)" : m_cfg.description, R_DESC);
    row(Kind::Folder, L"Create copy in",    Tail(m_targetFolder), R_DEDICATED_DIR);

    hdr(L"CONTENT");
    row(Kind::Folder, L"Images folder",     Tail(m_cfg.imageFolder), R_IMAGE_FOLDER);
    row(Kind::Folder, L"Promotions folder", Tail(m_cfg.promotionFolder), R_PROMO_FOLDER);

    hdr(L"PROMOTIONS");
    row(Kind::Choice, L"Pick",              m_cfg.promoOrder == D::PromoOrder::SEQUENTIAL
                                                ? L"Sequential" : L"Weighted by priority", R_PROMO_ORDER);
    row(Kind::Number, L"Every N images",    RangeText(m_cfg.promoImagesFrom, m_cfg.promoImagesTo, L"images"), R_PROMO_IMAGES);
    row(Kind::Number, L"Every N seconds",   RangeText(m_cfg.promoTimeFrom, m_cfg.promoTimeTo, L"sec"), R_PROMO_SECONDS);

    hdr(L"PRESENTATION");
    row(Kind::Number, L"Monitor",           m_cfg.monitorNum >= 1 ? std::to_wstring(m_cfg.monitorNum) : std::wstring(L"Any"), R_MONITOR);
    row(Kind::Toggle, L"Fullscreen",        OnOff(m_cfg.fullscreen), R_FULLSCREEN);
    row(Kind::Toggle, L"Start slideshow",   OnOff(m_cfg.slideshow), R_SLIDESHOW);
    row(Kind::Toggle, L"Loop",              OnOff(m_cfg.loop), R_LOOP);
    row(Kind::Toggle, L"Shuffle images",    OnOff(app.slideshow.shuffle), R_SHUFFLE);
    row(Kind::Toggle, L"Hide mouse",        OnOff(m_cfg.hideMouse), R_HIDEMOUSE);
    row(Kind::Number, L"Slide interval",    m_cfg.intervalSeconds > 0
                                                ? std::to_wstring(m_cfg.intervalSeconds) + L" sec"
                                                : std::wstring(L"Use saved"), R_INTERVAL);

    hdr(L"TRANSITIONS");
    row(Kind::Choice, L"Transition",        Constants::Messages::TRANSITION_NAMES[
                                                static_cast<int>(app.slideshow.transition.type)], R_TRANS_TYPE);
    row(Kind::Choice, L"Source",            Constants::Messages::TRANSITION_SOURCE_NAMES[
                                                app.slideshow.transition.source], R_TRANS_SOURCE);
    row(Kind::Choice, L"Order",             Constants::Messages::TRANSITION_ORDER_NAMES[
                                                app.slideshow.transition.order], R_TRANS_ORDER);

    hdr(L"VIEW & WINDOW");
    row(Kind::Number, L"View mode",         std::to_wstring(static_cast<int>(app.viewMode)), R_VIEWMODE);
    row(Kind::Number, L"Window width",      std::to_wstring(app.baseWidth), R_BASE_W);
    row(Kind::Number, L"Window height",     std::to_wstring(app.baseHeight), R_BASE_H);
    row(Kind::Toggle, L"Start fullscreen",  OnOff(app.startInFullscreen), R_START_FULLSCREEN);
    row(Kind::Toggle, L"Always on top",     OnOff(app.isAlwaysOnTop), R_ALWAYS_TOP);

    hdr(L"OVERLAYS");
    row(Kind::Toggle, L"Info overlays",     OnOff(app.showOverlayInfoText), R_OVERLAY_VISIBLE);
    row(Kind::Toggle, L"Overlay background", OnOff(app.overlayShowBackground), R_OVERLAY_BG);
    row(Kind::Number, L"Message duration",  std::to_wstring(app.msgCenterDisplayMs) + L" ms", R_MSG_MS);

    hdr(L"SORTING");
    row(Kind::Number, L"Sort order",        std::to_wstring(app.fileHandlerDefaultSortOrder), R_SORT_ORDER);
    row(Kind::Toggle, L"Reverse order",     OnOff(app.fileHandlerIsReverseSortOrder), R_SORT_REVERSE);

    hdr(L"PERFORMANCE");
    row(Kind::Number, L"VRAM cache",        std::to_wstring(app.vramCacheCount), R_VRAM);
    row(Kind::Number, L"Preload lookaside", std::to_wstring(app.preloadLookaside), R_LOOKASIDE);
    row(Kind::Number, L"Thumb cache MB",    std::to_wstring(app.dirThumbCacheMB), R_THUMB_CACHE);

    hdr(L"INPUT");
    row(Kind::Toggle, L"Swap mouse buttons", OnOff(app.swapMouseButtons), R_SWAP_MOUSE);
    row(Kind::Toggle, L"Invert wheel",       OnOff(app.invertWheelDirection), R_WHEEL_INV);
    row(Kind::Toggle, L"Invert h-wheel",     OnOff(app.invertWheelDirectionH), R_WHEEL_INV_H);
    row(Kind::Number, L"Left-click zoom",    std::to_wstring(static_cast<int>(app.zoomClickMultiplier + 0.5f)) + L"x", R_ZOOM_CLICK);
    row(Kind::Choice, L"Caret style",        app.caretStyle == 0 ? L"Bar" : L"Underscore", R_CARET);
    row(Kind::Toggle, L"Ctrl+C copy",        OnOff(app.ctrlCEnabled), R_CTRL_C);
    row(Kind::Toggle, L"Right-click menu",   OnOff(app.contextMenuEnabled), R_CONTEXT_MENU);

    hdr(L"MISC");
    row(Kind::Toggle, L"Keep in background", OnOff(app.isKeepInBackground), R_KEEP_BG);
    row(Kind::Toggle, L"Thumbnail effects",  OnOff(app.thumbnailEffectsEnabled), R_THUMB_FX);
    row(Kind::Toggle, L"Open strip on start", OnOff(app.openDirWndOnStart), R_OPEN_DIR_ON_START);
    row(Kind::Number, L"Theme brightness",   std::to_wstring(static_cast<int>(app.themeFactor * 100)) + L"%", R_THEME);

    // Buttons — Remove Startup only lights up when there is one to remove.
    m_buttons.clear();
    m_buttons.push_back({L"Generate",       BTN_GENERATE, {}, true});
    m_buttons.push_back({L"Add Startup",    BTN_ADD_STARTUP, {}, true});
    m_buttons.push_back({L"Remove Startup", BTN_REMOVE_STARTUP, {}, FileExists(StartupLinkPath())});
    m_buttons.push_back({L"Test",           BTN_TEST, {}, true});

    if (m_selected < 0) m_selected = 0;
    if (m_selected >= static_cast<int>(m_rows.size()))
        m_selected = static_cast<int>(m_rows.size()) - 1;
}

// =============================================================================
// Editing
// =============================================================================
void DedicatedWnd::BeginTextEdit(int rowIndex) {
    m_editingRow = rowIndex;
    const int id = m_rows[rowIndex].id;
    m_edit.SetText(id == R_NAME ? m_cfg.name : m_cfg.description);
    m_edit.SetPlaceholder(id == R_NAME ? L"e.g. Lobby" : L"shown on the shortcut");
    Repaint();
}

void DedicatedWnd::CommitTextEdit() {
    if (m_editingRow < 0) return;
    const int id = m_rows[m_editingRow].id;
    if (id == R_NAME) m_cfg.name = Dedicated::SanitizeInstanceName(m_edit.GetText());
    else              m_cfg.description = m_edit.GetText();
    m_editingRow = -1;
    BuildRows();
    Repaint();
}

void DedicatedWnd::CancelTextEdit() {
    m_editingRow = -1;
    m_edit.Reset();
}

void DedicatedWnd::EditRangePair(int &from, int &to, int maxValue,
                                 const wchar_t *caption, const wchar_t *unit) {
    const std::wstring l1 = std::wstring(L"Show a promotion every N ") + unit +
                            L".\n0 = this trigger is off.";
    const int f = DialogPromptInt(caption, l1.c_str(), std::max(0, from), 0, maxValue, 0);
    if (f < 0) return;
    from = f;
    if (from == 0) { to = 0; return; }

    const std::wstring l2 = std::wstring(L"Upper bound for a RANDOM gap.\n0 = strict: exactly every ") +
                            std::to_wstring(from) + L" " + unit + L".";
    const int t = DialogPromptInt(caption, l2.c_str(), std::max(0, to), 0, maxValue, 0);
    if (t >= 0) to = t;
}

bool DedicatedWnd::PickFolder(std::wstring &inOut, const wchar_t *title) {
    PushTopmostOff();
    IFileOpenDialog *pfd = nullptr;
    bool ok = false;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&pfd))) && pfd) {
        DWORD opts = 0;
        pfd->GetOptions(&opts);
        pfd->SetOptions(opts | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST);
        pfd->SetTitle(title);
        if (SUCCEEDED(pfd->Show(GetHwnd()))) {
            IShellItem *psi = nullptr;
            if (SUCCEEDED(pfd->GetResult(&psi))) {
                PWSTR p = nullptr;
                if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                    inOut = p; CoTaskMemFree(p); ok = true;
                }
                psi->Release();
            }
        }
        pfd->Release();
    }
    PopTopmost();
    return ok;
}

std::wstring DedicatedWnd::PickIniFile(bool save) {
    PushTopmostOff();
    std::wstring result;
    IFileDialog *pfd = nullptr;
    const CLSID clsid = save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog;
    if (SUCCEEDED(CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&pfd))) && pfd) {
        COMDLG_FILTERSPEC f[] = {{L"Config", L"*.ini"}, {L"All Files", L"*.*"}};
        pfd->SetFileTypes(ARRAYSIZE(f), f);
        pfd->SetDefaultExtension(L"ini");
        pfd->SetTitle(save ? L"Save config" : L"Choose a config to test");
        if (!save) {
            DWORD o = 0; pfd->GetOptions(&o);
            pfd->SetOptions(o | FOS_FILEMUSTEXIST);
        }
        if (SUCCEEDED(pfd->Show(GetHwnd()))) {
            IShellItem *psi = nullptr;
            if (SUCCEEDED(pfd->GetResult(&psi))) {
                PWSTR p = nullptr;
                if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                    result = p; CoTaskMemFree(p);
                }
                psi->Release();
            }
        }
        pfd->Release();
    }
    PopTopmost();
    return result;
}

void DedicatedWnd::EditRow(int rowIndex) {
    if (rowIndex < 0 || rowIndex >= static_cast<int>(m_rows.size())) return;
    namespace D  = Constants::Dedicated;
    namespace SS = Constants::Slideshow;
    const Row &r = m_rows[rowIndex];
    if (r.kind == Kind::Header) return;

    auto cycle = [](int v, int count) { return (v + 1) % count; };

    switch (r.id) {
        case R_NAME:
        case R_DESC: BeginTextEdit(rowIndex); return;

        case R_DEDICATED_DIR: PickFolder(m_targetFolder, L"Where to create the copy"); break;
        case R_IMAGE_FOLDER:  PickFolder(m_cfg.imageFolder, L"Images folder"); break;
        case R_PROMO_FOLDER:  PickFolder(m_cfg.promotionFolder, L"Promotions folder"); break;

        case R_PROMO_ORDER:
            m_cfg.promoOrder = (m_cfg.promoOrder == D::PromoOrder::SEQUENTIAL)
                                   ? D::PromoOrder::WEIGHTED : D::PromoOrder::SEQUENTIAL;
            break;
        case R_PROMO_IMAGES:
            EditRangePair(m_cfg.promoImagesFrom, m_cfg.promoImagesTo,
                          D::PROMO_IMAGES_MAX, L"Promotion every N images", L"images");
            break;
        case R_PROMO_SECONDS:
            EditRangePair(m_cfg.promoTimeFrom, m_cfg.promoTimeTo,
                          D::PROMO_SECONDS_MAX, L"Promotion every N seconds", L"seconds");
            break;

        case R_MONITOR: {
            const int v = DialogPromptInt(L"Monitor", L"1-based monitor number.\n0 = any.",
                                          m_cfg.monitorNum, 0, 16, 0);
            if (v >= 0) m_cfg.monitorNum = v;
            break;
        }
        case R_FULLSCREEN: m_cfg.fullscreen = !m_cfg.fullscreen; break;
        case R_SLIDESHOW:  m_cfg.slideshow  = !m_cfg.slideshow;  break;
        case R_LOOP:       m_cfg.loop       = !m_cfg.loop;       break;
        case R_HIDEMOUSE:  m_cfg.hideMouse  = !m_cfg.hideMouse;  break;
        case R_INTERVAL: {
            const int v = DialogPromptInt(L"Slide interval",
                                          L"Seconds between slides.\n0 = use the saved value.",
                                          m_cfg.intervalSeconds, 0, 3600, 0);
            if (v >= 0) m_cfg.intervalSeconds = v;
            break;
        }

        // --- Live app settings: edited here, persisted by the normal path -----
        case R_SHUFFLE: app.slideshow.shuffle = !app.slideshow.shuffle;
            Persistence::Registry::SaveSetting(Constants::Registry::SLIDESHOW_SHUFFLE, app.slideshow.shuffle); break;
        case R_TRANS_TYPE: {
            const int n = cycle(static_cast<int>(app.slideshow.transition.type), SS::TRANSITION_COUNT);
            app.slideshow.transition.type = static_cast<TransitionType>(n);
            Persistence::Registry::SaveSetting(Constants::Registry::SLIDESHOW_TRANSITION, n); break;
        }
        case R_TRANS_SOURCE: {
            const int n = cycle(app.slideshow.transition.source, SS::TransitionSource::COUNT);
            app.slideshow.transition.source = n;
            Persistence::Registry::SaveSetting(Constants::Registry::SLIDESHOW_TRANS_SOURCE, n); break;
        }
        case R_TRANS_ORDER: {
            const int n = cycle(app.slideshow.transition.order, SS::TransitionOrder::COUNT);
            app.slideshow.transition.order = n;
            Persistence::Registry::SaveSetting(Constants::Registry::SLIDESHOW_TRANS_ORDER, n); break;
        }
        case R_VIEWMODE: {
            const int v = DialogPromptInt(L"View mode", L"1 Fit  2 Width  3 Height  4 Stretch  5 Original",
                                          static_cast<int>(app.viewMode), 1, 5, 1);
            if (v >= 1) { app.viewMode = static_cast<Constants::ViewModes::ViewMode>(v);
                          Persistence::Registry::SaveSetting(Constants::Registry::VIEW_MODE, v); }
            break;
        }
        case R_BASE_W: {
            const int v = DialogPromptInt(L"Window width", L"Default width in pixels.", app.baseWidth, 240, 16000, Constants::IS_BASE_WIDTH);
            if (v >= 0) { app.baseWidth = v; Persistence::Registry::SaveSetting(Constants::Registry::BASE_WIDTH_KEY, v); }
            break;
        }
        case R_BASE_H: {
            const int v = DialogPromptInt(L"Window height", L"Default height in pixels.", app.baseHeight, 240, 16000, Constants::IS_BASE_HEIGHT);
            if (v >= 0) { app.baseHeight = v; Persistence::Registry::SaveSetting(Constants::Registry::BASE_HEIGHT_KEY, v); }
            break;
        }
        case R_START_FULLSCREEN: app.startInFullscreen = !app.startInFullscreen;
            Persistence::Registry::SaveSetting(Constants::Registry::START_FULLSCREEN, app.startInFullscreen); break;
        case R_ALWAYS_TOP: app.isAlwaysOnTop = !app.isAlwaysOnTop; break;

        case R_OVERLAY_VISIBLE: app.showOverlayInfoText = !app.showOverlayInfoText;
            Persistence::Registry::SaveSetting(Constants::Registry::OVERLAY_VISIBLE, app.showOverlayInfoText); break;
        case R_OVERLAY_BG: app.overlayShowBackground = !app.overlayShowBackground;
            Persistence::Registry::SaveSetting(Constants::Registry::OVERLAY_SHOW_BG, app.overlayShowBackground); break;
        case R_MSG_MS: {
            const int v = DialogPromptInt(L"Message duration", L"Center message duration in ms.", app.msgCenterDisplayMs, 250, 10000, 2000);
            if (v >= 0) { app.msgCenterDisplayMs = v; Persistence::Registry::SaveSetting(Constants::Registry::MSG_CENTER_MS, v); }
            break;
        }

        case R_SORT_ORDER: {
            const int v = DialogPromptInt(L"Sort order", L"0 Name  1 Date  2 Size  3 Type  4 Disk",
                                          app.fileHandlerDefaultSortOrder, 0, 4, 0);
            if (v >= 0) { app.fileHandlerDefaultSortOrder = v; Persistence::Registry::SaveSetting(Constants::Registry::SORT_ORDER, v); }
            break;
        }
        case R_SORT_REVERSE: app.fileHandlerIsReverseSortOrder = !app.fileHandlerIsReverseSortOrder;
            Persistence::Registry::SaveSetting(Constants::Registry::SORT_REVERSE, app.fileHandlerIsReverseSortOrder); break;

        case R_VRAM: {
            const int v = DialogPromptInt(L"VRAM cache", L"Images kept in VRAM (0-999).", app.vramCacheCount, 0, 999, 20);
            if (v >= 0) { app.vramCacheCount = v; Persistence::Registry::SaveSetting(Constants::Registry::VRAM_CACHE_COUNT, v); }
            break;
        }
        case R_LOOKASIDE: {
            const int v = DialogPromptInt(L"Preload lookaside", L"Images preloaded each way (1-99).", app.preloadLookaside, 1, 99, 2);
            if (v >= 0) { app.preloadLookaside = v; Persistence::Registry::SaveSetting(Constants::Registry::PRELOAD_LOOKASIDE, v); }
            break;
        }
        case R_THUMB_CACHE: {
            const int v = DialogPromptInt(L"Thumb cache", L"Thumbnail cache budget in MB.", app.dirThumbCacheMB, 100, 64000, 2000);
            if (v >= 0) { app.dirThumbCacheMB = v; Persistence::Registry::SaveSetting(Constants::Registry::DIR_THUMB_CACHE_MB, v); }
            break;
        }

        case R_SWAP_MOUSE: app.swapMouseButtons = !app.swapMouseButtons;
            Persistence::Registry::SaveSetting(Constants::Registry::SWAP_MOUSE_BUTTONS, app.swapMouseButtons); break;
        case R_WHEEL_INV: app.invertWheelDirection = !app.invertWheelDirection;
            Persistence::Registry::SaveSetting(Constants::Registry::WHEEL_INVERT, app.invertWheelDirection); break;
        case R_WHEEL_INV_H: app.invertWheelDirectionH = !app.invertWheelDirectionH;
            Persistence::Registry::SaveSetting(Constants::Registry::WHEEL_INVERT_H, app.invertWheelDirectionH); break;
        case R_ZOOM_CLICK: {
            const int v = DialogPromptInt(L"Left-click zoom", L"Multiplier (1 = off .. 10).",
                                          static_cast<int>(app.zoomClickMultiplier + 0.5f), 1, 10, 3);
            if (v >= 0) { app.zoomClickMultiplier = static_cast<float>(v);
                          Persistence::Registry::SaveSetting(Constants::Registry::ZOOM_CLICK_MULT, v); }
            break;
        }
        case R_CARET: app.caretStyle = app.caretStyle == 0 ? 1 : 0;
            Persistence::Registry::SaveSetting(Constants::Registry::INPUTBOX_CARET_STYLE, app.caretStyle); break;
        case R_CTRL_C: app.ctrlCEnabled = !app.ctrlCEnabled;
            Persistence::Registry::SaveSetting(Constants::Registry::CTRL_C_ENABLED, app.ctrlCEnabled); break;
        case R_CONTEXT_MENU: app.contextMenuEnabled = !app.contextMenuEnabled;
            Persistence::Registry::SaveSetting(Constants::Registry::CONTEXT_MENU_ENABLED, app.contextMenuEnabled); break;

        case R_KEEP_BG: app.isKeepInBackground = !app.isKeepInBackground;
            Persistence::Registry::SaveSetting(Constants::Registry::KEEP_IN_BACKGROUND, app.isKeepInBackground); break;
        case R_THUMB_FX: app.thumbnailEffectsEnabled = !app.thumbnailEffectsEnabled;
            Persistence::Registry::SaveSetting(Constants::Registry::THUMBNAIL_EFFECTS, app.thumbnailEffectsEnabled); break;
        case R_OPEN_DIR_ON_START: app.openDirWndOnStart = !app.openDirWndOnStart;
            Persistence::Registry::SaveSetting(Constants::Registry::OPEN_DIRWND_ON_START, app.openDirWndOnStart); break;
        case R_THEME: {
            const int v = DialogPromptInt(L"Theme brightness", L"0 = dark .. 100 = light.",
                                          static_cast<int>(app.themeFactor * 100), 0, 100, 12);
            if (v >= 0) { app.themeFactor = v / 100.0f;
                          Persistence::Registry::SaveSetting(Constants::Registry::THEME_FACTOR, v); }
            break;
        }
        default: break;
    }

    BuildRows();
    Repaint();
}

// =============================================================================
// Keyboard
// =============================================================================
bool DedicatedWnd::OnKeyDown(WPARAM vk, bool, bool, bool) {
    if (m_editingRow >= 0) {
        if (vk == VK_RETURN) { CommitTextEdit(); return true; }
        if (vk == VK_ESCAPE) { CancelTextEdit(); Repaint(); return true; }
        const InputResult r = m_edit.RouteKey(vk, GetHwnd());
        if (r != InputResult::Ignored) {
            if (r == InputResult::ConsumedRepaint) Repaint();
            return true;
        }
        return false;
    }

    auto step = [&](int dir) {
        int i = m_selected;
        // Skip headers — they are not editable.
        for (int n = 0; n < static_cast<int>(m_rows.size()); ++n) {
            i += dir;
            if (i < 0 || i >= static_cast<int>(m_rows.size())) return;
            if (m_rows[i].kind != Kind::Header) { m_selected = i; Repaint(); return; }
        }
    };

    switch (vk) {
        case VK_UP:    step(-1); return true;
        case VK_DOWN:  step(+1); return true;
        case VK_PRIOR: m_scrollY -= m_viewportH; ClampScroll(); Repaint(); return true;
        case VK_NEXT:  m_scrollY += m_viewportH; ClampScroll(); Repaint(); return true;
        case VK_RETURN:
        case VK_SPACE: EditRow(m_selected); return true;
        default: break;
    }
    return false;
}

bool DedicatedWnd::OnLocalHide() {
    if (m_editingRow >= 0) { CancelTextEdit(); Repaint(); return true; }
    return false;
}

// =============================================================================
// Painting
// =============================================================================
void DedicatedWnd::EnsureFonts(HDC dc) {
    const int dpi = GetDeviceCaps(dc, LOGPIXELSY);
    if (m_hFontBody && dpi == m_cachedFontDpi) return;
    if (m_hFontBody)  DeleteObject(m_hFontBody);
    if (m_hFontBold)  DeleteObject(m_hFontBold);
    if (m_hFontSmall) DeleteObject(m_hFontSmall);
    m_cachedFontDpi = dpi;
    auto mk = [&](int pt, int w) {
        return CreateFontW(-MulDiv(pt, dpi, 72), 0, 0, 0, w, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
    };
    m_hFontBody  = mk(10, FW_NORMAL);
    m_hFontBold  = mk(11, FW_SEMIBOLD);
    m_hFontSmall = mk(8,  FW_NORMAL);
}

void DedicatedWnd::EnsureBackBuffer(HDC refDC, int w, int h) {
    if (m_bbDC && m_bbW == w && m_bbH == h) return;
    DestroyBackBuffer();
    m_bbDC = CreateCompatibleDC(refDC);
    m_bbBmp = CreateCompatibleBitmap(refDC, w, h);
    m_bbBmpOld = static_cast<HBITMAP>(SelectObject(m_bbDC, m_bbBmp));
    m_bbW = w; m_bbH = h;
}

void DedicatedWnd::DestroyBackBuffer() {
    if (!m_bbDC) return;
    SelectObject(m_bbDC, m_bbBmpOld);
    DeleteObject(m_bbBmp);
    DeleteDC(m_bbDC);
    m_bbDC = nullptr; m_bbBmp = nullptr; m_bbBmpOld = nullptr; m_bbW = m_bbH = 0;
}

void DedicatedWnd::Repaint() { if (GetHwnd()) InvalidateRect(GetHwnd(), nullptr, FALSE); }

void DedicatedWnd::ClampScroll() {
    const int maxScroll = std::max(0, m_contentH - m_viewportH);
    m_scrollY = std::clamp(m_scrollY, 0, maxScroll);
}

int DedicatedWnd::HitTestRow(POINT pt) const {
    for (size_t i = 0; i < m_rows.size(); ++i)
        if (m_rows[i].kind != Kind::Header && PtInRect(&m_rows[i].rect, pt))
            return static_cast<int>(i);
    return -1;
}

int DedicatedWnd::HitTestButton(POINT pt) const {
    for (size_t i = 0; i < m_buttons.size(); ++i)
        if (m_buttons[i].enabled && PtInRect(&m_buttons[i].rect, pt))
            return static_cast<int>(i);
    return -1;
}

LRESULT DedicatedWnd::HandlePanelMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        // A hand cursor over anything clickable. The panel's window class uses
        // IDC_ARROW, so WM_SETCURSOR must be answered or the class cursor wins.
        case WM_SETCURSOR: {
            if (LOWORD(lParam) != HTCLIENT) break;
            POINT pt; GetCursorPos(&pt);
            ScreenToClient(GetHwnd(), &pt);
            if (HitTestButton(pt) >= 0 || HitTestRow(pt) >= 0) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            return TRUE;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(GetHwnd(), &ps);
            RECT rc; GetClientRect(GetHwnd(), &rc);
            const int W = rc.right - rc.left, H = rc.bottom - rc.top;

            EnsureFonts(dc);
            EnsureBackBuffer(dc, W, H);
            HDC bb = m_bbDC;

            const COLORREF bg = GetBgColor();
            const bool dark = BgIsDark(bg);
            const COLORREF fg     = dark ? RGB(235,235,235) : RGB(24,24,24);
            const COLORREF dim    = dark ? RGB(150,150,150) : RGB(110,110,110);
            const COLORREF selBg  = dark ? RGB(58,86,132)   : RGB(203,222,250);
            const COLORREF hotBg  = dark ? RGB(48,48,52)    : RGB(232,232,236);
            const COLORREF line   = dark ? RGB(64,64,64)    : RGB(220,220,220);
            const COLORREF btnBg  = dark ? RGB(56,56,60)    : RGB(238,238,242);
            const COLORREF btnHot = dark ? RGB(72,96,140)   : RGB(210,226,250);
            const COLORREF hdrCol = dark ? RGB(120,170,230) : RGB(40,90,160);

            HBRUSH b = CreateSolidBrush(bg); FillRect(bb, &rc, b); DeleteObject(b);
            SetBkMode(bb, TRANSPARENT);

            const float s   = app.dpiScale;
            const int pad   = static_cast<int>(PAD * s);
            const int rowH  = static_cast<int>(ROW_H * s);
            const int hdrH  = static_cast<int>(HDR_H * s);
            const int btnH  = static_cast<int>(BTN_H * s);
            const int labelW = static_cast<int>(210 * s);

            // ── Title ────────────────────────────────────────────────────────
            SelectObject(bb, m_hFontBold);
            SetTextColor(bb, fg);
            RECT tr{pad, static_cast<int>(6 * s), W - pad, static_cast<int>(24 * s)};
            DrawTextW(bb, L"Dedicated instance", -1, &tr, DT_LEFT | DT_SINGLELINE);

            // Which file is being edited — without it there is no way to tell a
            // running instance's own config from one being authored for a copy.
            SelectObject(bb, m_hFontSmall);
            SetTextColor(bb, dim);
            RECT sr{pad, tr.bottom, W - pad, tr.bottom + static_cast<int>(16 * s)};
            const std::wstring which =
                Dedicated::SettingsUseFile()
                    ? L"Editing: " + Dedicated::SettingsFilePath()
                    : std::wstring(L"New instance — not yet generated");
            DrawTextW(bb, which.c_str(), -1, &sr,
                      DT_LEFT | DT_SINGLELINE | DT_PATH_ELLIPSIS);

            // ── Buttons ──────────────────────────────────────────────────────
            {
                const int n = static_cast<int>(m_buttons.size());
                const int gap = static_cast<int>(BTN_GAP * s);
                const int total = W - pad * 2 - gap * (n - 1);
                const int bw = total / std::max(1, n);
                int x = pad;
                const int y = static_cast<int>(TITLE_H * s);

                SelectObject(bb, m_hFontBody);
                for (int i = 0; i < n; ++i) {
                    Button &btn = m_buttons[i];
                    btn.rect = {x, y, x + bw, y + btnH};
                    HBRUSH bb2 = CreateSolidBrush(
                        !btn.enabled ? bg : (i == m_hotButton ? btnHot : btnBg));
                    FillRect(bb, &btn.rect, bb2);
                    DeleteObject(bb2);

                    HPEN pen = CreatePen(PS_SOLID, 1, line);
                    HGDIOBJ oldPen = SelectObject(bb, pen);
                    HGDIOBJ oldBr  = SelectObject(bb, GetStockObject(NULL_BRUSH));
                    Rectangle(bb, btn.rect.left, btn.rect.top, btn.rect.right, btn.rect.bottom);
                    SelectObject(bb, oldBr); SelectObject(bb, oldPen); DeleteObject(pen);

                    SetTextColor(bb, btn.enabled ? fg : dim);
                    RECT lr = btn.rect;
                    DrawTextW(bb, btn.label.c_str(), -1, &lr,
                              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    x += bw + gap;
                }
            }

            // ── Rows (scrolled) ──────────────────────────────────────────────
            const int listTop = static_cast<int>(TITLE_H * s) + btnH + static_cast<int>(10 * s);
            const int listBot = H - static_cast<int>(FOOTER_H * s);
            m_viewportH = listBot - listTop;

            HRGN clip = CreateRectRgn(0, listTop, W, listBot);
            SelectClipRgn(bb, clip);

            int y = listTop - m_scrollY;
            for (size_t i = 0; i < m_rows.size(); ++i) {
                Row &row = m_rows[i];
                const int h = (row.kind == Kind::Header) ? hdrH : rowH;
                row.rect = {pad, y, W - pad, y + h};

                if (y + h >= listTop && y <= listBot) {
                    if (row.kind == Kind::Header) {
                        SelectObject(bb, m_hFontBold);
                        SetTextColor(bb, hdrCol);
                        RECT lr{row.rect.left, row.rect.top + static_cast<int>(8 * s),
                                row.rect.right, row.rect.bottom};
                        DrawTextW(bb, row.label.c_str(), -1, &lr, DT_LEFT | DT_SINGLELINE);
                    } else {
                        if (static_cast<int>(i) == m_selected) {
                            HBRUSH sb = CreateSolidBrush(selBg);
                            FillRect(bb, &row.rect, sb); DeleteObject(sb);
                        } else if (static_cast<int>(i) == m_hotRow) {
                            HBRUSH hb = CreateSolidBrush(hotBg);
                            FillRect(bb, &row.rect, hb); DeleteObject(hb);
                        }

                        SelectObject(bb, m_hFontBody);
                        SetTextColor(bb, fg);
                        RECT lr{row.rect.left + static_cast<int>(10 * s), row.rect.top,
                                row.rect.left + labelW, row.rect.bottom};
                        DrawTextW(bb, row.label.c_str(), -1, &lr,
                                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

                        RECT vr{row.rect.left + labelW, row.rect.top,
                                row.rect.right - static_cast<int>(10 * s), row.rect.bottom};
                        if (static_cast<int>(i) == m_editingRow) {
                            m_edit.Draw(bb, m_hFontBody, vr, static_cast<int>(6 * s),
                                        GetFocus() == GetHwnd());
                        } else {
                            const bool ph = row.value == L"(not set)" ||
                                            row.value == L"(optional)" ||
                                            row.value == L"(required)";
                            SetTextColor(bb, ph ? dim : fg);
                            DrawTextW(bb, row.value.c_str(), -1, &vr,
                                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                        }

                        HPEN pen = CreatePen(PS_SOLID, 1, line);
                        HGDIOBJ op = SelectObject(bb, pen);
                        MoveToEx(bb, row.rect.left, row.rect.bottom, nullptr);
                        LineTo(bb, row.rect.right, row.rect.bottom);
                        SelectObject(bb, op); DeleteObject(pen);
                    }
                }
                y += h;
            }
            m_contentH = y + m_scrollY - listTop;

            SelectClipRgn(bb, nullptr);
            DeleteObject(clip);

            // ── Footer ───────────────────────────────────────────────────────
            SelectObject(bb, m_hFontSmall);
            SetTextColor(bb, dim);
            RECT fr{pad, listBot, W - pad, H};
            DrawTextW(bb,
                m_editingRow >= 0
                    ? L"Enter = save   Esc = cancel"
                    : L"↑↓ select   Enter = edit   wheel = scroll   Esc = close",
                -1, &fr, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

            BitBlt(dc, 0, 0, W, H, bb, 0, 0, SRCCOPY);
            EndPaint(GetHwnd(), &ps);
            return 0;
        }

        case WM_CHAR:
            if (m_editingRow >= 0) {
                if (m_edit.RouteChar(static_cast<wchar_t>(wParam), GetHwnd()) ==
                    InputResult::ConsumedRepaint) Repaint();
            }
            return 0;

        case WM_MOUSEMOVE: {
            const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const int hr = HitTestRow(pt), hb = HitTestButton(pt);
            if (hr != m_hotRow || hb != m_hotButton) {
                m_hotRow = hr; m_hotButton = hb;
                Repaint();
            }
            if (m_editingRow >= 0) m_edit.RouteMouse(message, wParam, lParam, GetHwnd());
            return 0;
        }

        case WM_LBUTTONDOWN: {
            const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};

            if (m_editingRow >= 0 && PtInRect(&m_rows[m_editingRow].rect, pt)) {
                m_edit.RouteMouse(message, wParam, lParam, GetHwnd());
                Repaint();
                return 0;
            }
            if (m_editingRow >= 0) CommitTextEdit();

            const int bi = HitTestButton(pt);
            if (bi >= 0) {
                switch (m_buttons[bi].id) {
                    case BTN_GENERATE:       DoGenerate();      break;
                    case BTN_ADD_STARTUP:    DoAddStartup();    break;
                    case BTN_REMOVE_STARTUP: DoRemoveStartup(); break;
                    case BTN_TEST:           DoTest();          break;
                    default: break;
                }
                return 0;
            }

            const int ri = HitTestRow(pt);
            if (ri >= 0) { m_selected = ri; Repaint(); EditRow(ri); }
            return 0;
        }

        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
            if (m_editingRow >= 0 &&
                m_edit.RouteMouse(message, wParam, lParam, GetHwnd()) ==
                    InputResult::ConsumedRepaint) Repaint();
            return 0;

        case WM_MOUSEWHEEL: {
            m_scrollY -= (GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA) *
                         static_cast<int>(ROW_H * app.dpiScale) * 3;
            ClampScroll();
            Repaint();
            return 0;
        }

        default: break;
    }
    return DefWindowProcW(GetHwnd(), message, wParam, lParam);
}

} // namespace UI
