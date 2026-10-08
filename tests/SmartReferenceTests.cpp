#include "imageeditor/core/SmartSelection.hpp"
#include "imageeditor/ui/FlattenedDocument.hpp"
#include <QApplication>
#include <QImage>
#include <QStandardPaths>
#include <chrono>
#include <iostream>

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
void benchmark()
{
    using Clock = std::chrono::steady_clock;
    const auto elapsed
        = [](auto start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); };
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
                (void)source.step(2048);
                maxSlice = std::max(maxSlice, elapsed(slice));
                ++slices;
            } while (!source.image());
            std::cout << size.width << 'x' << size.height << (merged ? " merged-two" : " active")
                      << " construct_ms=" << constructMs << " cold_reference_ms=" << elapsed(scanStart)
                      << " max_2048px_slice_ms=" << maxSlice << " slices=" << slices << " cache_MiB="
                      << double(source.image()->pixels.capacity() * sizeof(c::Rgba8)
                             + source.image()->valid.capacity())
                    / (1024 * 1024)
                      << '\n';
        }
}
}
int main(int argc, char** argv)
{
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);
    if (app.arguments().contains("--benchmark"))
        benchmark();
    else {
        typedRenderedReferences();
        cropAndAlpha();
        batchedMatchesPixelSampling();
    }
    return failures ? 1 : 0;
}
