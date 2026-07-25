#include "OverlayManager.h"
#include "../AppState.h"
#include "../Common/Converters.h"
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
static constexpr float ROW_EFFECTS = static_cast<float>(EFFECT_MAX_LINES) * 28.0f;

static constexpr float BG_ALPHA = 0.45f;
static constexpr float BG_PADDING = 3.0f;

// ─────────────────────────────────────────────────────────────────────────────
//  Init
// ─────────────────────────────────────────────────────────────────────────────

void OverlayManager::Init(IDWriteFactory3 *dwriteFactory,
                          ID2D1SolidColorBrush *textBrush,
                          ID2D1DeviceContext *ctx) {
    m_pDWriteFactory = dwriteFactory;

    m_pTextBrush = textBrush;

    auto wire = [&](Slot s, TextOverlay *ov, bool defaultVisible) {
        m_slots[s].overlay = ov;
        m_slots[s].visible = defaultVisible;
        m_slots[s].compact = Constants::Overlay::IS_COMPACT_OVERLAY_MODE;
    };

    wire(TOP_LEFT, &slotTopLeft, true);
    wire(TOP_CENTER, &slotTopCenter, true); // used when switching with o do not disable for now
    wire(TOP_RIGHT, &slotTopRight, true); // zoom
    wire(MID_LEFT, &slotMidLeft, true);     // panel-selection overlay (left panel)
    wire(MID_CENTER, &slotMidCenter, true); // center-center message queue
    wire(MID_RIGHT, &slotMidRight, true);   // panel-selection overlay (right panel)
    wire(BOT_LEFT, &slotBotLeft, true); // effects
    wire(BOT_CENTER, &slotBotCenter, true); // panel-selection overlay (bottom panel)
    wire(BOT_RIGHT, &slotBotRight, true); // dims / size

    // MID_CENTER is never shown until a message is posted
    slotMidCenter.active = false;
    m_slots[MID_CENTER].visible = true; // enabled by default, but no text yet

    BuildSlotFormats();
    BuildCenterBrush(ctx);
    if (ctx) ctx->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, BG_ALPHA), &m_pBgBrush);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Init text Size and brush color
