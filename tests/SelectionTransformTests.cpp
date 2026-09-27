#include "imageeditor/core/LayerTransform.hpp"
#include "imageeditor/core/SelectionCommands.hpp"
#include "imageeditor/core/SelectionMask.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace
{
using namespace imageeditor::core;
int failures = 0;
void check(bool result, const char *expression, int line)
{
    if (!result) {
        std::cerr << "FAIL line " << line << ": " << expression << '\n';
        ++failures;
    }
}
#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)
using Plane = std::vector<std::uint8_t>;

bool near(double a, double b, double tolerance = 1.0e-8)
{
    return std::isfinite(a) && std::isfinite(b) && std::abs(a - b) <= tolerance;
}
bool near(Vec2d a, Vec2d b) { return near(a.x, b.x) && near(a.y, b.y); }
bool near(const AffineTransform &a, const AffineTransform &b)
{
    return near(a.m00, b.m00) && near(a.m01, b.m01) && near(a.m02, b.m02) && near(a.m10, b.m10) &&
           near(a.m11, b.m11) && near(a.m12, b.m12);
}

Plane read(const SelectionMask &mask)
{
    const auto e = mask.extent();
    Plane bytes(std::size_t(e.width) * e.height);
    for (unsigned y = 0; y < e.height; ++y)
        for (unsigned x = 0; x < e.width; ++x)
            bytes[std::size_t(y) * e.width + x] = mask.coverageAtDocumentPixel(int(x), int(y));
    return bytes;
}

// Deliberately independent, scalar full-plane reference. Invert explicitly,
// gather four pixel-center samples and use zero extension beyond the canvas.
// Reduction now averages coverage (float mip pyramid + trilinear reconstruction)
// instead of bilinear point-sampling, which aliases small holes and islands.
Plane reference(const Plane &source, Extent2u e, const AffineTransform &map)
{
    Plane output(source.size());
    const auto determinant = map.m00 * map.m11 - map.m01 * map.m10;
    const double a = map.m11 / determinant, b = -map.m01 / determinant, c = -map.m10 / determinant,
                 d = map.m00 / determinant;
    const double norm = a * a + b * b + c * c + d * d, det = a * d - b * c;
    const auto footprint = std::sqrt(.5 * (norm + std::sqrt(std::max(0.0, norm * norm - 4 * det * det))));
    struct Level {
        int w, h;
        std::vector<double> values;
    };
    std::vector<Level> levels;
    int left = int(e.width), top = int(e.height), right = 0, bottom = 0;
    for (unsigned y = 0; y < e.height; ++y)
        for (unsigned x = 0; x < e.width; ++x)
            if (source[std::size_t(y) * e.width + x]) {
                left = std::min(left, int(x));
                top = std::min(top, int(y));
                right = std::max(right, int(x) + 1);
                bottom = std::max(bottom, int(y) + 1);
            }
    if (right <= left || bottom <= top)
        return output;
    if (footprint > 1.000001) {
        Level base{right - left, bottom - top, {}};
        base.values.resize(std::size_t(base.w) * std::size_t(base.h));
        for (int y = 0; y < base.h; ++y)
            for (int x = 0; x < base.w; ++x)
                base.values[std::size_t(y) * std::size_t(base.w) + std::size_t(x)] =
                    source[std::size_t(top + y) * e.width + std::size_t(left + x)];
        levels.push_back(std::move(base));
        while (levels.back().w > 1 || levels.back().h > 1) {
            const auto &old = levels.back();
            Level next{(old.w + 1) / 2, (old.h + 1) / 2, {}};
            next.values.resize(std::size_t(next.w) * std::size_t(next.h));
            for (int y = 0; y < old.h; ++y)
                for (int x = 0; x < old.w; ++x)
                    next.values[std::size_t(y / 2) * std::size_t(next.w) + std::size_t(x / 2)] +=
                        old.values[std::size_t(y) * std::size_t(old.w) + std::size_t(x)] * .25;
            levels.push_back(std::move(next));
        }
    }
    const auto at = [&](int x, int y) -> double {
        return x < 0 || y < 0 || x >= int(e.width) || y >= int(e.height)
                   ? 0.0
                   : source[std::size_t(y) * e.width + unsigned(x)];
    };
    for (unsigned y = 0; y < e.height; ++y) {
        for (unsigned x = 0; x < e.width; ++x) {
            const double dx = x + 0.5 - map.m02, dy = y + 0.5 - map.m12;
            const double sx = (map.m11 * dx - map.m01 * dy) / determinant - 0.5;
            const double sy = (map.m00 * dy - map.m10 * dx) / determinant - 0.5;
            if (!levels.empty()) {
                const double lod = std::clamp(std::log2(footprint), 0.0, double(levels.size() - 1));
                const auto sample = [&](int level) {
                    const auto &l = levels[std::size_t(level)];
                    const double scale = std::ldexp(1.0, level);
                    const double xx = (sx + .5 - left) / scale - .5, yy = (sy + .5 - top) / scale - .5;
                    const int ix = int(std::floor(xx)), iy = int(std::floor(yy));
                    const auto fx = xx - ix, fy = yy - iy;
                    double value = 0;
                    for (int j = 0; j < 2; ++j)
                        for (int i = 0; i < 2; ++i)
                            if (ix + i >= 0 && iy + j >= 0 && ix + i < l.w && iy + j < l.h)
                                value +=
                                    l.values[std::size_t(iy + j) * std::size_t(l.w) + std::size_t(ix + i)] *
                                    (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
                    return value;
                };
                const int level = int(lod);
                const auto fraction = lod - level, first = sample(level);
                const auto value = fraction == 0 ? first : first + fraction * (sample(level + 1) - first);
                output[std::size_t(y) * e.width + x] = std::uint8_t(std::clamp(std::lround(value), 0L, 255L));
                continue;
            }
            if (sx < -1 || sy < -1 || sx >= e.width || sy >= e.height)
                continue;
            const auto ix = int(std::floor(sx)), iy = int(std::floor(sy));
            const double fx = sx - ix, fy = sy - iy;
            const auto top = at(ix, iy) * (1 - fx) + at(ix + 1, iy) * fx;
            const auto bottom = at(ix, iy + 1) * (1 - fx) + at(ix + 1, iy + 1) * fx;
            output[std::size_t(y) * e.width + x] =
                std::uint8_t(std::clamp(std::lround(top * (1 - fy) + bottom * fy), 0L, 255L));
        }
    }
    return output;
}

SelectionState islands(Extent2u e)
{
    auto mask = SelectionMask::rectangle(e, {3, 4, 12, 10});
    mask = mask->combined(*SelectionMask::rectangle(e, {6, 7, 5, 3}), SelectionOperation::Subtract);
    mask = mask->combined(*SelectionMask::rectangle(e, {21, 15, 5, 7}, 93), SelectionOperation::Add);
    return mask->combined(*SelectionMask::rectangle(e, {28, 2, 3, 5}, 192), SelectionOperation::Add);
}

void arbitraryAffineMatchesR8ReferenceAndLeavesOriginalUntouched()
{
    const Extent2u extent{271, 267}; // non-square, partial edge tiles, uniform interior tiles
    Plane noise(std::size_t(extent.width) * extent.height);
    std::uint32_t seed = 0x78219ad5;
    for (auto &value : noise) {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        value = std::uint8_t(seed & 255U);
    }
    const std::array masks{islands(extent), SelectionMask::filled(extent, 173),
                           SelectionMask::fromR8(extent, noise, extent.width),
                           SelectionMask::filled(extent, 0)};
    const std::array<AffineTransform, 10> mappings{{{},
                                                    {1, 0, 19, 0, 1, -7},
                                                    {1, 0, -270, 0, 1, 266},
                                                    {1, 0, 300, 0, 1, -300},
                                                    {1, 0, 0.25, 0, 1, -0.375},
                                                    {2, 0, 7.25, 0, 1.5, -3.125},
                                                    {0.75, 0, -1.25, 0, 0.5, 2.375},
                                                    {-1, 0, 271, 0, 1, 0},
                                                    {0, -1, 267, 1, 0, 0},
                                                    {0.82, -0.31, 7.25, 0.43, 1.12, -11.375}}};
    for (const auto &mask : masks) {
        const auto bytes = read(*mask);
        const auto revision = mask->revision();
        const auto bounds = mask->bounds();
        for (const auto &mapping : mappings) {
            const auto actual = mask->transformed(mapping);
            CHECK(actual->extent() == extent);
            const auto expected = reference(bytes, extent, mapping);
            const auto result = read(*actual);
            const auto mismatch = std::mismatch(result.begin(), result.end(), expected.begin(),
                                                [](int a, int b) { return std::abs(a - b) <= 1; });
            if (mismatch.first != result.end()) {
                const auto i = std::size_t(mismatch.first - result.begin());
                std::cerr << "Coverage mismatch at " << i % extent.width << "," << i / extent.width
                          << " got=" << int(*mismatch.first) << " expected=" << int(*mismatch.second)
                          << " map=" << mapping.m00 << "," << mapping.m01 << "," << mapping.m02 << ","
                          << mapping.m10 << "," << mapping.m11 << "," << mapping.m12
                          << " bounds=" << bounds.width << "x" << bounds.height << "\n";
            }
            // Affine inverse evaluation order can differ at exact .5 ties;
            // allow one R8 quantum for fractional maps, but no larger errors.
            CHECK(std::equal(result.begin(), result.end(), expected.begin(),
                             [](int a, int b) { return std::abs(a - b) <= 1; }));
            CHECK(mask->revision() == revision);
            CHECK(mask->bounds() == bounds);
            CHECK(read(*mask) == bytes);
        }
    }
}

void coverageReductionDoesNotAliasNarrowStripes()
{
    Plane stripes(64 * 64);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
            stripes[std::size_t(y) * 64 + std::size_t(x)] = (x % 4 < 2) ? 255 : 0;
    const auto mask = SelectionMask::fromR8({64, 64}, stripes, 64);
    const auto small = mask->transformed({.125, 0, 10.03, 0, .125, 10.07});
    for (int y = 12; y < 16; ++y)
        for (int x = 12; x < 16; ++x)
            CHECK(std::abs(int(small->coverageAtDocumentPixel(x, y)) - 128) <= 1);
}
void integerTranslationsAndFlipsAreExactAndPreserveHolesAndPartialCoverage()
{
    const Extent2u extent{40, 32};
    const auto source = islands(extent);
    const auto bytes = read(*source);
    for (const auto delta : std::array<Vec2d, 5>{{{0, 0}, {7, -2}, {-20, 4}, {40, 0}, {0, -32}}}) {
        const auto moved = source->transformed({1, 0, delta.x, 0, 1, delta.y});
        CHECK(moved->equivalent(*source->translated(int(delta.x), int(delta.y))));
    }
    const auto horizontal = source->transformed({-1, 0, 40, 0, 1, 0});
    const auto vertical = source->transformed({1, 0, 0, 0, -1, 32});
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 40; ++x) {
            CHECK(horizontal->coverageAtDocumentPixel(x, y) == source->coverageAtDocumentPixel(39 - x, y));
            CHECK(vertical->coverageAtDocumentPixel(x, y) == source->coverageAtDocumentPixel(x, 31 - y));
        }
    CHECK(horizontal->coverageAtDocumentPixel(32, 8) == 0); // real hole, not filled bounds
    CHECK(horizontal->coverageAtDocumentPixel(16, 17) == 93);
    CHECK(horizontal->transformed({-1, 0, 40, 0, 1, 0})->equivalent(*source));
    CHECK(vertical->transformed({1, 0, 0, 0, -1, 32})->equivalent(*source));
    CHECK(read(*source) == bytes);
}

void fractionalEdgesAndRotationUseOriginalCoverageNotRepeatedPreviews()
{
    const Extent2u extent{64, 64};
    const auto dot = SelectionMask::rectangle(extent, {20, 20, 1, 1});
    const auto shifted = dot->transformed({1, 0, 0.5, 0, 1, 0});
    CHECK(shifted->coverageAtDocumentPixel(20, 20) == 128);
    CHECK(shifted->coverageAtDocumentPixel(21, 20) == 128);
    CHECK(shifted->bounds() == RectI({20, 20, 2, 1}));
    const auto source = islands(extent);
    const Vec2d center{32, 32};
    for (double degrees : {0.0, 17.0, 90.0, -90.0, 180.0}) {
        const double radians = degrees * std::numbers::pi / 180.0;
        double c = std::cos(radians), s = std::sin(radians);
        if (std::remainder(degrees, 90.0) == 0) {
            c = std::round(c);
            s = std::round(s);
        }
        const AffineTransform mapping{c, -s, center.x - c * center.x + s * center.y,
                                      s, c,  center.y - s * center.x - c * center.y};
        const auto affine = source->transformed(mapping);
        const auto rotated = source->rotated(degrees, center);
        const auto a = read(*affine), b = read(*rotated);
        CHECK(std::equal(a.begin(), a.end(), b.begin(), [](int x, int y) { return std::abs(x - y) <= 1; }));
    }
    const auto away = source->transformed({1, 0, 60, 0, 1, 0});
    CHECK(away->bounds().width < source->bounds().width);
    CHECK(source->transformed({})->equivalent(*source)); // return from original, not away
    CHECK(!away->transformed({1, 0, -60, 0, 1, 0})->equivalent(*source));
}

void invalidMappingsAreRejectedRatherThanResamplingWithAnInvalidInverse()
{
    const auto mask = islands({40, 32});
    for (const auto map :
         std::array<AffineTransform, 4>{{{0, 0, 0, 0, 1, 0},
                                         {1, 2, 0, 2, 4, 0},
                                         {1, 0, std::numeric_limits<double>::infinity(), 0, 1, 0},
                                         {1, 0, 1e13, 0, 1, 0}}}) {
        bool rejected = false;
        try {
            (void)mask->transformed(map);
        } catch (const std::invalid_argument &) {
            rejected = true;
        }
        CHECK(rejected);
    }
}

void pureDragPreservesAnchorsConstraintsAndSignedZeroCrossing()
{
    const Extent2u extent{100, 80};
    const TransformValues original{{150, 120}, 1, 1, 0, 0};
    const auto start = transformFromValues(original, extent);
    const auto handles = transformHandles(start, extent);
    for (const bool shift : {false, true})
        for (const bool locked : {false, true}) {
            TransformDrag drag(extent, start, original, TransformHandle::BottomRight, handles[4]);
            const auto next = drag.resolve(handles[4] + Vec2d{100, 20}, {shift, false}, locked, original);
            CHECK(next.has_value());
            if (!next)
                continue;
            CHECK(near(transformFromValues(*next, extent).map({0, 0}), handles[0]));
            if (shift != locked)
                CHECK(near(next->scaleX, next->scaleY));
            else {
                CHECK(near(next->scaleX, 2));
                CHECK(near(next->scaleY, 1.25));
            }
        }
    TransformDrag centered(extent, start, original, TransformHandle::Right, handles[3]);
    const auto centeredValues = centered.resolve(handles[3] + Vec2d{25, 0}, {false, true}, false, original);
    CHECK(centeredValues && near(centeredValues->center, original.center));
    CHECK(centeredValues && near(centeredValues->scaleX, 1.5));
    CHECK(centeredValues && near(centeredValues->scaleY, 1));

    TransformDrag crossing(extent, start, original, TransformHandle::Right, handles[3]);
    auto current = original;
    for (double dx : {-99.0, -100.0, -100.001, -125.0, -100.0, -99.0, 0.0}) {
        const auto next = crossing.resolve(handles[3] + Vec2d{dx, 0}, {}, false, current);
        CHECK(next.has_value());
        if (!next)
            continue;
        const auto matrix = transformFromValues(*next, extent);
        CHECK(matrix.inverted().has_value());
        CHECK(std::abs(next->scaleX) >= kMinimumLayerScale);
        CHECK(near(matrix.map({0, 40}), handles[7]));
        if (dx < -100)
            CHECK(next->scaleX < 0);
        if (dx > -100)
            CHECK(next->scaleX > 0);
        current = *next;
    }
}

void extractedDragMatchesLayerTransformIncludingRotatedFlippedAndShearedFrames()
{
    const Extent2u extent{100, 80};
    for (const auto baseline : std::array<TransformValues, 4>{{{{180, 130}, 1, 1, 0, 0},
                                                               {{180, 130}, 1.6, 0.7, 37, 0},
                                                               {{180, 130}, -1.2, 0.8, -123, 0},
                                                               {{180, 130}, 1.2, -0.9, 171, 0.25}}}) {
        for (int index = 0; index <= int(TransformHandle::Rotate); ++index) {
            for (const auto modifiers : {TransformModifiers{}, {true, false}, {false, true}, {true, true}}) {
                Document document(CanvasSpec{.extent = {512, 512}});
                const auto surface =
                    std::make_shared<ContiguousRasterSurface>(extent, Rgba8{17, 31, 73, 191});
                const auto revision = surface->revision();
                auto layer = Layer::raster("Geometry parity", surface);
                const auto id = layer.id;
                layer.localToDocument = transformFromValues(baseline, extent);
                CHECK(document.insertLayer(0, std::move(layer)));
                LayerTransformSession session(document, id);
                CHECK(session.active());
                const auto initial = session.values(); // canonical affine decomposition
                const auto start = session.transform();
                const auto handle = static_cast<TransformHandle>(index);
                const auto press = index < 8 ? transformHandles(start, extent)[std::size_t(index)]
                                             : initial.center + Vec2d{90, 0};
                TransformDrag geometry(extent, start, initial, handle, press);
                CHECK(session.beginDrag(handle, press));
                auto current = initial;
                for (const auto delta : std::array<Vec2d, 3>{{{13, -11}, {-25, 37}, {51, 13}}}) {
                    const auto resolved = geometry.resolve(press + delta, modifiers, true, current);
                    CHECK(resolved.has_value());
                    CHECK(session.dragTo(press + delta, modifiers, true));
                    if (resolved) {
                        CHECK(near(transformFromValues(*resolved, extent), session.transform()));
                        current = *resolved;
                    }
                }
                session.cancel();
                CHECK(document.layer(id)->localToDocument == start);
                CHECK(surface->revision() == revision);
            }
        }
    }
}

void rotationWrapAndShiftSnappingTrackMotionWithoutFullTurnJumps()
{
    const Extent2u extent{100, 80};
    const TransformValues original{{150, 120}, 1, 1, 170, 0};
    const auto point = [&](double degrees) {
        const auto radians = degrees * std::numbers::pi / 180.0;
        return original.center + Vec2d{100 * std::cos(radians), 100 * std::sin(radians)};
    };
    TransformDrag drag(extent, transformFromValues(original, extent), original, TransformHandle::Rotate,
                       point(179));
    auto current = original;
    for (const auto pair :
         std::array<std::array<double, 2>, 4>{{{-179, 172}, {-170, 181}, {170, 161}, {179, 170}}}) {
        const auto resolved = drag.resolve(point(pair[0]), {}, false, current);
        CHECK(resolved && near(resolved->rotationDegrees, pair[1]));
        if (resolved)
            current = *resolved;
    }
    const auto snapped = drag.resolve(point(-175), {true, false}, false, current);
    CHECK(snapped && near(snapped->rotationDegrees, 180));
}
} // namespace

int main()
{
    arbitraryAffineMatchesR8ReferenceAndLeavesOriginalUntouched();
    coverageReductionDoesNotAliasNarrowStripes();
    integerTranslationsAndFlipsAreExactAndPreserveHolesAndPartialCoverage();
    fractionalEdgesAndRotationUseOriginalCoverageNotRepeatedPreviews();
    invalidMappingsAreRejectedRatherThanResamplingWithAnInvalidInverse();
    pureDragPreservesAnchorsConstraintsAndSignedZeroCrossing();
    extractedDragMatchesLayerTransformIncludingRotatedFlippedAndShearedFrames();
    rotationWrapAndShiftSnappingTrackMotionWithoutFullTurnJumps();
    if (failures)
        std::cerr << failures << " selection transform assertions failed\n";
    else
        std::cout << "Selection affine coverage and shared transform geometry passed\n";
    return failures ? 1 : 0;
}
