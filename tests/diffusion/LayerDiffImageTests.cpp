#include "layerdiff/image.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace layerdiff;
using Pixel = std::array<uint8_t, 4>;
void check(bool ok, const char* description) {
    if (!ok)
        throw std::runtime_error(description);
}
void setPixel(Image& im, int x, int y, Pixel p) {
    std::copy(p.begin(), p.end(), im.rgba.begin() + (y * im.w + x) * 4);
}
Pixel pixel(const Image& im, int x, int y) {
    Pixel p{};
    std::copy_n(im.rgba.data() + (y * im.w + x) * 4, 4, p.begin());
    return p;
}
Pixel atSource(const Part& part, int x, int y) {
    int px = x - part.x, py = y - part.y;
    return px < 0 || py < 0 || px >= part.pixels.w || py >= part.pixels.h ? Pixel{}
                                                                          : pixel(part.pixels, px, py);
}
Image solid(int w, int h, Pixel p) {
    Image img(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            setPixel(img, x, y, p);
    return img;
}
void garmentOverlapOrder() {
    const Pixel fabric{38, 35, 33, 255}, skin{205, 166, 149, 255};
    const Part lower{"bottomwear", solid(12, 8, fabric), 2, 3};
    Part upper{"topwear", solid(8, 7, skin), 5, 4};
    check(lowerGarmentInFront(solid(16, 16, fabric), upper, lower), "Lower garment should cover generated skin");
    check(!lowerGarmentInFront(solid(16, 16, skin), upper, lower), "Upper garment matching source should stay in front");
    check(!lowerGarmentInFront(solid(16, 16, {38, 35, 33, 0}), upper, lower), "Hidden RGB decided garment order");
    upper.pixels = solid(8, 7, {40, 37, 35, 255});
    check(!lowerGarmentInFront(solid(16, 16, fabric), upper, lower), "Similar colors should keep the usual garment order");
    upper.x = 14;
    check(!lowerGarmentInFront(solid(16, 16, fabric), upper, lower), "Disjoint parts changed order");
}
void centeredRgbConditioning() {
    for (auto shape : {std::pair{5, 8}, std::pair{8, 5}}) {
        auto [w, h] = shape;
        auto im = solid(w, h, {12, 32, 62, 0});
        setPixel(im, 0, 0, {255, 1, 4, 1});
        const auto model = modelPage(im, {0, 0, w, h}, 8);
        int left = (8 - w) / 2, top = (8 - h) / 2;
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x) {
                auto p = pixel(model, x, y);
                if (x == left && y == top)
                    check(p == Pixel{255, 1, 4, 255}, "First source pixel misaligned");
                else if (x >= left && x < left + w && y >= top && y < top + h)
                    check(p == Pixel{12, 32, 62, 255}, "Lost hidden source RGB during input conditioning");
                else
                    check(p == Pixel{}, "Nontransparent padding outside the source");
            }
    }
}
void headPlacement() {
    Image head(16, 16);
    check(headRegion(head, 32, 64).empty(), "An empty head must omit the second pass");
    for (int y = 3; y < 7; ++y)
        for (int x = 7; x < 10; ++x)
            setPixel(head, x, y, {88, 89, 90, 16});
    setPixel(head, 15, 0, {1, 2, 3, 15});
    auto crop = headRegion(head, 32, 64);
    check(crop.x == 10 && crop.y == 9 && crop.w == 16 && crop.h == 22,
          "Head margin or inverse portrait padding changed");
    auto src = solid(32, 64, {32, 78, 145, 255});
    auto page = modelPage(src, crop, 22);
    check(pixel(page, 2, 10)[3] == 0 && pixel(page, 3, 10)[3] == 255,
          "Head crop did not receive its own padding");
    // Keep bounds and tag names in the original layer's pixel grid.
    auto mask = solid(22, 22, {245, 230, 220, 0});
    setPixel(mask, 8, 7, {200, 198, 190, 255});
    auto parts = assemble("irides", mask, crop, src, false);
    check(parts.size() == 1 && parts.front().name == "irides", "Expected one head tag");
    check(atSource(parts[0], 15, 16) == Pixel{200, 198, 190, 255},
          "Incorrect head-pass placement on the source grid");
}
void softAlphaAndColorRestoration() {
    Image source = solid(10, 10, {32, 78, 145, 255});
    setPixel(source, 3, 5, {73, 12, 13, 128});
    setPixel(source, 4, 5, {1, 2, 3, 0});
    Image generated(10, 10);
    for (int y = 3; y < 7; ++y)
        for (int x = 2; x < 7; ++x)
            setPixel(generated, x, y, {244, 200, 80, 128});
    setPixel(generated, 9, 0, {155, 60, 30, 1});
    Rect full{0, 0, 10, 10};
    for (auto tag : {"mouth", "nose", "face"}) {
        auto result = assemble(tag, generated, full, source, true);
        check(result.size() == 1, "Unexpected empty result or spurious left/right split");
        const auto& p = result[0];
        check(p.x == 1 && p.y == 2 && p.pixels.w == 7 && p.pixels.h == 6,
              "Soft edge apron or noisy-pixel bounds changed");
        bool restore = std::string(tag) != "face";
        check(atSource(p, 3, 5) == (restore ? Pixel{73, 12, 13, 64} : Pixel{244, 200, 80, 64}),
              "Source alpha or input color restoration failed");
        check(atSource(p, 4, 5)[3] == 0, "Introduced pixels inside a source transparency hole");
    }
    check(assemble("nose", generated, full, solid(10, 10, {3, 4, 5, 0}), false).empty(),
          "Fully masked output survived");
}
void pairedComponents() {
    auto source = solid(14, 14, {15, 24, 33, 255});
    Image mask(14, 14);
    for (int y = 6; y < 9; ++y)
        for (auto start : {2, 10})
            for (int x = start; x < start + 3; ++x)
                setPixel(mask, x, y, {24, 61, 203, 195});
    setPixel(mask, 3, 9, {25, 62, 202, 8});
    const Rect full{0, 0, 14, 14};
    auto original = assemble("eyelash", mask, full, source, false);
    auto split = assemble("eyelash", mask, full, source, true);
    check(original.size() == 1 && split.size() == 2, "Paired-part toggle did not control splitting");
    check(split[0].name == "eyelash-r" && split[1].name == "eyelash-l",
          "Character-left/right names are reversed");
    for (int y = 0; y < 14; ++y)
        for (int x = 0; x < 14; ++x)
            check(atSource(split[0], x, y)[3] + atSource(split[1], x, y)[3] == atSource(original[0], x, y)[3],
                  "Splitting changed the assembled alpha");
    check(atSource(split[0], 3, 7)[3] == 195 && atSource(split[1], 11, 7)[3] == 195,
          "Left/right part placement is reversed");
    check(assemble("front hair", mask, full, source, true).size() == 1, "Split a non-paired part");
    Image speck(14, 14);
    for (int y = 6; y < 9; ++y)
        for (int x = 2; x < 5; ++x)
            setPixel(speck, x, y, {24, 61, 203, 195});
    setPixel(speck, 10, 7, {24, 61, 203, 195});
    check(assemble("irides", speck, full, source, true).size() == 1,
          "Fabricated a second eye from a single speck");
}
} // namespace

int main() {
    try {
        centeredRgbConditioning();
        garmentOverlapOrder();
        headPlacement();
        softAlphaAndColorRestoration();
        pairedComponents();
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
