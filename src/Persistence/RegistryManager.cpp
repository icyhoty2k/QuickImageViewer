#include "RegistryManager.h"
#include <windows.h>
#include <string>
#include <shlobj.h>
#include "../Platform/Constants.h"
#include "../AppState.h"

extern AppState app;

namespace Persistence::Registry {
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

    void SaveSetting(const wchar_t *valueName, DWORD value) {
        const std::wstring key = PrefixedName(valueName);
        RegSetKeyValueW(Constants::Registry::ROOT_HIVE, Constants::Registry::ROOT_KEY,
                        key.c_str(), REG_DWORD, &value, sizeof(DWORD));
    }

    DWORD LoadSetting(const wchar_t *valueName, DWORD defaultValue) {
        const std::wstring key = PrefixedName(valueName);
        DWORD value = defaultValue;
        DWORD size = sizeof(DWORD);
        RegGetValueW(Constants::Registry::ROOT_HIVE, Constants::Registry::ROOT_KEY,
                     key.c_str(), RRF_RT_REG_DWORD, nullptr, &value, &size);
        return value;
    }

    void SaveStringSetting(const wchar_t *valueName, const std::wstring &value) {
        const std::wstring key = PrefixedName(valueName);
        RegSetKeyValueW(Constants::Registry::ROOT_HIVE, Constants::Registry::ROOT_KEY, key.c_str(),
                        REG_SZ, value.c_str(), static_cast<DWORD>((value.length() + 1) * sizeof(wchar_t)));
    }

    void LoadStringSetting(const wchar_t *valueName, wchar_t *buffer, DWORD bufferSize) {
        const std::wstring key = PrefixedName(valueName);
        DWORD size = bufferSize * sizeof(wchar_t);
        RegGetValueW(Constants::Registry::ROOT_HIVE, Constants::Registry::ROOT_KEY, key.c_str(),
                     RRF_RT_REG_SZ, nullptr, buffer, &size);
    }

    std::wstring LoadStringSetting(const wchar_t *valueName) {
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
}
