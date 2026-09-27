#include "imageeditor/core/RegionFinder.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace imageeditor::core {
RegionFinder::RegionFinder(Extent2u extent, int x, int y, std::uint8_t tolerance, Source source)
    : extent_(extent)
    , source_(std::move(source))
    , tolerance_(tolerance)
{
    const auto pixels = std::uint64_t(extent.width) * extent.height;
    if (pixels > 64'000'000 || extent.width > 65536 || extent.height > 65536)
        throw std::length_error("Region discovery exceeds the 64 megapixel work limit");
    if (x < 0 || y < 0 || x >= int(extent.width) || y >= int(extent.height))
        return;
    const auto seed = source_(x, y);
    if (!seed)
        return;
    seed_ = *seed;
    visited_.resize((pixels + 63) / 64);
    region_.resize(visited_.size());
    enqueue({ y, x, x + 1 });
}
bool RegionFinder::matches(Rgba8 a, Rgba8 b, std::uint8_t tolerance) noexcept
{
    // Maximum channel distance in premultiplied sRGB bytes plus linear alpha.
    // Hidden RGB cannot influence transparent matching. Fixed seed, not a
    // rolling neighbor comparison: a shallow gradient cannot leak indefinitely.
    const auto premul = [](std::uint8_t c, std::uint8_t alpha) { return (int(c) * alpha + 127) / 255; };
    return std::max({ std::abs(int(a.alpha) - b.alpha),
               std::abs(premul(a.red, a.alpha) - premul(b.red, b.alpha)),
               std::abs(premul(a.green, a.alpha) - premul(b.green, b.alpha)),
               std::abs(premul(a.blue, a.alpha) - premul(b.blue, b.alpha)) })
        <= tolerance;
}
bool RegionFinder::test(int x, int y)
{
    ++work_;
    const auto index = std::size_t(y) * extent_.width + std::size_t(x);
    const auto bit = std::uint64_t(1) << (index % 64);
    if (visited_[index / 64] & bit)
        return false;
    visited_[index / 64] |= bit;
    const auto pixel = source_(x, y);
    if (!pixel || !matches(seed_, *pixel, tolerance_))
        return false;
    region_[index / 64] |= bit;
    ++count_;
    bounds_ = bounds_.united({ x, y, 1, 1 });
    return true;
}
void RegionFinder::enqueue(Span span)
{
    if (span.y < 0 || span.y >= int(extent_.height))
        return;
    if (pending_.size() >= 1'000'000)
        throw std::length_error("Region frontier exceeds the bounded work limit");
    pending_.push_back(span);
}
bool RegionFinder::step(std::size_t budget)
{
    work_ = 0;
    while (work_ < std::max(std::size_t(1), budget)) {
        if (!current_) {
            if (pending_.empty())
                return true;
            current_ = pending_.back();
            pending_.pop_back();
        }
        auto& span = *current_;
        if (span.left >= span.right) {
            current_.reset();
            continue;
        }
        const auto x = span.left++;
        if (!test(x, span.y))
            continue;
        int left = x, right = x + 1;
        while (left > 0 && test(left - 1, span.y))
            --left;
        while (right < int(extent_.width) && test(right, span.y))
            ++right;
        enqueue({ span.y - 1, left, right });
        enqueue({ span.y + 1, left, right });
        span.left = right;
    }
    return false;
}
bool RegionFinder::contains(int x, int y) const noexcept
{
    if (region_.empty() || x < 0 || y < 0 || x >= int(extent_.width) || y >= int(extent_.height))
        return false;
    const auto index = std::size_t(y) * extent_.width + std::size_t(x);
    return (region_[index / 64] >> (index % 64)) & 1;
}
std::size_t RegionFinder::memoryBytes() const noexcept
{
    return (visited_.capacity() + region_.capacity()) * sizeof(std::uint64_t)
        + pending_.capacity() * sizeof(Span);
}
}
