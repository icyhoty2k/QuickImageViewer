#include "DedicatedWnd.h"
#include "UI/LinkText.h"
#include "UI/GdiPool.h" // pooled brushes and pens — never DeleteObject them
#include "DedicatedSettings.h"
#include "DedicatedLists.h"        // list paths + AppendFolder
#include "SlideshowTransitions.h"  // TransitionDisplayOrder — same order as the menu
#include "Common/Converters.h"
#include "Platform/FileHandler.h"  // is_image_ext — one definition of "an image"
#include <filesystem>
#include <cwctype>
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
    constexpr int PANEL_H  = 720; // taller: rows now carry a description line
    constexpr int PAD      = 14;
    constexpr int ROW_H    = 44; // label + value on line 1, description on line 2
    constexpr int HDR_H    = 32;
    constexpr int BTN_H    = 34;
    constexpr int BTN_GAP  = 8;
    constexpr int TITLE_H  = 44;
    constexpr int FOOTER_H = 24;

    enum ButtonId {
        BTN_GENERATE_APP = 1, BTN_GENERATE_CONFIG, BTN_ADD_IMAGES, BTN_ADD_PROMOS,
        BTN_ADD_STARTUP, BTN_REMOVE_STARTUP,
        // Three separate tests: a broken config, a bad image list and a bad
        // promo list are different faults with different fixes, so lumping them
        // into one report made it hard to tell which thing to go and repair.
        BTN_TEST_CONFIG, BTN_TEST_IMAGES, BTN_TEST_PROMOS
    };

    // Row ids — the edit dispatch switches on these.
    enum RowId {
        R_NONE = 0,
        // Instance
        R_NAME, R_DESC, R_DEDICATED_DIR,
        // Content
        R_IMAGE_FOLDER, R_PROMO_FOLDER,
        // Promotions
        R_PROMO_ORDER, R_PROMO_IMAGES, R_PROMO_SECONDS, R_PROMO_SHOW,
        // Presentation
        R_MONITOR, R_FULLSCREEN, R_SLIDESHOW, R_LOOP, R_SHUFFLE, R_HIDEMOUSE, R_INTERVAL,
        R_LOCK, R_KEEP_AWAKE,
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
        // History (ignored by a dedicated screen, but a portable main-app
        // config uses the same file)
        R_RUN_STARTUP, R_HIST_FULL, R_HIST_DIRS, R_HIST_FAVS, R_HIST_SAVE,
        // Thumbnail strip file operations
        R_THUMB_COPY, R_THUMB_MOVE, R_THUMB_DELETE, R_THUMB_PASTE,
        // Custom transition set
        R_TRANS_LIST,
        // One id per TransitionType: R_TRANS_PICK_FIRST + <type value>.
        // Kept last so the block can grow with the enum.
        R_TRANS_PICK_FIRST = 1000,
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
    // Panel transparency — tune via Constants::Dedicated::PANEL_OPACITY.
    if (GetHwnd()) {
        SetWindowLongPtrW(GetHwnd(), GWL_EXSTYLE,
                          GetWindowLongPtrW(GetHwnd(), GWL_EXSTYLE) | WS_EX_LAYERED);
        SetLayeredWindowAttributes(GetHwnd(), 0,
                                   Constants::Dedicated::PANEL_OPACITY, LWA_ALPHA);
    }
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
    m_list.scrollY = 0;
    BuildRows();
    ShowCenterOverParent();

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

int DedicatedWnd::DialogPromptFloat(const wchar_t *caption, const wchar_t *label,
                                    float cur, float lo, float hi, float def) {
    PushTopmostOff();
    const int v = ThemedDialog::PromptFloat(GetHwnd(), caption, label, cur, lo, hi, def);
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
void DedicatedWnd::DoGenerateApp() {
    if (m_cfg.name.empty()) {
        DialogMessage(L"Give the instance a name first — the copy, its config and its "
                      L"shortcut are all named after it.", L"Generate App");
        return;
    }
    if (m_targetFolder.empty() &&
        !PickFolder(m_targetFolder, L"Where should the dedicated copy be created?"))
        return;

    const std::wstring srcExe = Persistence::Registry::GetExePathW();
    const std::wstring dstExe = TargetExePath();
    if (srcExe.empty() || dstExe.empty()) {
        DialogMessage(L"Could not resolve the target paths.", L"Generate App");
        return;
    }

    // Refuse to overwrite the exe we are currently running from — Windows locks
    // it anyway, and the failure would be confusing.
    if (_wcsicmp(srcExe.c_str(), dstExe.c_str()) == 0) {
        DialogMessage(L"That is this running copy. Choose a different folder or name.",
                      L"Generate App");
        return;
    }
    if (FileExists(dstExe) &&
        !DialogConfirm(L"A copy already exists:\n\n" + dstExe + L"\n\nOverwrite it?",
                       L"Generate App"))
        return;

    if (!CopyFileW(srcExe.c_str(), dstExe.c_str(), FALSE)) {
        DialogMessage(L"Could not copy the executable to:\n\n" + dstExe +
                      L"\n\nCheck the folder is writable and the copy is not running.",
                      L"Generate App");
        return;
    }

    DialogMessage(L"Dedicated copy created:\n\n" + dstExe +
                  L"\n\nNext: \"Generate Config\" to write its settings.",
                  L"Generate App");
    BuildRows();
    Repaint();
}

void DedicatedWnd::DoGenerateConfig() {
    if (m_cfg.name.empty()) {
        DialogMessage(L"Give the instance a name first — the config is named after it.",
                      L"Generate Config");
        return;
    }
    if (m_targetFolder.empty() &&
        !PickFolder(m_targetFolder, L"Where does the dedicated copy live?"))
        return;

    const std::wstring dstIni = TargetIniPath();
    if (dstIni.empty()) {
        DialogMessage(L"Could not resolve the config path.", L"Generate Config");
        return;
    }
    if (FileExists(dstIni) &&
        !DialogConfirm(L"A config already exists:\n\n" + dstIni +
                       L"\n\nOverwrite it with the settings shown here?",
                       L"Generate Config"))
        return;

    Dedicated::WriteConfigTo(dstIni, m_cfg);

    // The lists belong beside the copy, not beside this process.
    const std::wstring exe = TargetExePath();
    const std::wstring imgList = Dedicated::ImageListPathFor(exe);
    const std::wstring proList = Dedicated::PromotionListPathFor(exe);

    DialogMessage(L"Config written:\n\n" + dstIni +
                  L"\n\nFolder lists:\n" + imgList + L"\n" + proList +
                  L"\n\nUse \"Add Images\" / \"Add Promotions\" to fill them.",
                  L"Generate Config");
    BuildRows();
    Repaint();
}

// =============================================================================
// DoTestList — validate ONE folder list and what it actually contains.
//
// Counting the images matters as much as checking the folders exist: a folder
// that is present but holds nothing this app can decode looks identical to a
// working one until the screen goes blank in front of people.
// =============================================================================
void DedicatedWnd::DoTestList(bool promotions) {
    const wchar_t *caption = promotions ? L"Test Promos" : L"Test Images";

    // Use this instance's own list when there is one; otherwise let any list be
    // chosen. Same rule as Test Config — every test works standalone, so a list
    // copied off another machine can be checked here.
    std::wstring listPath;
    const std::wstring exe = ListOwnerExe();
    if (!exe.empty())
        listPath = promotions ? Dedicated::PromotionListPathFor(exe)
                              : Dedicated::ImageListPathFor(exe);

    // Whether this list belongs to the config shown in the panel. A picked list
    // from elsewhere does not, so the trigger cross-checks below must be skipped
    // — they would report the wrong instance's settings.
    const bool ownList = !listPath.empty() && FileExists(listPath);
    if (!ownList) listPath = PickListFile(promotions);
    if (listPath.empty()) return; // cancelled

    const std::vector<std::wstring> folders = Dedicated::LoadListAt(listPath);
    if (folders.empty()) {
        DialogMessage(L"The list is empty:\n\n" + listPath +
                      (promotions ? L"\n\nNo promotions will be shown."
                                  : L"\n\nThe screen would have nothing to show."),
                      caption);
        return;
    }

    std::wstring report = listPath;
    report += L"\n\n";
    int okFolders = 0, deadFolders = 0, emptyFolders = 0;
    long long total = 0;

    for (const std::wstring &f : folders) {
        std::error_code ec;
        if (!std::filesystem::is_directory(f, ec) || ec) {
            ++deadFolders;
            report += std::wstring(L"  ") + Constants::ThemeIcons::ICON_WARNING + L" " + f + L"   (folder missing)\n";
            continue;
        }

        long long count = 0;
        long long weighted = 0;
        for (const auto &e : std::filesystem::directory_iterator(f, ec)) {
            if (ec) break;
            if (!e.is_regular_file(ec) || ec) continue;
            std::wstring ext = e.path().extension().wstring();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
            if (!is_image_ext(ext)) continue;
            ++count;
            if (promotions)
                weighted += Dedicated::ParsePromotionWeight(e.path().stem().wstring());
        }
        total += count;

        if (count == 0) {
            ++emptyFolders;
            report += std::wstring(L"  ") + Constants::ThemeIcons::ICON_WARNING + L" " + f + L"   (no images)\n";
        } else {
            ++okFolders;
            report += L"   " + f + L"   " + std::to_wstring(count) + L" images";
            // Weight only means something for promotions, where it drives the
            // draw. Showing it here is the only place a mis-typed #N shows up.
            if (promotions && weighted != count)
                report += L", weight " + std::to_wstring(weighted);
            report += L"\n";
        }
    }

    report += L"\n" + std::to_wstring(total) + L" images across " +
              std::to_wstring(okFolders) + L" usable folder" +
              (okFolders == 1 ? L"" : L"s") + L".";
    if (deadFolders)  report += L"\n" + std::to_wstring(deadFolders) + L" missing.";
    if (emptyFolders) report += L"\n" + std::to_wstring(emptyFolders) + L" empty.";

    if (promotions && ownList) {
        if (total > 0 && m_cfg.promoImagesFrom <= 0 && m_cfg.promoTimeFrom <= 0)
            report += std::wstring(L"\n\n") + Constants::ThemeIcons::ICON_WARNING + L" Both promotion triggers are off — none of these will show.";
        if (total == 0 && (m_cfg.promoImagesFrom > 0 || m_cfg.promoTimeFrom > 0))
            report += std::wstring(L"\n\n") + Constants::ThemeIcons::ICON_WARNING + L" Triggers are set but there are no promotions to draw from.";
    }

    const bool clean = (deadFolders == 0 && emptyFolders == 0 && total > 0);
    DialogMessage((clean ? std::wstring(Constants::ThemeIcons::ICON_CHECK) + L" " : std::wstring(Constants::ThemeIcons::ICON_WARNING) + L" ") + report, caption);
}

// =============================================================================
// Folder lists
// =============================================================================
std::wstring DedicatedWnd::ListOwnerExe() const {
    // 1. A generated copy owns its lists — that is what is being authored.
    const std::wstring target = TargetExePath();
    if (!target.empty() && FileExists(target)) return target;

    // 2. Failing that, only a dedicated instance owns lists, and only its own.
    if (Dedicated::SettingsUseFile()) return Persistence::Registry::GetExePathW();

    // 3. Nothing owns them yet. Returning this exe here would scatter
    //    imageLists_QuickImageViewer.qim next to the MAIN app — files it never
    //    reads and the user never asked for.
    return {};
}

void DedicatedWnd::ShowFolderList(bool promotions) {
    const std::wstring exe = ListOwnerExe();
    if (exe.empty()) {
        DialogMessage(L"No dedicated copy exists yet.\n\nThe folder lists live beside "
                      L"the copy, so name the instance and press \"Generate App\" "
                      L"first.", L"Folders");
        return;
    }
    const std::wstring listPath = promotions ? Dedicated::PromotionListPathFor(exe)
                                             : Dedicated::ImageListPathFor(exe);
    const wchar_t *what = promotions ? L"promotion folders" : L"image folders";
    if (listPath.empty()) {
        DialogMessage(L"Could not resolve the list file path.", L"Folders");
        return;
    }

    // Create on first look, so the file is there to hand-edit even before any
    // folder has been added through the buttons.
    const bool existed = FileExists(listPath);
    Dedicated::EnsureListAt(listPath, what);

    const std::vector<std::wstring> folders = Dedicated::LoadListAt(listPath);

    std::wstring msg = listPath;
    if (!existed) msg += L"\n\n(created just now)";
    msg += L"\n\n";

    if (folders.empty()) {
        msg += L"No folders listed yet.\nUse the ";
        msg += promotions ? L"\"Add Promotions\"" : L"\"Add Images\"";
        msg += L" button to add one.";
    } else {
        // Flag folders that have gone away — a silently dead entry is the most
        // likely reason a screen shows nothing.
        for (const std::wstring &f : folders) {
            const DWORD a = GetFileAttributesW(f.c_str());
            const bool ok = (a != INVALID_FILE_ATTRIBUTES) && (a & FILE_ATTRIBUTE_DIRECTORY);
            msg += (ok ? L"   " : std::wstring(L"  ") + Constants::ThemeIcons::ICON_WARNING + L" ");
            msg += f;
            if (!ok) msg += L"   (missing)";
            msg += L"\n";
        }
        msg += L"\n";
        msg += std::to_wstring(folders.size());
        msg += (folders.size() == 1) ? L" folder listed." : L" folders listed.";
    }
    DialogMessage(msg, promotions ? L"Promotion folders" : L"Image folders");
}

void DedicatedWnd::AppendFolderToList(bool promotions) {
    const std::wstring exe = ListOwnerExe();
    if (exe.empty()) {
        DialogMessage(L"No dedicated copy exists yet.\n\nThe folder lists live beside "
                      L"the copy, so name the instance and press \"Generate App\" "
                      L"first.", L"Add folder");
        return;
    }
    const std::wstring listPath = promotions ? Dedicated::PromotionListPathFor(exe)
                                             : Dedicated::ImageListPathFor(exe);
    if (listPath.empty()) {
        DialogMessage(L"Could not resolve the list file path.", L"Add folder");
        return;
    }

    const wchar_t *what = promotions ? L"promotions" : L"images";
    std::wstring folder;
    if (!PickFolder(folder, promotions ? L"Add a promotions folder"
                                       : L"Add an images folder"))
        return;

    switch (Dedicated::AppendFolder(listPath, folder)) {
        case Dedicated::AppendResult::Added:
            DialogMessage(std::wstring(L"Added to the ") + what + L" list:\n\n" + folder +
                          L"\n\nList file:\n" + listPath, L"Add folder");
            break;
        case Dedicated::AppendResult::Duplicate:
            // Reported rather than skipped silently, so a click always has a
            // visible result.
            DialogMessage(L"That folder is already in the " + std::wstring(what) +
                          L" list — skipped:\n\n" + folder, L"Add folder");
            break;
        case Dedicated::AppendResult::Failed:
            DialogMessage(L"Could not write the list file:\n\n" + listPath,
                          L"Add folder");
            break;
    }
    BuildRows();
    Repaint();
}

void DedicatedWnd::DoAddImages()     { AppendFolderToList(false); }
void DedicatedWnd::DoAddPromotions() { AppendFolderToList(true); }

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

    // Content lives in the list files beside the exe the config belongs to, so
    // validate those rather than any path inside the .ini.
    std::wstring exe = ini;
    {
        const size_t dot = exe.find_last_of(L'.');
        const size_t slash = exe.find_last_of(L"\\/");
        if (dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash))
            exe.resize(dot);
        exe += L".exe";
    }
    if (!FileExists(exe))
        problems += L"\n  • no executable beside this config: " + exe;

    // Only the list FILES are checked here — their contents belong to
    // "Test Images" / "Test Promos", so each report has one subject.
    if (!FileExists(Dedicated::ImageListPathFor(exe)))
        problems += L"\n  • no image list beside the copy (run Test Images)";

    if (cfg.promoImagesTo > 0 && cfg.promoImagesTo < cfg.promoImagesFrom)
        problems += L"\n  • promotion image range is reversed";
    if (cfg.promoTimeTo > 0 && cfg.promoTimeTo < cfg.promoTimeFrom)
        problems += L"\n  • promotion time range is reversed";

    if (problems.empty()) {
        DialogMessage(std::wstring(Constants::ThemeIcons::ICON_CHECK) + L" This config looks valid.\n\n" + ini, L"Test Config");
        return;
    }

    if (DialogConfirm(std::wstring(Constants::ThemeIcons::ICON_WARNING) + L" Problems found in:\n" + ini + L"\n" + problems +
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
        m_rows.push_back({Kind::Header, t, L"", L"", R_NONE, {}});
    };
    auto row = [&](Kind k, const wchar_t *label, std::wstring value,
                   const wchar_t *desc, int id) {
        m_rows.push_back({k, label, std::move(value), desc, id, {}});
    };

    hdr(L"INSTANCE");
    row(Kind::Text,   L"Name", m_cfg.name.empty() ? L"(required)" : m_cfg.name,
        L"Identifies this screen. The copy, its config, its lists and its startup shortcut are all named after it.", R_NAME);
    row(Kind::Text,   L"Description", m_cfg.description.empty() ? L"(optional)" : m_cfg.description,
        L"Free text shown on the generated shortcut. Useful when several screens look alike.", R_DESC);
    row(Kind::Folder, L"Create copy in", Tail(m_targetFolder),
        L"Where Generate App places the copy. Each screen lives in its own folder with its own config.", R_DEDICATED_DIR);

    hdr(L"CONTENT");
    {
        // Counts come from the lists of the copy being AUTHORED, not this
        // process — otherwise the panel would report the wrong screen's content.
        // Reading by path also avoids creating anything as a side effect of
        // drawing: LoadListAt on a missing file simply yields nothing.
        const std::wstring owner = ListOwnerExe();
        auto describe = [&](const std::wstring &path) -> std::wstring {
            if (owner.empty()) return L"(no copy yet)";
            const size_t n = Dedicated::LoadListAt(path).size();
            return n ? std::to_wstring(n) + L" listed" : std::wstring(L"(none)");
        };
        row(Kind::Folder, L"Image folders",
            describe(owner.empty() ? L"" : Dedicated::ImageListPathFor(owner)),
            L"Folders of pictures to show, held in imageLists_<name>.qim. Click to view, Add Images to add.", R_IMAGE_FOLDER);
        row(Kind::Folder, L"Promotion folders",
            describe(owner.empty() ? L"" : Dedicated::PromotionListPathFor(owner)),
            L"Folders of promotions, held in promotionList_<name>.qpr. Click to view, Add Promotions to add.", R_PROMO_FOLDER);
    }

    hdr(L"PROMOTIONS");
    row(Kind::Choice, L"Pick", m_cfg.promoOrder == D::PromoOrder::SEQUENTIAL
                                   ? L"Sequential" : L"Weighted by priority",
        L"Sequential plays them in folder order. Weighted favours files whose name ends in #N — higher N appears more often.", R_PROMO_ORDER);
    row(Kind::Number, L"Every N images", RangeText(m_cfg.promoImagesFrom, m_cfg.promoImagesTo, L"images"),
        L"Gap counted in pictures. 0 = off. A single value is exact; a range is re-rolled each time so it never looks mechanical.", R_PROMO_IMAGES);
    row(Kind::Number, L"Every N seconds", RangeText(m_cfg.promoTimeFrom, m_cfg.promoTimeTo, L"sec"),
        L"Gap counted in time. 0 = off. Runs independently of the image counter — either one coming due shows a promotion.", R_PROMO_SECONDS);
    row(Kind::Number, L"Promotion shown for", m_cfg.promoShowSeconds > 0
                                                  ? std::to_wstring(m_cfg.promoShowSeconds) + L" sec"
                                                  : std::wstring(L"Same as a slide"),
        L"How long a promotion stays on screen. Independent of the slide interval — a message usually needs longer than a picture.", R_PROMO_SHOW);

    hdr(L"PRESENTATION");
    row(Kind::Number, L"Monitor", m_cfg.monitorNum >= 1 ? std::to_wstring(m_cfg.monitorNum) : std::wstring(L"Any"),
        L"Which display to open on, counted from 1. Any = wherever Windows puts it.", R_MONITOR);
    row(Kind::Toggle, L"Fullscreen", OnOff(m_cfg.fullscreen),
        L"Fill the chosen monitor with no window frame.", R_FULLSCREEN);
    row(Kind::Toggle, L"Start slideshow", OnOff(m_cfg.slideshow),
        L"Begin playing as soon as the screen launches, with no interaction.", R_SLIDESHOW);
    row(Kind::Toggle, L"Loop", OnOff(m_cfg.loop),
        L"Return to the first picture after the last, so playback never ends.", R_LOOP);
    row(Kind::Toggle, L"Shuffle images", OnOff(m_cfg.shuffle),
        L"Play pictures in random order instead of sorted order.", R_SHUFFLE);
    row(Kind::Toggle, L"Hide mouse", OnOff(m_cfg.hideMouse),
        L"Hide the pointer at launch. It returns when playback is stopped, so the screen stays configurable.", R_HIDEMOUSE);
    row(Kind::Number, L"Slide interval", m_cfg.intervalSeconds > 0
                                             ? std::to_wstring(m_cfg.intervalSeconds) + L" sec"
                                             : std::wstring(L"Use saved"),
        L"Seconds each picture stays on screen. 0 keeps whatever the app already had.", R_INTERVAL);
    row(Kind::Toggle, L"Kiosk lock", OnOff(m_cfg.lock),
        L"Ignore all keyboard and mouse input, so nobody can touch the screen. The tray icon still works — it is the only way to unlock it again.", R_LOCK);
    row(Kind::Toggle, L"Keep display awake", OnOff(m_cfg.keepDisplayAwake),
        L"Block the screensaver and display sleep while the screen is showing. Leave this off and Windows blanks the display, with no one there to wake it.", R_KEEP_AWAKE);

    hdr(L"TRANSITIONS");
    row(Kind::Choice, L"Transition", Constants::Messages::TRANSITION_NAMES[m_cfg.transitionType],
        L"The effect between pictures when the source below is None.", R_TRANS_TYPE);
    row(Kind::Choice, L"Source", Constants::Messages::TRANSITION_SOURCE_NAMES[m_cfg.transitionSource],
        L"Which effects are in play: only the one chosen, all of them, or the custom ticked list.", R_TRANS_SOURCE);
    row(Kind::Choice, L"Order", Constants::Messages::TRANSITION_ORDER_NAMES[m_cfg.transitionOrder],
        L"How the next effect is drawn from that set — in listed order, or at random.", R_TRANS_ORDER);
    // Per-effect picker, shown only when Source is List — 21 extra rows are
    // noise when the list is not being used. Ids are R_TRANS_PICK_FIRST + the
    // TransitionType value, so one case handles all of them.
    if (m_cfg.transitionSource == SS::TransitionSource::LIST) {
        int picked = 0;
        for (int i = 0; i < SS::TRANSITION_COUNT; ++i)
            if (m_cfg.transitionList & (1u << i)) ++picked;

        row(Kind::Number, L"Custom set", std::to_wstring(picked) + L" of " +
                                         std::to_wstring(SS::TRANSITION_COUNT),
            L"Click a effect below to add or remove it. Clicking here selects all animated effects.", R_TRANS_LIST);

        const int *order = TransitionDisplayOrder();
        for (int n = 0; n < SS::TRANSITION_COUNT; ++n) {
            const int t = order[n];
            row(Kind::Toggle,
                Constants::Messages::TRANSITION_NAMES[t],
                OnOff((m_cfg.transitionList & (1u << t)) != 0),
                L"Include this effect in the custom set.",
                R_TRANS_PICK_FIRST + t);
        }
    }

    hdr(L"VIEW & WINDOW");
    row(Kind::Number, L"View mode", std::to_wstring(m_cfg.viewMode),
        L"How a picture is fitted: 1 fit, 2 width, 3 height, 4 stretch, 5 original size.", R_VIEWMODE);
    row(Kind::Number, L"Window width", std::to_wstring(m_cfg.baseWidth),
        L"Default window width in pixels, used when not fullscreen.", R_BASE_W);
    row(Kind::Number, L"Window height", std::to_wstring(m_cfg.baseHeight),
        L"Default window height in pixels, used when not fullscreen.", R_BASE_H);
    row(Kind::Toggle, L"Start fullscreen", OnOff(m_cfg.startFullscreen),
        L"Open fullscreen every launch, independent of the presentation setting above.", R_START_FULLSCREEN);
    row(Kind::Toggle, L"Always on top", OnOff(m_cfg.alwaysOnTop),
        L"Keep the window above all others so nothing can cover the display.", R_ALWAYS_TOP);

    hdr(L"OVERLAYS");
    row(Kind::Toggle, L"Info overlays", OnOff(m_cfg.overlaysVisible),
        L"Show the corner information panels over the picture.", R_OVERLAY_VISIBLE);
    row(Kind::Toggle, L"Overlay background", OnOff(m_cfg.overlayBackground),
        L"Draw a backing behind overlay text so it stays readable on bright pictures.", R_OVERLAY_BG);
    row(Kind::Number, L"Message duration", std::to_wstring(m_cfg.msgDurationMs) + L" ms",
        L"How long centre messages stay on screen before fading.", R_MSG_MS);

    hdr(L"SORTING");
    row(Kind::Number, L"Sort order", std::to_wstring(m_cfg.sortOrder),
        L"Playback order: 0 name, 1 date, 2 size, 3 type, 4 physical disk order.", R_SORT_ORDER);
    row(Kind::Toggle, L"Reverse order", OnOff(m_cfg.sortReverse),
        L"Play the sort order backwards.", R_SORT_REVERSE);

    hdr(L"PERFORMANCE");
    row(Kind::Number, L"VRAM cache", std::to_wstring(m_cfg.vramCache),
        L"Decoded pictures kept in graphics memory. Higher is smoother but uses more VRAM.", R_VRAM);
    row(Kind::Number, L"Preload lookaside", std::to_wstring(m_cfg.preloadLookaside),
        L"How many pictures ahead and behind are decoded in advance.", R_LOOKASIDE);
    row(Kind::Number, L"Thumb cache MB", std::to_wstring(m_cfg.thumbCacheMB),
        L"Disk budget for thumbnails, in megabytes.", R_THUMB_CACHE);

    hdr(L"INPUT");
    row(Kind::Toggle, L"Swap mouse buttons", OnOff(m_cfg.swapMouse),
        L"Exchange what the left and right buttons do.", R_SWAP_MOUSE);
    row(Kind::Toggle, L"Invert wheel", OnOff(m_cfg.invertWheel),
        L"Reverse the direction the wheel moves through pictures.", R_WHEEL_INV);
    row(Kind::Toggle, L"Invert h-wheel", OnOff(m_cfg.invertWheelH),
        L"Reverse the direction of horizontal wheel tilting.", R_WHEEL_INV_H);
    wchar_t zoomBuf[16];
    swprintf_s(zoomBuf, L"%.2fx", Converters::toZoomFloat(m_cfg.zoomClick));
    row(Kind::Number, L"Left-click zoom", zoomBuf,
        L"Magnification while the left button is held.", R_ZOOM_CLICK);
    row(Kind::Choice, L"Caret style", m_cfg.caretStyle == 0 ? L"Bar" : L"Underscore",
        L"Shape of the text cursor in search and filter boxes.", R_CARET);
    row(Kind::Toggle, L"Ctrl+C copy", OnOff(m_cfg.ctrlCEnabled),
        L"Allow copying the current picture to the clipboard.", R_CTRL_C);
    row(Kind::Toggle, L"Right-click menu", OnOff(m_cfg.contextMenu),
        L"Show the context menu on a right-click in the main window.", R_CONTEXT_MENU);

    hdr(L"MISC");
    row(Kind::Toggle, L"Keep in background", OnOff(m_cfg.keepInBackground),
        L"Closing hides to the tray instead of quitting, so the next open is instant.", R_KEEP_BG);
    row(Kind::Toggle, L"Thumbnail effects", OnOff(m_cfg.thumbnailEffects),
        L"Rounded corners, glow and hover scaling in the thumbnail strips.", R_THUMB_FX);
    row(Kind::Toggle, L"Open strip on start", OnOff(m_cfg.openDirOnStart),
        L"Show the folder thumbnail strip automatically at launch.", R_OPEN_DIR_ON_START);
    row(Kind::Number, L"Theme brightness", std::to_wstring(m_cfg.themePercent) + L"%",
        L"Overall panel brightness, 0 darkest to 100 lightest.", R_THEME);
    row(Kind::Toggle, L"Run on startup", OnOff(m_cfg.runOnStartup),
        L"Register the app to launch with Windows. A dedicated screen ignores this and uses its startup shortcut instead.", R_RUN_STARTUP);

    hdr(L"THUMBNAIL OPERATIONS");
    row(Kind::Toggle, L"Copy", OnOff(m_cfg.thumbCopy),
        L"Allow copying files from the thumbnail strip.", R_THUMB_COPY);
    row(Kind::Toggle, L"Cut / Move", OnOff(m_cfg.thumbMove),
        L"Allow moving files out of the thumbnail strip.", R_THUMB_MOVE);
    row(Kind::Toggle, L"Delete", OnOff(m_cfg.thumbDelete),
        L"Allow sending files to the Recycle Bin from the strip.", R_THUMB_DELETE);
    row(Kind::Toggle, L"Paste", OnOff(m_cfg.thumbPaste),
        L"Allow pasting files into the strip's folder.", R_THUMB_PASTE);

    hdr(L"HISTORY");
    row(Kind::Toggle, L"Open full list", OnOff(m_cfg.historyFull),
        L"Tab opens the complete history instead of the capped view. A dedicated screen keeps no history at all.", R_HIST_FULL);
    row(Kind::Number, L"Max folders shown", std::to_wstring(m_cfg.historyMaxDirs),
        L"How many recent folders the history panel lists.", R_HIST_DIRS);
    row(Kind::Number, L"Max favourites shown", std::to_wstring(m_cfg.historyMaxFavs),
        L"How many favourites the history panel lists.", R_HIST_FAVS);
    row(Kind::Number, L"Folders remembered", std::to_wstring(m_cfg.historyMaxSave),
        L"How many folders are kept on disk between sessions.", R_HIST_SAVE);

    // Buttons, two rows: building the instance on top, deploying it below.
    // Remove Startup only lights up when there is a shortcut to remove.
    m_buttons.clear();
    m_buttons.push_back({L"Generate App",    BTN_GENERATE_APP,    {}, true, 0});
    m_buttons.push_back({L"Generate Config", BTN_GENERATE_CONFIG, {}, true, 0});
    m_buttons.push_back({L"Add Images",      BTN_ADD_IMAGES,      {}, true, 0});
    m_buttons.push_back({L"Add Promotions",  BTN_ADD_PROMOS,      {}, true, 0});
    m_buttons.push_back({L"Add Startup",     BTN_ADD_STARTUP,     {}, true, 1});
    m_buttons.push_back({L"Remove Startup",  BTN_REMOVE_STARTUP,  {},
                         FileExists(StartupLinkPath()), 1});
    m_buttons.push_back({L"Test Config",     BTN_TEST_CONFIG,     {}, true, 1});
    m_buttons.push_back({L"Test Images",     BTN_TEST_IMAGES,     {}, true, 1});
    m_buttons.push_back({L"Test Promos",     BTN_TEST_PROMOS,     {}, true, 1});

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

std::wstring DedicatedWnd::PickListFile(bool promotions) {
    PushTopmostOff();
    std::wstring result;
    IFileOpenDialog *pfd = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&pfd))) && pfd) {
        // One kind only — the whole point of separate extensions.
        const wchar_t *spec  = promotions ? L"*.qpr" : L"*.qim";
        const wchar_t *label = promotions ? L"Promotion list (*.qpr)"
                                          : L"Image list (*.qim)";
        COMDLG_FILTERSPEC f[] = {{label, spec}};
        pfd->SetFileTypes(ARRAYSIZE(f), f);
        pfd->SetDefaultExtension(promotions ? L"qpr" : L"qim");
        pfd->SetTitle(promotions ? L"Choose a promotion list to test"
                                 : L"Choose an image list to test");
        DWORD o = 0;
        pfd->GetOptions(&o);
        pfd->SetOptions(o | FOS_FILEMUSTEXIST);
        if (SUCCEEDED(pfd->Show(GetHwnd()))) {
            IShellItem *psi = nullptr;
            if (SUCCEEDED(pfd->GetResult(&psi))) {
                PWSTR p = nullptr;
                if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                    result = p;
                    CoTaskMemFree(p);
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

    // One case for all 21 per-effect toggles.
    if (r.id >= R_TRANS_PICK_FIRST &&
        r.id < R_TRANS_PICK_FIRST + SS::TRANSITION_COUNT) {
        m_cfg.transitionList ^= (1u << (r.id - R_TRANS_PICK_FIRST));
        BuildRows();
        Repaint();
        return;
    }

    switch (r.id) {
        case R_NAME:
        case R_DESC: BeginTextEdit(rowIndex); return;

        case R_DEDICATED_DIR: PickFolder(m_targetFolder, L"Where to create the copy"); break;
        // Clicking a Content row SHOWS what the list holds. Adding is the job of
        // the buttons above — a row that silently opened a folder picker made
        // "what is in here?" impossible to answer without changing it.
        case R_IMAGE_FOLDER: ShowFolderList(false); return;
        case R_PROMO_FOLDER: ShowFolderList(true);  return;

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

        case R_PROMO_SHOW: {
            const int v = DialogPromptInt(L"Promotion display time",
                L"Seconds a promotion stays on screen.\n0 = same as a normal slide.",
                m_cfg.promoShowSeconds, 0, 3600, 0);
            if (v >= 0) m_cfg.promoShowSeconds = v;
            break;
        }
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
        case R_LOCK:       m_cfg.lock       = !m_cfg.lock;       break;
        case R_KEEP_AWAKE: m_cfg.keepDisplayAwake = !m_cfg.keepDisplayAwake; break;
        case R_INTERVAL: {
            const int v = DialogPromptInt(L"Slide interval",
                                          L"Seconds between slides.\n0 = use the saved value.",
                                          m_cfg.intervalSeconds, 0, 3600, 0);
            if (v >= 0) m_cfg.intervalSeconds = v;
            break;
        }

        // --- Mirrored app settings ------------------------------------------
        // These edit the CONFIG ONLY. Nothing here touches the running app or
        // the registry: the panel authors a file for another instance, and
        // changing this machine's own settings as a side effect of describing
        // a screen would be wrong.
        case R_SHUFFLE:          m_cfg.shuffle          = !m_cfg.shuffle;          break;
        case R_START_FULLSCREEN: m_cfg.startFullscreen  = !m_cfg.startFullscreen;  break;
        case R_ALWAYS_TOP:       m_cfg.alwaysOnTop      = !m_cfg.alwaysOnTop;      break;
        case R_OVERLAY_VISIBLE:  m_cfg.overlaysVisible  = !m_cfg.overlaysVisible;  break;
        case R_OVERLAY_BG:       m_cfg.overlayBackground = !m_cfg.overlayBackground; break;
        case R_SORT_REVERSE:     m_cfg.sortReverse      = !m_cfg.sortReverse;      break;
        case R_SWAP_MOUSE:       m_cfg.swapMouse        = !m_cfg.swapMouse;        break;
        case R_WHEEL_INV:        m_cfg.invertWheel      = !m_cfg.invertWheel;      break;
        case R_WHEEL_INV_H:      m_cfg.invertWheelH     = !m_cfg.invertWheelH;     break;
        case R_CTRL_C:           m_cfg.ctrlCEnabled     = !m_cfg.ctrlCEnabled;     break;
        case R_CONTEXT_MENU:     m_cfg.contextMenu      = !m_cfg.contextMenu;      break;
        case R_KEEP_BG:          m_cfg.keepInBackground = !m_cfg.keepInBackground; break;
        case R_THUMB_FX:         m_cfg.thumbnailEffects = !m_cfg.thumbnailEffects; break;
        case R_OPEN_DIR_ON_START: m_cfg.openDirOnStart  = !m_cfg.openDirOnStart;   break;
        case R_RUN_STARTUP:  m_cfg.runOnStartup = !m_cfg.runOnStartup; break;
        case R_HIST_FULL:    m_cfg.historyFull  = !m_cfg.historyFull;  break;
        case R_THUMB_COPY:   m_cfg.thumbCopy    = !m_cfg.thumbCopy;    break;
        case R_THUMB_MOVE:   m_cfg.thumbMove    = !m_cfg.thumbMove;    break;
        case R_THUMB_DELETE: m_cfg.thumbDelete  = !m_cfg.thumbDelete;  break;
        case R_THUMB_PASTE:  m_cfg.thumbPaste   = !m_cfg.thumbPaste;   break;

        case R_HIST_DIRS: {
            const int v = DialogPromptInt(L"Max folders shown",
                L"Recent folders listed in the history panel (0-999).",
                m_cfg.historyMaxDirs, 0, 999,
                Constants::History::IS_HISTORY_MAX_DIRS_TO_SHOW);
            if (v >= 0) m_cfg.historyMaxDirs = v;
            break;
        }
        case R_HIST_FAVS: {
            const int v = DialogPromptInt(L"Max favourites shown",
                L"Favourites listed in the history panel (0-999).",
                m_cfg.historyMaxFavs, 0, 999,
                Constants::History::IS_HISTORY_MAX_FAVORITES_TO_SHOW);
            if (v >= 0) m_cfg.historyMaxFavs = v;
            break;
        }
        case R_HIST_SAVE: {
            const int v = DialogPromptInt(L"Folders remembered",
                L"Folders kept on disk between sessions (1-99999).",
                m_cfg.historyMaxSave, 1, 99999,
                Constants::History::IS_HISTORY_MAX_DIRS_TO_SAVE);
            if (v >= 0) m_cfg.historyMaxSave = v;
            break;
        }
        case R_TRANS_LIST: {
            // Header row: select all animated effects, or clear back to none if
            // they are already all on.
            const uint32_t all = (SS::TRANSITION_COUNT >= 32)
                                     ? 0xFFFFFFFFu
                                     : ((1u << SS::TRANSITION_COUNT) - 1u);
            const uint32_t animated = all & ~1u; // everything except Cut
            m_cfg.transitionList = (m_cfg.transitionList == animated) ? 0u : animated;
            break;
        }

        case R_CARET:      m_cfg.caretStyle = m_cfg.caretStyle == 0 ? 1 : 0; break;
        case R_TRANS_TYPE: m_cfg.transitionType   = cycle(m_cfg.transitionType, SS::TRANSITION_COUNT); break;
        case R_TRANS_SOURCE: m_cfg.transitionSource = cycle(m_cfg.transitionSource, SS::TransitionSource::COUNT); break;
        case R_TRANS_ORDER:  m_cfg.transitionOrder  = cycle(m_cfg.transitionOrder, SS::TransitionOrder::COUNT); break;

        case R_VIEWMODE: {
            const int v = DialogPromptInt(L"View mode",
                L"1 Fit  2 Width  3 Height  4 Stretch  5 Original",
                m_cfg.viewMode, 1, 5, 1);
            if (v >= 1) m_cfg.viewMode = v;
            break;
        }
        case R_BASE_W: {
            const int v = DialogPromptInt(L"Window width", L"Default width in pixels.",
                m_cfg.baseWidth, 240, 16000, Constants::IS_BASE_WIDTH);
            if (v >= 0) m_cfg.baseWidth = v;
            break;
        }
        case R_BASE_H: {
            const int v = DialogPromptInt(L"Window height", L"Default height in pixels.",
                m_cfg.baseHeight, 240, 16000, Constants::IS_BASE_HEIGHT);
            if (v >= 0) m_cfg.baseHeight = v;
            break;
        }
        case R_MSG_MS: {
            const int v = DialogPromptInt(L"Message duration",
                L"Centre message duration in milliseconds.",
                m_cfg.msgDurationMs, 250, 10000, 2000);
            if (v >= 0) m_cfg.msgDurationMs = v;
            break;
        }
        case R_SORT_ORDER: {
            const int v = DialogPromptInt(L"Sort order",
                L"0 Name  1 Date  2 Size  3 Type  4 Disk order",
                m_cfg.sortOrder, 0, 4, 0);
            if (v >= 0) m_cfg.sortOrder = v;
            break;
        }
        case R_VRAM: {
            const int v = DialogPromptInt(L"VRAM cache",
                L"Pictures kept in graphics memory (0-999).",
                m_cfg.vramCache, 0, 999, Constants::IS_VRAM_CACHE_IMAGES_COUNT);
            if (v >= 0) m_cfg.vramCache = v;
            break;
        }
        case R_LOOKASIDE: {
            const int v = DialogPromptInt(L"Preload lookaside",
                L"Pictures preloaded each way (1-99).",
                m_cfg.preloadLookaside, 1, 99, Constants::IS_PRELOAD_LOOKASIDE_COUNT);
            if (v >= 0) m_cfg.preloadLookaside = v;
            break;
        }
        case R_THUMB_CACHE: {
            const int v = DialogPromptInt(L"Thumb cache",
                L"Thumbnail cache budget in megabytes.",
                m_cfg.thumbCacheMB, 100, 64000, Constants::IS_DIR_THUMB_CACHE_BUDGET_MB);
            if (v >= 0) m_cfg.thumbCacheMB = v;
            break;
        }
        case R_ZOOM_CLICK: {
            wchar_t label[128];
            swprintf_s(label, L"Magnification while the left button is held (%.2f = off .. %.2f).",
                       Constants::ZOOM_CLICK_MIN, Constants::ZOOM_CLICK_MAX);
            const int v = DialogPromptFloat(L"Left-click zoom",
                label,
                Converters::toZoomFloat(m_cfg.zoomClick), Constants::ZOOM_CLICK_MIN, Constants::ZOOM_CLICK_MAX, Constants::ZOOM_CLICK);
            if (v >= 0) m_cfg.zoomClick = v;
            break;
        }
        case R_THEME: {
            const int v = DialogPromptInt(L"Theme brightness",
                L"0 darkest .. 100 lightest.",
                m_cfg.themePercent, 0, 100,
                static_cast<int>(Constants::Theme::DEFAULT_THEME_FACTOR * 100.0f));
            if (v >= 0) m_cfg.themePercent = v;
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
        case VK_PRIOR: m_list.ScrollBy(0, -m_list.Height()); Repaint(); return true;
        case VK_NEXT:  m_list.ScrollBy(0,  m_list.Height()); Repaint(); return true;
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
    if (m_hFontLink)  DeleteObject(m_hFontLink);
    m_cachedFontDpi = dpi;
    auto mk = [&](int pt, int w, BOOL underline = FALSE) {
        return CreateFontW(-MulDiv(pt, dpi, 72), 0, 0, 0, w, FALSE, underline, FALSE,
                           DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
    };
    m_hFontBody  = mk(10, FW_NORMAL);
    m_hFontBold  = mk(11, FW_SEMIBOLD);
    m_hFontSmall = mk(8,  FW_NORMAL);
    m_hFontLink  = mk(8,  FW_NORMAL, Constants::Links::UNDERLINE ? TRUE : FALSE);
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

// One wheel "line" is one row. The base applies the user's Mouse setting and the
// Shift accelerator on top.
int DedicatedWnd::ScrollLinePx(const ScrollView &) const {
    return static_cast<int>(ROW_H * app.dpiScale);
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
            // The bar is not tested here: the base answers for it with a hand,
            // the same as every other panel now, and only passes this on when
            // the cursor is somewhere else. This panel used to show an arrow
            // over its track deliberately — one rule across the app is worth
            // more than that distinction.
            if (HitTestButton(pt) >= 0 || HitTestRow(pt) >= 0 ||
                PtInRect(&m_iniLinkRect, pt)) {
                SetCursor(Constants::Cursors::CURR_CLICK);
                return TRUE;
            }
            SetCursor(Constants::Cursors::CURR_DEFAULT);
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
            namespace PC = Constants::Dedicated::PanelColors;

            FillRect(bb, &rc, UI::Gdi::Brush(bg));
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
            m_iniLinkRect = RECT{};
            if (Dedicated::SettingsUseFile()) {
                // Label then path, so the path can be a link measured off the
                // end of the label rather than positioned by hand.
                const std::wstring label = L"Editing: ";
                DrawTextW(bb, label.c_str(), -1, &sr, DT_LEFT | DT_SINGLELINE);
                const int lx = pad + UI::Link::MeasureIn(bb, m_hFontSmall, label);
                m_iniLinkRect = UI::Link::Draw(bb, m_hFontLink, lx, sr.top, W - pad,
                                               Dedicated::SettingsFilePath(),
                                               m_iniLinkHot, s);
            } else {
                DrawTextW(bb, L"New instance — not yet generated", -1, &sr,
                          DT_LEFT | DT_SINGLELINE | DT_PATH_ELLIPSIS);
            }

            // ── Buttons ──────────────────────────────────────────────────────
            {
                const int gap = static_cast<int>(BTN_GAP * s);
                SelectObject(bb, m_hFontBody);

                // Each row is laid out independently so both stretch the full
                // width regardless of how many buttons they hold.
                for (int rowIdx = 0; rowIdx <= 1; ++rowIdx) {
                    int count = 0;
                    for (const Button &b2 : m_buttons) if (b2.row == rowIdx) ++count;
                    if (count == 0) continue;

                    const int total = W - pad * 2 - gap * (count - 1);
                    const int bw = total / count;
                    int x = pad;
                    const int y = static_cast<int>(TITLE_H * s) + rowIdx * (btnH + gap);

                for (Button &btn : m_buttons) {
                    if (btn.row != rowIdx) continue;
                    btn.rect = {x, y, x + bw, y + btnH};
                    // Row 0 = building the instance, row 1 = deploying it —
                    // tinted differently so the two groups read as distinct.
                    const int myIndex = static_cast<int>(&btn - m_buttons.data());
                    COLORREF base = (btn.row == 0) ? PC::BTN_MAIN : PC::BTN_ALT;
                    if (!btn.enabled) base = bg;
                    else if (myIndex == m_hotButton) {
                        base = RGB(std::min(255, GetRValue(base) + 40),
                                   std::min(255, GetGValue(base) + 40),
                                   std::min(255, GetBValue(base) + 40));
                    }
                    FillRect(bb, &btn.rect, UI::Gdi::Brush(base));

                    HGDIOBJ oldPen = SelectObject(bb, UI::Gdi::Pen(line));
                    HGDIOBJ oldBr  = SelectObject(bb, GetStockObject(NULL_BRUSH));
                    Rectangle(bb, btn.rect.left, btn.rect.top, btn.rect.right, btn.rect.bottom);
                    SelectObject(bb, oldBr); SelectObject(bb, oldPen);

                    // Buttons are saturated, so white always reads — using the
                    // theme foreground would vanish on the light theme.
                    SetTextColor(bb, btn.enabled ? RGB(245, 245, 245) : dim);
                    RECT lr = btn.rect;
                    DrawTextW(bb, btn.label.c_str(), -1, &lr,
                              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    x += bw + gap;
                }
                }
            }

            // ── Rows (scrolled) ──────────────────────────────────────────────
            // Two button rows sit above the list.
            const int listTop = static_cast<int>(TITLE_H * s) +
                                btnH * 2 + static_cast<int>(BTN_GAP * s) +
                                static_cast<int>(10 * s);
            const int listBot = H - static_cast<int>(FOOTER_H * s);

            HRGN clip = CreateRectRgn(0, listTop, W, listBot);
            SelectClipRgn(bb, clip);

            // Leave room for the scrollbar so text never runs under it.
            const int sbW = UI::ScrollBarThicknessPx(s);
            const int listRight = W - pad - sbW;

            // Content height is only known after the row loop below, so the
            // view is set by hand here and Layout is not used: the bar's column
            // is reserved whether or not a bar is drawn, which also stops a list
            // crossing the "needs scrolling" threshold from reflowing its rows
            // sideways under the cursor.
            m_list.view = {pad, listTop, listRight, listBot};

            int y = listTop - m_list.scrollY;
            for (size_t i = 0; i < m_rows.size(); ++i) {
                Row &row = m_rows[i];
                const int h = (row.kind == Kind::Header) ? hdrH : rowH;
                row.rect = {pad, y, listRight, y + h};

                if (y + h >= listTop && y <= listBot) {
                    if (row.kind == Kind::Header) {
                        // Accent bar makes sections scannable in a long list.
                        RECT bar{row.rect.left, row.rect.top + static_cast<int>(10 * s),
                                 row.rect.left + static_cast<int>(3 * s),
                                 row.rect.bottom - static_cast<int>(2 * s)};
                        FillRect(bb, &bar, UI::Gdi::Brush(PC::STRIPE));

                        SelectObject(bb, m_hFontBold);
                        SetTextColor(bb, PC::HEADER);
                        RECT lr{row.rect.left + static_cast<int>(10 * s),
                                row.rect.top + static_cast<int>(8 * s),
                                row.rect.right, row.rect.bottom};
                        DrawTextW(bb, row.label.c_str(), -1, &lr, DT_LEFT | DT_SINGLELINE);
                    } else {
                        if (static_cast<int>(i) == m_selected) {
                            FillRect(bb, &row.rect, UI::Gdi::Brush(selBg));
                        } else if (static_cast<int>(i) == m_hotRow) {
                            FillRect(bb, &row.rect, UI::Gdi::Brush(hotBg));
                        }

                        // Line 1: label + value
                        const int line1H = static_cast<int>(22 * s);
                        SelectObject(bb, m_hFontBody);
                        SetTextColor(bb, fg);
                        RECT lr{row.rect.left + static_cast<int>(10 * s), row.rect.top,
                                row.rect.left + labelW, row.rect.top + line1H};
                        DrawTextW(bb, row.label.c_str(), -1, &lr,
                                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

                        RECT vr{row.rect.left + labelW, row.rect.top,
                                row.rect.right - static_cast<int>(10 * s),
                                row.rect.top + line1H};
                        if (static_cast<int>(i) == m_editingRow) {
                            m_edit.Draw(bb, m_hFontBody, vr, static_cast<int>(6 * s),
                                        GetFocus() == GetHwnd());
                        } else {
                            // Colour by MEANING, so a setting's state reads at a
                            // glance without parsing the words.
                            const bool ph = row.value == L"(not set)" ||
                                            row.value == L"(optional)" ||
                                            row.value == L"(required)";
                            COLORREF vc = PC::TEXT;
                            if (ph)                              vc = PC::WARN;
                            else if (row.value == L"On")         vc = PC::ON;
                            else if (row.value == L"Off")        vc = PC::OFF;
                            else switch (row.kind) {
                                case Kind::Folder: vc = PC::PATH;   break;
                                case Kind::Number: vc = PC::NUMBER; break;
                                case Kind::Choice: vc = PC::CHOICE; break;
                                default:           vc = PC::TEXT;   break;
                            }
                            SetTextColor(bb, vc);
                            DrawTextW(bb, row.value.c_str(), -1, &vr,
                                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                        }

                        // Line 2: what the setting actually does.
                        if (row.desc && row.desc[0]) {
                            SelectObject(bb, m_hFontSmall);
                            SetTextColor(bb, dim);
                            RECT dr{row.rect.left + static_cast<int>(10 * s),
                                    row.rect.top + line1H,
                                    row.rect.right - static_cast<int>(10 * s),
                                    row.rect.bottom - static_cast<int>(2 * s)};
                            DrawTextW(bb, row.desc, -1, &dr,
                                      DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
                        }

                        HGDIOBJ op = SelectObject(bb, UI::Gdi::Pen(line));
                        MoveToEx(bb, row.rect.left, row.rect.bottom, nullptr);
                        LineTo(bb, row.rect.right, row.rect.bottom);
                        SelectObject(bb, op);
                    }
                }
                y += h;
            }
            m_list.contentH = y + m_list.scrollY - listTop;

            SelectClipRgn(bb, nullptr);
            DeleteObject(clip);

            // ── Scrollbar ────────────────────────────────────────────────────
            // Drawn only when there is something to scroll — a permanent empty
            // track on a short list is just noise.
            // Cleared, then set only when there is something to scroll: the
            // track rect IS the hit box, so a stale one leaves a strip of empty
            // panel swallowing clicks.
            m_list.ClearBars();
            if (m_list.MaxScrollY() > 0 && m_list.Height() > 0)
                m_list.vTrack = {listRight, listTop, listRight + sbW, listBot};

            DrawBars(bb, m_list, s,
                     ThemeScrollBarColors(app.themeFactor));

            // ── Footer ───────────────────────────────────────────────────────
            SelectObject(bb, m_hFontSmall);
            SetTextColor(bb, dim);
            RECT fr{pad, listBot, W - pad, H};
            DrawTextW(bb,
                (m_editingRow >= 0
                    ? std::wstring(L"Enter = save   Esc = cancel")
                    : std::wstring(Constants::ThemeIcons::ICON_ARROWS_UP_DOWN) + L" select   Enter = edit   wheel = scroll   Esc = close")
                    .c_str(),
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

            // Dragging the thumb maps cursor travel onto scroll range, keeping
            // the grab point under the cursor so the thumb does not jump.
            // The thumb's hot state belongs to the base now — see the note in
            // RemoteClientsWnd's equivalent.
            const bool linkHot = PtInRect(&m_iniLinkRect, pt) != FALSE;
            const int hr = HitTestRow(pt), hb = HitTestButton(pt);
            if (hr != m_hotRow || hb != m_hotButton ||
                linkHot != m_iniLinkHot) {
                m_hotRow = hr; m_hotButton = hb;
                m_iniLinkHot = linkHot;
                Repaint();
            }
            if (m_editingRow >= 0) m_edit.RouteMouse(message, wParam, lParam, GetHwnd());
            return 0;
        }

        case WM_LBUTTONDOWN: {
            const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};

            // The subtitle link sits above every control, so nothing else can
            // claim this point.
            if (PtInRect(&m_iniLinkRect, pt)) {
                UI::Link::Reveal(Dedicated::SettingsFilePath());
                return 0;
            }

            // Scrollbar first — it overlays the row band on the right.
            // The bar never reaches here — FloatingPanelWnd consumes thumb and
            // track clicks before this panel is asked.

            if (m_editingRow >= 0 && PtInRect(&m_rows[m_editingRow].rect, pt)) {
                m_edit.RouteMouse(message, wParam, lParam, GetHwnd());
                Repaint();
                return 0;
            }
            if (m_editingRow >= 0) CommitTextEdit();

            const int bi = HitTestButton(pt);
            if (bi >= 0) {
                switch (m_buttons[bi].id) {
                    case BTN_GENERATE_APP:    DoGenerateApp();    break;
                    case BTN_GENERATE_CONFIG: DoGenerateConfig(); break;
                    case BTN_ADD_IMAGES:      DoAddImages();      break;
                    case BTN_ADD_PROMOS:      DoAddPromotions();  break;
                    case BTN_ADD_STARTUP:     DoAddStartup();     break;
                    case BTN_REMOVE_STARTUP:  DoRemoveStartup();  break;
                    case BTN_TEST_CONFIG:     DoTest();           break;
                    case BTN_TEST_IMAGES:     DoTestList(false);  break;
                    case BTN_TEST_PROMOS:     DoTestList(true);   break;
                    default: break;
                }
                return 0;
            }

            const int ri = HitTestRow(pt);
            if (ri >= 0) { m_selected = ri; Repaint(); EditRow(ri); }
            return 0;
        }

        // A thumb release never arrives here — the base holds capture for the
        // whole drag and consumes the button-up that ends it.
        case WM_LBUTTONUP:
            [[fallthrough]];
        case WM_RBUTTONUP:
            if (m_editingRow >= 0 &&
                m_edit.RouteMouse(message, wParam, lParam, GetHwnd()) ==
                    InputResult::ConsumedRepaint) Repaint();
            return 0;

        // No wheel cases: FloatingPanelWnd drives both wheels against
        // ScrollViewAt() and consumes them before this panel is asked.

        default: break;
    }
    return DefWindowProcW(GetHwnd(), message, wParam, lParam);
}

} // namespace UI
