// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#pragma once

#include <windows.h>
#include <string>
#include <vector>

// Reads raw SVG bytes from disk so RendererD2D can parse them
// with ID2D1DeviceContext5::CreateSvgDocument() on the UI thread.
// All the actual D2D SVG work happens in RendererD2D – this file
// is intentionally thin (just an IO helper).
class SvgDecoder {
    public:
        // Reads the entire file at filePath into outBytes.
        // Returns S_OK on success, HRESULT_FROM_WIN32 error otherwise.
        static HRESULT LoadFile(const std::wstring &filePath,
                                std::vector<BYTE> &outBytes);

        // Returns true when the extension (lowercased) is ".svg"
        static bool IsSvgPath(const std::wstring &filePath);
};
