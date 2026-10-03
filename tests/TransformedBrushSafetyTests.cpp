#include "imageeditor/core/BasicPixelBrushStroke.hpp"
#include "imageeditor/core/BrushTip.hpp"
#include "imageeditor/core/Document.hpp"
#include "imageeditor/core/History.hpp"
#include "imageeditor/core/Layer.hpp"
#include "imageeditor/core/RasterSurface.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace imageeditor::core;

int failures = 0;

void check(bool condition, const char* expression, int line)
{
    if (!condition) {
        std::cerr << "FAIL line " << line << ": " << expression << '\n';
        ++failures;
    }
}

#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

NormalizedPointerSample sample(Vec2d point, std::uint64_t timestamp = 0)
{
    return {
        .documentPosition = point,
        .timestampMicroseconds = timestamp,
        .pressure = 1.0,
        .tiltX = 0.0,
        .tiltY = 0.0,
        .rotationDegrees = 0.0,
        .barrelRotationDegrees = 0.0,
        .pointerType = PointerType::Mouse,
        .buttons = PointerButtonPrimary,
        .modifiers = PointerModifierNone,
    };
}

BrushSettings hardRound(double size)
{
    auto settings = proceduralBrushPreset(ProceduralBrushPreset::HardRound);
    settings.sizePixels = size;
    settings.hardness = 1.0;
    settings.opacity = 1.0;
    settings.flow = 1.0;
    settings.spacingPercent = 10.0;
    settings.foreground = {219, 47, 91, 255};
    settings.pressureToSize = false;
    settings.pressureToFlow = false;
    settings.tip.rotationMode = BrushTipRotationMode::Fixed;
    return settings;
}

Rgba8 pixelAt(const RasterSurface& surface, std::int32_t x, std::int32_t y)
{
    std::array<std::byte, 4> bytes {};
    surface.copyRgba8({x, y, 1, 1}, bytes, 4);
    return {
        std::to_integer<std::uint8_t>(bytes[0]),
        std::to_integer<std::uint8_t>(bytes[1]),
        std::to_integer<std::uint8_t>(bytes[2]),
        std::to_integer<std::uint8_t>(bytes[3]),
    };
}

class NoAllocationSurface final : public RasterSurface {
public:
    explicit NoAllocationSurface(Extent2u extent)
        : extent_(extent)
        , id_(nextId_.fetch_add(1, std::memory_order_relaxed))
    {
    }

    [[nodiscard]] SurfaceId id() const noexcept override { return id_; }
    [[nodiscard]] Extent2u extent() const noexcept override { return extent_; }
    [[nodiscard]] Revision revision() const noexcept override { return revision_; }
    [[nodiscard]] DirtySet dirtySince(Revision) const override
    {
        return {.revision = revision_, .fullRefresh = false, .regions = {}};
    }

    void copyRgba8(RectI region, std::span<std::byte> destination,
        std::size_t destinationStride) const override
    {
        ++copyCalls;
        const auto rowBytes = static_cast<std::size_t>(region.width) * 4U;
        for (std::int32_t row = 0; row < region.height; ++row) {
            const auto offset = static_cast<std::size_t>(row) * destinationStride;
            if (offset + rowBytes <= destination.size()) {
                std::fill_n(destination.begin()
                        + static_cast<std::ptrdiff_t>(offset),
                    static_cast<std::ptrdiff_t>(rowBytes), std::byte {0});
            }
        }
    }

    [[nodiscard]] DirtySet replaceRgba8Batch(
        std::span<const RasterPatch> patches) override
    {
        ++replaceCalls;
        ++revision_;
        std::vector<RectI> regions;
        regions.reserve(patches.size());
        for (const auto& patch : patches) {
            regions.push_back(patch.region);
        }
        return {.revision = revision_, .fullRefresh = false,
            .regions = std::move(regions)};
    }

    [[nodiscard]] DirtySet swapRgba8Batch(
        std::span<MutableRasterPatch> patches) override
    {
        ++swapCalls;
        ++revision_;
        std::vector<RectI> regions;
        regions.reserve(patches.size());
        for (const auto& patch : patches) {
            regions.push_back(patch.region);
        }
        return {.revision = revision_, .fullRefresh = false,
            .regions = std::move(regions)};
    }

    mutable std::size_t copyCalls {0};
    std::size_t replaceCalls {0};
    std::size_t swapCalls {0};

private:
    inline static std::atomic<SurfaceId> nextId_ {2000000};
    Extent2u extent_;
    SurfaceId id_ {0};
    Revision revision_ {1};
};

class CountingGrain final : public IBrushGrain {
public:
    explicit CountingGrain(double value = 1.0)
        : value_(value)
    {
    }

    void prepareDab(const BrushDab&, double) noexcept override
    {
        ++prepareCalls;
    }

    [[nodiscard]] double modulation(Vec2d) const noexcept override
    {
        ++modulationCalls;
        return value_;
    }

    std::size_t prepareCalls {0};
    mutable std::size_t modulationCalls {0};

private:
    double value_ {1.0};
};

