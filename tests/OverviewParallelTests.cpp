#include "imageeditor/ui/FlattenedDocument.hpp"
#include "imageeditor/ui/QtShapeRenderService.hpp"
#include "imageeditor/core/BoundedParallel.hpp"
#include "imageeditor/core/ColorSampler.hpp"
#include "imageeditor/core/ColorMath.hpp"
#include "imageeditor/core/SpatialFilterCache.hpp"
#include <QGuiApplication>
#include <atomic>
#include <future>
#include <iostream>
#include <mutex>
#include <set>
#include <thread>

namespace c = imageeditor::core;
namespace u = imageeditor::ui;
namespace {
int failures = 0;
#define CHECK(v) do { if (!(v)) { ++failures; std::cerr << "FAIL " << __LINE__ << ": " << #v << '\n'; } } while(false)
class ObservedSurface final : public c::RasterSurface {
public:
    std::shared_ptr<c::ContiguousRasterSurface> source;
    mutable std::atomic<int> readers {0}, reads {0};
    mutable std::mutex mutex;
    mutable std::set<std::thread::id> threads;
    bool failReads {};
    explicit ObservedSurface(c::Extent2u size) : source(std::make_shared<c::ContiguousRasterSurface>(size, c::Rgba8{30,80,140,220})) {}
    c::SurfaceId id() const noexcept override { return source->id(); }
    c::Extent2u extent() const noexcept override { return source->extent(); }
    c::Revision revision() const noexcept override { return source->revision(); }
    c::DirtySet dirtySince(c::Revision revision) const override { return source->dirtySince(revision); }
    void copyRgba8(c::RectI rect, std::span<std::byte> bytes, std::size_t stride) const override
    {
        ++readers; ++reads;
        struct Guard { std::atomic<int>& n; ~Guard() { --n; } } guard{readers};
        { std::lock_guard lock(mutex); threads.insert(std::this_thread::get_id()); }
        if (failReads) throw std::bad_alloc();
        source->copyRgba8(rect, bytes, stride);
    }
    c::DirtySet replaceRgba8Batch(std::span<const c::RasterPatch> patches) override { return source->replaceRgba8Batch(patches); }
    c::DirtySet swapRgba8Batch(std::span<c::MutableRasterPatch> patches) override { return source->swapRgba8Batch(patches); }
};
void overviewRows()
{
    c::Document document({{193,127},96});
    auto source = std::make_shared<ObservedSurface>(c::Extent2u{193,127});
    auto base = c::Layer::raster("Base", source); const auto id = base.id;
    CHECK(document.insertLayer(0, std::move(base)));
    auto upper = c::Layer::raster("Clipped dissolve", std::make_shared<c::ContiguousRasterSurface>(
        c::Extent2u{100,80}, c::Rgba8{190,30,60,170}));
    const auto upperId = upper.id;
    upper.localToDocument = {1.1,.15,24,-.1,.8,18}; upper.blendMode = c::BlendMode::Dissolve;
    CHECK(document.insertLayer(1, std::move(upper)));
    const auto group = c::makeLayerId(); auto tree = document.tree();
    tree.containers.push_back({group,"Clip",c::ContainerKind::ClippingMaskGroup,c::ColorLabel::None,{id,upperId}});
    tree.roots = {group}; CHECK(document.replaceStructure(document.tree(), tree));
    const auto owner = std::this_thread::get_id();
    std::uint64_t last = 0; bool advanced = false;
    auto progress = [&](auto done, auto total) {
        CHECK(std::this_thread::get_id() == owner && source->readers == 0);
        CHECK(done >= last && done <= total); last = done; advanced |= done > document.layers().size() && done < total;
        return true;
    };
    const auto image = u::renderDocumentOverview(document, {96,64}, progress);
    CHECK(image && advanced && source->readers == 0);
    const auto workers = c::boundedParallelWorkerCount();
    CHECK(source->threads.size() == workers); // Every participant has a nonempty row.

    // Independent scalar reference retains exact subpixel positions, dissolve
    // decisions and accumulation order, including noninteger aspect ratios.
    c::PinnedDocumentSampler scalar(document, {}, c::ColorSampleSource::MergedVisible);
    if (image) for (int y = 0; y < 64; ++y) for (int x = 0; x < 96; ++x) {
        std::array<double,4> sum {};
        for (double dy : {.25,.75}) for (double dx : {.25,.75}) {
            const auto sample = scalar.sampleLinear({(x+dx)*(193.0/96),(y+dy)*(127.0/64)});
            for (std::size_t i = 0; i < 4; ++i) sum[i] += double(sample[i])*.25;
        }
        auto expected = c::encodeColor({float(sum[0]),float(sum[1]),float(sum[2]),float(sum[3])});
        if (!expected.alpha) expected = {};
        const auto* p = image.image.constScanLine(y) + x*4;
        CHECK((expected == c::Rgba8{p[0],p[1],p[2],p[3]}));
    }
    const auto cancelled = u::renderDocumentOverview(document, {96,64}, [&](auto done, auto) {
        CHECK(std::this_thread::get_id() == owner && source->readers == 0);
        return done <= document.layers().size();
    });
    CHECK(cancelled.cancelled && cancelled.image.isNull() && source->readers == 0);
    bool changed = false; int readsAtChange = 0;
    const auto stale = u::renderDocumentOverview(document, {96,64}, [&](auto done, auto) {
        CHECK(source->readers == 0);
        if (!changed && done > document.layers().size()) {
            changed = true; readsAtChange = source->reads;
            const std::array bytes{std::byte{255},std::byte{0},std::byte{0},std::byte{255}};
            source->replaceRgba8({0,0,1,1},bytes,4);
        }
        return true;
    });
    CHECK(changed && !stale && stale.error.contains("changed"));
    CHECK(source->readers == 0 && source->reads == readsAtChange);
    source->failReads = true;
    const auto failed = u::renderDocumentOverview(document, {96,64});
    CHECK(!failed && failed.error.contains("memory") && source->readers == 0);
    source->failReads = false;
    CHECK(u::renderDocumentOverview(document, {96,64})); // Pool survives worker errors.

    if (workers > 1) {
        std::promise<void> occupied, release; auto released = release.get_future().share();
        auto busy = std::async(std::launch::async, [&] {
            return c::boundedParallel(workers, [&](unsigned rank, unsigned) {
                if (!rank) occupied.set_value();
                released.wait();
            });
        });
        occupied.get_future().wait();
        const auto fallback = u::renderDocumentOverview(document, {96,64});
        release.set_value(); CHECK(busy.get()); CHECK(fallback);
    }
}
void coherentEffectSourceReuse()
{
    c::Document document({{193,127},96});
    c::ShapeLayer shape; shape.size = {80,60}; shape.fillColor = {180,80,20,180};
    auto layer = c::Layer::shape("Styled shape",shape); const auto id = layer.id;
    auto effects = std::make_shared<c::LayerEffectStack>(); effects->items[std::size_t(c::LayerEffectType::OuterGlow)].enabled = true;
    layer.effects = effects; CHECK(document.insertLayer(0, std::move(layer)));
    const auto cold = u::renderDocumentOverview(document,{96,64}); CHECK(cold);
    const auto rendered = u::QtShapeRenderService{}.render({shape,1});
    auto observed = std::make_shared<ObservedSurface>(rendered->surface->extent());
    const auto e = observed->extent(); std::vector<std::byte> bytes(std::size_t(e.width)*e.height*4);
    rendered->surface->copyRgba8({0,0,int(e.width),int(e.height)},bytes,std::size_t(e.width)*4);
    observed->replaceRgba8({0,0,int(e.width),int(e.height)},bytes,std::size_t(e.width)*4);
    auto cache = std::make_shared<c::LayerRenderCache>(*rendered);
    cache->contentRevision = document.layer(id)->shapeRevision; cache->surface = observed;
    document.layer(id)->renderCache = cache;
    document.layer(id)->effectCache = c::prepareLayerEffects(*document.layer(id), {});
    const auto originalEffects = document.layer(id)->effectCache;
    observed->reads = 0;
    const auto warm = u::renderDocumentOverview(document,{96,64});
    CHECK(warm && warm.image == cold.image && observed->reads > 0);
    CHECK(document.layer(id)->renderCache == cache && document.layer(id)->effectCache == originalEffects);
    for (int invalid = 0; invalid < 3; ++invalid) {
        auto wrong = std::make_shared<c::LayerRenderCache>(*cache);
        if (invalid == 0) wrong->contentRevision = 0;
        if (invalid == 1) wrong->density = .5;
        if (invalid == 2) wrong->rasterizedDocumentTransform = c::AffineTransform{};
        document.layer(id)->renderCache = wrong; observed->reads = 0;
        const auto fresh = u::renderDocumentOverview(document,{96,64});
        CHECK(fresh && fresh.image == cold.image && observed->reads == 0);
    }
}
}
int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    if (app.arguments().contains("--serial")) CHECK(c::configureBoundedParallelLimit(1));
    overviewRows(); coherentEffectSourceReuse();
    std::cout << "Overview parallelism: " << failures << " failures\n";
    return failures ? 1 : 0;
}
