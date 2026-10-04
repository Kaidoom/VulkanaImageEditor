#include "imageeditor/core/SelectionCommands.hpp"
#include "imageeditor/core/ColorSampler.hpp"
#include "imageeditor/core/LayerGeometry.hpp"
#include "imageeditor/core/LayerCrop.hpp"
#include <algorithm>
#include <array>
#include <cmath>

namespace imageeditor::core {
SetSelectionCommand::SetSelectionCommand(SelectionState after, std::string label,
    std::optional<SelectionEvidenceState> evidence)
    : after_(std::move(after)), evidenceAfter_(evidence ? std::move(*evidence) : SelectionEvidenceState {}),
      label_(std::move(label)), explicitEvidence_(evidence.has_value()) {}
bool SetSelectionCommand::apply(Document& document)
{
    if (!initialized_) {
        before_ = document.selection();
        rememberedBefore_ = document.lastSelection();
        evidenceBefore_ = document.selectionEvidence();
    }
    // Materialize immutable caches before history accounts for this command.
    // Its retained memory cost must not grow later during rendering.
    if (before_) (void)before_->boundaryEdges();
    if (after_) (void)after_->boundaryEdges();
    if (rememberedBefore_) (void)rememberedBefore_->boundaryEdges();
    const bool changed = initialized_ || explicitEvidence_
        ? document.setSelection(after_, evidenceAfter_) : document.setSelection(after_);
    evidenceAfter_ = document.selectionEvidence();
    if (changed) document.setLastSelection(after_ ? after_ : before_ ? before_ : rememberedBefore_);
    initialized_ = true;
    return changed;
}
bool SetSelectionCommand::undo(Document& document)
{
    if (!initialized_ || !document.setSelection(before_, evidenceBefore_)) return false;
    document.setLastSelection(rememberedBefore_);
    return true;
}
std::size_t SetSelectionCommand::memoryCost() const noexcept
{ return sizeof(*this) + label_.size() + (before_ ? before_->memoryCost() : 0) + (after_ ? after_->memoryCost() : 0)
    + (rememberedBefore_ && rememberedBefore_ != before_ && rememberedBefore_ != after_ ? rememberedBefore_->memoryCost() : 0)
    + (evidenceBefore_ ? evidenceBefore_->memoryCost() : 0) + (evidenceAfter_ ? evidenceAfter_->memoryCost() : 0); }

LayerViaCopyCommand::LayerViaCopyCommand(LayerId source, std::optional<LayerId> previousActive)
    : source_(source), previousActive_(previousActive.value_or(source)) {}
bool LayerViaCopyCommand::apply(Document& document)
{
    if (result_) return placement_ && document.insertLayerAt(*placement_, *result_);
    const auto* source = document.layer(source_);
    const auto* raster = source ? std::get_if<RasterLayer>(&source->payload) : nullptr;
    if (!raster || !raster->surface) return false;
    const auto extent = raster->surface->extent();
    const auto selection = document.selection();
    const bool bakeFilters=bool(selection)&&(hasActiveSpatialFilters(source->filters)||hasActiveLayerEffects(source->effects)||(source->mask&&source->mask->enabled));
    std::optional<Layer> filteredSource;
    if(bakeFilters)filteredSource=prepareSpatialFilterLayer(*source);
    std::shared_ptr<RasterSurface> surface;
    auto transform = source->localToDocument;
    if (!selection) {
        std::vector<std::byte> bytes(std::size_t(extent.width) * extent.height * 4);
        raster->surface->copyRgba8({0,0,int(extent.width),int(extent.height)}, bytes, std::size_t(extent.width) * 4);
        surface = std::make_shared<ContiguousRasterSurface>(extent, std::move(bytes));
    } else {
        auto region = selection->bounds();
        if (region.empty()) return false;
        const auto sourceBounds=bakeFilters?layerStyledBounds(*filteredSource):RectD{source->rasterOrigin.x,source->rasterOrigin.y,double(extent.width),double(extent.height)};
        const std::array corners {transform.map({sourceBounds.x,sourceBounds.y}),transform.map({sourceBounds.right(),sourceBounds.y}),
            transform.map({sourceBounds.x,sourceBounds.bottom()}),transform.map({sourceBounds.right(),sourceBounds.bottom()})};
        double left = corners[0].x, right = left, top = corners[0].y, bottom = top;
        for (const auto p : corners) {
            if (!std::isfinite(p.x) || !std::isfinite(p.y)) return false;
            left = std::min(left,p.x); right = std::max(right,p.x); top = std::min(top,p.y); bottom = std::max(bottom,p.y);
        }
        const auto canvas = document.canvas().extent;
        const int x0 = int(std::floor(std::clamp(left,0.0,double(canvas.width))));
        const int y0 = int(std::floor(std::clamp(top,0.0,double(canvas.height))));
        const int x1 = int(std::ceil(std::clamp(right,0.0,double(canvas.width))));
        const int y1 = int(std::ceil(std::clamp(bottom,0.0,double(canvas.height))));
        region = region.clippedTo({x0,y0,x1-x0,y1-y0});
        if (region.empty()) return false;
        std::vector<std::byte> bytes(std::size_t(region.width) * std::size_t(region.height) * 4);
        const PreparedRasterSampler sampler(*source);
        std::optional<PreparedLayerSampler> filteredSampler;
        if(bakeFilters)filteredSampler.emplace(*filteredSource);
        const auto inverse=transform.inverted();
        if(!inverse)return false;
        const bool pixelAligned = !bakeFilters && transform.isAffine() && transform.m00 == 1 && transform.m11 == 1
            && transform.m01 == 0 && transform.m10 == 0
            && transform.m02 == std::floor(transform.m02) && transform.m12 == std::floor(transform.m12);
        if (pixelAligned) {
            // Exact translated copy: a single bounded surface read, rather
            // than a virtual 1-texel read/filter for each output pixel.
            raster->surface->copyRgba8({int(double(region.x)-transform.m02-source->rasterOrigin.x), int(double(region.y)-transform.m12-source->rasterOrigin.y),
                region.width, region.height}, bytes, std::size_t(region.width)*4);
        }
        bool nonempty = false;
        for (int y = 0; y < region.height; ++y) for (int x = 0; x < region.width; ++x) {
            const auto offset = (std::size_t(y) * std::size_t(region.width) + std::size_t(x)) * 4;
            const auto coverage = selection->coverageAtDocumentPixel(region.x+x, region.y+y);
            const Vec2d point{double(region.x+x)+.5,double(region.y+y)+.5};
            const auto derivatives=inverse->derivatives(point);
            const auto cropCoverage=source->crop && !bakeFilters ? layerCropCoverage(*source->crop,inverse->map(point),
                derivatives[0],derivatives[1]) : 1.0F;
            if (pixelAligned) {
                const auto alpha = std::uint8_t(std::lround(std::to_integer<unsigned>(bytes[offset+3]) * (double(coverage)/255.0) * cropCoverage));
                if (alpha) { bytes[offset+3] = std::byte(alpha); nonempty = true; }
                else std::fill_n(bytes.begin()+std::ptrdiff_t(offset), 4, std::byte {0});
                continue;
            }
            if (!coverage) continue;
            const auto color = filteredSampler?encodeColor(filteredSampler->sample(point)):sampler.sample(point);
            const auto alpha = std::uint8_t(std::lround(unsigned(color.alpha) * (double(coverage)/255.0) * cropCoverage));
            if (!alpha) continue;
            nonempty = true;
            bytes[offset] = std::byte(color.red); bytes[offset+1] = std::byte(color.green);
            bytes[offset+2] = std::byte(color.blue); bytes[offset+3] = std::byte(alpha);
        }
        if (!nonempty) return false;
        surface = std::make_shared<ContiguousRasterSurface>(Extent2u {std::uint32_t(region.width),std::uint32_t(region.height)}, std::move(bytes));
        transform = {}; transform.m02 = region.x; transform.m12 = region.y;
    }
    auto layer = Layer::raster(source->name + " copy", std::move(surface));
    layer.localToDocument = transform;
    if(!selection)layer.mask=source->mask;
    else if(source->mask&&!source->mask->enabled) {
        auto mask=std::make_shared<LayerMask>(*source->mask);
        mask->localToMask=composeTransform(mask->localToMask,composeTransform(*source->localToDocument.inverted(),transform));
        layer.mask=std::move(mask);
    }
    layer.crop=selection?std::nullopt:source->crop; // Extraction bakes visible crop once; duplication retains it.
    layer.opacity = source->opacity; // sampled pixels do not include layer opacity
    layer.blendSeed = source->blendSeed;
    layer.blendMode = source->blendMode; // Extraction retains unblended source pixels.
    layer.visible = source->visible;
    layer.colorLabel = source->colorLabel;
    // Duplication keeps the independent editable stack. A selected extraction
    // bakes the complete adjusted/spatially-filtered source BEFORE clipping the
    // selection, so neighborhoods are neither discarded nor filtered twice.
    if(!selection){layer.filters=source->filters;layer.effects=source->effects;
        layer.rasterOrigin=source->rasterOrigin;layer.rasterEffectFrame=source->rasterEffectFrame;}
    if (source->adjustments && !bakeFilters) {
        // The copy owns independently editable values; immutable coverage may
        // be shared. A selected extraction stores pixels in document-aligned
        // output coordinates, so migrate each frozen local→mask mapping.
        auto adjustments = std::make_shared<AdjustmentStack>(*source->adjustments);
        if (selection) {
            const auto inverse = source->localToDocument.inverted();
            if (!inverse) return false;
            const auto outputToSource = composeAffine(*inverse, layer.localToDocument);
            for (auto& adjustment : adjustments->items)
                if (adjustment.mask)
                    adjustment.mask->localToMask = composeAffine(adjustment.mask->localToMask, outputToSource);
        }
        if (!validAdjustments(*adjustments)) return false;
        layer.adjustments = std::move(adjustments);
    }
    index_ = std::size_t(std::distance(document.layers().data(), source)) + 1;
    placement_=document.tree().placement(source_);
    ++placement_->index;
    result_ = std::move(layer);
    return document.insertLayerAt(*placement_, *result_);
}
bool LayerViaCopyCommand::undo(Document& document)
{ return result_ && document.takeLayer(result_->id).has_value(); }
std::size_t LayerViaCopyCommand::memoryCost() const noexcept
{
    if (!result_) return sizeof(*this);
    const auto extent = std::get<RasterLayer>(result_->payload).surface->extent();
    return sizeof(*this) + result_->name.size() + std::size_t(extent.width) * extent.height * 4
        + adjustmentMemoryCost(result_->adjustments) + spatialFilterMemoryCost(result_->filters)
        + layerEffectMemoryCost(result_->effects)
        + (result_->mask?sizeof(LayerMask)+result_->mask->coverage->memoryCost():0);
}
std::optional<std::uint64_t> LayerViaCopyCommand::activeLayerAfter(bool undo) const noexcept
{ return undo ? std::optional(previousActive_) : createdLayerId(); }
}
