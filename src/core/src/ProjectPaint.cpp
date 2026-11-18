#include "imageeditor/core/ProjectPaint.hpp"
#include "imageeditor/core/LayerGeometry.hpp"
#include "imageeditor/core/ProjectLayer.hpp"
#include "ReadOnlyRasterSurface.hpp"

#include <cstring>
#include <functional>
#include <stdexcept>

namespace imageeditor::core {
namespace {
using Surface = std::shared_ptr<RasterSurface>;
using Key = ProjectPaint::Tiles::key_type;
using Tile = ProjectPaint::Tile;
constexpr int side = ProjectPaint::tileSide;
static_assert(sizeof(Rgba8) == 4);

RectI bounds(const RasterSurface& surface)
{
    return { 0, 0, int(surface.extent().width), int(surface.extent().height) };
}
RectI tileBounds(Key k) { return { k.first * side, k.second * side, side, side }; }
size_t offset(int x, int y) { return size_t((y % side) * side + x % side); }
template <class F> void eachTile(RectI r, F visit)
{
    if (r.empty())
        return;
    for (int ty = r.y / side; ty <= (r.bottom() - 1) / side; ++ty)
        for (int tx = r.x / side; tx <= (r.right() - 1) / side; ++tx) {
            Key key { tx, ty };
            visit(key, r.clippedTo(tileBounds(key)));
        }
}
void validateBuffer(RectI r, size_t bytes, size_t stride)
{
    const size_t width = size_t(r.width) * sizeof(Rgba8);
    if (stride < width || bytes < width || size_t(r.height - 1) > (bytes - width) / stride)
        throw std::invalid_argument("Project paint buffer is too small");
}
bool emptyTile(const Tile& tile)
{
    return std::ranges::none_of(tile, [](Rgba8 p) { return p.alpha != 0; });
}
bool equalPaint(const ProjectPaintState& a, const ProjectPaintState& b)
{
    if (a == b)
        return true;
    if (!a || !b)
        return (a ? a : b)->tiles.empty();
    if (a->tiles.size() != b->tiles.size())
        return false;
    return std::ranges::equal(a->tiles, b->tiles, [](const auto& x, const auto& y) {
        return x.first == y.first && (x.second == y.second || *x.second == *y.second);
    });
}
Surface linkedSurface(const Layer* layer)
{
    const auto* r = layer && layer->projectSource ? std::get_if<RasterLayer>(&layer->payload) : nullptr;
    return r ? r->surface : nullptr;
}

// Published views are immutable. The renderer can upload just the changed tiles
// within a stroke. A frozen snapshot or a duplicate keeps its original view.
class PaintedSurface final : public ReadOnlyRasterSurface {
public:
    PaintedSurface(Surface base, ProjectPaintState paint, SurfaceId id, DirtySet dirty, RectI strokeBounds = { })
        : ReadOnlyRasterSurface(std::move(base))
        , paint_(std::move(paint))
        , id_(id)
        , dirty_(std::move(dirty))
        , strokeBounds_(strokeBounds)
    {
    }
    SurfaceId id() const noexcept override { return id_; }
    Revision revision() const noexcept override { return dirty_.revision; }
    DirtySet dirtySince(Revision previous) const override
    {
        if (previous == revision())
            return { revision(), false, { } };
        if (previous && previous == revision() - 1)
            return dirty_;
        if (previous && previous < revision() && !strokeBounds_.empty())
            return { revision(), false, { strokeBounds_ } };
        return { revision(), true, { } };
    }
    const ProjectPaintState& paint() const noexcept { return paint_; }
    size_t allocatedPixelBytes() const noexcept override
    {
        return source()->allocatedPixelBytes() + paint_->tiles.size() * sizeof(Tile);
    }
    size_t memoryCost() const noexcept override
    {
        return sizeof(*this) + source()->memoryCost() + paint_->tiles.size() * (sizeof(Tile) + 64);
    }
    void copyRgba8(RectI r, std::span<std::byte> out, size_t stride) const override
    {
        r = r.clippedTo(bounds(*this));
        if (r.empty())
            return;
        source()->copyRgba8(r, out, stride);
        eachTile(r, [&](Key key, RectI part) {
            const auto it = paint_->tiles.find(key);
            if (it == paint_->tiles.end())
                return;
            for (int y = part.y; y < part.bottom(); ++y) {
                auto* dest = out.data() + size_t(y - r.y) * stride + size_t(part.x - r.x) * 4;
                const auto* ink = it->second->data() + offset(part.x, y);
                for (int x = part.x; x < part.right(); ++x, ++ink, dest += 4) {
                    if (!ink->alpha)
                        continue;
                    Rgba8 result = *ink;
                    if (ink->alpha != 255) {
                        Rgba8 base;
                        std::memcpy(&base, dest, 4);
                        if (base.alpha)
                            result = encodeColor(
                                compositeLayer(decodeColor(base), decodeColor(*ink), 1, BlendMode::Normal));
                    }
                    std::memcpy(dest, &result, 4);
                }
            }
        });
    }
    void copyNative(RectI r, std::span<std::byte> out, size_t stride) const override { copyRgba8(r, out, stride); }

