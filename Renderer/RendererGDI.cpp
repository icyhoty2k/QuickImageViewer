#include "RendererGDI.h"
#include "../AppState.h"
#include <algorithm>

RendererGDI::RendererGDI() = default;

RendererGDI::~RendererGDI() {
    DestroyBackBuffer();
    if (g_app.hDIB) DeleteObject(g_app.hDIB);
    if (m_backgroundBrush) DeleteObject(m_backgroundBrush);
}

HRESULT RendererGDI::Initialize(HWND hwnd) {
    m_hwnd = hwnd;
    m_backgroundBrush = CreateSolidBrush(RGB(20, 20, 20));
    return S_OK;
}

void RendererGDI::Resize(UINT width, UINT height) {
    if (m_windowWidth == width && m_windowHeight == height) return;
    m_windowWidth = width;
    m_windowHeight = height;
    CreateBackBuffer(width, height);
}

HRESULT RendererGDI::LoadBitmap(IWICBitmapSource* bitmap, UINT width, UINT height, const std::wstring& /*filePath*/) {
    // nullptr bitmap = cache-only probe. GDI has no cache, always miss.
    if (!bitmap) return E_FAIL;

    m_imageWidth  = width;
    m_imageHeight = height;

    if (g_app.hDIB) {
        DeleteObject(g_app.hDIB);
        g_app.hDIB = nullptr;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = (LONG)width;
    bmi.bmiHeader.biHeight      = -(LONG)height; // Top-down
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pPixels = nullptr;
    HDC hdcScreen = GetDC(m_hwnd);
    g_app.hDIB = CreateDIBSection(hdcScreen, &bmi, DIB_RGB_COLORS, &pPixels, nullptr, 0);
    ReleaseDC(m_hwnd, hdcScreen);

    if (g_app.hDIB && pPixels) {
        bitmap->CopyPixels(nullptr, width * 4, width * 4 * height, static_cast<BYTE*>(pPixels));
    }

    return g_app.hDIB ? S_OK : E_FAIL;
}

HRESULT RendererGDI::Render() {
    if (!m_backDC || !m_hwnd) return E_FAIL;

    // 1. Clear background
    RECT rc = { 0, 0, (LONG)m_windowWidth, (LONG)m_windowHeight };
    FillRect(m_backDC, &rc, m_backgroundBrush);

    // 2. Draw scaled image
    if (g_app.hDIB) {
        float ratioX  = static_cast<float>(m_windowWidth)  / m_imageWidth;
        float ratioY  = static_cast<float>(m_windowHeight) / m_imageHeight;
        float baseRatio = (std::min)(ratioX, ratioY);

        int renderW = static_cast<int>(m_imageWidth  * baseRatio * g_app.viewport.zoom);
        int renderH = static_cast<int>(m_imageHeight * baseRatio * g_app.viewport.zoom);
        int drawX   = static_cast<int>((m_windowWidth  - renderW) / 2.0f + g_app.viewport.offsetX);
        int drawY   = static_cast<int>((m_windowHeight - renderH) / 2.0f + g_app.viewport.offsetY);

        HDC hdcDIB = CreateCompatibleDC(m_backDC);
        HBITMAP hbmDIBOld = static_cast<HBITMAP>(SelectObject(hdcDIB, g_app.hDIB));

        SetStretchBltMode(m_backDC, HALFTONE);
        StretchBlt(m_backDC, drawX, drawY, renderW, renderH,
                   hdcDIB, 0, 0, m_imageWidth, m_imageHeight, SRCCOPY);

        SelectObject(hdcDIB, hbmDIBOld);
        DeleteDC(hdcDIB);
    }

    // 3. Final blit to screen.
    // NOTE: Render() is called from WM_PAINT between BeginPaint/EndPaint.
    // We must use the HWND's DC obtained via GetDC, not BeginPaint's HDC,
    // because the interface does not pass the HDC through. This works correctly
    // but means we do not use BeginPaint's clip region — acceptable for a full-window blit.
    HDC hdc = GetDC(m_hwnd);
    BitBlt(hdc, 0, 0, m_windowWidth, m_windowHeight, m_backDC, 0, 0, SRCCOPY);
    ReleaseDC(m_hwnd, hdc);

    return S_OK;
}

HRESULT RendererGDI::PreloadBitmap(const std::wstring& /*filePath*/) {
    return S_OK; // GDI has no VRAM cache — no-op
}

HRESULT RendererGDI::CreateBackBuffer(UINT width, UINT height) {
    DestroyBackBuffer();
    HDC hdcScreen = GetDC(m_hwnd);
    m_backDC        = CreateCompatibleDC(hdcScreen);
    m_backBitmap    = CreateCompatibleBitmap(hdcScreen, width, height);
    m_backBitmapOld = static_cast<HBITMAP>(SelectObject(m_backDC, m_backBitmap));
    ReleaseDC(m_hwnd, hdcScreen);
    return S_OK;
}

void RendererGDI::DestroyBackBuffer() {
    if (m_backDC) {
        SelectObject(m_backDC, m_backBitmapOld);
        DeleteDC(m_backDC);
        m_backDC = nullptr;
    }
    if (m_backBitmap) {
        DeleteObject(m_backBitmap);
        m_backBitmap = nullptr;
    }
}