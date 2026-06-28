#include "RegistrySetup.h"
#include <windows.h>
#include <string>
#include <shlobj.h>
#include <vector>

namespace System {
    // Helper function to check if the current path is already registered
    bool NeedsRegistration(const std::wstring &expectedCommand) {
        HKEY hKey;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\Applications\\QuickImageViewer.exe\\shell\\open\\command",
                          0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            DWORD bufferSize = 0;
            RegQueryValueExW(hKey, nullptr, nullptr, nullptr, nullptr, &bufferSize);
            if (bufferSize > 0) {
                std::vector<wchar_t> buffer(bufferSize / sizeof(wchar_t) + 1);
                if (RegQueryValueExW(hKey, nullptr, nullptr, nullptr, (LPBYTE) buffer.data(), &bufferSize) == ERROR_SUCCESS) {
                    RegCloseKey(hKey);
                    if (expectedCommand == buffer.data()) return false;
                    return true;
                }
            }
            RegCloseKey(hKey);
        }
        return true;
    }

    void RegisterAppForOpenWith() {
        // 1. Get the dynamic, absolute path to this running .exe
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);

        // Build the execution command: "H:\Path\To\QuickImageViewer.exe" "%1"
        std::wstring command = std::wstring(L"\"") + exePath + L"\" \"%1\"";

        // FAST EXIT: If already registered at this location, do absolutely nothing.
        if (!NeedsRegistration(command)) {
            return;
        }

        HKEY hKey;

        // 2. Register the executable command
        if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\Applications\\QuickImageViewer.exe\\shell\\open\\command",
                            0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
            RegSetValueExW(hKey, nullptr, 0, REG_SZ, (const BYTE *) command.c_str(), static_cast<DWORD>((command.length() + 1) * sizeof(wchar_t)));
            RegCloseKey(hKey);
        }

        // 3. Set the clean display name Windows will show in the menu
        if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\Applications\\QuickImageViewer.exe",
                            0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
            std::wstring friendlyName = L"Quick Image Viewer";
            RegSetValueExW(hKey, L"FriendlyAppName", 0, REG_SZ, (const BYTE *) friendlyName.c_str(), static_cast<DWORD>((friendlyName.length() + 1) * sizeof(wchar_t)));
            RegCloseKey(hKey);
        }

        // 4. Tell Windows exactly which extensions this app supports
        if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\Applications\\QuickImageViewer.exe\\SupportedTypes",
                            0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
            const wchar_t *emptyStr = L"";
            RegSetValueExW(hKey, L".jpg", 0, REG_SZ, (const BYTE *) emptyStr, sizeof(wchar_t));
            RegSetValueExW(hKey, L".jpeg", 0, REG_SZ, (const BYTE *) emptyStr, sizeof(wchar_t));
            RegSetValueExW(hKey, L".png", 0, REG_SZ, (const BYTE *) emptyStr, sizeof(wchar_t));
            RegSetValueExW(hKey, L".webp", 0, REG_SZ, (const BYTE *) emptyStr, sizeof(wchar_t));
            RegSetValueExW(hKey, L".bmp", 0, REG_SZ, (const BYTE *) emptyStr, sizeof(wchar_t));
            RegCloseKey(hKey);
        }

        // 5. Force Windows Explorer to refresh its icon and menu cache (Only runs on install/move!)
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    }

    void EnableRunOnStartup() {
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);

        // Build the command: "H:\Path\To\QuickImageViewer.exe" -background
        std::wstring command = std::wstring(L"\"") + exePath + L"\" -background";

        HKEY hKey;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                            0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
            bool needsUpdate = true;

            DWORD bufferSize = 0;
            RegQueryValueExW(hKey, L"QuickImageViewer", nullptr, nullptr, nullptr, &bufferSize);
            if (bufferSize > 0) {
                std::vector<wchar_t> buffer(bufferSize / sizeof(wchar_t) + 1);
                if (RegQueryValueExW(hKey, L"QuickImageViewer", nullptr, nullptr, (LPBYTE) buffer.data(), &bufferSize) == ERROR_SUCCESS) {
                    if (command == buffer.data()) needsUpdate = false;
                }
            }

            if (needsUpdate) {
                RegSetValueExW(hKey, L"QuickImageViewer", 0, REG_SZ, (const BYTE *) command.c_str(), static_cast<DWORD>((command.length() + 1) * sizeof(wchar_t)));
            }
            RegCloseKey(hKey);
        }
    }
}
