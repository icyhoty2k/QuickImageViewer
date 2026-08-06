// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#include "OverlayManager.h"
#include "../AppState.h"
#include "../Rem_TCP_IP/RemoteServer.h"   // IsRunning / ActiveConnections
#include "../Rem_TCP_IP/RemoteSettings.h" // Config().maxConnections
#include "../Common/Converters.h"
#include "../Persistence/RegistryManager.h"
#include "../Platform/Constants.h"
#include "../Platform/ConstantsStrings.h"
#include <algorithm>
#include <wrl/client.h>

OverlayManager g_overlayManager;

// ─────────────────────────────────────────────────────────────────────────────
//  Layout constants
// ─────────────────────────────────────────────────────────────────────────────

static constexpr float MARGIN = 12.0f;
static constexpr int EFFECT_MAX_LINES = 8;

static constexpr float COL_LEFT_WIDTH = 360.0f;
static constexpr float COL_RIGHT_WIDTH = 280.0f;
static constexpr float COL_CENTER_WIDTH = 200.0f;

static constexpr float ROW_SINGLE = 24.0f;
static constexpr float ROW_DOUBLE = 44.0f;
static constexpr float EFFECT_LINE_HEIGHT = 28.0f;
static constexpr float ROW_EFFECTS = static_cast<float>(EFFECT_MAX_LINES) * EFFECT_LINE_HEIGHT;

// BOT_LEFT height. The folder name is an extra line alongside the effect list,
// so the slot needs one more row when it is shown — otherwise a full 8-effect
// stack would push the topmost entry outside the rect and get clipped.
// Summary puts the name on TOP_LEFT's first line instead, so it costs nothing
// here in that mode.
static float BotLeftRowHeight() {
    const bool nameInSlot = app.overlayShowDirName && app.overlayLayoutMode != 2;
    return ROW_EFFECTS + (nameInSlot ? EFFECT_LINE_HEIGHT : 0.0f);
}

static constexpr float BG_ALPHA = 0.45f;
// The centre message sits over the middle of the picture, so its plate is
// darker than the corner slots' to keep a one-line notice readable.
static constexpr float MSG_CENTER_BG_ALPHA = 0.60f;
static constexpr float BG_PADDING = 3.0f;

// ─────────────────────────────────────────────────────────────────────────────
//  Init
// ─────────────────────────────────────────────────────────────────────────────

