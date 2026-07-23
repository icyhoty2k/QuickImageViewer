#include "DpiAwareInit.h"
#include "Constants.h"
#include "../AppState.h"

HWND CreateViewerWindow(HINSTANCE hInstance, const wchar_t *className) {
    UINT sysDpi = GetDpiForSystem();
    int winW = MulDiv(app.baseWidth, sysDpi, 96);
    int winH = MulDiv(app.baseHeight, sysDpi, 96);

    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);

    int workW = mi.rcWork.right - mi.rcWork.left;
    int workH = mi.rcWork.bottom - mi.rcWork.top;

    HWND hWnd = CreateWindowExW(
            WS_EX_APPWINDOW,
            className, Constants::APP_TASKBAR_NAME,
            WS_POPUP,
            mi.rcWork.left + (workW - winW) / 2,
            mi.rcWork.top  + (workH - winH) / 2,
            winW, winH,
            nullptr, nullptr, hInstance, nullptr
            );

    // Step 2: If the actual monitor DPI differs, re-center within the work area.
    UINT actualDpi = GetDpiForWindow(hWnd);
    if (actualDpi != sysDpi) {
        int actualW = MulDiv(app.baseWidth, actualDpi, 96);
        int actualH = MulDiv(app.baseHeight, actualDpi, 96);

        GetMonitorInfoW(MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST), &mi);

        int posX = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - actualW) / 2;
        int posY = mi.rcWork.top  + (mi.rcWork.bottom - mi.rcWork.top  - actualH) / 2;

        SetWindowPos(hWnd, nullptr, posX, posY, actualW, actualH, SWP_NOZORDER | SWP_NOACTIVATE);
    }

    return hWnd;
}
