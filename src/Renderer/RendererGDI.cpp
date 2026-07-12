#include "RendererGDI.h"
#include "../AppState.h"
#include "../Platform/Constants.h"
#include <algorithm>

RendererGDI::RendererGDI() = default;

RendererGDI::~RendererGDI() {
    DestroyBackBuffer();
    if (app.hDIB) {
        (void) DeleteObject(app.hDIB);
        app.hDIB = nullptr;
    }
    if (m_backgroundBrush) {
        (void) DeleteObject(m_backgroundBrush);
        m_backgroundBrush = nullptr;
    }
}

HRESULT RendererGDI::Initialize(HWND hwnd) {
    m_hwnd = hwnd;
    BYTE bgVal = static_cast<BYTE>(Constants::Theme::Background::MAIN_WINDOW * 255);
    m_backgroundBrush = CreateSolidBrush(RGB(bgVal, bgVal, bgVal));
    return m_backgroundBrush ? S_OK : E_FAIL;
}


void RendererGDI::UpdateColorEffects() {
    // GDI renderer does not support D2D effects
}

void RendererGDI::SetThemeFactor(float factor) {
    if (m_backgroundBrush) { DeleteObject(m_backgroundBrush); m_backgroundBrush = nullptr; }
    const BYTE v = static_cast<BYTE>(Constants::Theme::Apply(Constants::Theme::Background::MAIN_WINDOW, factor) * 255.0f + 0.5f);
    m_backgroundBrush = CreateSolidBrush(RGB(v, v, v));
}

void RendererGDI::Resize(UINT width, UINT height) {
    if (m_windowWidth == width && m_windowHeight == height) return;
    m_windowWidth = width;
    m_windowHeight = height;
    (void) CreateBackBuffer(width, height);
}