    RectI contentBounds() const override
    {
        std::call_once(boundsOnce_, [&] {
            paintBounds_ = source()->contentBounds();
            const auto full = bounds(*this);
            if (paintBounds_ == full)
                return;
            for (const auto& [key, tile] : paint_->tiles) {
                const auto part = tileBounds(key).clippedTo(full);
                if (part.clippedTo(paintBounds_) == part)
                    continue;
                for (int y = part.y; y < part.bottom(); ++y)
                    for (int x = part.x; x < part.right(); ++x)
                        if ((*tile)[offset(x, y)].alpha)
                            paintBounds_ = paintBounds_.united({ x, y, 1, 1 });
            }
        });
        return paintBounds_;
    }

private:
    ProjectPaintState paint_;
    SurfaceId id_;
    DirtySet dirty_;
    RectI strokeBounds_;
    mutable std::once_flag boundsOnce_;
    mutable RectI paintBounds_;
};

} // namespace

// The ordinary raster brush reads and writes only the transparent overlay.
// Tiles are copied once per changed tile in a batch; zero-alpha tiles disappear.
class ProjectPaintEdit::SurfaceProxy final : public RasterSurface {
public:
    using Publish = std::function<void(ProjectPaintState, const DirtySet&)>;
    SurfaceProxy(Extent2u extent, ProjectPaintState paint, Publish publish)
        : extent_(extent)
        , paint_(std::move(paint))
        , publish_(std::move(publish))
    {
    }
    SurfaceId id() const noexcept override { return id_; }
    Revision revision() const noexcept override { return revision_; }
    Extent2u extent() const noexcept override { return extent_; }
    DirtySet dirtySince(Revision r) const override { return { revision_, r != revision_, { } }; }
    void detach() noexcept { publish_ = { }; }

