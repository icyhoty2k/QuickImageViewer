#include "Converters.h"

namespace Converters {
    int toZoomInt(float zoom) {
        return static_cast<int>(zoom * 100.0f + 0.5f);
    }

    float toZoomFloat(int stored) {
        return stored / 100.0f;
    }
}
