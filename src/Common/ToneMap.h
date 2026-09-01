// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#pragma once
#include <cmath>

// HIGH DYNAMIC RANGE TO ONE DISPLAY BYTE.
//
// Reinhard, then gamma 2.2. Pulled out of the EXR decoder because the ORDER of
// the guards is the whole correctness argument, and inside a per-pixel loop it
// was unreachable from a test.
//
// ⚠ THE ORIGINAL CLAMPED NEGATIVES AFTER THE TONE-MAP, WHICH IS TOO LATE.
// It read, in effect:
//
//     v = v / (1 + v);            // Reinhard
//     v = powf(v < 0 ? 0 : v, 1/2.2f);
//
// and OpenEXR is a float format that legitimately carries negatives, infinities
// and NaN - render passes produce all three routinely. Each one broke it
// differently, and all of them ended in a cast to BYTE that is undefined
// behaviour when the value does not fit:
//
//     v = -2   ->  -2 / (1 + -2)  =  +2      positive, so the v<0 guard misses
//                  it entirely; 2^(1/2.2) * 255 + 0.5 = 348, cast to BYTE.
//     v = -1   ->  division by zero.
//     v = +inf ->  inf / inf = NaN, and NaN < 0 is FALSE, so the guard misses
//                  that too.
//     v = NaN  ->  propagates to the cast.
//
// So a single stray pixel in a render pass could paint a bright wrong colour,
// or invoke UB, in a viewer whose whole claim is that it opens these files.
namespace Common::ToneMap {

    // Returns 0..255 for any float whatsoever, finite or not.
    //
    // ⚠ WRITTEN AS !(v > 0) RATHER THAN v <= 0, and that is deliberate: every
    // comparison against NaN is false, so !(NaN > 0) is TRUE and NaN lands on
    // the black branch instead of falling through to the arithmetic. `v <= 0`
    // would be false for NaN and let it through - which is exactly how the
    // original guard failed.
    inline unsigned char HdrToByte(float v) {
        if (!(v > 0.0f)) return 0;              // negatives, zero, and NaN
        if (!std::isfinite(v)) return 255;      // +inf saturates to white

        // Only now is the Reinhard safe: v is finite and strictly positive, so
        // the denominator is greater than 1 and the result lands in (0, 1).
        const float mapped = v / (1.0f + v);
        const float gamma  = std::pow(mapped, 1.0f / 2.2f);

        // gamma is in (0, 1), so this cannot exceed 255 - but the clamp stays,
        // because "cannot" here rests on floating point behaving exactly as
        // reasoned, and the cost of being wrong is undefined behaviour.
        const float scaled = gamma * 255.0f + 0.5f;
        if (scaled <= 0.0f)   return 0;
        if (scaled >= 255.0f) return 255;
        return static_cast<unsigned char>(scaled);
    }

    // The same treatment for a straight 0..1 channel, such as EXR's alpha,
    // which is also a float and also free to arrive out of range or as NaN.
    inline unsigned char UnitToByte(float v) {
        if (!(v > 0.0f)) return 0;
        if (!std::isfinite(v)) return 255;
        const float scaled = v * 255.0f + 0.5f;
        if (scaled >= 255.0f) return 255;
        return static_cast<unsigned char>(scaled);
    }

} // namespace Common::ToneMap
