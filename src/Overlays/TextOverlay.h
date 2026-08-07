// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

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

            ForceEmojiFaceOnIcons();

            m_layout->GetMetrics(&m_cachedMetrics);
            m_layoutDirty = false;
            return m_layout.Get();
        }

        // Returns text metrics measured when the layout was last rebuilt.
        // Valid only after a successful GetLayout call.
        [[nodiscard]]
        const DWRITE_TEXT_METRICS &GetCachedMetrics() const { return m_cachedMetrics; }

        // Pin every emoji-marked run to the emoji face.
        //
        // WHY IT IS NEEDED. DirectWrite falls back to another font only for a
        // character the base font DOES NOT HAVE. Segoe UI carries U+2B05/2B06/2B07,
        // so the up, down and left arrows were drawn from it as plain arrows, while
        // U+27A1 — which it lacks — fell through to Segoe UI Emoji and came out as
        // an arrow inside a rounded rectangle. One boxed arrow and three bare ones
        // from a single string, and U+FE0F could not fix it on its own because no
        // fallback is consulted in the first place. Naming the family for the run
        // is what settles it.
        //
        // SCOPE IS FOUR CODEPOINTS, deliberately, and it must stay that way.
        //
        // Only the direction arrows are broken; every other icon in the app already
        // renders the way it is wanted. A general rule — "pin anything carrying
        // U+FE0F", or every surrogate pair — would sweep in ▶️ ℹ️ ⚙️ ⌨️ 🖱️ and the
        // supplementary-plane set as well, and any of those that DirectWrite is
        // currently drawing from the UI face would change appearance. Fixing four
        // arrows is not worth restyling icons nobody complained about, so this
        // touches the four and nothing else.
        //
        // Runs on layout REBUILD only — once per message, not once per frame.
        void ForceEmojiFaceOnIcons() {
            if (!m_layout || text.empty()) return;

            const wchar_t c = text[0];
            if (c != 0x2B05 && c != 0x2B06 && c != 0x2B07 && c != 0x27A1)
                return;   // every other message, and every other icon, untouched

            // The arrow plus its variation selector when one follows.
            const UINT32 len = (text.length() > 1 && text[1] == 0xFE0F) ? 2u : 1u;
            (void) m_layout->SetFontFamilyName(L"Segoe UI Emoji",
                                               DWRITE_TEXT_RANGE{0, len});
        }

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
