#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <string>
#include <vector>

namespace OverlayText {
    // --- Font Settings ---
    inline const wchar_t *FallbackFont = L"Arial"; // Used if primary font load fails
    // Defines how a MasterBox flows its children
    struct TextBox {
        // --- Identity ---
        std::string name;
        std::string parentName;

        // --- Content ---
        std::wstring text;
        float x = 0.0f; // Add this
        float y = 0.0f; // Add this
        float width = 0.0f; // New: Width of the box
        float height = 0.0f; // New: Height of the box

        // =========================================================================
        // --- CONTAINER & LAYOUT CONFIGURATION ---
        // All measurements are in pixels unless otherwise specified.
        // =========================================================================
        BYTE BgColorR = 0; // Background RGB (0-255)
        BYTE BgColorG = 0;
        BYTE BgColorB = 0;
        float BgOpacity = 0.5f; // 0.0f-1.0f
        int orientation = 1; // 0=Vertical, 1=Horizontal
        bool isVisibleBackground = true; //visibility
        float MarginLeft = 15.0f;
        float MarginTop = 6.0f;
        float MarginRight = 10.0f;
        float MarginBottom = 10.0f;
        float paddingLeft = 15.0f;
        float paddingTop = 6.0f;
        float paddingRight = 10.0f;
        float paddingBottom = 10.0f;
        // =========================================================================
        // --- TEXT STYLING CONFIGURATION ---
        // RGB values are clamped to 0-255. TextOpacity is clamped to 0.0f-1.0f.
        // =========================================================================
        // --- Font Styling ---
        float fontSize = 14.0f;
        const wchar_t *fontFamily = L"Segoe UI";
        int weight = 400; // 100-900
        int style = 0; // 0=Normal, 1=Italic, 2=Oblique

        // --- Text Color & Opacity ---
        BYTE TextColorR = 0;
        BYTE TextColorG = 255;
        BYTE TextColorB = 0;
        float TextOpacity = 1.0f; // 0.0f = Transparent, 1.0f = Opaque
        bool isVisibleText = true; //visibility
    };


    class OverlayManager {
        private:
            std::vector<OverlayText::TextBox> m_boxes;

        public:
            // 1. Logic: Calculate the width of a text string
            float GetTextWidth(const std::wstring &text, float fontSize) {
                return (float) text.length() * (fontSize * 0.6f);
            }

            float GetTextHeight(float fontSize, float paddingTop, float paddingBottom) {
                // Standard UI height = font size + top and bottom padding
                return fontSize + paddingTop + paddingBottom;
            }

            // 2. Logic: Calculate the width of a Vertical Stack
            float GetStackWidth(const std::string &parentName) {
                float maxWidth = 0.0f;
                for (auto &child: m_boxes) {
                    if (child.parentName == parentName) {
                        float w = GetTextWidth(child.text, child.fontSize);
                        if (w > maxWidth) maxWidth = w;
                    }
                }
                return maxWidth;
            }

            // Logic: Calculate the total height of a Vertical Stack
            float GetStackHeight(const std::string &parentName) {
                float totalHeight = 0.0f;
                for (auto &child: m_boxes) {
                    if (child.parentName == parentName) {
                        // Sum the height of each child based on its font and padding
                        totalHeight += child.fontSize + child.paddingTop + child.paddingBottom;
                    }
                }
                return totalHeight;
            }

            // 3. Logic: The Layout Engine (The "Glue" pass)
            void ApplyLayout() {
                for (auto &master: m_boxes) {
                    if (master.parentName.empty()) { // Root Master
                        float currentX = master.MarginLeft;
                        float currentY = master.MarginTop;

                        for (auto &child: m_boxes) {
                            if (child.parentName == master.name) {
                                // 1. Calculate dimensions (check if it's a nested stack or a single box)
                                if (child.orientation == 0) { // It's a Vertical Stack container
                                    child.width = GetStackWidth(child.name) + child.paddingLeft + child.paddingRight;
                                    child.height = GetStackHeight(child.name);
                                } else { // It's a single text box
                                    child.width = GetTextWidth(child.text, child.fontSize) + child.paddingLeft + child.paddingRight;
                                    child.height = GetTextHeight(child.fontSize, child.paddingTop, child.paddingBottom);
                                }

                                // 2. Set position
                                child.x = currentX;
                                child.y = currentY;

                                // 3. Move cursor based on MASTER orientation
                                if (master.orientation == 1) { // Master is Horizontal
                                    currentX += child.width;
                                } else { // Master is Vertical
                                    currentY += child.height;
                                }
                            }
                        }
                    }
                }
            }

            // 4. State Management
            void AddBox(const OverlayText::TextBox &box) {
                auto it = std::find_if(m_boxes.begin(), m_boxes.end(),
                                       [&box](const OverlayText::TextBox &b) {
                                           return b.name == box.name;
                                       });
                if (it != m_boxes.end()) *it = box;
                else m_boxes.push_back(box);
            }

            void RemoveBoxAndChildren(const std::string &name) {
                m_boxes.erase(
                        std::remove_if(m_boxes.begin(), m_boxes.end(),
                                       [&name](const OverlayText::TextBox &b) {
                                           return (b.name == name) || (b.parentName == name);
                                       }),
                        m_boxes.end()
                        );
            }

            void Clear() {
                m_boxes.clear();
            }

            // Accessor for the renderer
            const std::vector<OverlayText::TextBox> &GetBoxes() const {
                return m_boxes;
            }
    };


    // =========================================================================
    // --- CORE API ---
    // =========================================================================
    HRESULT InitializeDWrite();

    HRESULT CreateDeviceResources(ID2D1RenderTarget *pRT);

    void DiscardDeviceResources();

    void DrawD2D(ID2D1RenderTarget *pRenderTarget, const std::wstring &currentPath,
                 int currentIndex, size_t totalCount, float rtWidth, float rtHeight);

    void DrawGDI(HDC hdc, const std::wstring &currentPath,
                 int currentIndex, size_t totalCount, int windowWidth, int windowHeight);
}
