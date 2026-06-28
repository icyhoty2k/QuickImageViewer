#pragma once

#include "IRenderer.h"
#include <d2d1.h>
#include <wrl/client.h>
#include <list>
#include <unordered_map>
#include <string>
#include <mutex>
#include <queue>
#include <vector> // Required for std::vector<BYTE>
#include <dwrite.h>
#include "../Platform/Constants.h"

class RendererD2D final : public IImageRenderer {
    public:
        RendererD2D() = default;

        ~RendererD2D() override = default;

        [[nodiscard]] HRESULT Initialize(HWND hwnd) override;

        void Resize(UINT width, UINT height) override;

        [[nodiscard]] HRESULT LoadBitmap(IWICBitmapSource *bitmap, UINT width, UINT height, const std::wstring &filePath) override;

        [[nodiscard]] HRESULT PreloadBitmap(const std::wstring &filePath, int requestIndex) override;

        [[nodiscard]] HRESULT Render() override;

        // UI THREAD: Processes the background-decoded images
        void ProcessPendingUploads() override;

        Microsoft::WRL::ComPtr<IDWriteFactory> m_pDWriteFactory;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> m_pTextFormat;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_pTextBrush;

    private:
        struct CachedBitmap {
            Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
            std::list<std::wstring>::iterator lruIt;
            UINT width = 0;
            UINT height = 0;
        };

        struct PendingUpload {
            int playlistIndex;
            std::wstring filePath;
            std::vector<BYTE> pixelData; // Fully replaces WIC Format Converter
            UINT stride; // Bytes per row
            UINT width;
            UINT height;
        };

        // Direct2D Resources
        Microsoft::WRL::ComPtr<ID2D1Factory> m_pFactory;
        Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> m_pRenderTarget;
        Microsoft::WRL::ComPtr<ID2D1Bitmap> m_pBitmap;

        // --- UPDATED SIGNATURES ---
        HRESULT UploadAndCacheBitmap(const std::vector<BYTE> &pixelData, UINT stride, const std::wstring &filePath, UINT width, UINT height);

        // Internal version — caller must already hold m_cacheMutex
        HRESULT UploadAndCacheBitmap_Locked(const std::vector<BYTE> &pixelData, UINT stride, const std::wstring &filePath, UINT width, UINT height);

        // --------------------------

        // Cache management
        std::unordered_map<std::wstring, CachedBitmap> m_bitmapCache;
        std::list<std::wstring> m_lruList;
        std::queue<PendingUpload> m_pendingUploads;

        std::mutex m_cacheMutex;

        HWND m_hwnd = nullptr;
        D2D1_COLOR_F m_clearColor = D2D1::ColorF(0.08f, 0.08f, 0.08f);
};
