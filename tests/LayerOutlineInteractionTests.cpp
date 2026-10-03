#include "imageeditor/core/LayerCommands.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/LayerListModel.hpp"
#include "imageeditor/ui/LayerListView.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/NewDocumentDialog.hpp"
#include "imageeditor/ui/OverlayDockWorkspace.hpp"
#include "imageeditor/ui/PreferencesDialog.hpp"
#include "imageeditor/ui/ProjectFile.hpp"
#include "imageeditor/ui/Theme.hpp"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QImage>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListView>
#include <QMouseEvent>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QVulkanInstance>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <vector>

namespace {
namespace core = imageeditor::core;
namespace render = imageeditor::render;
namespace ui = imageeditor::ui;
int failures = 0;
#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "FAIL line " << __LINE__ << ": " #condition "\n"; ++failures; } } while (false)
bool near(core::Vec2d a, core::Vec2d b)
{ return std::isfinite(a.x) && std::isfinite(a.y) && std::hypot(a.x-b.x, a.y-b.y) < 1e-6; }
void settle()
{
    for (int pass = 0; pass < 3; ++pass) {
        QCoreApplication::sendPostedEvents();
        QCoreApplication::processEvents();
    }
}
bool waitFor(const std::function<bool()>& predicate, int timeout = 5000)
{
    QElapsedTimer timer; timer.start();
    do { settle(); if (predicate()) return true; QTest::qWait(2); }
    while (timer.elapsed() < timeout);
    return predicate();
}
template<class Receiver>
void mouse(Receiver& receiver, QEvent::Type type, QPointF position, Qt::MouseButton button,
    Qt::MouseButtons buttons)
{
    QMouseEvent event(type, position, position, QPointF(receiver.mapToGlobal(position.toPoint())),
        button, buttons, Qt::NoModifier);
    QCoreApplication::sendEvent(&receiver, &event);
}
void key(QObject* receiver, QEvent::Type type, int code, const QString& text = {})
{
    QKeyEvent event(type, code, Qt::NoModifier, text);
    QCoreApplication::sendEvent(receiver, &event); settle();
}

struct Fixture {
    QTemporaryDir assets;
    ui::MainWindow window;
    render::CanvasWindow* canvas {};
    ui::LayerListView* view {};
    ui::LayerListModel* model {};
    ui::OverlayDockWorkspace* workspace {};
    core::LayerId base {};
    explicit Fixture(QVulkanInstance* instance = nullptr, bool persist = false)
        : window(instance, persist, false)
    {
        window.setUnsavedPromptEnabled(false);
        window.resize(1640, 940); window.show(); settle();
        for (auto* candidate : QGuiApplication::allWindows())
            if (candidate->objectName() == QStringLiteral("VulkanCanvasWindow"))
                canvas = dynamic_cast<render::CanvasWindow*>(candidate);
        view = dynamic_cast<ui::LayerListView*>(window.findChild<QListView*>(QStringLiteral("LayerList")));
        model = view ? dynamic_cast<ui::LayerListModel*>(view->model()) : nullptr;
        workspace = dynamic_cast<ui::OverlayDockWorkspace*>(
            window.findChild<QWidget*>(QStringLiteral("CanvasWorkspace")));
        QImage source(128, 96, QImage::Format_RGBA8888);
        source.fill(QColor(72, 140, 218, 255));
        const auto path = assets.filePath(QStringLiteral("outline-source.png"));
        CHECK(source.save(path)); CHECK(window.openImageFromPath(path));
        base = session().activeLayer().value_or(0);
        CHECK(canvas && !outlined());
        action("ToolAction_move");
        CHECK(canvas && !outlined());
        if (view && model && base) clickLayer(base);
        if (auto* focus = QApplication::focusWidget()) focus->clearFocus();
    }
    ~Fixture() { window.close(); settle(); }
    bool valid() const { return canvas && view && model && workspace && base; }
    core::EditorSession& session() { return const_cast<core::EditorSession&>(window.editorSession()); }
    core::Document& document() { return *session().document(); }
    const core::RasterSurface& surface()
    { return *std::get<core::RasterLayer>(document().layer(base)->payload).surface; }
    template<class T> T* find(const char* name)
    {
        auto* result = dynamic_cast<T*>(window.findChild<QObject*>(QString::fromLatin1(name)));
        CHECK(result); return result;
    }
    void action(const char* name)
    { if (auto* target = find<QAction>(name)) target->trigger(); settle(); }
    bool outlined() const
    { return canvas->scene().layerOutlineEdges && !canvas->scene().layerOutlineEdges->empty(); }
    QPointF logical(core::Vec2d point) const
    {
        const auto extent = window.editorSession().document()->canvas().extent;
        const auto& scene = canvas->scene();
        const auto mapped = scene.viewport.documentToViewport(point,
            {double(extent.width), double(extent.height)}, scene.logicalViewport);
        return {mapped.x, mapped.y};
    }
    void clickCanvas(core::Vec2d point)
    {
        mouse(*canvas, QEvent::MouseMove, logical(point), Qt::NoButton, Qt::NoButton);
        mouse(*canvas, QEvent::MouseButtonPress, logical(point), Qt::LeftButton, Qt::LeftButton);
        mouse(*canvas, QEvent::MouseButtonRelease, logical(point), Qt::LeftButton, Qt::NoButton);
        settle();
    }
    void clickLayer(core::LayerId id, Qt::KeyboardModifiers modifiers = {})
    {
        const auto index = model->index(model->rowForLayer(id), 0);
        CHECK(index.isValid()); if (!index.isValid()) return;
        view->scrollTo(index); settle();
        QTest::mouseClick(view->viewport(), Qt::LeftButton, modifiers, view->visualRect(index).center());
        settle();
    }
    void clickEmptyPanel(Qt::MouseButton button = Qt::LeftButton)
    {
        const auto point = view->viewport()->rect().bottomLeft() + QPoint(20,-4);
        CHECK(!view->indexAt(point).isValid());
        QTest::mouseClick(view->viewport(), button, Qt::NoModifier, point); settle();
    }
    void publish()
    {
        canvas->setDocument(document().snapshot(), false);
        model->refresh();
        action("ToolAction_brush"); action("ToolAction_move");
    }
    void o()
    { QTest::keyClick(canvas, Qt::Key_O); settle(); }
};

void expectFrame(const Fixture& f, const std::array<core::Vec2d, 4>& corners)
{
    const auto edges = f.canvas->scene().layerOutlineEdges;
    CHECK(edges); if (!edges) return;
    for (std::size_t i = 0; i < corners.size(); ++i) {
        const auto a = corners[i], b = corners[(i+1)%corners.size()];
        CHECK(std::ranges::count_if(*edges, [&](const core::SelectionEdge& edge) {
            return (near(edge.from, a) && near(edge.to, b)) || (near(edge.from, b) && near(edge.to, a));
        }) == 1);
    }
}

void rasterOutlinesUseContentOrigin(QVulkanInstance* instance = nullptr)
{
    Fixture f(instance); CHECK(f.valid()); if (!f.valid()) return;
    auto surface = std::make_shared<core::ContiguousRasterSurface>(core::Extent2u{18,14});
    // Transparent storage padding, including a faint edge that must not be trimmed.
    std::vector<std::byte> pixels(12 * 7 * 4, std::byte{1});
    surface->replaceRgba8({2,3,12,7}, pixels, 12 * 4);
    const std::array<core::AffineTransform, 3> transforms{{
        {}, {-1,.25,120,.15,1,8}, {1,.2,20,-.1,1,10,.002,-.001,1}
    }};
    for (const auto origin : {core::Vec2d{52,39}, core::Vec2d{-9,-6}, core::Vec2d{0,0}}) {
        CHECK(f.document().setLayerRasterStorage(f.base, surface, origin, std::nullopt));
        for (const auto& transform : transforms) {
            f.document().setLayerTransform(f.base, transform);
            f.publish();
            CHECK(f.canvas->scene().layerOutlineEdges && f.canvas->scene().layerOutlineEdges->size() == 4);
            expectFrame(f, {{transform.map(origin + core::Vec2d{2,3}),
                transform.map(origin + core::Vec2d{14,3}),
                transform.map(origin + core::Vec2d{14,10}),
                transform.map(origin + core::Vec2d{2,10})}});
        }
    }
    f.document().setLayerTransform(f.base, {});
    CHECK(f.document().setLayerRasterStorage(f.base, surface, {52,39}, std::nullopt));
    CHECK(f.document().setLayerCrop(f.base, core::LayerCrop{60,40,20,6})); f.publish();
    expectFrame(f, {{{60,42}, {66,42}, {66,46}, {60,46}}});
    CHECK(f.document().setLayerCrop(f.base, core::LayerCrop{0,0,10,10})); f.publish();
    CHECK(!f.outlined());
    CHECK(f.document().setLayerCrop(f.base, {}));
    const auto path = f.assets.filePath(QStringLiteral("compact-outline.vulkana"));
    CHECK(ui::saveProject(path, f.document()));
    CHECK(f.window.openImageFromPath(path)); settle();
    f.base = f.session().activeLayer().value_or(0);
    f.clickLayer(f.base);
    expectFrame(f, {{{54,42}, {66,42}, {66,49}, {54,49}}});
    // Clearing content keeps an editable frame at the storage origin, not canvas zero.
    auto empty = std::make_shared<core::ContiguousRasterSurface>(core::Extent2u{18,14});
    CHECK(f.document().setLayerRasterStorage(f.base, empty, {-9,39}, std::nullopt)); f.publish();
    expectFrame(f, {{{-9,39}, {9,39}, {9,53}, {-9,53}}});
    CHECK(f.session().history().undoDepth() == 0);
}

void mixedContainersKeepIndividualTransformedFrames()
{
    Fixture f; CHECK(f.valid()); if (!f.valid()) return;
    CHECK(f.document().setLayerTransform(f.base, {0, -1, 40, 1, 0, -15}));
    CHECK(f.document().setLayerVisibility(f.base, false));
    core::ShapeLayer shape; shape.size = {30.5, 20.25};
    shape.strokeEnabled = true; shape.strokeWidth = 11;
    auto shapeLayer = core::Layer::shape("Fractional shape", shape);
    shapeLayer.localToDocument = {1.2, .4, 65, -.3, .8, 20};
    const auto shapeId = shapeLayer.id;
    CHECK(f.document().insertLayer(1, std::move(shapeLayer)));
    core::TextLayer text; text.utf8 = "Outline"; text.defaultStyle.sizePixels = 17;
    auto textLayer = core::Layer::text("Transformed text", text);
    textLayer.localToDocument = {.5, -.25, 20, .4, 1, 70};
    const auto textId = textLayer.id;
    CHECK(f.document().insertLayer(2, std::move(textLayer)));
    // An import publishes the text and shape caches through the real shell.
    QImage spare(8, 8, QImage::Format_RGBA8888); spare.fill(Qt::green);
    const auto path = f.assets.filePath(QStringLiteral("unselected.png"));
    CHECK(spare.save(path)); CHECK(f.window.importImageAsLayerFromPath(path));
    const auto spareId = f.session().activeLayer().value_or(0);
    const auto group = core::makeLayerId(), folder = core::makeLayerId();
    core::LayerTree tree {{folder, spareId}, {
        {group, "Group", core::ContainerKind::Group, core::ColorLabel::None, {f.base, shapeId}, true},
        {folder, "Folder", core::ContainerKind::Folder, core::ColorLabel::None, {group, textId}, true}}};
    CHECK(f.document().replaceStructure(f.document().tree(), tree));
    f.publish(); f.clickLayer(folder);
    CHECK(f.outlined());
    CHECK(f.canvas->scene().layerOutlineEdges && f.canvas->scene().layerOutlineEdges->size() == 12);
    expectFrame(f, {{{40,-15}, {40,113}, {-56,113}, {-56,-15}}});
    expectFrame(f, {{{65,20}, {101.6,10.85}, {109.7,27.05}, {73.1,36.2}}});
    const auto* layer = f.document().layer(textId);
    CHECK(layer && layer->renderCache && !layer->renderCache->logicalExtent.empty());
    if (layer && layer->renderCache) {
        const auto width = double(layer->renderCache->logicalExtent.width);
        const auto height = double(layer->renderCache->logicalExtent.height);
        expectFrame(f, {{{20,70}, {20+.5*width,70+.4*width},
            {20+.5*width-.25*height,70+.4*width+height}, {20-.25*height,70+height}}});
    }
    // Repeated/overlapping targets still contribute one frame per leaf.
    f.session().setLayerSelection(std::array {folder, group, shapeId}, folder);
    f.publish();
    CHECK(f.canvas->scene().layerOutlineEdges && f.canvas->scene().layerOutlineEdges->size() == 12);
    f.canvas->setLayerOutlineTargets(std::array {shapeId, shapeId}, true);
    CHECK(f.canvas->scene().layerOutlineEdges && f.canvas->scene().layerOutlineEdges->size() == 4);
    expectFrame(f, {{{65,20}, {101.6,10.85}, {109.7,27.05}, {73.1,36.2}}});
}

void outlineViewChangesKeepGeometryPixelsAndHistory()
{
    Fixture f; CHECK(f.valid()); if (!f.valid()) return;
    CHECK(f.canvas->layerOutlinesVisible() && f.outlined());
    CHECK(f.session().execute(std::make_unique<core::SetLayerOpacityCommand>(f.base, .75F)));
    CHECK(f.session().execute(std::make_unique<core::SetLayerOpacityCommand>(f.base, .5F)));
    CHECK(f.session().undo()); f.publish(); f.document().markSaved();
    const auto revision = f.document().revision(), pixels = f.surface().revision();
    const auto history = f.session().history().undoDepth(), redo = f.session().history().redoDepth();
    const auto selection = f.session().layerSelectionState();
    const auto edges = f.canvas->scene().layerOutlineEdges;
    const auto geometryRevision = f.canvas->scene().layerOutlineRevision;
    expectFrame(f, {{{0,0}, {128,0}, {128,96}, {0,96}}});
    f.canvas->resetTo100Percent();
    const QPointF start(600, 450), end(637.5, 428.75);
    mouse(*f.canvas, QEvent::MouseButtonPress, start, Qt::MiddleButton, Qt::MiddleButton);
    mouse(*f.canvas, QEvent::MouseMove, end, Qt::NoButton, Qt::MiddleButton);
    mouse(*f.canvas, QEvent::MouseButtonRelease, end, Qt::MiddleButton, Qt::NoButton);
    const core::Rgba8 color {30, 220, 100, 255};
    f.canvas->setLayerOutlineColor(color); settle();
    CHECK(f.canvas->scene().layerOutlineColor == color);
    CHECK(f.canvas->scene().layerOutlineEdges == edges);
    CHECK(f.canvas->scene().layerOutlineRevision == geometryRevision);
    f.action("ToolAction_brush"); CHECK(!f.outlined());
    f.action("ToolAction_move"); CHECK(f.outlined());
    CHECK(f.canvas->setTemporaryMeasure(true)); CHECK(!f.outlined());
    CHECK(f.canvas->setTemporaryMeasure(false)); CHECK(f.outlined());
    f.action("LayerTransformAction");
    CHECK(f.canvas->scene().transformOverlay && !f.outlined());
    key(f.canvas, QEvent::KeyPress, Qt::Key_Escape);
    CHECK(!f.canvas->scene().transformOverlay && f.outlined());
    f.canvas->setLayerOutlinesVisible(false); CHECK(!f.outlined());
    f.canvas->setLayerOutlinesVisible(true); CHECK(f.outlined());
    CHECK(f.document().revision() == revision && f.surface().revision() == pixels && !f.document().isModified());
    CHECK(f.session().history().undoDepth() == history && f.session().history().redoDepth() == redo);
    CHECK(f.session().layerSelectionState() == selection);
}

void dismissalAndOOnlyRevealSelectedLayers()
{
    Fixture f; CHECK(f.valid()); if (!f.valid()) return;
    auto* toggle = f.find<QAction>("ToggleLayerOutlinesAction");
    CHECK(toggle && toggle->shortcut() == QKeySequence(QStringLiteral("O")) && toggle->isCheckable());
    // Leave an empty area in the canvas and retain visible content outside it.
    CHECK(f.document().setLayerTransform(f.base, {.5,0,-10,0,.5,10})); f.publish();
    const auto selection = f.session().layerSelectionState();
    const auto revision = f.document().revision(), pixels = f.surface().revision();
    const auto history = f.session().history().undoDepth();
    for (const auto empty : {core::Vec2d {100,80}, core::Vec2d {-5,20}}) {
        f.clickCanvas(empty); CHECK(!f.outlined());
        CHECK(f.session().layerSelectionState() == selection);
        f.o(); CHECK(!f.canvas->layerOutlinesVisible() && !f.outlined());
        f.o(); CHECK(f.canvas->layerOutlinesVisible() && f.outlined());
        f.clickCanvas(empty); CHECK(!f.outlined());
        f.clickLayer(f.base); CHECK(f.outlined()); // Reclicking an already-selected row reveals it.
        f.clickCanvas(empty); CHECK(!f.outlined());
        f.clickCanvas({20,20}); CHECK(f.outlined());
    }
    f.canvas->setLayerOutlineTargets({}, true);
    f.canvas->setLayerOutlinesVisible(false); f.canvas->setLayerOutlinesVisible(true);
    CHECK(!f.outlined());
    CHECK(f.document().revision() == revision && f.surface().revision() == pixels);
    CHECK(f.session().history().undoDepth() == history);
}

void editableFieldsAndCanvasTextOwnO()
{
    Fixture f; CHECK(f.valid()); if (!f.valid()) return;
    QLineEdit field(f.workspace->panelOverlay());
    field.setGeometry(80, 70, 180, 32); field.show(); field.setFocus(); settle();
    QTest::keyClick(&field, Qt::Key_O); settle();
    CHECK(field.text() == QStringLiteral("o") && f.canvas->layerOutlinesVisible());
    field.clearFocus(); field.hide();
    f.action("ToolAction_text"); f.clickCanvas({20,25});
    f.o();
    CHECK(f.canvas->layerOutlinesVisible() && !f.outlined());
    CHECK(std::ranges::any_of(f.document().layers(), [](const core::Layer& layer) {
        const auto* text = std::get_if<core::TextLayer>(&layer.payload);
        return text && text->utf8 == "o";
    }));
}

void selectedFramesFollowMovePreviewAndUndo()
{
    Fixture f; CHECK(f.valid()); if (!f.valid()) return;
    core::ShapeLayer shape; shape.size = {20.5, 10.25};
    auto layer = core::Layer::shape("Moved companion", shape);
    layer.localToDocument = {1,0,75,0,1,55};
    const auto id = layer.id;
    CHECK(f.document().insertLayer(1, std::move(layer)));
    f.publish();
    f.clickLayer(f.base); f.clickLayer(id, Qt::ControlModifier);
    CHECK(f.session().selectedLayers().size() == 2);
    const auto pixels = f.surface().revision();
    const auto depth = f.session().history().undoDepth();
    const auto before = f.canvas->scene().layerOutlineEdges;
    CHECK(before && before->size() == 8);
    mouse(*f.canvas, QEvent::MouseButtonPress, f.logical({20,20}), Qt::LeftButton, Qt::LeftButton);
    mouse(*f.canvas, QEvent::MouseMove, f.logical({30,40}), Qt::NoButton, Qt::LeftButton);
    CHECK(f.canvas->scene().layerOutlineEdges && f.canvas->scene().layerOutlineEdges->size() == 8);
    expectFrame(f, {{{10,20}, {138,20}, {138,116}, {10,116}}});
    expectFrame(f, {{{85,75}, {105.5,75}, {105.5,85.25}, {85,85.25}}});
    CHECK(f.session().history().undoDepth() == depth && f.surface().revision() == pixels);
    mouse(*f.canvas, QEvent::MouseButtonRelease, f.logical({30,40}), Qt::LeftButton, Qt::NoButton);
    settle();
    CHECK(f.session().history().undoDepth() == depth+1 && f.surface().revision() == pixels);
    QTest::keyClick(f.canvas, Qt::Key_Z, Qt::ControlModifier); settle();
    CHECK(f.canvas->scene().layerOutlineEdges && before && *f.canvas->scene().layerOutlineEdges == *before);
    CHECK(f.session().selectedLayers().size() == 2 && f.surface().revision() == pixels);
}

core::LayerId addCompanion(Fixture& f)
{
    core::ShapeLayer shape; shape.size = {20.5,10.25};
    auto layer = core::Layer::shape("Selection companion", shape);
    layer.localToDocument = {1,0,75,0,1,55};
    const auto id = layer.id;
    CHECK(f.document().insertLayer(f.document().layers().size(), std::move(layer)));
    f.publish();
    return id;
}

void selectPair(Fixture& f, core::LayerId first, core::LayerId primary)
{
    f.clickLayer(first); f.clickLayer(primary, Qt::ControlModifier);
    CHECK(f.session().selectedLayers().size() == 2 && f.session().activeLayer() == primary);
}

void emptyLeftClicksKeepTheMostRecentlySelectedLayer()
{
    Fixture f; CHECK(f.valid()); if (!f.valid()) return;
    const auto companion = addCompanion(f);
    f.document().markSaved();
    const auto revision = f.document().revision(), pixels = f.surface().revision();
    const auto depth = f.session().history().undoDepth();
    for (const auto* tool : {"ToolAction_move", "ToolAction_brush"}) {
        f.action(tool);
        for (const auto primary : {companion, f.base}) {
            const auto other = primary == f.base ? companion : f.base;
            selectPair(f, other, primary);
            f.clickCanvas({-80,20}); // Clear of the Brush tip as well as the canvas boundary.
            CHECK(f.session().selectedLayers() == std::vector<core::LayerId> {primary});
            CHECK(f.session().activeLayer() == primary && !f.outlined());
            selectPair(f, other, primary);
            f.clickEmptyPanel();
            CHECK(f.session().selectedLayers() == std::vector<core::LayerId> {primary});
            CHECK(f.session().activeLayer() == primary && !f.outlined());
        }
    }
    f.action("ToolAction_move"); selectPair(f, f.base, companion);
    const auto selection = f.session().layerSelectionState();
    mouse(*f.canvas, QEvent::MouseButtonPress, f.logical({-8,20}), Qt::RightButton, Qt::RightButton);
    mouse(*f.canvas, QEvent::MouseButtonRelease, f.logical({-8,20}), Qt::RightButton, Qt::NoButton);
    settle();
    CHECK(f.session().layerSelectionState() == selection);
    f.clickEmptyPanel(Qt::RightButton);
    CHECK(f.session().layerSelectionState() == selection);
    CHECK(f.document().revision() == revision && f.surface().revision() == pixels && !f.document().isModified());
    CHECK(f.session().history().undoDepth() == depth);
}

void emptyClicksRespectTransformsAndOwnedMoveGestures()
{
    Fixture f; CHECK(f.valid()); if (!f.valid()) return;
    const auto companion = addCompanion(f);
    selectPair(f, f.base, companion);
    const auto selection = f.session().layerSelectionState();
    const auto depth = f.session().history().undoDepth();
    f.action("LayerTransformAction");
    CHECK(f.session().activeTool() == core::ToolId::Transform && f.canvas->scene().transformOverlay);
    f.clickCanvas({-80,-70}); f.clickEmptyPanel();
    CHECK(f.session().activeTool() == core::ToolId::Transform && f.canvas->scene().transformOverlay);
    CHECK(f.session().layerSelectionState() == selection);
    key(f.canvas, QEvent::KeyPress, Qt::Key_Escape);
    CHECK(f.session().activeTool() == core::ToolId::Move);
    // Crossing the canvas boundary during an existing drag is still one move.
    mouse(*f.canvas, QEvent::MouseButtonPress, f.logical({20,20}), Qt::LeftButton, Qt::LeftButton);
    mouse(*f.canvas, QEvent::MouseMove, f.logical({-10,25}), Qt::NoButton, Qt::LeftButton);
    CHECK(f.session().selectedLayers().size() == 2);
    mouse(*f.canvas, QEvent::MouseButtonRelease, f.logical({-10,25}), Qt::LeftButton, Qt::NoButton);
    settle();
    CHECK(f.session().selectedLayers().size() == 2 && f.session().history().undoDepth() == depth+1);
    CHECK(f.canvas->scene().layerOutlineEdges && f.canvas->scene().layerOutlineEdges->size() == 8);
}

void collapsePreferenceAppliesRollsBackAndSurvivesRestart()
{
    QSettings settings; settings.clear();
    const auto key = QStringLiteral("preferences/canvas/collapseLayerSelectionOnEmptyClick");
    {
        Fixture f(nullptr, true); CHECK(f.valid()); if (!f.valid()) return;
        const auto companion = addCompanion(f);
        QTimer::singleShot(0, &f.window, [&] {
            auto* dialog = dynamic_cast<ui::PreferencesDialog*>(f.window.findChild<QDialog*>("PreferencesDialog"));
            CHECK(dialog); if (!dialog) return;
            auto* checkbox = dialog->findChild<QCheckBox*>("CollapseLayerSelectionOnEmptyClick");
            CHECK(checkbox && checkbox->isChecked());
            if (checkbox) checkbox->setChecked(false);
            dialog->reject();
        });
        f.action("PreferencesAction");
        selectPair(f, f.base, companion); f.clickEmptyPanel();
        CHECK(f.session().selectedLayers() == std::vector<core::LayerId> {companion});
        QTimer::singleShot(0, &f.window, [&] {
            auto* dialog = dynamic_cast<ui::PreferencesDialog*>(f.window.findChild<QDialog*>("PreferencesDialog"));
            CHECK(dialog); if (!dialog) return;
            auto* checkbox = dialog->findChild<QCheckBox*>("CollapseLayerSelectionOnEmptyClick");
            CHECK(checkbox && checkbox->isChecked());
            if (!checkbox) { dialog->reject(); return; }
            checkbox->setChecked(false);
            auto* apply = dialog->findChild<QPushButton*>("PreferencesApply");
            CHECK(apply); if (apply) apply->click();
            checkbox->setChecked(true);
            dialog->reject(); // Restore the latest applied false, not the entry true.
        });
        f.action("PreferencesAction");
        CHECK(settings.contains(key) && !settings.value(key).toBool());
        selectPair(f, f.base, companion);
        const auto selection = f.session().layerSelectionState();
        f.clickCanvas({-8,20}); CHECK(f.session().layerSelectionState() == selection && !f.outlined());
        f.clickEmptyPanel(); CHECK(f.session().layerSelectionState() == selection && !f.outlined());
    }
    {
        Fixture f(nullptr, true); CHECK(f.valid()); if (!f.valid()) return;
        const auto companion = addCompanion(f); selectPair(f, f.base, companion);
        const auto selection = f.session().layerSelectionState();
        f.clickCanvas({-8,20}); f.clickEmptyPanel();
        CHECK(f.session().layerSelectionState() == selection && !f.outlined());
        QTimer::singleShot(0, &f.window, [&] {
            auto* dialog = dynamic_cast<ui::PreferencesDialog*>(f.window.findChild<QDialog*>("PreferencesDialog"));
            CHECK(dialog); if (!dialog) return;
            auto* checkbox = dialog->findChild<QCheckBox*>("CollapseLayerSelectionOnEmptyClick");
            CHECK(checkbox && !checkbox->isChecked());
            if (checkbox) checkbox->setChecked(true);
            auto* apply = dialog->findChild<QPushButton*>("PreferencesApply");
            CHECK(apply); if (apply) apply->click();
            dialog->reject();
        });
        f.action("PreferencesAction");
        f.clickEmptyPanel();
        CHECK(f.session().selectedLayers() == std::vector<core::LayerId> {companion});
    }
    {
        Fixture f(nullptr, true); CHECK(f.valid()); if (!f.valid()) return;
        const auto companion = addCompanion(f); selectPair(f, f.base, companion);
        f.clickCanvas({-8,20});
        CHECK(f.session().selectedLayers() == std::vector<core::LayerId> {companion});
    }
    settings.clear();
}

void openedProjectsAndNewDocumentsWaitForExplicitOutlineSelection()
{
    Fixture f; CHECK(f.valid()); if (!f.valid()) return;
    addCompanion(f);
    const auto path = f.assets.filePath(QStringLiteral("outline-open.vulkana"));
    CHECK(ui::saveProject(path, f.document()));
    CHECK(f.window.openImageFromPath(path)); settle();
    CHECK(f.session().activeLayer() && f.session().selectedLayers().size() == 1);
    CHECK(!f.outlined() && f.canvas->layerOutlinesVisible());
    f.action("ToolAction_brush"); f.action("ToolAction_move"); CHECK(!f.outlined());
    f.clickCanvas({20,20}); CHECK(f.outlined());
    CHECK(f.window.openImageFromPath(path)); settle(); CHECK(f.outlined()); // Already-open project is focused, not reloaded.
    if (const auto primary = f.session().activeLayer()) f.clickLayer(*primary);
    CHECK(f.outlined());
    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, [&] {
        CHECK(false);
        for (auto* widget : QApplication::allWidgets())
            if (auto* dialog = qobject_cast<QDialog*>(widget)) dialog->reject();
    });
    QTimer::singleShot(0, &f.window, [&] {
        ui::NewDocumentDialog* dialog = nullptr;
        for (auto* candidate : f.window.findChildren<QDialog*>())
            if (auto* card = dynamic_cast<ui::NewDocumentDialog*>(candidate); card && !card->isHidden()) dialog = card;
        CHECK(dialog); if (!dialog) return;
        auto* width = dialog->findChild<QDoubleSpinBox*>("CanvasWidthSpinBox");
        auto* height = dialog->findChild<QDoubleSpinBox*>("CanvasHeightSpinBox");
        CHECK(width && height);
        if (width && height) { width->setValue(96); height->setValue(64); }
        dialog->accept();
    });
    watchdog.start(2000); f.action("NewDocumentAction"); watchdog.stop();
    CHECK(f.document().canvas().extent == (core::Extent2u {96,64}));
    CHECK(f.session().activeLayer() && !f.outlined());
    f.action("ToolAction_move"); CHECK(!f.outlined());
    if (const auto primary = f.session().activeLayer()) f.clickLayer(*primary);
    CHECK(f.outlined());
}

