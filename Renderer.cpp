#include "Renderer.h"
#include "AppState.h"
#include <algorithm>

void RenderViewport(HWND hWnd, HDC hdc) {
    RECT rect;
    GetClientRect(hWnd, &rect);
    int winW = rect.right - rect.left;
    int winH = rect.bottom - rect.top;

    HDC hdcMem = CreateCompatibleDC(hdc);
    HBITMAP hbmMem = CreateCompatibleBitmap(hdc, winW, winH);
    HBITMAP hbmOld = static_cast<HBITMAP>(SelectObject(hdcMem, hbmMem));

    HBRUSH bgBrush = CreateSolidBrush(RGB(15, 15, 15));
    FillRect(hdcMem, &rect, bgBrush);
    DeleteObject(bgBrush);

    if (g_app.hDIB) {
        float ratioX = static_cast<float>(winW) / g_app.imgWidth;
        float ratioY = static_cast<float>(winH) / g_app.imgHeight;
        float baseRatio = (std::min)(ratioX, ratioY);

        int renderW = static_cast<int>(g_app.imgWidth * baseRatio * g_app.viewport.zoom);
        int renderH = static_cast<int>(g_app.imgHeight * baseRatio * g_app.viewport.zoom);

        int drawX = static_cast<int>((winW - renderW) / 2.0f + g_app.viewport.offsetX);
        int drawY = static_cast<int>((winH - renderH) / 2.0f + g_app.viewport.offsetY);

        HDC hdcDIB = CreateCompatibleDC(hdcMem);
        HBITMAP hbmDIBOld = static_cast<HBITMAP>(SelectObject(hdcDIB, g_app.hDIB));

        SetStretchBltMode(hdcMem, HALFTONE);
        SetBrushOrgEx(hdcMem, 0, 0, nullptr);
        StretchBlt(hdcMem, drawX, drawY, renderW, renderH, hdcDIB, 0, 0, g_app.imgWidth, g_app.imgHeight, SRCCOPY);

        SelectObject(hdcDIB, hbmDIBOld);
        DeleteDC(hdcDIB);
    }

    BitBlt(hdc, 0, 0, winW, winH, hdcMem, 0, 0, SRCCOPY);
    SelectObject(hdcMem, hbmOld);
    DeleteObject(hbmMem);
    DeleteDC(hdcMem);
}