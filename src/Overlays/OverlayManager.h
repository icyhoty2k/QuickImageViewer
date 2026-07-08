#pragma once
#include "TextOverlay.h"
#include <string>
#include <d2d1_3.h>
#include <dwrite_3.h>
#include <wrl/client.h>

#include "Constants.h"
#include "RendererD2D.h"

// =============================================================================
// OverlayManager — 9-slot 3×3 grid overlay system for QIV
//
// Grid layout  (slot number = Ctrl+N shortcut key):
//   [1] TOP_LEFT    [2] TOP_CENTER    [3] TOP_RIGHT
//   [4] MID_LEFT    [5] MID_CENTER    [6] MID_RIGHT
//   [7] BOT_LEFT    [8] BOT_CENTER    [9] BOT_RIGHT
//
// Current slot assignments:
//   TOP_LEFT   [1] : "42 / 100\nimage1.jpg"    (index / total + filename)
//   TOP_CENTER [2] : "86.0%"                    (current viewport zoom)
//   TOP_RIGHT  [3] : unused
//   MID_LEFT   [4] : unused
//   MID_CENTER [5] : general message queue      (transient — auto-hides)
//   MID_RIGHT  [6] : unused
//   BOT_LEFT   [7] : stacked effect list        (active effects, grows upward)
//   BOT_CENTER [8] : unused
//   BOT_RIGHT  [9] : "1920×1080 / 4.3 MB"      (dimensions / file size)
//
// Shortcut scheme:
//   Ctrl+1..9   — toggle individual slot ON/OFF
//   Ctrl+0      — master toggle (all slots)
//   I / N       — master toggle (same as Ctrl+0)
//   Ctrl+Alt+1..9 — toggle compact (1-line) vs full (2-line) display per slot
//
// Center-center [5] is special:
//   • Has its own font size, color, and brush (independent from other slots).
//   • Receives transient messages via PostCenterMessage().
//   • Auto-hides after Constants::MSG_CENTER_DISPLAY_MS milliseconds.
//   • Compact-mode toggle is a no-op for this slot (always single-line).
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
        TextOverlay slotTopLeft; // [1] index / total + filename
        TextOverlay slotTopCenter; // [2] zoom %
        TextOverlay slotTopRight; // [3] unused
        TextOverlay slotMidLeft; // [4] unused
        TextOverlay slotMidCenter; // [5] general message queue (center-center)
        TextOverlay slotMidRight; // [6] unused
        TextOverlay slotBotLeft; // [7] active effects list
        TextOverlay slotBotCenter; // [8] unused
        TextOverlay slotBotRight; // [9] dimensions / file size

        // ── Lifecycle ─────────────────────────────────────────────────────────
        // Must be called once after the DWrite factory and text resources are ready.
        // Does NOT take ownership — caller (RendererD2D) keeps the base format alive.
        void Init(IDWriteFactory3 *dwriteFactory,
                  IDWriteTextFormat *textFormat,
                  ID2D1SolidColorBrush *textBrush,
                  ID2D1DeviceContext *ctx);

        // Called on every WM_SIZE / Resize. Recomputes all slot rects.
        void OnResize(float rtW, float rtH);

        // Called when O key cycles OVERLAY_LAYOUT_MODE — recomputes rects and
        // rebuilds slot content for the new mode.
        void OnLayoutModeChanged(HWND hWnd);

        // ── Content updates ───────────────────────────────────────────────────
        // TOP_LEFT  — index is 0-based; displays as (index+1) / total
        void UpdateInfo(int index, int total, const std::wstring &filename);

        // TOP_CENTER — raw zoom scalar, e.g. 0.86f → "86.0%"
        void UpdateZoom(float zoom, HWND hWnd);

        // BOT_RIGHT  — pixel dimensions + file size in bytes
        void UpdateDims(int imgW, int imgH, int64_t fileSizeBytes);

        // BOT_LEFT   — rebuild from current AppState effects
        void UpdateEffects();

        // MID_CENTER — post a transient message; auto-hides after MSG_CENTER_DISPLAY_MS.
        // Pass hWnd so the timer can be set/reset on the main window.
        // Center-center must be enabled (slot visible) for the message to appear.
        void PostCenterMessage(HWND hWnd, const std::wstring &msg);

        // Called from WM_TIMER in AppMain — hides MID_CENTER after timeout.
        void OnCenterMessageTimer(HWND hWnd);

        // ── Visibility ────────────────────────────────────────────────────────
        void SetSlotVisible(Slot slot, bool show);

        void SetAllVisible(bool show);

        [[nodiscard]] bool IsSlotVisible(Slot slot) const;

        [[nodiscard]] bool AreAllVisible() const {
            return m_masterVisible;
        }

        // ── Compact mode ──────────────────────────────────────────────────────
        // Toggles 1-line (compact) vs 2-line (full) rendering for a slot.
        // MID_CENTER ignores this — it is always displayed on one line.
        void ToggleCompactMode(Slot slot);

        [[nodiscard]] bool IsCompact(Slot slot) const;

        // ── Render ────────────────────────────────────────────────────────────
        void RenderAll(ID2D1DeviceContext *ctx) const;

        // ── Device loss ───────────────────────────────────────────────────────
        void InvalidateLayouts();

        // Called when the D2D device is lost — recreate center-center brush.
        void OnDeviceLost();

        void OnDeviceRestored(ID2D1DeviceContext *ctx);

    private:
        // ── Resources (not owned, except m_pCenterBrush) ─────────────────────
        IDWriteFactory3 *m_pDWriteFactory = nullptr;
        IDWriteTextFormat *m_pTextFormat = nullptr; // base format (not owned)
        ID2D1SolidColorBrush *m_pTextBrush = nullptr; // normal brush (not owned)

        // Center-center owns its own brush and format (independent colour + size)
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_pCenterBrush;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> m_fmtCenter5; // center-center format

        // Per-column text formats (derived from base format)
        Microsoft::WRL::ComPtr<IDWriteTextFormat> m_fmtLeft; // LEADING
        Microsoft::WRL::ComPtr<IDWriteTextFormat> m_fmtCenter; // CENTER
        Microsoft::WRL::ComPtr<IDWriteTextFormat> m_fmtTrailing; // TRAILING
        // Bottom-row variants (paragraph-aligned FAR so text anchors to bottom)
        Microsoft::WRL::ComPtr<IDWriteTextFormat> m_fmtBotLeft;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> m_fmtBotCenter;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> m_fmtBotRight;

        // ── Slot metadata ────────────────────────────────────────────────────
        struct SlotMeta {
            TextOverlay *overlay = nullptr;
            bool visible = false;
            bool compact = Constants::Overlay::COMPACT_OVERLAY_MODE; // true → 1-line, false → 2-line
            IDWriteTextFormat *fmt = nullptr;
        };

        SlotMeta m_slots[SLOT_COUNT];

        bool m_masterVisible = true;
        float m_rtW = 0.0f;
        float m_rtH = 0.0f;

        // Center-message timer state
        static constexpr UINT_PTR TIMER_CENTER_MSG = 1002;
        bool m_centerMsgActive = false; // true while the auto-hide timer is running

        // ── Helpers ──────────────────────────────────────────────────────────
        void BuildSlotFormats();

        void BuildCenterBrush(ID2D1DeviceContext *ctx);

        void RecomputeRects();

        TextOverlay *OverlayForSlot(Slot slot);

        // "4521472 → 4.3 MB",  "890123 → 869 KB",  "512 → 512 B"
        static std::wstring FormatFileSize(int64_t bytes);

        // Rebuild TOP_LEFT text honouring compact flag
        void RebuildTopLeft();

        // Stores the raw data so compact toggle / layout change can re-render
        int m_infoIndex = 0;
        int m_infoTotal = 0;
        std::wstring m_infoFilename;

        // Cached zoom + dims for layout-mode 2 combined line
        float m_zoom = 1.0f;
        int   m_imgW = 0;
        int   m_imgH = 0;
        int64_t m_fileSizeBytes = 0;

        // Rebuild the combined zoom+dims line used in layout mode 2
        void RebuildSummaryLine2();
};

extern OverlayManager g_overlayManager;
