#pragma once

#include "imageeditor/core/CloneSettings.hpp"
#include "imageeditor/core/ColorSampler.hpp"
#include "imageeditor/core/Document.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace imageeditor::core {

// Immutable, continuous, premultiplied linear-light reference for one stroke.
// Every contributing raster and typed render cache is copied before the first
// destination mutation, deduplicated across source and healing context. No
// live document, layer, or mutable original surface is retained. Capture again
// between strokes. The caller owns target/revision/cancellation validation.
class CloneReference {
public:
    static constexpr std::size_t defaultSnapshotLimit = 256U * 1024U * 1024U;

    // Current & Below includes the target in document bottom-to-top order;
    // All Visible includes every effectively visible leaf. SourceLayer binds
    // sourceId independently of targetId and bypasses adjustments, opacity and
    // blending, while retaining crop, chamfer and hierarchy visibility.
    // Healing context is intrinsic target content for direct same-layer raw
    // cloning, or rendered Current & Below for the retouch-layer workflow.
    // No source selection mask is applied. includeDestinationContext=false
    // avoids copying otherwise unused context for ordinary Stamp strokes.
    [[nodiscard]] static std::optional<CloneReference> capture(const Document&, LayerId targetId,
        LayerId sourceId, CloneSampleSource, std::string& diagnostic,
        std::size_t limitBytes = defaultSnapshotLimit, bool includeDestinationContext = true,
        std::function<bool()> cancelled = { }, std::span<const SampleCacheOverride> prepared = { },
        bool deferRenderedPreparation = false);

    // Opt-in immutable capture/preparation split for automatic reconstruction.
    // The owner copies original source bytes; worker preparation may evaluate
    // spatial effects without touching the live document or its render cache.
    [[nodiscard]] CloneReference prepared(const std::function<bool()>& cancelled = {},
        std::size_t byteBudget = 768U * 1024U * 1024U) const;
    // Same-source repair uses exact intrinsic texels: inverse-rounding or
    // bilinear interpolation must not leak marked damaged RGB into context.
    [[nodiscard]] PremultipliedColor rawTexel(int x, int y) const;
    [[nodiscard]] Extent2u rawExtent() const noexcept;
    // Conservative document-axis support, including interpolation and active
    // spatial kernels. Reconstruction excludes this neighborhood around damage
    // from its trusted rendered-reference context.
    [[nodiscard]] Vec2d readSupport() const noexcept;

    // Continuous document coordinates; invalid/out-of-source points return
    // transparent, and interpolation never reads hidden RGB at zero alpha.
    [[nodiscard]] PremultipliedColor sample(Vec2d) const;
    [[nodiscard]] PremultipliedColor sampleDestination(Vec2d) const;
    [[nodiscard]] bool validSample(Vec2d) const noexcept;
    [[nodiscard]] std::size_t snapshotBytes() const noexcept;
    [[nodiscard]] std::size_t sourceCount() const noexcept;

private:
    struct Impl;
    explicit CloneReference(std::shared_ptr<const Impl> impl)
        : impl_(std::move(impl))
    {
    }
    std::shared_ptr<const Impl> impl_;
};

} // namespace imageeditor::core
