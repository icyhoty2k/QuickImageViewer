#pragma once
#include <intsafe.h>
#include <string>

namespace System {
    // Infrastructure
    void RegisterAppForOpenWith();

    void EnableRunOnStartup();

    // Integer/Flag persistence (DWORD)
    void SaveSetting(const wchar_t *valueName, DWORD value);

    DWORD LoadSetting(const wchar_t *valueName, DWORD defaultValue);

    // Text/Path persistence (String)
    void SaveStringSetting(const wchar_t *valueName, const std::wstring &value);

    void LoadStringSetting(const wchar_t *valueName, wchar_t *buffer, DWORD bufferSize);
}
