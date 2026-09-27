#pragma once

#include "imageeditor/core/SmartSelection.hpp"

namespace imageeditor::core {

// Only compact hint masks and identity metadata enter history, never the full
// rendered reference/solver workspace. Weak surface identities do not retain
// deleted layer pixels or extend a document/window lifetime.
class SmartSelectionEvidence final : public SelectionEvidence {
public:
    SmartSelectionEvidence(const Document& document, std::optional<LayerId> layer, ColorSampleSource source,
        QuickSelectionHints hints)
        : revision_(document.revision())
        , layer_(layer)
        , source_(source)
        , hints_(std::move(hints))
    {
        for (const auto& item : document.layers())
            if (const auto* raster = std::get_if<RasterLayer>(&item.payload); raster && raster->surface)
                pixels_.push_back({ item.id, raster->surface, raster->surface->revision() });
        // Freeze lazy allocations before command history accounts for them.
        if (hints_.foreground)
            (void)hints_.foreground->boundaryEdges();
        if (hints_.background)
            (void)hints_.background->boundaryEdges();
        if (hints_.rejected)
            (void)hints_.rejected->boundaryEdges();
    }
    [[nodiscard]] const QuickSelectionHints& hints() const noexcept { return hints_; }
    [[nodiscard]] bool matches(
        const Document& document, std::optional<LayerId> layer, ColorSampleSource source) const noexcept
    {
        if (revision_ != document.revision() || layer_ != layer || source_ != source)
            return false;
        for (const auto& fingerprint : pixels_) {
            const auto* item = document.layer(fingerprint.layer);
            const auto* raster = item ? std::get_if<RasterLayer>(&item->payload) : nullptr;
            const auto surface = fingerprint.surface.lock();
            if (!raster || !surface || raster->surface != surface
                || surface->revision() != fingerprint.revision)
                return false;
        }
        return true;
    }
    [[nodiscard]] bool equivalent(const SelectionEvidence& other) const noexcept override
    {
        const auto* evidence = dynamic_cast<const SmartSelectionEvidence*>(&other);
        if (!evidence || revision_ != evidence->revision_ || layer_ != evidence->layer_
            || source_ != evidence->source_ || pixels_.size() != evidence->pixels_.size())
            return false;
        for (std::size_t i = 0; i < pixels_.size(); ++i) {
            const auto& a = pixels_[i];
            const auto& b = evidence->pixels_[i];
            if (a.layer != b.layer || a.revision != b.revision || a.surface.owner_before(b.surface)
                || b.surface.owner_before(a.surface))
                return false;
        }
        const auto same = [](const SelectionState& a, const SelectionState& b) {
            return a == b || (a && b && a->equivalent(*b));
        };
        return same(hints_.foreground, evidence->hints_.foreground)
            && same(hints_.background, evidence->hints_.background)
            && same(hints_.rejected, evidence->hints_.rejected);
    }
    [[nodiscard]] std::size_t memoryCost() const noexcept override
    {
        return sizeof(*this) + pixels_.capacity() * sizeof(PixelIdentity)
            + (hints_.foreground ? hints_.foreground->memoryCost() : 0)
            + (hints_.background ? hints_.background->memoryCost() : 0)
            + (hints_.rejected ? hints_.rejected->memoryCost() : 0);
    }

private:
    struct PixelIdentity {
        LayerId layer;
        std::weak_ptr<const RasterSurface> surface;
        Revision revision;
    };
    Revision revision_;
    std::optional<LayerId> layer_;
    ColorSampleSource source_;
    QuickSelectionHints hints_;
    std::vector<PixelIdentity> pixels_;
};

} // namespace imageeditor::core