    void copyRgba8(RectI r, std::span<std::byte> out, size_t stride) const override
    {
        r = r.clippedTo(bounds(*this));
        if (r.empty())
            return;
        validateBuffer(r, out.size(), stride);
        for (int y = 0; y < r.height; ++y)
            std::memset(out.data() + size_t(y) * stride, 0, size_t(r.width) * 4);
        if (!paint_)
            return;
        eachTile(r, [&](Key key, RectI part) {
            auto it = paint_->tiles.find(key);
            if (it == paint_->tiles.end())
                return;
            for (int y = part.y; y < part.bottom(); ++y)
                std::memcpy(out.data() + size_t(y - r.y) * stride + size_t(part.x - r.x) * 4,
                    it->second->data() + offset(part.x, y), size_t(part.width) * 4);
        });
    }
    DirtySet replaceRgba8Batch(std::span<const RasterPatch> patches) override
    {
        std::map<Key, std::shared_ptr<Tile>> touched;
        for (const auto& p : patches) {
            const auto r = p.region;
            if (r.empty())
                continue;
            if (r.clippedTo(bounds(*this)) != r)
                throw std::out_of_range("Stroke outside project layer");
            validateBuffer(r, p.rgbaBytes.size(), p.stride);
            eachTile(r, [&](Key key, RectI part) {
                auto [it, fresh] = touched.try_emplace(key);
                if (fresh) {
                    it->second = std::make_shared<Tile>();
                    if (paint_) {
                        const auto old = paint_->tiles.find(key);
                        if (old != paint_->tiles.end())
                            *it->second = *old->second;
                    }
                }
                for (int y = part.y; y < part.bottom(); ++y)
                    std::memcpy(it->second->data() + offset(part.x, y),
                        p.rgbaBytes.data() + size_t(y - r.y) * p.stride + size_t(part.x - r.x) * 4,
                        size_t(part.width) * 4);
            });
        }
        std::shared_ptr<ProjectPaint> next;
        DirtySet dirty { revision_, false, { } };
        for (auto& [key, tile] : touched) {
            // The brush may leave RGB behind after erasure. The overlay has no
            // content at zero coverage, including for equality and serialization.
            for (auto& p : *tile)
                if (!p.alpha)
                    p = { };
            const bool empty = emptyTile(*tile);
            if (paint_ && paint_->tiles.contains(key)) {
                if (*paint_->tiles.at(key) == *tile)
                    continue;
            } else if (empty)
                continue;
            if (!next)
                next = paint_ ? std::make_shared<ProjectPaint>(*paint_) : std::make_shared<ProjectPaint>();
            if (empty)
                next->tiles.erase(key);
            else
                next->tiles[key] = std::move(tile);
            dirty.regions.push_back(tileBounds(key).clippedTo(bounds(*this)));
        }
        if (!next)
            return dirty;
        if (!publish_)
            throw std::logic_error("Project paint edit has ended");
        dirty.revision = revision_ + 1;
        if (next->tiles.empty())
            next.reset();
        publish_(next, dirty);
        paint_ = std::move(next);
        revision_ = dirty.revision;
        return dirty;
    }
    DirtySet swapRgba8Batch(std::span<MutableRasterPatch> patches) override
    {
        std::vector<std::vector<std::byte>> original;
        std::vector<RasterPatch> writes;
        for (const auto& p : patches) {
            if (p.region.empty()) {
                original.emplace_back();
                continue;
            }
            if (p.region.clippedTo(bounds(*this)) != p.region)
                throw std::out_of_range("Stroke outside project layer");
            validateBuffer(p.region, p.rgbaBytes.size(), p.stride);
            const size_t stride = size_t(p.region.width) * 4;
            auto& bytes = original.emplace_back(stride * size_t(p.region.height));
            copyRgba8(p.region, bytes, stride);
            writes.push_back({ p.region, p.rgbaBytes, p.stride });
        }
        auto dirty = replaceRgba8Batch(writes);
        for (size_t i = 0; i < patches.size(); ++i) {
            const auto& p = patches[i];
            if (p.region.empty())
                continue;
            const size_t stride = size_t(p.region.width) * 4;
            for (int y = 0; y < p.region.height; ++y)
                std::memcpy(
                    p.rgbaBytes.data() + size_t(y) * p.stride, original[i].data() + size_t(y) * stride, stride);
        }
        return dirty;
    }

private:
    Extent2u extent_;
    ProjectPaintState paint_;
    Publish publish_;
    SurfaceId id_ { makeSurfaceId() };
    Revision revision_ { 1 };
};

namespace {
class PaintCommand final : public Command {
public:
    PaintCommand(LayerId layer, Surface before, Surface after, std::string label)
        : layer_(layer)
        , before_(std::move(before))
        , after_(std::move(after))
        , label_(std::move(label))
    {
        const auto a = projectPaintState(before_), b = projectPaintState(after_);
        for (const auto& state : { a, b }) {
            if (!state)
                continue;
            const auto& other = state == a ? b : a;
            cost_ += sizeof(ProjectPaint) + state->tiles.size() * 64;
            for (const auto& [key, tile] : state->tiles)
                if (!other || !other->tiles.contains(key) || other->tiles.at(key) != tile)
                    cost_ += sizeof(Tile);
        }
    }
    bool apply(Document& doc) override { return doc.setProjectPaintSurface(layer_, before_, after_); }
    bool undo(Document& doc) override { return doc.setProjectPaintSurface(layer_, after_, before_); }
    bool canAdoptApplied(const Document& doc) const noexcept override
    {
        return linkedSurface(doc.layer(layer_)) == after_;
    }
    std::string_view label() const noexcept override { return label_; }
    size_t memoryCost() const noexcept override { return sizeof(*this) + label_.capacity() + cost_; }

private:
    LayerId layer_;
    Surface before_, after_;
    std::string label_;
    size_t cost_ { };
};
} // namespace

ProjectPaintState projectPaintState(const Surface& combined)
{
    const auto* painted = dynamic_cast<const PaintedSurface*>(combined.get());
    return painted ? painted->paint() : nullptr;
}
Surface projectBasePixels(const Surface& combined)
{
    const auto* painted = dynamic_cast<const PaintedSurface*>(combined.get());
    return painted ? std::const_pointer_cast<RasterSurface>(painted->source()) : combined;
}
Surface withProjectPaint(Surface base, ProjectPaintState paint)
{
    base = readOnlyProjectSurface(projectBasePixels(base));
    if (!paint || paint->tiles.empty())
        return base;
    return std::make_shared<PaintedSurface>(
        std::move(base), std::move(paint), makeSurfaceId(), DirtySet { 1, true, { } });
}
bool hasProjectPaint(const Layer& layer) { return bool(projectPaintState(linkedSurface(&layer))); }

bool Document::setProjectPaintSurface(LayerId id, const Surface& expected, const Surface& replacement) noexcept
{
    auto* target = layer(id);
    if (!expected || !replacement || replacement == expected || linkedSurface(target) != expected
        || projectBasePixels(expected) != projectBasePixels(replacement))
        return false;
    std::get<RasterLayer>(target->payload).surface = replacement;
    target->filterCache.reset();
    target->effectCache.reset();
    touch();
    return true;
}

std::unique_ptr<Command> makeProjectPaintCommand(
    const Document& doc, LayerId id, ProjectPaintState after, std::string label)
{
    const auto before = linkedSurface(doc.layer(id));
    if (!before || equalPaint(projectPaintState(before), after))
        return { };
    return std::make_unique<PaintCommand>(id, before, withProjectPaint(before, std::move(after)), std::move(label));
}
std::unique_ptr<Command> makeProjectPaintClearCommand(const Document& doc, LayerId id, bool useSelection)
{
    const auto* layer = doc.layer(id);
    const auto surface = linkedSurface(layer);
    const auto paint = projectPaintState(surface);
    if (!paint)
        return { };
    const auto selection = useSelection ? doc.selection() : nullptr;
    if (!selection)
        return makeProjectPaintCommand(doc, id, { }, "Clear paint overlay");
    auto next = std::make_shared<ProjectPaint>(*paint);
    const auto transform = intrinsicTransform(*layer);
    auto crop = layer->crop;
    if (crop) {
        crop->x -= layer->rasterOrigin.x;
        crop->y -= layer->rasterOrigin.y;
    }
    const auto canvas = doc.canvas().extent;
    for (const auto& [key, tile] : paint->tiles) {
        const auto r = tileBounds(key).clippedTo(bounds(*surface));
        std::shared_ptr<Tile> edited;
        for (int y = r.y; y < r.bottom(); ++y)
            for (int x = r.x; x < r.right(); ++x) {
                const auto i = offset(x, y);
                const auto& p = (*tile)[i];
                if (!p.alpha || !cropAllowsTexel(crop, x, y))
                    continue;
                const auto point = transform.map({ x + .5, y + .5 });
                if (!(point.x >= 0 && point.y >= 0 && point.x < canvas.width && point.y < canvas.height))
                    continue;
                const auto coverage = selection->coverageAtDocumentPixel(int(point.x), int(point.y));
                const auto alpha = std::uint8_t((p.alpha * (255 - coverage) + 127) / 255);
                if (alpha == p.alpha)
                    continue;
                if (!edited)
                    edited = std::make_shared<Tile>(*tile);
                (*edited)[i] = alpha ? Rgba8 { p.red, p.green, p.blue, alpha } : Rgba8 { };
            }
        if (!edited)
            continue;
        if (emptyTile(*edited))
            next->tiles.erase(key);
        else
            next->tiles[key] = std::move(edited);
    }
    return makeProjectPaintCommand(doc, id, std::move(next), "Clear selected project paint");
}

ProjectPaintEdit::ProjectPaintEdit(Document& owner, LayerId id, std::string label)
    : owner_(owner)
    , id_(id)
    , before_(linkedSurface(owner.layer(id)))
    , current_(before_)
    , selection_(owner.selection())
    , label_(std::move(label))
    , proxy_(owner.canvas())
{
    if (!before_)
        throw std::invalid_argument("Choose a linked project layer to paint");
    const auto& layer = *owner.layer(id);
    transform_ = intrinsicTransform(layer);
    source_ = layer.projectSource;
    editable_ = std::make_shared<SurfaceProxy>(before_->extent(), projectPaintState(before_),
        [this, base = projectBasePixels(before_), viewId = makeSurfaceId(), strokeBounds = RectI { }](
            ProjectPaintState paint, const DirtySet& dirty) mutable {
            if (!valid())
                throw std::runtime_error("Project layer changed during painting");
            for (auto r : dirty.regions)
                strokeBounds = strokeBounds.united(r);
            Surface next = paint
                ? std::make_shared<PaintedSurface>(base, std::move(paint), viewId, dirty, strokeBounds)
                : base;
            if (next != current_ && !owner_.setProjectPaintSurface(id_, current_, next))
                throw std::runtime_error("Could not update project paint overlay");
            current_ = std::move(next);
        });
    auto proxy = Layer::raster(layer.name, editable_);
    proxy.id = id;
    proxy.localToDocument = layer.localToDocument;
    proxy.rasterOrigin = layer.rasterOrigin;
    proxy.crop = layer.crop;
    if (!proxy_.insertLayer(0, std::move(proxy)))
        throw std::runtime_error("Could not prepare project paint");
    proxy_.setSelection(selection_);
}
ProjectPaintEdit::~ProjectPaintEdit()
{
    cancel();
    editable_->detach();
}
bool ProjectPaintEdit::valid() const noexcept
{
    const auto* l = owner_.layer(id_);
    const auto* proxyLayer = proxy_.layer(id_);
    return !finished_ && l && proxyLayer && linkedSurface(l) == current_ && l->projectSource == source_
        && intrinsicTransform(*l) == transform_ && owner_.selection() == selection_ && l->crop == proxyLayer->crop
        && owner_.canvas().extent == proxy_.canvas().extent;
}
RasterEditCommitResult ProjectPaintEdit::commit(History& history)
{
    if (!valid()) {
        cancel();
        return RasterEditCommitResult::TargetUnavailable;
    }
    if (equalPaint(projectPaintState(before_), projectPaintState(current_))) {
        cancel();
        return RasterEditCommitResult::NoChanges;
    }
    std::unique_ptr<Command> command = std::make_unique<PaintCommand>(id_, before_, current_, label_);
    if (!history.adoptApplied(owner_, command)) {
        cancel();
        return RasterEditCommitResult::HistoryRejected;
    }
    finished_ = true;
    return RasterEditCommitResult::Committed;
}
void ProjectPaintEdit::cancel() noexcept
{
    if (finished_)
        return;
    if (before_ != current_)
        owner_.setProjectPaintSurface(id_, current_, before_);
    finished_ = true;
}
} // namespace imageeditor::core
