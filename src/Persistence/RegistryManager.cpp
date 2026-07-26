#include "RegistryManager.h"
#include <windows.h>
#include <string>
#include <algorithm>
#include <shlobj.h>
#include "../Common/Converters.h"
#include "../Platform/Constants.h"
#include "../Platform/WriteQueue.h"
#include "../AppState.h"
#include "../Dedicated/DedicatedSettings.h"

extern AppState app;

namespace Persistence::Registry {
    // Routes every persisted value to a per-instance INI when this process is a
    // dedicated instance. See src/Dedicated/DedicatedSettings.h for the why.
    // Returns the value name to use in the registry, prepending the dedicated-mode
    // prefix when this instance was launched with -dedicated.
    static std::wstring PrefixedName(const wchar_t *valueName) {
        if (app.isDedicated)
            return std::wstring(Constants::DedicatedMode::DEDICATED_MODE_GLOBAL_PREFIX) + valueName;
        return valueName;
    }

    bool NeedsRegistration(const std::wstring &expectedCommand) {
        HKEY hKey;
        // Uses dynamic ROOT_HIVE
        if (RegOpenKeyExW(Constants::Registry::ROOT_HIVE, Constants::Registry::OPEN_WITH_COMMAND,
                          0, KEY_READ, &hKey) != ERROR_SUCCESS) {
            return true;
        }

        DWORD size = 0;
        DWORD type = 0;
        if (RegQueryValueExW(hKey, nullptr, nullptr, &type, nullptr, &size) != ERROR_SUCCESS || type != REG_SZ) {
            RegCloseKey(hKey);
            return true;
        }

        if (size > 1024 * 1024) {
            RegCloseKey(hKey);
            return true;
        }

        std::wstring current(size / sizeof(wchar_t), L'\0');
        DWORD readType = 0;
        if (RegQueryValueExW(hKey, nullptr, nullptr, &readType,
                             reinterpret_cast<LPBYTE>(current.data()), &size) != ERROR_SUCCESS || readType != REG_SZ) {
            RegCloseKey(hKey);
            return true;
        }
        RegCloseKey(hKey);

        if (!current.empty() && current.back() == L'\0') current.pop_back();

        return current != expectedCommand;
    }

