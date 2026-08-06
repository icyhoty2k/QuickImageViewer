// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#pragma once
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <string>
#include <vector>

namespace SimpleFormats {
    bool IsSimpleFormat(const std::wstring& filePath);

    Microsoft::WRL::ComPtr<IWICBitmap> Decode(
        const std::wstring& filePath,
        const std::vector<BYTE>& fileBytes,
        IWICImagingFactory* wicFac,
        UINT& outWidth,
        UINT& outHeight);
}
