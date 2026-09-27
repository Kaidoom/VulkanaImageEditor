#include "imageeditor/core/SelectedPixelTransform.hpp"
#include "imageeditor/core/BoundedParallel.hpp"
#include "imageeditor/core/CoverageResampler.hpp"
#include "imageeditor/core/LayerCrop.hpp"
#include "imageeditor/core/LayerGeometry.hpp"
#include "imageeditor/core/TransformSupport.hpp"
#include <chrono>
#include <cstring>
#include <set>
#include <stdexcept>

namespace imageeditor::core
{
namespace
{
RectI pixelBounds(const std::vector<Vec2d> &points, double padding = 0)
{
    double left = 1e30, top = 1e30, right = -1e30, bottom = -1e30;
    for (auto p : points) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y))
            throw std::invalid_argument("Nonfinite transformed pixel bounds");
        left = std::min(left, p.x);
        top = std::min(top, p.y);
        right = std::max(right, p.x);
        bottom = std::max(bottom, p.y);
    }
    left = std::floor(left - padding);
    top = std::floor(top - padding);
    right = std::ceil(right + padding);
    bottom = std::ceil(bottom + padding);
    if (left < -1e9 || top < -1e9 || right > 1e9 || bottom > 1e9)
        throw std::length_error("Transformed pixels exceed supported coordinates");
    return {int(left), int(top), int(right - left), int(bottom - top)};
}
std::vector<Vec2d> corners(RectI r, const ProjectiveTransform &m)
{
    return {m.map({double(r.x), double(r.y)}), m.map({double(r.right()), double(r.y)}),
            m.map({double(r.right()), double(r.bottom())}), m.map({double(r.x), double(r.bottom())})};
}
std::vector<Vec2d> clipPolygon(std::vector<Vec2d> polygon, RectD box)
{
    for (int edge = 0; edge < 4 && !polygon.empty(); ++edge) {
        const auto distance = [&](Vec2d p) {
            return edge == 0   ? p.x - box.x
                   : edge == 1 ? box.right() - p.x
                   : edge == 2 ? p.y - box.y
                               : box.bottom() - p.y;
        };
        std::vector<Vec2d> next;
        auto a = polygon.back();
        double da = distance(a);
        for (auto b : polygon) {
            const auto db = distance(b);
            if ((da < 0) != (db < 0))
                next.push_back(a + (b - a) * (da / (da - db)));
            if (db >= 0)
                next.push_back(b);
            a = b;
            da = db;
        }
        polygon = std::move(next);
    }
    return polygon;
}
struct PixelState {
    std::shared_ptr<RasterSurface> surface;
    std::optional<RegionalRasterSurface::State> regional;
    Vec2d origin;
    std::optional<RectD> frame;
    SelectionState selection;
    SelectionEvidenceState evidence;
    SelectionState lastSelection;
};
class PixelTransformCommand final : public Command
{
  public:
    PixelTransformCommand(LayerId id, ProjectiveTransform external, PixelState before, PixelState after)
        : id_(id), external_(external), before_(std::move(before)), after_(std::move(after))
    {
        for (const auto &mask :
             {before_.selection, after_.selection, before_.lastSelection, after_.lastSelection})
            if (mask)
                (void)mask->boundaryEdges();
        cost_ = sizeof(*this) + (before_.regional ? before_.regional->tiles.size() * 96 : 0) +
                (after_.regional ? after_.regional->tiles.size() * 96 : 0);
        if (after_.regional)
            for (const auto &[key, tile] : after_.regional->tiles) {
                if (!before_.regional || !before_.regional->tiles.contains(key) ||
                    before_.regional->tiles.at(key) != tile)
                    cost_ += sizeof(RegionalRasterSurface::Tile);
            }
        if (before_.selection)
            cost_ += before_.selection->memoryCost();
        if (after_.selection && after_.selection != before_.selection)
            cost_ += after_.selection->memoryCost();
        if (before_.evidence)
            cost_ += before_.evidence->memoryCost();
        if (after_.evidence && after_.evidence != before_.evidence)
            cost_ += after_.evidence->memoryCost();
        for (const auto &mask : {before_.lastSelection, after_.lastSelection})
            if (mask && mask != before_.selection && mask != after_.selection)
                cost_ += mask->memoryCost();
    }
    bool apply(Document &doc) override { return restore(doc, before_, after_); }
    bool undo(Document &doc) override { return restore(doc, after_, before_); }
    bool canAdoptApplied(const Document &doc) const noexcept override { return matches(doc, after_); }
    std::string_view label() const noexcept override { return "Transform selected pixels"; }
    std::size_t memoryCost() const noexcept override { return cost_; }

