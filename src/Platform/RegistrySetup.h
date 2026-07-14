#pragma once
#include <intsafe.h>
#include <string>
#include <windows.h>
#include "Constants.h"

namespace System {
    // Infrastructure
    void RegisterAppForOpenWith();

    void EnableRunOnStartup(bool isEnabledRunOnStartup);

    // Integer/Flag persistence (DWORD)
    void SaveSetting(const wchar_t *valueName, DWORD value);

    DWORD LoadSetting(const wchar_t *valueName, DWORD defaultValue);

    // Text/Path persistence (String)
    void SaveStringSetting(const wchar_t *valueName, const std::wstring &value);

    void LoadStringSetting(const wchar_t *valueName, wchar_t *buffer, DWORD bufferSize);

    // Returns the registry string value as std::wstring; empty on missing/error.
    // Handles paths of any length (no MAX_PATH limit).
    std::wstring LoadStringSetting(const wchar_t *valueName);

    // Returns the full path of this exe as std::wstring.
    // Handles paths of any length — never truncates.
    inline std::wstring GetExePathW() {
        std::wstring buf(Constants::MAX_FILE_PATH, L'\0');
        DWORD len = GetModuleFileNameW(nullptr, buf.data(), Constants::MAX_FILE_PATH);
        if (len == 0 || len >= Constants::MAX_FILE_PATH) return {};
        buf.resize(len);
        return buf;
    }
}
