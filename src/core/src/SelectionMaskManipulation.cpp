#include "imageeditor/core/SelectionMask.hpp"
#include "imageeditor/core/CoverageResampler.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace imageeditor::core {
namespace {
    RectI canvasRect(Extent2u extent) { return { 0, 0, int(extent.width), int(extent.height) }; }
    RectI clippedBounds(double left, double top, double right, double bottom, Extent2u extent)
    {
        const auto x0 = int(std::floor(std::clamp(left, 0.0, double(extent.width))));
        const auto y0 = int(std::floor(std::clamp(top, 0.0, double(extent.height))));
        const auto x1 = int(std::ceil(std::clamp(right, 0.0, double(extent.width))));
        const auto y1 = int(std::ceil(std::clamp(bottom, 0.0, double(extent.height))));
        return { x0, y0, x1 - x0, y1 - y0 };
    }
}

std::optional<std::uint8_t> SelectionMask::constantCoverage(RectI region) const
{
    const auto clipped = region.clippedTo(canvasRect(extent_));
    if (clipped.empty())
        return 0;
    std::optional<std::uint8_t> value;
    if (clipped != region)
        value = 0; // zero extension beyond the document
    for (int ty = clipped.y / int(tileSize); ty <= (clipped.bottom() - 1) / int(tileSize); ++ty)
        for (int tx = clipped.x / int(tileSize); tx <= (clipped.right() - 1) / int(tileSize); ++tx) {
            const auto& tile = tiles_[std::size_t(ty) * columns_ + std::size_t(tx)];
            if (tile.pixels || (value && *value != tile.uniform))
                return { };
            value = tile.uniform;
        }
    return value;
}

SelectionState SelectionMask::translated(int dx, int dy) const
{
    auto result = std::shared_ptr<SelectionMask>(new SelectionMask(extent_));
    if (!dx && !dy) {
        result->tiles_ = tiles_;
        result->bounds_ = bounds_;
        return result;
    }
    if (bounds_.empty() || std::abs(double(dx)) >= extent_.width || std::abs(double(dy)) >= extent_.height)
        return result;
    const auto occupied = clippedBounds(double(bounds_.x) + dx, double(bounds_.y) + dy,
        double(bounds_.right()) + dx, double(bounds_.bottom()) + dy, extent_);
    for (std::size_t i = 0; i < result->tiles_.size(); ++i) {
        const auto r = result->tileRect(i);
        if (r.clippedTo(occupied).empty())
            continue;
        const RectI source { r.x - dx, r.y - dy, r.width, r.height };
        if (const auto value = constantCoverage(source)) {
            result->tiles_[i].uniform = *value;
            if (*value)
                result->tiles_[i].bounds = { 0, 0, r.width, r.height };
            continue;
        }
        auto pixels = std::make_shared<Pixels>();
        for (int y = 0; y < r.height; ++y)
            for (int x = 0; x < r.width; ++x)
                (*pixels)[std::size_t(y) * tileSize + std::size_t(x)]
                    = coverageAtDocumentPixel(source.x + x, source.y + y);
        result->tiles_[i] = compress(std::move(pixels), { std::uint32_t(r.width), std::uint32_t(r.height) });
    }
    result->updateBounds();
    return result;
}

