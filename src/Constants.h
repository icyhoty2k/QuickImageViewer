#pragma once

namespace Constants {
    constexpr const wchar_t *BASE_NAME = L"QuickImageViewer";

    constexpr const wchar_t *APP_CREATOR = L"Ivan Hristov Yanev";
    constexpr const wchar_t *APP_HELP_FOOTER = L"Copyright® 06.2026 All rights reserved";
    constexpr const wchar_t *APP_TASKBAR_NAME = L"QIV";
    constexpr const wchar_t *APP_VERSION = L"1.0.0"; // major.minor.patch

    static constexpr float ZOOM_STEP = 1.1f; // +/- keys and ctrl+wheel
    static constexpr float ZOOM_CLICK = 3.0f; // left click zoom multiplier
    static constexpr int OPACITY_STEP = 10; // left click zoom multiplier from 10 to 255 step is 10


    constexpr int BASE_WIDTH = 1200;
    constexpr int BASE_HEIGHT = 800;

    inline bool SWAP_MOUSE_BUTTONS = true; // Set this to true to swap Left and Right mouse button functions

    //==========================Cache optimization====================================
    constexpr const int VRAM_CACHE_IMAGES_COUNT = 30;
    constexpr const int PRELOAD_LOOKASIDE_COUNT = 3;
    constexpr const int PRELOAD_TIMER_COUNTDOWN = 150; // this is used to delay preloading if user scrolls very fast
    //==========================Cache optimization====================================


    // Custom window messages
    constexpr UINT WM_QIV_PENDING_UPLOADS = WM_USER + 1; // Posted by background decoder thread

    // =============================================================================
    constexpr const wchar_t *APP_NAME = BASE_NAME;
    constexpr const wchar_t *WINDOW_CLASS_NAME = BASE_NAME;
}
