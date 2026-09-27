#pragma once

#include "imageeditor/core/BrushEngine.hpp"
#include "imageeditor/core/BrushLineConstraint.hpp"

#include <optional>

namespace imageeditor::core {

class BasicPixelBrushEngine final : public IBrushEngine {
public:
    bool beginStroke(const BrushSettings& settings,
        const NormalizedPointerSample& first, BrushDabSink& sink) override;
    bool appendSample(const NormalizedPointerSample& sample,
        BrushDabSink& sink) override;
    bool endStroke(const NormalizedPointerSample& finalSample,
        BrushDabSink& sink) override;
    void cancelStroke() noexcept override;
    [[nodiscard]] bool active() const noexcept override { return active_; }
    [[nodiscard]] std::optional<Vec2d> constrainedPosition() const noexcept override
    { return constraint_.constrainedPosition(); }

private:
    [[nodiscard]] NormalizedPointerSample sanitize(
        const NormalizedPointerSample& sample) const noexcept;
    [[nodiscard]] NormalizedPointerSample smooth(
        const NormalizedPointerSample& sample) noexcept;
    [[nodiscard]] static NormalizedPointerSample interpolate(
        const NormalizedPointerSample& from,
        const NormalizedPointerSample& to, double amount) noexcept;
    [[nodiscard]] BrushDab makeDab(
        const NormalizedPointerSample& sample,
        std::optional<double> strokeDirectionDegrees = std::nullopt)
        const noexcept;
    [[nodiscard]] std::optional<double> resolveStrokeDirection(
        Vec2d documentCenter) noexcept;
    [[nodiscard]] double spacingFor(const BrushDab& dab) const noexcept;
    void emitDabAt(const NormalizedPointerSample& sample, BrushDabSink& sink);
    bool appendFiltered(const NormalizedPointerSample& sample, BrushDabSink& sink);

    BrushSettings settings_;
    BrushLineConstraint constraint_;
    std::optional<NormalizedPointerSample> previousRaw_;
    std::optional<NormalizedPointerSample> previousFiltered_;
    std::optional<NormalizedPointerSample> pathCursor_;
    std::optional<Vec2d> lastDabCenter_;
    std::optional<Vec2d> directionAnchor_;
    std::optional<double> resolvedDirectionDegrees_;
    double distanceUntilNextDab_ {0.0};
    std::uint64_t sequenceIndex_ {0};
    bool active_ {false};
};

} // namespace imageeditor::core
