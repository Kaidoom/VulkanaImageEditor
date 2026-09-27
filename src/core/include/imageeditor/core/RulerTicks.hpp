#pragma once

#include <vector>

namespace imageeditor::core {

struct RulerTick {
    double document {0};
    double logical {0};
    bool major {false};
};
struct RulerTicks {
    double majorStep {0};
    double minorStep {0};
    std::vector<RulerTick> ticks;
};

// All logical positions share the caller's viewport/workspace axis, not strip-
// local coordinates: logical = zeroLogical + document * zoom. Device pixel
// ratio deliberately is not an input. Ticks remain anchored at document zero.
// Major steps use 1/2/5 decades, roughly 80 logical pixels apart; minor marks
// are at least 8 logical pixels apart. Only the visible projected document is
// covered. Invalid input returns an empty result; at most 4096 ticks are made.
[[nodiscard]] RulerTicks makeRulerTicks(double zoom, double zeroLogical,
    double stripStartLogical, double stripLength, double documentLength);

} // namespace imageeditor::core
