//
// Created by aHome on 26-06-2026.
//

#include "RendererGDI.h"
#include "../AppState.h"
#include <algorithm>

RendererGDI::RendererGDI() = default;

RendererGDI::~RendererGDI() {
    // Clean up all resources
    DestroyBackBuffer();
    if (g_app.hDIB) DeleteObject(g_app.hDIB);
    if (m_backgroundBrush) DeleteObject(m_backgroundBrush);
}

// Initialize GDI resources
HRESULT RendererGDI::Initialize(HWND hwnd) {
    m_hwnd = hwnd;
    // Cache the brush to avoid recreating it every frame
    m_backgroundBrush = CreateSolidBrush(RGB(20, 20, 20));
    return S_OK;
}

// Resize the backbuffer when window dimensions change
void RendererGDI::Resize(UINT width, UINT height) {
    if (m_windowWidth == width && m_windowHeight == height) return;

    m_windowWidth = width;
    m_windowHeight = height;
    CreateBackBuffer(width, height);
}

// Create a DIB section from the WIC bitmap source
HRESULT RendererGDI::LoadBitmap(IWICBitmapSource* bitmap, UINT width, UINT height) {
    m_imageWidth = width;
    m_imageHeight = height;

    // Release old DIB
    if (g_app.hDIB) {
        DeleteObject(g_app.hDIB);
        g_app.hDIB = nullptr;
    }

    // Prepare BITMAPINFO for 32-bit DIB
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = (LONG)width;
    bmi.bmiHeader.biHeight = -(LONG)height; // Top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pPixels = nullptr;
    HDC hdcScreen = GetDC(m_hwnd);
    g_app.hDIB = CreateDIBSection(hdcScreen, &bmi, DIB_RGB_COLORS, &pPixels, nullptr, 0);
    ReleaseDC(m_hwnd, hdcScreen);

    // Upload pixel data to GDI
    if (g_app.hDIB && pPixels) {
        bitmap->CopyPixels(nullptr, width * 4, width * 4 * height, static_cast<BYTE*>(pPixels));
    }

    return g_app.hDIB ? S_OK : E_FAIL;
}

// Render the backbuffer to the screen
HRESULT RendererGDI::Render() {
    if (!m_backDC || !m_hwnd) return E_FAIL;

    // 1. Clear background
    RECT rc = { 0, 0, (LONG)m_windowWidth, (LONG)m_windowHeight };
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
        HBITMAP hbmDIBOld = static_cast<HBITMAP>(SelectObject(hdcDIB, g_app.hDIB));

        SetStretchBltMode(m_backDC, HALFTONE);
        StretchBlt(m_backDC, drawX, drawY, renderW, renderH, hdcDIB, 0, 0, m_imageWidth, m_imageHeight, SRCCOPY);

        SelectObject(hdcDIB, hbmDIBOld);
        DeleteDC(hdcDIB);
    }

    // 3. Final copy to screen
    HDC hdc = GetDC(m_hwnd);
    BitBlt(hdc, 0, 0, m_windowWidth, m_windowHeight, m_backDC, 0, 0, SRCCOPY);
    ReleaseDC(m_hwnd, hdc);

    return S_OK;
}

// Allocate the double-buffer surface
HRESULT RendererGDI::CreateBackBuffer(UINT width, UINT height) {
    DestroyBackBuffer();

    HDC hdcScreen = GetDC(m_hwnd);
    m_backDC = CreateCompatibleDC(hdcScreen);
    m_backBitmap = CreateCompatibleBitmap(hdcScreen, width, height);
    m_backBitmapOld = static_cast<HBITMAP>(SelectObject(m_backDC, m_backBitmap));

    ReleaseDC(m_hwnd, hdcScreen);
    return S_OK;
}

// Cleanup GDI objects
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