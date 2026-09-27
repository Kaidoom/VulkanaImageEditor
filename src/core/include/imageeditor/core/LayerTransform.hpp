#pragma once
#include "imageeditor/core/Document.hpp"
#include "imageeditor/core/History.hpp"
#include "imageeditor/core/BoundsSnapping.hpp"
#include <array>
#include <limits>

namespace imageeditor::core {

// Corner/edge order is clockwise in layer-local space, independent of flips.
enum class TransformHandle : int {
    None = -1,
    TopLeft,
    Top,
    TopRight,
    Right,
    BottomRight,
    Bottom,
    BottomLeft,
    Left,
    Move,
    Rotate
};

struct TransformModifiers {
    bool shift { false };
    bool alt { false };
    bool control { false };
};
struct TransformValues {
    Vec2d center;
    double scaleX { 1.0 };
    double scaleY { 1.0 };
    double rotationDegrees { 0.0 };
    // Preserved QR shear from imported affine data; not a V1 editing control.
    double shear { 0.0 };
    // Residual mapping in the original local frame. Numeric scale/rotation
    // describe the Jacobian at the frame center, and compose with this shape.
    ProjectiveTransform distortion {};
};

inline constexpr double kMinimumLayerScale = 1.0e-6;
inline constexpr double kMaximumLayerScale = 1000.0;
[[nodiscard]] AffineTransform transformFromValues(const TransformValues&, Extent2u);
[[nodiscard]] std::optional<TransformValues> valuesFromTransform(const AffineTransform&, Extent2u);
[[nodiscard]] AffineTransform transformFromGeometryValues(const TransformValues&, Extent2d);
[[nodiscard]] std::optional<TransformValues> geometryValuesFromTransform(const AffineTransform&, Extent2d);
// Corners plus geometric midpoints of the projected edges, matching the visible
// overlay even when perspective does not preserve the local edge midpoints.
[[nodiscard]] std::array<Vec2d, 8> transformHandles(const AffineTransform&, Extent2u);
[[nodiscard]] std::array<Vec2d, 8> geometryTransformHandles(const AffineTransform&, Extent2d);

// Shared resize math, before layer-scale policy or geometry application.
// Signed extents can cross zero without requiring an inverse of the result.
struct ResizedGeometryBox {
    Extent2d signedExtent;
    Vec2d unitAnchor;
};
[[nodiscard]] std::optional<ResizedGeometryBox> resolveResizeGeometry(Extent2d,
    const AffineTransform& start, TransformHandle, Vec2d press, Vec2d point,
    TransformModifiers, bool aspectLocked);
// Input handles and point are Qt-logical viewport pixels; never document pixels.
[[nodiscard]] TransformHandle hitTestTransform(const std::array<Vec2d, 8>& handles, Vec2d point,
    double handleRadius = 9.0, double rotationRadius = 25.0);

enum class TransformCommitResult { Committed,
    NoChange,
    TargetUnavailable };

// Pure geometry for both layer and selection gizmos. No document, pixels,
// commands or UI types; the caller owns preview/commit/cancel semantics.
class TransformDrag final {
public:
    TransformDrag(Extent2u extent, AffineTransform start, TransformValues values,
        TransformHandle handle, Vec2d press, std::optional<Extent2d> geometryExtent = { });
    [[nodiscard]] std::optional<TransformValues> resolve(Vec2d point,
        TransformModifiers modifiers, bool aspectLocked, const TransformValues& current);
    [[nodiscard]] SnapGuides snapEdge(const Document&, BoundsSnapping&, TransformValues& resolved,
        TransformModifiers, bool aspectLocked, SnapOptions);

private:
    Extent2d extent_;
    AffineTransform start_;
    TransformValues values_;
    TransformHandle handle_;
    Vec2d press_;
    double previousAngle_ { 0 }, accumulatedAngle_ { 0 };
};

struct LayerTransformTarget {
    LayerId id;
    SurfaceId surface;
    Extent2u extent;
    std::size_t payloadKind;
    AffineTransform original, current;
};

class LayerTransformSession final {
public:
    LayerTransformSession(Document& document, LayerId layer);
    LayerTransformSession(Document& document, std::span<const LayerId> layers);
    ~LayerTransformSession();
    LayerTransformSession(const LayerTransformSession&) = delete;
    LayerTransformSession& operator=(const LayerTransformSession&) = delete;
    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] bool targetAvailable() const noexcept;
    [[nodiscard]] LayerId layerId() const noexcept { return layerId_; }
    [[nodiscard]] Extent2u extent() const noexcept { return extent_; }
    [[nodiscard]] Extent2d geometryExtent() const noexcept { return geometryExtent_; }
    [[nodiscard]] bool grouped() const noexcept { return targets_.size() > 1; }
    [[nodiscard]] const TransformValues& values() const noexcept { return values_; }
    [[nodiscard]] const AffineTransform& transform() const noexcept { return current_; }
    [[nodiscard]] bool dragging() const noexcept { return handle_ != TransformHandle::None; }
    bool setValues(TransformValues values);
    bool flip(bool horizontal);
    bool beginDrag(TransformHandle handle, Vec2d documentPosition);
    bool dragTo(Vec2d documentPosition, TransformModifiers modifiers, bool aspectLocked,
        SnapOptions snapping = {.enabled = false});
    [[nodiscard]] const SnapGuides& snapGuides() const noexcept { return snapGuides_; }
    void endDrag();
    void cancelDrag();
    bool completeAction();
    bool undo();
    bool redo();
    [[nodiscard]] const History& pendingHistory() const noexcept { return pendingHistory_; }
    TransformCommitResult commit(History& history);
    void cancel();

private:
    bool setFrame(const AffineTransform& frame);
    bool restoreSurvivingTargets();
    void synchronizeTargets();
    Document* document_;
    LayerId layerId_ { 0 };
    std::uint64_t entryContentState_ { 0 };
    Extent2u extent_;
    Extent2d geometryExtent_;
    std::vector<LayerTransformTarget> targets_;
    struct ContainerMembership { LayerId id; ContainerKind kind; std::vector<LayerId> children; };
    std::vector<ContainerMembership> memberships_;
    std::vector<LayerTransformUpdate> updates_;
    bool active_ { false };
    bool reframed_ {false}; // A crop frame has an offset from the stable source origin.
    AffineTransform original_, current_, dragStart_, actionStart_;
    TransformValues values_, dragValues_, actionStartValues_;
    // Pending actions must not be evicted before whole-session cancellation.
    History pendingHistory_ { std::numeric_limits<std::size_t>::max() };
    // Preserve the user's signed-scale/angle representation across local travel.
    std::vector<TransformValues> actionValues_;
    std::vector<AffineTransform> actionFrames_;
    TransformHandle handle_ { TransformHandle::None };
    std::optional<TransformDrag> drag_;
    BoundsSnapping snapping_;
    Vec2d dragPress_;
    bool snapMovementStarted_ {false};
    SnapGuides snapGuides_;
};
} // namespace imageeditor::core