    void RegisterAppForOpenWith() {
        // Keyed on the DEDICATED flag, not on being file-backed. A file-backed
        // MAIN app is simply portable: its settings live in the .ini, but it is
        // still the machine's image viewer, so it must own the association —
        // and that is inherently a registry write, with the CURRENT exe path.
        // Only an appliance stays out of the registry entirely.
        if (Dedicated::IsDedicatedFlag()) return;

        // Allocate the 32k character buffer cleanly on the heap
        std::wstring exePathStr(32768, L'\0');
        DWORD len = GetModuleFileNameW(nullptr, exePathStr.data(), static_cast<DWORD>(exePathStr.size()));

        if (len == 0 || len >= 32768) return;

        // Shrink the wstring to the exact actual path length
        exePathStr.resize(len);

        std::wstring command = std::wstring(L"\"") + exePathStr + L"\" \"%1\"";
        if (!NeedsRegistration(command)) return;

        HKEY hKey;
        // 1. Command registration using ROOT_HIVE
        if (RegCreateKeyExW(Constants::Registry::ROOT_HIVE, Constants::Registry::OPEN_WITH_COMMAND,
                            0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
            RegSetValueExW(hKey, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE *>(command.c_str()),
                           static_cast<DWORD>((command.length() + 1) * sizeof(wchar_t)));
            RegCloseKey(hKey);
        }

        // 2. Friendly application name
        if (RegCreateKeyExW(Constants::Registry::ROOT_HIVE, Constants::Registry::OPEN_WITH_ROOT,
                            0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
            const std::wstring friendlyName = L"Quick Image Viewer";
            RegSetValueExW(hKey, L"FriendlyAppName", 0, REG_SZ, reinterpret_cast<const BYTE *>(friendlyName.c_str()),
                           static_cast<DWORD>((friendlyName.length() + 1) * sizeof(wchar_t)));
            RegCloseKey(hKey);
        }

        // 3. Supported image types
        if (RegCreateKeyExW(Constants::Registry::ROOT_HIVE, Constants::Registry::OPEN_WITH_TYPES,
                            0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
            const wchar_t *empty = L"";
            for (size_t i = 0; i < Constants::Registry::SUPPORTED_EXTENSIONS_COUNT; ++i) {
                RegSetValueExW(hKey, Constants::Registry::SUPPORTED_EXTENSIONS[i], 0, REG_SZ,
                               reinterpret_cast<const BYTE *>(empty), sizeof(wchar_t));
            }
            RegCloseKey(hKey);
        }

        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    }

    void EnableRunOnStartup(bool isEnabledRunOnStartup) {
        // Same split as RegisterAppForOpenWith: a file-backed MAIN app still
        // registers auto-start in the Run key — the flag is read from its .ini,
        // and the command written below always uses the current exe path, so
        // moving a portable copy re-points the entry on the next launch.
        //
        // A DEDICATED instance never does: it auto-starts from a shortcut in
        // shell:startup instead (the Generate button in the F8 panel).
        if (Dedicated::IsDedicatedFlag()) return;

        // Dedicated and normal instances each get their own Run key entry so they
        // can coexist without clobbering each other. The dedicated entry name is
        // built from the same prefix that governs all other dedicated-mode names.
        const std::wstring runValueName = app.isDedicated
            ? std::wstring(Constants::DedicatedMode::DEDICATED_MODE_GLOBAL_PREFIX) + Constants::Registry::RUN_VALUE_NAME
            : Constants::Registry::RUN_VALUE_NAME;

        HKEY hKey;
        if (RegCreateKeyExW(Constants::Registry::ROOT_HIVE, Constants::Registry::RUN_KEY,
                            0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, nullptr) != ERROR_SUCCESS) {
            return;
        }

        if (!isEnabledRunOnStartup) {
            RegDeleteValueW(hKey, runValueName.c_str());
        } else {
            std::wstring exePathStr(32768, L'\0');
            DWORD len = GetModuleFileNameW(nullptr, exePathStr.data(), static_cast<DWORD>(exePathStr.size()));
            if (len == 0 || len >= 32768) {
                RegCloseKey(hKey);
                return;
            }
            exePathStr.resize(len);

            // Dedicated instances must relaunch with -dedicated so they stay isolated.
            std::wstring command = std::wstring(L"\"") + exePathStr +
                                   (app.isDedicated ? L"\" -dedicated -background" : L"\" -background");

            bool needsUpdate = true;
            DWORD size = 0, type = 0;
            if (RegQueryValueExW(hKey, runValueName.c_str(), nullptr, &type, nullptr, &size) == ERROR_SUCCESS) {
                if (type == REG_SZ && size <= 1024 * 1024) {
                    std::wstring current(size / sizeof(wchar_t), L'\0');
                    if (RegQueryValueExW(hKey, runValueName.c_str(), nullptr, nullptr,
                                         reinterpret_cast<LPBYTE>(current.data()), &size) == ERROR_SUCCESS) {
                        if (!current.empty() && current.back() == L'\0') current.pop_back();
                        if (current == command) needsUpdate = false;
                    }
                }
            }

            if (needsUpdate) {
                RegSetValueExW(hKey, runValueName.c_str(), 0, REG_SZ,
                               reinterpret_cast<const BYTE *>(command.c_str()),
                               static_cast<DWORD>((command.length() + 1) * sizeof(wchar_t)));
            }
        }
        RegCloseKey(hKey);
    }

    // =========================================================================
    // DEDICATED INSTANCES BYPASS THE REGISTRY ENTIRELY.
    //
    // Each of the four accessors below checks Dedicated::SettingsUseFile() and
    // routes to <exe folder>\qivDedicated<Name>.ini instead. Intercepting here —
    // rather than at the call sites — means every existing SaveSetting/
    // LoadSetting caller keeps working untouched, and no future one can
    // accidentally write to the registry from a dedicated copy.
    // =========================================================================
    void SaveSetting(const wchar_t *valueName, DWORD value) {
        if (Dedicated::SettingsUseFile()) {
            Dedicated::WriteDword(valueName, value);
            return;
        }
        g_writeQueue.PushDword(PrefixedName(valueName), value);
    }

    DWORD LoadSetting(const wchar_t *valueName, DWORD defaultValue) {
        if (Dedicated::SettingsUseFile())
            return Dedicated::ReadDword(valueName, defaultValue);

        const std::wstring key = PrefixedName(valueName);
        DWORD value = defaultValue;
        DWORD size = sizeof(DWORD);
        RegGetValueW(Constants::Registry::ROOT_HIVE, Constants::Registry::ROOT_KEY,
                     key.c_str(), RRF_RT_REG_DWORD, nullptr, &value, &size);
        return value;
    }

    void SaveStringSetting(const wchar_t *valueName, const std::wstring &value) {
        if (Dedicated::SettingsUseFile()) {
            Dedicated::WriteString(valueName, value);
            return;
        }
        g_writeQueue.PushString(PrefixedName(valueName), value);
    }

    std::wstring LoadStringSetting(const wchar_t *valueName) {
        if (Dedicated::SettingsUseFile())
            return Dedicated::ReadString(valueName);

        const std::wstring key = PrefixedName(valueName);
        DWORD size = 0;
        if (RegGetValueW(Constants::Registry::ROOT_HIVE, Constants::Registry::ROOT_KEY, key.c_str(),
                         RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS || size == 0)
            return {};
        std::wstring result(size / sizeof(wchar_t), L'\0');
        if (RegGetValueW(Constants::Registry::ROOT_HIVE, Constants::Registry::ROOT_KEY, key.c_str(),
                         RRF_RT_REG_SZ, nullptr, result.data(), &size) != ERROR_SUCCESS)
            return {};
        // RegGetValueW includes the null terminator in size; trim it
        while (!result.empty() && result.back() == L'\0')
            result.pop_back();
        return result;
    }

    void LoadAllSettings(AppState &a) {
        // Open the key once. RegQueryValueExW then reads each value from the already-open
        // handle — no per-setting open/close round-trips through the registry driver.
        const std::wstring prefix = app.isDedicated
            ? std::wstring(Constants::DedicatedMode::DEDICATED_MODE_GLOBAL_PREFIX)
            : std::wstring{};

        HKEY hKey = nullptr;
        RegOpenKeyExW(Constants::Registry::ROOT_HIVE, Constants::Registry::ROOT_KEY,
                      0, KEY_READ, &hKey);
        // hKey is null on first launch (key doesn't exist yet); readDword returns defaults.

        // Dedicated instances read from their INI instead — see SaveSetting.
        const bool useFile = Dedicated::SettingsUseFile();

        const auto readDword = [&](const wchar_t *name, DWORD def) -> DWORD {
            if (useFile) return Dedicated::ReadDword(name, def);
            if (!hKey) return def;
            const std::wstring fullName = prefix + name;
            DWORD val = def, size = sizeof(DWORD), type = 0;
            if (RegQueryValueExW(hKey, fullName.c_str(), nullptr, &type,
                                 reinterpret_cast<LPBYTE>(&val), &size) != ERROR_SUCCESS
                || type != REG_DWORD)
                return def;
            return val;
        };

        a.isEnableRunOnStartup = readDword(
            Constants::Registry::RUN_ON_STARTUP,
            static_cast<DWORD>(Constants::IS_ENABLE_RUN_ON_STARTUP)) != 0;
        a.isKeepInBackground = readDword(
            Constants::Registry::KEEP_IN_BACKGROUND,
            static_cast<DWORD>(Constants::IS_KEEP_IN_BACKGROUND)) != 0;
        a.thumbnailEffectsEnabled = readDword(
            Constants::Registry::THUMBNAIL_EFFECTS,
            static_cast<DWORD>(Constants::ThumbnailPanel::ThumbnailEffects::EFFECTS_MASTER_ENABLED)) != 0;
        a.historyFullModeEnabled = readDword(
            Constants::Registry::HISTORY_FULL_MODE,
            static_cast<DWORD>(Constants::History::HISTORY_SHOW_FULL_HISTORY)) != 0;
        a.showOverlayInfoText = readDword(
            Constants::Registry::OVERLAY_VISIBLE,
            static_cast<DWORD>(Constants::Overlay::DEFAULT_SHOW_OVERLAY)) != 0;
        a.openDirWndOnStart = readDword(
            Constants::Registry::OPEN_DIRWND_ON_START,
            static_cast<DWORD>(Constants::IS_OPEN_DIRWND_ON_START)) != 0;
        a.overlayShowBackground = readDword(
            Constants::Registry::OVERLAY_SHOW_BG,
            static_cast<DWORD>(Constants::Overlay::IS_OVERLAY_SHOW_BACKGROUND)) != 0;
        a.overlayLayoutMode = std::max(0, std::min(Constants::Overlay::LAYOUT_MODE_COUNT - 1,
            static_cast<int>(readDword(Constants::Registry::OVERLAY_LAYOUT_MODE,
                static_cast<DWORD>(Constants::Overlay::DEFAULT_LAYOUT_MODE)))));
        // Only the low 9 bits are meaningful — mask so a corrupt value cannot
        // set bits for slots that do not exist.
        a.overlaySlotVisibleMask = readDword(
            Constants::Registry::OVERLAY_SLOT_VISIBLE,
            static_cast<DWORD>(Constants::Overlay::DEFAULT_SLOT_VISIBLE_MASK))
            & Constants::Overlay::SLOT_MASK_ALL;
        a.overlaySlotCompactMask = readDword(
            Constants::Registry::OVERLAY_SLOT_COMPACT,
            static_cast<DWORD>(Constants::Overlay::DEFAULT_SLOT_COMPACT_MASK))
            & Constants::Overlay::SLOT_MASK_ALL;
        // Only the folder name is persisted; the effects-list toggle is
        // deliberately session-only and resets to its Constants default.
        a.overlayShowDirName = readDword(
            Constants::Registry::OVERLAY_SHOW_DIR_NAME,
            static_cast<DWORD>(Constants::Overlay::SHOW_DIR_NAME)) != 0;
        a.overlayFontSize = std::max(Constants::Overlay::OVERLAY_FONT_SIZE_MIN,
            std::min(Constants::Overlay::OVERLAY_FONT_SIZE_MAX,
                static_cast<int>(readDword(Constants::Registry::OVERLAY_FONT_SIZE,
                    static_cast<DWORD>(Constants::Overlay::OVERLAY_FONT_SIZE_DEFAULT)))));
        a.overlayFontColor = static_cast<COLORREF>(readDword(
            Constants::Registry::OVERLAY_FONT_COLOR,
            static_cast<DWORD>(Constants::Overlay::OVERLAY_FONT_COLOR_DEFAULT)));
        // Clamped: the saved index may point past the end if the family list
        // ever shrinks, and an out-of-range read would be a hard crash.
        a.overlayFontFamily = std::max(0, std::min(Constants::Overlay::OVERLAY_FONT_FAMILY_COUNT - 1,
            static_cast<int>(readDword(Constants::Registry::OVERLAY_FONT_FAMILY,
                static_cast<DWORD>(Constants::Overlay::OVERLAY_FONT_FAMILY_DEFAULT)))));
        a.caretStyle = std::max(0, std::min(1, static_cast<int>(
            readDword(Constants::Registry::INPUTBOX_CARET_STYLE,
                static_cast<DWORD>(Constants::InputBox::CARET_STYLE)))));
        int raw = static_cast<int>(
            readDword(Constants::Registry::ZOOM_CLICK_MULT,
                static_cast<DWORD>(Converters::toZoomInt(Constants::ZOOM_CLICK))));
        raw = std::max(Converters::toZoomInt(Constants::ZOOM_CLICK_MIN),
                       std::min(Converters::toZoomInt(Constants::ZOOM_CLICK_MAX), raw));
        a.zoomClickMultiplier = Converters::toZoomFloat(raw);
        a.swapMouseButtons = readDword(
            Constants::Registry::SWAP_MOUSE_BUTTONS,
            static_cast<DWORD>(Constants::IS_SWAP_MOUSE_BUTTONS)) != 0;
        a.contextMenuEnabled = readDword(
            Constants::Registry::CONTEXT_MENU_ENABLED,
            static_cast<DWORD>(Constants::IS_CONTEXT_MENU_ENABLED)) != 0;
        // KIOSK lock and always-on-top. Both are applied to the window in
        // ApplyCmdArgs, AFTER the command line — so -lock / -awaysOnTop can still
        // force them on for one launch without rewriting the stored value.
        a.isLocked = readDword(
            Constants::Registry::KIOSK_LOCK,
            static_cast<DWORD>(Constants::IS_KIOSK_LOCK_ENABLED)) != 0;
        a.isAlwaysOnTop = readDword(
            Constants::Registry::ALWAYS_ON_TOP,
            static_cast<DWORD>(Constants::IS_ALWAYS_ON_TOP)) != 0;
        a.keepDisplayAwake = readDword(
            Constants::Registry::KEEP_DISPLAY_AWAKE,
            static_cast<DWORD>(Constants::IS_KEEP_DISPLAY_AWAKE)) != 0;
        a.invertWheelDirection = readDword(
            Constants::Registry::WHEEL_INVERT,
            static_cast<DWORD>(Constants::IS_MOUSE_VERTICAL_REVERSE_SCROLL_DIRECTION)) != 0;
        a.invertWheelDirectionH = readDword(
            Constants::Registry::WHEEL_INVERT_H,
            static_cast<DWORD>(Constants::IS_MOUSE_HORIZONTAL_REVERSE_SCROLL_DIRECTION)) != 0;
        a.vramCacheCount = std::max(0, std::min(999, static_cast<int>(
            readDword(Constants::Registry::VRAM_CACHE_COUNT,
                static_cast<DWORD>(Constants::IS_VRAM_CACHE_IMAGES_COUNT)))));
        {
            int m = std::max(1, std::min(5, static_cast<int>(
                readDword(Constants::Registry::VIEW_MODE,
                    static_cast<DWORD>(Constants::ViewModes::defaultViewMode)))));
            a.viewMode = static_cast<Constants::ViewModes::ViewMode>(m);
        }
        a.baseWidth = std::max(240, std::min(16000, static_cast<int>(
            readDword(Constants::Registry::BASE_WIDTH_KEY,
                static_cast<DWORD>(Constants::IS_BASE_WIDTH)))));
        a.baseHeight = std::max(240, std::min(16000, static_cast<int>(
            readDword(Constants::Registry::BASE_HEIGHT_KEY,
                static_cast<DWORD>(Constants::IS_BASE_HEIGHT)))));
        a.startInFullscreen = readDword(
            Constants::Registry::START_FULLSCREEN,
            static_cast<DWORD>(Constants::IS_START_FULLSCREEN)) != 0;
        a.historyMaxDirs = std::max(0, std::min(999, static_cast<int>(
            readDword(Constants::Registry::HISTORY_MAX_DIRS,
                static_cast<DWORD>(Constants::History::IS_HISTORY_MAX_DIRS_TO_SHOW)))));
        a.historyMaxFavs = std::max(0, std::min(999, static_cast<int>(
            readDword(Constants::Registry::HISTORY_MAX_FAVS,
                static_cast<DWORD>(Constants::History::IS_HISTORY_MAX_FAVORITES_TO_SHOW)))));
        a.dirThumbCacheMB = std::max(100, std::min(64000, static_cast<int>(
            readDword(Constants::Registry::DIR_THUMB_CACHE_MB,
                static_cast<DWORD>(Constants::IS_DIR_THUMB_CACHE_BUDGET_MB)))));
        a.preloadLookaside = std::max(1, std::min(99, static_cast<int>(
            readDword(Constants::Registry::PRELOAD_LOOKASIDE,
                static_cast<DWORD>(Constants::IS_PRELOAD_LOOKASIDE_COUNT)))));
        a.msgCenterDisplayMs = std::max(250, std::min(10000, static_cast<int>(
            readDword(Constants::Registry::MSG_CENTER_MS,
                static_cast<DWORD>(Constants::Overlay::IS_MSG_CENTER_DISPLAY_MS)))));
        a.historyMaxDirsSave = std::max(1, std::min(99999, static_cast<int>(
            readDword(Constants::Registry::HISTORY_MAX_DIRS_SAVE,
                static_cast<DWORD>(Constants::History::IS_HISTORY_MAX_DIRS_TO_SAVE)))));
        a.slideshow.intervalMs = std::max(100, std::min(60000, static_cast<int>(
            readDword(Constants::Registry::SLIDESHOW_INTERVAL_MS,
                static_cast<DWORD>(Constants::Slideshow::IS_INTERVAL_MS)))));
        a.slideshow.loop = readDword(
            Constants::Registry::SLIDESHOW_LOOP,
            static_cast<DWORD>(Constants::Slideshow::IS_LOOP)) != 0;
        a.slideshow.shuffle = readDword(
            Constants::Registry::SLIDESHOW_SHUFFLE,
            static_cast<DWORD>(Constants::Slideshow::IS_SHUFFLE)) != 0;
        {
            int t = std::max(0, std::min(Constants::Slideshow::TRANSITION_COUNT - 1, static_cast<int>(
                readDword(Constants::Registry::SLIDESHOW_TRANSITION,
                    static_cast<DWORD>(TransitionType::Cut)))));
            a.slideshow.transition.type = static_cast<TransitionType>(t);
        }
        a.slideshow.transition.source = std::max(0,
            std::min(Constants::Slideshow::TransitionSource::COUNT - 1, static_cast<int>(
                readDword(Constants::Registry::SLIDESHOW_TRANS_SOURCE,
                    static_cast<DWORD>(Constants::Slideshow::TransitionSource::NONE)))));
        a.slideshow.transition.order = std::max(0,
            std::min(Constants::Slideshow::TransitionOrder::COUNT - 1, static_cast<int>(
                readDword(Constants::Registry::SLIDESHOW_TRANS_ORDER,
                    static_cast<DWORD>(Constants::Slideshow::TransitionOrder::SEQUENTIAL)))));
        a.slideshow.transition.listMask = static_cast<uint32_t>(
            readDword(Constants::Registry::SLIDESHOW_TRANS_LIST,
                Constants::Slideshow::TRANSITION_LIST_DEFAULT_MASK));
        a.fileHandlerDefaultSortOrder = std::max(0, std::min(4, static_cast<int>(
            readDword(Constants::Registry::SORT_ORDER,
                static_cast<DWORD>(Constants::FileHandler::FILE_HANDLER_DEFAULT_SORT_ORDER)))));
        a.fileHandlerIsReverseSortOrder = readDword(
            Constants::Registry::SORT_REVERSE,
            static_cast<DWORD>(Constants::FileHandler::FILE_HANDLER_SORT_TYPE_IS_REVERSE)) != 0;
        a.ctrlCEnabled = readDword(
            Constants::Registry::CTRL_C_ENABLED,
            static_cast<DWORD>(Constants::IS_CTRL_C_ENABLED)) != 0;
        a.thumbCopyEnabled = readDword(
            Constants::Registry::THUMB_COPY_ENABLED,
            static_cast<DWORD>(Constants::IS_THUMB_COPY_ENABLED)) != 0;
        a.thumbMoveEnabled = readDword(
            Constants::Registry::THUMB_MOVE_ENABLED,
            static_cast<DWORD>(Constants::IS_THUMB_MOVE_ENABLED)) != 0;
        a.thumbDeleteEnabled = readDword(
            Constants::Registry::THUMB_DELETE_ENABLED,
            static_cast<DWORD>(Constants::IS_THUMB_DELETE_ENABLED)) != 0;
        a.thumbPasteEnabled = readDword(
            Constants::Registry::THUMB_PASTE_ENABLED,
            static_cast<DWORD>(Constants::IS_THUMB_PASTE_ENABLED)) != 0;
        // The factor is a 0..1 ratio stored as a whole percent, so the DEFAULT
        // must be scaled UP before it is handed to readDword — casting the raw
        // ratio to DWORD truncates every value below 1.0 to 0. Clamped on the way
        // out like every other setting here: changeAppThemeFactor() clamps the
        // runtime setter, but startup assigns this field directly and would
        // otherwise push a corrupt value straight into the renderer.
        a.themeFactor = std::clamp(
            static_cast<float>(readDword(Constants::Registry::THEME_FACTOR,
                static_cast<DWORD>(Constants::Theme::DEFAULT_THEME_FACTOR *
                                   Constants::Theme::THEME_FACTOR_STORE_SCALE)))
            / Constants::Theme::THEME_FACTOR_STORE_SCALE,
            Constants::Theme::THEME_FACTOR_MIN,
            Constants::Theme::THEME_FACTOR_MAX);

        if (hKey) RegCloseKey(hKey);
    }
}
