// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#pragma once
#include <windows.h>
#include <algorithm>
#include <string>

#include "Platform/Constants.h"

// =============================================================================
// LinkText — a clickable path drawn inline in a panel's own painting.
//
// Every panel that names a file wants the same thing: the path in link colour,
// underlined, brighter under the cursor, opening Explorer with the file selected
// when clicked. F9 grew that first; F10 and F8 need it identically, and three
// hand-rolled copies is three chances for the hit box to drift from the text.
//
// NOT a control. There is no window and no message handling — the panels are
// owner-drawn and adding a child HWND per path would mean z-order, focus and
// theming problems for one line of text. Each panel keeps its own hot flag and
// hit rect (they already track hover for rows and buttons) and calls these.
//
// THE HIT BOX IS THE MEASURED TEXT, always. Draw returns the rect it painted
// into, so a caller cannot position the box independently of the glyphs and let
// the two disagree when the wording changes.
// =============================================================================

namespace UI::Link {

    // Lightens a colour per channel, for the hover state. Explicit masking
    // rather than GetRValue/GetGValue/GetBValue — those return BYTE, so the
    // addition would overflow before the clamp could see it.
    inline COLORREF Brighten(COLORREF c, int amount) {
        const int r = std::min(255, static_cast<int>(c & 0xFF) + amount);
        const int g = std::min(255, static_cast<int>((c >> 8) & 0xFF) + amount);
        const int b = std::min(255, static_cast<int>((c >> 16) & 0xFF) + amount);
        return RGB(r, g, b);
    }

    // Draws `text` as a link starting at (x, y), clipped so it never paints past
    // `maxRight`. Returns the hit box — empty when nothing was drawn, which is
    // the caller's cue to clear its stored rect rather than leave a stale one
    // that still answers clicks.
    //
    // Selects `linkFont` into `dc` and does NOT restore it: these panels select
    // a font per drawing step anyway, and restoring here would be a promise the
    // surrounding code does not keep in either direction.
    inline RECT Draw(HDC dc, HFONT linkFont, int x, int y, int maxRight,
                     const std::wstring &text, bool hot, float dpiScale) {
        if (text.empty() || x >= maxRight) return RECT{};

        SelectObject(dc, linkFont);

        SIZE sz{};
        GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &sz);

        const int w = std::min<int>(sz.cx, maxRight - x);
        if (w <= 0) return RECT{};

        RECT r{x, y, x + w, y + static_cast<int>(18 * dpiScale)};
        SetTextColor(dc, hot ? Brighten(Constants::Links::COLOR, 50)
                             : Constants::Links::COLOR);
        RECT draw = r;
        // PATH_ELLIPSIS so a long path loses its MIDDLE — the file name at the
        // end is the part worth keeping, and END_ELLIPSIS would eat exactly that.
        DrawTextW(dc, text.c_str(), -1, &draw, DT_LEFT | DT_SINGLELINE | DT_PATH_ELLIPSIS);
        return r;
    }

    // Width of `text` in the panel's ordinary font, for placing a link after a
    // label without hardcoding an offset.
    inline int MeasureIn(HDC dc, HFONT font, const std::wstring &text) {
        if (text.empty()) return 0;
        SelectObject(dc, font);
        SIZE sz{};
        GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &sz);
        return sz.cx;
    }

    // Opens Explorer with the file selected. No-op for an empty or missing path,
    // so a stale link cannot open a window on nothing.
    void Reveal(const std::wstring &path);

    // Puts `text` on the clipboard as Unicode. False when the clipboard could
    // not be opened — another process can hold it, and that is a normal
    // transient failure worth reporting rather than swallowing.
    //
    // `owner` is the window that takes clipboard ownership; passing the panel
    // means the data outlives the click but dies with the application, which is
    // the right lifetime for a value copied to paste somewhere immediately.
    bool CopyToClipboard(HWND owner, const std::wstring &text);

} // namespace UI::Link