void OverlayManager::Init(IDWriteFactory3 *dwriteFactory,
                          ID2D1SolidColorBrush *textBrush,
                          ID2D1DeviceContext *ctx) {
    m_pDWriteFactory = dwriteFactory;

    m_pTextBrush = textBrush;

    // Visibility and compact state come from AppState, which LoadAllSettings
    // has already filled from the registry (or the dedicated .ini). Hardcoding
    // them here would silently override whatever the user last chose.
    auto wire = [&](Slot s, TextOverlay *ov) {
        m_slots[s].overlay = ov;
        m_slots[s].visible = (app.overlaySlotVisibleMask >> s) & 1u;
        m_slots[s].compact = (app.overlaySlotCompactMask >> s) & 1u;
    };

    wire(TOP_LEFT, &slotTopLeft);     // index / total + filename
    wire(TOP_CENTER, &slotTopCenter); // panel-selection overlay (top panel)
    wire(TOP_RIGHT, &slotTopRight);   // zoom
    wire(MID_LEFT, &slotMidLeft);     // panel-selection overlay (left panel)
    wire(MID_CENTER, &slotMidCenter); // center-center message queue
    wire(MID_RIGHT, &slotMidRight);   // panel-selection overlay (right panel)
    wire(BOT_LEFT, &slotBotLeft);     // effects
    wire(BOT_CENTER, &slotBotCenter); // panel-selection overlay (bottom panel)
    wire(BOT_RIGHT, &slotBotRight);   // dims / size

    // The effect list reads as bare text over the image — no background rect,
    // regardless of the global Overlay Background setting.
    m_slots[BOT_LEFT].drawBackground = false;

    // MID_CENTER is never shown until a message is posted
    slotMidCenter.active = false;

    BuildSlotFormats();
    BuildCenterBrush(ctx);
    BuildOuterBrush(ctx);
    if (ctx) ctx->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, BG_ALPHA), &m_pBgBrush);

    // A saved Stacked/Summary mode needs more than rects: the formats and the
    // per-slot active flags only get set here. Without this a restart in mode 1
    // or 2 would lay the slots out correctly but keep the grid's alignments.
    // Rects are recomputed again by the OnResize that follows Init.
    ApplyLayoutMode(nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Init text Size and brush color
// ─────────────────────────────────────────────────────────────────────────────
void OverlayManager::UpdateTextFormat() {
    if (!m_pDWriteFactory) return;

    // -------------------------------------------------------------------------
    // 1. Create Base Font (for the 8 outer slots)
    // -------------------------------------------------------------------------
    // Size and family are the user's, from the Overlays ▸ Font items. The index
    // was clamped on load, so it is safe to subscript. A missing family falls
    // through to MSG_ALL_FONT_FAMILY_FALLBACK below, same as before.
    float scaledFontSize = static_cast<float>(app.overlayFontSize) * app.dpiScale;
    const wchar_t *family =
            Constants::Overlay::OVERLAY_FONT_FAMILIES[app.overlayFontFamily];

    // Reset(), NOT a manual Release() followed by clearing the pointer.
    //
    // m_pTextFormat is a ComPtr and already owns its reference, so releasing by
    // hand dropped the count to zero and destroyed the object — and the
    // assignment on the next line then released the freed one a second time.
    // The crash landed in ComPtr::InternalRelease, several frames from anything
    // that looked related, which is why this survived so long: the only caller
    // that reaches it twice in one session is a settings import.
    //
    // Matches how m_fmtCenter5 is cleared a few lines below.
    m_pTextFormat.Reset();

    HRESULT hr = m_pDWriteFactory->CreateTextFormat(
            family, nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            scaledFontSize, Constants::Overlay::MSG_ALL_FONT_LOCALE, &m_pTextFormat);

    if (FAILED(hr)) {
        (void) m_pDWriteFactory->CreateTextFormat(
                Constants::Overlay::MSG_ALL_FONT_FAMILY_FALLBACK, nullptr,
                DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                scaledFontSize, Constants::Overlay::MSG_ALL_FONT_LOCALE, &m_pTextFormat);
    }

    // -------------------------------------------------------------------------
    // 2. Create Center Message Font (Totally Independent)
    // -------------------------------------------------------------------------
    float centerSize = Constants::Overlay::MSG_CENTER_FONT_SIZE * app.dpiScale;
    m_fmtCenter5.Reset();

    // You can now independently change "Segoe UI" to any other font family,
    // or change the weight (e.g., DWRITE_FONT_WEIGHT_BOLD) for just the center text.
    HRESULT hrCenter = m_pDWriteFactory->CreateTextFormat(
            Constants::Overlay::MSG_CENTER__FONT_FAMILY_DEFAULT, nullptr,
            DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            centerSize, Constants::Overlay::MSG_ALL_FONT_LOCALE, &m_fmtCenter5);
    if (FAILED(hrCenter)) {
        (void) m_pDWriteFactory->CreateTextFormat(
                Constants::Overlay::MSG_ALL_FONT_FAMILY_FALLBACK, nullptr,
                DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                scaledFontSize, Constants::Overlay::MSG_ALL_FONT_LOCALE, &m_fmtCenter5);
    }
    if (SUCCEEDED(hrCenter)) {
        // Set the required center alignments directly on this specific format
        m_fmtCenter5->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        m_fmtCenter5->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        m_fmtCenter5->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }

    // -------------------------------------------------------------------------
    // 3. Rebuild the outer slot layouts.
    // -------------------------------------------------------------------------
    BuildSlotFormats();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Build center-center brush (independent colour).
// ─────────────────────────────────────────────────────────────────────────────

// Stand-in for "no folder line", so the reference binding in UpdateEffects has
// something with static lifetime to point at when the line is switched off.
static const std::wstring kNoLine;

// Colour for the eight outer slots, from app.overlayFontColor. COLORREF is
// 0x00BBGGRR, so the channels are pulled out with the GetXValue macros rather
// than shifted by hand.
static D2D1_COLOR_F OuterTextColor() {
    const COLORREF c = app.overlayFontColor;
    return D2D1::ColorF(GetRValue(c) / 255.0f,
                        GetGValue(c) / 255.0f,
                        GetBValue(c) / 255.0f,
                        1.0f);
}

void OverlayManager::BuildOuterBrush(ID2D1DeviceContext *ctx) {
    if (!ctx) return;
    m_pOuterBrush.Reset();
    ctx->CreateSolidColorBrush(OuterTextColor(), &m_pOuterBrush);
}

void OverlayManager::ApplyTextColor() {
    if (m_pOuterBrush) m_pOuterBrush->SetColor(OuterTextColor());
}

void OverlayManager::BuildCenterBrush(ID2D1DeviceContext *ctx) {
    if (!ctx) return;
    m_pCenterBrush.Reset();
    D2D1_COLOR_F color = D2D1::ColorF(
            Constants::Overlay::MSG_CENTER_COLOR_R,
            Constants::Overlay::MSG_CENTER_COLOR_G,
            Constants::Overlay::MSG_CENTER_COLOR_B,
            Constants::Overlay::MSG_CENTER_COLOR_A);
    ctx->CreateSolidColorBrush(color, &m_pCenterBrush);
    // A fresh brush holds the colour above, not whatever severity was last
    // applied — force RenderAll to re-apply on the next frame.
    m_centerBrushSet = false;

    m_pCenterBgBrush.Reset();
    ctx->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, MSG_CENTER_BG_ALPHA),
                               &m_pCenterBgBrush);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Build per-column IDWriteTextFormat objects
// ─────────────────────────────────────────────────────────────────────────────

void OverlayManager::BuildSlotFormats() {
    if (!m_pDWriteFactory || !m_pTextFormat) return;
    // --- Read parameters from base format ---
    const UINT32 famLen = m_pTextFormat->GetFontFamilyNameLength() + 1;
    std::wstring famName(famLen, L'\0');
    m_pTextFormat->GetFontFamilyName(&famName[0], famLen);
    const float fontSize = m_pTextFormat->GetFontSize();
    const auto weight = m_pTextFormat->GetFontWeight();
    const auto style = m_pTextFormat->GetFontStyle();
    const auto stretch = m_pTextFormat->GetFontStretch();

    const UINT32 locLen = m_pTextFormat->GetLocaleNameLength() + 1;
    std::wstring locale(locLen, L'\0');
    m_pTextFormat->GetLocaleName(&locale[0], locLen);

    auto makeFormat = [&](IDWriteTextFormat **ppOut,
                          DWRITE_TEXT_ALIGNMENT textAlign,
                          DWRITE_PARAGRAPH_ALIGNMENT paraAlign,
                          float size = 0.0f) {
        Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt;
        HRESULT hr = m_pDWriteFactory->CreateTextFormat(
                famName.c_str(), nullptr,
                weight, style, stretch,
                size > 0.0f ? size : fontSize,
                locale.c_str(), &fmt);
        if (SUCCEEDED(hr)) {
            fmt->SetTextAlignment(textAlign);
            fmt->SetParagraphAlignment(paraAlign);
            fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            *ppOut = fmt.Detach();
        }
    };
    IDWriteTextFormat *raw = nullptr;
    // Normal rows (top/mid — paragraph NEAR)
    makeFormat(&raw, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    m_fmtLeft.Attach(raw);
    raw = nullptr;
    makeFormat(&raw, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    m_fmtCenter.Attach(raw);
    raw = nullptr;
    makeFormat(&raw, DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    m_fmtTrailing.Attach(raw);
    raw = nullptr;

    // Bottom rows (paragraph FAR — text anchors to bottom of rect)
    makeFormat(&raw, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_FAR);
    m_fmtBotLeft.Attach(raw);
    raw = nullptr;
    makeFormat(&raw, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_FAR);
    m_fmtBotCenter.Attach(raw);
    raw = nullptr;
    makeFormat(&raw, DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_FAR);
    m_fmtBotRight.Attach(raw);
    raw = nullptr;

    // --- Assign to slots (fmt + cachedAlignment kept in sync) ---
    m_slots[TOP_LEFT].fmt = m_fmtLeft.Get();        m_slots[TOP_LEFT].cachedAlignment = DWRITE_TEXT_ALIGNMENT_LEADING;
    m_slots[TOP_CENTER].fmt = m_fmtCenter.Get();    m_slots[TOP_CENTER].cachedAlignment = DWRITE_TEXT_ALIGNMENT_CENTER;
    m_slots[TOP_RIGHT].fmt = m_fmtTrailing.Get();   m_slots[TOP_RIGHT].cachedAlignment = DWRITE_TEXT_ALIGNMENT_TRAILING;
    m_slots[MID_LEFT].fmt = m_fmtLeft.Get();        m_slots[MID_LEFT].cachedAlignment = DWRITE_TEXT_ALIGNMENT_LEADING;
    m_slots[MID_CENTER].fmt = m_fmtCenter5.Get();   m_slots[MID_CENTER].cachedAlignment = DWRITE_TEXT_ALIGNMENT_CENTER;
    m_slots[MID_RIGHT].fmt = m_fmtTrailing.Get();   m_slots[MID_RIGHT].cachedAlignment = DWRITE_TEXT_ALIGNMENT_TRAILING;
    m_slots[BOT_LEFT].fmt = m_fmtBotLeft.Get();     m_slots[BOT_LEFT].cachedAlignment = DWRITE_TEXT_ALIGNMENT_LEADING;
    m_slots[BOT_CENTER].fmt = m_fmtBotCenter.Get(); m_slots[BOT_CENTER].cachedAlignment = DWRITE_TEXT_ALIGNMENT_CENTER;
    m_slots[BOT_RIGHT].fmt = m_fmtBotRight.Get();   m_slots[BOT_RIGHT].cachedAlignment = DWRITE_TEXT_ALIGNMENT_TRAILING;

    InvalidateLayouts();
}


// ─────────────────────────────────────────────────────────────────────────────
//  Resize
// ─────────────────────────────────────────────────────────────────────────────

void OverlayManager::OnResize(float rtW, float rtH) {
    m_rtW = rtW;
    m_rtH = rtH;
    RecomputeRects();
}

void OverlayManager::RecomputeRects() {
    const float W = m_rtW;
    const float H = m_rtH;
    const float M = MARGIN;

    // Only the four CORNER slots are ever re-arranged by a layout mode. The
    // other five — TOP_CENTER, MID_LEFT, MID_CENTER, MID_RIGHT, BOT_CENTER —
    // carry the per-panel selection counts and the centre message queue, whose
    // whole meaning is which edge of the screen they sit on. Moving those would
    // break the association with the panel they describe, so every mode places
    // them identically, right here.
    auto placeNonCorners = [&]() {
        // [2] TOP_CENTER — top panel selection, always single line
        slotTopCenter.UpdateRect(D2D1::RectF(
                (W - COL_CENTER_WIDTH) * 0.5f, M,
                (W + COL_CENTER_WIDTH) * 0.5f, M + ROW_SINGLE));

        // [4] MID_LEFT — left panel selection
        {
            float rowH = m_slots[MID_LEFT].compact ? ROW_SINGLE : ROW_DOUBLE;
            slotMidLeft.UpdateRect(D2D1::RectF(
                    M, (H - rowH) * 0.5f,
                    M + COL_LEFT_WIDTH, (H + rowH) * 0.5f));
        }

        // [5] MID_CENTER — message queue, always screen-centred
        slotMidCenter.UpdateRect(D2D1::RectF(
                (W - Constants::Overlay::MSG_CENTER_WIDTH) * 0.5f,
                (H - Constants::Overlay::MSG_CENTER_HEIGHT) * 0.5f,
                (W + Constants::Overlay::MSG_CENTER_WIDTH) * 0.5f,
                (H + Constants::Overlay::MSG_CENTER_HEIGHT) * 0.5f));

        // [6] MID_RIGHT — right panel selection
        {
            float rowH = m_slots[MID_RIGHT].compact ? ROW_SINGLE : ROW_DOUBLE;
            slotMidRight.UpdateRect(D2D1::RectF(
                    W - COL_RIGHT_WIDTH - M, (H - rowH) * 0.5f,
                    W - M, (H + rowH) * 0.5f));
        }

        // [8] BOT_CENTER — bottom panel selection
        {
            float rowH = m_slots[BOT_CENTER].compact ? ROW_SINGLE : ROW_DOUBLE;
            slotBotCenter.UpdateRect(D2D1::RectF(
                    (W - COL_CENTER_WIDTH) * 0.5f, H - M - rowH,
                    (W + COL_CENTER_WIDTH) * 0.5f, H - M));
        }
    };

    // ── Mode 1: the four corners stacked vertically on the top-left ──────────
    if (app.overlayLayoutMode == 1) {
        float cursorY = M;
        // Corners only, effects last because it is by far the tallest row.
        struct StackEntry {
            Slot slot;
            TextOverlay *ov;
        };
        const StackEntry entries[] = {
            {TOP_LEFT, &slotTopLeft},
            {TOP_RIGHT, &slotTopRight},
            {BOT_RIGHT, &slotBotRight},
            {BOT_LEFT, &slotBotLeft}, // effects last
        };
        for (const auto &e: entries) {
            float rowH = (e.slot == BOT_LEFT)
                             ? BotLeftRowHeight()
                             : m_slots[e.slot].compact
                                   ? ROW_SINGLE
                                   : ROW_DOUBLE;
            e.ov->UpdateRect(D2D1::RectF(M, cursorY, M + COL_LEFT_WIDTH, cursorY + rowH));
            // Force leading alignment so all stacked text is left-aligned.
            // cachedAlignment must move with fmt — RenderAll sizes the
            // background rect from it, so leaving it TRAILING here would draw
            // the box off to the right of text that is now left-aligned.
            // BOT_LEFT gets m_fmtLeft too, not its usual bottom-anchored
            // format: in a top-down column it has to start at the top of its
            // row and grow downward, or its block would sink to the bottom of
            // the tall effects row and leave a gap under line 3.
            m_slots[e.slot].fmt = m_fmtLeft.Get();
            m_slots[e.slot].cachedAlignment = DWRITE_TEXT_ALIGNMENT_LEADING;
            cursorY += rowH + M;
        }
        placeNonCorners();
        return;
    }

    // ── Modes 0 and 2 — corners in their own quarters ────────────────────────
    // Summary (2) differs from Grid (0) in only two ways: TOP_LEFT is always
    // the 2-line block, and TOP_RIGHT / BOT_RIGHT are folded into it. Those two
    // are dropped at draw time by IsSlotSuppressed rather than being given zero
    // rects — UpdateZoom and UpdateDims keep refilling their text on every
    // navigation, so any rect- or text-based trick here is undone moments
    // later. Leaving the rects valid also means nothing to restore on the way
    // back to Grid. BOT_LEFT keeps its own corner in both modes: effects stay
    // bottom-left in Summary.
    const bool summary = app.overlayLayoutMode == 2;

    // [1] TOP_LEFT — index/total + filename (1 or 2 lines), or the summary block
    {
        float rowH = (summary || !m_slots[TOP_LEFT].compact) ? ROW_DOUBLE : ROW_SINGLE;
        slotTopLeft.UpdateRect(D2D1::RectF(M, M, M + COL_LEFT_WIDTH, M + rowH));
    }

    // [3] TOP_RIGHT — zoom %
    {
        float rowH = m_slots[TOP_RIGHT].compact ? ROW_SINGLE : ROW_DOUBLE;
        slotTopRight.UpdateRect(D2D1::RectF(
                W - COL_RIGHT_WIDTH - M, M,
                W - M, M + rowH));
    }

    // [7] BOT_LEFT — effect list + folder name, anchored to bottom, grows upward
    {
        const float rowH = BotLeftRowHeight();
        slotBotLeft.UpdateRect(D2D1::RectF(
                M, H - M - rowH,
                M + COL_LEFT_WIDTH, H - M));
    }

    // [9] BOT_RIGHT — dimensions / file size
    {
        float rowH = m_slots[BOT_RIGHT].compact ? ROW_SINGLE : ROW_DOUBLE;
        slotBotRight.UpdateRect(D2D1::RectF(
                W - COL_RIGHT_WIDTH - M, H - M - rowH,
                W - M, H - M));
    }

    placeNonCorners();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Content updates
// ─────────────────────────────────────────────────────────────────────────────

void OverlayManager::RebuildTopLeft() {
    wchar_t buf[32];

    // ── Summary: folder name alone on line 1, everything else on line 2 ──────
    // "Everything else" is this slot's own index/filename plus the two corners
    // folded in here, TOP_RIGHT (zoom) and BOT_RIGHT (dimensions / size).
    // Effects are NOT folded in — they stay in BOT_LEFT.
    if (app.overlayLayoutMode == 2) {
        std::wstring text;
        if (app.overlayShowDirName) {
            const std::wstring &dirLine = CurrentFolderLine();
            if (!dirLine.empty()) {
                text = dirLine;
                text += L'\n';
            }
        }
        swprintf_s(buf, L"%d / %d  ", m_infoIndex + 1, m_infoTotal);
        text += buf;
        text += m_infoFilename;
        text += L"  ";
        text += BuildTopRightText();
        text += L"  ";
        wchar_t dimBuf[32];
        swprintf_s(dimBuf, L"%d×%d", m_imgW, m_imgH);
        text += dimBuf;
        text += L" / ";
        text += FormatFileSize(m_fileSizeBytes);
        slotTopLeft.UpdateText(std::move(text));
        return;
    }

    // ── Grid / Stacked: index + filename, on one or two lines ────────────────
    if (m_slots[TOP_LEFT].compact) {
        // 1-line: "42 / 100  image1.jpg"
        swprintf_s(buf, L"%d / %d  ", m_infoIndex + 1, m_infoTotal);
    } else {
        // 2-line: "42 / 100\nimage1.jpg"
        swprintf_s(buf, L"%d / %d\n", m_infoIndex + 1, m_infoTotal);
    }
    std::wstring text = buf;
    text += m_infoFilename;
    slotTopLeft.UpdateText(std::move(text));
}

void OverlayManager::OnLayoutModeChanged(HWND hWnd) {
    // Single choke point for every *user* layout change — the O key and the
    // Overlays submenu both land here, so persisting once here covers both.
    // Init and ApplyPersistedState call ApplyLayoutMode directly instead: they
    // are restoring a stored value, not producing a new one to store.
    Persistence::Registry::SaveSetting(Constants::Registry::OVERLAY_LAYOUT_MODE,
                                       static_cast<DWORD>(app.overlayLayoutMode));
    ApplyLayoutMode(hWnd);
}

void OverlayManager::ApplyLayoutMode(HWND /*hWnd*/) {
    RecomputeRects();

    // ── Formats ──────────────────────────────────────────────────────────────
    // Every mode starts from the grid assignment. RecomputeRects ran above and
    // may already have applied the stacked overrides, so those are re-applied
    // after this reset — otherwise the reset would silently undo them until the
    // next resize happened to call RecomputeRects again.
    m_slots[TOP_LEFT].fmt = m_fmtLeft.Get();        m_slots[TOP_LEFT].cachedAlignment = DWRITE_TEXT_ALIGNMENT_LEADING;
    m_slots[TOP_CENTER].fmt = m_fmtCenter.Get();    m_slots[TOP_CENTER].cachedAlignment = DWRITE_TEXT_ALIGNMENT_CENTER;
    m_slots[TOP_RIGHT].fmt = m_fmtTrailing.Get();   m_slots[TOP_RIGHT].cachedAlignment = DWRITE_TEXT_ALIGNMENT_TRAILING;
    m_slots[MID_LEFT].fmt = m_fmtLeft.Get();        m_slots[MID_LEFT].cachedAlignment = DWRITE_TEXT_ALIGNMENT_LEADING;
    m_slots[MID_CENTER].fmt = m_fmtCenter5.Get();   m_slots[MID_CENTER].cachedAlignment = DWRITE_TEXT_ALIGNMENT_CENTER;
    m_slots[MID_RIGHT].fmt = m_fmtTrailing.Get();   m_slots[MID_RIGHT].cachedAlignment = DWRITE_TEXT_ALIGNMENT_TRAILING;
    m_slots[BOT_LEFT].fmt = m_fmtBotLeft.Get();     m_slots[BOT_LEFT].cachedAlignment = DWRITE_TEXT_ALIGNMENT_LEADING;
    m_slots[BOT_CENTER].fmt = m_fmtBotCenter.Get(); m_slots[BOT_CENTER].cachedAlignment = DWRITE_TEXT_ALIGNMENT_CENTER;
    m_slots[BOT_RIGHT].fmt = m_fmtBotRight.Get();   m_slots[BOT_RIGHT].cachedAlignment = DWRITE_TEXT_ALIGNMENT_TRAILING;
    if (app.overlayLayoutMode == 1) {
        for (Slot s: {TOP_LEFT, TOP_RIGHT, BOT_LEFT, BOT_RIGHT}) {
            m_slots[s].fmt = m_fmtLeft.Get(); // top-anchored — see RecomputeRects
            m_slots[s].cachedAlignment = DWRITE_TEXT_ALIGNMENT_LEADING;
        }
    }

    // ── Active flags ─────────────────────────────────────────────────────────
    for (int i = 0; i < SLOT_COUNT; ++i) {
        if (i == MID_CENTER) continue;
        m_slots[i].overlay->active = m_masterVisible && m_slots[i].visible;
    }
    if (app.overlayLayoutMode == 2) {
        // Folded into TOP_LEFT's second line. IsSlotSuppressed is what actually
        // keeps them off screen — this only keeps `active` honest.
        slotTopRight.active = false;
        slotBotRight.active = false;
    }

    // ── Content ──────────────────────────────────────────────────────────────
    // All three depend on the mode: TOP_LEFT switches between the summary block
    // and the plain index/filename, and BOT_LEFT moves the folder-name line.
    RebuildTopLeft();
    slotTopRight.UpdateText(BuildTopRightText());
    UpdateEffects();                                // BOT_LEFT
    UpdateDims(m_imgW, m_imgH, m_fileSizeBytes);    // BOT_RIGHT
}

// TOP_RIGHT is "<dot> <active>/<max>  <zoom>" while the listener runs, and just
// the zoom otherwise — the indicator's ABSENCE is the stopped state, so there is
// nothing to grey out and no second visual language to learn.
//
// The figures are read live rather than cached: this is called on a zoom change
// or a connect/disconnect, never per frame, so two atomic loads cost nothing.
std::wstring OverlayManager::BuildTopRightText() const {
    const std::wstring zoom = Converters::FormatZoomPercent(m_zoom);
    if (!Remote::IsRunning()) return zoom;

    // THREE STATES, in order of precedence.
    //
    // While blinking, the dot alternates dark and an EVENT colour — green for an
    // arrival, red for a departure — so the direction is readable from across a
    // room without going to read the count.
    //
    // Once the blink ends it falls back to the steady meaning: green encrypted,
    // orange plaintext loopback. Every glyph is the same emoji class, so the
    // count and zoom beside it never shift — see OVERLAY_SERVER_DOT_OFF.
    std::wstring dot;
    if (m_blinkPhasesLeft > 0) {
        using CE = Constants::RemoteTcpIp::ClientEvent;
        const wchar_t *lit = Constants::Messages::OVERLAY_SERVER_DOT_JOIN;
        switch (m_blinkEvent) {
            case CE::Joined:     lit = Constants::Messages::OVERLAY_SERVER_DOT_JOIN;   break;
            case CE::LeftClean:  lit = Constants::Messages::OVERLAY_SERVER_DOT_LEFT;   break;
            case CE::LeftAbrupt: lit = Constants::Messages::OVERLAY_SERVER_DOT_LOST;   break;
            case CE::Ejected:    lit = Constants::Messages::OVERLAY_SERVER_DOT_KICKED; break;
            default:             break;   // Other never starts a blink
        }
        dot = m_blinkDark ? Constants::Messages::OVERLAY_SERVER_DOT_OFF : lit;
    } else {
        dot = Remote::IsEncrypted()
                  ? Constants::Messages::OVERLAY_SERVER_DOT_TLS
                  : Constants::Messages::OVERLAY_SERVER_DOT_PLAIN;
    }

    // COMPACT shows the dot alone — the slot's compact state means "the least
    // that still carries the information", and for a server that is "it is up,
    // and this is how it is exposed". FULL adds the client count.
    if (m_slots[TOP_RIGHT].compact) return dot + L"  " + zoom;

    return dot + L" " + std::to_wstring(Remote::ActiveConnections()) + L"/" +
           std::to_wstring(Remote::Config().maxConnections) + L"  " + zoom;
}

// The text half, with no opinion about blinking. Called by anything that needs
// TOP_RIGHT redrawn for a reason that is NOT somebody connecting — a compact
// toggle, a layout change, a blink phase.
void OverlayManager::RefreshRemoteIndicator() {
    slotTopRight.UpdateText(BuildTopRightText());
    // Summary mode folds TOP_RIGHT into TOP_LEFT's second line.
    if (app.overlayLayoutMode == 2)
        RebuildTopLeft();
}

void OverlayManager::UpdateRemoteStatus(HWND hWnd,
                                        Constants::RemoteTcpIp::ClientEvent event) {
    using CE = Constants::RemoteTcpIp::ClientEvent;

    // ONLY A REAL ARRIVAL OR DEPARTURE BLINKS. `Other` is the listener starting
    // or stopping, and a viewer that blinked at its own startup would do it on
    // every launch with autostart on.
    //
    // Driven by the EVENT rather than by comparing counts: two clients leaving
    // between repaints is still two departures, and only the socket thread knows
    // whether each was a goodbye, a crash or an eject.
    if (event != CE::Other && hWnd) {
        // Two phases per blink — dark, then lit.
        m_blinkPhasesLeft = Constants::Messages::OVERLAY_SERVER_BLINK_COUNT * 2;
        m_blinkDark       = true;   // start dark so the first change is visible
        m_blinkEvent      = event;
        KillTimer(hWnd, TIMER_SERVER_BLINK);
        SetTimer(hWnd, TIMER_SERVER_BLINK,
                 static_cast<UINT>(Constants::Messages::OVERLAY_SERVER_BLINK_MS), nullptr);
    }

    RefreshRemoteIndicator();
}

void OverlayManager::OnServerBlinkTimer(HWND hWnd) {
    if (m_blinkPhasesLeft > 0) --m_blinkPhasesLeft;

    if (m_blinkPhasesLeft <= 0) {
        // ALWAYS SETTLE LIT. Whatever happened to the count, the indicator must
        // end showing the true colour rather than the dark phase — a dot left
        // black would read as "the server stopped".
        m_blinkPhasesLeft = 0;
        m_blinkDark       = false;
        KillTimer(hWnd, TIMER_SERVER_BLINK);
    } else {
        m_blinkDark = !m_blinkDark;
    }

    RefreshRemoteIndicator();

    // ONLY THE OVERLAY. This does not touch the image, the playlist or the
    // renderer's state — it swaps one glyph in a text slot and asks for a
    // repaint, which is the same thing a zoom change already does several times
    // a second while the wheel is turning.
    InvalidateRect(hWnd, nullptr, FALSE);
}

void OverlayManager::UpdateInfo(int index, int total, const std::wstring &filename) {
    m_infoIndex = index;
    m_infoTotal = total;
    m_infoFilename = filename;
    RebuildTopLeft();
}

void OverlayManager::UpdateZoom(float /*zoom*/, HWND /*hWnd*/) {
    // Compute effective zoom from cached render-target size — avoids a
    // GetClientRect syscall on every frame (was called via app.GetRealZoom).
    // Same formula as AppState::GetRealZoom(), but fed from the cached render-target
    // size so the displayed % always matches what "Zoom to N%" computes.
    float newZoom = 1.0f;
    if (app.imgWidth > 0 && app.imgHeight > 0 && m_rtW > 0.0f && m_rtH > 0.0f) {
        float renderW = 0.0f, renderH = 0.0f;
        GetRenderSize(m_rtW, m_rtH,
                      static_cast<float>(app.imgWidth), static_cast<float>(app.imgHeight),
                      app.viewMode, app.viewport.zoom, renderW, renderH);
        newZoom = renderW / static_cast<float>(app.imgWidth);
    }
    if (newZoom == m_zoom && !slotTopRight.text.empty())
        return;
    m_zoom = newZoom;
    slotTopRight.UpdateText(BuildTopRightText());
    // Summary mode mirrors the zoom into TOP_LEFT's second line.
    if (app.overlayLayoutMode == 2)
        RebuildTopLeft();
}

void OverlayManager::UpdateDims(int imgW, int imgH, int64_t fileSizeBytes) {
    m_imgW = imgW;
    m_imgH = imgH;
    m_fileSizeBytes = fileSizeBytes;
    // Summary mode mirrors the dimensions into TOP_LEFT's second line.
    if (app.overlayLayoutMode == 2) {
        RebuildTopLeft();
    }
    wchar_t dimBuf2[32];
    swprintf_s(dimBuf2, L"%d×%d", imgW, imgH);
    std::wstring text = dimBuf2;
    if (m_slots[BOT_RIGHT].compact) {
        // 1-line: "1920×1080 / 4.3 MB"
        text += L" / ";
        text += FormatFileSize(fileSizeBytes);
    } else {
        // 2-line: "1920×1080\n4.3 MB"
        text += L'\n';
        text += FormatFileSize(fileSizeBytes);
    }
    slotBotRight.UpdateText(std::move(text));
}

// BOT_LEFT holds two independent readouts — the effect list and the folder
// name — whose order flips with the layout:
//   Grid / Summary : slot is bottom-anchored and grows upward, so the name is
//                    last (lowest) and effects stack above it.
//   Stacked        : slot is top-anchored as the 4th row of the column, so the
//                    name comes first and effects grow downward beneath it.
// Either way the name stays adjacent to the run of effects, never separated.
// The two toggles never touch each other: hiding the folder name leaves the
// effects list exactly as it was, and vice versa.
void OverlayManager::UpdateEffects() {
    // The folder-name toggle changes this slot's height, so the rect has to be
    // recomputed here rather than only on resize — otherwise turning it on
    // would clip the topmost effect. But this runs on every navigation, so do
    // it only when the height genuinely moved, not once per image.
    const float rowH = BotLeftRowHeight();
    if (rowH != m_botLeftRowH) {
        m_botLeftRowH = rowH;
        RecomputeRects();
    }

    std::wstring lines;
    auto appendLine = [&](const std::wstring &s) {
        if (s.empty()) return;
        if (!lines.empty()) lines += L"\n";
        lines += s;
    };

    // ── Folder name line ─────────────────────────────────────────────────────
    // Built up front because where it goes depends on the layout:
    //   Grid    — this slot is bottom-anchored, so the name is appended LAST to
    //             end up lowest, with effects stacking above it.
    //   Stacked — the column flows top-down, so the name comes FIRST (4th line)
    //             and the effects grow downward beneath it.
    //   Summary — the name is TOP_LEFT's first line instead, so it is omitted
    //             here entirely and BOT_LEFT carries effects only.
    const int mode = app.overlayLayoutMode;
    const bool wantDir = app.overlayShowDirName && mode != 2;
    const std::wstring &dirLine = wantDir ? CurrentFolderLine() : kNoLine;
    if (mode == 1) appendLine(dirLine);

    // ── Effects list ─────────────────────────────────────────────────────────
    // effectPreviewEnabled is the render-side flag (it decides whether effects
    // are applied at all); overlayShowEffectsList only decides whether they are
    // listed here. Both must be on for the list to appear.
    if (app.hasActiveEffects && app.effectPreviewEnabled && app.overlayShowEffectsList) {
        auto fmtFloat = [](float v, int decimals = 1) -> std::wstring {
            wchar_t buf[32];
            swprintf_s(buf, L"%.*f", decimals, static_cast<double>(v));
            return buf;
        };

        constexpr float EPS = 0.001f;

        if (std::abs(app.brightness - 0.0f) > EPS)
            appendLine(Constants::Strings::LABEL_BRIGHTNESS + (app.brightness >= 0 ? std::wstring(Constants::Strings::SIGN_POSITIVE) : L"") + fmtFloat(app.brightness));
        if (std::abs(app.contrast - 1.0f) > EPS)
            appendLine(Constants::Strings::LABEL_CONTRAST + fmtFloat(app.contrast));
        if (std::abs(app.saturation - 1.0f) > EPS)
            appendLine(Constants::Strings::LABEL_SATURATION + fmtFloat(app.saturation));
        if (std::abs(app.gamma - 1.0f) > EPS)
            appendLine(Constants::Strings::LABEL_GAMMA + fmtFloat(app.gamma));

        // Listed in application order — top line runs first, bottom line runs last
        // on the result of everything above it. Same vector the renderer chains.
        for (const auto &effectName: app.activeEffectsList)
            appendLine(effectName);
    }

    // Grid: the name goes last so it lands at the bottom. (Empty in Summary.)
    if (mode != 1) appendLine(dirLine);

    // No InvalidateLayout() here on purpose. UpdateText already marks the layout
    // dirty when — and only when — the text actually differs, so forcing it
    // would rebuild an IDWriteTextLayout on every navigation even during a fast
    // scroll where the effect list and folder are unchanged.
    slotBotLeft.UpdateText(std::move(lines));
}

void OverlayManager::RefreshFolderNameLine() {
    RebuildTopLeft();  // Summary keeps the name here
    UpdateEffects();   // Grid / Stacked keep it in BOT_LEFT
}

// Folder holding the current image, name only. Empty when nothing is loaded —
// callers skip the line rather than printing a stray icon.
//
// Cached on the directory portion of the path. Every image in a folder yields
// the same answer, so during a scroll this is one wcsncmp per call instead of
// building an fs::path and three strings. Constructing fs::path is the
// expensive part, so the cache check deliberately works on the raw string and
// only falls through to fs when the directory really changed.
const std::wstring &OverlayManager::CurrentFolderName() {
    // Both "no answer" paths must drop the cache, not just return empty — the
    // composed line is cached alongside, and leaving it set would keep printing
    // the previous folder after the playlist empties.
    auto forget = [this]() -> const std::wstring & {
        m_folderSrc.clear();
        m_folderName.clear();
        m_folderLine.clear();
        return m_folderName;
    };

    if (app.currentIndex < 0 ||
        app.currentIndex >= static_cast<int>(app.playlist.size()))
        return forget();

    const std::wstring &full = app.playlist[app.currentIndex];
    const size_t slash = full.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return forget();

    // Same directory as last time? Compare in place — no allocation.
    if (m_folderSrc.size() == slash &&
        wcsncmp(m_folderSrc.c_str(), full.c_str(), slash) == 0)
        return m_folderName;

    m_folderSrc.assign(full, 0, slash);
    const fs::path p(m_folderSrc);
    m_folderName = p.filename().wstring();
    // A drive root ("D:\") has an empty filename — fall back to the root name
    // so the line reads "D:" instead of vanishing.
    if (m_folderName.empty()) m_folderName = p.root_name().wstring();

    // Compose the display line while we are here. It changes on exactly the
    // same event as the name, so caching it costs no extra invalidation and
    // saves rebuilding "icon + space + name" on every navigation.
    m_folderLine.clear();
    if (!m_folderName.empty()) {
        m_folderLine = Constants::ThemeIcons::ICON_FOLDER;
        m_folderLine += L' ';
        m_folderLine += m_folderName;
    }
    return m_folderName;
}

// "📁 Holiday2024", or empty when no image is loaded. Same cache as
// CurrentFolderName — call that first so a folder change is picked up.
const std::wstring &OverlayManager::CurrentFolderLine() {
    CurrentFolderName();
    return m_folderLine;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Panel selection overlay
// ─────────────────────────────────────────────────────────────────────────────

void OverlayManager::UpdatePanelSelectionOverlay(int8_t position, int selected, int total) {
    TextOverlay *ov = nullptr;
    int slotIdx = -1;
    switch (position) {
        case 1: ov = &slotTopCenter; slotIdx = TOP_CENTER; break;
        case 2: ov = &slotMidRight;  slotIdx = MID_RIGHT;  break;
        case 3: ov = &slotBotCenter; slotIdx = BOT_CENTER; break;
        case 4: ov = &slotMidLeft;   slotIdx = MID_LEFT;   break;
        default: return;
    }
    if (selected <= 0) {
        ov->UpdateText(L"");
        return;
    }
    const bool compact = m_slots[slotIdx].compact;
    wchar_t buf[32];
    swprintf_s(buf, L"%d / %d", selected, total);
    std::wstring text = buf;
    text += compact ? L" sel" : L"\nselected";
    ov->UpdateText(std::move(text));
}

// ─────────────────────────────────────────────────────────────────────────────
//  Center-center message queue
// ─────────────────────────────────────────────────────────────────────────────

void OverlayManager::PostCenterMessage(HWND hWnd, const std::wstring &msg,
                                       MsgSeverity severity) {
    // Only show if the slot is enabled
    if (!m_slots[MID_CENTER].visible) return;

    slotMidCenter.UpdateText(msg);
    slotMidCenter.active = true;
    m_centerMsgSeverity = severity;

    // Reset (or start) the auto-hide timer on the main window
    KillTimer(hWnd, TIMER_CENTER_MSG);
    SetTimer(hWnd, TIMER_CENTER_MSG, static_cast<UINT>(app.msgCenterDisplayMs), nullptr);
    m_centerMsgActive = true;

    InvalidateRect(hWnd, nullptr, FALSE);
}

void OverlayManager::OnCenterMessageTimer(HWND hWnd) {
    KillTimer(hWnd, TIMER_CENTER_MSG);
    m_centerMsgActive = false;
    slotMidCenter.active = false;
    slotMidCenter.UpdateText(L"");
    InvalidateRect(hWnd, nullptr, FALSE);
}

// ─────────────────────────────────────────────────────────────────────────────
//  File size formatter
// ─────────────────────────────────────────────────────────────────────────────

std::wstring OverlayManager::FormatFileSize(int64_t bytes) {
    wchar_t buf[32];
    if (bytes <= 0) {
        return L"";
    } else if (bytes < 1024LL) {
        swprintf_s(buf, L"%lld B", bytes);
    } else if (bytes < 1024LL * 1024LL) {
        swprintf_s(buf, L"%.0f KB", static_cast<double>(bytes) / 1024.0);
    } else if (bytes < 1024LL * 1024LL * 1024LL) {
        swprintf_s(buf, L"%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else {
        swprintf_s(buf, L"%.2f GB", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    }
    return buf;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Visibility
// ─────────────────────────────────────────────────────────────────────────────

TextOverlay *OverlayManager::OverlayForSlot(Slot slot) {
    switch (slot) {
        case TOP_LEFT: return &slotTopLeft;
        case TOP_CENTER: return &slotTopCenter;
        case TOP_RIGHT: return &slotTopRight;
        case MID_LEFT: return &slotMidLeft;
        case MID_CENTER: return &slotMidCenter;
        case MID_RIGHT: return &slotMidRight;
        case BOT_LEFT: return &slotBotLeft;
        case BOT_CENTER: return &slotBotCenter;
        case BOT_RIGHT: return &slotBotRight;
        default: return nullptr;
    }
}

// Rebuilds both bitmasks from the live slots and writes them through. Every
// mutator funnels here, so no per-slot change can escape unpersisted — the same
// reason SaveSetting intercepts dedicated mode centrally rather than per caller.
void OverlayManager::PersistSlotState() {
    unsigned visMask = 0, cmpMask = 0;
    for (int i = 0; i < SLOT_COUNT; ++i) {
        if (m_slots[i].visible) visMask |= (1u << i);
        if (m_slots[i].compact) cmpMask |= (1u << i);
    }
    app.overlaySlotVisibleMask = visMask;
    app.overlaySlotCompactMask = cmpMask;
    Persistence::Registry::SaveSetting(Constants::Registry::OVERLAY_SLOT_VISIBLE, visMask);
    Persistence::Registry::SaveSetting(Constants::Registry::OVERLAY_SLOT_COMPACT, cmpMask);
}

void OverlayManager::SetSlotVisible(Slot slot, bool show) {
    if (slot < 0 || slot >= SLOT_COUNT) return;
    m_slots[slot].visible = show;
    // If hiding MID_CENTER, also clear any active message
    if (slot == MID_CENTER && !show) {
        slotMidCenter.active = false;
        slotMidCenter.UpdateText(L"");
    }
    PersistSlotState();
}

void OverlayManager::SetAllVisible(bool show) {
    m_masterVisible = show;
    for (int i = 0; i < SLOT_COUNT; ++i)
        m_slots[i].overlay->active = show && m_slots[i].visible;
    // MID_CENTER re-active only if there is a pending message
    if (show && m_centerMsgActive) {
        slotMidCenter.active = true;
    } else if (!show) {
        slotMidCenter.active = false;
    }
}

bool OverlayManager::IsSlotVisible(Slot slot) const {
    if (slot < 0 || slot >= SLOT_COUNT) return false;
    return m_slots[slot].visible;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Compact mode
// ─────────────────────────────────────────────────────────────────────────────

void OverlayManager::ToggleCompactMode(Slot slot) {
    if (slot < 0 || slot >= SLOT_COUNT) return;
    if (slot == MID_CENTER) return; // no-op — always single line

    m_slots[slot].compact = !m_slots[slot].compact;

    // Invalidate that slot's layout so it re-measures at the new rect
    if (m_slots[slot].overlay)
        m_slots[slot].overlay->InvalidateLayout();

    // Recalculate the rect for this slot
    RecomputeRects();

    // Some slots need text rebuilt to reflect 1-line vs 2-line format
    RebuildForCompactChange(slot);

    PersistSlotState();
}

bool OverlayManager::IsCompact(Slot slot) const {
    if (slot < 0 || slot >= SLOT_COUNT) return false;
    return m_slots[slot].compact;
}

// Ctrl+1..9 walks the same three states the Overlays submenu offers, in the
// same order it lists them: Compact → Full → Off → Compact. This is why there
// is no separate compact shortcut — visibility and compactness are one control.
// MID_CENTER has no compact form, so it degenerates to On → Off, matching its
// two-item submenu.
void OverlayManager::CycleSlotState(Slot slot) {
    if (slot < 0 || slot >= SLOT_COUNT) return;

    if (slot == MID_CENTER) {
        SetSlotVisible(slot, !m_slots[slot].visible); // persists on its own
        return;
    }

    SlotMeta &m = m_slots[slot];
    if (!m.visible) {
        m.visible = true;
        m.compact = true;   // Off → Compact
    } else if (m.compact) {
        m.compact = false;  // Compact → Full
    } else {
        m.visible = false;  // Full → Off
    }

    if (m.overlay) m.overlay->InvalidateLayout();
    RecomputeRects();
    RebuildForCompactChange(slot);
    PersistSlotState();
}

// Slots whose text is formatted differently in 1-line vs 2-line mode have to be
// rebuilt when that flag flips, or they keep the old shape until whatever
// normally refreshes them happens to run.
void OverlayManager::RebuildForCompactChange(Slot slot) {
    if (slot == TOP_LEFT) RebuildTopLeft();
    if (slot == BOT_RIGHT) UpdateDims(m_imgW, m_imgH, m_fileSizeBytes);
    // TOP_RIGHT joined this list when the server indicator arrived: compact is
    // the dot alone, full adds the client count.
    //
    // The TEXT half only — a compact toggle is not somebody connecting, so it
    // must neither blink nor move the client-count baseline that decides when
    // the next real change does.
    if (slot == TOP_RIGHT) RefreshRemoteIndicator();
}

std::wstring OverlayManager::SlotStateMessage(Slot slot) const {
    if (slot < 0 || slot >= SLOT_COUNT) return {};
    std::wstring msg = Constants::Messages::OVERLAY_PREFIX;
    msg += Constants::Messages::OVERLAY_SLOT_NAMES[slot];
    if (!m_slots[slot].visible)
        msg += Constants::Messages::STATE_OFF_SUFFIX;
    else if (slot == MID_CENTER) // no compact state to report
        msg += Constants::Messages::STATE_ON_SUFFIX;
    else
        msg += m_slots[slot].compact ? Constants::Messages::OVERLAY_STATE_COMPACT
                                     : Constants::Messages::OVERLAY_STATE_FULL;
    return msg;
}

// A slot whose content a layout mode has folded elsewhere. Summary merges the
// zoom (TOP_RIGHT) and the dimensions / size (BOT_RIGHT) into TOP_LEFT's second
// line, so drawing them in their own corners too would show each value twice.
//
// This has to be a render-time test. UpdateZoom and UpdateDims rewrite those
// TextOverlays on every navigation and zoom change, so blanking their text on
// entry to Summary only holds until the next image — after which the stale
// corner reappears, and with a zero-size rect GetLayout clamps to 1x1 and DWrite
// stacks it one glyph per line in the top-left corner, over the summary itself.
bool OverlayManager::IsSlotSuppressed(int slot) const {
    if (app.overlayLayoutMode != 2) return false;
    return slot == TOP_RIGHT || slot == BOT_RIGHT;
}

void OverlayManager::ApplyPersistedState(HWND hWnd) {
    for (int i = 0; i < SLOT_COUNT; ++i) {
        m_slots[i].visible = (app.overlaySlotVisibleMask >> i) & 1u;
        m_slots[i].compact = (app.overlaySlotCompactMask >> i) & 1u;
        if (m_slots[i].overlay)
            m_slots[i].overlay->InvalidateLayout();
    }
    // MID_CENTER stays dark until a message arrives, whatever its bit says.
    slotMidCenter.active = false;
    // Text style is AppState too, so a wholesale replace has to re-derive the
    // formats and the brush colour — assigning the fields alone changes nothing.
    UpdateTextFormat();
    ApplyTextColor();
    // Rebuilds rects, formats, text and active flags for the restored mode.
    ApplyLayoutMode(hWnd);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Render
// ─────────────────────────────────────────────────────────────────────────────

void OverlayManager::RenderAll(ID2D1DeviceContext *ctx) const {
    if (!m_pTextBrush) return;

    for (int i = 0; i < SLOT_COUNT; ++i) {
        const SlotMeta &meta = m_slots[i];

        // MID_CENTER: rendered only when active (has a live message).
        // IMPORTANT: MID_CENTER bypasses m_masterVisible so that "Info Panels: OFF"
        // and other state-change messages appear even when all other overlays are hidden.
        if (i == MID_CENTER) {
            if (!meta.visible || !slotMidCenter.active || slotMidCenter.text.empty()) continue;
            if (!meta.fmt || !m_pCenterBrush) continue;

            IDWriteTextLayout *layout = const_cast<TextOverlay *>(&slotMidCenter)->GetLayout(
                    m_pDWriteFactory, meta.fmt);
            if (!layout) continue;

            const DWRITE_TEXT_METRICS &tm = slotMidCenter.GetCachedMetrics();

            const D2D1_RECT_F &slotRect = slotMidCenter.rect;
            const float slotW = slotRect.right - slotRect.left;
            const float inkLeft = slotRect.left + (slotW - tm.width) * 0.5f;
            const float inkTop = slotRect.top + (slotRect.bottom - slotRect.top - tm.height) * 0.5f;

            D2D1_RECT_F bgRect = D2D1::RectF(
                    inkLeft - BG_PADDING * 3.0f,
                    inkTop - BG_PADDING * 2.0f,
                    inkLeft + tm.width + BG_PADDING * 3.0f,
                    inkTop + tm.height + BG_PADDING * 2.0f);

            bgRect.left = std::max(0.0f, bgRect.left);
            bgRect.top = std::max(0.0f, bgRect.top);
            bgRect.right = std::min(m_rtW, bgRect.right);
            bgRect.bottom = std::min(m_rtH, bgRect.bottom);

            // Its own brush, so the message brush no longer has to be recoloured
            // to black and back on every single frame just to fill this rect.
            if (app.overlayShowBackground && m_pCenterBgBrush)
                ctx->FillRectangle(bgRect, m_pCenterBgBrush.Get());

            // Text colour follows severity: Warning / Error pull from
            // Constants::Theme::Markers, the same source the History panel uses
            // for its empty / missing folder rows. Applied only when the
            // severity actually changes — it is constant for the life of a
            // message, so re-setting it per frame was pure churn.
            if (!m_centerBrushSet || m_centerBrushSeverity != m_centerMsgSeverity) {
                m_centerBrushSet = true;
                m_centerBrushSeverity = m_centerMsgSeverity;
                if (m_centerMsgSeverity == MsgSeverity::Normal) {
                    m_pCenterBrush->SetColor(D2D1::ColorF(
                            Constants::Overlay::MSG_CENTER_COLOR_R,
                            Constants::Overlay::MSG_CENTER_COLOR_G,
                            Constants::Overlay::MSG_CENTER_COLOR_B,
                            Constants::Overlay::MSG_CENTER_COLOR_A));
                } else {
                    const COLORREF c = (m_centerMsgSeverity == MsgSeverity::Error)
                                               ? Constants::Theme::Markers::ERR
                                               : Constants::Theme::Markers::WARNING;
                    m_pCenterBrush->SetColor(D2D1::ColorF(
                            GetRValue(c) / 255.0f,
                            GetGValue(c) / 255.0f,
                            GetBValue(c) / 255.0f,
                            Constants::Overlay::MSG_CENTER_COLOR_A));
                }
            }
            ctx->DrawTextLayout(
                    D2D1::Point2F(slotRect.left, slotRect.top),
                    layout,
                    m_pCenterBrush.Get(),
                    D2D1_DRAW_TEXT_OPTIONS_NONE);
            continue;
        }

        // All other slots — respect the master toggle
        if (!m_masterVisible) continue;
        if (IsSlotSuppressed(i)) continue;
        if (!meta.visible || !meta.overlay || meta.overlay->text.empty()) continue;
        if (!meta.fmt) continue;

        const TextOverlay *ov = meta.overlay;

        IDWriteTextLayout *layout = const_cast<TextOverlay *>(ov)->GetLayout(
                m_pDWriteFactory, meta.fmt);
        if (!layout) continue;

        const DWRITE_TEXT_METRICS &tm = ov->GetCachedMetrics();

        const D2D1_RECT_F &slotRect = ov->rect;
        const float slotW = slotRect.right - slotRect.left;

        float inkLeft;
        switch (meta.cachedAlignment) {
            case DWRITE_TEXT_ALIGNMENT_LEADING:
                inkLeft = slotRect.left;
                break;
            case DWRITE_TEXT_ALIGNMENT_TRAILING:
                inkLeft = slotRect.right - tm.width;
                break;
            case DWRITE_TEXT_ALIGNMENT_CENTER:
            default:
                inkLeft = slotRect.left + (slotW - tm.width) * 0.5f;
                break;
        }

        float inkTop = slotRect.top + tm.top;

        D2D1_RECT_F bgRect = D2D1::RectF(
                inkLeft - BG_PADDING,
                inkTop - BG_PADDING,
                inkLeft + tm.width + BG_PADDING,
                inkTop + tm.height + BG_PADDING);

        bgRect.left = std::max(0.0f, bgRect.left);
        bgRect.top = std::max(0.0f, bgRect.top);
        bgRect.right = std::min(m_rtW, bgRect.right);
        bgRect.bottom = std::min(m_rtH, bgRect.bottom);

        // Semi-transparent background — slots may opt out entirely.
        if (app.overlayShowBackground && meta.drawBackground && m_pBgBrush)
            ctx->FillRectangle(bgRect, m_pBgBrush.Get());

        // Draw text in the user-chosen outer-slot colour, falling back to the
        // renderer's shared brush if the device context was gone at Init time.
        // ENABLE_COLOR_FONT so the server dot (U+1F7E2) renders as the colour
        // glyph rather than a monochrome outline in the slot's brush colour.
        // Only affects glyphs that actually carry colour layers, so ordinary
        // text — including every other slot — is drawn exactly as before.
        ctx->DrawTextLayout(
                D2D1::Point2F(slotRect.left, slotRect.top),
                layout,
                m_pOuterBrush ? m_pOuterBrush.Get() : m_pTextBrush,
                D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Device loss / restore
// ─────────────────────────────────────────────────────────────────────────────

void OverlayManager::InvalidateLayouts() {
    for (int i = 0; i < SLOT_COUNT; ++i) {
        if (m_slots[i].overlay)
            m_slots[i].overlay->InvalidateLayout();
    }
}

void OverlayManager::OnDeviceLost() {
    m_pCenterBrush.Reset();
    m_pCenterBgBrush.Reset();
    m_pOuterBrush.Reset();
    m_pBgBrush.Reset();
    m_centerBrushSet = false;
    InvalidateLayouts();
}

void OverlayManager::OnDeviceRestored(ID2D1DeviceContext *ctx) {
    BuildCenterBrush(ctx);
    BuildOuterBrush(ctx);
    m_pBgBrush.Reset();
    if (ctx) ctx->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, BG_ALPHA), &m_pBgBrush);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Free function bridge — called from AppState::WakeUpAndApplyEffects()
// ─────────────────────────────────────────────────────────────────────────────
void QIV_UpdateEffectsOverlay() {
    g_overlayManager.UpdateEffects();
}