SelectionState SelectionMask::adjusted(int horizontal, int vertical) const
{
    if (!horizontal && !vertical)
        return translated(0, 0);
    if (bounds_.empty() || double(horizontal) <= -double(extent_.width)
        || double(vertical) <= -double(extent_.height))
        return filled(extent_, 0);
    horizontal = std::min(horizontal, int(extent_.width));
    vertical = std::min(vertical, int(extent_.height));
    const int growX = std::max(0, horizontal), growY = std::max(0, vertical);
    const auto work = clippedBounds(
        bounds_.x - growX, bounds_.y - growY, bounds_.right() + growX, bounds_.bottom() + growY, extent_);
    const int width = work.width, height = work.height;
    std::vector<std::uint8_t> values(std::size_t(width) * std::size_t(height));
    std::vector<std::uint8_t> scratch(values.size());
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            values[std::size_t(y) * std::size_t(width) + std::size_t(x)]
                = coverageAtDocumentPixel(work.x + x, work.y + y);
    std::vector<int> queue(std::size_t(std::max(width, height)));
    const auto axis = [&](int amount, bool xAxis) {
        if (!amount)
            return;
        const bool maximum = amount > 0;
        const int radius = std::abs(amount), length = xAxis ? width : height, lines = xAxis ? height : width;
        const auto index = [&](int line, int pos) {
            return xAxis ? std::size_t(line) * std::size_t(width) + std::size_t(pos)
                         : std::size_t(pos) * std::size_t(width) + std::size_t(line);
        };
        for (int line = 0; line < lines; ++line) {
            int head = 0, tail = 0, next = 0;
            for (int pos = 0; pos < length; ++pos) {
                const int last = std::min(length - 1, pos + radius);
                while (next <= last) {
                    const auto value = values[index(line, next)];
                    while (head < tail
                        && (maximum ? values[index(line, queue[std::size_t(tail - 1)])] <= value
                                    : values[index(line, queue[std::size_t(tail - 1)])] >= value))
                        --tail;
                    queue[std::size_t(tail++)] = next++;
                }
                while (head < tail && queue[std::size_t(head)] < pos - radius)
                    ++head;
                scratch[index(line, pos)] = !maximum && (pos - radius < 0 || pos + radius >= length)
                    ? 0
                    : values[index(line, queue[std::size_t(head)])];
            }
        }
        values.swap(scratch);
    };
    // O(covered bounds), independent of radius; a rectangular separable kernel.
    // For mixed signs, horizontal always precedes vertical.
    axis(horizontal, true);
    axis(vertical, false);
    auto result = std::shared_ptr<SelectionMask>(new SelectionMask(extent_));
    for (std::size_t i = 0; i < result->tiles_.size(); ++i) {
        const auto r = result->tileRect(i), overlap = r.clippedTo(work);
        if (overlap.empty())
            continue;
        auto pixels = std::make_shared<Pixels>();
        for (int y = overlap.y; y < overlap.bottom(); ++y)
            for (int x = overlap.x; x < overlap.right(); ++x)
                (*pixels)[std::size_t(y - r.y) * tileSize + std::size_t(x - r.x)]
                    = values[std::size_t(y - work.y) * std::size_t(width) + std::size_t(x - work.x)];
        result->tiles_[i] = compress(std::move(pixels), { std::uint32_t(r.width), std::uint32_t(r.height) });
    }
    result->updateBounds();
    return result;
}

SelectionState SelectionMask::rotated(double degrees, Vec2d pivot) const
{
    if (!std::isfinite(degrees) || !std::isfinite(pivot.x) || !std::isfinite(pivot.y)
        || std::abs(pivot.x) > 1e9 || std::abs(pivot.y) > 1e9)
        throw std::invalid_argument("Selection rotation requires finite angle and pivot");
    degrees = std::remainder(degrees, 360.0);
    if (degrees == 0 || bounds_.empty())
        return translated(0, 0);
    const double radians = degrees * std::numbers::pi / 180.0;
    double c = std::cos(radians), s = std::sin(radians);
    if (std::remainder(degrees, 90.0) == 0) {
        c = std::round(c);
        s = std::round(s);
    }
    const auto map = [&](Vec2d p, bool inverse) {
        p = p - pivot;
        const double sine = inverse ? -s : s;
        return Vec2d { pivot.x + c * p.x - sine * p.y, pivot.y + sine * p.x + c * p.y };
    };
    return resampled(map);
}

SelectionState SelectionMask::transformed(const AffineTransform& mapping) const
{
    const auto inverse = mapping.inverted();
    if (!inverse) throw std::invalid_argument("Selection transform must be finite and invertible");
    for (const auto value : {mapping.m00, mapping.m01, mapping.m02, mapping.m10, mapping.m11, mapping.m12})
        if (std::abs(value) > 1e12) throw std::invalid_argument("Selection transform exceeds supported range");
    for (const auto value : {inverse->m00, inverse->m01, inverse->m02, inverse->m10, inverse->m11, inverse->m12})
        if (std::abs(value) > 1e24) throw std::invalid_argument("Selection inverse exceeds supported range");
    if (!mapping.validOver({double(bounds_.x)-.5,double(bounds_.y)-.5,double(bounds_.width)+1,double(bounds_.height)+1}))
        throw std::invalid_argument("Selection transform crosses a projective horizon or exceeds supported bounds");
    if (mapping.isAffine() && mapping.m00 == 1 && mapping.m11 == 1 && mapping.m01 == 0 && mapping.m10 == 0
        && mapping.m02 == std::round(mapping.m02) && mapping.m12 == std::round(mapping.m12)) {
        if (std::abs(mapping.m02) >= extent_.width || std::abs(mapping.m12) >= extent_.height)
            return filled(extent_, 0);
        return translated(int(mapping.m02), int(mapping.m12));
    }
    return resampled([&](Vec2d p, bool backwards) { return backwards ? inverse->map(p) : mapping.map(p); },mapping.isAffine());
}

