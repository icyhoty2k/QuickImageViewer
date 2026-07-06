#pragma once
#include "TextOverlay.h"
#include <vector>
#include <d2d1.h>
#include <dwrite_3.h>

#include "RendererD2D.h"

class OverlayManager {
    public:
        TextOverlay info; // top-left:     "1 / 42 — filename.jpg"
        TextOverlay zoom; // bottom-left:  "Zoom: 150%"
        TextOverlay dims; // bottom-right: "3840 × 2160"
        TextOverlay effects; // top-right:    active effect names

        void Init(IDWriteTextFormat *textFormat, ID2D1SolidColorBrush *textBrush);

        void OnResize(float rtW, float rtH);

        void UpdateInfo(int currentIndex, int total, const std::wstring &filename);

        void UpdateZoom(float zoom);

        void UpdateDims(int imgW, int imgH);

        void UpdateEffects(const std::wstring &effectNames);

        void SetInfoVisible(bool show);

        void SetZoomVisible(bool show);

        void SetDimsVisible(bool show);

        void SetEffectsVisible(bool show);

        void SetAllVisible(bool show);

        void RenderAll(ID2D1DeviceContext *ctx) const;

    private:
        IDWriteTextFormat *m_pTextFormat = nullptr;
        ID2D1SolidColorBrush *m_pTextBrush = nullptr;
        std::vector<TextOverlay *> m_active;
        float m_rtW = 0.0f;
        float m_rtH = 0.0f;

        void AddToActive(TextOverlay *ov);

        void RemoveFromActive(TextOverlay *ov);

        void RecomputeRects();
};

extern OverlayManager g_overlayManager;
