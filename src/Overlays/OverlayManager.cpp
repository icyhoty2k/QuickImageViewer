#include "OverlayManager.h"
#include "../AppState.h"
#include <algorithm>
#include <cmath>
#include <wrl/client.h>

OverlayManager g_overlayManager;

// ─────────────────────────────────────────────────────────────────────────────
//  Constants
// ─────────────────────────────────────────────────────────────────────────────

static constexpr float MARGIN = 12.0f;
static constexpr int EFFECT_MAX_LINES = 8;

static constexpr float COL_LEFT_WIDTH = 360.0f;
static constexpr float COL_RIGHT_WIDTH = 280.0f;
static constexpr float COL_CENTER_WIDTH = 200.0f;

static constexpr float ROW_SINGLE = 24.0f;
static constexpr float ROW_DOUBLE = 44.0f;
static constexpr float ROW_EFFECTS = static_cast<float>(EFFECT_MAX_LINES) * 20.0f;

static constexpr float BG_ALPHA = 0.45f;
static constexpr float BG_PADDING = 3.0f;

// ─────────────────────────────────────────────────────────────────────────────
//  Init
// ─────────────────────────────────────────────────────────────────────────────

void OverlayManager::Init(IDWriteFactory3 *dwriteFactory,
                          IDWriteTextFormat *textFormat,
                          ID2D1SolidColorBrush *textBrush) {
    m_pDWriteFactory = dwriteFactory;
    m_pTextFormat = textFormat;
    m_pTextBrush = textBrush;

    auto wire = [&](Slot s, TextOverlay *ov, bool defaultVisible) {
        m_slots[s].overlay = ov;
        m_slots[s].visible = defaultVisible;
    };
    wire(TOP_LEFT, &slotTopLeft, true);
    wire(TOP_CENTER, &slotTopCenter, true); // zoom
    wire(TOP_RIGHT, &slotTopRight, true); // index / filename
    wire(MID_LEFT, &slotMidLeft, true);
    wire(MID_CENTER, &slotMidCenter, true);
    wire(MID_RIGHT, &slotMidRight, true);
    wire(BOT_LEFT, &slotBotLeft, true); // effects
    wire(BOT_CENTER, &slotBotCenter, true);
    wire(BOT_RIGHT, &slotBotRight, true); // dims / size

    BuildSlotFormats();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Build per-column IDWriteTextFormat objects
// ─────────────────────────────────────────────────────────────────────────────

void OverlayManager::BuildSlotFormats() {
    if (!m_pDWriteFactory || !m_pTextFormat) return;

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

    auto makeFormat = [&](IDWriteTextFormat **ppOut, DWRITE_TEXT_ALIGNMENT align) {
        Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt;
        HRESULT hr = m_pDWriteFactory->CreateTextFormat(
                famName.c_str(), nullptr,
                weight, style, stretch,
                fontSize, locale.c_str(), &fmt);
        if (SUCCEEDED(hr)) {
            fmt->SetTextAlignment(align);
            fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            *ppOut = fmt.Detach();
        }
    };

    IDWriteTextFormat *raw = nullptr;

    makeFormat(&raw, DWRITE_TEXT_ALIGNMENT_LEADING);
    m_fmtLeft.Attach(raw);
    raw = nullptr;

    makeFormat(&raw, DWRITE_TEXT_ALIGNMENT_CENTER);
    m_fmtCenter.Attach(raw);
    raw = nullptr;

    makeFormat(&raw, DWRITE_TEXT_ALIGNMENT_TRAILING);
    m_fmtTrailing.Attach(raw);
    raw = nullptr;

    m_slots[TOP_LEFT].fmt = m_fmtLeft.Get();
    m_slots[TOP_CENTER].fmt = m_fmtCenter.Get();
    m_slots[TOP_RIGHT].fmt = m_fmtTrailing.Get();
    m_slots[MID_LEFT].fmt = m_fmtLeft.Get();
    m_slots[MID_CENTER].fmt = m_fmtCenter.Get();
    m_slots[MID_RIGHT].fmt = m_fmtTrailing.Get();
    m_slots[BOT_LEFT].fmt = m_fmtLeft.Get();
    m_slots[BOT_CENTER].fmt = m_fmtCenter.Get();
    m_slots[BOT_RIGHT].fmt = m_fmtTrailing.Get();

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

    // TOP_RIGHT — index/total line + filename line, right-aligned
    slotTopRight.UpdateRect(D2D1::RectF(
            W - COL_RIGHT_WIDTH - M, M,
            W - M, M + ROW_DOUBLE));

    // TOP_CENTER — zoom percentage, centered
    slotTopCenter.UpdateRect(D2D1::RectF(
            (W - COL_CENTER_WIDTH) * 0.5f, M,
            (W + COL_CENTER_WIDTH) * 0.5f, M + ROW_SINGLE));

    // TOP_LEFT — unused
    slotTopLeft.UpdateRect(D2D1::RectF(
            M, M,
            M + COL_LEFT_WIDTH, M + ROW_SINGLE));

    // MID_LEFT — unused
    slotMidLeft.UpdateRect(D2D1::RectF(
            M, (H - ROW_SINGLE) * 0.5f,
            M + COL_LEFT_WIDTH, (H + ROW_SINGLE) * 0.5f));

    // MID_CENTER — unused
    slotMidCenter.UpdateRect(D2D1::RectF(
            (W - COL_CENTER_WIDTH) * 0.5f, (H - ROW_SINGLE) * 0.5f,
            (W + COL_CENTER_WIDTH) * 0.5f, (H + ROW_SINGLE) * 0.5f));

    // MID_RIGHT — unused
    slotMidRight.UpdateRect(D2D1::RectF(
            W - COL_RIGHT_WIDTH - M, (H - ROW_SINGLE) * 0.5f,
            W - M, (H + ROW_SINGLE) * 0.5f));

    // BOT_LEFT — effect list, left-aligned, anchored to bottom, grows upward
    slotBotLeft.UpdateRect(D2D1::RectF(
            M, H - M - ROW_EFFECTS,
            M + COL_LEFT_WIDTH, H - M));

    // BOT_CENTER — unused
    slotBotCenter.UpdateRect(D2D1::RectF(
            (W - COL_CENTER_WIDTH) * 0.5f, H - M - ROW_SINGLE,
            (W + COL_CENTER_WIDTH) * 0.5f, H - M));

    // BOT_RIGHT — dimensions / file size, right-aligned
    slotBotRight.UpdateRect(D2D1::RectF(
            W - COL_RIGHT_WIDTH - M, H - M - ROW_SINGLE,
            W - M, H - M));
}

// ─────────────────────────────────────────────────────────────────────────────
//  Content updates
// ─────────────────────────────────────────────────────────────────────────────

void OverlayManager::UpdateInfo(int index, int total, const std::wstring &filename) {
    std::wstring text =
            std::to_wstring(index + 1) + L" / " + std::to_wstring(total) +
            L"\n" + filename;
    slotTopLeft.UpdateText(std::move(text));
}

void OverlayManager::UpdateZoom(float zoom) {
    wchar_t buf[32];
    swprintf_s(buf, L"%.1f%%", zoom * 100.0f);
    slotTopRight.UpdateText(buf);
}

void OverlayManager::UpdateDims(int imgW, int imgH, int64_t fileSizeBytes) {
    std::wstring text =
            std::to_wstring(imgW) + L"\u00D7" + std::to_wstring(imgH) +
            L" / " + FormatFileSize(fileSizeBytes);
    slotBotRight.UpdateText(std::move(text));
}

void OverlayManager::UpdateEffects() {
    if (!app.hasActiveEffects) {
        slotBotLeft.UpdateText(L"");
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
        appendLine(L"Brightness: " + (app.brightness >= 0 ? std::wstring(L"+") : L"") + fmtFloat(app.brightness));
    if (std::abs(app.contrast - 1.0f) > EPS)
        appendLine(L"Contrast: " + fmtFloat(app.contrast));
    if (std::abs(app.saturation - 1.0f) > EPS)
        appendLine(L"Saturation: " + fmtFloat(app.saturation));
    if (std::abs(app.gamma - 1.0f) > EPS)
        appendLine(L"Gamma: " + fmtFloat(app.gamma));

    if (app.effectGrayscale) appendLine(L"Grayscale");
    if (app.effectInvert) appendLine(L"Invert");
    if (app.effectSepia) appendLine(L"Sepia");
    if (app.effectSolarize) appendLine(L"Solarize");
    if (app.effectOutline) appendLine(L"Outline");
    if (app.effectThreshold) appendLine(L"Threshold");

    slotBotLeft.UpdateText(std::move(lines));
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
}

void OverlayManager::SetAllVisible(bool show) {
    m_masterVisible = show;
    for (int i = 0; i < SLOT_COUNT; ++i)
        m_slots[i].overlay->active = show && m_slots[i].visible;
}

bool OverlayManager::IsSlotVisible(Slot slot) const {
    if (slot < 0 || slot >= SLOT_COUNT) return false;
    return m_slots[slot].visible;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Render
// ─────────────────────────────────────────────────────────────────────────────

void OverlayManager::RenderAll(ID2D1DeviceContext *ctx) const {
    if (!m_pTextBrush || !m_masterVisible) return;

    for (int i = 0; i < SLOT_COUNT; ++i) {
        const SlotMeta &meta = m_slots[i];
        if (!meta.visible || !meta.overlay || meta.overlay->text.empty()) continue;
        if (!meta.fmt) continue;

        const TextOverlay *ov = meta.overlay;

        IDWriteTextLayout *layout = const_cast<TextOverlay *>(ov)->GetLayout(
                m_pDWriteFactory, meta.fmt);
        if (!layout) continue;

        // Measure the actual ink rect so the background fits tightly
        DWRITE_TEXT_METRICS tm{};
        layout->GetMetrics(&tm);

        const D2D1_RECT_F &slotRect = ov->rect;
        const float slotW = slotRect.right - slotRect.left;

        float inkLeft;
        DWRITE_TEXT_ALIGNMENT ta = meta.fmt->GetTextAlignment();
        switch (ta) {
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

        D2D1_RECT_F bgRect = D2D1::RectF(
                inkLeft - BG_PADDING,
                slotRect.top - BG_PADDING,
                inkLeft + tm.width + BG_PADDING,
                slotRect.top + tm.height + BG_PADDING);

        bgRect.left = std::max(0.0f, bgRect.left);
        bgRect.top = std::max(0.0f, bgRect.top);
        bgRect.right = std::min(m_rtW, bgRect.right);
        bgRect.bottom = std::min(m_rtH, bgRect.bottom);

        // Draw semi-transparent background
        const D2D1_COLOR_F prevColor = m_pTextBrush->GetColor();
        m_pTextBrush->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, BG_ALPHA));
        ctx->FillRectangle(bgRect, m_pTextBrush);
        m_pTextBrush->SetColor(prevColor);

        // Draw text
        ctx->DrawTextLayout(
                D2D1::Point2F(slotRect.left, slotRect.top),
                layout,
                m_pTextBrush,
                D2D1_DRAW_TEXT_OPTIONS_NONE);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Device loss
// ─────────────────────────────────────────────────────────────────────────────

void OverlayManager::InvalidateLayouts() {
    for (int i = 0; i < SLOT_COUNT; ++i) {
        if (m_slots[i].overlay)
            m_slots[i].overlay->InvalidateLayout();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Free function bridge — forward-declared in AppState.h to avoid
//  circular includes. Called from AppState::WakeUpAndApplyEffects().
// ─────────────────────────────────────────────────────────────────────────────
void QIV_UpdateEffectsOverlay() {
    g_overlayManager.UpdateEffects();
}
