// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#pragma once
#include "TextOverlay.h"
#include <string>
#include <d2d1_3.h>
#include <dwrite_3.h>
#include <wrl/client.h>

#include "Constants.h"
#include "ConstantsStrings.h"   // Constants::Messages — AnnounceZoomClamp below
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
//   TOP_CENTER [2] : panel selection           (top thumbnail strip)
//   TOP_RIGHT  [3] : "86.0%"                    (current viewport zoom)
//   MID_LEFT   [4] : panel selection           (left thumbnail strip)
//   MID_CENTER [5] : general message queue      (transient — auto-hides)
//   MID_RIGHT  [6] : panel selection           (right thumbnail strip)
//   BOT_LEFT   [7] : stacked effect list        (active effects, grows upward)
//   BOT_CENTER [8] : panel selection           (bottom thumbnail strip)
//   BOT_RIGHT  [9] : "1920×1080 / 4.3 MB"      (dimensions / file size)
//
// Shortcut scheme:
//   Ctrl+1..9   — toggle individual slot ON/OFF
//   Ctrl+0      — master toggle (all slots)
//   I / N       — master toggle (same as Ctrl+0)
//   Ctrl+Shift+1..9 — toggle compact (1-line) vs full (2-line) display per slot
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

        // The persisted masks are one bit per slot, and Constants::Overlay
        // builds its defaults by writing those bit positions out by hand — it
        // cannot see this enum. These assertions are what stops the two drifting:
        // renumber a slot without updating the mask builders and the build
        // fails here rather than silently starting the wrong slots.
        static_assert(TOP_LEFT == 0 && TOP_CENTER == 1 && TOP_RIGHT == 2 &&
                      MID_LEFT == 3 && MID_CENTER == 4 && MID_RIGHT == 5 &&
                      BOT_LEFT == 6 && BOT_CENTER == 7 && BOT_RIGHT == 8,
                      "Slot indices must match the bit positions used by "
                      "Constants::Overlay::DEFAULT_SLOT_*_MASK");
        static_assert(SLOT_COUNT == 9,
                      "Nine slots, nine SLOT_STATE_* constants — add both or neither");

        // ── Public overlays (content set by callers) ──────────────────────────
        TextOverlay slotTopLeft; // [1] index / total + filename
        TextOverlay slotTopCenter; // [2] unused
        TextOverlay slotTopRight; // [3] zoom %
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

        // Repaints TOP_RIGHT's server indicator. Called from the
        // WM_QIV_REMOTE_CLIENTS handler — the listener starting, stopping, or
        // gaining/losing a client.
        //
        // Takes the window because a CHANGE in the client count starts the blink
        // timer, and only a real arrival or departure does: the listener merely
        // starting is not somebody connecting.
        void UpdateRemoteStatus(HWND hWnd,
                                Constants::RemoteTcpIp::ClientEvent event =
                                    Constants::RemoteTcpIp::ClientEvent::Other);

        // One blink phase. Called from WM_TIMER for TIMER_SERVER_BLINK.
        void OnServerBlinkTimer(HWND hWnd);

        // Redraws TOP_RIGHT's text without any blink logic — for changes that
        // are not a connection: a compact toggle, a layout switch, a blink phase.
        void RefreshRemoteIndicator();

        // BOT_RIGHT  — pixel dimensions + file size in bytes
        void UpdateDims(int imgW, int imgH, int64_t fileSizeBytes);

        // BOT_LEFT — rebuild the effects list and the folder-name line. Both
        // are governed by their own AppState toggle and neither affects the
        // other; the folder name is always emitted last so it sits lowest.
        void UpdateEffects();

        // The folder name lives in BOT_LEFT normally but on TOP_LEFT's first
        // line under Summary, so flipping its toggle has to rebuild both.
        void RefreshFolderNameLine();

        // Panel selection overlay — called by ThumbnailPanelWnd whenever m_selectedPaths
        // changes. Maps panel position to the nearest free overlay slot:
        //   1 (top) → TOP_CENTER,  2 (right) → MID_RIGHT,
        //   3 (bot) → BOT_CENTER,  4 (left)  → MID_LEFT,  0 → no-op.
        // Clears the slot when selected == 0 (text made empty → not rendered).
        void UpdatePanelSelectionOverlay(int8_t position, int selected, int total);

        // Colour of a centre-centre message. Normal uses the configured
        // MSG_CENTER_COLOR_*; the other two pull from Constants::Theme::Markers
        // so a problem reported here looks like the same problem reported in the
        // History panel's rows.
        enum class MsgSeverity { Normal, Warning, Error };

        // MID_CENTER — post a transient message; auto-hides after MSG_CENTER_DISPLAY_MS.
        // Pass hWnd so the timer can be set/reset on the main window.
        // Center-center must be enabled (slot visible) for the message to appear.
        void PostCenterMessage(HWND hWnd, const std::wstring &msg,
                               MsgSeverity severity = MsgSeverity::Normal);

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

        // Advance one slot through Compact → Full → Off, the same three states
        // and order the Overlays submenu lists. Ctrl+1..9 uses this, which is
        // why there is no separate compact shortcut.
        void CycleSlotState(Slot slot);

        // "Overlay Top Left: Compact" — the slot's current state, for the
        // centre message. Shared by the keyboard and the menu so both report
        // a state change identically.
        [[nodiscard]] std::wstring SlotStateMessage(Slot slot) const;

        // Re-seed every slot from app.overlaySlotVisibleMask / ...CompactMask
        // and app.overlayLayoutMode, then recompute rects and content. Call
        // after anything that replaces AppState wholesale — Import Settings and
        // Restore Defaults. Does not write back: the caller owns persisting
        // what it just assigned.
        void ApplyPersistedState(HWND hWnd);

        // ── Render ────────────────────────────────────────────────────────────
        void RenderAll(ID2D1DeviceContext *ctx) const;

        // ── Device loss ───────────────────────────────────────────────────────
        void InvalidateLayouts();

        // Called when the D2D device is lost — recreate center-center brush.
        void OnDeviceLost();

        void OnDeviceRestored(ID2D1DeviceContext *ctx);

        void UpdateTextFormat();

        // Re-read app.overlayFontColor into the outer-slot brush. Cheap — no
        // device context needed, so it is safe to call straight from a menu.
        void ApplyTextColor();

    private:
        // ── Resources (not owned, except m_pCenterBrush / m_pBgBrush) ────────
        IDWriteFactory3 *m_pDWriteFactory = nullptr;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> m_pTextFormat; // base format (not owned)
        ID2D1SolidColorBrush *m_pTextBrush = nullptr; // normal brush (not owned)

        // Center-center owns its own brush and format (independent colour + size)
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_pCenterBrush;
        // Backing rect behind the centre message. Separate from m_pCenterBrush
        // so drawing it costs no SetColor — the two differ in alpha.
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_pCenterBgBrush;
        // Severity currently baked into m_pCenterBrush. Mutable because
        // RenderAll is const and this is a cache of what the GPU brush holds,
        // not part of the overlay's logical state.
        mutable MsgSeverity m_centerBrushSeverity = MsgSeverity::Normal;
        mutable bool m_centerBrushSet = false;
        // The eight outer slots draw with this, NOT the brush handed in by
        // RendererD2D — that one is shared with the folder-deleted overlay, so
        // recolouring it would drag unrelated UI along with the user's choice.
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_pOuterBrush;
        // Dedicated semi-transparent background brush — avoids GetColor/SetColor per slot per frame
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_pBgBrush;
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
            bool compact = Constants::Overlay::IS_COMPACT_OVERLAY_MODE; // true → 1-line, false → 2-line
            // Opts a slot out of the shared overlay background rect, whatever
            // app.overlayShowBackground says. BOT_LEFT uses this: its effect
            // list is meant to read as bare text over the image.
            bool drawBackground = true;
            IDWriteTextFormat *fmt = nullptr;
            DWRITE_TEXT_ALIGNMENT cachedAlignment = DWRITE_TEXT_ALIGNMENT_LEADING;
        };

        SlotMeta m_slots[SLOT_COUNT];

        bool m_masterVisible = true;
        float m_rtW = 0.0f;
        float m_rtH = 0.0f;

        // Center-message timer state.
        // The VALUE lives in Constants.h with the rest of the main window's timer
        // map — this is the local name for it, not a second definition of it.
        static constexpr UINT_PTR TIMER_CENTER_MSG = Constants::CENTER_MSG_TIMER_ID;
        bool m_centerMsgActive = false; // true while the auto-hide timer is running
        MsgSeverity m_centerMsgSeverity = MsgSeverity::Normal; // colour of the live message

        // ── Server-dot blink ────────────────────────────────────────────────
        // The indicator blinks three times when a client ARRIVES or LEAVES, so
        // a change is noticeable on a screen nobody is staring at. The count
        // beside it already says what happened; this is what makes anyone look.
        //
        // Value from Constants.h, which holds the whole main-window timer map —
        // the list of who owns which number is there rather than restated here.
        static constexpr UINT_PTR TIMER_SERVER_BLINK = Constants::SERVER_BLINK_TIMER_ID;

        // Phases remaining, counting down to 0 = not blinking. Two phases make
        // one blink, so this starts at OVERLAY_SERVER_BLINK_COUNT * 2 and the
        // even total is what guarantees it ends lit.
        int  m_blinkPhasesLeft = 0;
        bool m_blinkDark       = false;

        // WHAT happened, deciding the lit phase's colour. Only meaningful while
        // m_blinkPhasesLeft > 0. Comes from the socket thread via wParam — the
        // UI thread cannot work it out for itself, see ClientEvent.
        Constants::RemoteTcpIp::ClientEvent m_blinkEvent =
            Constants::RemoteTcpIp::ClientEvent::Other;

        // No client-count baseline is kept. Comparing counts was the first
        // design and it could not survive two clients leaving between repaints,
        // nor say WHY any of them left — the socket thread sends the reason with
        // the message instead. See ClientEvent.

        // ── Helpers ──────────────────────────────────────────────────────────
        // Rebuilds both slot bitmasks from m_slots into AppState and saves them.
        // Every per-slot mutator funnels through here so no change escapes
        // unpersisted.
        void PersistSlotState();

        // The apply half of OnLayoutModeChanged, without the registry write.
        // Used when restoring a stored mode rather than setting a new one.
        void ApplyLayoutMode(HWND hWnd);

        void BuildSlotFormats();

        void BuildCenterBrush(ID2D1DeviceContext *ctx);

        void BuildOuterBrush(ID2D1DeviceContext *ctx);

        void RecomputeRects();

        TextOverlay *OverlayForSlot(Slot slot);

        // "4521472 → 4.3 MB",  "890123 → 869 KB",  "512 → 512 B"
        static std::wstring FormatFileSize(int64_t bytes);

        // Name of the folder holding the current image; empty when none.
        // Cached on the directory portion of the path — see the definition.
        const std::wstring &CurrentFolderName();
        // The composed "📁 <name>" display line, cached on the same event.
        const std::wstring &CurrentFolderLine();
        std::wstring m_folderSrc;  // directory the cached name was derived from
        std::wstring m_folderName; // cached last path component of m_folderSrc
        std::wstring m_folderLine; // cached icon + name, ready to display

        // Last BOT_LEFT height handed to RecomputeRects, so UpdateEffects can
        // skip the recompute when the folder-name toggle has not moved it.
        float m_botLeftRowH = -1.0f;

        // True when the active layout has folded this slot's content into
        // another slot, so drawing it in place would duplicate the value.
        [[nodiscard]] bool IsSlotSuppressed(int slot) const;

        // Re-render the slots whose text shape depends on the compact flag.
        void RebuildForCompactChange(Slot slot);

        // Rebuild TOP_LEFT text honouring the compact flag. In layout mode 2 it
        // also appends the zoom + dimensions/size summary as a second line, so
        // that mode needs no other slot.
        void RebuildTopLeft();

        // TOP_RIGHT's text: the zoom, prefixed with the server dot and
        // active/max client count while the listener is running.
        std::wstring BuildTopRightText() const;

        // Stores the raw data so compact toggle / layout change can re-render
        int m_infoIndex = 0;
        int m_infoTotal = 0;
        std::wstring m_infoFilename;

        // Cached zoom + dims for layout-mode 2 combined line
        float m_zoom = 1.0f;
        int m_imgW = 0;
        int m_imgH = 0;
        int64_t m_fileSizeBytes = 0;

};

extern OverlayManager g_overlayManager;

// Posts the centre-screen "limit reached" notice for a ClampZoomToLimits()
// result. No-op when nothing was capped, so every zoom call site can hand its
// result straight here without a branch of its own.
inline void AnnounceZoomClamp(HWND hWnd, Constants::ZoomClampResult result) {
    if (result == Constants::ZoomClampResult::ClampedMax)
        g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::ZOOM_MAX_REACHED);
    else if (result == Constants::ZoomClampResult::ClampedMin)
        g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::ZOOM_MIN_REACHED);
}