void generalPreferencePreviewsAppliesAndSurvivesRestart()
{
    QSettings settings; settings.clear();
    {
        Fixture f(nullptr, true); CHECK(f.valid()); if (!f.valid()) return;
        CHECK(f.canvas->layerOutlinesVisible());
        QTimer::singleShot(0, &f.window, [&] {
            auto* dialog = dynamic_cast<ui::PreferencesDialog*>(f.window.findChild<QDialog*>("PreferencesDialog"));
            CHECK(dialog); if (!dialog) return;
            auto* checkbox = dialog->findChild<QCheckBox*>("SelectedLayerOutlines");
            CHECK(checkbox && checkbox->isChecked());
            if (!checkbox) { dialog->reject(); return; }
            checkbox->setChecked(false);
            CHECK(!f.canvas->layerOutlinesVisible() && !f.outlined());
            auto* apply = dialog->findChild<QPushButton*>("PreferencesApply");
            CHECK(apply); if (apply) apply->click();
            checkbox->setChecked(true); CHECK(f.canvas->layerOutlinesVisible());
            dialog->reject(); // Cancel rolls back to the most recent applied value.
        });
        f.action("PreferencesAction");
        CHECK(!f.canvas->layerOutlinesVisible() && !f.outlined());
    }
    {
        Fixture f(nullptr, true); CHECK(f.valid()); if (!f.valid()) return;
        CHECK(!f.canvas->layerOutlinesVisible() && !f.outlined());
        f.o(); CHECK(f.canvas->layerOutlinesVisible() && f.outlined());
    }
    {
        Fixture f(nullptr, true); CHECK(f.valid()); if (!f.valid()) return;
        CHECK(f.canvas->layerOutlinesVisible() && f.outlined());
    }
    settings.clear();
}

