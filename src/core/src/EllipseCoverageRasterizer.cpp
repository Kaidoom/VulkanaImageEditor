#include "imageeditor/core/EllipseCoverageRasterizer.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <tuple>

namespace imageeditor::core {
namespace {
    using Real = long double;
    Real chord(Real x) noexcept { return std::sqrt(std::max(Real(0), (1 - x) * (1 + x))); }
    Real segment(Real angle) noexcept
    {
        // angle - sin(angle), without loss of precision for very short arcs.
        if (angle < .1L) {
            const auto a2 = angle * angle;
            return angle * a2
                * (1.L / 6 - a2 * (1.L / 120 - a2 * (1.L / 5040 - a2 * (1.L / 362880 - a2 / 39916800))));
        }
        return angle - std::sin(angle);
    }
    Real quarterArea(Real a, Real b, Real c, Real d) noexcept
    {
        // Clip a first-quadrant rectangle to the unit disk. Full strips plus a
        // positive trapezoid/circular-segment identity avoid subtracting nearly
        // equal asin primitives (important for large, off-canvas ellipses).
        a = std::clamp(a, 0.L, 1.L);
        b = std::clamp(b, 0.L, 1.L);
        c = std::clamp(c, 0.L, 1.L);
        d = std::clamp(d, 0.L, 1.L);
        if (a >= b || c >= d)
            return 0;
        if (b * b + d * d <= 1)
            return (b - a) * (d - c);
        if (a * a + c * c >= 1)
            return 0;
        const auto full = chord(d), zero = chord(c);
        Real area = std::max(0.L, std::min(b, full) - a) * (d - c);
        const auto u = std::max(a, full), v = std::min(b, zero);
        if (u < v) {
            const auto fu = chord(u), fv = chord(v);
            const auto height = [&](Real x, Real f) {
                return f + c > 0 ? std::max(0.L, (zero - x) * (zero + x) / (f + c)) : 0.L;
            };
            const auto angle = 2 * std::atan2(v - u, fu + fv);
            area += (v - u) * (height(u, fu) + height(v, fv)) / 2 + segment(angle) / 2;
        }
        return area;
    }
    struct Interval {
        Real low, high;
    };
    std::array<Interval, 2> folded(Real low, Real high) noexcept
    {
        return { { { std::max(0.L, -high), std::max(0.L, -low) },
            { std::max(0.L, low), std::max(0.L, high) } } };
    }
    void mergeEdges(std::vector<SelectionEdge>& edges)
    {
        const auto key = [](const SelectionEdge& e) {
            const bool v = e.from.x == e.to.x;
            return std::tuple(v, v ? e.from.x : e.from.y, v ? e.from.y : e.from.x);
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
                    if (v)
                        previous.to.y = std::max(previous.to.y, edge.to.y);
                    else
                        previous.to.x = std::max(previous.to.x, edge.to.x);
                    continue;
                }
            }
            edges[count++] = edge;
        }
        edges.resize(count);
    }
}

