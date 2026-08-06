// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#include "Converters.h"
#include <cstdio>   // swprintf_s

namespace Converters {
    // The one and only ratio<->percent factor in the codebase. Everything that
    // crosses between the two units goes through the helpers below.
    namespace {
        constexpr float PERCENT_PER_UNIT = 100.0f;
    }

    float PercentToRatio(float percent) {
        return percent / PERCENT_PER_UNIT;
    }

    float RatioToPercent(float ratio) {
        return ratio * PERCENT_PER_UNIT;
    }

    int toZoomInt(float zoom) {
        return static_cast<int>(RatioToPercent(zoom) + 0.5f);
    }

    float toZoomFloat(int stored) {
        return PercentToRatio(static_cast<float>(stored));
    }

    std::wstring FormatPercentCompact(float percent) {
        // Six decimals is enough for any sane zoom bound; the trim below removes
        // whatever the value did not need, so one format serves 99999 and 0.01
        // alike and no caller has to guess a precision.
        wchar_t buf[32];
        swprintf_s(buf, L"%.6f", percent);
        std::wstring s = buf;
        if (s.find(L'.') != std::wstring::npos) {
            while (!s.empty() && s.back() == L'0') s.pop_back();
            if (!s.empty() && s.back() == L'.') s.pop_back();
        }
        return s;
    }

    std::wstring FormatZoomPercent(float zoom) {
        const float percent = RatioToPercent(zoom);
        wchar_t buf[32];

        // Guard against a negative or non-finite zoom reaching the overlay —
        // swprintf_s would happily print "-nan%" into the top-right slot.
        if (!(percent > 0.0f))
            return L"0%";

        if (percent >= 1.0f) {
            // Normal range: whole percents, matching the old readout exactly.
            swprintf_s(buf, L"%d%%", static_cast<int>(percent + 0.5f));
        } else if (percent >= 0.01f) {
            // 0.01%–1%: two decimals, otherwise the whole band reads "0%".
            // Reachable from ZOOM_MIN (0.1%) and from FitToView on a very large
            // image in a small window.
            swprintf_s(buf, L"%.2f%%", percent);
        } else {
            // Below 0.01% two decimals also round to "0.00" — say so instead of
            // printing a value that looks like zoom is off.
            return L"<0.01%";
        }
        return buf;
    }
}
