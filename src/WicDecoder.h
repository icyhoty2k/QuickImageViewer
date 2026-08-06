// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#pragma once

#include <wincodec.h>
#include <wrl/client.h>
#include <string>

using Microsoft::WRL::ComPtr;

struct DecodedImage {
    ComPtr<IWICBitmap> bitmap;
    UINT width = 0;
    UINT height = 0;
};

class WicDecoder {
    public:
        static HRESULT DecodeImage(
                const std::wstring &filePath,
                DecodedImage &result
                );
};