int nativeValidation()
{
    if (QGuiApplication::platformName() != QStringLiteral("wayland")
        && QGuiApplication::platformName() != QStringLiteral("xcb")) return 77;
    std::atomic_uint64_t warnings {0}, errors {0};
    QVulkanInstance instance;
    instance.setApiVersion(QVersionNumber(1, 2));
    if (!instance.supportedLayers().contains(QByteArrayLiteral("VK_LAYER_KHRONOS_validation"))) return EXIT_FAILURE;
    instance.setLayers({QByteArrayLiteral("VK_LAYER_KHRONOS_validation")});
    instance.installDebugOutputFilter([&](QVulkanInstance::DebugMessageSeverityFlags severity,
        QVulkanInstance::DebugMessageTypeFlags type, const void* message) {
        if (!type.testFlag(QVulkanInstance::ValidationMessage)) return false;
        if (severity.testFlag(QVulkanInstance::ErrorSeverity)) ++errors;
        else if (severity.testFlag(QVulkanInstance::WarningSeverity)) ++warnings;
        else return false;
        const auto* data = static_cast<const VkDebugUtilsMessengerCallbackDataEXT*>(message);
        std::cerr << "Vulkan layer outlines: " << (data ? data->pMessage : "unknown") << '\n';
        return true;
    });
    if (!instance.create()) return EXIT_FAILURE;
    rasterOutlinesUseContentOrigin(&instance);
    {
        Fixture f(&instance); CHECK(f.valid()); if (!f.valid()) return EXIT_FAILURE;
        CHECK(f.document().setLayerTransform(f.base, {.8,-.25,-12,.25,.8,8})); f.publish();
        CHECK(waitFor([&] { return f.canvas->rendererStats().fullUploads > 0
            && !f.canvas->presentationSuppressedForResize(); }));
        QTest::qWait(150);
        const auto baseline = f.canvas->rendererStats();
        const auto revision = f.document().revision(), pixels = f.surface().revision();
        const auto history = f.session().history().undoDepth();
        for (int i = 0; i < 4; ++i) {
            f.o(); CHECK(!f.outlined()); f.o(); CHECK(f.outlined());
            f.action("ToolAction_brush"); f.action("ToolAction_move");
            const auto frame = f.canvas->rendererStats().framesSubmitted;
            f.canvas->scheduleFrame();
            CHECK(waitFor([&] { return f.canvas->rendererStats().framesSubmitted > frame; }));
        }
        const auto cache = f.canvas->scene().layerOutlineEdges;
        const auto geometryRevision = f.canvas->scene().layerOutlineRevision;
        f.canvas->resetTo100Percent();
        f.canvas->setLayerOutlineColor({245,145,45,255});
        const auto frame = f.canvas->rendererStats().framesSubmitted;
        f.canvas->scheduleFrame();
        CHECK(waitFor([&] { return f.canvas->rendererStats().framesSubmitted > frame; }));
        CHECK(f.canvas->scene().layerOutlineEdges == cache && f.canvas->scene().layerOutlineRevision == geometryRevision);
        QTest::qWait(150);
        const auto cached = f.canvas->rendererStats(); QTest::qWait(180);
        const auto idle = f.canvas->rendererStats();
        CHECK(idle.framesSubmitted == cached.framesSubmitted);
        CHECK(idle.fullUploads == baseline.fullUploads && idle.regionalUploads == baseline.regionalUploads
            && idle.uploadedBytes == baseline.uploadedBytes);
        CHECK(idle.resourceGeneration == baseline.resourceGeneration && idle.swapchainGeneration == baseline.swapchainGeneration);
        CHECK(f.document().revision() == revision && f.surface().revision() == pixels);
        CHECK(f.session().history().undoDepth() == history);
        const auto review = qEnvironmentVariable("IMAGEEDITOR_LAYER_OUTLINE_REVIEW");
        if (!review.isEmpty()) {
            f.canvas->fitDocumentToView();
            f.window.activateWindow(); QTest::qWait(200);
            QProcess capture;
            capture.start(QStringLiteral("spectacle"), {QStringLiteral("--background"), QStringLiteral("--nonotify"),
                QStringLiteral("--activewindow"), QStringLiteral("--output"), review});
            CHECK(capture.waitForFinished(5000) && capture.exitCode() == 0);
        }
    }
    instance.destroy(); settle(); CHECK(warnings == 0 && errors == 0);
    std::cout << "Native layer outline Vulkan validation: " << warnings << " warnings, " << errors << " errors\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
}

int main(int argc, char** argv)
{
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("ImageEditorTests"));
    QCoreApplication::setApplicationName(QStringLiteral("LayerOutlineInteractions"));
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings; CHECK(settings.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    ui::applyEditorTheme(application);
    if (application.arguments().contains(QStringLiteral("--wayland-validation"))
        || application.arguments().contains(QStringLiteral("--native-validation"))) return nativeValidation();
    rasterOutlinesUseContentOrigin();
    mixedContainersKeepIndividualTransformedFrames();
    outlineViewChangesKeepGeometryPixelsAndHistory();
    dismissalAndOOnlyRevealSelectedLayers();
    editableFieldsAndCanvasTextOwnO();
    selectedFramesFollowMovePreviewAndUndo();
    emptyLeftClicksKeepTheMostRecentlySelectedLayer();
    emptyClicksRespectTransformsAndOwnedMoveGestures();
    collapsePreferenceAppliesRollsBackAndSurvivesRestart();
    openedProjectsAndNewDocumentsWaitForExplicitOutlineSelection();
    generalPreferencePreviewsAppliesAndSurvivesRestart();
    if (failures) std::cerr << failures << " layer outline assertion(s) failed\n";
    else std::cout << "Layer outline interaction tests passed\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
