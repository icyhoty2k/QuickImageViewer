#pragma once
#include <string>
#include <d2d1.h>

// A single text overlay drawn on the main window.
// Created once at startup, updated when content changes, toggled via SetActive().
class TextOverlay {
    public:
        std::wstring text;
        D2D1_RECT_F rect = {};
        bool active = false;

        // Update content and position in one call.
        // Call this whenever the displayed value changes (image navigation, zoom, etc.)
        void Update(std::wstring newText, D2D1_RECT_F newRect) {
            text = std::move(newText);
            rect = newRect;
        }

        // Update only the text, keep the existing rect.
        // Use when the overlay position never changes (e.g. fixed-corner overlays).
        void UpdateText(std::wstring newText) {
            text = std::move(newText);
        }

        // Update only the rect (e.g. on window resize).
        void UpdateRect(D2D1_RECT_F newRect) {
            rect = newRect;
        }
};
