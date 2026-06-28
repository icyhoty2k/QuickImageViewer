#include "DpiAwareInit.h"
#include "../Constants.h"

HWND CreateViewerWindow(HINSTANCE hInstance, const wchar_t *className) {
    // Step 1: Create the window using system DPI for initial placement
    UINT sysDpi = GetDpiForSystem();
    int winW = MulDiv(Config::BASE_WIDTH, sysDpi, 96);
    int winH = MulDiv(Config::BASE_HEIGHT, sysDpi, 96);

    int screenW = GetSystemMetricsForDpi(SM_CXSCREEN, sysDpi);
    int screenH = GetSystemMetricsForDpi(SM_CYSCREEN, sysDpi);

    HWND hWnd = CreateWindowExW(
        WS_EX_APPWINDOW,
        className, Config::APP_TASKBAR_NAME,
        WS_POPUP,
        (screenW - winW) / 2, (screenH - winH) / 2, winW, winH,
        nullptr, nullptr, hInstance, nullptr
    );

    // Step 2: Now that the window exists, get the actual monitor DPI it landed on
    UINT actualDpi = GetDpiForWindow(hWnd);
    if (actualDpi != sysDpi) {
        int actualW = MulDiv(Config::BASE_WIDTH, actualDpi, 96);
        int actualH = MulDiv(Config::BASE_HEIGHT, actualDpi, 96);

        MONITORINFO mi = {sizeof(mi)};
        GetMonitorInfo(MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST), &mi);

        int posX = mi.rcMonitor.left + (mi.rcMonitor.right - mi.rcMonitor.left - actualW) / 2;
        int posY = mi.rcMonitor.top + (mi.rcMonitor.bottom - mi.rcMonitor.top - actualH) / 2;

        SetWindowPos(hWnd, nullptr, posX, posY, actualW, actualH, SWP_NOZORDER | SWP_NOACTIVATE);
    }

    return hWnd;
}
