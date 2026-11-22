#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace layerdiff {
struct Rect {
    int x{}, y{}, w{}, h{};
    bool empty() const { return w <= 0 || h <= 0; }
};
struct Image {
    int w{}, h{};
    std::vector<uint8_t> rgba;
    Image() = default;
    Image(int width, int height);
};
struct Part {
    std::string name;
    Image pixels;
    int x{}, y{};
};

// Maps a square model grid onto an unscaled rectangle of the input image.
Image modelPage(const Image &source, Rect region, int resolution);
Rect headRegion(const Image &head, int w, int h);
Rect support(const Image &image, int minimumAlpha);
bool lowerGarmentInFront(const Image &source, const Part &upper, const Part &lower);
std::vector<Part> assemble(const std::string &tag, Image piece, Rect region, const Image &source,
                           bool splitPaired);
} // namespace layerdiff
