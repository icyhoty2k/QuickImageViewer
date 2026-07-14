#pragma once
#include <string>

namespace GeoNames {

struct Location {
    std::wstring city;
    std::wstring district;   // admin2 (county / borough)
    std::wstring state;      // admin1 (state / province / region)
    std::wstring country;
    std::wstring continent;
    std::wstring capital;
    std::wstring currency;   // e.g. "EUR (Euro)"
    std::wstring phone;      // e.g. "+33"
    std::wstring timezone;   // e.g. "Europe/Paris"
};

// Returns rich location info for the given GPS coordinates.
// Data is loaded and decompressed lazily on the first call.
// Safe to call from any thread.
Location Lookup(double lat, double lon);

// Triggers the one-time data load without doing a lookup.
// Call once from a background thread at startup to avoid a 100-500 ms stall
// on the first ExifWnd open that contains GPS coordinates.
void WarmUp();

} // namespace GeoNames
