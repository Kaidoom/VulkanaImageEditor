#include "imageeditor/core/BlendCompositing.hpp"
#include "imageeditor/core/ColorSampler.hpp"
#include "imageeditor/core/EditorSession.hpp"
#include "imageeditor/core/LayerCommands.hpp"
#include "imageeditor/core/LayerGeometry.hpp"
#include "imageeditor/core/LayerStructureCommands.hpp"
#include "imageeditor/ui/FlattenedDocument.hpp"
#include "imageeditor/ui/LayerListModel.hpp"
#include "imageeditor/ui/ProjectFile.hpp"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QTemporaryDir>
#include <QTest>
#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

namespace c = imageeditor::core;
namespace u = imageeditor::ui;
namespace {
int failures = 0;
void check(bool value, const char* expression, int line)
{
    if (!value) { ++failures; std::cerr << "FAIL " << line << ": " << expression << '\n'; }
}
#define CHECK(...) check(bool(__VA_ARGS__), #__VA_ARGS__, __LINE__)

c::Rgba8 pixel(const QImage& image, int x, int y)
{
    const auto color = image.pixelColor(x, y);
    return {std::uint8_t(color.red()),std::uint8_t(color.green()),
        std::uint8_t(color.blue()),std::uint8_t(color.alpha())};
}
bool near(c::Rgba8 a, c::Rgba8 b, int tolerance = 1)
{
    return std::abs(int(a.red)-int(b.red)) <= tolerance
        && std::abs(int(a.green)-int(b.green)) <= tolerance
        && std::abs(int(a.blue)-int(b.blue)) <= tolerance
        && std::abs(int(a.alpha)-int(b.alpha)) <= tolerance;
}
bool nearImage(const QImage& a, const QImage& b, int tolerance = 1)
{
    if (a.size() != b.size()) return false;
    for (int y=0; y<a.height(); ++y) for (int x=0; x<a.width(); ++x)
        if (!near(pixel(a,x,y),pixel(b,x,y),tolerance)) return false;
    return true;
}
c::Layer raster(const char* name, c::Extent2u extent, c::Rgba8 color,
    c::BlendMode mode = c::BlendMode::Normal, float opacity = 1)
{
    auto layer = c::Layer::raster(name, std::make_shared<c::ContiguousRasterSurface>(extent,color));
    layer.blendMode = mode; layer.opacity = opacity;
    return layer;
}
void prepareCaches(c::Document& document)
{
    for (const auto& layer : document.layers())
        if (layer.visible && !std::holds_alternative<c::RasterLayer>(layer.payload))
            document.layer(layer.id)->renderCache = u::prepareDocumentSampleCache(layer, 1U<<20U);
}

// The oracle here is the owned shared blend/composition contract. Independent
// numeric formula/reference tests live in BlendMathTests; these tests enforce
// agreement among the actual consumers and catch alpha/filter/cache routing.
void allModesConsumerParity()
{
    QTemporaryDir directory;
    for (const auto mode : c::allBlendModes) {
        c::Document document({{7,5}});
        const c::Rgba8 base {43,181,221,143}, source {209,72,119,117};
        CHECK(document.insertLayer(0, raster("Base",{7,5},base)));
        auto top = raster("Top",{7,5},source,mode,.37F);
        const auto id = top.id;
        CHECK(document.insertLayer(1,std::move(top)));
        const auto expected = c::encodeColor(c::compositeLayer(c::decodeColor(base),c::decodeColor(source),.37F,mode));
        c::PinnedDocumentSampler pinned(document,id,c::ColorSampleSource::MergedVisible);
        const auto output = u::flattenDocument(document);
        CHECK(output); if (!output) continue;
        for (int y=0; y<5; ++y) for (int x=0; x<7; ++x) {
            const c::Vec2d p {x+.5,y+.5};
            CHECK(near(c::sampleDocumentColor(document,id,p,c::ColorSampleSource::MergedVisible).color,expected,0));
            CHECK(near(pinned.sample(p),expected,0));
            CHECK(near(pixel(output.image,x,y),expected,0));
        }
        // Active-layer samples must remain the original source, not apply the
        // source mode, its layer opacity, or any underlying layer twice.
        CHECK(c::sampleDocumentColor(document,id,{2.5,2.5},c::ColorSampleSource::ActiveLayer).color == source);
        const auto filename = directory.filePath(QString::fromUtf8(c::blendModeId(mode)) + ".png");
        CHECK(output.image.save(filename,"PNG"));
        CHECK(nearImage(QImage(filename),output.image,0));
        const auto next = mode == c::BlendMode::Multiply ? c::BlendMode::Screen : c::BlendMode::Multiply;
        CHECK(document.setLayerBlendMode(id,next));
        CHECK(!pinned.matches(document));
        // A pinned reference freezes metadata, and callers reject its mismatch.
        CHECK(pinned.sample({2.5,2.5}) == expected);
        c::PinnedDocumentSampler fresh(document,id,c::ColorSampleSource::MergedVisible);
        CHECK(fresh.matches(document));
        CHECK(fresh.sample({2.5,2.5}) == c::sampleDocumentColor(document,id,{2.5,2.5},c::ColorSampleSource::MergedVisible).color);
    }
}

void alphaAwareTransformedRasterEdges()
{
    // A bright hidden texel must not tint a visible edge during filtering,
    // regardless of blend mode or partially transparent backdrop/source.
    for (const auto mode : c::allBlendModes) {
        QImage cleanImage;
        for (const bool contaminated : {false,true}) {
            c::Document doc({{8,4}});
            CHECK(doc.insertLayer(0,raster("Backdrop",{8,4},{77,153,209,129})));
            std::vector<std::byte> bytes {std::byte {239},std::byte {51},std::byte {23},std::byte {173},
                std::byte {0},std::byte {0},std::byte {0},std::byte {0}};
            if (contaminated) { bytes[4]=std::byte {0}; bytes[5]=std::byte {255}; bytes[6]=std::byte {255}; }
            auto edge = c::Layer::raster("Filtered alpha edge",std::make_shared<c::ContiguousRasterSurface>(
                c::Extent2u {2,1},std::move(bytes)));
            edge.blendMode = mode; edge.opacity = .61F; edge.localToDocument = {4,0,0,0,4,0};
            const auto id = edge.id;
            CHECK(doc.insertLayer(1,std::move(edge)));
            const auto flat = u::flattenDocument(doc);
            CHECK(flat); if (!flat) continue;
            c::PinnedDocumentSampler pinned(doc,{},c::ColorSampleSource::MergedVisible);
            for (int x=0;x<8;++x) {
                CHECK(pinned.sample({x+.5,1.5}) == pixel(flat.image,x,1));
                CHECK(c::sampleDocumentColor(doc,id,{x+.5,1.5},c::ColorSampleSource::MergedVisible).color
                    == pixel(flat.image,x,1));
            }
            if (!contaminated) cleanImage=flat.image;
            else CHECK(nearImage(cleanImage,flat.image,0));
        }
    }
}

std::unique_ptr<c::Document> mixedDocument(const std::string& family,
    c::BlendMode mode = c::BlendMode::Normal)
{
    auto doc = std::make_unique<c::Document>(c::CanvasSpec {{64,48},300});
    auto background = raster("Backdrop beyond canvas",{76,52},{51,148,211,163});
    background.localToDocument.m02 = -4; background.localToDocument.m12 = -2;
    CHECK(doc->insertLayer(0,std::move(background)));
    auto patch = raster("Rotated raster",{24,22},{228,94,37,121},c::BlendMode::Overlay,.63F);
    patch.localToDocument = {1.1,.25,8.3,-.18,.8,9.1};
    CHECK(doc->insertLayer(1,std::move(patch)));
    c::ShapeLayer shape;
    shape.kind = c::ShapeKind::Ellipse; shape.size = {26.5,19.25};
    shape.fillColor = {34,223,133,103}; shape.strokeEnabled = true;
    shape.strokeColor = {185,30,196,179}; shape.strokeWidth = 2.75;
    auto ellipse = c::Layer::shape("Flipped stroked ellipse",shape);
    ellipse.localToDocument = {-.9,.3,47,.15,1.2,6};
    ellipse.blendMode = c::BlendMode::SoftLight; ellipse.opacity = .82F;
    CHECK(doc->insertLayer(2,std::move(ellipse)));
    c::TextLayer text;
    text.utf8 = "Oa\nBlend";
    text.defaultStyle.font = {family,"Regular",400,false};
    text.defaultStyle.sizePixels = 17.25;
    text.defaultStyle.color = {225,173,49,137};
    auto letters = c::Layer::text("Semi-transparent transformed text",text);
    letters.localToDocument = {.9,-.12,11,.13,.8,6};
    letters.blendMode = c::BlendMode::Color; letters.opacity = .58F;
    CHECK(doc->insertLayer(3,std::move(letters)));
    auto hidden = raster("Hidden non-Normal",{8,8},{255,0,255,255},c::BlendMode::Difference);
    hidden.visible = false;
    CHECK(doc->insertLayer(4,std::move(hidden)));
    if (mode != c::BlendMode::Normal)
        for (std::size_t i=1;i<4;++i) CHECK(doc->setLayerBlendMode(doc->layers()[i].id,mode));
    prepareCaches(*doc);
    return doc;
}

c::LayerId wrapAll(c::Document& doc, bool withFolder)
{
    auto tree = doc.tree();
    const auto groupId = c::makeLayerId();
    c::LayerContainer group {groupId,"Mixed pass-through group",c::ContainerKind::Group};
    group.children = tree.roots;
    tree.roots = {groupId}; tree.containers.push_back(std::move(group));
    if (withFolder) {
        const auto folderId = c::makeLayerId();
        c::LayerContainer folder {folderId,"Pass-through folder",c::ContainerKind::Folder};
        folder.children = {groupId}; tree.roots = {folderId}; tree.containers.push_back(std::move(folder));
    }
    CHECK(doc.replaceStructure(doc.tree(),std::move(tree)));
    return groupId;
}

void typedTransformsHierarchyAndSelection(const std::string& family,
    c::BlendMode mode = c::BlendMode::Normal)
{
    auto doc = mixedDocument(family,mode);
    const auto image = u::flattenDocument(*doc);
    CHECK(image); if (!image) return;
    c::PinnedDocumentSampler pinned(*doc,{},c::ColorSampleSource::MergedVisible);
    for (int y=0; y<48; ++y) for (int x=0; x<64; ++x) {
        const c::Vec2d p {x+.5,y+.5};
        CHECK(near(pixel(image.image,x,y),pinned.sample(p),0));
        CHECK(near(pixel(image.image,x,y),c::sampleDocumentColor(*doc,{},p,c::ColorSampleSource::MergedVisible).color,0));
    }
    const auto group = wrapAll(*doc,true);
    CHECK(nearImage(image.image,u::flattenDocument(*doc).image,0));
    CHECK(!pinned.matches(*doc));
    // The raster selection is an editing mask, never a content compositor gate.
    CHECK(doc->setSelection(c::SelectionMask::filled(doc->canvas().extent,0)));
    CHECK(nearImage(image.image,u::flattenDocument(*doc).image,0));
    auto tree = doc->tree();
    CHECK(tree.dissolve(group));
    CHECK(doc->replaceStructure(doc->tree(),std::move(tree)));
    CHECK(nearImage(image.image,u::flattenDocument(*doc).image,0));
    const auto folderId = doc->tree().roots.front();
    CHECK(doc->setLayerVisibility(folderId,false));
    const auto hidden = u::flattenDocument(*doc);
    CHECK(hidden);
    if (hidden) CHECK(pixel(hidden.image,20,20).alpha == 0);
    CHECK(doc->setLayerVisibility(folderId,true));
    CHECK(nearImage(image.image,u::flattenDocument(*doc).image,0));

    QTemporaryDir directory;
    const auto filename = directory.filePath("mixed-blends.vulkana");
    CHECK(u::saveProject(filename,*doc));
    auto reopened = u::loadProject(filename);
    CHECK(reopened);
    if (reopened) CHECK(nearImage(image.image,u::flattenDocument(*reopened.document).image,0));
}

void mergeBakingAndHistory(const std::string& family, c::BlendMode mode = c::BlendMode::Normal)
{
    c::EditorSession session;
    session.replaceDocument(mixedDocument(family,mode));
    auto& doc = *session.document();
    const auto group = wrapAll(doc,false);
    session.setActiveLayer(group);
    CHECK(doc.setSelection(c::SelectionMask::filled(doc.canvas().extent,0)));
    const auto originalTree = doc.tree();
    const auto originalSelection = session.layerSelectionState();
    const auto originalLeaves = doc.expandedLayers(std::array {group});
    const auto originalImage = u::flattenDocument(doc);
    const auto merge = u::flattenLayerItems(doc,std::array {group});
    CHECK(merge); if (!merge) { std::cerr << merge.error.toStdString() << '\n'; return; }
    CHECK(merge.origin.x < 0 && merge.origin.y < 0);
    CHECK(merge.image.width() > int(doc.canvas().extent.width));
    const auto width = merge.image.width(), height = merge.image.height();
    std::vector<std::byte> bytes(std::size_t(width)*std::size_t(height)*4);
    for (int y=0;y<height;++y)
        std::memcpy(bytes.data()+std::size_t(y)*std::size_t(width)*4,merge.image.constScanLine(y),std::size_t(width)*4);
    auto merged = c::Layer::raster("Merged",std::make_shared<c::ContiguousRasterSurface>(
        c::Extent2u {std::uint32_t(width),std::uint32_t(height)},std::move(bytes)));
    merged.localToDocument.m02 = merge.origin.x; merged.localToDocument.m12 = merge.origin.y;
    const auto mergedId = merged.id;
    CHECK(merged.blendMode == c::BlendMode::Normal && merged.opacity == 1);
    c::LayerTree tree; tree.roots = {mergedId};
    const c::LayerSelectionState selected {{mergedId},mergedId,mergedId};
    CHECK(session.execute(std::make_unique<c::LayerStructureCommand>("Merge layers",doc,std::move(tree),
        originalLeaves,std::vector<c::Layer> {merged},originalSelection,selected)));
    CHECK(doc.layers().size() == 1 && session.activeLayer() == mergedId);
    CHECK(nearImage(originalImage.image,u::flattenDocument(doc).image,1));
    CHECK(session.undo());
    CHECK(doc.tree() == originalTree);
    CHECK(session.layerSelectionState() == originalSelection);
    CHECK(std::holds_alternative<c::TextLayer>(doc.layers()[3].payload));
    CHECK(doc.layers()[3].blendMode == (mode == c::BlendMode::Normal ? c::BlendMode::Color : mode));
    CHECK(nearImage(originalImage.image,u::flattenDocument(doc).image,0));
    CHECK(session.redo());
    CHECK(nearImage(originalImage.image,u::flattenDocument(doc).image,1));
}

void isolatedMergeIgnoresExternalBackdrop()
{
    c::Document doc({{16,16}});
    auto base = raster("External backdrop",{16,16},{60,185,240,200});
    const auto baseId = base.id;
    CHECK(doc.insertLayer(0,std::move(base)));
    auto middle = raster("Middle",{16,16},{210,84,40,150}); const auto middleId = middle.id;
    CHECK(doc.insertLayer(1,std::move(middle)));
    auto top = raster("Top",{16,16},{200,160,50,90},c::BlendMode::Multiply); const auto topId = top.id;
    CHECK(doc.insertLayer(2,std::move(top)));
    const std::array partial {middleId,topId};
    // Every implemented blend mode is evaluated in isolation over transparency.
    // The unselected external backdrop neither contaminates nor vetoes output.
    for (const auto mode:c::allBlendModes) {
        if (mode == c::BlendMode::Normal) continue;
        (void)doc.setLayerBlendMode(topId,mode);
        const auto revision = doc.revision();
        const auto image = u::flattenDocument(doc);
        const auto isolated = u::flattenLayerItems(doc,partial);
        CHECK(isolated);
        CHECK(doc.revision() == revision && nearImage(image.image,u::flattenDocument(doc).image,0));
    }
    CHECK(doc.setLayerBlendMode(topId,c::BlendMode::Normal));
    CHECK(u::flattenLayerItems(doc,partial)); // Source-over partial merge is associative.
    CHECK(doc.setLayerBlendMode(topId,c::BlendMode::Multiply));
    CHECK(doc.setLayerVisibility(baseId,false));
    CHECK(u::flattenLayerItems(doc,partial)); // No visible external backdrop remains.
    CHECK(doc.setLayerVisibility(baseId,true));
    auto far = doc.layer(baseId)->localToDocument; far.m02 = 10000;
    CHECK(doc.setLayerTransform(baseId,far));
    CHECK(u::flattenLayerItems(doc,partial)); // No external alpha reaches the blended pixels.
    CHECK(u::flattenLayerItems(doc,std::array {baseId,middleId,topId}));
}

void groupThumbnailBlendAndBackdropInvalidation()
{
    c::EditorSession session;
    auto document = std::make_unique<c::Document>(c::CanvasSpec {{32,32}});
    auto base = raster("Backdrop",{32,32},{90,145,210,255}); const auto baseId = base.id;
    auto top = raster("Child",{32,32},{190,125,70,255},c::BlendMode::Multiply); const auto topId = top.id;
    CHECK(document->insertLayer(0,std::move(base)));
    CHECK(document->insertLayer(1,std::move(top)));
    auto tree = document->tree(); const auto groupId = c::makeLayerId();
    c::LayerContainer group {groupId,"Pass-through preview",c::ContainerKind::Group};
    group.children = {topId}; tree.roots = {baseId,groupId}; tree.containers.push_back(std::move(group));
    CHECK(document->replaceStructure(document->tree(),std::move(tree)));
    session.replaceDocument(std::move(document));
    auto& doc = *session.document();
    u::LayerListModel model; model.setSession(&session);
    const auto icon = [&] { return model.data(model.index(model.rowForLayer(groupId)),Qt::DecorationRole).value<QIcon>(); };
    const auto first = icon();
    CHECK(!first.isNull());
    CHECK(icon().cacheKey() == first.cacheKey()); // Unchanged reads reuse the cache.
    CHECK(near(pixel(first.pixmap(120,120).toImage(),60,60),
        c::sampleDocumentColor(doc,{}, {16.5,16.5},c::ColorSampleSource::MergedVisible).color,1));
    CHECK(doc.setLayerBlendMode(topId,c::BlendMode::Screen));
    QTest::qWait(180); model.refresh();
    const auto second = icon();
    CHECK(second.cacheKey() != first.cacheKey());
    CHECK(near(pixel(second.pixmap(120,120).toImage(),60,60),
        c::sampleDocumentColor(doc,{}, {16.5,16.5},c::ColorSampleSource::MergedVisible).color,1));
    const auto surface = std::get<c::RasterLayer>(doc.layer(baseId)->payload).surface;
    const std::array<std::byte,4> rgba {std::byte {240},std::byte {35},std::byte {80},std::byte {255}};
    std::vector<std::byte> patch(32*32*4);
    for (std::size_t i=0;i<patch.size();i+=4) std::copy(rgba.begin(),rgba.end(),patch.begin()+std::ptrdiff_t(i));
    (void)surface->replaceRgba8({0,0,32,32},patch,32*4);
    QTest::qWait(180); model.refresh();
    const auto third = icon();
    CHECK(third.cacheKey() != second.cacheKey());
    CHECK(near(pixel(third.pixmap(120,120).toImage(),60,60),
        c::sampleDocumentColor(doc,{}, {16.5,16.5},c::ColorSampleSource::MergedVisible).color,1));
}
}