HRESULT RendererGDI::LoadBitmap(IWICBitmapSource *bitmap, UINT width, UINT height, const std::wstring & /*filePath*/) {
    if (!bitmap) return E_FAIL;

    m_imageWidth = width;
    m_imageHeight = height;

    if (app.hDIB) {
        (void) DeleteObject(app.hDIB);
        app.hDIB = nullptr;
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
    app.hDIB = CreateDIBSection(hdcScreen, &bmi, DIB_RGB_COLORS, &pPixels, nullptr, 0);
    ReleaseDC(m_hwnd, hdcScreen);

    if (app.hDIB && pPixels) {
        HRESULT hr = bitmap->CopyPixels(
                nullptr,
                width * 4,
                width * 4 * height,
                static_cast<BYTE *>(pPixels));

        if (FAILED(hr)) {
            (void) DeleteObject(app.hDIB);
            app.hDIB = nullptr;
            return hr;
        }
    }

    return app.hDIB ? S_OK : E_FAIL;
}

HRESULT RendererGDI::Render() {
    if (!m_backDC || !m_hwnd) return E_FAIL;

    RECT rc = {0, 0, static_cast<LONG>(m_windowWidth), static_cast<LONG>(m_windowHeight)};
    FillRect(m_backDC, &rc, m_backgroundBrush);

    if (app.hDIB) {
        // 1. Calculate ratios using floats for precision
        float ratioX = static_cast<float>(m_windowWidth) / m_imageWidth;
        float ratioY = static_cast<float>(m_windowHeight) / m_imageHeight;

        float renderW = static_cast<float>(m_imageWidth);
        float renderH = static_cast<float>(m_imageHeight);

        // 2. Exact, rigid axis control (Identical to D2D logic)
        switch (app.viewMode) {
            case Constants::ViewModes::ViewMode::FitToView_PreserveAspectRatio:
                renderW = m_imageWidth * (std::min)(ratioX, ratioY);
                renderH = m_imageHeight * (std::min)(ratioX, ratioY);
                break;

            case Constants::ViewModes::ViewMode::FitToWidth_DoNotPreserveAspectRatio:
                // Force width to window edges
                renderW = static_cast<float>(m_windowWidth);
                // Take original height
                renderH = static_cast<float>(m_imageHeight);
                // The Hard Stop: Crush to window height if it spills
                if (renderH > m_windowHeight) {
                    renderH = static_cast<float>(m_windowHeight);
                }
                break;

            case Constants::ViewModes::ViewMode::FitToHeight_DoNotPreserveAspectRatio:
                // Force height to window edges
                renderH = static_cast<float>(m_windowHeight);
                // Take original width
                renderW = static_cast<float>(m_imageWidth);
                // The Hard Stop: Crush to window width if it spills
                if (renderW > m_windowWidth) {
                    renderW = static_cast<float>(m_windowWidth);
                }
                break;

            case Constants::ViewModes::ViewMode::FitToWindow_DoNotPreserveAspectRatio:
                // Stretch both axes
                renderW = static_cast<float>(m_windowWidth);
                renderH = static_cast<float>(m_windowHeight);
                break;

            case Constants::ViewModes::ViewMode::OriginalImageSize_PreserveAspectRatio:
                // Raw 1:1 pixels
                renderW = static_cast<float>(m_imageWidth);
                renderH = static_cast<float>(m_imageHeight);
                break;
        }

        // 3. Apply Zoom and convert back to GDI integers for drawing
        const float z = (app.viewport.zoom <= 0.0f) ? 1.0f : app.viewport.zoom;
        int finalRenderW = static_cast<int>(renderW * z);
        int finalRenderH = static_cast<int>(renderH * z);

        int drawX = static_cast<int>((m_windowWidth - finalRenderW) / 2.0f + app.viewport.offsetX);
        int drawY = static_cast<int>((m_windowHeight - finalRenderH) / 2.0f + app.viewport.offsetY);

        HDC hdcDIB = CreateCompatibleDC(m_backDC);
        if (hdcDIB) {
            HBITMAP hbmOld = static_cast<HBITMAP>(SelectObject(hdcDIB, app.hDIB));
            SetStretchBltMode(m_backDC, HALFTONE);
            StretchBlt(m_backDC, drawX, drawY, finalRenderW, finalRenderH,
                       hdcDIB, 0, 0, m_imageWidth, m_imageHeight, SRCCOPY);
            SelectObject(hdcDIB, hbmOld);
            DeleteDC(hdcDIB);
        }
    }

    // Overlays text logic remains untouched
    if (!app.playlist.empty() && app.showOverlayInfoText) {
        std::wstring fullPath = app.playlist[app.currentIndex];
        std::wstring fileName = fullPath.substr(fullPath.find_last_of(L"\\/") + 1);
        std::wstring text = std::to_wstring(app.currentIndex + 1) + L" / " +
                            std::to_wstring(app.playlist.size()) + L" - " + fileName;

        HFONT hFont = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");

        if (hFont) {
            HFONT hOldFont = static_cast<HFONT>(SelectObject(m_backDC, hFont));
            SetBkMode(m_backDC, TRANSPARENT);
            SetTextColor(m_backDC, Constants::Theme::Color(
                    Constants::Theme::Renderer::TEXT_DEBUG_R,
                    Constants::Theme::Renderer::TEXT_DEBUG_G,
                    Constants::Theme::Renderer::TEXT_DEBUG_B));
            RECT textRect = {
                0, static_cast<LONG>(m_windowHeight) - 35,
                static_cast<LONG>(m_windowWidth) - 10,
                static_cast<LONG>(m_windowHeight) - 5
            };
            DrawTextW(m_backDC, text.c_str(), -1, &textRect, DT_SINGLELINE | DT_RIGHT | DT_VCENTER);
            SelectObject(m_backDC, hOldFont);
            DeleteObject(hFont);
        }
    }

    HDC hdc = GetDC(m_hwnd);
    if (hdc) {
        BitBlt(hdc, 0, 0, m_windowWidth, m_windowHeight, m_backDC, 0, 0, SRCCOPY);
        ReleaseDC(m_hwnd, hdc);
    }

    return S_OK;
}

HRESULT RendererGDI::PreloadBitmap(const std::wstring &, int) {
    return S_OK;
}

HRESULT RendererGDI::CreateBackBuffer(UINT width, UINT height) {
    DestroyBackBuffer();
    HDC hdcScreen = GetDC(m_hwnd);
    if (!hdcScreen) return E_FAIL;

    m_backDC = CreateCompatibleDC(hdcScreen);
    m_backBitmap = CreateCompatibleBitmap(hdcScreen, width, height);
    if (m_backDC && m_backBitmap) {
        m_backBitmapOld = static_cast<HBITMAP>(SelectObject(m_backDC, m_backBitmap));
    }
    ReleaseDC(m_hwnd, hdcScreen);
    return (m_backDC && m_backBitmap) ? S_OK : E_FAIL;
}

void RendererGDI::DestroyBackBuffer() {
    if (m_backDC) {
        if (m_backBitmapOld) SelectObject(m_backDC, m_backBitmapOld);
        DeleteDC(m_backDC);
        m_backDC = nullptr;
    }
    if (m_backBitmap) {
        DeleteObject(m_backBitmap);
        m_backBitmap = nullptr;
    }
}

void RendererGDI::ProcessPendingUploads() {}
