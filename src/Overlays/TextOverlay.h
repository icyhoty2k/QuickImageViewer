#pragma once
#include <string>
#include <d2d1.h>
#include <dwrite_3.h>
#include <wrl/client.h>

// A single text overlay drawn on the main window.
// Created once at startup, updated when content changes, toggled via SetActive().
//
// IDWriteTextLayout is cached and only recreated when the text or bounding rect
// changes. DrawText() (the D2D convenience wrapper) internally allocates and
// destroys a layout on every call — this avoids that per-frame heap churn.
class TextOverlay {
    public:
        std::wstring text;
        D2D1_RECT_F rect = {};
        bool active = false;

        // Update content and position in one call.
        void Update(std::wstring newText, D2D1_RECT_F newRect) {
            if (newText != text) {
                text = std::move(newText);
                m_layoutDirty = true;
            }
            if (RectsAreDifferent(newRect, rect)) {
                rect = newRect;
                m_layoutDirty = true;
            }
        }

        // Update only the text, keep the existing rect.
        void UpdateText(std::wstring newText) {
            if (newText != text) {
                text = std::move(newText);
                m_layoutDirty = true;
            }
        }

        // Update only the rect (e.g. on window resize).
        void UpdateRect(D2D1_RECT_F newRect) {
            if (RectsAreDifferent(newRect, rect)) {
                rect = newRect;
                m_layoutDirty = true;
            }
        }

        // Returns the cached layout, (re)creating it if text or rect changed.
        // Returns nullptr if factory/format are null or CreateTextLayout fails.
        // Side effect: GetMetrics is called once on rebuild and cached.
        [[nodiscard]]
        IDWriteTextLayout *GetLayout(IDWriteFactory *factory, IDWriteTextFormat *format) {
            if (!m_layoutDirty && m_layout) return m_layout.Get();
            if (!factory || !format || text.empty()) return nullptr;

            m_layout.Reset();
            const float maxW = std::max(rect.right - rect.left, 1.0f);
            const float maxH = std::max(rect.bottom - rect.top, 1.0f);
            HRESULT hr = factory->CreateTextLayout(
                    text.c_str(),
                    static_cast<UINT32>(text.length()),
                    format,
                    maxW,
                    maxH,
                    &m_layout);

            if (FAILED(hr))
                return nullptr;

            m_layout->GetMetrics(&m_cachedMetrics);
            m_layoutDirty = false;
            return m_layout.Get();
        }

        // Returns text metrics measured when the layout was last rebuilt.
        // Valid only after a successful GetLayout call.
        [[nodiscard]]
        const DWRITE_TEXT_METRICS &GetCachedMetrics() const { return m_cachedMetrics; }

        // Call when the D2D/DWrite device is lost — layouts hold device resources.
        void InvalidateLayout() {
            m_layout.Reset();
            m_layoutDirty = true;
        }

    private:
        Microsoft::WRL::ComPtr<IDWriteTextLayout> m_layout;
        DWRITE_TEXT_METRICS m_cachedMetrics{};
        bool m_layoutDirty = true;

        static bool RectsAreDifferent(const D2D1_RECT_F &a, const D2D1_RECT_F &b) {
            return a.left != b.left || a.top != b.top ||
                   a.right != b.right || a.bottom != b.bottom;
        }
};
