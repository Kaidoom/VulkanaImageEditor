#include "imageeditor/core/CoverageResampler.hpp"
#include <stdexcept>
namespace imageeditor::core
{
float CoverageResampler::texel(int x, int y, int level) const
{
    if (level == 0)
        return mask_.coverageAtDocumentPixel(bounds_.x + x, bounds_.y + y) / 255.f;
    const auto &l = levels_[std::size_t(level - 1)];
    return x < 0 || y < 0 || x >= l.width || y >= l.height
               ? 0
               : l.values[std::size_t(y) * std::size_t(l.width) + std::size_t(x)];
}
void CoverageResampler::prepare()
{
    if (!levels_.empty() || bounds_.empty())
        return;
    int width = bounds_.width, height = bounds_.height;
    for (int level = 0; width > 1 || height > 1; ++level) {
        const int w = (width + 1) / 2, h = (height + 1) / 2;
        const auto count = std::size_t(w) * std::size_t(h);
        if (memoryBytes() + count * sizeof(float) > 96ULL * 1024 * 1024)
            throw std::length_error("Selection filtering exceeds the 96 MiB coverage budget");
        Level next{w, h, std::vector<float>(count)};
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                next.values[std::size_t(y) * std::size_t(w) + std::size_t(x)] =
                    (texel(x * 2, y * 2, level) + texel(x * 2 + 1, y * 2, level) +
                     texel(x * 2, y * 2 + 1, level) + texel(x * 2 + 1, y * 2 + 1, level)) *
                    .25f;
        levels_.push_back(std::move(next));
        width = w;
        height = h;
    }
}
float CoverageResampler::sample(Vec2d p, double footprint)
{
    if (bounds_.empty() || !std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(footprint))
        return 0;
    if (footprint > 1.000001)
        prepare();
    const auto lod = std::clamp(std::log2(std::max(1.0, footprint)), 0.0, double(levels_.size()));
    const auto at = [&](int level) {
        const double scale = std::ldexp(1.0, level);
        const double x = (p.x - bounds_.x) / scale - .5, y = (p.y - bounds_.y) / scale - .5;
        // Odd source dimensions leave a partially populated final mip texel.
        // Its reconstruction tail is real coverage, not outside-image garbage.
        const int width = level ? levels_[std::size_t(level - 1)].width : bounds_.width;
        const int height = level ? levels_[std::size_t(level - 1)].height : bounds_.height;
        if (x < -1 || y < -1 || x >= width || y >= height)
            return 0.f;
        const int ix = int(std::floor(x)), iy = int(std::floor(y));
        const float fx = float(x - ix), fy = float(y - iy);
        return (1 - fy) * ((1 - fx) * texel(ix, iy, level) + fx * texel(ix + 1, iy, level)) +
               fy * ((1 - fx) * texel(ix, iy + 1, level) + fx * texel(ix + 1, iy + 1, level));
    };
    const int level = int(std::floor(lod));
    const float a = at(level), fraction = float(lod - level);
    return fraction == 0 ? a : a + fraction * (at(level + 1) - a);
}
std::size_t CoverageResampler::memoryBytes() const noexcept
{
    std::size_t n = 0;
    for (const auto &l : levels_)
        n += l.values.capacity() * sizeof(float);
    return n;
}
} // namespace imageeditor::core
