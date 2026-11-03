#pragma once
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>
#include <stb_image_resize.h>

namespace vulkana::diffusion {
struct ReferenceSize { int width, height; };
// Fit proportionally, then pad to the encoder grid. Small references receive
// only alignment padding instead of being enlarged to a full output-sized image.
inline ReferenceSize boundedReferenceSize(int width, int height, int limit, int alignment = 32)
{
    if (width <= 0 || height <= 0 || limit < 64 || limit > 8192 || alignment <= 0 || alignment > limit)
        throw std::invalid_argument("Invalid reference image size limit");
    const int maximum = limit / alignment * alignment;
    const double scale = std::min(1.0, double(maximum) / std::max(width, height));
    const auto aligned = [&](int side) {
        return std::clamp(int(std::ceil(side * scale / alignment)) * alignment, alignment, maximum);
    };
    return {aligned(width), aligned(height)};
}
struct SizedReference {
    ReferenceSize size;
    std::vector<unsigned char> pixels;
    int contentWidth{}, contentHeight{}, left{}, top{};
};
// Preserve the entire image and its proportions. Extend edge pixels only for
// the encoder grid; constant/transparent borders can become visible in results.
inline SizedReference resizeReference(const unsigned char* source, int width, int height, int channels, int limit)
{
    const auto size = boundedReferenceSize(width, height, limit);
    if (!source || channels < 1 || channels > 4)
        throw std::invalid_argument("Invalid reference image pixels");
    const double scale = std::min({1.0, double(size.width) / width, double(size.height) / height});
    const int w = std::max(1, int(std::round(width * scale)));
    const int h = std::max(1, int(std::round(height * scale)));
    std::vector<unsigned char> resized(std::size_t(w) * h * channels);
    if (w == width && h == height) std::copy_n(source, resized.size(), resized.data());
    else if (!stbir_resize_uint8_generic(source, width, height, 0, resized.data(), w, h, 0,
                 channels, channels == 4 ? 3 : channels == 2 ? 1 : STBIR_ALPHA_CHANNEL_NONE,
                 0, STBIR_EDGE_CLAMP, STBIR_FILTER_TRIANGLE, STBIR_COLORSPACE_LINEAR, nullptr))
        throw std::bad_alloc();
    SizedReference result{size, std::vector<unsigned char>(std::size_t(size.width) * size.height * channels)};
    const int left = (size.width - w) / 2, top = (size.height - h) / 2;
    result.contentWidth = w; result.contentHeight = h; result.left = left; result.top = top;
    for (int y = 0; y < size.height; ++y)
        for (int x = 0; x < size.width; ++x) {
            const auto from = (std::size_t(std::clamp(y - top, 0, h - 1)) * w + std::clamp(x - left, 0, w - 1)) * channels;
            const auto to = (std::size_t(y) * size.width + x) * channels;
            std::copy_n(resized.data() + from, channels, result.pixels.data() + to);
        }
    return result;
}
}
