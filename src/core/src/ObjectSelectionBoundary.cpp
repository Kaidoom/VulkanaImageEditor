#include "imageeditor/core/ObjectSelectionBoundary.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
namespace imageeditor::core {
std::vector<std::uint8_t> recoverObjectSelectionBoundary(
    const SmartReferenceImage& image, std::span<const float> logits, const std::atomic_bool& cancelled)
{
    const auto e = image.extent;
    const auto count = std::size_t(e.width) * e.height;
    if (logits.size() != count || image.pixels.size() != count || image.valid.size() != count)
        throw std::invalid_argument("Invalid Object Selection boundary input");
    std::vector<std::uint8_t> result(count);
    // Support follows the encoder sampling footprint, not viewport scale or
    // brush size. Original pixels are never downsampled for this stage.
    const int spacing = std::max(1, int(std::ceil(double(std::max(e.width, e.height)) / 1024)));
    constexpr std::array<Vec2d, 8> directions { { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 }, { 1, 1 },
        { -1, 1 }, { 1, -1 }, { -1, -1 } } };
    for (unsigned y = 0; y < e.height; ++y) {
        if (cancelled)
            return { };
        for (unsigned x = 0; x < e.width; ++x) {
            const auto i = std::size_t(y) * e.width + x;
            const auto p = image.pixels[i];
            if (!std::isfinite(logits[i]))
                throw std::invalid_argument("Invalid Object Selection model response");
            if (!image.valid[i] || !p.alpha)
                continue;
            float score = logits[i];
            if (std::abs(score) < 8) {
                double foreground = 1e9, background = 1e9;
                for (int radius : { 2, 4, 8, 12 })
                    for (const auto direction : directions) {
                        const int nx = int(x) + int(direction.x) * radius * spacing,
                                  ny = int(y) + int(direction.y) * radius * spacing;
                        if (nx < 0 || ny < 0 || nx >= int(e.width) || ny >= int(e.height))
                            continue;
                        const auto j = std::size_t(ny) * e.width + std::size_t(nx);
                        if (!image.valid[j] || !image.pixels[j].alpha || std::abs(logits[j]) < 4)
                            continue;
                        const auto q = image.pixels[j];
                        const double dr = int(p.red) - q.red, dg = int(p.green) - q.green,
                                     db = int(p.blue) - q.blue;
                        const double distance = std::sqrt((dr * dr + dg * dg + db * db) / 3);
                        auto& best = logits[j] > 0 ? foreground : background;
                        best = std::min(best, distance);
                    }
                if (foreground < 1e9 && background < 1e9)
                    score = .2f * score + float(std::clamp((background - foreground) * .07, -4.0, 4.0));
            }
            result[i] = score > 0 ? 255 : 0;
        }
    }
    return result;
}
}