// ─────────────────────────────────────────────────────────────────────────────
void OverlayManager::UpdateTextFormat() {
    if (!m_pDWriteFactory) return;

    // -------------------------------------------------------------------------
    // 1. Create Base Font (for the 8 outer slots)
    // -------------------------------------------------------------------------
    float scaledFontSize = Constants::Overlay::MSG_ALL_BUT_CENTER_FONT_SIZE * app.dpiScale;

    if (m_pTextFormat) {
        m_pTextFormat->Release();
        m_pTextFormat = nullptr;
    }

    HRESULT hr = m_pDWriteFactory->CreateTextFormat(
            Constants::Overlay::MSG_ALL_BUT_CENTER_FONT_FAMILY_DEFAULT, nullptr,
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

void OverlayManager::BuildCenterBrush(ID2D1DeviceContext *ctx) {
    if (!ctx) return;
    m_pCenterBrush.Reset();
    D2D1_COLOR_F color = D2D1::ColorF(
            Constants::Overlay::MSG_CENTER_COLOR_R,
            Constants::Overlay::MSG_CENTER_COLOR_G,
            Constants::Overlay::MSG_CENTER_COLOR_B,
            Constants::Overlay::MSG_CENTER_COLOR_A);
    ctx->CreateSolidColorBrush(color, &m_pCenterBrush);
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

    // MID_CENTER is always screen-centered regardless of layout mode
    auto placeMidCenter = [&]() {
        slotMidCenter.UpdateRect(D2D1::RectF(
                (W - Constants::Overlay::MSG_CENTER_WIDTH) * 0.5f,
                (H - Constants::Overlay::MSG_CENTER_HEIGHT) * 0.5f,
                (W + Constants::Overlay::MSG_CENTER_WIDTH) * 0.5f,
                (H + Constants::Overlay::MSG_CENTER_HEIGHT) * 0.5f));
    };

    // ── Mode 1: all slots stacked vertically on top-left ─────────────────────
    if (Constants::Overlay::OVERLAY_LAYOUT_MODE == 1) {
        float cursorY = M;
        // Order: TOP_LEFT, TOP_CENTER, TOP_RIGHT, MID_LEFT, MID_RIGHT,
        //        BOT_RIGHT, BOT_CENTER, BOT_LEFT(effects last)
        // All slots use COL_LEFT_WIDTH and LEADING alignment when stacked.
        struct StackEntry {
            Slot slot;
            TextOverlay *ov;
        };
        const StackEntry entries[] = {
            {TOP_LEFT, &slotTopLeft},
            {TOP_CENTER, &slotTopCenter},
            {TOP_RIGHT, &slotTopRight},
            {MID_LEFT, &slotMidLeft},
            {MID_RIGHT, &slotMidRight},
            {BOT_RIGHT, &slotBotRight},
            {BOT_CENTER, &slotBotCenter},
            {BOT_LEFT, &slotBotLeft}, // effects last
        };
        for (const auto &e: entries) {
            float rowH = (e.slot == BOT_LEFT)
                             ? ROW_EFFECTS
                             : m_slots[e.slot].compact
                                   ? ROW_SINGLE
                                   : ROW_DOUBLE;
            e.ov->UpdateRect(D2D1::RectF(M, cursorY, M + COL_LEFT_WIDTH, cursorY + rowH));
            // Force leading alignment so all stacked text is left-aligned
            m_slots[e.slot].fmt = (e.slot == BOT_LEFT) ? m_fmtBotLeft.Get() : m_fmtLeft.Get();
            cursorY += rowH + M;
        }
        placeMidCenter();
        return;
    }

    // ── Mode 2: compact 2-line summary top-left ───────────────────────────────
    // Line 1 (TOP_LEFT):   index / total + filename  (unchanged)
    // Line 2 (TOP_CENTER): zoom% + WxH / size        (combined, repurposed rect)
    // All other info slots get zero-size rects so they produce no output.
    if (Constants::Overlay::OVERLAY_LAYOUT_MODE == 2) {
        slotTopLeft.UpdateRect(D2D1::RectF(M, M, M + COL_LEFT_WIDTH, M + ROW_SINGLE));
        slotTopCenter.UpdateRect(D2D1::RectF(M, M + ROW_SINGLE + M, M + COL_LEFT_WIDTH, M + ROW_SINGLE * 2.0f + M));
        const D2D1_RECT_F zero = D2D1::RectF(0.0f, 0.0f, 0.0f, 0.0f);
        slotTopRight.UpdateRect(zero);
        slotMidLeft.UpdateRect(zero);
        slotMidRight.UpdateRect(zero);
        slotBotLeft.UpdateRect(zero);
        slotBotCenter.UpdateRect(zero);
        slotBotRight.UpdateRect(zero);
        placeMidCenter();
        return;
    }

    // ── Mode 0: normal 3×3 grid ───────────────────────────────────────────────

    // [1] TOP_LEFT — index/total + filename (1 or 2 lines)
    {
        float rowH = m_slots[TOP_LEFT].compact ? ROW_SINGLE : ROW_DOUBLE;
        slotTopLeft.UpdateRect(D2D1::RectF(M, M, M + COL_LEFT_WIDTH, M + rowH));
    }

    // [2] TOP_CENTER — zoom %, always single line
    slotTopCenter.UpdateRect(D2D1::RectF(
            (W - COL_CENTER_WIDTH) * 0.5f, M,
            (W + COL_CENTER_WIDTH) * 0.5f, M + ROW_SINGLE));

    // [3] TOP_RIGHT — unused
    {
        float rowH = m_slots[TOP_RIGHT].compact ? ROW_SINGLE : ROW_DOUBLE;
        slotTopRight.UpdateRect(D2D1::RectF(
                W - COL_RIGHT_WIDTH - M, M,
                W - M, M + rowH));
    }

    // [4] MID_LEFT
    {
        float rowH = m_slots[MID_LEFT].compact ? ROW_SINGLE : ROW_DOUBLE;
        slotMidLeft.UpdateRect(D2D1::RectF(
                M, (H - rowH) * 0.5f,
                M + COL_LEFT_WIDTH, (H + rowH) * 0.5f));
    }

    // [5] MID_CENTER — center-center message, always single line, centered in screen
    slotMidCenter.UpdateRect(D2D1::RectF(
            (W - Constants::Overlay::MSG_CENTER_WIDTH) * 0.5f,
            (H - Constants::Overlay::MSG_CENTER_HEIGHT) * 0.5f,
            (W + Constants::Overlay::MSG_CENTER_WIDTH) * 0.5f,
            (H + Constants::Overlay::MSG_CENTER_HEIGHT) * 0.5f));

    // [6] MID_RIGHT
    {
        float rowH = m_slots[MID_RIGHT].compact ? ROW_SINGLE : ROW_DOUBLE;
        slotMidRight.UpdateRect(D2D1::RectF(
                W - COL_RIGHT_WIDTH - M, (H - rowH) * 0.5f,
                W - M, (H + rowH) * 0.5f));
    }

    // [7] BOT_LEFT — effect list, anchored to bottom, grows upward
    slotBotLeft.UpdateRect(D2D1::RectF(
            M, H - M - ROW_EFFECTS,
            M + COL_LEFT_WIDTH, H - M));

    // [8] BOT_CENTER
    {
        float rowH = m_slots[BOT_CENTER].compact ? ROW_SINGLE : ROW_DOUBLE;
        slotBotCenter.UpdateRect(D2D1::RectF(
                (W - COL_CENTER_WIDTH) * 0.5f, H - M - rowH,
                (W + COL_CENTER_WIDTH) * 0.5f, H - M));
    }

    // [9] BOT_RIGHT — dimensions / file size
    {
        float rowH = m_slots[BOT_RIGHT].compact ? ROW_SINGLE : ROW_DOUBLE;
        slotBotRight.UpdateRect(D2D1::RectF(
                W - COL_RIGHT_WIDTH - M, H - M - rowH,
                W - M, H - M));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Content updates
// ─────────────────────────────────────────────────────────────────────────────

void OverlayManager::RebuildTopLeft() {
    wchar_t buf[32];
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

void OverlayManager::RebuildSummaryLine2() {
    // "86%  1920×1080 / 4.3 MB"
    wchar_t zoomBuf[16];
    swprintf_s(zoomBuf, L"%d%%", Converters::toZoomInt(m_zoom));
    std::wstring text = zoomBuf;
    text += L"  ";
    wchar_t dimBuf[32];
    swprintf_s(dimBuf, L"%d\u00D7%d", m_imgW, m_imgH);
    text += dimBuf;
    text += L" / ";
    text += FormatFileSize(m_fileSizeBytes);
    slotTopCenter.UpdateText(std::move(text));
}

void OverlayManager::OnLayoutModeChanged(HWND /*hWnd*/) {
    RecomputeRects();
    // Rebuild slot content to match the new mode
    RebuildTopLeft();
    if (Constants::Overlay::OVERLAY_LAYOUT_MODE == 2) {
        RebuildSummaryLine2();
        // Activate the two visible slots; deactivate the rest
        slotTopLeft.active = m_masterVisible;
        slotTopCenter.active = m_masterVisible;
        slotTopRight.active = false;
        slotMidLeft.active = false;
        slotMidRight.active = false;
        slotBotLeft.active = false;
        slotBotCenter.active = false;
        slotBotRight.active = false;
    } else {
        // Restore fmt assignments (mode 1 overrides them to LEADING for all slots)
        m_slots[TOP_LEFT].fmt = m_fmtLeft.Get();        m_slots[TOP_LEFT].cachedAlignment = DWRITE_TEXT_ALIGNMENT_LEADING;
        m_slots[TOP_CENTER].fmt = m_fmtCenter.Get();    m_slots[TOP_CENTER].cachedAlignment = DWRITE_TEXT_ALIGNMENT_CENTER;
        m_slots[TOP_RIGHT].fmt = m_fmtTrailing.Get();   m_slots[TOP_RIGHT].cachedAlignment = DWRITE_TEXT_ALIGNMENT_TRAILING;
        m_slots[MID_LEFT].fmt = m_fmtLeft.Get();        m_slots[MID_LEFT].cachedAlignment = DWRITE_TEXT_ALIGNMENT_LEADING;
        m_slots[MID_CENTER].fmt = m_fmtCenter5.Get();   m_slots[MID_CENTER].cachedAlignment = DWRITE_TEXT_ALIGNMENT_CENTER;
        m_slots[MID_RIGHT].fmt = m_fmtTrailing.Get();   m_slots[MID_RIGHT].cachedAlignment = DWRITE_TEXT_ALIGNMENT_TRAILING;
        m_slots[BOT_LEFT].fmt = m_fmtBotLeft.Get();     m_slots[BOT_LEFT].cachedAlignment = DWRITE_TEXT_ALIGNMENT_LEADING;
        m_slots[BOT_CENTER].fmt = m_fmtBotCenter.Get(); m_slots[BOT_CENTER].cachedAlignment = DWRITE_TEXT_ALIGNMENT_CENTER;
        m_slots[BOT_RIGHT].fmt = m_fmtBotRight.Get();   m_slots[BOT_RIGHT].cachedAlignment = DWRITE_TEXT_ALIGNMENT_TRAILING;
        // Clear the summary text that mode 2 wrote into slotTopCenter
        slotTopCenter.UpdateText(L"");
        // Restore active state from master + per-slot visibility
        for (int i = 0; i < SLOT_COUNT; ++i) {
            if (i == MID_CENTER) continue;
            m_slots[i].overlay->active = m_masterVisible && m_slots[i].visible;
        }
        // Restore normal zoom text in TOP_RIGHT
        wchar_t buf[32];
        swprintf_s(buf, L"%d%%", Converters::toZoomInt(m_zoom));
        slotTopRight.UpdateText(buf);
    }
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
    wchar_t buf[32];
    swprintf_s(buf, L"%d%%", Converters::toZoomInt(m_zoom));
    slotTopRight.UpdateText(buf);
    if (Constants::Overlay::OVERLAY_LAYOUT_MODE == 2)
        RebuildSummaryLine2();
}

void OverlayManager::UpdateDims(int imgW, int imgH, int64_t fileSizeBytes) {
    m_imgW = imgW;
    m_imgH = imgH;
    m_fileSizeBytes = fileSizeBytes;
    if (Constants::Overlay::OVERLAY_LAYOUT_MODE == 2) {
        RebuildSummaryLine2();
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

void OverlayManager::UpdateEffects() {
    if (!app.hasActiveEffects || !app.effectPreviewEnabled) {
        slotBotLeft.UpdateText(L"");
        slotBotLeft.InvalidateLayout();
        return;
    }

    std::wstring lines;
    auto appendLine = [&](const std::wstring &s) {
        if (!lines.empty()) lines += L"\n";
        lines += s;
    };

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

    for (const auto &effectName: app.activeEffectsList)
        appendLine(effectName);

    slotBotLeft.UpdateText(std::move(lines));
    slotBotLeft.InvalidateLayout();
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

void OverlayManager::PostCenterMessage(HWND hWnd, const std::wstring &msg) {
    // Only show if the slot is enabled
    if (!m_slots[MID_CENTER].visible) return;

    slotMidCenter.UpdateText(msg);
    slotMidCenter.active = true;

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

void OverlayManager::SetSlotVisible(Slot slot, bool show) {
    if (slot < 0 || slot >= SLOT_COUNT) return;
    m_slots[slot].visible = show;
    // If hiding MID_CENTER, also clear any active message
    if (slot == MID_CENTER && !show) {
        slotMidCenter.active = false;
        slotMidCenter.UpdateText(L"");
    }
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
    if (slot == TOP_LEFT)
        RebuildTopLeft();
}

bool OverlayManager::IsCompact(Slot slot) const {
    if (slot < 0 || slot >= SLOT_COUNT) return false;
    return m_slots[slot].compact;
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

            // Draw semi-transparent dark background
            if (app.overlayShowBackground) {
                m_pCenterBrush->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.60f));
                ctx->FillRectangle(bgRect, m_pCenterBrush.Get());
            }

            // Draw text in center-center color
            m_pCenterBrush->SetColor(D2D1::ColorF(
                    Constants::Overlay::MSG_CENTER_COLOR_R,
                    Constants::Overlay::MSG_CENTER_COLOR_G,
                    Constants::Overlay::MSG_CENTER_COLOR_B,
                    Constants::Overlay::MSG_CENTER_COLOR_A));
            ctx->DrawTextLayout(
                    D2D1::Point2F(slotRect.left, slotRect.top),
                    layout,
                    m_pCenterBrush.Get(),
                    D2D1_DRAW_TEXT_OPTIONS_NONE);
            continue;
        }

        // All other slots — respect the master toggle
        if (!m_masterVisible) continue;
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

        // Semi-transparent background
        if (app.overlayShowBackground && m_pBgBrush)
            ctx->FillRectangle(bgRect, m_pBgBrush.Get());

        // Draw text
        ctx->DrawTextLayout(
                D2D1::Point2F(slotRect.left, slotRect.top),
                layout,
                m_pTextBrush,
                D2D1_DRAW_TEXT_OPTIONS_NONE);
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
    m_pBgBrush.Reset();
    InvalidateLayouts();
}

void OverlayManager::OnDeviceRestored(ID2D1DeviceContext *ctx) {
    BuildCenterBrush(ctx);
    m_pBgBrush.Reset();
    if (ctx) ctx->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, BG_ALPHA), &m_pBgBrush);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Free function bridge — called from AppState::WakeUpAndApplyEffects()
// ─────────────────────────────────────────────────────────────────────────────
void QIV_UpdateEffectsOverlay() {
    g_overlayManager.UpdateEffects();
}
