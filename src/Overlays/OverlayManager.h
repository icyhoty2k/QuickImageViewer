#pragma once
#include "TextOverlay.h"
#include <vector>
#include <string>
#include <d2d1.h>
#include <dwrite_3.h>

#include "RendererD2D.h"

// =============================================================================
// OverlayManager — 9-slot 3×3 grid overlay system for QIV
//
// Grid layout:
//   TOP_LEFT    TOP_CENTER    TOP_RIGHT
//   MID_LEFT    MID_CENTER    MID_RIGHT
//   BOT_LEFT    BOT_CENTER    BOT_RIGHT
//
// Assigned notifications:
//   TOP_RIGHT  : "42 / 100\nimage1.jpg"    (index / total + filename)
//   TOP_CENTER : "86.0%"                    (current viewport zoom)
//   BOT_RIGHT  : "1920×1080 / 4.3 MB"      (dimensions / file size on disc)
//   BOT_LEFT   : stacked effect list        (active effects, grows upward)
//
// Growth directions:
//   Left  slots: text left-aligned,  rect grows rightward from left edge
//   Right slots: text right-aligned, rect grows leftward  from right edge
//   Center slots: text centered,     rect grows from center outward
//
// Toggle keys (defined in Shortcuts.h):
//   I          : master toggle (all slots on/off)
//   Individual : Ctrl+Alt+1..4 (see Shortcuts.h)
// =============================================================================

class OverlayManager {
    public:
        // ── Slot indices ──────────────────────────────────────────────────────
        enum Slot {
            TOP_LEFT = 0,
            TOP_CENTER = 1,
            TOP_RIGHT = 2,
            MID_LEFT = 3,
            MID_CENTER = 4,
            MID_RIGHT = 5,
            BOT_LEFT = 6,
            BOT_CENTER = 7,
            BOT_RIGHT = 8,
            SLOT_COUNT = 9
        };

        // ── Public overlays (content set by callers) ──────────────────────────
        // TOP_RIGHT  — "42 / 100\nimage1.jpg"
        TextOverlay slotTopRight;
        // TOP_CENTER — "86.0%"
        TextOverlay slotTopCenter;
        // BOT_RIGHT  — "1920×1080 / 4.3 MB"
        TextOverlay slotBotRight;
        // BOT_LEFT   — active effects, one per line, grows bottom→top
        TextOverlay slotBotLeft;

        // Unused slots (available for future use)
        TextOverlay slotTopLeft;
        TextOverlay slotMidLeft;
        TextOverlay slotMidCenter;
        TextOverlay slotMidRight;
        TextOverlay slotBotCenter;

        // ── Lifecycle ─────────────────────────────────────────────────────────
        // Must be called once after the DWrite factory and text resources are ready.
        // Does NOT take ownership — caller (RendererD2D) keeps them alive.
        void Init(IDWriteFactory3 *dwriteFactory,
                  IDWriteTextFormat *textFormat,
                  ID2D1SolidColorBrush *textBrush);

        // Called on every WM_SIZE / Resize. Recomputes all slot rects.
        void OnResize(float rtW, float rtH);

        // ── Content updates ───────────────────────────────────────────────────
        // TOP_RIGHT  — index is 0-based; displays as (index+1) / total
        void UpdateInfo(int index, int total, const std::wstring &filename);

        // TOP_CENTER — raw zoom scalar, e.g. 0.86f → "86.0%"
        void UpdateZoom(float zoom, HWND hWnd);

        // BOT_RIGHT  — pixel dimensions + file size in bytes
        void UpdateDims(int imgW, int imgH, int64_t fileSizeBytes);

        // BOT_LEFT   — rebuild from current AppState effects
        void UpdateEffects();

        // ── Visibility ────────────────────────────────────────────────────────
        void SetSlotVisible(Slot slot, bool show);

        void SetAllVisible(bool show);

        [[nodiscard]] bool IsSlotVisible(Slot slot) const;

        [[nodiscard]] bool AreAllVisible() const {
            return m_masterVisible;
        }

        // ── Render ────────────────────────────────────────────────────────────
        // Called once per frame from RendererD2D::Render().
        // No allocations unless text/rect changed since last call.
        void RenderAll(ID2D1DeviceContext *ctx) const;

        // ── Device loss ───────────────────────────────────────────────────────
        void InvalidateLayouts();

    private:
        // ── Resources (not owned) ────────────────────────────────────────────
        IDWriteFactory3 *m_pDWriteFactory = nullptr;
        IDWriteTextFormat *m_pTextFormat = nullptr;
        ID2D1SolidColorBrush *m_pTextBrush = nullptr;

        // Per-column text formats (created from the base format's parameters)
        Microsoft::WRL::ComPtr<IDWriteTextFormat> m_fmtLeft; // DWRITE_TEXT_ALIGNMENT_LEADING
        Microsoft::WRL::ComPtr<IDWriteTextFormat> m_fmtCenter; // DWRITE_TEXT_ALIGNMENT_CENTER
        Microsoft::WRL::ComPtr<IDWriteTextFormat> m_fmtTrailing; // DWRITE_TEXT_ALIGNMENT_TRAILING

        // --- BOTTOM ONLY ---
        Microsoft::WRL::ComPtr<IDWriteTextFormat> m_fmtBotLeft;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> m_fmtBotCenter;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> m_fmtBotRight;

        // ── Slot metadata ────────────────────────────────────────────────────
        struct SlotMeta {
            TextOverlay *overlay = nullptr;
            bool visible = false;
            IDWriteTextFormat *fmt = nullptr; // points to one of m_fmtLeft/Center/Trailing
        };

        SlotMeta m_slots[SLOT_COUNT];

        bool m_masterVisible = true;
        float m_rtW = 0.0f;
        float m_rtH = 0.0f;

        // ── Helpers ──────────────────────────────────────────────────────────
        void BuildSlotFormats();

        void RecomputeRects();

        TextOverlay *OverlayForSlot(Slot slot);

        // "4521472 → 4.3 MB",  "890123 → 869 KB",  "512 → 512 B"
        static std::wstring FormatFileSize(int64_t bytes);
};

extern OverlayManager g_overlayManager;