int main(int argc,char** argv)
{
    QApplication app(argc,argv);
    try {
        const auto path = QFileInfo(QString::fromUtf8(__FILE__)).dir().filePath("assets/fonts/NotoSans-Regular.ttf");
        const auto font = QFontDatabase::addApplicationFont(path);
        CHECK(font >= 0);
        if (font < 0) return 1;
        const auto family = QFontDatabase::applicationFontFamilies(font).front().toStdString();
        allModesConsumerParity();
        alphaAwareTransformedRasterEdges();
        typedTransformsHierarchyAndSelection(family);
        mergeBakingAndHistory(family);
        for (const auto mode:{c::BlendMode::ColorDodge,c::BlendMode::LinearDodge,
                 c::BlendMode::ColorBurn,c::BlendMode::LinearBurn,c::BlendMode::Subtract,c::BlendMode::Divide}) {
            typedTransformsHierarchyAndSelection(family,mode);
            mergeBakingAndHistory(family,mode);
        }
    isolatedMergeIgnoresExternalBackdrop();
        groupThumbnailBlendAndBackdropInvalidation();
    } catch (const std::exception& error) {
        ++failures; std::cerr << error.what() << '\n';
    }
    std::cout << (failures ? "Blend integration tests FAILED\n" : "Blend integration tests passed\n");
    return failures ? 1 : 0;
}
