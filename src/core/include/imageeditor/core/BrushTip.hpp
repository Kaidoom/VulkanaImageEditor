#pragma once

#include "imageeditor/core/BrushEngine.hpp"

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace imageeditor::core {

struct BrushTipBounds {
    double left {0.0};
    double top {0.0};
    double right {0.0};
    double bottom {0.0};

    [[nodiscard]] constexpr bool empty() const noexcept
    {
        return right <= left || bottom <= top;
    }
};

// Tip generation is independent from stroke interpolation. IBrushEngine emits
// generic, document-space dabs; an IBrushTip turns each dab into coverage. Tip
// implementations may use the preserved pressure/tilt/rotation in
// BrushDab::sourceSample without changing the interpolation path.
class IBrushTip {
public:
    virtual ~IBrushTip() = default;
    // Called once per dab. Implementations cache inverse transforms and filter
    // selection here; coverage() remains the hot per-pixel path.
    [[nodiscard]] virtual BrushTipBounds prepareDab(const BrushDab& dab,
        double hardness, double documentPixelFootprint) noexcept = 0;
    [[nodiscard]] virtual double coverage(
        Vec2d documentPixelCenter) const noexcept = 0;
};

class ProceduralRoundTip final : public IBrushTip {
public:
    [[nodiscard]] BrushTipBounds prepareDab(const BrushDab& dab,
        double hardness, double documentPixelFootprint) noexcept override;
    [[nodiscard]] double coverage(
        Vec2d documentPixelCenter) const noexcept override;

private:
    Vec2d center_;
    double halfWidth_ {0.0};
    double halfHeight_ {0.0};
    double cosine_ {1.0};
    double sine_ {0.0};
    double hardness_ {1.0};
    double normalizedAntialiasWidth_ {0.25};
};

class ProceduralEllipseTip final : public IBrushTip {
public:
    [[nodiscard]] BrushTipBounds prepareDab(const BrushDab& dab,
        double hardness, double documentPixelFootprint) noexcept override;
    [[nodiscard]] double coverage(
        Vec2d documentPixelCenter) const noexcept override;

private:
    Vec2d center_;
    double halfWidth_ {0.0};
    double halfHeight_ {0.0};
    double cosine_ {1.0};
    double sine_ {0.0};
    double hardness_ {1.0};
    double normalizedAntialiasWidth_ {0.25};
};

// Immutable top-left/Y-down grayscale data plus a prefiltered mip pyramid.
// The same instance is shared across strokes by an asset resolver.
class GrayscaleMaskAsset final {
public:
    GrayscaleMaskAsset(std::string id, std::uint64_t revision,
        std::uint32_t width, std::uint32_t height,
        std::span<const std::uint8_t> pixels);

    [[nodiscard]] const std::string& id() const noexcept { return id_; }
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
    [[nodiscard]] std::uint32_t width() const noexcept;
    [[nodiscard]] std::uint32_t height() const noexcept;
    [[nodiscard]] std::size_t mipLevelCount() const noexcept
    {
        return levels_.size();
    }
    [[nodiscard]] std::size_t retainedBytes() const noexcept;
    [[nodiscard]] double sample(double u, double v, double lod,
        bool repeat) const noexcept;

private:
    struct Level {
        std::uint32_t width {0};
        std::uint32_t height {0};
        std::vector<float> texels;
    };

    [[nodiscard]] double sampleLevel(
        std::size_t level, double u, double v, bool repeat) const noexcept;

    std::string id_;
    std::uint64_t revision_ {0};
    std::vector<Level> levels_;
};

class BitmapMaskTip final : public IBrushTip {
public:
    enum class Filtering { Isotropic, Anisotropic };
    // Existing bitmap assets retain their reviewed filtering. New directional
    // recipes opt into bounded anisotropy to preserve detail along a wide nib.
    explicit BitmapMaskTip(std::shared_ptr<const GrayscaleMaskAsset> mask,
        Filtering filtering = Filtering::Isotropic);

    [[nodiscard]] BrushTipBounds prepareDab(const BrushDab& dab,
        double hardness, double documentPixelFootprint) noexcept override;
    [[nodiscard]] double coverage(
        Vec2d documentPixelCenter) const noexcept override;
    [[nodiscard]] const GrayscaleMaskAsset* mask() const noexcept
    {
        return mask_.get();
    }

private:
    std::shared_ptr<const GrayscaleMaskAsset> mask_;
    Filtering filtering_;
    Vec2d center_;
    double halfWidth_ {0.0};
    double halfHeight_ {0.0};
    double cosine_ {1.0};
    double sine_ {0.0};
    double lod_ {0.0};
    double coverageExponent_ {1.0};
    int filterTaps_ {1};
    double filterStepU_ {0.0};
    double filterStepV_ {0.0};
};

// Grain is a separate, document-space coverage modulation stage. It never
// changes stroke interpolation or the tip's local geometry.
class IBrushGrain {
public:
    virtual ~IBrushGrain() = default;
    virtual void prepareDab(const BrushDab& dab,
        double documentPixelFootprint) noexcept = 0;
    [[nodiscard]] virtual double modulation(
        Vec2d documentPixelCenter) const noexcept = 0;
};

class NoBrushGrain final : public IBrushGrain {
public:
    void prepareDab(const BrushDab&, double) noexcept override {}
    [[nodiscard]] double modulation(Vec2d) const noexcept override
    {
        return 1.0;
    }
};

class DocumentAnchoredMaskGrain final : public IBrushGrain {
public:
    explicit DocumentAnchoredMaskGrain(
        std::shared_ptr<const GrayscaleMaskAsset> mask);

    void prepareDab(const BrushDab& dab,
        double documentPixelFootprint) noexcept override;
    [[nodiscard]] double modulation(
        Vec2d documentPixelCenter) const noexcept override;

private:
    std::shared_ptr<const GrayscaleMaskAsset> mask_;
    double scaleX_ {96.0};
    double scaleY_ {96.0};
    double cosine_ {1.0};
    double sine_ {0.0};
    double offsetU_ {0.0};
    double offsetV_ {0.0};
    double lod_ {0.0};
    double strength_ {0.0};
    bool invert_ {false};
};

struct BrushAssetCacheStats {
    std::size_t grayscaleMaskCount {0};
    std::size_t retainedBytes {0};
};

class IBrushAssetResolver {
public:
    virtual ~IBrushAssetResolver() = default;
    [[nodiscard]] virtual std::unique_ptr<IBrushTip> createTip(
        const BrushTipDescriptor& descriptor) const = 0;
    [[nodiscard]] virtual std::unique_ptr<IBrushGrain> createGrain(
        const BrushGrainDescriptor& descriptor) const = 0;
    [[nodiscard]] virtual BrushAssetCacheStats cacheStats() const noexcept = 0;
};

// Built-in resolution is intentionally narrow. A later asset service can own
// external asset lifetime and replace these calls at the composition root.
[[nodiscard]] std::unique_ptr<IBrushTip> makeBuiltinBrushTip(
    std::string_view assetId);
[[nodiscard]] std::unique_ptr<IBrushGrain> makeBuiltinBrushGrain(
    std::string_view assetId);
[[nodiscard]] const IBrushAssetResolver& builtinBrushAssetResolver() noexcept;

} // namespace imageeditor::core
