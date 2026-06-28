#include "RegistrySetup.h"
#include <windows.h>
#include <string>
#include <shlobj.h>
#include "Constants.h"

namespace System {
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
        wchar_t exePath[32768];
        DWORD len = GetModuleFileNameW(nullptr, exePath, 32768);
        if (len == 0 || len >= 32768) return;

        std::wstring command = std::wstring(L"\"") + exePath + L"\" \"%1\"";
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

    void EnableRunOnStartup() {
        wchar_t exePath[32768];
        DWORD len = GetModuleFileNameW(nullptr, exePath, 32768);
        if (len == 0 || len >= 32768) return;

        std::wstring command = std::wstring(L"\"") + exePath + L"\" -background";

        HKEY hKey;
        if (RegCreateKeyExW(Constants::Registry::ROOT_HIVE, Constants::Registry::RUN_KEY,
                            0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
            bool needsUpdate = false;
            DWORD size = 0;
            DWORD type = 0;
            DWORD readType = 0;

            if (RegQueryValueExW(hKey, Constants::Registry::RUN_VALUE_NAME, nullptr, &type, nullptr, &size) == ERROR_SUCCESS) {
                if (type != REG_SZ || size > 1024 * 1024) {
                    needsUpdate = true;
                } else {
                    std::wstring current(size / sizeof(wchar_t), L'\0');
                    if (RegQueryValueExW(hKey, Constants::Registry::RUN_VALUE_NAME, nullptr, &readType,
                                         reinterpret_cast<LPBYTE>(current.data()), &size) == ERROR_SUCCESS) {
                        if (readType == REG_SZ) {
                            if (!current.empty() && current.back() == L'\0') current.pop_back();
                            if (current != command) needsUpdate = true;
                        } else {
                            needsUpdate = true;
                        }
                    }
                }
            } else {
                needsUpdate = true;
            }

            if (needsUpdate) {
                RegSetValueExW(hKey, Constants::Registry::RUN_VALUE_NAME, 0, REG_SZ, reinterpret_cast<const BYTE *>(command.c_str()),
                               static_cast<DWORD>((command.length() + 1) * sizeof(wchar_t)));
            }
            RegCloseKey(hKey);
        }
    }

    void SaveSetting(const wchar_t *valueName, DWORD value) {
        RegSetKeyValueW(Constants::Registry::ROOT_HIVE, Constants::Registry::ROOT_KEY,
                        valueName, REG_DWORD, &value, sizeof(DWORD));
    }

    DWORD LoadSetting(const wchar_t *valueName, DWORD defaultValue) {
        DWORD value = defaultValue;
        DWORD size = sizeof(DWORD);
        RegGetValueW(Constants::Registry::ROOT_HIVE, Constants::Registry::ROOT_KEY,
                     valueName, RRF_RT_REG_DWORD, nullptr, &value, &size);
        return value;
    }

    void SaveStringSetting(const wchar_t *valueName, const std::wstring &value) {
        RegSetKeyValueW(Constants::Registry::ROOT_HIVE, Constants::Registry::ROOT_KEY, valueName,
                        REG_SZ, value.c_str(), static_cast<DWORD>((value.length() + 1) * sizeof(wchar_t)));
    }

    void LoadStringSetting(const wchar_t *valueName, wchar_t *buffer, DWORD bufferSize) {
        DWORD size = bufferSize * sizeof(wchar_t);
        RegGetValueW(Constants::Registry::ROOT_HIVE, Constants::Registry::ROOT_KEY, valueName,
                     RRF_RT_REG_SZ, nullptr, buffer, &size);
    }
}
