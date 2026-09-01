// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#pragma once

// WHICH THUMBNAIL A CLICK LANDED ON.
//
// A row of equally sized boxes with a gap between them, and the question is
// which box - if any - contains a point. Pulled out of FindWnd because that is
// the whole of the decision, and inside a window procedure it is unreachable
// from a test: the arithmetic that decides whether a click selects the copy you
// are looking at or the one beside it is exactly the arithmetic worth proving.
//
// Header-only for the same reason - the test links it without dragging GDI in.
namespace Common {
    namespace PreviewStrip {

        // Returns the slot under (x, y), or -1 for a miss.
        //
        // `left` and `top` are the first box's top-left, `box` its side, `cell`
        // the distance to the next box's left edge - so cell - box is the gap,
        // and a point in that gap belongs to NEITHER neighbour. That last part
        // is the reason this exists: treating the gap as part of the box on its
        // left makes a click in visibly empty space select a picture, which
        // reads as the click landing somewhere it did not.
        inline int SlotAt(int x, int y, int left, int top, int box, int cell, int count) {
            if (box <= 0 || cell <= 0 || count <= 0) return -1;
            if (y < top || y >= top + box) return -1;
            if (x < left) return -1;

            const int dx = x - left;
            const int slot = dx / cell;
            if (slot >= count) return -1;
            if (dx % cell >= box) return -1; // the gap after that box
            return slot;
        }

    } // namespace PreviewStrip
} // namespace Common
