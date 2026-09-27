#include "imageeditor/render/DirtyRegionCoalescer.hpp"
#include "imageeditor/render/RasterUploadPlan.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* expression, int line)
{
    if (!condition) {
        std::cerr << "FAIL line " << line << ": " << expression << '\n';
        ++failures;
    }
}

#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

using imageeditor::core::Extent2u;
using imageeditor::core::RectI;
using imageeditor::render::coalesceDirtyRegionsForUpload;
using imageeditor::render::makeRasterUploadPlan;

bool overlaps(RectI left, RectI right)
{
    return !left.clippedTo(right).empty();
}

void thousandsOfRunsCollapseToTouchedGridCells()
{
    std::vector<RectI> runs;
    for (int row = 0; row < 64; ++row) {
        for (int repeat = 0; repeat < 50; ++repeat) {
            runs.push_back({4 + repeat % 24, row, 2, 1});
            runs.push_back({68 + repeat % 24, row, 2, 1});
        }
    }
    const auto result = coalesceDirtyRegionsForUpload(runs, {128, 64});
    CHECK(result == std::vector<RectI>({{4, 0, 25, 64}, {68, 0, 25, 64}}));
}

void crossingRegionsSplitAtGridAndSurfaceEdges()
{
    const std::vector<RectI> input {{-10, 60, 90, 20}, {63, 63, 4, 4}};
    const auto result = coalesceDirtyRegionsForUpload(input, {70, 70});
    CHECK(result == std::vector<RectI>({
        {0, 60, 64, 4}, {0, 64, 64, 6},
        {64, 60, 6, 4}, {64, 64, 6, 6}}));
    for (std::size_t first = 0; first < result.size(); ++first) {
        for (std::size_t second = first + 1; second < result.size(); ++second) {
            CHECK(!overlaps(result[first], result[second]));
        }
    }
}

void emptyAndInvalidInputsAreControlled()
{
    CHECK(coalesceDirtyRegionsForUpload({}, Extent2u {64, 64}).empty());
    const std::vector<RectI> outside {{100, 100, 4, 4}, {0, 0, 0, 0}};
    CHECK(coalesceDirtyRegionsForUpload(outside, {64, 64}).empty());
    bool rejectedZeroGrid = false;
    try {
        (void)coalesceDirtyRegionsForUpload(outside, {64, 64}, 0);
    } catch (const std::invalid_argument&) {
        rejectedZeroGrid = true;
    }
    CHECK(rejectedZeroGrid);
}

void uploadPlanPacksAlignedRgbaCopies()
{
    const std::vector<RectI> regions {
        {2, 3, 3, 2},
        {17, 19, 1, 1},
        {40, 50, 2, 3},
    };
    const auto plan = makeRasterUploadPlan(regions, 256);
    CHECK(plan.copies.size() == 3);
    CHECK(plan.copies[0].bufferOffset == 0);
    CHECK(plan.copies[0].rowBytes == 12);
    CHECK(plan.copies[0].byteCount == 24);
    CHECK(plan.copies[1].bufferOffset == 256);
    CHECK(plan.copies[1].byteCount == 4);
    CHECK(plan.copies[2].bufferOffset == 512);
    CHECK(plan.copies[2].byteCount == 24);
    CHECK(plan.stagingBytes == 536);
    CHECK(plan.pixelBytes == 52);
    for (const auto& copy : plan.copies) {
        CHECK(copy.bufferOffset % 256 == 0);
    }
    CHECK(makeRasterUploadPlan({}, 4).copies.empty());
}

void uploadPlanRejectsInvalidAndOverflowingInputs()
{
    bool rejectedZeroAlignment = false;
    try {
        (void)makeRasterUploadPlan({}, 0);
    } catch (const std::invalid_argument&) {
        rejectedZeroAlignment = true;
    }
    CHECK(rejectedZeroAlignment);

    bool rejectedEmptyRegion = false;
    try {
        const std::vector<RectI> emptyRegion {{0, 0, 0, 1}};
        (void)makeRasterUploadPlan(emptyRegion, 4);
    } catch (const std::invalid_argument&) {
        rejectedEmptyRegion = true;
    }
    CHECK(rejectedEmptyRegion);

    bool rejectedOverflow = false;
    try {
        constexpr auto maximum = std::numeric_limits<std::int32_t>::max();
        const std::vector<RectI> enormous {
            {0, 0, maximum, maximum},
            {0, 0, maximum, maximum},
        };
        (void)makeRasterUploadPlan(enormous, 4);
    } catch (const std::overflow_error&) {
        rejectedOverflow = true;
    }
    CHECK(rejectedOverflow);
}

} // namespace

int main()
{
    thousandsOfRunsCollapseToTouchedGridCells();
    crossingRegionsSplitAtGridAndSurfaceEdges();
    emptyAndInvalidInputsAreControlled();
    uploadPlanPacksAlignedRgbaCopies();
    uploadPlanRejectsInvalidAndOverflowingInputs();
    if (failures != 0) {
        std::cerr << failures << " dirty-region coalescer assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All dirty-region coalescer tests passed\n";
    return EXIT_SUCCESS;
}
