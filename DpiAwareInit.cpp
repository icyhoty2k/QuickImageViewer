#include "DpiAwareInit.h"

HWND CreateViewerWindow(HINSTANCE hInstance, const wchar_t* className) {
    int baseLogicalW = 1200;
    int baseLogicalH = 800;

    UINT dpi = GetDpiForSystem();

    int winW = MulDiv(baseLogicalW, dpi, 96);
    int winH = MulDiv(baseLogicalH, dpi, 96);

    int screenW = GetSystemMetricsForDpi(SM_CXSCREEN, dpi);
    int screenH = GetSystemMetricsForDpi(SM_CYSCREEN, dpi);

    int posX = (screenW - winW) / 2;
    int posY = (screenH - winH) / 2;

    return CreateWindowExW(
        WS_EX_APPWINDOW,
        className, L"Viewer",
        WS_POPUP,
        posX, posY, winW, winH,
        nullptr, nullptr, hInstance, nullptr
    );
}