EllipseGeometry::EllipseGeometry(Vec2d start, Vec2d end, bool circle)
{
    for (auto point : { start, end })
        if (!std::isfinite(point.x) || !std::isfinite(point.y) || std::abs(point.x) > 1e9
            || std::abs(point.y) > 1e9)
            throw std::invalid_argument("Invalid ellipse coordinate");
    if (circle) {
        const auto delta = end - start;
        const auto side = std::max(std::abs(delta.x), std::abs(delta.y));
        end = { start.x + std::copysign(side, delta.x), start.y + std::copysign(side, delta.y) };
    }
    minimum_ = { std::min(start.x, end.x), std::min(start.y, end.y) };
    maximum_ = { std::max(start.x, end.x), std::max(start.y, end.y) };
    cx_ = (Real(minimum_.x) + Real(maximum_.x)) / 2;
    cy_ = (Real(minimum_.y) + Real(maximum_.y)) / 2;
    rx_ = (Real(maximum_.x) - Real(minimum_.x)) / 2;
    ry_ = (Real(maximum_.y) - Real(minimum_.y)) / 2;
}
RectI EllipseGeometry::region(Extent2u canvas) const
{
    if (canvas.empty() || canvas.width > 32768 || canvas.height > 32768)
        throw std::invalid_argument("Invalid ellipse canvas extent");
    if (empty())
        return { };
    const int left = int(std::clamp(std::floor(minimum_.x), 0., double(canvas.width)));
    const int top = int(std::clamp(std::floor(minimum_.y), 0., double(canvas.height)));
    const int right = int(std::clamp(std::ceil(maximum_.x), 0., double(canvas.width)));
    const int bottom = int(std::clamp(std::ceil(maximum_.y), 0., double(canvas.height)));
    if (left >= right || top >= bottom)
        return { };
    return { left, top, right - left, bottom - top };
}
std::uint8_t EllipseGeometry::coverageAt(int x, int y) const noexcept
{
    if (empty())
        return 0;
    const auto xs = folded((Real(x) - cx_) / rx_, (Real(x) + 1 - cx_) / rx_);
    const auto ys = folded((Real(y) - cy_) / ry_, (Real(y) + 1 - cy_) / ry_);
    Real area = 0;
    for (const auto xx : xs)
        for (const auto yy : ys)
            area += quarterArea(xx.low, xx.high, yy.low, yy.high);
    return std::uint8_t(std::clamp(std::lround(std::clamp(area * rx_ * ry_, 0.L, 1.L) * 255), 0L, 255L));
}
EllipseGeometry::RowBand EllipseGeometry::rowBand(int y, RectI clip) const noexcept
{
    const auto low = (Real(y) - cy_) / ry_, high = (Real(y) + 1 - cy_) / ry_;
    const auto near = low > 0 ? low : high < 0 ? -high : 0;
    const auto far = std::max(std::abs(low), std::abs(high));
    const auto outer = rx_ * chord(std::min(1.L, near));
    const auto inner = rx_ * chord(std::min(1.L, far));
    const auto floorX
        = [&](Real x) { return int(std::clamp(std::floor(x), Real(clip.x), Real(clip.right()))); };
    const auto ceilX
        = [&](Real x) { return int(std::clamp(std::ceil(x), Real(clip.x), Real(clip.right()))); };
    return { floorX(cx_ - outer), ceilX(cx_ + outer), ceilX(cx_ - inner), floorX(cx_ + inner) };
}
std::vector<SelectionEdge> EllipseGeometry::previewEdges(Extent2u canvas) const
{
    const auto clip = region(canvas);
    if (clip.empty())
        return { };
    std::vector<SelectionEdge> edges;
    edges.reserve(std::size_t(clip.height) * 4 + 4);
    int previousLeft = 0, previousRight = 0;
    const auto difference = [&](int a, int b, int c, int d, int y) {
        const auto append = [&](int left, int right) {
            if (left < right)
                edges.push_back({ { double(left), double(y) }, { double(right), double(y) } });
        };
        if (c >= d || d <= a || c >= b)
            append(a, b);
        else {
            append(a, std::min(b, c));
            append(std::max(a, d), b);
        }
    };
    for (int y = clip.y; y <= clip.bottom(); ++y) {
        int left = 0, right = 0;
        if (y < clip.bottom()) {
            const auto band = rowBand(y, clip);
            const int peak = int(std::clamp(std::floor(cx_), Real(clip.x), Real(clip.right() - 1)));
            if (band.outerLeft < band.outerRight
                && (band.innerLeft < band.innerRight || coverageAt(peak, y) >= 128)) {
                int lo = band.outerLeft, hi = std::min(peak, std::max(band.outerLeft, band.innerLeft));
                while (lo < hi) {
                    const int mid = lo + (hi - lo) / 2;
                    if (coverageAt(mid, y) >= 128)
                        hi = mid;
                    else
                        lo = mid + 1;
                }
                left = lo;
                lo = std::max(peak, std::min(band.outerRight - 1, band.innerRight - 1));
                hi = band.outerRight;
                while (lo < hi) {
                    const int mid = lo + (hi - lo) / 2;
                    if (coverageAt(mid, y) >= 128)
                        lo = mid + 1;
                    else
                        hi = mid;
                }
                right = lo;
                edges.push_back({ { double(left), double(y) }, { double(left), double(y + 1) } });
                edges.push_back({ { double(right), double(y) }, { double(right), double(y + 1) } });
            }
        }
        difference(left, right, previousLeft, previousRight, y);
        difference(previousLeft, previousRight, left, right, y);
        previousLeft = left;
        previousRight = right;
    }
    mergeEdges(edges);
    return edges;
}

EllipseCoverageRasterizer::EllipseCoverageRasterizer(Extent2u canvas, EllipseGeometry geometry)
    : geometry_(geometry)
    , region_(geometry.region(canvas))
    , nextRow_(region_.y)
{
    const auto area = std::uint64_t(region_.width) * std::uint64_t(region_.height);
    if (area > maximumPixels)
        throw std::length_error("Ellipse affected area exceeds the 64-million-pixel safety limit");
    pixels_.resize(std::size_t(area));
}
bool EllipseCoverageRasterizer::step(std::size_t workBudget)
{
    std::size_t work = 0;
    while (!finished()) {
        const auto band = geometry_.rowBand(nextRow_, region_);
        auto* row = pixels_.data() + std::size_t(nextRow_ - region_.y) * stride();
        const auto leftEnd = std::min(band.outerRight, std::max(band.outerLeft, band.innerLeft));
        const auto rightStart = std::max(leftEnd, band.innerRight);
        for (int x = band.outerLeft; x < leftEnd; ++x) {
            row[x - region_.x] = geometry_.coverageAt(x, nextRow_);
            ++stats_.boundaryPixels;
        }
        if (band.innerLeft < band.innerRight)
            std::fill(row + band.innerLeft - region_.x, row + band.innerRight - region_.x, std::uint8_t(255));
        for (int x = rightStart; x < band.outerRight; ++x) {
            row[x - region_.x] = geometry_.coverageAt(x, nextRow_);
            ++stats_.boundaryPixels;
        }
        stats_.writtenPixels += stride();
        work += stride();
        ++nextRow_;
        if (work >= std::max<std::size_t>(1, workBudget))
            break;
    }
    return finished();
}
} // namespace imageeditor::core