class SmallThenOversizedTip final : public IBrushTip {
public:
    explicit SmallThenOversizedTip(double oversizedExtent)
        : oversizedExtent_(oversizedExtent)
    {
    }

    [[nodiscard]] BrushTipBounds prepareDab(const BrushDab& dab,
        double, double) noexcept override
    {
        ++prepareCalls;
        if (prepareCalls == 1) {
            return {dab.documentCenter.x - 0.5, dab.documentCenter.y - 0.5,
                dab.documentCenter.x + 0.5, dab.documentCenter.y + 0.5};
        }
        return {0.0, 0.0, oversizedExtent_, oversizedExtent_};
    }

    [[nodiscard]] double coverage(Vec2d) const noexcept override
    {
        ++coverageCalls;
        return 1.0;
    }

    std::size_t prepareCalls {0};
    mutable std::size_t coverageCalls {0};

private:
    double oversizedExtent_ {0.0};
};

class NonFiniteBoundsTip final : public IBrushTip {
public:
    [[nodiscard]] BrushTipBounds prepareDab(const BrushDab&,
        double, double) noexcept override
    {
        return {0.0, 0.0, std::numeric_limits<double>::infinity(), 1.0};
    }

    [[nodiscard]] double coverage(Vec2d) const noexcept override
    {
        ++coverageCalls;
        return 1.0;
    }

    mutable std::size_t coverageCalls {0};
};

LayerId addRaster(Document& document,
    const std::shared_ptr<RasterSurface>& surface)
{
    auto layer = Layer::raster("Safety target", surface);
    const auto id = layer.id;
    CHECK(document.insertLayer(0, std::move(layer)));
    return id;
}

AffineTransform tinyTransform(Vec2d documentOrigin)
{
    return {
        .m00 = 1.0e-6,
        .m01 = 0.0,
        .m02 = documentOrigin.x,
        .m10 = 0.0,
        .m11 = 1.0e-6,
        .m12 = documentOrigin.y,
    };
}

void unboundedGrowthAtExtremeScaleRejectsSafely()
{
    Document document(CanvasSpec {.extent = {512, 512}});
    auto surface = std::make_shared<ContiguousRasterSurface>(
        Extent2u {8, 8}, Rgba8 {0, 0, 0, 0});
    const auto layerId = addRaster(document, surface);
    const auto transform = tinyTransform({100.0, 100.0});
    CHECK(document.setLayerTransform(layerId, transform));
    const auto documentCenter = transform.map({4.0, 4.0});
    History history;
    BasicPixelBrushStroke stroke(document, layerId, hardRound(64.0));
    CHECK(stroke.valid());
    // The brush now paints beyond the old 8x8 rectangle. At 1e-6 scale
    // its actual local footprint exceeds the same per-dab resource ceiling.
    CHECK(!stroke.begin(sample(documentCenter)));
    CHECK(stroke.end(sample(documentCenter), history)
        == RasterEditCommitResult::TargetUnavailable);
    CHECK(stroke.failure() == BrushStrokeFailure::RasterWorkLimitExceeded);
    CHECK(stroke.stats().rejectedCandidatePixels > kMaximumBrushDabCandidatePixels);
    CHECK(pixelAt(*surface, 4, 4) == Rgba8({0, 0, 0, 0}));
    CHECK(history.undoDepth() == 0);
}

void fakeFiveKSurfaceRejectsBeforeAnyRasterOrGrainWork()
{
    Document document(CanvasSpec {.extent = {512, 512}});
    auto surface = std::make_shared<NoAllocationSurface>(Extent2u {5120, 2880});
    const auto layerId = addRaster(document, surface);
    const auto transform = tinyTransform({100.0, 100.0});
    CHECK(document.setLayerTransform(layerId, transform));
    const auto documentCenter = transform.map({2560.0, 1440.0});
    auto grain = std::make_unique<CountingGrain>();
    auto* grainObserver = grain.get();
    BasicPixelBrushStroke stroke(document, layerId, hardRound(64.0),
        std::make_unique<BasicPixelBrushEngine>(), {}, std::move(grain));
    CHECK(stroke.valid());
    CHECK(!stroke.begin(sample(documentCenter)));
    CHECK(!stroke.active());
    CHECK(stroke.failure() == BrushStrokeFailure::RasterWorkLimitExceeded);
    CHECK(stroke.stats().rejectedCandidatePixels
        > kMaximumBrushDabCandidatePixels);
    CHECK(stroke.stats().maximumCandidatePixels
        == stroke.stats().rejectedCandidatePixels);
    CHECK(grainObserver->prepareCalls == 0);
    CHECK(grainObserver->modulationCalls == 0);
    CHECK(surface->copyCalls == 0);
    CHECK(surface->replaceCalls == 0);
    CHECK(surface->swapCalls == 0);
    CHECK(surface->revision() == 1);
}

