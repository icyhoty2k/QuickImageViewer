// #include "OverlayText.h"
// #include <wrl/client.h>
// #include <algorithm>
//
// #pragma comment(lib, "dwrite.lib")
//
// namespace OverlayText {
//     // --- ENCAPSULATED TEXT RESOURCES ---
//     static Microsoft::WRL::ComPtr<IDWriteFactory> g_pDWriteFactory;
//     static Microsoft::WRL::ComPtr<IDWriteTextFormat> g_pNumberFormat;
//     static Microsoft::WRL::ComPtr<IDWriteTextFormat> g_pNameFormat;
//     static Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> g_pTextBrush;
//
//     // State trackers to detect configuration changes
//     static float s_lastNumSize = 0.0f;
//     static float s_lastNameSize = 0.0f;
//     static const wchar_t *s_lastNumFont = nullptr;
//     static const wchar_t *s_lastNameFont = nullptr;
//
//     HRESULT InitializeDWrite() {
//         if (g_pDWriteFactory) return S_OK;
//         return DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
//                                    reinterpret_cast<IUnknown **>(g_pDWriteFactory.GetAddressOf()));
//     }
//
//     HRESULT CreateDeviceResources(ID2D1RenderTarget *pRT) {
//         if (!pRT) return E_POINTER;
//         return pRT->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &g_pTextBrush);
//     }
//
//     void DiscardDeviceResources() {
//         g_pTextBrush.Reset();
//     }
//
//     static void EnsureFormat(Microsoft::WRL::ComPtr<IDWriteTextFormat> &format, const wchar_t *font, float size) {
//         if (!g_pDWriteFactory) return;
//         format.Reset();
//         HRESULT hr = g_pDWriteFactory->CreateTextFormat(
//                 font, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
//                 DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &format);
//
//         if (FAILED(hr)) {
//             g_pDWriteFactory->CreateTextFormat(
//                     FallbackFont, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
//                     DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &format);
//         }
//     }
//
//     void DrawD2D(ID2D1RenderTarget *pRT, const std::wstring &path, int idx, size_t total, float w, float h) {
//         if (!pRT || !g_pTextBrush) return;
//
//         // Auto-rebuild formats if config changed
//         if (!g_pNumberFormat || s_lastNumSize != NumberFontSize || s_lastNumFont != NumberFontFamily) {
//             EnsureFormat(g_pNumberFormat, NumberFontFamily, NumberFontSize);
//             s_lastNumSize = NumberFontSize;
//             s_lastNumFont = NumberFontFamily;
//         }
//         if (!g_pNameFormat || s_lastNameSize != NameFontSize || s_lastNameFont != NameFontFamily) {
//             EnsureFormat(g_pNameFormat, NameFontFamily, NameFontSize);
//             s_lastNameSize = NameFontSize;
//             s_lastNameFont = NameFontFamily;
//         }
//
//         g_pTextBrush->SetColor(D2D1::ColorF(TextColorR / 255.0f, TextColorG / 255.0f, TextColorB / 255.0f));
//
//         float currentX = 15.0f;
//
//         if (ShowImageNumber) {
//             std::wstring numText = std::to_wstring(idx + 1) + CounterSeparator + std::to_wstring(total);
//             D2D1_RECT_F rect = D2D1::RectF(currentX, 6.0f, w, h);
//             pRT->DrawText(numText.c_str(), (UINT32) numText.length(), g_pNumberFormat.Get(), rect, g_pTextBrush.Get());
//             currentX += (numText.length() * (NumberFontSize * 0.6f)); // Approximate advance
//         }
//
//         if (ShowImageName) {
//             std::wstring nameText = (ShowImageNumber ? NameSeparator : L"") + path.substr(path.find_last_of(L"\\/") + 1);
//             D2D1_RECT_F rect = D2D1::RectF(currentX, 6.0f, w, h);
//             pRT->DrawText(nameText.c_str(), (UINT32) nameText.length(), g_pNameFormat.Get(), rect, g_pTextBrush.Get());
//         }
//     }
//
//     void DrawGDI(HDC hdc, const std::wstring &path, int idx, size_t total, int w, int h) {
//         if (!hdc) return;
//
//         // GDI handles font selection directly; we use cached HFONT for performance
//         static HFONT s_hNumFont = nullptr;
//         static HFONT s_hNameFont = nullptr;
//         // Logic to recreate HFONTs if NumberFontSize/NameFontSize change would go here...
//
//         SetTextColor(hdc, RGB(TextColorR, TextColorG, TextColorB));
//         SetBkMode(hdc, TRANSPARENT);
//
//         std::wstring output = L"";
//         if (ShowImageNumber) output += std::to_wstring(idx + 1) + CounterSeparator + std::to_wstring(total);
//         if (ShowImageName) output += (ShowImageNumber ? NameSeparator : L"") + path.substr(path.find_last_of(L"\\/") + 1);
//
//         RECT rect = {0, h - 35, w - 10, h - 5};
//         DrawTextW(hdc, output.c_str(), -1, &rect, DT_SINGLELINE | DT_RIGHT | DT_VCENTER);
//     }
// }
