#include "imageeditor/core/LayerGeometry.hpp"
#include "imageeditor/core/RichText.hpp"
#include "imageeditor/core/SelectedPixelTransform.hpp"
#include "imageeditor/ui/FlattenedDocument.hpp"
#include "imageeditor/ui/NativeRasterBake.hpp"
#include "imageeditor/ui/PixelPreview.hpp"
#include "imageeditor/ui/ProjectFile.hpp"
#include "imageeditor/ui/QtShapeRenderService.hpp"
#include "imageeditor/ui/QtTextLayoutService.hpp"
#include <QEventLoop>
#include <QGuiApplication>
#include <QTemporaryDir>
#include <QTimer>
#include <iostream>
namespace c = imageeditor::core;
namespace u = imageeditor::ui;
int failures = 0;
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        if (!(x)) {                                                                                          \
            std::cerr << __LINE__ << ": " #x "\n";                                                           \
            ++failures;                                                                                      \
        }                                                                                                    \
    } while (false)
int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    QTemporaryDir directory;
    c::Document doc({{180, 140}, 96});
    c::History history;
    auto layer = c::Layer::raster("Source", std::make_shared<c::ContiguousRasterSurface>(
                                                c::Extent2u{64, 48}, c::Rgba8{77, 133, 201, 155}));
    const auto id = layer.id;
    const auto source = std::get<c::RasterLayer>(layer.payload).surface;
    layer.localToDocument = *c::rectangleToQuad({0, 0, 64, 48}, {{{5, 4}, {82, 10}, {73, 73}, {-2, 57}}});
    auto adjustments = std::make_shared<c::AdjustmentStack>();
    adjustments->items[0].enabled = true;
    adjustments->items[0].parameters = c::ExposureParameters{.7};
    adjustments->items[0].mask = c::AdjustmentMask{
        c::SelectionMask::rectangle({180, 140}, {8, 8, 40, 30}, 191), layer.localToDocument};
    layer.adjustments = adjustments;
    auto filters = std::make_shared<c::SpatialFilterStack>();
    filters->items[0].enabled = true;
    filters->items[0].parameters = c::GaussianBlurParameters{1.5, .7};
    filters->items[0].mask = adjustments->items[0].mask;
    layer.filters = filters;
    auto effects = std::make_shared<c::LayerEffectStack>();
    effects->items[6].enabled = true;
    effects->items[6].opacity = .35;
    layer.effects = effects;
    layer.crop = c::LayerCrop{{-20, -20, 100, 90}};
    CHECK(doc.insertLayer(0, layer));
    doc.setSelection(c::SelectionMask::rectangle({180, 140}, {10, 15, 32, 26}, 123));
    {
        c::SelectedPixelTransformSession edit(doc, id);
        CHECK(edit.preview({1, 0, -23, 0, 1, -4.25}));
        CHECK(edit.completeAction());
        CHECK(edit.commit(history) == c::TransformCommitResult::Committed);
    }
    CHECK(doc.layer(id)->rasterOrigin.x < 0);
    CHECK(doc.layer(id)->adjustments == adjustments);
    CHECK(doc.layer(id)->filters == filters);
    CHECK(doc.layer(id)->effects == effects);
    CHECK(doc.layer(id)->crop == layer.crop);
    CHECK(doc.layer(id)->localToDocument == layer.localToDocument);
    const auto after = u::flattenDocument(doc);
    CHECK(after);
    if (!after)
        std::cerr << after.error.toStdString() << '\n';
    const auto path = directory.filePath("styled-projective.vulkana");
    const auto saved = u::saveProject(path, doc);
    CHECK(saved);
    if (!saved)
        std::cerr << saved.error.toStdString() << '\n';
    auto loaded = u::loadProject(path);
    CHECK(loaded);
    if (loaded) {
        const auto reopened = u::flattenDocument(*loaded.document);
        CHECK(reopened);
        CHECK(reopened.image == after.image);
        const auto *r = loaded.document->layer(id);
        CHECK(r->rasterOrigin == doc.layer(id)->rasterOrigin);
        CHECK(r->rasterEffectFrame == doc.layer(id)->rasterEffectFrame);
        CHECK(r->localToDocument == layer.localToDocument);
        CHECK(r->adjustments->items[0].mask->localToMask == layer.localToDocument);
        CHECK(r->filters->items[0].mask->localToMask == layer.localToDocument);
        const auto png = directory.filePath("output.png");
        CHECK(reopened.image.save(png));
        CHECK(QImage(png).convertToFormat(QImage::Format_RGBA8888) == after.image);
        u::PixelPreview preview;
        std::shared_ptr<const c::RasterSurface> native;
        QString previewError;
        QEventLoop loop;
        preview.onReady = [&](auto output, QString error) {
            native = std::move(output);
            previewError = error;
            if (native || !error.isEmpty())
                loop.quit();
        };
        preview.setDocumentInstance(991);
        preview.setEnabled(true);
        preview.request(loaded.document->snapshot());
        QTimer::singleShot(5000, &loop, &QEventLoop::quit);
        loop.exec();
        CHECK(native && previewError.isEmpty());
        if (native) {
            QImage output(int(native->extent().width), int(native->extent().height), QImage::Format_RGBA8888);
            native->copyRgba8(
                {0, 0, output.width(), output.height()},
                std::span(reinterpret_cast<std::byte *>(output.bits()), std::size_t(output.sizeInBytes())),
                std::size_t(output.bytesPerLine()));
            CHECK(output == after.image);
        }
        auto bake = u::prepareRasterizeLayers(*loaded.document, c::LayerSelectionState{{id}, id, id},
                                              512ULL * 1024 * 1024);
        CHECK(bake.command && bake.error.isEmpty());
        if (bake.command) {
            c::History baking;
            CHECK(baking.execute(*loaded.document, std::move(bake.command)));
            CHECK(!u::needsRasterization(*loaded.document->layer(id)));
            CHECK(u::flattenDocument(*loaded.document).image == after.image);
            CHECK(baking.undo(*loaded.document));
            CHECK(loaded.document->layer(id)->localToDocument == layer.localToDocument);
        }
    }
    CHECK(history.undo(doc));
    CHECK(std::get<c::RasterLayer>(doc.layer(id)->payload).surface == source);
    CHECK(history.redo(doc));
    CHECK(u::flattenDocument(doc).image == after.image);
    // Perspective is retained on editable typed models; it is not a preview bake.
    c::ShapeLayer shape;
    shape.size = {30, 23};
    shape.kind = c::ShapeKind::Triangle;
    shape.fillColor = {220, 90, 12, 255};
    auto triangle = c::Layer::shape("Triangle", shape);
    triangle.renderCache = u::QtShapeRenderService{}.render({shape, 1});
    triangle.localToDocument = {1, 0, 80, 0, 1, 10};
    const auto shapeId = triangle.id;
    CHECK(doc.insertLayer(1, triangle));
    c::TextLayer text;
    text.utf8 = "ABO";
    text.defaultStyle.font.family = "Sans Serif";
    text.defaultStyle.sizePixels = 20;
    text = c::normalizedText(text);
    auto type = c::Layer::text("Text", text);
    type.renderCache = u::QtTextLayoutService{}.layout({text, 1}).cache;
    type.localToDocument = {1, 0, 60, 0, 1, 80};
    const auto textId = type.id;
    CHECK(doc.insertLayer(2, type));
    const std::array ids{shapeId, textId};
    c::LayerTransformSession transform(doc, ids);
    CHECK(transform.active());
    const auto before = transform.transform();
    const auto corner = before.map({0, 0});
    CHECK(transform.beginDrag(c::TransformHandle::TopLeft, corner));
    CHECK(transform.dragTo(corner + c::Vec2d{4, 5}, {.control = true}, true));
    transform.endDrag();
    CHECK(transform.commit(history) == c::TransformCommitResult::Committed);
    CHECK(!doc.layer(shapeId)->localToDocument.isAffine());
    CHECK(std::get<c::ShapeLayer>(doc.layer(shapeId)->payload) == shape);
    CHECK(std::get<c::TextLayer>(doc.layer(textId)->payload) == text);
    CHECK(u::saveProject(path, doc));
    loaded = u::loadProject(path);
    CHECK(loaded);
    if (loaded) {
        CHECK(std::get<c::ShapeLayer>(loaded.document->layer(shapeId)->payload) == shape);
        CHECK(std::get<c::TextLayer>(loaded.document->layer(textId)->payload) == text);
    }
    auto changed = shape;
    changed.fillColor = {20, 140, 90, 255};
    CHECK(doc.setLayerShape(shapeId, changed));
    text.utf8 += "!";
    CHECK(doc.setLayerText(textId, text));
    const auto typed = u::flattenDocument(doc);
    CHECK(typed);
    if (!typed)
        std::cerr << typed.error.toStdString() << '\n';
    std::cout << "Projective persistence/export: " << failures << " failures\n";
    return failures ? 1 : 0;
}