void invalidTipBoundsRejectBeforeCoordinateConversion()
{
    Document document(CanvasSpec {.extent = {64, 64}});
    auto surface = std::make_shared<NoAllocationSurface>(Extent2u {64, 64});
    const auto layerId = addRaster(document, surface);
    auto tip = std::make_unique<NonFiniteBoundsTip>();
    auto* tipObserver = tip.get();
    auto grain = std::make_unique<CountingGrain>();
    auto* grainObserver = grain.get();
    BasicPixelBrushStroke stroke(document, layerId, hardRound(8.0),
        std::make_unique<BasicPixelBrushEngine>(), std::move(tip),
        std::move(grain));
    CHECK(!stroke.begin(sample({32.0, 32.0})));
    CHECK(stroke.failure() == BrushStrokeFailure::RasterWorkLimitExceeded);
    CHECK(tipObserver->coverageCalls == 0);
    CHECK(grainObserver->prepareCalls == 0);
    CHECK(grainObserver->modulationCalls == 0);
    CHECK(surface->copyCalls == 0);
    CHECK(surface->replaceCalls == 0);
    CHECK(surface->swapCalls == 0);
}

void priorLiveDabRollsBackForPaintAndErase()
{
    constexpr Extent2u extent {1536, 1536};
    for (const auto mode : {BrushCompositeMode::Paint,
             BrushCompositeMode::Erase}) {
        const Rgba8 original {37, 83, 151, 211};
        Document document(CanvasSpec {.extent = extent});
        auto surface = std::make_shared<ContiguousRasterSurface>(extent, original);
        const auto layerId = addRaster(document, surface);
        auto tip = std::make_unique<SmallThenOversizedTip>(1536.0);
        auto* tipObserver = tip.get();
        auto grain = std::make_unique<CountingGrain>();
        auto* grainObserver = grain.get();
        auto settings = hardRound(1.0);
        settings.spacingPercent = 200.0;
        BasicPixelBrushStroke stroke(document, layerId, settings, mode,
            std::make_unique<BasicPixelBrushEngine>(), std::move(tip),
            std::move(grain));
        CHECK(stroke.valid());
        CHECK(stroke.begin(sample({100.5, 100.5}, 0)));
        CHECK(pixelAt(*surface, 100, 100) != original);
        CHECK(!stroke.append(sample({110.5, 100.5}, 10000)));
        CHECK(stroke.failure() == BrushStrokeFailure::RasterWorkLimitExceeded);
        CHECK(!stroke.active());
        CHECK(pixelAt(*surface, 100, 100) == original);
        CHECK(tipObserver->prepareCalls == 2);
        CHECK(tipObserver->coverageCalls == 1);
        CHECK(grainObserver->prepareCalls == 1);
        CHECK(grainObserver->modulationCalls == 1);
        CHECK(stroke.stats().rejectedCandidatePixels
            == static_cast<std::uint64_t>(extent.width) * extent.height);
        History history;
        CHECK(!history.canUndo());
    }
}

void approvedThousandPixelTipsRemainWithinBudget()
{
    constexpr Extent2u extent {1600, 1600};
    struct Case {
        BrushSettings settings;
        std::uint64_t expectedCandidatePixels;
    };
    auto round = hardRound(1000.0);
    auto bitmap = hardRound(1000.0);
    bitmap.tip.assetId = std::string(BrushAssetIds::DryInkMaskTip);
    bitmap.tip.aspectRatio = 1.0;
    bitmap.tip.angleDegrees = 45.0;
    const std::array cases {
        Case {round, 1002ULL * 1002ULL},
        Case {bitmap, 1418ULL * 1418ULL},
    };

    for (const auto& test : cases) {
        Document document(CanvasSpec {.extent = extent});
        auto surface = std::make_shared<NoAllocationSurface>(extent);
        const auto layerId = addRaster(document, surface);
        auto grain = std::make_unique<CountingGrain>(0.0);
        BasicPixelBrushStroke stroke(document, layerId, test.settings,
            std::make_unique<BasicPixelBrushEngine>(), {}, std::move(grain));
        CHECK(stroke.valid());
        const auto center = Vec2d {800.0, 800.0};
        CHECK(stroke.begin(sample(center)));
        History history;
        CHECK(stroke.end(sample(center), history)
            == RasterEditCommitResult::NoChanges);
        CHECK(stroke.failure() == BrushStrokeFailure::None);
        CHECK(stroke.stats().maximumCandidatePixels
            == test.expectedCandidatePixels);
        CHECK(stroke.stats().maximumCandidatePixels
            <= kMaximumBrushDabCandidatePixels);
        CHECK(stroke.stats().rejectedCandidatePixels == 0);
        CHECK(surface->copyCalls == 0);
        CHECK(surface->replaceCalls == 0);
        CHECK(surface->swapCalls == 0);
        CHECK(!history.canUndo());
    }
}

} // namespace

int main()
{
    unboundedGrowthAtExtremeScaleRejectsSafely();
    fakeFiveKSurfaceRejectsBeforeAnyRasterOrGrainWork();
    invalidTipBoundsRejectBeforeCoordinateConversion();
    priorLiveDabRollsBackForPaintAndErase();
    approvedThousandPixelTipsRemainWithinBudget();

    if (failures != 0) {
        std::cerr << failures << " transformed brush safety assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All transformed brush safety tests passed\n";
    return EXIT_SUCCESS;
}
