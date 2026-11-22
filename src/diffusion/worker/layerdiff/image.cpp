#include "image.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace layerdiff {
Image::Image(int width, int height) : w(width), h(height) {
    if (w <= 0 || h <= 0 || int64_t(w) * h > 67108864)
        throw std::invalid_argument("Invalid LayerDiff image size");
    rgba.resize(size_t(w) * h * 4);
}
namespace {
Rect intersect(Rect a, Rect b) {
    int x = std::max(a.x, b.x), y = std::max(a.y, b.y);
    return {x, y, std::min(a.x + a.w, b.x + b.w) - x, std::min(a.y + a.h, b.y + b.h) - y};
}
struct Tap {
    int pos;
    float weight;
};
using Filter = std::vector<std::vector<Tap>>;
Filter taps(int outputSize, double scale, double origin, int first, int limit) {
    Filter out(outputSize);
    for (int i = 0; i < outputSize; ++i) {
        double left = origin + i * scale;
        if (scale >= 1) {
            double right = left + scale;
            for (int j = std::max(first, int(std::floor(left))); j < std::min(limit, int(std::ceil(right))); ++j) {
                double area = std::min(double(j + 1), right) - std::max(double(j), left);
                if (area > 0)
                    out[i].push_back({j, float(area / scale)});
            }
        } else {
            double center = left + (scale - 1) * .5;
            int j = int(std::floor(center));
            float mix = float(center - j);
            if (j >= first && j < limit)
                out[i].push_back({j, 1 - mix});
            if (j + 1 >= first && j + 1 < limit)
                out[i].push_back({j + 1, mix});
        }
    }
    return out;
}
// Only compute the requested output rectangle. Outside the input region is zero.
Image resize(const Image &image, Rect clip, int w, int h, double scale, double startX, double startY,
             bool opaque) {
    Image result(w, h);
    auto xf = taps(w, scale, startX, clip.x, clip.x + clip.w);
    auto yf = taps(h, scale, startY, clip.y, clip.y + clip.h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            std::array<float, 4> channels{};
            for (auto [sy, wy] : yf[y])
                for (auto [sx, wx] : xf[x]) {
                    const auto *pixel = &image.rgba[(size_t(sy) * image.w + sx) * 4];
                    const auto a = wy * wx;
                    for (int c = 0; c < 3; ++c)
                        channels[c] += a * pixel[c];
                    channels[3] += a * (opaque ? 255 : pixel[3]);
                }
            auto *dest = &result.rgba[(size_t(y) * w + x) * 4];
            for (int c = 0; c < 4; ++c)
                dest[c] = uint8_t(std::clamp(std::lrint(channels[c]), 0L, 255L));
        }
    return result;
}
struct Island {
    int count{};
    double x{}, y{};
};
std::vector<std::pair<std::string, Image>> split(const std::string &name, Image image, bool enabled) {
    if (!enabled)
        return {{name, std::move(image)}};
    const int w = image.w, h = image.h;
    std::vector<int> labels(size_t(w) * h, -1), queue;
    std::vector<Island> islands;
    for (int i = 0; i < w * h; ++i) {
        if (labels[i] >= 0 || image.rgba[4 * i + 3] <= 10)
            continue;
        auto label = int(islands.size());
        islands.emplace_back();
        auto &island = islands.back();
        queue.assign(1, i);
        labels[i] = label;
        for (size_t j = 0; j < queue.size(); ++j) {
            auto k = queue[j], x = k % w, y = k / w;
            island.count++;
            island.x += x;
            island.y += y;
            for (int v = std::max(0, y - 1); v <= std::min(h - 1, y + 1); ++v)
                for (int u = std::max(0, x - 1); u <= std::min(w - 1, x + 1); ++u) {
                    int next = v * w + u;
                    if (labels[next] < 0 && image.rgba[next * 4 + 3] > 10) {
                        queue.push_back(next);
                        labels[next] = label;
                    }
                }
        }
        island.x /= island.count;
        island.y /= island.count;
    }
    if (islands.size() < 2)
        return {{name, std::move(image)}};
    std::array<Island, 2> largest;
    std::partial_sort_copy(islands.begin(), islands.end(), largest.begin(), largest.end(),
                           [](auto a, auto b) { return a.count > b.count; });
    if (largest[1].count < std::max(4, largest[0].count / 20) || std::abs(largest[0].x - largest[1].x) < 2)
        return {{name, std::move(image)}};
    if (largest[0].x > largest[1].x)
        std::swap(largest[0], largest[1]);
    auto isRight = [&](double x, double y) {
        auto distance = [&](const Island &i) { return (x - i.x) * (x - i.x) + (y - i.y) * (y - i.y); };
        return distance(largest[0]) > distance(largest[1]);
    };
    std::vector<bool> side;
    for (auto i : islands)
        side.push_back(isRight(i.x, i.y));
    Image right = image;
    for (int i = 0; i < w * h; ++i) {
        bool r = labels[i] < 0 ? isRight(i % w, i / w) : side[labels[i]];
        (r ? image : right).rgba[4 * i + 3] = 0;
    }
    // In a front-facing illustration, image-left is the character's right.
    return {{name + "-r", std::move(image)}, {name + "-l", std::move(right)}};
}
Image crop(const Image &in, Rect box) {
    Image out(box.w, box.h);
    for (int y = 0; y < box.h; ++y)
        std::copy_n(&in.rgba[(size_t(box.y + y) * in.w + box.x) * 4], box.w * 4, &out.rgba[size_t(y) * box.w * 4]);
    return out;
}
} // namespace
Rect support(const Image &im, int min) {
    int l = im.w, t = im.h, r = -1, b = -1;
    for (int y = 0; y < im.h; ++y)
        for (int x = 0; x < im.w; ++x)
            if (im.rgba[(size_t(y) * im.w + x) * 4 + 3] > min) {
                l = std::min(l, x);
                r = std::max(r, x);
                t = std::min(t, y);
                b = y;
            }
    return {l, t, r - l + 1, b - t + 1};
}
Image modelPage(const Image &source, Rect region, int resolution) {
    int side = std::max(region.w, region.h);
    return resize(source, region, resolution, resolution, double(side) / resolution,
                  region.x - (side - region.w) / 2, region.y - (side - region.h) / 2, true);
}
bool lowerGarmentInFront(const Image &source, const Part &upper, const Part &lower) {
    const auto overlap = intersect(intersect({upper.x, upper.y, upper.pixels.w, upper.pixels.h},
                                             {lower.x, lower.y, lower.pixels.w, lower.pixels.h}),
                                   {0, 0, source.w, source.h});
    // Weight evidence by all three alphas so invisible RGB cannot decide the order.
    // Keep the usual order unless at least 16 opaque pixels' worth of overlap
    // favor the lower garment by more than three RGB levels per channel.
    double improvement = 0, coverage = 0;
    for (int y = overlap.y; y < overlap.y + overlap.h; ++y)
        for (int x = overlap.x; x < overlap.x + overlap.w; ++x) {
            const auto *original = &source.rgba[(size_t(y) * source.w + x) * 4];
            const auto *a = &upper.pixels.rgba[(size_t(y - upper.y) * upper.pixels.w + x - upper.x) * 4];
            const auto *b = &lower.pixels.rgba[(size_t(y - lower.y) * lower.pixels.w + x - lower.x) * 4];
            const auto weight = double(original[3]) * a[3] * b[3] / (255.0 * 255 * 255);
            if (weight == 0)
                continue;
            for (int c = 0; c < 3; ++c)
                improvement += weight * (std::abs(a[c] - original[c]) - std::abs(b[c] - original[c]));
            coverage += weight;
        }
    return coverage >= 16 && improvement > 3 * 3 * coverage;
}
Rect headRegion(const Image &head, int w, int h) {
    auto b = support(head, 15);
    if (b.empty())
        return {};
    int side = std::max(w, h);
    double scale = double(side) / head.w;
    Rect r{int(b.x * scale) - (side - w) / 2, int(b.y * scale) - (side - h) / 2, int(b.w * scale),
           int(b.h * scale)};
    r = intersect(r, {0, 0, w, h});
    if (r.empty())
        return {};
    int dx = r.w < w / 2 ? std::min({w - r.x - r.w, r.x, r.w / 5}) : 0;
    int dy = r.h < h / 2 ? std::min({h - r.y - r.h, r.y, r.h / 5}) : 0;
    return {r.x - dx, r.y - dy, r.w + 2 * dx, r.h + 2 * dy};
}
std::vector<Part> assemble(const std::string &tag, Image image, Rect region, const Image &source,
                           bool splitPaired) {
    bool pair = tag == "irides" || tag == "eyewhite" || tag == "eyelash" || tag == "eyebrow" || tag == "ears" ||
                tag == "handwear";
    std::vector<Part> output;
    for (auto &[name, piece] : split(tag, std::move(image), pair && splitPaired)) {
        auto b = support(piece, 8);
        if (b.empty())
            continue;
        const int side = std::max(region.w, region.h);
        const double scale = double(side) / piece.w;
        const int x0 = region.x - (side - region.w) / 2, y0 = region.y - (side - region.h) / 2;
        const int x = x0 + int(std::floor((b.x - 1) * scale)) - 1, y = y0 + int(std::floor((b.y - 1) * scale)) - 1;
        Rect extent = intersect({x, y, x0 + int(std::ceil((b.x + b.w + 1) * scale)) + 1 - x,
                                 y0 + int(std::ceil((b.y + b.h + 1) * scale)) + 1 - y},
                                region);
        if (extent.empty())
            continue;
        auto pixels = resize(piece, {0, 0, piece.w, piece.h}, extent.w, extent.h, 1 / scale,
                             (extent.x - x0) / scale, (extent.y - y0) / scale, false);
        for (int v = 0; v < extent.h; ++v)
            for (int u = 0; u < extent.w; ++u) {
                auto *d = &pixels.rgba[(size_t(v) * extent.w + u) * 4];
                auto *p = &source.rgba[(size_t(v + extent.y) * source.w + u + extent.x) * 4];
                d[3] = uint8_t((int(d[3]) * p[3] + 127) / 255);
                if (tag == "nose" || tag == "mouth")
                    std::copy_n(p, 3, d);
            }
        auto box = support(pixels, 10);
        if (box.empty())
            continue;
        box = intersect({box.x - 1, box.y - 1, box.w + 2, box.h + 2}, {0, 0, pixels.w, pixels.h});
        output.push_back({name, crop(pixels, box), extent.x + box.x, extent.y + box.y});
    }
    return output;
}
} // namespace layerdiff
