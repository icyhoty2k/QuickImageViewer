#include "RendererGDI.h"
#include "AppState.h"
#include <algorithm>

RendererGDI::RendererGDI() = default;

RendererGDI::~RendererGDI() {
    DestroyBackBuffer();
    if (g_app.hDIB) {
        (void) DeleteObject(g_app.hDIB);
        g_app.hDIB = nullptr;
    }
    if (m_backgroundBrush) {
        (void) DeleteObject(m_backgroundBrush);
        m_backgroundBrush = nullptr;
    }
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
    (void) CreateBackBuffer(width, height);
}

HRESULT RendererGDI::LoadBitmap(IWICBitmapSource *bitmap, UINT width, UINT height, const std::wstring & /*filePath*/) {
    // nullptr bitmap = cache-only probe. GDI has no cache, always miss.
    if (!bitmap) return E_FAIL;

    m_imageWidth = width;
    m_imageHeight = height;

    if (g_app.hDIB) {
        (void) DeleteObject(g_app.hDIB);
        g_app.hDIB = nullptr;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = static_cast<LONG>(width);
    bmi.bmiHeader.biHeight = -static_cast<LONG>(height);
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void *pPixels = nullptr;
    HDC hdcScreen = GetDC(m_hwnd);
    g_app.hDIB = CreateDIBSection(hdcScreen, &bmi, DIB_RGB_COLORS, &pPixels, nullptr, 0);
    ReleaseDC(m_hwnd, hdcScreen);

    if (g_app.hDIB && pPixels) {
        HRESULT hr = bitmap->CopyPixels(
                nullptr,
                width * 4,
                width * 4 * height,
                static_cast<BYTE *>(pPixels));

        if (FAILED(hr)) {
            (void) DeleteObject(g_app.hDIB);
            g_app.hDIB = nullptr;
            return hr;
        }
    }

    return g_app.hDIB ? S_OK : E_FAIL;
}

HRESULT RendererGDI::Render() {
    if (!m_backDC || !m_hwnd) return E_FAIL;

    // 1. Clear background
    RECT rc = {0, 0, static_cast<LONG>(m_windowWidth), static_cast<LONG>(m_windowHeight)};
    FillRect(m_backDC, &rc, m_backgroundBrush);

    // 2. Draw scaled image
    if (g_app.hDIB) {
        float ratioX = static_cast<float>(m_windowWidth) / m_imageWidth;
        float ratioY = static_cast<float>(m_windowHeight) / m_imageHeight;
        float baseRatio = (std::min)(ratioX, ratioY);

        int renderW = static_cast<int>(m_imageWidth * baseRatio * g_app.viewport.zoom);
        int renderH = static_cast<int>(m_imageHeight * baseRatio * g_app.viewport.zoom);
        int drawX = static_cast<int>((m_windowWidth - renderW) / 2.0f + g_app.viewport.offsetX);
        int drawY = static_cast<int>((m_windowHeight - renderH) / 2.0f + g_app.viewport.offsetY);

        HDC hdcDIB = CreateCompatibleDC(m_backDC);
        HBITMAP hbmOld = static_cast<HBITMAP>(SelectObject(hdcDIB, g_app.hDIB));

        SetStretchBltMode(m_backDC, HALFTONE);
        StretchBlt(m_backDC, drawX, drawY, renderW, renderH,
                   hdcDIB, 0, 0, m_imageWidth, m_imageHeight, SRCCOPY);

        SelectObject(hdcDIB, hbmOld);
        DeleteDC(hdcDIB);
    }

    // NOTE: Render() is called from WM_PAINT between BeginPaint/EndPaint.
    // We must use the HWND's DC obtained via GetDC, not BeginPaint's HDC,
    // because the interface does not pass the HDC through. This works correctly
    // but means we do not use BeginPaint's clip region — acceptable for a full-window blit.

    // 3. Overlay text (drawn into back buffer before final blit)
    if (!g_app.playlist.empty() && g_app.showOverlayInfoText) {
        std::wstring fullPath = g_app.playlist[g_app.currentIndex];
        std::wstring fileName = fullPath.substr(fullPath.find_last_of(L"\\/") + 1);
        std::wstring text = std::to_wstring(g_app.currentIndex + 1) + L" / " +
                            std::to_wstring(g_app.playlist.size()) + L" - " + fileName;

        HFONT hFont = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
        HFONT hOldFont = static_cast<HFONT>(SelectObject(m_backDC, hFont));

        SetBkMode(m_backDC, TRANSPARENT);
        SetTextColor(m_backDC, RGB(0, 255, 0));

        RECT textRect = {
            0,
            static_cast<LONG>(m_windowHeight) - 35,
            static_cast<LONG>(m_windowWidth) - 10,
            static_cast<LONG>(m_windowHeight) - 5
        };
        DrawTextW(m_backDC, text.c_str(), -1, &textRect, DT_SINGLELINE | DT_RIGHT | DT_VCENTER);

        SelectObject(m_backDC, hOldFont);
        (void) DeleteObject(hFont);
    }

    // 4. Final blit to screen
    HDC hdc = GetDC(m_hwnd);
    BitBlt(hdc, 0, 0, m_windowWidth, m_windowHeight, m_backDC, 0, 0, SRCCOPY);
    ReleaseDC(m_hwnd, hdc);

    return S_OK;
}

HRESULT RendererGDI::PreloadBitmap(const std::wstring & /*filePath*/, int /*requestIndex*/) {
    return S_OK; // GDI has no VRAM cache — no-op
}

HRESULT RendererGDI::CreateBackBuffer(UINT width, UINT height) {
    DestroyBackBuffer();
    HDC hdcScreen = GetDC(m_hwnd);
    m_backDC = CreateCompatibleDC(hdcScreen);
    m_backBitmap = CreateCompatibleBitmap(hdcScreen, width, height);
    m_backBitmapOld = static_cast<HBITMAP>(SelectObject(m_backDC, m_backBitmap));
    ReleaseDC(m_hwnd, hdcScreen);
    return S_OK;
}

void RendererGDI::DestroyBackBuffer() {
    if (m_backDC) {
        (void) SelectObject(m_backDC, m_backBitmapOld);
        (void) DeleteDC(m_backDC);
        m_backDC = nullptr;
    }
    if (m_backBitmap) {
        (void) DeleteObject(m_backBitmap);
        m_backBitmap = nullptr;
    }
}

void RendererGDI::ProcessPendingUploads() {
    // No-op for GDI.
    // GDI uses system memory (DIB sections) and does not require
    // synchronized VRAM upload queuing like Direct2D.
}
