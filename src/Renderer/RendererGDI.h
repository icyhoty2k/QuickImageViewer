// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#pragma once

#include "IRenderer.h"
#include <string>

class RendererGDI final : public IImageRenderer {
    public:
        RendererGDI();

        ~RendererGDI() override;

        const wchar_t* GetName() const override { return L"GDI"; }
        [[nodiscard]] HRESULT Initialize(HWND hwnd) override;

        void Resize(UINT width, UINT height) override;

        [[nodiscard]] HRESULT LoadBitmap(
                IWICBitmapSource *bitmap,
                UINT width,
                UINT height,
                const std::wstring &filePath) override;

        [[nodiscard]] HRESULT Render() override;

        [[nodiscard]] HRESULT PreloadBitmap(const std::wstring &filePath, int requestIndex, int expectedCurrentIndex = -1) override;

        void ProcessPendingUploads() override;

        void UpdateColorEffects() override;
        void SetThemeFactor(float factor) override;

        void ApplyPreviousEffects() override {
            // GDI does not use the effect pipeline, so this is just an empty implementation.
        }

    private:
        void DestroyBackBuffer();

        [[nodiscard]] HRESULT CreateBackBuffer(UINT width, UINT height);

    private:
        HWND m_hwnd = nullptr;
        UINT m_windowWidth = 0;
        UINT m_windowHeight = 0;
        UINT m_imageWidth = 0;
        UINT m_imageHeight = 0;

        HDC m_backDC = nullptr;
        HBITMAP m_backBitmap = nullptr;
        HBITMAP m_backBitmapOld = nullptr;
        HBRUSH m_backgroundBrush = nullptr;
};
