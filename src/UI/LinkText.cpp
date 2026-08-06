// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#include "LinkText.h"

#include <shlobj_core.h> // ILCreateFromPathW / SHOpenFolderAndSelectItems

namespace UI::Link {

void Reveal(const std::wstring &path) {
    if (path.empty()) return;

    // Checked here rather than trusted: a panel can be showing a path that was
    // valid when it was painted and deleted since, and SHOpenFolderAndSelectItems
    // on a missing file opens a window on the wrong folder rather than failing.
    const DWORD attr = GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return;

    if (PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(path.c_str())) {
        SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
        ILFree(pidl);
    }
}

bool CopyToClipboard(HWND owner, const std::wstring &text) {
    if (text.empty()) return false;
    if (!OpenClipboard(owner)) return false;

    bool ok = false;
    if (EmptyClipboard()) {
        const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        // GMEM_MOVEABLE: SetClipboardData takes ownership of a moveable block
        // and frees it itself. A GMEM_FIXED block would be leaked, and freeing
        // it here after a successful SetClipboardData is a double free.
        if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
            if (void *dst = GlobalLock(h)) {
                memcpy(dst, text.c_str(), bytes);
                GlobalUnlock(h);
                ok = SetClipboardData(CF_UNICODETEXT, h) != nullptr;
            }
            if (!ok) GlobalFree(h);   // ownership never transferred
        }
    }

    CloseClipboard();
    return ok;
}

} // namespace UI::Link
