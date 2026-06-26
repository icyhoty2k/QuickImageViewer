#pragma once

namespace Config {

    constexpr const wchar_t* APP_NAME         = L"QuickImageViewer";
    constexpr const wchar_t* APP_TASKBAR_NAME = L"QIV";
    constexpr const wchar_t* APP_VERSION      = L"1.0.0"; // major.minor.patch

    static constexpr float ZOOM_STEP          = 1.1f;  // +/- keys and ctrl+wheel
    static constexpr float ZOOM_LMB           = 3.0f;  // left click zoom multiplier

    constexpr int BASE_WIDTH                  = 1200;
    constexpr int BASE_HEIGHT                 = 800;

}