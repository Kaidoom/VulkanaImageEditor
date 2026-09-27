#include "imageeditor/core/PolygonCoverageRasterizer.hpp"
#include "imageeditor/core/FreehandSelectionPath.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <tuple>

namespace imageeditor::core {
PolygonCoverageRasterizer::PolygonCoverageRasterizer(Extent2u canvas, std::span<const Vec2d> polygon)
{
    if (canvas.empty() || canvas.width > 32768 || canvas.height > 32768)
        throw std::invalid_argument("Invalid polygon canvas extent");
    if (polygon.size() > FreehandSelectionPath::maximumPoints)
        throw std::length_error("Lasso has too many vertices");
    double left = double(canvas.width), top = double(canvas.height), right = 0, bottom = 0;
    for (const auto point : polygon) {
        if (!std::isfinite(point.x) || !std::isfinite(point.y)
            || std::abs(point.x) > FreehandSelectionPath::maximumCoordinate
            || std::abs(point.y) > FreehandSelectionPath::maximumCoordinate)
            throw std::invalid_argument("Invalid lasso coordinate");
        left = std::min(left, point.x);
        right = std::max(right, point.x);
        top = std::min(top, point.y);
        bottom = std::max(bottom, point.y);
    }
    if (polygon.size() < 3 || left >= right || top >= bottom)
        return;
    const int x0 = int(std::clamp(std::floor(left), 0.0, double(canvas.width)));
    const int y0 = int(std::clamp(std::floor(top), 0.0, double(canvas.height)));
    const int x1 = int(std::clamp(std::ceil(right), 0.0, double(canvas.width)));
    const int y1 = int(std::clamp(std::ceil(bottom), 0.0, double(canvas.height)));
    if (x1 <= x0 || y1 <= y0)
        return;
    region_ = { x0, y0, x1 - x0, y1 - y0 };
    const auto area = std::uint64_t(region_.width) * std::uint64_t(region_.height);
    if (area > maximumPixels)
        throw std::length_error("Lasso affected area exceeds the 64-million-pixel safety limit");
    nextSample_ = y0 * subrowsPerPixel;
    endSample_ = y1 * subrowsPerPixel;
    std::uint64_t work = 0;
    edges_.reserve(polygon.size());
    for (std::size_t i = 0; i < polygon.size(); ++i) {
        auto low = polygon[i], high = polygon[(i + 1) % polygon.size()];
        if (low.y == high.y)
            continue;
        if (low.y > high.y)
            std::swap(low, high);
        // Evaluate canonical low->high edges. Half-open Y includes top and
        // excludes bottom; coincident X crossings retain their multiplicity.
        const auto sample = [&](double y) {
            return int(
                std::clamp(std::ceil(y * subrowsPerPixel - 0.5), double(nextSample_), double(endSample_)));
        };
        const int start = sample(low.y), end = sample(high.y);
        if (start >= end)
            continue;
        work += std::uint64_t(end - start);
        if (work > maximumEdgeSamples)
            throw std::length_error("Lasso is too complex to rasterize safely; selection unchanged");
        edges_.push_back({ low, high, start, end });
    }
    std::sort(edges_.begin(), edges_.end(), [](const auto& a, const auto& b) {
        return std::tie(a.start, a.low.y, a.low.x, a.high.y, a.high.x)
            < std::tie(b.start, b.low.y, b.low.x, b.high.y, b.high.x);
    });
    if (edges_.empty()) {
        region_ = { };
        nextSample_ = endSample_;
        return;
    }
    active_.reserve(edges_.size());
    crossings_.reserve(edges_.size());
    differences_.resize(std::size_t(region_.width) + 1);
    partials_.resize(std::size_t(region_.width));
    pixels_.resize(std::size_t(area));
}
void PolygonCoverageRasterizer::addInterval(double left, double right)
{
    // Pair before clipping: crossings outside the canvas still establish parity.
    left = std::clamp(left - region_.x, 0.0, double(region_.width));
    right = std::clamp(right - region_.x, 0.0, double(region_.width));
    if (left >= right)
        return;
    const auto first = std::size_t(std::floor(left));
    const auto last = std::size_t(std::ceil(right)) - 1;
    if (first == last) {
        partials_[first] += right - left;
        return;
    }
    partials_[first] += double(first + 1) - left;
    partials_[last] += right - double(last);
    // Full-pixel interiors use range differences, not width*256 work.
    if (last > first + 1) {
        differences_[first + 1] += 1;
        differences_[last] -= 1;
    }
}
void PolygonCoverageRasterizer::finishRow(int y)
{
    double full = 0;
    auto* output = pixels_.data() + std::size_t(y - region_.y) * stride();
    for (std::size_t x = 0; x < stride(); ++x) {
        full += differences_[x];
        output[x] = std::uint8_t(
            std::clamp(std::lround((full + partials_[x]) * 255.0 / subrowsPerPixel), 0L, 255L));
    }
    std::fill(differences_.begin(), differences_.end(), 0);
    std::fill(partials_.begin(), partials_.end(), 0);
    stats_.writtenPixels += stride();
}
bool PolygonCoverageRasterizer::step(std::size_t workBudget)
{
    std::size_t work = 0;
    while (!finished()) {
        std::erase_if(active_, [&](std::size_t i) { return edges_[i].end <= nextSample_; });
        while (nextEdge_ < edges_.size() && edges_[nextEdge_].start <= nextSample_)
            active_.push_back(nextEdge_++);
        crossings_.clear();
        const double y = (double(nextSample_) + 0.5) / subrowsPerPixel;
        for (const auto i : active_) {
            const auto& edge = edges_[i];
            crossings_.push_back(
                edge.low.x + (edge.high.x - edge.low.x) * ((y - edge.low.y) / (edge.high.y - edge.low.y)));
        }
        // Crossing order changes at self-intersections, even within one pixel.
        std::sort(crossings_.begin(), crossings_.end());
        if (crossings_.size() % 2)
            throw std::runtime_error("Invalid polygon crossing parity");
        for (std::size_t i = 0; i + 1 < crossings_.size(); i += 2)
            addInterval(crossings_[i], crossings_[i + 1]);
        stats_.edgeSamples += active_.size();
        ++stats_.subrows;
        work += std::max<std::size_t>(1, active_.size());
        ++nextSample_;
        if (nextSample_ % subrowsPerPixel == 0) {
            finishRow(nextSample_ / subrowsPerPixel - 1);
            work += stride();
        }
        if (work >= std::max<std::size_t>(1, workBudget))
            break;
    }
    return finished();
}
std::size_t PolygonCoverageRasterizer::memoryBytes() const noexcept
{
    return pixels_.capacity() + edges_.capacity() * sizeof(Edge) + active_.capacity() * sizeof(std::size_t)
        + (crossings_.capacity() + differences_.capacity() + partials_.capacity()) * sizeof(double);
}
}