template<class Mapping> SelectionState SelectionMask::resampled(Mapping map,bool affine) const
{
    const auto mappedBounds = [&](RectI r, bool inverse, double padding) {
        const std::array points { map({ r.x - padding, r.y - padding }, inverse),
            map({ r.right() + padding, r.y - padding }, inverse),
            map({ r.x - padding, r.bottom() + padding }, inverse),
            map({ r.right() + padding, r.bottom() + padding }, inverse) };
        double left = points[0].x, right = left, top = points[0].y, bottom = top;
        for (const auto p : points) {
            left = std::min(left, p.x);
            right = std::max(right, p.x);
            top = std::min(top, p.y);
            bottom = std::max(bottom, p.y);
        }
        return std::array { left, top, right, bottom };
    };
    const auto b = mappedBounds(bounds_, false, 0.5);
    const auto occupied = clippedBounds(b[0]-1, b[1]-1, b[2]+1, b[3]+1, extent_);
    auto result = std::shared_ptr<SelectionMask>(new SelectionMask(extent_));
    const auto sample = [&](Vec2d p) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || p.x <= -0.5 || p.y <= -0.5 || p.x >= extent_.width + 0.5
            || p.y >= extent_.height + 0.5)
            return std::uint8_t(0);
        const double x = p.x - 0.5, y = p.y - 0.5;
        const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
        const double fx = x - x0, fy = y - y0;
        const auto a = coverageAtDocumentPixel(x0, y0) * (1 - fx) + coverageAtDocumentPixel(x0 + 1, y0) * fx;
        const auto d
            = coverageAtDocumentPixel(x0, y0 + 1) * (1 - fx) + coverageAtDocumentPixel(x0 + 1, y0 + 1) * fx;
        return std::uint8_t(std::clamp(std::lround(a * (1 - fy) + d * fy), 0L, 255L));
    };
    CoverageResampler coverage(*this);
    for (std::size_t i = 0; i < result->tiles_.size(); ++i) {
        const auto r = result->tileRect(i);
        if (r.clippedTo(occupied).empty())
            continue;
        const auto src = affine?mappedBounds(r, true, 0.5):std::array{0.0,0.0,double(extent_.width),double(extent_.height)};
        // Only zero extension matters outside this range; clamp before integer
        // conversion so even a distant caller-supplied pivot cannot overflow.
        const int x0 = int(std::floor(std::clamp(src[0], -1.0, double(extent_.width) + 1)));
        const int y0 = int(std::floor(std::clamp(src[1], -1.0, double(extent_.height) + 1)));
        const RectI source { x0, y0,
            int(std::ceil(std::clamp(src[2], -1.0, double(extent_.width) + 1))) - x0 + 1,
            int(std::ceil(std::clamp(src[3], -1.0, double(extent_.height) + 1))) - y0 + 1 };
        if (const auto value = affine?constantCoverage(source):std::nullopt) {
            result->tiles_[i].uniform = *value;
            if (*value)
                result->tiles_[i].bounds = { 0, 0, r.width, r.height };
            continue;
        }
        auto pixels = std::make_shared<Pixels>();
        for (int y = 0; y < r.height; ++y)
            for (int x = 0; x < r.width; ++x)
            {
                const Vec2d p{r.x+x+.5,r.y+y+.5};const auto local=map(p,true);
                const auto dx=map(p+Vec2d{.001,0},true)-local,dy=map(p+Vec2d{0,.001},true)-local;
                const ProjectiveTransform derivative{dx.x*1000,dy.x*1000,0,dx.y*1000,dy.y*1000,0};
                const auto footprint=derivative.maximumScale();
                (*pixels)[std::size_t(y)*tileSize+std::size_t(x)]=footprint<=1.000001?sample(local)
                    :std::uint8_t(std::clamp(std::lround(255*coverage.sample(local,footprint)),0L,255L));
            }
        result->tiles_[i] = compress(std::move(pixels), { std::uint32_t(r.width), std::uint32_t(r.height) });
    }
    result->updateBounds();
    return result;
}

bool selectionMoveHit(
    const SelectionState& mask, Vec2d point, SelectionOperation op, bool shift, bool alt) noexcept
{
    if (!mask || op != SelectionOperation::Replace || shift || alt || !std::isfinite(point.x)
        || !std::isfinite(point.y) || point.x < 0 || point.y < 0 || point.x >= mask->extent().width
        || point.y >= mask->extent().height)
        return false;
    return mask->coverageAtDocumentPixel(int(std::floor(point.x)), int(std::floor(point.y))) != 0;
}
}
