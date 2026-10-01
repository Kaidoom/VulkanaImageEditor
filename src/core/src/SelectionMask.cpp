#include "imageeditor/core/SelectionMask.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>
#include <tuple>

namespace imageeditor::core {
SelectionState SelectionMask::replacedR8(std::span<const CoveragePatch> patches) const
{
    auto next=std::shared_ptr<SelectionMask>(new SelectionMask(extent_));
    next->tiles_=tiles_;
    std::map<std::size_t,std::shared_ptr<Pixels>> changed;
    for (const auto& patch:patches) {
        const auto r=patch.region;
        if(r.empty())continue;
        if(r.x<0||r.y<0||r.right()>int(extent_.width)||r.bottom()>int(extent_.height)
            ||patch.stride<std::size_t(r.width)||patch.bytes.size()<patch.stride*std::size_t(r.height-1)+std::size_t(r.width))
            throw std::invalid_argument("Invalid coverage patch");
        for(int y=0;y<r.height;++y)for(int x=0;x<r.width;++x) {
            const auto px=std::uint32_t(r.x+x),py=std::uint32_t(r.y+y);
            const auto index=std::size_t(py/tileSize)*columns_+px/tileSize;
            const auto tx=px%tileSize,ty=py%tileSize;
            const auto value=patch.bytes[std::size_t(y)*patch.stride+std::size_t(x)];
            auto found=changed.find(index);
            if((found==changed.end()?tiles_[index].at(tx,ty):(*found->second)[ty*tileSize+tx])==value)continue;
            if(found==changed.end()) {
                auto pixels=std::make_shared<Pixels>();
                if(tiles_[index].pixels)*pixels=*tiles_[index].pixels;else pixels->fill(tiles_[index].uniform);
                found=changed.emplace(index,std::move(pixels)).first;
            }
            (*found->second)[ty*tileSize+tx]=value;
        }
    }
    if(changed.empty())return {};
    for(const auto& [index,pixels]:changed) {
        const auto r=tileRect(index);
        next->tiles_[index]=compress(pixels,{std::uint32_t(r.width),std::uint32_t(r.height)});
    }
    next->updateBounds();
    return next;
}
namespace {
std::atomic<Revision> nextSelectionRevision {1};
RectI canvasBounds(Extent2u e) { return {0, 0, std::int32_t(e.width), std::int32_t(e.height)}; }
std::uint8_t combine(std::uint8_t a, std::uint8_t b, SelectionOperation op)
{
    switch (op) {
    case SelectionOperation::Replace: return b;
    case SelectionOperation::Add: return std::max(a, b);
    case SelectionOperation::Subtract: return std::uint8_t(std::max(0, int(a) - int(b)));
    case SelectionOperation::Intersect: return std::min(a, b);
    }
    return 0;
}
bool contains(RectI r, int x, int y)
{ return x >= r.x && y >= r.y && x < r.right() && y < r.bottom(); }
void normalizeEdges(std::vector<SelectionEdge>& edges)
{
    const auto key = [](const SelectionEdge& e) {
        const bool vertical = e.from.x == e.to.x;
        return std::tuple(vertical, vertical ? e.from.x : e.from.y,
            vertical ? e.from.y : e.from.x);
    };
    std::sort(edges.begin(), edges.end(), [&](const auto& a, const auto& b) { return key(a) < key(b); });
    std::size_t count = 0;
    for (const auto edge : edges) {
        if (count) {
            auto& previous = edges[count - 1];
            const auto [v, fixed, start] = key(edge);
            const auto [pv, pfixed, pstart] = key(previous);
            (void)pstart;
            if (v == pv && fixed == pfixed && start <= (v ? previous.to.y : previous.to.x)) {
                if (v) previous.to.y = std::max(previous.to.y, edge.to.y);
                else previous.to.x = std::max(previous.to.x, edge.to.x);
                continue;
            }
        }
        edges[count++] = edge;
    }
    edges.resize(count);
}
}

SelectionMask::SelectionMask(Extent2u extent)
    : extent_(extent), columns_((extent.width + tileSize - 1) / tileSize),
      revision_(nextSelectionRevision.fetch_add(1, std::memory_order_relaxed))
{
    if (extent.empty() || extent.width > 32768 || extent.height > 32768)
        throw std::invalid_argument("Selection extent must be between 1 and 32768 pixels per axis");
    tiles_.resize(std::size_t(columns_) * ((extent.height + tileSize - 1) / tileSize));
}
RectI SelectionMask::tileRect(std::size_t index) const noexcept
{
    return RectI {std::int32_t(index % columns_ * tileSize), std::int32_t(index / columns_ * tileSize),
        int(tileSize), int(tileSize)}.clippedTo(canvasBounds(extent_));
}
SelectionMask::Tile SelectionMask::compress(std::shared_ptr<Pixels> pixels, Extent2u extent)
{
    Tile tile;
    tile.uniform = (*pixels)[0];
    bool allUniform = true;
    for (std::uint32_t y = 0; y < extent.height && allUniform; ++y) {
        const auto* row = pixels->data() + std::size_t(y) * tileSize;
        allUniform = std::all_of(row, row + extent.width, [&](auto value) { return value == tile.uniform; });
    }
    if (allUniform) {
        if (tile.uniform) tile.bounds = {0, 0, int(extent.width), int(extent.height)};
        return tile;
    }
    for (std::uint32_t y = 0; y < extent.height; ++y)
        for (std::uint32_t x = 0; x < extent.width; ++x) {
            const auto value = (*pixels)[y * tileSize + x];
            if (value) tile.bounds = tile.bounds.united({int(x), int(y), 1, 1});
        }
    tile.pixels = std::move(pixels);
    return tile;
}
void SelectionMask::updateBounds()
{
    bounds_ = {};
    for (std::size_t i = 0; i < tiles_.size(); ++i) {
        auto b = tiles_[i].bounds;
        if (b.empty()) continue;
        const auto r = tileRect(i);
        b.x += r.x; b.y += r.y;
        bounds_ = bounds_.united(b);
    }
}
SelectionState SelectionMask::filled(Extent2u extent, std::uint8_t value)
{
    auto result = std::shared_ptr<SelectionMask>(new SelectionMask(extent));
    for (std::size_t i = 0; i < result->tiles_.size(); ++i) {
        auto& tile = result->tiles_[i];
        const auto r = result->tileRect(i);
        tile.uniform = value;
        if (value) tile.bounds = {0, 0, r.width, r.height};
    }
    result->updateBounds();
    return result;
}
SelectionState SelectionMask::rectangle(Extent2u extent, RectI rect, std::uint8_t value)
{
    auto result = std::shared_ptr<SelectionMask>(new SelectionMask(extent));
    rect = rect.clippedTo(canvasBounds(extent));
    if (value) for (std::size_t i = 0; i < result->tiles_.size(); ++i) {
        const auto r = result->tileRect(i), overlap = r.clippedTo(rect);
        if (overlap.empty()) continue;
        auto& tile = result->tiles_[i];
        tile.bounds = {overlap.x - r.x, overlap.y - r.y, overlap.width, overlap.height};
        if (overlap == r) tile.uniform = value;
        else {
            auto pixels = std::make_shared<Pixels>();
            for (int y = tile.bounds.y; y < tile.bounds.bottom(); ++y)
                std::fill_n(pixels->begin() + std::ptrdiff_t(std::size_t(y) * tileSize + std::size_t(tile.bounds.x)), tile.bounds.width, value);
            tile.pixels = std::move(pixels);
        }
    }
    result->updateBounds();
    return result;
}
SelectionState SelectionMask::fromR8(Extent2u extent, std::span<const std::uint8_t> bytes, std::size_t stride)
{
    auto result = std::shared_ptr<SelectionMask>(new SelectionMask(extent));
    if (stride < extent.width || bytes.size() < extent.width
        || (extent.height > 1 && stride > (bytes.size() - extent.width) / (extent.height - 1)))
        throw std::invalid_argument("Selection R8 source is too small");
    for (std::size_t i = 0; i < result->tiles_.size(); ++i) {
        const auto r = result->tileRect(i);
        auto pixels = std::make_shared<Pixels>();
        for (int y = 0; y < r.height; ++y)
            std::copy_n(bytes.begin() + std::ptrdiff_t(std::size_t(r.y + y) * stride + std::size_t(r.x)),
                r.width, pixels->begin() + std::ptrdiff_t(std::size_t(y) * tileSize));
        result->tiles_[i] = compress(std::move(pixels), {std::uint32_t(r.width), std::uint32_t(r.height)});
    }
    result->updateBounds();
    return result;
}
SelectionState SelectionMask::fromR8Region(Extent2u extent, RectI region,
    std::span<const std::uint8_t> bytes, std::size_t stride)
{
    auto result = std::shared_ptr<SelectionMask>(new SelectionMask(extent));
    if (region.empty()) return result;
    if (std::int64_t(region.x) + region.width > std::numeric_limits<int>::max()
        || std::int64_t(region.y) + region.height > std::numeric_limits<int>::max()
        || stride < std::size_t(region.width) || bytes.size() < std::size_t(region.width)
        || (region.height > 1 && stride > (bytes.size() - std::size_t(region.width)) / std::size_t(region.height - 1)))
        throw std::invalid_argument("Selection R8 region source is invalid");
    const auto clipped = region.clippedTo(canvasBounds(extent));
    if (clipped.empty()) return result;
    for (int ty = clipped.y / int(tileSize); ty <= (clipped.bottom() - 1) / int(tileSize); ++ty)
        for (int tx = clipped.x / int(tileSize); tx <= (clipped.right() - 1) / int(tileSize); ++tx) {
            const auto index = std::size_t(ty) * result->columns_ + std::size_t(tx);
            const auto tile = result->tileRect(index), overlap = tile.clippedTo(clipped);
            auto pixels = std::make_shared<Pixels>();
            for (int y = overlap.y; y < overlap.bottom(); ++y)
                std::copy_n(bytes.data() + std::size_t(y - region.y) * stride + std::size_t(overlap.x - region.x),
                    overlap.width, pixels->data() + std::size_t(y - tile.y) * tileSize + std::size_t(overlap.x - tile.x));
            result->tiles_[index] = compress(std::move(pixels), {std::uint32_t(tile.width), std::uint32_t(tile.height)});
        }
    result->updateBounds();
    return result;
}
std::uint8_t SelectionMask::coverageAtDocumentPixel(std::int32_t x, std::int32_t y) const noexcept
{
    if (x < 0 || y < 0 || std::uint32_t(x) >= extent_.width || std::uint32_t(y) >= extent_.height) return 0;
    return tiles_[std::size_t(std::uint32_t(y) / tileSize) * columns_ + std::uint32_t(x) / tileSize]
        .at(std::uint32_t(x) % tileSize, std::uint32_t(y) % tileSize);
}
SelectionState SelectionMask::combined(const SelectionMask& other, SelectionOperation op) const
{
    if (extent_ != other.extent_) throw std::invalid_argument("Selection extent mismatch");
    auto result = std::shared_ptr<SelectionMask>(new SelectionMask(extent_));
    for (std::size_t i = 0; i < tiles_.size(); ++i) {
        const auto& a = tiles_[i]; const auto& b = other.tiles_[i];
        const auto r = tileRect(i);
        auto& out = result->tiles_[i];
        if (op == SelectionOperation::Replace) { out = b; continue; }
        if (!b.pixels && b.uniform == 0 && op != SelectionOperation::Intersect) { out = a; continue; }
        if (!b.pixels && b.uniform == 255 && op == SelectionOperation::Intersect) { out = a; continue; }
        if (!a.pixels && !b.pixels) {
            out.uniform = combine(a.uniform, b.uniform, op);
            if (out.uniform) out.bounds = {0, 0, r.width, r.height};
        } else {
            auto pixels = std::make_shared<Pixels>();
            for (int y = 0; y < r.height; ++y)
                for (int x = 0; x < r.width; ++x)
                    (*pixels)[std::size_t(y) * tileSize + std::size_t(x)] = combine(a.at(std::uint32_t(x), std::uint32_t(y)),
                        b.at(std::uint32_t(x), std::uint32_t(y)), op);
            out = compress(std::move(pixels), {std::uint32_t(r.width), std::uint32_t(r.height)});
        }
    }
    result->updateBounds();
    return result;
}
SelectionState SelectionMask::inverted() const
{ return filled(extent_, 255)->combined(*this, SelectionOperation::Subtract); }
SelectionState SelectionMask::resized(Extent2u extent) const
{
    auto result = std::shared_ptr<SelectionMask>(new SelectionMask(extent));
    for (std::size_t i = 0; i < result->tiles_.size(); ++i) {
        const auto r = result->tileRect(i), overlap = r.clippedTo(canvasBounds(extent_));
        if (overlap.empty()) continue;
        const auto oldIndex = std::size_t(r.y / int(tileSize)) * columns_ + std::size_t(r.x / int(tileSize));
        if (r == tileRect(oldIndex)) { result->tiles_[i] = tiles_[oldIndex]; continue; }
        auto pixels = std::make_shared<Pixels>();
        for (int y = 0; y < overlap.height; ++y)
            for (int x = 0; x < overlap.width; ++x)
                (*pixels)[std::size_t(y) * tileSize + std::size_t(x)] = coverageAtDocumentPixel(r.x + x, r.y + y);
        result->tiles_[i] = compress(std::move(pixels), {std::uint32_t(r.width), std::uint32_t(r.height)});
    }
    result->updateBounds();
    return result;
}
bool SelectionMask::equivalent(const SelectionMask& other) const noexcept
{
    if (extent_ != other.extent_ || bounds_ != other.bounds_) return false;
    for (std::size_t i = 0; i < tiles_.size(); ++i) {
        const auto& a = tiles_[i]; const auto& b = other.tiles_[i];
        if (a.pixels == b.pixels && (a.pixels || a.uniform == b.uniform)) continue;
        if (!a.pixels && !b.pixels) return false;
        const auto r = tileRect(i);
        for (int y = 0; y < r.height; ++y)
            for (int x = 0; x < r.width; ++x)
                if (a.at(std::uint32_t(x), std::uint32_t(y)) != b.at(std::uint32_t(x), std::uint32_t(y))) return false;
    }
    return true;
}
std::size_t SelectionMask::memoryCost() const noexcept
{
    std::size_t bytes = sizeof(*this) + tiles_.size() * sizeof(Tile);
    for (const auto& tile : tiles_) if (tile.pixels) bytes += sizeof(Pixels);
    return bytes + edgeMemoryBytes_.load(std::memory_order_relaxed)
        + nonzeroEdgeMemoryBytes_.load(std::memory_order_relaxed);
}
const std::vector<SelectionEdge>& SelectionMask::boundaryEdges() const
{
    std::call_once(edgesOnce_, [this] {
        edges_ = buildEdges(128);
        edgeMemoryBytes_.store(edges_.capacity() * sizeof(SelectionEdge), std::memory_order_relaxed);
    });
    return edges_;
}
const std::vector<SelectionEdge>& SelectionMask::nonzeroBoundaryEdges() const
{
    std::call_once(nonzeroEdgesOnce_, [this] {
        nonzeroEdges_ = buildEdges(1);
        nonzeroEdgeMemoryBytes_.store(nonzeroEdges_.capacity() * sizeof(SelectionEdge), std::memory_order_relaxed);
    });
    return nonzeroEdges_;
}
std::vector<SelectionEdge> SelectionMask::buildEdges(std::uint8_t threshold) const
{
    // Build privately before publishing. A failed allocation can be retried by
    // call_once without retaining half-built geometry or miscounting memory.
    std::vector<SelectionEdge> edges;
    const auto selected = [this, threshold](int x, int y) {
        return coverageAtDocumentPixel(x, y) >= threshold;
    };
    for (std::size_t i = 0; i < tiles_.size(); ++i) {
        const auto& tile = tiles_[i];
        if (tile.bounds.empty() || (!tile.pixels && tile.uniform < threshold)) continue;
        const auto r = tileRect(i);
        const auto pixelEdges = [&](int x, int y) {
                if (!selected(x, y)) return;
                if (!selected(x, y-1)) edges.push_back({{double(x), double(y)}, {double(x+1), double(y)}});
                if (!selected(x, y+1)) edges.push_back({{double(x), double(y+1)}, {double(x+1), double(y+1)}});
                if (!selected(x-1, y)) edges.push_back({{double(x), double(y)}, {double(x), double(y+1)}});
                if (!selected(x+1, y)) edges.push_back({{double(x+1), double(y)}, {double(x+1), double(y+1)}});
        };
        if (!tile.pixels) {
            for (int x = r.x; x < r.right(); ++x) {
                pixelEdges(x, r.y);
                if (r.height > 1) pixelEdges(x, r.bottom()-1);
            }
            for (int y = r.y+1; y < r.bottom()-1; ++y) {
                pixelEdges(r.x, y);
                if (r.width > 1) pixelEdges(r.right()-1, y);
            }
        } else for (int y = r.y; y < r.bottom(); ++y)
            for (int x = r.x; x < r.right(); ++x) pixelEdges(x, y);
    }
    normalizeEdges(edges);
    // Keep merged segments, not the much larger temporary unit-edge capacity.
    edges.shrink_to_fit();
    return edges;
}
SelectionState combineSelection(SelectionState base, SelectionState operand, SelectionOperation op)
{
    if (!operand) return {};
    if (op == SelectionOperation::Replace || !base) {
        if (!base && op == SelectionOperation::Subtract) return operand->inverted();
        return operand;
    }
    return base->combined(*operand, op);
}
RectI alignedSelectionRectangle(Vec2d a, Vec2d b, Extent2u extent) noexcept
{
    if (!std::isfinite(a.x) || !std::isfinite(a.y) || !std::isfinite(b.x) || !std::isfinite(b.y)) return {};
    // Snap both endpoints to pixel edges, not inclusive pixel indices. A click
    // without motion is active-empty; the same logical drag is zoom invariant.
    const auto x0 = int(std::round(std::clamp(std::min(a.x, b.x), 0.0, double(extent.width))));
    const auto y0 = int(std::round(std::clamp(std::min(a.y, b.y), 0.0, double(extent.height))));
    const auto x1 = int(std::round(std::clamp(std::max(a.x, b.x), 0.0, double(extent.width))));
    const auto y1 = int(std::round(std::clamp(std::max(a.y, b.y), 0.0, double(extent.height))));
    return {x0, y0, x1-x0, y1-y0};
}
std::vector<SelectionEdge> rectangleSelectionPreviewEdges(
    const SelectionState& base, RectI rect, SelectionOperation op, Extent2u extent)
{
    rect = rect.clippedTo(canvasBounds(extent));
    const auto baseline = base ? base : SelectionMask::filled(extent,
        op == SelectionOperation::Subtract || op == SelectionOperation::Intersect ? 255 : 0);
    const auto selected = [&](int x, int y) {
        if (!contains(canvasBounds(extent), x, y)) return false;
        return combine(baseline->coverageAtDocumentPixel(x, y), contains(rect, x, y) ? 255 : 0, op) >= 128;
    };
    std::vector<SelectionEdge> result;
    const auto add = [&](bool vertical, int fixed, int begin, int end) {
        if (end <= begin) return;
        const int midpoint = begin + (end-begin)/2;
        const bool different = vertical ? selected(fixed-1, midpoint) != selected(fixed, midpoint)
            : selected(midpoint, fixed-1) != selected(midpoint, fixed);
        if (different) result.push_back(vertical ? SelectionEdge {{double(fixed),double(begin)},{double(fixed),double(end)}}
            : SelectionEdge {{double(begin),double(fixed)},{double(end),double(fixed)}});
    };
    if (op != SelectionOperation::Replace) for (const auto& edge : baseline->boundaryEdges()) {
        const bool vertical = edge.from.x == edge.to.x;
        const int fixed = int(vertical ? edge.from.x : edge.from.y);
        const int begin = int(vertical ? edge.from.y : edge.from.x), end = int(vertical ? edge.to.y : edge.to.x);
        std::array<int,4> cuts {begin, std::clamp(vertical ? rect.y : rect.x, begin, end),
            std::clamp(vertical ? rect.bottom() : rect.right(), begin, end), end};
        std::sort(cuts.begin(), cuts.end());
        for (std::size_t j = 1; j < cuts.size(); ++j) {
            const int midpoint = cuts[j-1] + (cuts[j]-cuts[j-1])/2;
            // Merged baseline edges can alternate which side is selected.
            // Coincident rectangle edges need pixel-wise evaluation below;
            // one midpoint is not representative after a Boolean operation.
            const bool coincident = !rect.empty() && (vertical
                ? (fixed == rect.x || fixed == rect.right()) && midpoint >= rect.y && midpoint < rect.bottom()
                : (fixed == rect.y || fixed == rect.bottom()) && midpoint >= rect.x && midpoint < rect.right());
            if (!coincident) add(vertical, fixed, cuts[j-1], cuts[j]);
        }
    }
    if (!rect.empty()) {
        for (int x = rect.x; x < rect.right(); ++x) { add(false, rect.y, x, x+1); add(false, rect.bottom(), x, x+1); }
        for (int y = rect.y; y < rect.bottom(); ++y) { add(true, rect.x, y, y+1); add(true, rect.right(), y, y+1); }
    }
    normalizeEdges(result);
    return result;
}
std::vector<SelectionEdge> translatedSelectionPreviewEdges(const SelectionState& mask, double dx, double dy)
{
    std::vector<SelectionEdge> result;
    if (!mask || mask->bounds().empty() || !std::isfinite(dx) || !std::isfinite(dy)
        || std::abs(dx) >= mask->extent().width || std::abs(dy) >= mask->extent().height) return result;
    const int width = int(mask->extent().width), height = int(mask->extent().height);
    for (const auto& edge : mask->boundaryEdges()) {
        auto a = edge.from + Vec2d {double(dx),double(dy)}, b = edge.to + Vec2d {double(dx),double(dy)};
        if (a.x == b.x) {
            if (a.x <= 0 || a.x >= width) continue;
            a.y=std::clamp(a.y,0.0,double(height)); b.y=std::clamp(b.y,0.0,double(height));
        } else {
            if (a.y <= 0 || a.y >= height) continue;
            a.x=std::clamp(a.x,0.0,double(width)); b.x=std::clamp(b.x,0.0,double(width));
        }
        if (a != b) result.push_back({a,b});
    }
    // Clipping introduces new contours at the canvas perimeter. Query original
    // threshold coverage and translate its cell intervals, not rounded offsets:
    // fractional centering must remain continuous with the interior contours.
    const int top = int(std::floor(-dy)), bottom = int(std::ceil(height-dy))-1;
    const int left = int(std::floor(-dx)), right = int(std::ceil(width-dx))-1;
    for (int x=0; x<width; ++x) {
        const double a=std::max(0.0,x+dx), b=std::min(double(width),x+1+dx);
        if (a >= b) continue;
        if (mask->coverageAtDocumentPixel(x,top) >= 128) result.push_back({{a,0},{b,0}});
        if (mask->coverageAtDocumentPixel(x,bottom) >= 128) result.push_back({{a,double(height)},{b,double(height)}});
    }
    for (int y=0; y<height; ++y) {
        const double a=std::max(0.0,y+dy), b=std::min(double(height),y+1+dy);
        if (a >= b) continue;
        if (mask->coverageAtDocumentPixel(left,y) >= 128) result.push_back({{0,a},{0,b}});
        if (mask->coverageAtDocumentPixel(right,y) >= 128) result.push_back({{double(width),a},{double(width),b}});
    }
    normalizeEdges(result);
    return result;
}
} // namespace imageeditor::core
