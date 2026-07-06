#include "OverlayManager.h"
#include <algorithm>

OverlayManager g_overlayManager;

// ─────────────────────────────────────────────────────────────────────────────
//  Init / resize
// ─────────────────────────────────────────────────────────────────────────────

void OverlayManager::Init(IDWriteTextFormat *textFormat, ID2D1SolidColorBrush *textBrush) {
    m_pTextFormat = textFormat;
    m_pTextBrush = textBrush;
}

void OverlayManager::OnResize(float rtW, float rtH) {
    m_rtW = rtW;
    m_rtH = rtH;
    RecomputeRects();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Rect layout — all corner positions defined in one place
// ─────────────────────────────────────────────────────────────────────────────

void OverlayManager::RecomputeRects() {
    // top-left: info text starts near top-left corner
    info.UpdateRect(D2D1::RectF(15.0f, 6.0f, m_rtW - 10.0f, m_rtH - 10.0f));

    // bottom-left: zoom label, anchored from the bottom
    zoom.UpdateRect(D2D1::RectF(15.0f, m_rtH - 30.0f, 300.0f, m_rtH - 6.0f));

    // bottom-right: image dimensions
    dims.UpdateRect(D2D1::RectF(m_rtW - 250.0f, m_rtH - 30.0f, m_rtW - 10.0f, m_rtH - 6.0f));

    // top-right: active effect names
    effects.UpdateRect(D2D1::RectF(m_rtW - 350.0f, 6.0f, m_rtW - 10.0f, 100.0f));
}

// ─────────────────────────────────────────────────────────────────────────────
//  Content updates
// ─────────────────────────────────────────────────────────────────────────────

void OverlayManager::UpdateInfo(int currentIndex, int total, const std::wstring &filename) {
    info.UpdateText(
            std::to_wstring(currentIndex + 1) + L" / " +
            std::to_wstring(total) + L" \u2014 " + filename
            );
}

void OverlayManager::UpdateZoom(float zoomValue) {
    zoom.UpdateText(L"Zoom: " + std::to_wstring(static_cast<int>(zoomValue * 100.0f)) + L"%");
}

void OverlayManager::UpdateDims(int imgW, int imgH) {
    dims.UpdateText(
            std::to_wstring(imgW) + L" \u00D7 " + std::to_wstring(imgH)
            );
}

void OverlayManager::UpdateEffects(const std::wstring &effectNames) {
    effects.UpdateText(effectNames);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Visibility toggles
// ─────────────────────────────────────────────────────────────────────────────

void OverlayManager::SetInfoVisible(bool show) {
    info.active = show;
    show ? AddToActive(&info) : RemoveFromActive(&info);
}

void OverlayManager::SetZoomVisible(bool show) {
    zoom.active = show;
    show ? AddToActive(&zoom) : RemoveFromActive(&zoom);
}

void OverlayManager::SetDimsVisible(bool show) {
    dims.active = show;
    show ? AddToActive(&dims) : RemoveFromActive(&dims);
}

void OverlayManager::SetEffectsVisible(bool show) {
    effects.active = show;
    show ? AddToActive(&effects) : RemoveFromActive(&effects);
}

void OverlayManager::SetAllVisible(bool show) {
    SetInfoVisible(show);
    SetZoomVisible(show);
    SetDimsVisible(show);
    SetEffectsVisible(show);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Render — called once per frame, iterates only active overlays
// ─────────────────────────────────────────────────────────────────────────────

void OverlayManager::RenderAll(ID2D1DeviceContext *ctx) const {
    if (!m_pTextFormat || !m_pTextBrush) return;

    for (TextOverlay *ov: m_active) {
        if (ov->text.empty()) continue;
        ctx->DrawText(
                ov->text.c_str(),
                static_cast<UINT32>(ov->text.length()),
                m_pTextFormat,
                ov->rect,
                m_pTextBrush
                );
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Active list management — called only on toggle events, never per frame
// ─────────────────────────────────────────────────────────────────────────────

void OverlayManager::AddToActive(TextOverlay *ov) {
    for (TextOverlay *existing: m_active) {
        if (existing == ov) return; // already in list
    }
    m_active.push_back(ov);
}

void OverlayManager::RemoveFromActive(TextOverlay *ov) {
    m_active.erase(
            std::remove(m_active.begin(), m_active.end(), ov),
            m_active.end()
            );
}
