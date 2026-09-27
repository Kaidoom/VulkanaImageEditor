#pragma once

#include "imageeditor/core/Measurement.hpp"
#include <array>

namespace imageeditor::core {

struct SnapOptions {
    bool enabled {true}, canvas {true}, layers {true};
    // Qt logical pixels per document pixel, not framebuffer/device pixels.
    double logicalScale {1};
    bool bypass {false};
    bool allowX {true}, allowY {true};
};
struct SnapGuide {
    Vec2d a, b;
    LayerId target {0}; // Zero identifies the canvas.
    friend bool operator==(const SnapGuide&, const SnapGuide&) = default;
};
using SnapGuides = std::array<std::optional<SnapGuide>, 2>;
struct SnapResult {
    Vec2d translation;
    SnapGuides guides;
};
struct HandleSnapResult { Vec2d position; SnapGuides guides; };

// One gesture, in document coordinates. Reads geometry only; never creates a
// layout, raster cache, image snapshot or history entry. Raw translation always
// comes from the original press, never from the previous snapped position.
class BoundsSnapping final {
public:
    static constexpr double capturePixels = 6, releasePixels = 9;
    bool begin(const Document&, std::span<const LayerId> moving);
    bool begin(const Document&, DocumentBounds movingBounds, std::span<const LayerId> excluded);
    [[nodiscard]] SnapResult resolve(const Document&, Vec2d rawTranslation, const SnapOptions&);
    // A resize edge has one degree of freedom. Attract its handle along that
    // line only, to any canvas/layer edge or center; never invent a second axis.
    [[nodiscard]] HandleSnapResult resolveHandle(const Document&, Vec2d position, Vec2d direction, const SnapOptions&);
    // Call only after the owner's own translation; other document revisions
    // invalidate cached targets on the next evaluation.
    void acknowledgeTranslation(Revision revision) noexcept { revision_ = revision; }
    void clear() noexcept;
    [[nodiscard]] std::size_t targetBuildCount() const noexcept { return targetBuildCount_; }

private:
    struct Anchor {
        double coordinate;
        unsigned kind; // 0=min edge, 1=center, 2=max edge.
        LayerId id;
        DocumentBounds bounds;
    };
    struct Alignment { std::size_t target; unsigned movingAnchor; };
    void rebuild(const Document&);
    std::optional<DocumentBounds> start_;
    std::vector<LayerId> moving_;
    std::array<std::vector<Anchor>, 2> anchors_;
    std::array<std::optional<Alignment>, 2> active_;
    Revision revision_ {0};
    std::size_t targetBuildCount_ {0};
};
} // namespace imageeditor::core
