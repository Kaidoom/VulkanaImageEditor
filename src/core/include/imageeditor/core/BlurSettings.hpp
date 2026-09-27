#pragma once

namespace imageeditor::core {

// Local Blur is editor/tool state, not brush preset or persistent layer data.
// Radius is finite Gaussian support in DOCUMENT pixels (sigma=radius/3).
// Strength is the accumulated brush influence ceiling, independent of Flow.
struct BlurSettings {
    double strength {0.5};
    double radius {8.0};
    bool operator==(const BlurSettings&) const = default;
};

}
