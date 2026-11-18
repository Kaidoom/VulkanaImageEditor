#include "imageeditor/core/LayerCommands.hpp"
#include "imageeditor/core/LayerMaskEdit.hpp"
#include "imageeditor/core/LayerStructureCommands.hpp"
#include "imageeditor/core/ProjectLayer.hpp"
#include "imageeditor/core/ProjectPaint.hpp"
#include "imageeditor/core/SelectionCommands.hpp"
#include "imageeditor/core/LayerTransform.hpp"
#include "imageeditor/ui/ImageExport.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/NativeRasterBake.hpp"
#include "imageeditor/ui/LayerListView.hpp"
#include "imageeditor/ui/LayerListModel.hpp"
#include "imageeditor/ui/LayerTransferMimeData.hpp"
#include "NativeGraphicsStartup.hpp"
#include "imageeditor/ui/PsdImport.hpp"
#include "imageeditor/ui/ProjectFile.hpp"

#include <QApplication>
#include <QFile>
#include <QFileDialog>
#include <QSettings>
#include <QTimer>
#include <QMenu>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QTabBar>
#include <QVulkanInstance>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <array>
#include <iostream>
#include <stdexcept>

namespace c = imageeditor::core;
namespace u = imageeditor::ui;
namespace {

#define REQUIRE(...) do { if (!(__VA_ARGS__)) throw std::runtime_error( \
    std::string("Line ") + std::to_string(__LINE__) + ": " + #__VA_ARGS__); } while (false)

constexpr c::Extent2u sourceSize{545, 9};
constexpr c::Rgba8 baseColor{200, 80, 20, 128};
constexpr c::Rgba8 detailColor{15, 210, 90, 255};
constexpr c::Rgba8 alternateColor{80, 40, 150, 255};

c::LayerId saveSource(const QString& path, c::Rgba8 base)
{
    c::Document document({sourceSize, 96});
    auto background = c::Layer::raster("Base", std::make_shared<c::ContiguousRasterSurface>(sourceSize, base));
    auto detail = c::Layer::raster("One pixel", std::make_shared<c::ContiguousRasterSurface>(
        c::Extent2u{1, 1}, detailColor));
    detail.localToDocument.m02 = sourceSize.width - 1;
    detail.localToDocument.m12 = sourceSize.height - 1;
    const auto id = detail.id;
    REQUIRE(document.insertLayer(0, std::move(background)));
    REQUIRE(document.insertLayer(1, std::move(detail)));
    REQUIRE(u::saveProject(path, document));
    return id;
}

c::EditorSession& session(u::MainWindow& window)
{
    return const_cast<c::EditorSession&>(window.editorSession());
}

const c::Layer& layer(const u::MainWindow& window, c::LayerId id)
{
    const auto* result = window.editorSession().document()->layer(id);
    REQUIRE(result);
    return *result;
}

std::shared_ptr<c::RasterSurface> surface(const u::MainWindow& window, c::LayerId id)
{
    return std::get<c::RasterLayer>(layer(window, id).payload).surface;
}

c::Rgba8 pixel(const u::MainWindow& window, c::LayerId id, int x, int y)
{
    std::array<c::Rgba8, 1> colors{};
    surface(window, id)->copyRgba8({x, y, 1, 1}, std::as_writable_bytes(std::span(colors)), 4);
    return colors.front();
}

c::Rgba8 detail(const u::MainWindow& window, c::LayerId id)
{
    return pixel(window, id, int(sourceSize.width) - 1, int(sourceSize.height) - 1);
}

QImage exportedPng(const u::MainWindow& window)
{
    const auto& doc = *window.editorSession().document();
    u::ExportSettings settings;
    settings.format = u::ExportFormat::Png;
    settings.size = QSize(int(doc.canvas().extent.width), int(doc.canvas().extent.height));
    settings.pngEffort = 1;
    auto rendered = u::renderExport(doc, settings);
    REQUIRE(rendered);
    std::atomic_bool cancelled{false};
    auto encoded = u::encodeExport(rendered.image, settings, cancelled);
    REQUIRE(encoded);
    const auto image = QImage::fromData(encoded.bytes, "PNG").convertToFormat(QImage::Format_RGBA8888);
    REQUIRE(!image.isNull());
    return image;
}

void sparseOverlayKeepsSnapshotsAndTransforms()
{
    constexpr c::Extent2u extent{260, 130};
    c::Document doc({extent, 96});
    c::History history;
    auto layer = c::Layer::raster("Linked", std::make_shared<c::ContiguousRasterSurface>(extent, c::Rgba8{8,7,6,0}));
    auto source = std::make_shared<c::ProjectLayerSource>();
    source->path = "Source.vulkana";
    layer.projectSource = source;
    layer.localToDocument.m02 = 10;
    const auto id = layer.id;
    REQUIRE(doc.insertLayer(0, std::move(layer)));
    auto pixels = [&] { return std::get<c::RasterLayer>(doc.layer(id)->payload).surface; };
    auto read = [](const c::RasterSurface& surface, int x) {
        std::array<c::Rgba8, 1> value{};
        surface.copyNative({x, 2, 1, 1}, std::as_writable_bytes(std::span(value)), 4);
        return value.front();
    };
    const auto base = pixels();
    c::ProjectPaintEdit edit(doc, id, "Brush stroke");
    const auto writer = std::get<c::RasterLayer>(edit.proxy().layer(id)->payload).surface;
    const std::array ink{c::Rgba8{32,17,13,128}};
    REQUIRE(!writer->replaceRgba8({2,2,1,1},std::as_bytes(std::span(ink)),4).empty());
    const auto first = pixels();
    REQUIRE(!writer->replaceRgba8({80,2,1,1},std::as_bytes(std::span(ink)),4).empty());
    const auto second = pixels();
    REQUIRE(!writer->replaceRgba8({145,2,1,1},std::as_bytes(std::span(ink)),4).empty());
    const auto all = pixels();
    const auto dirty = all->dirtySince(first->revision());
    REQUIRE(first->id() == all->id() && !dirty.empty() && !dirty.fullRefresh);
    REQUIRE(read(*first,80) == (c::Rgba8{8,7,6,0}) && read(*second,80) == ink.front());
    REQUIRE(all->contentBounds() == (c::RectI{2,2,144,1}));
    REQUIRE(c::projectPaintState(all)->tiles.size() == 3 && c::projectBasePixels(all) == base);
    REQUIRE(edit.commit(history) == c::RasterEditCommitResult::Committed);
    REQUIRE(history.undo(doc) && pixels() == base && history.redo(doc) && pixels() == all);
    REQUIRE(doc.setSelection(c::SelectionMask::rectangle(extent,{90,2,1,1},128)));
    REQUIRE(history.execute(doc,c::makeProjectPaintClearCommand(doc,id,true)));
    REQUIRE(read(*pixels(),80) == (c::Rgba8{32,17,13,64}) && read(*pixels(),2) == ink.front());
    REQUIRE(read(*all,80) == ink.front() && read(*base,80) == (c::Rgba8{8,7,6,0}));
    REQUIRE(history.undo(doc) && pixels() == all);
}

void linkedProjectWorkflow(const QTemporaryDir& dir)
{
    const auto parentPath = dir.filePath("Parent.vulkana");
    const auto sourcePath = dir.filePath("Eyes.vulkana");
    const auto anotherPath = dir.filePath("Alternate.vulkana");
    const auto detailId = saveSource(sourcePath, baseColor);
    saveSource(anotherPath, alternateColor);
    c::Document parentFixture({{600, 40}, 96});
    REQUIRE(parentFixture.insertLayer(0, c::Layer::raster("Background",
        std::make_shared<c::ContiguousRasterSurface>(c::Extent2u{1, 1}, c::Rgba8{}))));
    REQUIRE(u::saveProject(parentPath, parentFixture));

    c::LayerId first{}, second{};
    QImage savedAppearance;
    {
        u::MainWindow window(nullptr, false);
        window.setUnsavedPromptEnabled(false);
        u::MainWindow::FileInteractions hooks;
        hooks.reportError = [](const QString&) {}; // Expected rejection must not open a modal.
        window.setFileInteractions(std::move(hooks));
        REQUIRE(window.openImageFromPath(sourcePath));
        const auto sourceTab = window.activeDocumentId();
        REQUIRE(window.openImageFromPath(parentPath));
        const auto parentTab = window.activeDocumentId();
        REQUIRE(!window.addProjectLayer(parentPath));
        REQUIRE(window.addProjectLayer(sourcePath));
        first = session(window).activeLayer().value();
        REQUIRE(window.addProjectLayer(sourcePath));
        second = session(window).activeLayer().value();
        REQUIRE(first != second && layer(window, first).projectSource && layer(window, second).projectSource);
        REQUIRE(surface(window, first)->id() == surface(window, second)->id());
        REQUIRE(surface(window, first)->extent() == sourceSize);
        REQUIRE(detail(window, first) == detailColor);

        const auto original = layer(window, first).localToDocument;
        auto moved = original;
        moved.m02 += 4;
        REQUIRE(session(window).execute(c::makeLayerTransformCommand(*session(window).document(),
            {{first, original, moved}})));
        const auto afterMove = session(window).history().undoDepth();
        const auto secondPlacement = layer(window, second).localToDocument;
        REQUIRE(window.openProjectLayer(first) && window.activeDocumentId() == sourceTab);
        REQUIRE(!window.addProjectLayer(parentPath)); // Source would depend on its own parent.
        REQUIRE(session(window).execute(std::make_unique<c::SetLayerVisibilityCommand>(detailId, false)));
        REQUIRE(session(window).document()->isModified());
        REQUIRE(window.activateDocument(parentTab));
        REQUIRE(detail(window, first) == baseColor && detail(window, second) == baseColor);
        REQUIRE(surface(window, first)->id() == surface(window, second)->id());
        REQUIRE(layer(window, first).localToDocument == moved && layer(window, second).localToDocument == secondPlacement);
        REQUIRE(session(window).history().undoDepth() == afterMove + 1);
        REQUIRE(session(window).undo());
        REQUIRE(detail(window, first) == detailColor && detail(window, second) == detailColor);
        REQUIRE(window.activateDocument(sourceTab) && window.activateDocument(parentTab));
        REQUIRE(detail(window, first) == detailColor && session(window).history().undoDepth() == afterMove);
        REQUIRE(session(window).undo() && layer(window, first).localToDocument == original);
        REQUIRE(session(window).redo() && layer(window, first).localToDocument == moved);
        REQUIRE(session(window).redo() && detail(window, first) == baseColor);
        REQUIRE(window.openProjectLayer(second) && window.activeDocumentId() == sourceTab);
        REQUIRE(session(window).undo());
        REQUIRE(window.activateDocument(parentTab));
        REQUIRE(detail(window, first) == detailColor);
        REQUIRE(window.openProjectLayer(second) && session(window).redo());
        REQUIRE(window.activateDocument(parentTab));
        REQUIRE(detail(window, first) == baseColor);

        // Persist exactly what was rendered from the still-unsaved source.
        savedAppearance = exportedPng(window);
        REQUIRE(window.saveDocument());
    }
    REQUIRE(QFile::remove(sourcePath));
    {
        u::MainWindow window(nullptr, false);
        window.setUnsavedPromptEnabled(false);
        u::MainWindow::FileInteractions hooks;
        hooks.reportError = [](const QString&) {};
        window.setFileInteractions(std::move(hooks));
        REQUIRE(window.openImageFromPath(parentPath));
        REQUIRE(detail(window, first) == baseColor);
        REQUIRE(surface(window, first)->extent() == sourceSize);
        REQUIRE(exportedPng(window) == savedAppearance);
        REQUIRE(!window.refreshProjectLayer(first));
        REQUIRE(exportedPng(window) == savedAppearance);

        const std::atomic_bool cancelled{false};
        const auto psd = u::capturePsdExport(*session(window).document(), window.activeDocumentId());
        const auto plan = u::planPsdExport(psd, {}, cancelled);
        REQUIRE(plan);
        const auto firstRow = std::ranges::find(plan.entries, first, &u::PsdExportEntry::id);
        REQUIRE(firstRow != plan.entries.end() && firstRow->projectLayer && firstRow->attention);
        REQUIRE(firstRow->kind == u::PsdContentKind::Raster);
        const auto psdPath = dir.filePath("Layered.psd");
        REQUIRE(u::writePsdExport(psd, plan, psdPath, cancelled));
        const auto inspection = u::inspectPsd(psdPath, std::make_shared<u::PsdJob>());
        REQUIRE(inspection.error.isEmpty());
        REQUIRE(std::ranges::count_if(inspection.layers, [&](const auto& info) {
            return info.name == QString::fromStdString(layer(window, first).name) && info.raster;
        }) >= 1);

        auto bake = u::prepareRasterizeLayers(*session(window).document(),
            c::LayerSelectionState{{first}, first, first}, 64ULL * 1024 * 1024);
        REQUIRE(bake.command && session(window).execute(std::move(bake.command)));
        REQUIRE(!layer(window, first).projectSource && exportedPng(window) == savedAppearance);
        REQUIRE(session(window).undo() && layer(window, first).projectSource);

        REQUIRE(window.relinkProjectLayer(first, anotherPath));
        REQUIRE(pixel(window, first, 0, 0) == alternateColor);
        REQUIRE(pixel(window, second, 0, 0) == baseColor);
        REQUIRE(session(window).undo());
        REQUIRE(exportedPng(window) == savedAppearance);
        REQUIRE(session(window).redo());
        REQUIRE(pixel(window, first, 0, 0) == alternateColor);

        const auto historyBefore = session(window).history().undoDepth();
        REQUIRE(window.refreshProjectLayer(first));
        REQUIRE(session(window).history().undoDepth() == historyBefore);
        const auto originalPosition = layer(window, first).localToDocument;
        const c::Extent2u newSize{37, 19};
        c::Document updated({newSize, 72});
        REQUIRE(updated.insertLayer(0, c::Layer::raster("New source bounds",
            std::make_shared<c::ContiguousRasterSurface>(newSize, detailColor))));
        REQUIRE(u::saveProject(anotherPath, updated));
        REQUIRE(window.refreshProjectLayer(first));
        REQUIRE(surface(window, first)->extent() == newSize && surface(window, second)->extent() == sourceSize);
        REQUIRE(layer(window, first).localToDocument == originalPosition);

        const auto linkedAppearance = exportedPng(window);
        REQUIRE(window.convertProjectLayerToRaster(first));
        REQUIRE(!layer(window, first).projectSource && layer(window, second).projectSource);
        REQUIRE(exportedPng(window) == linkedAppearance);
        REQUIRE(session(window).undo() && layer(window, first).projectSource);
        REQUIRE(session(window).redo() && !layer(window, first).projectSource);
    }
}

void linkedPixelsAndPlacementMasks(const QTemporaryDir& dir)
{
    c::Document doc({{128, 128}, 72});
    auto pixels = std::make_shared<c::ContiguousRasterSurface>(doc.canvas().extent, baseColor);
    auto layer = c::Layer::raster("Placement", pixels);
    auto source = std::make_shared<c::ProjectLayerSource>();
    source->path = dir.filePath("Eyes.vulkana").toStdString();
    layer.projectSource = source;
    const auto id = layer.id;
    REQUIRE(doc.insertLayer(0, layer));
    const auto linked = std::get<c::RasterLayer>(doc.layer(id)->payload).surface;
    const std::array<std::byte, 4> black{};
    bool refused = false;
    try { linked->replaceRgba8({0, 0, 1, 1}, black, 4); }
    catch (const std::logic_error&) { refused = true; }
    REQUIRE(refused && linked->revision() == 1);
    REQUIRE(!c::RasterEditTransaction(doc, id, "Paint").active());
    REQUIRE(!doc.setLayerRasterStorage(id, pixels, {}, {}));

    c::History history;
    const c::LayerSelectionState selected{{id}, id, id};
    REQUIRE(history.execute(doc, c::duplicateLayerItems(doc, selected)));
    REQUIRE(history.execute(doc, std::make_unique<c::LayerViaCopyCommand>(id, id)));
    REQUIRE(doc.layers().size() == 3);
    for (const auto& placement : doc.layers()) {
        REQUIRE(placement.projectSource == source);
        REQUIRE(std::get<c::RasterLayer>(placement.payload).surface == linked);
    }
    std::vector<c::LayerId> ids;
    for (const auto& placement : doc.layers()) ids.push_back(placement.id);
    const c::LayerSelectionState all{ids, id, id};
    auto batch = c::duplicateLayerItems(doc, all);
    // Three new placements only retain one 64 KiB image.
    REQUIRE(batch && batch->memoryCost() < 2 * linked->allocatedPixelBytes());
    auto mask = std::make_shared<c::LayerMask>();
    mask->coverage = c::SelectionMask::filled(doc.canvas().extent, 255);
    REQUIRE(doc.setLayerMask(id, mask));
    c::LayerMaskEdit edit(doc, id, "Paint placement mask");
    auto proxy = std::get<c::RasterLayer>(edit.proxy().layer(id)->payload).surface;
    proxy->replaceRgba8({0, 0, 1, 1}, black, 4);
    REQUIRE(edit.commit(history) == c::RasterEditCommitResult::Committed);
    REQUIRE(doc.layer(id)->mask->coverage->coverageAtDocumentPixel(0, 0) == 0);
    REQUIRE(linked->revision() == 1 && !doc.layers().back().mask);
    REQUIRE(history.undo(doc));
    REQUIRE(doc.layer(id)->mask->coverage->coverageAtDocumentPixel(0, 0) == 255);
}

void paintOnProjectPlacement(const QTemporaryDir& dir, QVulkanInstance* vulkan)
{
    const auto parentPath = dir.filePath("Painted parent.vulkana");
    const auto sourcePath = dir.filePath("Paint source.vulkana");
    const c::Extent2u extent{128, 128};
    const auto saveSolid = [](const QString& path, c::Extent2u size, c::Rgba8 color) {
        c::Document doc({size, 72});
        REQUIRE(doc.insertLayer(0, c::Layer::raster("Base", std::make_shared<c::ContiguousRasterSurface>(size, color))));
        REQUIRE(u::saveProject(path, doc));
    };
    saveSolid(parentPath, extent, {});
    saveSolid(sourcePath, extent, alternateColor);
    u::MainWindow window(vulkan, false, false);
    window.setUnsavedPromptEnabled(false);
    window.resize(1200, 800);
    window.show();
    REQUIRE(imageeditor::test::waitForGraphicsStartup(window));
    REQUIRE(window.openImageFromPath(parentPath));
    REQUIRE(window.addProjectLayer(sourcePath));
    const auto first = session(window).activeLayer().value();
    REQUIRE(window.addProjectLayer(sourcePath));
    const auto id = session(window).activeLayer().value();
    const auto original = surface(window, first);
    REQUIRE(surface(window, id) == original);
    auto* view = dynamic_cast<u::LayerListView*>(window.findChild<QListView*>("LayerList"));
    REQUIRE(view);
    auto* model = dynamic_cast<u::LayerListModel*>(view->model());
    REQUIRE(model);
    const auto row = [&] { return model->index(model->rowForLayer(id), 0); };
    const auto clearActionEnabled = [&](bool run) {
        bool found = false, enabled = false;
        QTimer::singleShot(0, &window, [&] {
            for (auto* menu : window.findChildren<QMenu*>()) if (menu->isVisible()) {
                for (auto* action : menu->actions()) if (action->objectName() == "ClearProjectPaintAction") {
                    found = true;
                    enabled = action->isEnabled();
                    if (run && enabled) action->trigger();
                    break;
                }
                menu->close();
            }
        });
        view->customContextMenuRequested(view->visualRect(row()).center());
        REQUIRE(found);
        return enabled;
    };
    REQUIRE(!row().data(u::LayerListModel::ProjectPaintRole).toBool() && !clearActionEnabled(false));
    imageeditor::render::CanvasWindow* canvas = nullptr;
    for (auto* win : QGuiApplication::allWindows())
        if (auto* candidate = dynamic_cast<imageeditor::render::CanvasWindow*>(win)) canvas = candidate;
    REQUIRE(canvas);
    const auto action = [&](const char* name) {
        auto* value = window.findChild<QAction*>(name);
        REQUIRE(value && value->isEnabled());
        value->trigger();
        QApplication::processEvents();
    };
    const auto mouse = [&](QEvent::Type type, c::Vec2d documentPoint, Qt::MouseButton button, Qt::MouseButtons buttons) {
        const auto& scene = canvas->scene();
        const auto p = scene.viewport.documentToViewport(documentPoint, {128, 128}, scene.logicalViewport);
        const QPointF local(p.x, p.y);
        QMouseEvent event(type, local, local, QPointF(canvas->mapToGlobal(local.toPoint())), button, buttons, {});
        QCoreApplication::sendEvent(canvas, &event);
        QApplication::processEvents();
    };
    const c::Vec2d mark{84.5, 92.5}, other{32.5, 32.5};
    const auto click = [&](c::Vec2d point) {
        mouse(QEvent::MouseMove, point, Qt::NoButton, Qt::NoButton);
        mouse(QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, point, Qt::LeftButton, Qt::NoButton);
    };

    session(window).setForegroundColor(detailColor);
    action("ToolAction_brush");
    const auto depth = session(window).history().undoDepth();
    click(mark);
    REQUIRE(session(window).history().undoDepth() == depth + 1);
    REQUIRE(c::hasProjectPaint(layer(window, id)) && !c::hasProjectPaint(layer(window, first)));
    REQUIRE(row().data(u::LayerListModel::ProjectPaintRole).toBool());
    const auto painted = pixel(window, id, 84, 92);
    REQUIRE(painted != alternateColor && pixel(window, first, 84, 92) == alternateColor);
    action("ToolAction_fill");
    auto* contiguous = window.findChild<QAbstractButton*>("FillModeContiguous");
    REQUIRE(contiguous && contiguous->isEnabled());
    contiguous->click();
    const c::Rgba8 fillColor{255, 0, 0, 255};
    session(window).setForegroundColor(fillColor);
    click(mark);
    REQUIRE(QTest::qWaitFor([&] { return session(window).history().undoDepth() == depth + 2; }));
    REQUIRE(pixel(window, id, 84, 92) == fillColor && pixel(window, id, 32, 32) == alternateColor);
    REQUIRE(pixel(window, first, 84, 92) == alternateColor);
    action("UndoAction");
    REQUIRE(pixel(window, id, 84, 92) == painted);
    session(window).setForegroundColor(detailColor);
    action("ToolAction_brush");
    action("UndoAction");
    REQUIRE(!c::hasProjectPaint(layer(window, id)));
    action("RedoAction");
    REQUIRE(pixel(window, id, 84, 92) == painted);
    mouse(QEvent::MouseButtonPress, other, Qt::LeftButton, Qt::LeftButton);
    REQUIRE(pixel(window, id, 32, 32) != alternateColor);
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(window.windowHandle(), &escape);
    mouse(QEvent::MouseButtonRelease, other, Qt::LeftButton, Qt::NoButton);
    REQUIRE(pixel(window, id, 32, 32) == alternateColor && session(window).history().undoDepth() == depth + 1);

    action("ToolAction_eraser");
    click(mark);
    REQUIRE(pixel(window, id, 84, 92) == alternateColor);
    action("UndoAction");
    REQUIRE(pixel(window, id, 84, 92) == painted);
    const auto selection = c::SelectionMask::rectangle(extent, {84, 92, 1, 1}, 128);
    REQUIRE(session(window).execute(std::make_unique<c::SetSelectionCommand>(selection)));
    action("ErasePixelsAction");
    const auto faded = pixel(window, id, 84, 92);
    REQUIRE(faded != painted && faded != alternateColor && faded.alpha == 255);
    REQUIRE(session(window).document()->selection() == selection);
    action("UndoAction");
    REQUIRE(pixel(window, id, 84, 92) == painted);
    REQUIRE(clearActionEnabled(true));
    REQUIRE(!row().data(u::LayerListModel::ProjectPaintRole).toBool() && session(window).document()->selection() == selection);
    action("UndoAction");
    REQUIRE(pixel(window, id, 84, 92) == painted);
    action("UndoAction");
    REQUIRE(!session(window).document()->selection());
    action("ErasePixelsAction");
    REQUIRE(!c::hasProjectPaint(layer(window, id)) && pixel(window, id, 84, 92) == alternateColor);
    action("UndoAction");
    action("AddLayerMaskAction");
    REQUIRE(session(window).editingLayerMask());
    click(mark);
    REQUIRE(c::layerMaskCoverage(layer(window, id).mask, mark) < .01f);
    REQUIRE(pixel(window, id, 84, 92) == painted);
    action("UndoAction");
    REQUIRE(c::layerMaskCoverage(layer(window, id).mask, mark) == 1.f);

    // Source updates change pixels beneath this placement's local paint. A
    // smaller source temporarily clips the mark without discarding its tile.
    saveSolid(sourcePath, {40, 40}, baseColor);
    REQUIRE(window.refreshProjectLayer(id));
    REQUIRE(surface(window, id)->extent() == (c::Extent2u{40, 40}));
    REQUIRE(c::hasProjectPaint(layer(window, id)) && pixel(window, id, 0, 0) == baseColor);
    REQUIRE(window.saveDocument());
    saveSolid(sourcePath, extent, baseColor);
    u::MainWindow reopened(nullptr, false, false);
    reopened.setUnsavedPromptEnabled(false);
    REQUIRE(reopened.openImageFromPath(parentPath));
    REQUIRE(reopened.refreshProjectLayer(id));
    REQUIRE(pixel(reopened, id, 84, 92) == painted && pixel(reopened, id, 0, 0) == baseColor);
    REQUIRE(!c::hasProjectPaint(layer(reopened, first)));
    const auto visible = exportedPng(reopened);
    REQUIRE(visible.pixelColor(84, 92) == QColor(painted.red, painted.green, painted.blue, painted.alpha));
    REQUIRE(reopened.saveDocument());
    const auto loaded = u::loadProject(parentPath);
    REQUIRE(loaded && c::hasProjectPaint(*loaded.document->layer(id)) && !c::hasProjectPaint(*loaded.document->layer(first)));
    const auto fromDisk = std::get<c::RasterLayer>(loaded.document->layer(id)->payload).surface;
    std::array<c::Rgba8, 1> restored{};
    fromDisk->copyRgba8({84, 92, 1, 1}, std::as_writable_bytes(std::span(restored)), 4);
    REQUIRE(restored.front() == painted);
    REQUIRE(reopened.convertProjectLayerToRaster(id) && !layer(reopened, id).projectSource);
    REQUIRE(pixel(reopened, id, 84, 92) == painted);
    REQUIRE(exportedPng(reopened) == visible);
    REQUIRE(session(reopened).undo());
    REQUIRE(layer(reopened, id).projectSource && c::hasProjectPaint(layer(reopened, id)));
}

void thumbnailWorkflow(QVulkanInstance& vulkan, const QTemporaryDir& dir)
{
    const auto parentPath = dir.filePath("Parent.vulkana");
    const auto sourcePath = dir.filePath("Source.vulkana");
    c::Document parentDoc({{110, 7}, 96});
    REQUIRE(parentDoc.insertLayer(0, c::Layer::raster("Background",
        std::make_shared<c::ContiguousRasterSurface>(c::Extent2u{1, 1}, baseColor))));
    REQUIRE(u::saveProject(parentPath, parentDoc));
    const auto detailId = saveSource(sourcePath, alternateColor);
    u::MainWindow window(&vulkan, false);
    int discardPrompts = 0;
    u::MainWindow::FileInteractions files;
    files.askUnsaved = [&] { ++discardPrompts; return u::MainWindow::UnsavedChoice::Discard; };
    files.reportError = [](const QString& error) { std::cerr << error.toStdString() << '\n'; };
    window.setFileInteractions(std::move(files));
    window.resize(1200, 800);
    window.show();
    REQUIRE(imageeditor::test::waitForGraphicsStartup(window));
    REQUIRE(window.openImageFromPath(parentPath));
    const auto parentTab = window.activeDocumentId();
    auto* import = window.findChild<QAction*>("ImportImageAction");
    REQUIRE(import && import->isEnabled());
    bool projectsOffered = false;
    QTimer::singleShot(0, &window, [&] {
        if (auto* picker = qobject_cast<QFileDialog*>(QApplication::activeModalWidget())) {
            projectsOffered = picker->selectedNameFilter().contains("*.vulkana");
            picker->selectFile(sourcePath);
            QMetaObject::invokeMethod(picker, "accept", Qt::QueuedConnection);
        }
    });
    import->trigger();
    REQUIRE(projectsOffered && window.activeDocumentId() == parentTab && window.documentCount() == 1);
    const auto placed = session(window).activeLayer().value();
    auto* view = dynamic_cast<u::LayerListView*>(window.findChild<QListView*>("LayerList"));
    REQUIRE(view);
    auto* model = dynamic_cast<u::LayerListModel*>(view->model());
    REQUIRE(model);
    const auto row = [&] { return model->index(model->rowForLayer(placed), 0); };
    REQUIRE(row().isValid() && !row().data(u::LayerListModel::ProjectSourceMissingRole).toBool());
    view->scrollTo(row());
    QApplication::processEvents();
    std::unique_ptr<QMimeData> mime(model->mimeData({row()}));
    auto* transfer = dynamic_cast<u::LayerTransferMimeData*>(mime.get());
    REQUIRE(transfer);
    const auto thumb = view->thumbnailRect(row(), false).center();
    REQUIRE(view->viewport()->rect().contains(thumb));
    QTest::mouseDClick(view->viewport(), Qt::LeftButton, {}, thumb);
    REQUIRE(window.projectPath() == sourcePath);
    const auto sourceTab = window.activeDocumentId();
    REQUIRE(sourceTab != parentTab);
    REQUIRE(window.activateDocument(parentTab));
    imageeditor::render::CanvasWindow* canvas = nullptr;
    for (auto* win : QGuiApplication::allWindows())
        if (auto* candidate = dynamic_cast<imageeditor::render::CanvasWindow*>(win)) canvas = candidate;
    REQUIRE(canvas);
    canvas->setZoomAt(1.5, {200, 250});
    canvas->centerDocumentAt({42, 3}, {100, 175});
    const auto parentView = canvas->scene().viewport;
    const auto rowCenter = view->visualRect(row()).center();
    REQUIRE(!view->thumbnailRect(row(), false).contains(rowCenter));
    QTest::mouseDClick(view->viewport(), Qt::LeftButton, {}, rowCenter);
    REQUIRE(window.activeDocumentId() == sourceTab);
    canvas->setZoomAt(.625, {230, 330});
    REQUIRE(!model->onTransfer(mime.get(), {0, 0}) && !transfer->consumed);
    REQUIRE(session(window).execute(std::make_unique<c::SetLayerVisibilityCommand>(detailId, false)));

    auto* tabs = window.findChild<QTabBar*>("DocumentTabs");
    REQUIRE(tabs);
    // Observe the nested event loop while the linked render is pending. A
    // final-state assertion alone misses a frame shown with the source view.
    int observed = 0;
    bool stable = true;
    QTimer sample;
    QObject::connect(&sample, &QTimer::timeout, &window, [&] {
        if (window.activeDocumentId() != parentTab) return;
        ++observed;
        const auto& scene = canvas->scene();
        stable &= scene.document.canvas.extent == parentDoc.canvas().extent
            && scene.viewport.zoom() == parentView.zoom() && scene.viewport.pan() == parentView.pan();
    });
    sample.start(1);
    for (int i = 0; i < tabs->count(); ++i) {
        if (tabs->tabData(i).toULongLong() == parentTab)
            QTest::mouseClick(tabs, Qt::LeftButton, {}, tabs->tabRect(i).center());
    }
    sample.stop();
    REQUIRE(observed > 0 && stable);
    REQUIRE(canvas->scene().viewport.pan() == parentView.pan() && canvas->zoom() == parentView.zoom());
    REQUIRE(window.activeDocumentId() == parentTab && detail(window, placed) == alternateColor);
    REQUIRE(window.saveDocument());
    // The parent saw and saved the in-memory edit. Discarding it in the source
    // tab restores the external file's actual saved rendering in the live parent.
    REQUIRE(window.closeDocument(sourceTab));
    REQUIRE(discardPrompts == 1 && window.activeDocumentId() == parentTab);
    REQUIRE(detail(window, placed) == detailColor && session(window).document()->isModified());
    REQUIRE(QFile::remove(sourcePath));
    const auto other = dir.filePath("Other.vulkana");
    saveSource(other, baseColor);
    REQUIRE(window.openImageFromPath(other) && window.activateDocument(parentTab));
    REQUIRE(row().data(u::LayerListModel::ProjectSourceMissingRole).toBool());
    bool relinkAvailable = false;
    QTimer::singleShot(0, &window, [&] {
        for (auto* menu : window.findChildren<QMenu*>()) if (menu->isVisible()) {
            for (const auto* action : menu->actions())
                if (action->objectName() == "RelinkProjectLayerAction") relinkAvailable = action->isEnabled();
            menu->close();
        }
    });
    view->customContextMenuRequested(view->visualRect(row()).center());
    REQUIRE(relinkAvailable);
    window.setUnsavedPromptEnabled(false);
}

void savedSourceCanCloseBeforeParentIsActivated(const QTemporaryDir& dir)
{
    const auto sourcePath = dir.filePath("Closing source.vulkana");
    const auto parentPath = dir.filePath("Inactive parent.vulkana");
    const auto otherPath = dir.filePath("Other tab.vulkana");
    const auto detailId = saveSource(sourcePath, baseColor);
    saveSource(parentPath, baseColor);
    saveSource(otherPath, baseColor);
    u::MainWindow window(nullptr, false);
    window.setUnsavedPromptEnabled(false);
    REQUIRE(window.openImageFromPath(parentPath));
    const auto parent = window.activeDocumentId();
    REQUIRE(window.addProjectLayer(sourcePath));
    const auto placed = session(window).activeLayer().value();
    REQUIRE(window.openProjectLayer(placed));
    const auto source = window.activeDocumentId();
    REQUIRE(session(window).execute(std::make_unique<c::SetLayerVisibilityCommand>(detailId, false)));
    REQUIRE(window.saveDocument());
    REQUIRE(window.openImageFromPath(otherPath));
    REQUIRE(window.closeDocument(source));
    REQUIRE(window.activateDocument(parent));
    REQUIRE(detail(window, placed) == baseColor);
}

void rejectIndirectCycleBetweenClosedProjects(const QTemporaryDir& dir)
{
    const auto a = dir.filePath("A.vulkana");
    const auto b = dir.filePath("B.vulkana");
    const auto c = dir.filePath("C.vulkana");
    for (const auto& path : {a, b, c}) saveSource(path, baseColor);
    // Each link is placed with only its host open. This exercises the saved
    // dependency information rather than accidentally relying on the tab graph.
    const auto place = [](const QString& host, const QString& source, bool allowed) {
        u::MainWindow window(nullptr, false);
        window.setUnsavedPromptEnabled(false);
        u::MainWindow::FileInteractions hooks;
        hooks.reportError = [](const QString&) {};
        window.setFileInteractions(std::move(hooks));
        REQUIRE(window.openImageFromPath(host));
        const auto before = session(window).history().undoDepth();
        REQUIRE(window.addProjectLayer(source) == allowed);
        if (allowed) REQUIRE(window.saveDocument());
        else REQUIRE(session(window).history().undoDepth() == before);
    };
    place(b, c, true);
    place(a, b, true);
    place(c, a, false);
}
}

int main(int argc, char** argv)
{
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("ImageEditorTests");
    QCoreApplication::setApplicationName("ProjectLayers");
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir dir;
    if (!dir.isValid()) return 1;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
    try {
        if (app.arguments().contains("--native")) {
            QVulkanInstance vulkan;
            vulkan.setApiVersion(QVersionNumber(1, 2));
            REQUIRE(vulkan.create());
            thumbnailWorkflow(vulkan, dir);
            paintOnProjectPlacement(dir, &vulkan);
            std::cout << "Project Layer Wayland thumbnail, tab and context interactions passed\n";
            return 0;
        }
        sparseOverlayKeepsSnapshotsAndTransforms();
        linkedPixelsAndPlacementMasks(dir);
        paintOnProjectPlacement(dir, nullptr);
        linkedProjectWorkflow(dir);
        savedSourceCanCloseBeforeParentIsActivated(dir);
        rejectIndirectCycleBetweenClosedProjects(dir);
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
    std::cout << "Project Layer integration passed\n";
    return 0;
}
