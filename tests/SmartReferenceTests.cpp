#include "imageeditor/core/SmartSelection.hpp"
#include "imageeditor/core/BoundedParallel.hpp"
#include "imageeditor/core/SpatialFilterCache.hpp"
#include "imageeditor/ui/FlattenedDocument.hpp"
#include "imageeditor/ui/ProjectFile.hpp"
#include <QApplication>
#include <QCryptographicHash>
#include <QImage>
#include <QStandardPaths>
#include <chrono>
#include <iostream>
#include <future>
#include <thread>

namespace c = imageeditor::core;
namespace u = imageeditor::ui;
namespace {
int failures = 0;
#define CHECK(...)                                                                                           \
    do {                                                                                                     \
        if (!(__VA_ARGS__)) {                                                                                \
            ++failures;                                                                                      \
            std::cerr << "FAIL " << __LINE__ << ": " << #__VA_ARGS__ << '\n';                                \
        }                                                                                                    \
    } while (false)
std::shared_ptr<const c::SmartReferenceImage> finish(c::SmartSelectionReference& source)
{
    while (!source.step(3072)) { }
    return source.image();
}
std::shared_ptr<c::RasterSurface> solid(c::Extent2u size, c::Rgba8 color)
{
    return std::make_shared<c::ContiguousRasterSurface>(size, color);
}
void typedRenderedReferences()
{
    for (bool text : { false, true }) {
        c::Document document({ { 112, 96 } });
        auto base = c::Layer::raster("Backdrop", solid({ 112, 96 }, { 110, 70, 40, 192 }));
        CHECK(document.insertLayer(0, base));
        c::TextLayer t;
        t.utf8 = "O O\nVulkana";
        t.defaultStyle.sizePixels = 18.5;
        t.defaultStyle.color = { 220, 130, 60, 177 };
        c::ShapeLayer s;
        s.kind = c::ShapeKind::Triangle;
        s.size = { 47.5, 37.25 };
        s.fillColor = { 220, 130, 60, 177 };
        s.strokeEnabled = true;
        s.strokeWidth = 1.25;
        s.strokeColor = { 30, 220, 80, 128 };
        auto layer = text ? c::Layer::text("Text", t) : c::Layer::shape("Shape", s);
        layer.localToDocument = { -1.1, -.18, 95.5, -.24, .9, 27.5 };
        layer.opacity = .63F;
        layer.blendMode = c::BlendMode::Screen;
        layer.crop = c::LayerCrop { 1.25, 1.5, 65, 38 };
        layer.crop->corners = { 4.5, 8, 2.75, 5 };
        auto adjustments = std::make_shared<c::AdjustmentStack>();
        adjustments->items[std::size_t(c::AdjustmentType::Invert)].enabled = true;
        layer.adjustments = adjustments;
        CHECK(document.insertLayer(1, layer));
        const auto cache = u::prepareDocumentSampleCache(layer, 1'000'000);
        CHECK(cache && cache->surface);
        if (!cache)
            return;
        const std::array overrides { c::SampleCacheOverride { layer.id, cache } };
        const auto revision = document.revision();
        c::SmartSelectionReference merged(document, layer.id, c::ColorSampleSource::MergedVisible, overrides);
        const auto image = finish(merged);
        const auto flat = u::flattenDocument(document);
        CHECK(flat);
        if (!flat)
            continue;
        for (unsigned y = 0; y < 96; ++y)
            for (unsigned x = 0; x < 112; ++x) {
                const auto a = image->pixels[std::size_t(y) * 112 + x];
                const auto b = flat.image.pixelColor(int(x), int(y));
                CHECK(a.red == b.red() && a.green == b.green() && a.blue == b.blue() && a.alpha == b.alpha());
            }
        c::Document isolated({ { 112, 96 } });
        auto single = layer;
        single.blendMode = c::BlendMode::Normal;
        CHECK(isolated.insertLayer(0, single));
        const auto isolatedFlat = u::flattenDocument(isolated);
        CHECK(isolatedFlat);
        c::SmartSelectionReference active(document, layer.id, c::ColorSampleSource::ActiveLayer, overrides);
        const auto activeImage = finish(active);
        unsigned partial = 0, invalid = 0;
        for (unsigned y = 0; y < 96; ++y)
            for (unsigned x = 0; x < 112; ++x) {
                const auto i = std::size_t(y) * 112 + x;
                const auto a = activeImage->pixels[i];
                const auto b = isolatedFlat.image.pixelColor(int(x), int(y));
                if (activeImage->valid[i]) {
                    // Smart reference canonicalizes invisible RGB after RGBA8 alpha
                    // quantization; export may retain a color with rounded-zero alpha.
                    CHECK(a.alpha == b.alpha());
                    if (a.alpha)
                        CHECK(a.red == b.red() && a.green == b.green() && a.blue == b.blue());
                    else
                        CHECK(a == c::Rgba8({ 0, 0, 0, 0 }));
                } else {
                    ++invalid;
                    CHECK(a.alpha == 0);
                }
                if (a.alpha > 0 && a.alpha < 255)
                    ++partial;
            }
        CHECK(partial > 0 && invalid > 0);
        CHECK(document.revision() == revision && !document.isModified());
        CHECK(!document.layer(layer.id)->renderCache); // Detached caches never installed by sampling.
        auto tree = document.tree();
        const auto old = tree;
        tree.roots = { base.id, 900000 };
        tree.containers.push_back(
            { 900000, "Hidden parent", c::ContainerKind::Folder, c::ColorLabel::None, { layer.id }, false });
        CHECK(document.replaceStructure(old, tree));
        CHECK(!active.matches(document));
        bool rejected = false;
        try {
            c::SmartSelectionReference hidden(
                document, layer.id, c::ColorSampleSource::ActiveLayer, overrides);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        CHECK(rejected);
        c::SmartSelectionReference hiddenMerged(
            document, layer.id, c::ColorSampleSource::MergedVisible, overrides);
        const auto hiddenImage = finish(hiddenMerged);
        CHECK(hiddenImage->pixels[300] == c::Rgba8({ 110, 70, 40, 192 }));
    }
}
void cropAndAlpha()
{
    c::Document document({ { 20, 20 } });
    auto layer = c::Layer::raster("Crop", solid({ 20, 20 }, { 20, 90, 180, 200 }));
    layer.crop = c::LayerCrop { 3.25, 3.75, 12.5, 12.25 };
    layer.crop->corners = { 4.5, 4.5, 4.5, 4.5 };
    layer.opacity = .5F;
    CHECK(document.insertLayer(0, layer));
    c::SmartSelectionReference active(document, layer.id, c::ColorSampleSource::ActiveLayer);
    const auto image = finish(active);
    CHECK(!image->valid[3 * 20 + 3]);
    CHECK(image->valid[10 * 20 + 10]);
    CHECK(image->pixels[10 * 20 + 10].alpha == 100);
    unsigned partial = 0;
    for (std::size_t i = 0; i < image->pixels.size(); ++i)
        if (image->pixels[i].alpha > 0 && image->pixels[i].alpha < 100) {
            CHECK(image->valid[i]);
            ++partial;
        }
    CHECK(partial > 0);
    auto invisible = c::Layer::raster("Transparent RGB", solid({ 20, 20 }, { 250, 90, 180, 0 }));
    CHECK(document.insertLayer(1, invisible));
    c::SmartSelectionReference zero(document, invisible.id, c::ColorSampleSource::ActiveLayer);
    for (const auto p : finish(zero)->pixels)
        CHECK(p == c::Rgba8({ 0, 0, 0, 0 }));
}
void batchedMatchesPixelSampling()
{
    for(int scenario=0;scenario<4;++scenario) {
        c::Document doc({{67,49}});
        auto base=c::Layer::raster("Base",solid({67,49},{80,130,170,128}));
        auto top=c::Layer::raster("Top",solid({39,34},{220,70,30,191}));
        top.localToDocument=scenario==1?c::AffineTransform{1.05,.2,5.25,-.1,.9,8.75,.002,-.001,1}
                                      :c::AffineTransform{1,0,7,0,1,4};
        top.opacity=.61F;top.blendMode=c::BlendMode::Dissolve;
        top.crop=c::LayerCrop{1.25,2.75,31,26};top.crop->corners={3,4,2,5};
        auto mask=std::make_shared<c::LayerMask>();
        mask->coverage=c::SelectionMask::rectangle({39,34},{3,1,20,31},170);mask->outside=0;
        top.mask=mask;
        CHECK(doc.insertLayer(0,base));CHECK(doc.insertLayer(1,top));
        if(scenario==2) {
            auto tree=doc.tree();const auto group=c::makeLayerId();tree.roots={group};
            tree.containers={{group,"Clipped",c::ContainerKind::ClippingMaskGroup,c::ColorLabel::None,{base.id,top.id}}};
            CHECK(doc.replaceStructure(doc.tree(),tree));
        }
        if(scenario==3) {
            auto op=c::Layer::adjustment("Invert");auto stack=std::make_shared<c::AdjustmentStack>();
            stack->items[std::size_t(c::AdjustmentType::Invert)].enabled=true;
            op.adjustments=stack;op.opacity=.4F;CHECK(doc.insertLayer(2,op));
        }
        for(auto source:{c::ColorSampleSource::ActiveLayer,c::ColorSampleSource::MergedVisible}) {
            c::PinnedDocumentSampler oracle(doc,top.id,source,c::SampleFiltering::AlphaAware,{},c::ActiveReferenceAppearance::Rendered);
            for(std::size_t budget:{1U,31U,2048U}) {
                c::SmartSelectionReference batched(doc,top.id,source);
                while(!batched.step(budget)){}
                const auto image=batched.image();
                for(int y=0;y<49;++y)for(int x=0;x<67;++x) {
                    const c::Vec2d p{double(x)+.5,double(y)+.5};
                    const bool valid=oracle.validSample(p);
                    auto expected=valid?oracle.sample(p):c::Rgba8{};
                    if(!expected.alpha)expected={0,0,0,0};
                    CHECK(image->valid[std::size_t(y*67+x)]==valid);
                    CHECK(image->pixels[std::size_t(y*67+x)]==expected);
                }
            }
        }
    }
}
void parallelHierarchyMatchesPixelSampling()
{
    for (int scenario = 0; scenario < 3; ++scenario) {
        c::Document doc({{257,193}}); // Crosses row, batch and worker boundaries.
        auto base = c::Layer::raster("Base", solid({211,167}, {80,130,170,128}));
        base.localToDocument = {1,0,13.5,0,1,8.25};
        auto top = c::Layer::raster("Small transformed leaf", solid({19,23}, {220,70,30,191}));
        top.localToDocument = scenario == 0 ? c::AffineTransform{-1.4,.25,135.5,.3,.7,37.75}
            : scenario == 1 ? c::AffineTransform{.1,-1,124.5,1.4,.1,37.75}
                            : c::AffineTransform{1.1,.3,75.5,-.1,.9,43.25,.002,-.001,1};
        top.opacity = .61F;
        auto effects = std::make_shared<c::LayerEffectStack>();
        effects->items[std::size_t(c::LayerEffectType::Stroke)].enabled = true;
        effects->items[std::size_t(c::LayerEffectType::Stroke)].size = 5;
        auto& shadow = effects->items[std::size_t(c::LayerEffectType::DropShadow)];
        shadow.enabled = true; shadow.distance = 17; shadow.size = 4;
        top.effects = effects;
        top = c::prepareSpatialFilterLayer(top);
        CHECK(top.effectCache);
        auto mask = std::make_shared<c::LayerMask>();
        mask->coverage = c::SelectionMask::rectangle({19,23},{0,0,13,23},170);
        mask->outside = 255; top.mask = mask;
        auto adjustment = c::Layer::adjustment("Invert");
        auto stack = std::make_shared<c::AdjustmentStack>();
        stack->items[std::size_t(c::AdjustmentType::Invert)].enabled = true;
        adjustment.adjustments = stack; adjustment.opacity = .3F;
        CHECK(doc.insertLayer(0,base)); CHECK(doc.insertLayer(1,top)); CHECK(doc.insertLayer(2,adjustment));
        auto tree = doc.tree();
        const auto folder = c::makeLayerId(), group = c::makeLayerId();
        tree.roots = {folder};
        tree.containers = {{folder,"Nested",c::ContainerKind::Folder,c::ColorLabel::None,{group,adjustment.id}},
            {group,"Clipped",c::ContainerKind::ClippingMaskGroup,c::ColorLabel::None,{base.id,top.id}}};
        CHECK(doc.replaceStructure(doc.tree(),tree));
        for (auto source : {c::ColorSampleSource::ActiveLayer,c::ColorSampleSource::MergedVisible}) {
            c::PinnedDocumentSampler oracle(doc,top.id,source,c::SampleFiltering::AlphaAware,{},c::ActiveReferenceAppearance::Rendered);
            std::vector<c::Rgba8> pixels;
            std::vector<std::uint8_t> valid;
            for (int y = 0; y < 193; ++y) for (int x = 0; x < 257; ++x) {
                const c::Vec2d p{double(x)+.5,double(y)+.5};
                valid.push_back(oracle.validSample(p));
                auto color = valid.back() ? oracle.sample(p) : c::Rgba8{};
                if (!color.alpha) color = {0,0,0,0};
                pixels.push_back(color);
            }
            for (unsigned workers : {1U,2U,8U,0U}) for (std::size_t budget : {17003U,32768U}) {
                c::SmartSelectionReference batched(doc,top.id,source);
                CHECK(!batched.image());
                while (!batched.step(budget,workers)) CHECK(!batched.image());
                CHECK(batched.image()->pixels == pixels);
                CHECK(batched.image()->valid == valid);
                const auto image = batched.image();
                CHECK(batched.step(budget,workers)); // Finished stepping is an identity.
                CHECK(batched.image() == image && image->pixels == pixels);
            }
        }
    }
}

class CountedSurface final : public c::RasterSurface {
public:
    c::ContiguousRasterSurface storage{{19,23},{120,70,30,191}};
    mutable std::atomic_size_t extentQueries{0};
    c::SurfaceId id() const noexcept override { return storage.id(); }
    c::Extent2u extent() const noexcept override { ++extentQueries; return storage.extent(); }
    c::Revision revision() const noexcept override { return storage.revision(); }
    c::DirtySet dirtySince(c::Revision r) const override { return storage.dirtySince(r); }
    void copyRgba8(c::RectI r,std::span<std::byte> out,std::size_t stride) const override { storage.copyRgba8(r,out,stride); }
    c::DirtySet replaceRgba8Batch(std::span<const c::RasterPatch> p) override { return storage.replaceRgba8Batch(p); }
    c::DirtySet swapRgba8Batch(std::span<c::MutableRasterPatch> p) override { return storage.swapRgba8Batch(p); }
};
void sparseRowsAndBusyPool()
{
    c::Document doc({{512,384}});
    const auto surface = std::make_shared<CountedSurface>();
    auto leaf = c::Layer::raster("Fractional leaf",surface);
    leaf.localToDocument = {1,0,151.5,0,1,105.25};
    auto adjustment = c::Layer::adjustment("Operator");
    CHECK(doc.insertLayer(0,leaf)); CHECK(doc.insertLayer(1,adjustment));
    c::SmartSelectionReference serial(doc,leaf.id,c::ColorSampleSource::MergedVisible);
    surface->extentQueries = 0;
    const auto expected = finish(serial);
    // Count visits rather than assert a machine-dependent elapsed-time limit.
    // A tiny fractional leaf must not be sampled at every document pixel.
    CHECK(surface->extentQueries < 5000);
    c::SmartSelectionReference parallel(doc,leaf.id,c::ColorSampleSource::MergedVisible);
    while (!parallel.step(32768,0)) {}
    CHECK(parallel.image()->pixels == expected->pixels && parallel.image()->valid == expected->valid);

    std::promise<void> occupied, release;
    auto occupiedFuture = occupied.get_future();
    auto releaseFuture = release.get_future().share();
    std::jthread worker([&] {
        CHECK(c::boundedParallel(2,[&](unsigned rank,unsigned) {
            if (!rank) occupied.set_value();
            releaseFuture.wait();
        }));
    });
    occupiedFuture.wait();
    // Always release on failure, so a regression cannot hang the test process.
    auto fallback = std::async(std::launch::async,[&] {
        c::SmartSelectionReference source(doc,leaf.id,c::ColorSampleSource::MergedVisible);
        while (!source.step(32768,0)) {}
        return source.image();
    });
    CHECK(fallback.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    release.set_value(); worker.join();
    const auto result = fallback.get();
    CHECK(result->pixels == expected->pixels && result->valid == expected->valid);

    c::SmartSelectionReference interrupted(doc,leaf.id,c::ColorSampleSource::MergedVisible);
    CHECK(!interrupted.step(32768,0));
    const std::array bytes{std::byte{1},std::byte{2},std::byte{3},std::byte{255}};
    surface->replaceRgba8({0,0,1,1},bytes,4);
    CHECK(!interrupted.matches(doc));
    bool rejected = false;
    try { (void)interrupted.step(32768,0); } catch (const std::runtime_error&) { rejected = true; }
    CHECK(rejected && !interrupted.image());
}
void benchmark(const QStringList& args)
{
    using Clock = std::chrono::steady_clock;
    const auto elapsed
        = [](auto start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); };
    const auto budget = args.contains("--budget") ? args.value(args.indexOf("--budget")+1).toULongLong() : 32768ULL;
    const auto workers = args.contains("--workers") ? args.value(args.indexOf("--workers")+1).toUInt() : 0U;
    for (const auto size : { c::Extent2u { 3840, 2160 }, c::Extent2u { 5120, 2880 } })
        for (bool merged : { false, true }) {
            c::Document document({ size });
            auto layer = c::Layer::raster("Gradient", solid(size, { 170, 80, 45, 255 }));
            CHECK(document.insertLayer(0, layer));
            if (merged) {
                auto overlay = c::Layer::raster("Blended crop", solid(size, { 60, 150, 90, 80 }));
                overlay.blendMode = c::BlendMode::SoftLight;
                overlay.localToDocument = { .95, .05, 1, -.05, .95, 30 };
                overlay.crop = c::LayerCrop { 40, 30, double(size.width) - 80, double(size.height) - 60 };
                overlay.crop->corners = { 90, 90, 90, 90 };
                CHECK(document.insertLayer(1, overlay));
            }
            const auto start = Clock::now();
            c::SmartSelectionReference source(document, layer.id,
                merged ? c::ColorSampleSource::MergedVisible : c::ColorSampleSource::ActiveLayer);
            const auto constructMs = elapsed(start);
            double maxSlice = 0;
            unsigned slices = 0;
            const auto scanStart = Clock::now();
            do {
                const auto slice = Clock::now();
                (void)source.step(budget,workers);
                maxSlice = std::max(maxSlice, elapsed(slice));
                ++slices;
            } while (!source.image());
            std::cout << size.width << 'x' << size.height << (merged ? " merged-two" : " active")
                      << " construct_ms=" << constructMs << " cold_reference_ms=" << elapsed(scanStart)
                      << " workers=" << workers << " budget=" << budget
                      << " max_step_ms=" << maxSlice << " slices=" << slices << " cache_MiB="
                      << double(source.image()->pixels.capacity() * sizeof(c::Rgba8)
                             + source.image()->valid.capacity())
                    / (1024 * 1024)
                      << '\n';
        }
}
void projectBenchmark(const QStringList& args)
{
    using Clock = std::chrono::steady_clock;
    const auto elapsed = [](auto start) {
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    };
    const auto start = Clock::now();
    auto loaded = u::loadProject(args.value(args.indexOf("--project") + 1));
    if (!loaded) { std::cerr << loaded.error.toStdString() << '\n'; ++failures; return; }
    auto& doc = *loaded.document;
    std::cout << "load_ms=" << elapsed(start) << " extent=" << doc.canvas().extent.width << 'x'
              << doc.canvas().extent.height << " layers=" << doc.layers().size() << '\n';
    const auto prepare = Clock::now();
    std::vector<c::SampleCacheOverride> caches;
    for (const auto& layer : doc.layers())
        if (doc.isEffectivelyVisible(layer.id) && layer.opacity > 0
            && (std::holds_alternative<c::TextLayer>(layer.payload) || std::holds_alternative<c::ShapeLayer>(layer.payload)))
            caches.push_back({layer.id, u::prepareDocumentSampleCache(layer, 64 * 1024 * 1024)});
    std::cout << "typed_prepare_ms=" << elapsed(prepare) << '\n';
    const auto setup = Clock::now();
    c::SmartSelectionReference source(doc, {}, c::ColorSampleSource::MergedVisible, caches);
    std::cout << "sampler_setup_ms=" << elapsed(setup) << " sources=" << source.sourcesPerPixel() << '\n';
    const auto budget = args.contains("--budget") ? args.value(args.indexOf("--budget") + 1).toULongLong()
        : std::max(std::size_t(1), std::size_t(2048) / std::max(std::size_t(1), source.sourcesPerPixel()));
    const auto workers = args.contains("--workers") ? args.value(args.indexOf("--workers")+1).toUInt() : 1U;
    const auto scan = Clock::now();
    std::size_t calls = 0;
    double maximumMs = 0;
    do {
        const auto tick = Clock::now();
        (void)source.step(budget, workers);
        maximumMs = std::max(maximumMs, elapsed(tick));
        ++calls;
    } while (!source.image());
    std::cout << "reference_ms=" << elapsed(scan) << " budget=" << budget << " workers=" << workers << " calls=" << calls
              << " max_step_ms=" << maximumMs << '\n';
    const auto image = source.image();
    const auto bytes = QByteArrayView(reinterpret_cast<const char*>(image->pixels.data()),
        qsizetype(image->pixels.size() * sizeof(c::Rgba8)));
    std::cout << "reference_sha256=" << QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex().constData() << '\n';
    const auto seedIndex = args.indexOf("--seed");
    if (seedIndex >= 0) {
        const c::Vec2d seed{args.value(seedIndex+1).toDouble(), args.value(seedIndex+2).toDouble()};
        const std::atomic_bool cancelled{false};
        for (bool quick : {false, true}) {
            c::QuickSelectionPath path;
            c::NormalizedPointerSample sample; sample.documentPosition = seed; sample.pressure = 1;
            sample.buttons = c::PointerButtonPrimary; sample.pointerType = c::PointerType::Mouse;
            path.begin(24, sample); path.end(sample);
            const auto begin = Clock::now();
            const auto result = quick
                ? c::buildQuickSelection(*image, path.dabs(), {}, {}, c::SelectionOperation::Replace, cancelled)
                : c::buildMagicWand(*image, seed, 16, {}, c::SelectionOperation::Replace, cancelled);
            std::cout << (quick ? "quick" : "wand") << "_solve_ms=" << elapsed(begin);
            QCryptographicHash hash(QCryptographicHash::Sha256);
            std::vector<char> row(image->extent.width);
            for (std::uint32_t y = 0; y < image->extent.height; ++y) {
                for (std::uint32_t x = 0; x < image->extent.width; ++x)
                    row[x] = result.combined ? char(result.combined->coverageAtDocumentPixel(int(x), int(y))) : 0;
                hash.addData(QByteArrayView(row.data(), qsizetype(row.size())));
            }
            std::cout << " mask_sha256=" << hash.result().toHex().constData() << '\n';
        }
    }
    if (args.contains("--write-reference")) {
        const QImage view(reinterpret_cast<const uchar*>(image->pixels.data()), int(image->extent.width),
            int(image->extent.height), qsizetype(image->extent.width)*4, QImage::Format_RGBA8888);
        CHECK(view.save(args.value(args.indexOf("--write-reference")+1)));
    }
}
}
int main(int argc, char** argv)
{
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);
    if (app.arguments().contains("--project"))
        projectBenchmark(app.arguments());
    else if (app.arguments().contains("--benchmark"))
        benchmark(app.arguments());
    else {
        typedRenderedReferences();
        cropAndAlpha();
        batchedMatchesPixelSampling();
        parallelHierarchyMatchesPixelSampling();
        sparseRowsAndBusyPool();
    }
    return failures ? 1 : 0;
}
