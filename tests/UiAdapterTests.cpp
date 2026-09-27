#include "imageeditor/core/EditorSession.hpp"
#include "imageeditor/core/Layer.hpp"
#include "imageeditor/core/LayerCommands.hpp"
#include "imageeditor/core/RasterSurface.hpp"
#include "imageeditor/ui/LayerListModel.hpp"
#include "imageeditor/ui/QtRasterImageLoader.hpp"
#include "imageeditor/ui/RasterLimits.hpp"

#include <QCoreApplication>
#include <QImage>
#include <QImageWriter>
#include <QMimeData>
#include <QTemporaryDir>

#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <memory>
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

using namespace imageeditor;

void codecsPreserveDimensionsAndPngAlpha(const QString& directory)
{
    QImage source(8, 6, QImage::Format_RGBA8888);
    source.fill(Qt::transparent);
    source.setPixelColor(3, 2, QColor(21, 87, 204, 73));

    const QStringList formats {QStringLiteral("png"), QStringLiteral("PNG"),
        QStringLiteral("jpg"), QStringLiteral("webp")};
    for (const auto& format : formats) {
        const auto path = directory + QStringLiteral("/sample.") + format;
        QImage writeImage = source;
        if (format.compare(QStringLiteral("jpg"), Qt::CaseInsensitive) == 0) {
            writeImage = source.convertToFormat(QImage::Format_RGB888);
        }
        CHECK(writeImage.save(path));
        auto loadedLayer = ui::loadRasterLayer(path);
        CHECK(loadedLayer);
        if (loadedLayer) {
            CHECK(loadedLayer.extent == core::Extent2u({8, 6}));
            CHECK(loadedLayer.layer->name == "sample");
            const auto& payload = std::get<core::RasterLayer>(loadedLayer.layer->payload);
            CHECK(payload.surface && payload.surface->extent() == core::Extent2u({8, 6}));
        }

        auto loaded = ui::loadRasterDocument(path);
        CHECK(loaded);
        if (!loaded) {
            continue;
        }
        CHECK(loaded.document->canvas().extent == core::Extent2u({8, 6}));
        CHECK(loaded.document->layers().size() == 1);
        if (format.compare(QStringLiteral("png"), Qt::CaseInsensitive) == 0) {
            const auto& payload = std::get<core::RasterLayer>(loaded.document->layers().front().payload);
            std::vector<std::byte> pixel(4);
            payload.surface->copyRgba8({3, 2, 1, 1}, pixel, 4);
            CHECK(std::to_integer<int>(pixel[0]) == 21);
            CHECK(std::to_integer<int>(pixel[1]) == 87);
            CHECK(std::to_integer<int>(pixel[2]) == 204);
            CHECK(std::to_integer<int>(pixel[3]) == 73);
        }
    }
}

void layerModelReversesCompositorOrder()
{
    core::EditorSession session;
    auto document = std::make_unique<core::Document>(core::CanvasSpec {.extent = {4, 4}});
    auto bottomSurface = std::make_shared<core::ContiguousRasterSurface>(core::Extent2u {4, 4});
    auto topSurface = std::make_shared<core::ContiguousRasterSurface>(core::Extent2u {4, 4});
    const auto bottom = core::Layer::raster("Bottom", bottomSurface);
    const auto top = core::Layer::raster("Top", topSurface);
    const auto bottomId = bottom.id;
    const auto topId = top.id;
    CHECK(document->insertLayer(0, bottom));
    CHECK(document->insertLayer(1, top));
    session.replaceDocument(std::move(document));

    ui::LayerListModel model;
    model.setSession(&session);
    CHECK(model.rowCount() == 2);
    CHECK(model.layerIdAt(0) == topId);
    CHECK(model.layerIdAt(1) == bottomId);

    int resetCount = 0;
    QObject::connect(&model, &QAbstractItemModel::modelReset, [&resetCount] { ++resetCount; });
    model.onVisibilityChanged = [&session](core::LayerId id, bool visible) {
        return session.execute(std::make_unique<core::SetLayerVisibilityCommand>(id, visible));
    };
    const auto topIndex = model.index(0);
    CHECK(model.setData(topIndex, Qt::Unchecked, Qt::CheckStateRole));
    CHECK(!session.document()->layer(topId)->visible);
    CHECK(model.layerIdAt(0) == topId);
    CHECK(resetCount == 0);
}

void layerModelMovesRowsInVisualTopToBottomOrder()
{
    core::EditorSession session;
    auto document = std::make_unique<core::Document>(core::CanvasSpec {.extent = {4, 4}});
    const auto bottom = core::Layer::raster("Bottom",
        std::make_shared<core::ContiguousRasterSurface>(core::Extent2u {4, 4}));
    const auto middle = core::Layer::raster("Middle",
        std::make_shared<core::ContiguousRasterSurface>(core::Extent2u {4, 4}));
    const auto top = core::Layer::raster("Top",
        std::make_shared<core::ContiguousRasterSurface>(core::Extent2u {4, 4}));
    CHECK(document->insertLayer(0, bottom));
    CHECK(document->insertLayer(1, middle));
    CHECK(document->insertLayer(2, top));
    session.replaceDocument(std::move(document));
    session.setActiveLayer(top.id);

    ui::LayerListModel model;
    model.setSession(&session);
    model.onMoveRequested = [&session](core::LayerId id, std::size_t destination) {
        return session.execute(
            std::make_unique<core::MoveLayerCommand>(id, destination));
    };

    std::unique_ptr<QMimeData> topDrag(model.mimeData({model.index(0)}));
    CHECK(topDrag != nullptr);
    CHECK(model.canDropMimeData(topDrag.get(), Qt::MoveAction,
        model.rowCount(), 0, {}));
    CHECK(model.dropMimeData(topDrag.get(), Qt::MoveAction,
        model.rowCount(), 0, {}));
    CHECK(model.layerIdAt(0) == middle.id);
    CHECK(model.layerIdAt(1) == bottom.id);
    CHECK(model.layerIdAt(2) == top.id);
    CHECK(session.activeLayer() == top.id);

    std::unique_ptr<QMimeData> bottomDrag(model.mimeData({model.index(2)}));
    CHECK(model.dropMimeData(bottomDrag.get(), Qt::MoveAction, 0, 0, {}));
    CHECK(model.layerIdAt(0) == top.id);
    CHECK(model.layerIdAt(1) == middle.id);
    CHECK(model.layerIdAt(2) == bottom.id);
}

void rasterLimitsBoundMemoryWithoutRejectingWideImages()
{
    CHECK(ui::rasterExtentWithinLimits(16384, 2048));
    CHECK(ui::rasterExtentWithinLimits(7680, 4320));
    CHECK(!ui::rasterExtentWithinLimits(8192, 8192));
    CHECK(!ui::rasterExtentWithinLimits(8193, 8193));
    CHECK(!ui::rasterExtentWithinLimits(16385, 1));
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    QTemporaryDir directory;
    CHECK(directory.isValid());
    if (directory.isValid()) {
        codecsPreserveDimensionsAndPngAlpha(directory.path());
    }
    layerModelReversesCompositorOrder();
    layerModelMovesRowsInVisualTopToBottomOrder();
    rasterLimitsBoundMemoryWithoutRejectingWideImages();

    if (failures != 0) {
        std::cerr << failures << " UI adapter assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All UI adapter tests passed\n";
    return EXIT_SUCCESS;
}
