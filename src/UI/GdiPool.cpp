// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#include "GdiPool.h"

#include <unordered_map>

namespace UI::Gdi {

namespace {
    // Colour → handle. Flat maps rather than anything cleverer: the population
    // is a dozen or so entries drawn from the theme, and a lookup happens a few
    // times per repaint, not per pixel.
    std::unordered_map<COLORREF, HBRUSH> g_brushes;

    // Pens are keyed by colour AND width — a 1px and a 2px pen of the same
    // colour are different objects, and keying on colour alone would hand back
    // the wrong thickness to whichever caller asked second.
    std::unordered_map<unsigned long long, HPEN> g_pens;

    unsigned long long PenKey(COLORREF c, int width) {
        return (static_cast<unsigned long long>(static_cast<unsigned>(width)) << 32) |
               static_cast<unsigned long long>(c);
    }
}

HBRUSH Brush(COLORREF colour) {
    auto it = g_brushes.find(colour);
    if (it != g_brushes.end()) return it->second;

    HBRUSH b = CreateSolidBrush(colour);
    // A failed creation is not cached: caching a null would make the failure
    // permanent for that colour, where retrying next paint may well succeed
    // (GDI exhaustion is usually transient). The caller gets null and Win32
    // treats a null brush as "do not fill", which degrades to a missing
    // background rather than a crash.
    if (b) g_brushes.emplace(colour, b);
    return b;
}

HPEN Pen(COLORREF colour, int width) {
    const unsigned long long key = PenKey(colour, width);
    auto it = g_pens.find(key);
    if (it != g_pens.end()) return it->second;

    HPEN p = CreatePen(PS_SOLID, width, colour);
    if (p) g_pens.emplace(key, p);
    return p;
}

void Flush() {
    for (auto &kv : g_brushes) DeleteObject(kv.second);
    for (auto &kv : g_pens)    DeleteObject(kv.second);
    g_brushes.clear();
    g_pens.clear();
}

} // namespace UI::Gdi
