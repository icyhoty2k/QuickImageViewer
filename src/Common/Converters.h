#pragma once
#include <string>

namespace Converters {
    // ---- Zoom units -------------------------------------------------------
    // Two units exist and must never be mixed:
    //   RATIO   — 1.0 is 1:1. app.viewport.zoom and every renderer scale.
    //   PERCENT — 100.0 is 1:1. What the user reads and types, and the unit of
    //             Constants::ZoomPanel::ZOOM_MIN / ZOOM_MAX.
    // These four functions are the ONLY places the factor of 100 appears.
    float PercentToRatio(float percent);
    float RatioToPercent(float ratio);

    // Integer percent — for STORAGE only (registry, tray menu, zoom-click dialog).
    // Truncates: everything below 0.5% collapses to 0. Never use it for display.
    int toZoomInt(float zoom);
    float toZoomFloat(int stored);

    // A percent VALUE with no "%" suffix and no fixed precision: trailing zeros
    // are trimmed, so 99999 -> "99999", 0.1 -> "0.1", 0.01 -> "0.01".
    // Use for printing the ZOOM_MIN / ZOOM_MAX bounds in labels — a hardcoded
    // "%.1f" turns any limit below 0.05 into a misleading "0.0".
    std::wstring FormatPercentCompact(float percent);

    // Display percent, "%"-suffixed: "150%", "0.35%", "<0.01%".
    // The single source of truth for every zoom readout — overlay slots and the
    // zoom panel's applied-value message must agree character for character, or
    // typing back the number the overlay shows stops being a no-op.
    std::wstring FormatZoomPercent(float zoom);
}
