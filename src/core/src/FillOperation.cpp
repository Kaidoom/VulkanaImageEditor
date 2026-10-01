#include "imageeditor/core/FillOperation.hpp"
#include "imageeditor/core/ColorMath.hpp"
#include "imageeditor/core/LayerGeometry.hpp"
#include <cstring>
#include <stdexcept>

namespace imageeditor::core {
FillOperation::FillOperation(Document& document, LayerId layer, FillOptions options)
    : document_(document)
    , layer_(layer)
    , options_(options)
    , canvas_(document.canvas().extent)
    , selection_(document.selection())
{
    try {
        const auto* target = document.layer(layer);
        if (!target || !std::holds_alternative<RasterLayer>(target->payload))
            throw std::invalid_argument("Fill requires a raster layer");
        surface_ = std::get<RasterLayer>(target->payload).surface;
        mapping_ = intrinsicTransform(*target);
        const auto inverse = mapping_.inverted();
        if (!surface_ || !inverse || !std::isfinite(options.opacity))
            throw std::invalid_argument("Fill target has invalid geometry or opacity");
        extent_ = surface_->extent();
        expectedRevision_ = surface_->revision();
        if (std::uint64_t(extent_.width) * extent_.height > 64'000'000 || extent_.width > 65536
            || extent_.height > 65536)
            throw std::length_error("Fill exceeds the 64 megapixel work limit");
        options_.opacity = std::clamp(options.opacity, 0.0, 1.0);
        const auto docBounds
            = selection_ ? selection_->bounds() : RectI { 0, 0, int(canvas_.width), int(canvas_.height) };
        double left = double(extent_.width), top = double(extent_.height), right = 0, bottom = 0;
        for (const auto p : { Vec2d { double(docBounds.x), double(docBounds.y) },
                 Vec2d { double(docBounds.right()), double(docBounds.y) },
                 Vec2d { double(docBounds.right()), double(docBounds.bottom()) },
                 Vec2d { double(docBounds.x), double(docBounds.bottom()) } }) {
            const auto local = inverse->map(p);
            left = std::min(left, local.x);
            top = std::min(top, local.y);
            right = std::max(right, local.x);
            bottom = std::max(bottom, local.y);
        }
        // Crop is a visibility gate, not fractional source alpha. Bound work
        // to its intersecting texels; the transaction pins the same rectangle.
        const bool clearLayer = options_.eraseSelection && !selection_;
        if (clearLayer) { left=top=0; right=extent_.width; bottom=extent_.height; }
        if (target->crop && !clearLayer) {
            left=std::max(left,target->crop->x-target->rasterOrigin.x);top=std::max(top,target->crop->y-target->rasterOrigin.y);
            right=std::min(right,target->crop->right()-target->rasterOrigin.x);bottom=std::min(bottom,target->crop->bottom()-target->rasterOrigin.y);
        }
        const auto l = int(std::floor(std::clamp(left, 0.0, double(extent_.width))));
        const auto t = int(std::floor(std::clamp(top, 0.0, double(extent_.height))));
        const auto r = int(std::ceil(std::clamp(right, 0.0, double(extent_.width))));
        const auto b = int(std::ceil(std::clamp(bottom, 0.0, double(extent_.height))));
        // Admit only intersecting fixed journal tiles. Exact texel-center
        // clipping still happens below; small selections do not scan a 5K layer.
        workBounds_
            = { l / 64 * 64, t / 64 * 64, std::max(0, r - l / 64 * 64), std::max(0, b - t / 64 * 64) };
        transaction_ = std::make_unique<RasterEditTransaction>(document, layer, std::string(label()),
            RasterEditTransactionOptions{.ignoreCrop=clearLayer,.coverageValues=options_.coverageValues});
        state_ = FillState::Ready;
        if ((!options_.eraseSelection && (!options_.color.alpha || options_.opacity == 0)) || workBounds_.empty()
            || (selection_ && selection_->bounds().empty()))
            return;
        state_ = FillState::Applying;
        if (!options_.eraseSelection && options_.mode == FillMode::Contiguous) {
            if (!std::isfinite(options_.seed.x) || !std::isfinite(options_.seed.y) || options_.seed.x < 0
                || options_.seed.y < 0 || options_.seed.x >= canvas_.width
                || options_.seed.y >= canvas_.height) {
                state_ = FillState::Ready;
                return;
            }
            const auto seed = inverse->map(options_.seed);
            if (!std::isfinite(seed.x) || !std::isfinite(seed.y) || seed.x < 0 || seed.y < 0
                || seed.x >= extent_.width || seed.y >= extent_.height) {
                state_ = FillState::Ready;
                return;
            }
            region_ = std::make_unique<RegionFinder>(extent_, int(std::floor(seed.x)),
                int(std::floor(seed.y)), options_.tolerance, [this](int x, int y) -> std::optional<Rgba8> {
                    if (!eligible(x, y))
                        return std::nullopt;
                    return source(x, y);
                });
            state_ = FillState::Discovering;
        }
    } catch (const std::exception& e) {
        error_ = e.what();
        state_ = FillState::Failed;
    }
}
FillOperation::~FillOperation() { cancel(); }
bool FillOperation::eligible(int x, int y) const
{
    if (options_.eraseSelection && !selection_) return true;
    if(!transaction_ || !transaction_->cropAllows(x,y))return false;
    const auto p = mapping_.map({ x + 0.5, y + 0.5 });
    return std::isfinite(p.x) && std::isfinite(p.y) && p.x >= 0 && p.y >= 0 && p.x < canvas_.width
        && p.y < canvas_.height
        && (!selection_
            || selection_->coverageAtDocumentPixel(int(std::floor(p.x)), int(std::floor(p.y))) > 0);
}
bool FillOperation::targetValid() const
{
    const auto* layer = document_.layer(layer_);
    return transaction_ && transaction_->targetAvailable() && layer && std::holds_alternative<RasterLayer>(layer->payload)
        && std::get<RasterLayer>(layer->payload).surface == surface_ && intrinsicTransform(*layer) == mapping_
        && document_.canvas().extent == canvas_ && document_.selection() == selection_
        && surface_->revision() == expectedRevision_;
}
Rgba8 FillOperation::source(int x, int y)
{
    auto& row = rows_[std::size_t(y) % rows_.size()];
    if (row.y != y) {
        row.bytes.resize(std::size_t(extent_.width) * 4);
        surface_->copyRgba8({ 0, y, int(extent_.width), 1 }, row.bytes, row.bytes.size());
        row.y = y;
    }
    const auto* p = row.bytes.data() + std::size_t(x) * 4;
    return { std::to_integer<std::uint8_t>(p[0]), std::to_integer<std::uint8_t>(p[1]),
        std::to_integer<std::uint8_t>(p[2]), std::to_integer<std::uint8_t>(p[3]) };
}
Rgba8 FillOperation::composite(Rgba8 before)
{
    if(options_.coverageValues) {
        const double amount=options_.eraseSelection?1:options_.opacity*double(options_.color.alpha)/255;
        const double target=options_.eraseSelection?0:maskGray(options_.color);
        const auto value=std::uint8_t(std::lround(before.red+(target-before.red)*amount));
        return {value,value,value,255};
    }
    if (options_.eraseSelection) return {before.red, before.green, before.blue, 0};
    auto& table = tables_[before.alpha];
    if (!table) {
        table = std::make_unique<CompositeTable>();
        const auto sourceAlpha = double(options_.color.alpha) / 255 * options_.opacity;
        const auto remaining = double(before.alpha) / 255 * (1 - sourceAlpha);
        const auto alpha = sourceAlpha + remaining;
        table->alpha = alphaToByte(alpha);
        const std::array channels { options_.color.red, options_.color.green, options_.color.blue };
        for (std::size_t c = 0; c < 3; ++c)
            for (int i = 0; i < 256; ++i)
                table->rgb[c][std::size_t(i)] = linearToSrgb(
                    (srgbToLinear(channels[c]) * sourceAlpha + srgbToLinear(std::uint8_t(i)) * remaining)
                    / alpha);
    }
    // Sub-byte alpha cannot affect the visible RGBA8 result. Preserve hidden
    // RGB instead of inventing an invisible history entry on transparent pixels.
    if (!table->alpha)
        return before;
    return { table->rgb[0][before.red], table->rgb[1][before.green], table->rgb[2][before.blue],
        table->alpha };
}
FillState FillOperation::step(std::size_t budget)
{
    if (state_ != FillState::Discovering && state_ != FillState::Applying)
        return state_;
    try {
        if (!targetValid())
            throw std::runtime_error("Fill target changed before completion");
        if (state_ == FillState::Discovering) {
            if (region_->step(budget)) {
                state_ = region_->pixelCount() ? FillState::Applying : FillState::Ready;
                const auto b = region_->bounds();
                workBounds_ = { b.x / 64 * 64, b.y / 64 * 64, b.right() - b.x / 64 * 64,
                    b.bottom() - b.y / 64 * 64 };
            }
            stats_.discoveryBytes = region_->memoryBytes();
            return state_;
        }
        // Fixed 64px working patches; no full-layer candidate or stroke copy.
        struct Patch {
            RectI rect;
            std::vector<std::byte> bytes;
        };
        std::vector<Patch> owned;
        const auto columns = std::uint32_t(workBounds_.width + 63) / 64,
                   rows = std::uint32_t(workBounds_.height + 63) / 64;
        const auto total = std::uint64_t(columns) * rows;
        std::size_t work = 0;
        while (nextTile_ < total && work < std::max(std::size_t(4096), budget)) {
            const auto tx = workBounds_.x + int(nextTile_ % columns) * 64,
                       ty = workBounds_.y + int(nextTile_ / columns) * 64;
            ++nextTile_;
            const RectI rect { tx, ty, std::min(64, int(extent_.width) - tx),
                std::min(64, int(extent_.height) - ty) };
            const auto stride = std::size_t(rect.width) * 4;
            Patch patch { rect, std::vector<std::byte>(stride * std::size_t(rect.height)) };
            surface_->copyRgba8(rect, patch.bytes, stride);
            bool changed = false;
            for (int y = 0; y < rect.height; ++y)
                for (int x = 0; x < rect.width; ++x) {
                    ++work;
                    ++stats_.evaluatedPixels;
                    if ((region_ && !region_->contains(tx + x, ty + y)) || !eligible(tx + x, ty + y))
                        continue;
                    auto* p = patch.bytes.data() + std::size_t(y) * stride + std::size_t(x) * 4;
                    const Rgba8 before { std::to_integer<std::uint8_t>(p[0]),
                        std::to_integer<std::uint8_t>(p[1]), std::to_integer<std::uint8_t>(p[2]),
                        std::to_integer<std::uint8_t>(p[3]) };
                    const auto after = composite(before);
                    if (before == after)
                        continue;
                    p[0] = std::byte(after.red);
                    p[1] = std::byte(after.green);
                    p[2] = std::byte(after.blue);
                    p[3] = std::byte(after.alpha);
                    changed = true;
                    ++stats_.candidatePixels;
                }
            if (changed)
                owned.push_back(std::move(patch));
        }
        if (!owned.empty()) {
            std::vector<RasterPatch> patches;
            for (const auto& p : owned)
                patches.push_back({ p.rect, p.bytes, std::size_t(p.rect.width) * 4 });
            const auto dirty = transaction_->writeRgba8Batch(patches);
            expectedRevision_ = surface_->revision();
            if (!dirty.empty())
                ++stats_.writeBatches;
            stats_.journalBytes = transaction_->capturedPixelBytes();
        }
        if (nextTile_ == total)
            state_ = FillState::Ready;
    } catch (const std::exception& e) {
        error_ = e.what();
        if (transaction_)
            transaction_->cancel();
        state_ = FillState::Failed;
    }
    return state_;
}
RasterEditCommitResult FillOperation::commit(History& history)
{
    if (state_ != FillState::Ready || !transaction_)
        return RasterEditCommitResult::TargetUnavailable;
    if (!targetValid()) {
        cancel();
        return RasterEditCommitResult::TargetUnavailable;
    }
    try {
        const auto result = transaction_->commit(history);
        state_ = FillState::Finished;
        return result;
    } catch (const std::exception& e) {
        error_ = e.what();
        transaction_->cancel();
        state_ = FillState::Failed;
        return RasterEditCommitResult::HistoryRejected;
    }
}
void FillOperation::cancel() noexcept
{
    if (transaction_)
        transaction_->cancel();
    if (state_ != FillState::Finished && state_ != FillState::Failed)
        state_ = FillState::Cancelled;
}
}