  private:
    bool matches(const Document &doc, const PixelState &s) const noexcept
    {
        const auto *l = doc.layer(id_);
        const auto *r = l ? std::get_if<RasterLayer>(&l->payload) : nullptr;
        return r && r->surface == s.surface && l->localToDocument == external_ &&
               l->rasterOrigin == s.origin &&
               (doc.selection() == s.selection ||
                (doc.selection() && s.selection && doc.selection()->equivalent(*s.selection)));
    }
    bool restore(Document &doc, const PixelState &expected, const PixelState &desired)
    {
        if (!matches(doc, expected))
            return false;
        if (desired.regional)
            static_cast<RegionalRasterSurface &>(*desired.surface).restore(*desired.regional);
        doc.setLayerRasterStorage(id_, desired.surface, desired.origin, desired.frame);
        doc.setSelection(desired.selection, desired.evidence);
        doc.setLastSelection(desired.lastSelection);
        return true;
    }
    LayerId id_;
    ProjectiveTransform external_;
    PixelState before_, after_;
    std::size_t cost_;
};
} // namespace

SelectedPixelTransformSession::SelectedPixelTransformSession(Document &document, LayerId id,
                                                             std::size_t budget)
    : document_(&document), layer_(id), sourceRevision_(0), contentState_(document.contentState()),
      budget_(budget)
{
    const auto *layer = document.layer(id);
    const auto *raster = layer ? std::get_if<RasterLayer>(&layer->payload) : nullptr;
    if (!raster || !raster->surface)
        throw std::invalid_argument("Transform Selected Pixels requires a raster layer. Use Rasterize "
                                    "Layer for text or shapes.");
    selection_ = document.selection();
    originalLastSelection_ = document.lastSelection();
    expectedSelection_ = selection_;
    evidence_ = document.selectionEvidence();
    if (!selection_ || selection_->bounds().empty())
        throw std::invalid_argument("Select some pixels before using Transform Selected Pixels");
    source_ = raster->surface;
    sourceRevision_ = source_->revision();
    external_ = layer->localToDocument;
    const auto inverse = external_.inverted();
    if (!inverse)
        throw std::invalid_argument("Layer transform is not invertible");
    inverseExternal_ = *inverse;
    originalOrigin_ = layer->rasterOrigin;
    originalEffectFrame_ = layer->rasterEffectFrame;
    crop_ = layer->crop;
    adjustments_ = layer->adjustments;
    filters_ = layer->filters;
    effects_ = layer->effects;
    working_ = std::make_shared<RegionalRasterSurface>(source_, originalOrigin_);
    initialState_ = working_->state();
    actionState_ = initialState_;
    storageCheckpoints_.push_back(initialState_);
    originalBounds_ = initialState_.bounds;
    const auto b = selection_->bounds();
    auto polygon = clipPolygon(corners(originalBounds_, external_),
                               {double(b.x) - 1, double(b.y) - 1, double(b.width) + 2, double(b.height) + 2});
    if (polygon.empty())
        throw std::invalid_argument("The selection contains no pixels on the primary raster layer");
    for (auto &p : polygon)
        p = inverseExternal_.map(p);
    fragmentBounds_ = pixelBounds(polygon, 1).clippedTo(originalBounds_);
    const auto count = std::size_t(fragmentBounds_.width) * std::size_t(fragmentBounds_.height);
    if (fragmentBounds_.empty() || count > budget_ / 16)
        throw std::length_error("Selected fragment exceeds the transform memory budget");
    fragment_.resize(count * 4);
    coverage_.resize(count);
    source_->copyRgba8({fragmentBounds_.x - originalBounds_.x, fragmentBounds_.y - originalBounds_.y,
                        fragmentBounds_.width, fragmentBounds_.height},
                       fragment_, std::size_t(fragmentBounds_.width) * 4);
    bool applicable = false;
    CoverageResampler selectionCoverage(*selection_);
    for (int y = 0; y < fragmentBounds_.height; ++y)
        for (int x = 0; x < fragmentBounds_.width; ++x) {
            const int lx = fragmentBounds_.x + x, ly = fragmentBounds_.y + y;
            const auto index = std::size_t(y) * std::size_t(fragmentBounds_.width) + std::size_t(x);
            if (crop_ && !cropAllowsTexel(crop_, lx, ly))
                continue;
            const Vec2d p{lx + .5, ly + .5};
            coverage_[index] = selectionCoverage.sample(external_.map(p), external_.maximumScale(p));
            applicable |= coverage_[index] > 0 && fragment_[index * 4 + 3] != std::byte{};
        }
    if (!applicable)
        throw std::invalid_argument("The selection contains no visible source "
                                    "pixels on the primary raster layer");
    canvasExtent_ = document.canvas().extent;
    rememberTarget();
}
SelectedPixelTransformSession::~SelectedPixelTransformSession() { cancel(); }
void SelectedPixelTransformSession::rememberTarget() noexcept
{
    workingRevision_ = working_->revision();
    expectedOrigin_ = document_->layer(layer_)->rasterOrigin;
    expectedEffectFrame_ = document_->layer(layer_)->rasterEffectFrame;
    expectedContentState_ = document_->contentState();
}
bool SelectedPixelTransformSession::targetAvailable() const noexcept
{
    const auto *l = document_->layer(layer_);
    const auto *r = l ? std::get_if<RasterLayer>(&l->payload) : nullptr;
    return active_ && r && (r->surface == source_ || r->surface == working_) &&
           source_->revision() == sourceRevision_ && working_->revision() == workingRevision_ &&
           l->rasterOrigin == expectedOrigin_ && l->rasterEffectFrame == expectedEffectFrame_ &&
           document_->contentState() == expectedContentState_ &&
           document_->canvas().extent == canvasExtent_ && l->localToDocument == external_ &&
           l->crop == crop_ && l->adjustments == adjustments_ && l->filters == filters_ &&
           l->effects == effects_ && document_->selection() == expectedSelection_;
}
bool SelectedPixelTransformSession::identity(const ProjectiveTransform &mapping) const noexcept
{
    const auto b = selection_->bounds();
    for (auto p :
         std::array{Vec2d{double(b.x), double(b.y)}, Vec2d{double(b.right()), double(b.y)},
                    Vec2d{double(b.right()), double(b.bottom())}, Vec2d{double(b.x), double(b.bottom())}}) {
        const auto delta = mapping.map(p) - p;
        if (!std::isfinite(delta.x) || !std::isfinite(delta.y) || std::hypot(delta.x, delta.y) > 1e-8)
            return false;
    }
    return true;
}
SelectedPixelTransformSession::Sample SelectedPixelTransformSession::texel(int x, int y, int level) const
{
    if (level) {
        const auto &mip = mips_[std::size_t(level - 1)];
        if (x < 0 || y < 0 || x >= int(mip.extent.width) || y >= int(mip.extent.height))
            return {};
        return mip.pixels[std::size_t(y) * mip.extent.width + std::size_t(x)];
    }
    if (x < 0 || y < 0 || x >= fragmentBounds_.width || y >= fragmentBounds_.height)
        return {};
    const auto i = std::size_t(y) * std::size_t(fragmentBounds_.width) + std::size_t(x);
    const auto *b = fragment_.data() + i * 4;
    Sample result{decodeColor({std::to_integer<std::uint8_t>(b[0]), std::to_integer<std::uint8_t>(b[1]),
                               std::to_integer<std::uint8_t>(b[2]), std::to_integer<std::uint8_t>(b[3])}),
                  coverage_[i]};
    for (auto &channel : result.color)
        channel *= result.coverage;
    return result;
}
void SelectedPixelTransformSession::prepareMips()
{
    if (!mips_.empty())
        return;
    Extent2u size{std::uint32_t(fragmentBounds_.width), std::uint32_t(fragmentBounds_.height)};
    for (int level = 0; size.width > 1 || size.height > 1; ++level) {
        Mip mip{{(size.width + 1) / 2, (size.height + 1) / 2}, {}};
        const auto count = std::size_t(mip.extent.width) * mip.extent.height;
        if (temporaryBytes() + count * sizeof(Sample) > budget_)
            throw std::length_error("Fragment filtering exceeds transform memory budget");
        mip.pixels.resize(count);
        for (std::uint32_t y = 0; y < mip.extent.height; ++y)
            for (std::uint32_t x = 0; x < mip.extent.width; ++x) {
                auto &value = mip.pixels[std::size_t(y) * mip.extent.width + x];
                for (int sy = 0; sy < 2; ++sy)
                    for (int sx = 0; sx < 2; ++sx) {
                        const auto s = texel(int(x) * 2 + sx, int(y) * 2 + sy, level);
                        for (std::size_t c = 0; c < 4; ++c)
                            value.color[c] += s.color[c] * .25f;
                        value.coverage += s.coverage * .25f;
                    }
            }
        size = mip.extent;
        mips_.push_back(std::move(mip));
    }
}
SelectedPixelTransformSession::Sample
SelectedPixelTransformSession::sample(Vec2d point, const ProjectiveTransform &inverse, double affineLod) const
{
    const auto p = inverse.map(point);
    if (!std::isfinite(p.x) || !std::isfinite(p.y))
        return {};
    const double lod = affineLod >= 0  ? affineLod
                       : mips_.empty() ? 0
                                       : std::clamp(std::log2(std::max(1.0, inverse.maximumScale(point))),
                                                    0.0, double(mips_.size()));
    const auto atLevel = [&](int level) {
        const auto scale = std::ldexp(1.0, level);
        const double x = (p.x - fragmentBounds_.x) / scale - .5, y = (p.y - fragmentBounds_.y) / scale - .5;
        const auto width = level ? int(mips_[std::size_t(level - 1)].extent.width) : fragmentBounds_.width;
        const auto height = level ? int(mips_[std::size_t(level - 1)].extent.height) : fragmentBounds_.height;
        if (x < -1 || y < -1 || x >= width || y >= height)
            return Sample{};
        const int ix = int(std::floor(x)), iy = int(std::floor(y));
        const float fx = float(x - ix), fy = float(y - iy);
        Sample result;
        for (int dy = 0; dy < 2; ++dy)
            for (int dx = 0; dx < 2; ++dx) {
                const auto v = texel(ix + dx, iy + dy, level);
                const float w = (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy);
                for (std::size_t c = 0; c < 4; ++c)
                    result.color[c] += v.color[c] * w;
                result.coverage += v.coverage * w;
            }
        return result;
    };
    const int level = int(std::floor(lod));
    auto result = atLevel(level);
    const float fraction = float(lod - level);
    if (fraction > 0) {
        const auto next = atLevel(level + 1);
        for (std::size_t c = 0; c < 4; ++c)
            result.color[c] += fraction * (next.color[c] - result.color[c]);
        result.coverage += fraction * (next.coverage - result.coverage);
    }
    return result;
}
bool SelectedPixelTransformSession::preview(const ProjectiveTransform &requested)
{
    if (!targetAvailable())
        return false;
    if (requested == mapping_)
        return true;
    if (identity(requested)) {
        working_->restore(initialState_);
        document_->setLayerRasterStorage(layer_, source_, originalOrigin_, originalEffectFrame_);
        mapping_ = {};
        rememberTarget();
        return true;
    }
    const auto local = composeTransform(inverseExternal_, composeTransform(requested, external_));
    const auto inverse = local.inverted();
    const RectD support{double(fragmentBounds_.x) - 1, double(fragmentBounds_.y) - 1,
                        double(fragmentBounds_.width) + 2, double(fragmentBounds_.height) + 2};
    if (!inverse || !local.validOver(support))
        return false;
    const auto destination = pixelBounds(corners(fragmentBounds_, local), 2);
    const auto bounds = originalBounds_.united(destination);
    RegionalRasterSurface::validateBounds(bounds);
    const auto evaluatedSupport = transformEvaluationSupport(
        {double(bounds.x), double(bounds.y), double(bounds.width), double(bounds.height)}, filters_,
        effects_);
    if (!external_.validOver(evaluatedSupport))
        return false;
    if (!inverse->isAffine() || inverse->maximumScale() > 1.000001)
        prepareMips();
    const double affineLod =
        inverse->isAffine()
            ? std::clamp(std::log2(std::max(1.0, inverse->maximumScale())), 0.0, double(mips_.size()))
            : -1;
    const bool integerMove = inverse->isAffine() && inverse->m00 == 1 && inverse->m11 == 1 &&
                             inverse->m01 == 0 && inverse->m10 == 0 &&
                             inverse->m02 == std::floor(inverse->m02) &&
                             inverse->m12 == std::floor(inverse->m12);
    std::set<RegionalRasterSurface::Key> tiles;
    // Restoring the staged state below removes obsolete preview tiles and
    // journals their GPU refresh; do not resample the previous destination.
    for (auto region : {fragmentBounds_, destination}) {
        region = region.clippedTo(bounds);
        if (region.empty())
            continue;
        for (int ty = int(std::floor(double(region.y) / 64));
             ty <= int(std::floor(double(region.bottom() - 1) / 64)); ++ty)
            for (int tx = int(std::floor(double(region.x) / 64));
                 tx <= int(std::floor(double(region.right() - 1) / 64)); ++tx)
                tiles.emplace(tx, ty);
    }
    if (tiles.size() * sizeof(RegionalRasterSurface::Tile) * 2 + temporaryBytes() +
            working_->retainedBytes() + pending_.memoryUsed() >
        budget_)
        throw std::length_error("Pixel transform preview exceeds the memory budget");
    std::vector<std::vector<std::byte>> bytes;
    bytes.reserve(tiles.size());
    std::vector<RasterPatch> patches;
    patches.reserve(tiles.size());
    std::vector<RectI> regions;
    regions.reserve(tiles.size());
    for (auto [tx, ty] : tiles) {
        const auto region = RectI{tx * 64, ty * 64, 64, 64}.clippedTo(bounds);
        const auto stride = std::size_t(region.width) * 4;
        auto &buffer = bytes.emplace_back(stride * std::size_t(region.height));
        const auto original = region.clippedTo(originalBounds_);
        if (!original.empty())
            source_->copyRgba8({original.x - originalBounds_.x, original.y - originalBounds_.y,
                                original.width, original.height},
                               std::span(buffer).subspan(std::size_t(original.y - region.y) * stride +
                                                         std::size_t(original.x - region.x) * 4),
                               stride);
        regions.push_back(region);
        evaluatedPixels_ += std::uint64_t(region.width) * std::uint64_t(region.height);
        patches.push_back(
            {{region.x - bounds.x, region.y - bounds.y, region.width, region.height}, buffer, stride});
    }
    // The immutable gather has no cross-tile dependencies or reductions.
    // Reuse the application's bounded repair/filter executor, never create a
    // per-document pool. A busy background solve gets a short admission wait;
    // serial fallback avoids waiting for that unrelated operation to finish.
    const auto evaluate = [&](unsigned rank, unsigned participants) {
        for (std::size_t i = rank; i < regions.size(); i += participants) {
            const auto region = regions[i];
            const auto stride = std::size_t(region.width) * 4;
            auto &buffer = bytes[i];
            for (int y = 0; y < region.height; ++y)
                for (int x = 0; x < region.width; ++x) {
                    const int lx = region.x + x, ly = region.y + y;
                    const int sx = lx - fragmentBounds_.x, sy = ly - fragmentBounds_.y;
                    const float removed =
                        sx >= 0 && sy >= 0 && sx < fragmentBounds_.width && sy < fragmentBounds_.height
                            ? coverage_[std::size_t(sy) * std::size_t(fragmentBounds_.width) +
                                        std::size_t(sx)]
                            : 0;
                    const auto fragment = integerMove ? texel(lx + int(inverse->m02) - fragmentBounds_.x,
                                                              ly + int(inverse->m12) - fragmentBounds_.y, 0)
                                                      : sample({lx + .5, ly + .5}, *inverse, affineLod);
                    auto *pixel = buffer.data() + std::size_t(y) * stride + std::size_t(x) * 4;
                    if (removed == 0 && fragment.color[3] == 0)
                        continue;
                    const Rgba8 originalPixel{
                        std::to_integer<std::uint8_t>(pixel[0]), std::to_integer<std::uint8_t>(pixel[1]),
                        std::to_integer<std::uint8_t>(pixel[2]), std::to_integer<std::uint8_t>(pixel[3])};
                    if (fragment.color[3] == 0) {
                        pixel[3] =
                            std::byte(std::clamp(std::lround(originalPixel.alpha * (1 - removed)), 0L, 255L));
                        continue;
                    }
                    const auto originalColor = decodeColor(originalPixel);
                    PremultipliedColor output;
                    // Complementary cut/placement coverage shares the same subpixel
                    // area. Only destination coverage beyond the vacated area occludes
                    // the remaining source. With s=t, remainder+fragment recovers the
                    // original (including soft edges); s=0 is ordinary source-over.
                    const float unselected = 1 - removed;
                    const float overlap = std::max(0.0f, fragment.coverage - removed);
                    const float remaining =
                        unselected -
                        (fragment.coverage > 0 ? fragment.color[3] * overlap / fragment.coverage : 0);
                    for (std::size_t c = 0; c < 4; ++c)
                        output[c] = fragment.color[c] + originalColor[c] * remaining;
                    const auto encoded = encodeColor(output);
                    pixel[0] = std::byte(encoded.red);
                    pixel[1] = std::byte(encoded.green);
                    pixel[2] = std::byte(encoded.blue);
                    pixel[3] = std::byte(encoded.alpha);
                }
        }
    };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
    if (!boundedParallel(regions.size() < 128 ? 1 : 0, evaluate,
                         [&] { return std::chrono::steady_clock::now() > deadline; }))
        evaluate(0, 1);
    // Stage all allocations and pixel production before changing the live owner.
    // Start every candidate from the immutable captured source, not the last
    // preview's tiles. A far-away destination can leave tiles outside a later
    // shrunken surface; retaining those would resurrect old fragments when a
    // subsequent move expands the bounding rectangle across them again.
    auto candidate = std::make_shared<RegionalRasterSurface>(source_, originalOrigin_);
    auto state = initialState_;
    state.bounds = bounds;
    candidate->restore(std::move(state));
    (void)candidate->replaceRgba8Batch(patches);
    // Admit the normal downstream evaluator before publishing a larger raw
    // surface; a failed filter allocation must not strand a cut preview.
    auto styled = *document_->layer(layer_);
    styled.payload = RasterLayer{candidate};
    styled.rasterOrigin = candidate->origin();
    (void)preflightLayerSpatialFilters(styled);
    if (hasActiveLayerEffects(effects_) &&
        (evaluatedSupport.width > 32768 || evaluatedSupport.height > 32768 ||
         evaluatedSupport.width * evaluatedSupport.height >
             double(FilterPreparationOptions{}.byteBudget / 32)))
        throw std::length_error("Transformed pixels exceed the effect working-memory budget");
    working_->restore(candidate->state());
    document_->setLayerRasterStorage(
        layer_, working_, working_->origin(),
        originalEffectFrame_.value_or(RectD{originalOrigin_.x, originalOrigin_.y,
                                            double(originalBounds_.width), double(originalBounds_.height)}));
    mapping_ = requested;
    rememberTarget();
    return true;
}
bool SelectedPixelTransformSession::completeAction()
{
    if (!targetAvailable())
        return false;
    const auto delta = composeTransform(mapping_, *actionMapping_.inverted());
    if (identity(delta))
        return false;
    const bool original = identity(mapping_);
    auto transformed = original ? selection_ : selection_->transformed(mapping_);
    PixelState before{
        actionWasOriginal_ ? source_ : working_,
        actionWasOriginal_ ? std::nullopt : std::optional(actionState_),
        actionWasOriginal_ ? originalOrigin_
                           : Vec2d{double(actionState_.bounds.x), double(actionState_.bounds.y)},
        actionWasOriginal_ ? originalEffectFrame_
                           : std::optional(originalEffectFrame_.value_or(
                                 RectD{originalOrigin_.x, originalOrigin_.y, double(originalBounds_.width),
                                       double(originalBounds_.height)})),
        expectedSelection_,
        pending_.canUndo() ? SelectionEvidenceState{} : evidence_,
        document_->lastSelection()};
    const auto *layer = document_->layer(layer_);
    PixelState after{original ? source_ : working_,
                     original ? std::nullopt : std::optional(working_->state()),
                     layer->rasterOrigin,
                     layer->rasterEffectFrame,
                     transformed,
                     original ? evidence_ : SelectionEvidenceState{},
                     original ? originalLastSelection_ : transformed};
    auto command = std::unique_ptr<Command>(
        std::make_unique<PixelTransformCommand>(layer_, external_, std::move(before), std::move(after)));
    if (command->memoryCost() + pending_.memoryUsed() > budget_)
        throw std::length_error("Transform gestures exceed the pending history "
                                "budget; apply or undo before continuing");
    checkpoints_.reserve(pending_.undoDepth() + 2);
    storageCheckpoints_.reserve(pending_.undoDepth() + 2);
    auto nextActionState = working_->state(); // Allocate before history publication.
    auto nextCheckpoint = nextActionState;
    const auto previousSelection = expectedSelection_;
    const auto previousEvidence = document_->selectionEvidence();
    document_->setSelection(transformed, original ? evidence_ : SelectionEvidenceState{});
    expectedSelection_ = document_->selection();
    try {
        if (!pending_.adoptApplied(*document_, command)) {
            document_->setSelection(previousSelection, previousEvidence);
            expectedSelection_ = document_->selection();
            return false;
        }
    } catch (...) {
        document_->setSelection(previousSelection, previousEvidence);
        expectedSelection_ = document_->selection();
        throw;
    }
    checkpoints_.resize(pending_.undoDepth());
    document_->setLastSelection(original ? originalLastSelection_ : transformed);
    checkpoints_.push_back(mapping_);
    storageCheckpoints_.resize(pending_.undoDepth());
    storageCheckpoints_.push_back(std::move(nextCheckpoint));
    actionMapping_ = mapping_;
    actionState_ = std::move(nextActionState);
    actionWasOriginal_ = original;
    rememberTarget();
    return true;
}
bool SelectedPixelTransformSession::undo()
{
    if (!targetAvailable() || !pending_.canUndo())
        return false;
    auto nextState = storageCheckpoints_[pending_.undoDepth() - 1];
    if (!pending_.undo(*document_))
        return false;
    expectedSelection_ = document_->selection();
    mapping_ = actionMapping_ = checkpoints_[pending_.undoDepth()];
    actionState_ = std::move(nextState);
    actionWasOriginal_ = identity(mapping_);
    rememberTarget();
    return true;
}
bool SelectedPixelTransformSession::redo()
{
    if (!targetAvailable() || !pending_.canRedo())
        return false;
    auto nextState = storageCheckpoints_[pending_.undoDepth() + 1];
    if (!pending_.redo(*document_))
        return false;
    expectedSelection_ = document_->selection();
    mapping_ = actionMapping_ = checkpoints_[pending_.undoDepth()];
    actionState_ = std::move(nextState);
    actionWasOriginal_ = identity(mapping_);
    rememberTarget();
    return true;
}
TransformCommitResult SelectedPixelTransformSession::commit(History &history)
{
    if (!targetAvailable())
        return TransformCommitResult::TargetUnavailable;
    completeAction();
    if (identity(mapping_)) {
        cancel();
        return TransformCommitResult::NoChange;
    }
    if (!pending_.canUndo()) {
        cancel();
        return TransformCommitResult::NoChange;
    }
    if (!history.publishAppliedBranch(*document_, pending_))
        return TransformCommitResult::TargetUnavailable;
    active_ = false;
    return TransformCommitResult::Committed;
}
void SelectedPixelTransformSession::cancel() noexcept
{
    if (!active_)
        return;
    if (targetAvailable()) {
        document_->setLayerRasterStorage(layer_, source_, originalOrigin_, originalEffectFrame_);
        document_->setSelection(selection_, evidence_);
        document_->setLastSelection(originalLastSelection_);
        document_->restoreContentState(contentState_);
    }
    active_ = false;
}
std::size_t SelectedPixelTransformSession::temporaryBytes() const noexcept
{
    auto bytes = fragment_.capacity() + coverage_.capacity() * sizeof(float);
    for (const auto &mip : mips_)
        bytes += mip.pixels.capacity() * sizeof(Sample);
    return bytes;
}
} // namespace imageeditor::core
