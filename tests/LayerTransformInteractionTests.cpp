#include "imageeditor/core/LayerTransform.hpp"
#include "imageeditor/core/LayerGeometry.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/CrossWindowPointerRouter.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/LayerListView.hpp"
#include "imageeditor/ui/OverlayDockWorkspace.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/ui/ToolOptionsBar.hpp"
#include "imageeditor/ui/TransformOptionsPage.hpp"

#include <QAction>
#include <QAbstractButton>
#include <QApplication>
#include <QCoreApplication>
#include <QContextMenuEvent>
#include <QCursor>
#include <QDoubleSpinBox>
#include <QDialog>
#include <QEventLoop>
#include <QFocusEvent>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QPen>
#include <QPixmap>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVulkanInstance>
#include <QVersionNumber>
#include <QWidget>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace core = imageeditor::core;
namespace render = imageeditor::render;
namespace ui = imageeditor::ui;

int failures = 0;

void check(bool condition, const char* expression, int line)
{
    if (!condition) {
        std::cerr << "FAIL line " << line << ": " << expression << '\n';
        ++failures;
    }
}

#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

void settleEvents()
{
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
}

bool near(double left, double right, double tolerance = 1.0e-6)
{
    return std::abs(left - right) <= tolerance;
}

bool sameTransform(const core::AffineTransform& left,
    const core::AffineTransform& right, double tolerance = 1.0e-7)
{
    return near(left.m00, right.m00, tolerance)
        && near(left.m01, right.m01, tolerance)
        && near(left.m02, right.m02, tolerance)
        && near(left.m10, right.m10, tolerance)
        && near(left.m11, right.m11, tolerance)
        && near(left.m12, right.m12, tolerance);
}

render::CanvasWindow* findCanvas()
{
    for (auto* candidate : QGuiApplication::allWindows()) {
        if (candidate->objectName() == QStringLiteral("VulkanCanvasWindow")) {
            if (auto* canvas = dynamic_cast<render::CanvasWindow*>(candidate)) {
                return canvas;
            }
        }
    }
    return nullptr;
}

QAction* actionWithShortcut(ui::MainWindow& window,
    const QKeySequence& shortcut)
{
    for (auto* action : window.findChildren<QAction*>()) {
        if (action->shortcuts().contains(shortcut)) {
            return action;
        }
    }
    return nullptr;
}

void sendMouse(render::CanvasWindow& canvas, QEvent::Type type,
    QPointF localPosition, Qt::MouseButton button, Qt::MouseButtons buttons,
    Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    const QPoint rounded = localPosition.toPoint();
    const QPointF globalPosition(canvas.mapToGlobal(rounded));
    QMouseEvent event(type, localPosition, localPosition, globalPosition,
        button, buttons, modifiers);
    QCoreApplication::sendEvent(&canvas, &event);
}

void sendMouse(QWidget& receiver, QEvent::Type type, QPointF localPosition,
    Qt::MouseButton button, Qt::MouseButtons buttons,
    Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    const QPointF globalPosition(receiver.mapToGlobal(localPosition.toPoint()));
    QMouseEvent event(type, localPosition, localPosition, globalPosition,
        button, buttons, modifiers);
    QCoreApplication::sendEvent(&receiver, &event);
}

QPointF centerOf(const std::array<core::Vec2d, 8>& handles)
{
    QPointF center;
    for (const auto index : {0U, 2U, 4U, 6U}) {
        center += QPointF(handles[index].x, handles[index].y);
    }
    return center / 4.0;
}

QPointF rotatePoint(QPointF point, QPointF center, double degrees)
{
    const auto radians = degrees * std::acos(-1.0) / 180.0;
    const auto cosine = std::cos(radians);
    const auto sine = std::sin(radians);
    const auto offset = point - center;
    return center + QPointF(cosine * offset.x() - sine * offset.y(),
        sine * offset.x() + cosine * offset.y());
}

QPointF rotationPointForCorner(const std::array<core::Vec2d, 8>& handles,
    std::size_t index)
{
    const QPointF corner(handles[index].x, handles[index].y);
    auto outward = corner - centerOf(handles);
    outward *= 15.0 / std::hypot(outward.x(), outward.y());
    return corner + outward;
}

// The original double-arrow arch sits outward of its centered hotspot and
// opens toward the layer. Check the graphic itself, not just its cache key,
// so four distinct but incorrectly oriented cursor variants cannot pass.
void checkRotationCursorFacesCorner(const render::CanvasWindow& canvas,
    std::size_t cornerIndex)
{
    const auto cursor = canvas.cursor();
    CHECK(cursor.shape() == Qt::BitmapCursor);
    CHECK(cursor.hotSpot() == QPoint(16, 16));
    const auto image = cursor.pixmap().toImage();
    CHECK(!image.isNull());
    if (image.isNull()) return;
    CHECK(near(image.devicePixelRatio(), canvas.devicePixelRatio(), 0.011));
    QPointF centroid;
    double weight = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const auto alpha = image.pixelColor(x, y).alphaF();
            centroid += alpha * QPointF((x + 0.5) / image.devicePixelRatio() - 16,
                (y + 0.5) / image.devicePixelRatio() - 16);
            weight += alpha;
        }
    }
    CHECK(weight > 0);
    if (weight == 0) return;
    centroid /= weight;
    const auto handles = canvas.logicalTransformHandles();
    const auto outward = QPointF(handles[cornerIndex].x, handles[cornerIndex].y)
        - centerOf(handles);
    const auto length = std::hypot(centroid.x(), centroid.y())
        * std::hypot(outward.x(), outward.y());
    CHECK(length > 0);
    CHECK(QPointF::dotProduct(centroid, outward) > length * 0.9);
}

struct Fixture {
    ui::MainWindow window {nullptr, false, false};
    render::CanvasWindow* canvas {nullptr};
    ui::OverlayDockWorkspace* workspace {nullptr};
    ui::ToolOptionsBar* optionsBar {nullptr};
    ui::TransformOptionsPage* transformPage {nullptr};
    QAction* transformAction {nullptr};
    ui::CrossWindowPointerRouter* pointerRouter {nullptr};

    Fixture()
    {
        window.setUnsavedPromptEnabled(false);
        window.resize(1420, 860);
        window.show();
        settleEvents();
        settleEvents();
        canvas = findCanvas();
        workspace = dynamic_cast<ui::OverlayDockWorkspace*>(
            window.findChild<QWidget*>(QStringLiteral("CanvasWorkspace")));
        optionsBar = dynamic_cast<ui::ToolOptionsBar*>(
            window.findChild<QToolBar*>(QStringLiteral("ToolOptionsBar")));
        transformPage = dynamic_cast<ui::TransformOptionsPage*>(
            window.findChild<QWidget*>(QStringLiteral("TransformOptionsPage")));
        transformAction = window.findChild<QAction*>(
            QStringLiteral("LayerTransformAction"));
        pointerRouter = dynamic_cast<ui::CrossWindowPointerRouter*>(
            window.findChild<QObject*>(QStringLiteral("CrossWindowPointerRouter")));
        // Existing geometry tests deliberately exercise unconstrained raw
        // movement. Dedicated snapping tests below turn the default back on.
        auto* snapping = window.findChild<QAction*>(QStringLiteral("SnappingAction"));
        CHECK(snapping && snapping->isChecked());
        if (snapping) snapping->setChecked(false);
    }

    ~Fixture()
    {
        window.close();
        settleEvents();
    }

    [[nodiscard]] bool valid() const
    {
        return canvas && workspace && optionsBar && transformPage
            && transformAction && pointerRouter
            && window.editorSession().document()
            && window.editorSession().activeLayer().has_value();
    }

    [[nodiscard]] const core::Layer* activeLayer() const
    {
        if (!window.editorSession().document()
            || !window.editorSession().activeLayer()) {
            return nullptr;
        }
        return window.editorSession().document()->layer(
            *window.editorSession().activeLayer());
    }

    [[nodiscard]] core::AffineTransform transform() const
    {
        const auto* layer = activeLayer();
        return layer ? layer->localToDocument : core::AffineTransform {};
    }

    [[nodiscard]] core::Revision surfaceRevision() const
    {
        const auto* layer = activeLayer();
        const auto* raster = layer
            ? std::get_if<core::RasterLayer>(&layer->payload) : nullptr;
        return raster && raster->surface ? raster->surface->revision() : 0;
    }

    void startWithAction()
    {
        transformAction->trigger();
        settleEvents();
    }

    void startWithShortcut()
    {
        canvas->requestActivate();
        settleEvents();
        QTest::keyClick(canvas, Qt::Key_T, Qt::ControlModifier);
        settleEvents();
    }
};

bool activeTransform(const Fixture& fixture)
{
    return fixture.window.editorSession().activeTool() == core::ToolId::Transform
        && fixture.canvas->scene().transformOverlay.has_value()
        && fixture.transformPage->isVisible();
}

void dragOnCanvas(render::CanvasWindow& canvas, QPointF from, QPointF to,
    Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    sendMouse(canvas, QEvent::MouseMove, from,
        Qt::NoButton, Qt::NoButton, modifiers);
    sendMouse(canvas, QEvent::MouseButtonPress, from,
        Qt::LeftButton, Qt::LeftButton, modifiers);
    sendMouse(canvas, QEvent::MouseMove, to,
        Qt::NoButton, Qt::LeftButton, modifiers);
    sendMouse(canvas, QEvent::MouseButtonRelease, to,
        Qt::LeftButton, Qt::NoButton, modifiers);
    settleEvents();
}

QPointF logicalPoint(const Fixture& fixture, core::Vec2d documentPoint)
{
    const auto& scene = fixture.canvas->scene();
    const auto extent = fixture.window.editorSession().document()->canvas().extent;
    const auto result = scene.viewport.documentToViewport(documentPoint,
        {static_cast<double>(extent.width), static_cast<double>(extent.height)},
        scene.logicalViewport);
    return {result.x, result.y};
}

struct RasterFixture : Fixture {
    QTemporaryDir assets;
    core::LayerId lowerId {0};
    core::LayerId upperId {0};

    explicit RasterFixture(bool transparentPadding = true)
    {
        QImage lower(256, 192, QImage::Format_RGBA8888);
        lower.fill(QColor(210, 75, 42, 255));
        QImage upper(128, 96, QImage::Format_RGBA8888);
        upper.fill(Qt::transparent);
        {
            QPainter painter(&upper);
            painter.fillRect(transparentPadding?64:0, 0, transparentPadding?64:128, 96, QColor(40, 180, 210, 255));
        }
        const auto lowerPath = assets.filePath(QStringLiteral("lower.png"));
        const auto upperPath = assets.filePath(QStringLiteral("upper.png"));
        CHECK(lower.save(lowerPath));
        CHECK(upper.save(upperPath));
        CHECK(window.openImageFromPath(lowerPath));
        lowerId = window.editorSession().activeLayer().value_or(0);
        CHECK(window.importImageAsLayerFromPath(upperPath));
        upperId = window.editorSession().activeLayer().value_or(0);
        settleEvents();
    }

    [[nodiscard]] core::AffineTransform geometry(core::LayerId id) const
    {
        return window.editorSession().document()->layer(id)->localToDocument;
    }
};

std::vector<std::byte> rasterBytes(const core::Layer& layer);

ui::LayerListView* layerView(const Fixture& fixture)
{
    return dynamic_cast<ui::LayerListView*>(
        fixture.window.findChild<QListView*>(QStringLiteral("LayerList")));
}

QModelIndex layerRow(const Fixture& fixture, core::LayerId id)
{
    auto* view = layerView(fixture);
    if (!view) return {};
    for (int row = 0; row < view->model()->rowCount(); ++row) {
        const auto index = view->model()->index(row, 0);
        if (index.data(Qt::UserRole).toULongLong() == id) return index;
    }
    return {};
}

void clickLayerRow(const Fixture& fixture, core::LayerId id,
    Qt::KeyboardModifiers modifiers = Qt::NoModifier, bool eye = false)
{
    auto* view = layerView(fixture);
    const auto index = layerRow(fixture, id);
    CHECK(view && index.isValid());
    if (!view || !index.isValid()) return;
    view->scrollTo(index);
    settleEvents();
    const auto point = eye ? view->visibilityIndicatorRect(index).center()
                           : view->visualRect(index).center();
    QTest::mouseClick(view->viewport(), Qt::LeftButton, modifiers, point);
    settleEvents();
}

void checkLayerHighlights(const Fixture& fixture)
{
    auto* view = layerView(fixture);
    CHECK(view);
    if (!view) return;
    const auto& session = fixture.window.editorSession();
    CHECK(view->selectionModel()->selectedRows().size() == qsizetype(session.selectedLayers().size()));
    for (int row = 0; row < view->model()->rowCount(); ++row) {
        const auto index = view->model()->index(row, 0);
        const auto id = index.data(Qt::UserRole).toULongLong();
        CHECK(view->selectionModel()->isSelected(index) == session.isLayerSelected(id));
        CHECK(index.data(Qt::UserRole + 1).toBool() == (session.activeLayer() == id));
    }
}

struct MixedLayerFixture : RasterFixture {
    std::array<core::LayerId, 3> group {};
    core::LayerId spare {0};

    MixedLayerFixture()
    {
        auto& session = const_cast<core::EditorSession&>(window.editorSession());
        auto& document = *session.document();
        CHECK(document.setLayerTransform(lowerId, {0.8, 0.1, 5, 0.1, 0.7, 10}));
        core::TextLayer text;
        text.utf8 = "Group";
        text.defaultStyle.sizePixels = 19;
        auto textLayer = core::Layer::text("Group text", text);
        textLayer.localToDocument = {-0.9, 0.2, 175, 0.15, 1.1, 30};
        core::ShapeLayer shape;
        shape.kind = core::ShapeKind::RoundedRectangle;
        shape.size = {47.5, 33.25};
        shape.strokeEnabled = true;
        shape.strokeWidth = 4;
        auto shapeLayer = core::Layer::shape("Group shape", shape);
        shapeLayer.localToDocument = {1.1, -0.25, 145, 0.2, 0.9, 125};
        group = {lowerId, textLayer.id, shapeLayer.id};
        CHECK(document.insertLayer(document.layers().size(), std::move(textLayer)));
        CHECK(document.insertLayer(document.layers().size(), std::move(shapeLayer)));
        // An ordinary import refreshes the real model and disposable text/shape
        // caches. This extra raster remains outside the eventual mixed group.
        CHECK(window.importImageAsLayerFromPath(assets.filePath(QStringLiteral("upper.png"))));
        spare = session.activeLayer().value_or(0);
        settleEvents();
    }

    void selectGroup()
    {
        clickLayerRow(*this, group[0]);
        clickLayerRow(*this, group[1], Qt::ControlModifier);
        clickLayerRow(*this, group[2], Qt::ControlModifier);
        CHECK(window.editorSession().selectedLayers().size() == group.size());
        checkLayerHighlights(*this);
    }

    std::array<core::AffineTransform, 3> matrices() const
    {
        return {geometry(group[0]), geometry(group[1]), geometry(group[2])};
    }
};

void checkMatrices(const std::array<core::AffineTransform, 3>& actual,
    const std::array<core::AffineTransform, 3>& expected)
{
    for (std::size_t i = 0; i < actual.size(); ++i) CHECK(sameTransform(actual[i], expected[i]));
}

void canvasShiftSelectionAndGroupMoveShareThePanelSelection()
{
    RasterFixture f;
    CHECK(f.valid()); if (!f.valid()) return;
    auto* undo = actionWithShortcut(f.window, QKeySequence::Undo);
    auto* redo = actionWithShortcut(f.window, QKeySequence::Redo);
    auto* activeOnly = f.window.findChild<QAbstractButton*>(QStringLiteral("MoveActiveLayerOnly"));
    CHECK(undo && redo && activeOnly); if (!undo || !redo || !activeOnly) return;
    const auto& session = f.window.editorSession();
    const auto depth = session.history().undoDepth();
    const auto content = session.document()->contentState();
    const auto lower = f.geometry(f.lowerId), upper = f.geometry(f.upperId);
    const auto lowerPoint = logicalPoint(f, {32, 40}), upperPoint = logicalPoint(f, {96, 40});
    const auto toggleLower = [&] {
        dragOnCanvas(*f.canvas, lowerPoint, lowerPoint, Qt::ShiftModifier);
        checkLayerHighlights(f);
    };
    toggleLower();
    CHECK(session.isLayerSelected(f.lowerId) && session.isLayerSelected(f.upperId));
    CHECK(session.activeLayer() == f.lowerId);
    // A rapid Shift-click's second press is a Qt double-click event, not a
    // MouseButtonPress. It must still toggle exactly once without editing text.
    sendMouse(*f.canvas,QEvent::MouseButtonDblClick,lowerPoint,Qt::LeftButton,Qt::LeftButton,Qt::ShiftModifier);
    sendMouse(*f.canvas,QEvent::MouseButtonRelease,lowerPoint,Qt::LeftButton,Qt::NoButton,Qt::ShiftModifier);
    CHECK(!session.isLayerSelected(f.lowerId));
    toggleLower();
    toggleLower();
    CHECK(session.selectedLayers().size() == 1 && session.isLayerSelected(f.upperId));
    toggleLower();
    CHECK(session.history().undoDepth() == depth && session.document()->contentState() == content);
    // Pressing a selected member makes it primary without collapsing the group.
    dragOnCanvas(*f.canvas, upperPoint, logicalPoint(f, {106, 48}));
    CHECK(session.selectedLayers().size() == 2 && session.activeLayer() == f.upperId);
    CHECK(near(f.geometry(f.lowerId).m02, lower.m02 + 10));
    CHECK(near(f.geometry(f.upperId).m02, upper.m02 + 10));
    CHECK(near(f.geometry(f.lowerId).m12, lower.m12 + 8));
    CHECK(near(f.geometry(f.upperId).m12, upper.m12 + 8));
    CHECK(session.history().undoDepth() == depth + 1);
    checkLayerHighlights(f);
    undo->trigger(); settleEvents();
    CHECK(sameTransform(f.geometry(f.lowerId), lower) && sameTransform(f.geometry(f.upperId), upper));
    CHECK(session.selectedLayers().size() == 2);
    activeOnly->click();
    dragOnCanvas(*f.canvas, logicalPoint(f, {230, 160}), logicalPoint(f, {235, 165}));
    CHECK(session.selectedLayers().size() == 2);
    CHECK(near(f.geometry(f.lowerId).m02, lower.m02 + 5));
    CHECK(near(f.geometry(f.upperId).m02, upper.m02 + 5));
    undo->trigger(); settleEvents();
    const auto redoDepth = session.history().redoDepth();
    f.window.findChild<QAbstractButton*>(QStringLiteral("MoveSelectUnderMouse"))->click();
    toggleLower(); // lower is now unselected
    dragOnCanvas(*f.canvas, lowerPoint, lowerPoint);
    CHECK(session.selectedLayers().size() == 1 && session.activeLayer() == f.lowerId);
    CHECK(session.history().redoDepth() == redoDepth);
    redo->trigger(); settleEvents();
    CHECK(near(f.geometry(f.lowerId).m02, lower.m02 + 5));
    CHECK(near(f.geometry(f.upperId).m02, upper.m02 + 5));
}

void altDragDuplicatesOnceAndMovesCopiesWithAtomicHistory()
{
    RasterFixture f; CHECK(f.valid()); if (!f.valid()) return;
    const auto& s=f.window.editorSession();
    const auto* d=s.document();
    auto* undo=actionWithShortcut(f.window,QKeySequence::Undo);
    auto* redo=actionWithShortcut(f.window,QKeySequence::Redo);
    CHECK(undo && redo); if (!undo || !redo) return;
    const auto original=f.geometry(f.upperId), lower=f.geometry(f.lowerId);
    const auto tree=d->tree(); const auto bytes=rasterBytes(*d->layer(f.upperId));
    const auto content=d->contentState(); const auto depth=s.history().undoDepth();
    const auto before=s.layerSelectionState();
    const auto press=logicalPoint(f,{96,40});

    // An Alt click or sub-threshold jitter is selection-only, not a copy/move.
    dragOnCanvas(*f.canvas,press,press,Qt::AltModifier);
    dragOnCanvas(*f.canvas,press,press+QPointF(1,0),Qt::AltModifier);
    CHECK(d->tree()==tree && s.history().undoDepth()==depth);
    CHECK(d->contentState()==content && sameTransform(f.geometry(f.upperId),original));

    sendMouse(*f.canvas,QEvent::MouseButtonPress,press,Qt::LeftButton,Qt::LeftButton,Qt::AltModifier);
    // Releasing Alt before crossing the threshold retains the latched intent.
    sendMouse(*f.canvas,QEvent::MouseMove,logicalPoint(f,{116,50}),Qt::NoButton,Qt::LeftButton);
    CHECK(d->layers().size()==3 && s.selectedLayers().size()==1);
    const auto copy=s.activeLayer().value_or(0);
    CHECK(copy && copy!=f.upperId && copy!=f.lowerId); if (!copy || copy==f.upperId) return;
    CHECK(near(f.geometry(copy).m02,original.m02+20));
    CHECK(near(f.geometry(copy).m12,original.m12+10));
    CHECK(d->contentState()==content && s.history().undoDepth()==depth);
    sendMouse(*f.canvas,QEvent::MouseMove,logicalPoint(f,{126,55}),Qt::NoButton,Qt::LeftButton,Qt::AltModifier);
    CHECK(d->layers().size()==3 && s.activeLayer()==copy);
    sendMouse(*f.canvas,QEvent::MouseButtonRelease,logicalPoint(f,{126,55}),Qt::LeftButton,Qt::NoButton);
    settleEvents();
    const auto copied=f.geometry(copy);
    CHECK(near(copied.m02,original.m02+30) && near(copied.m12,original.m12+15));
    CHECK(sameTransform(f.geometry(f.upperId),original) && sameTransform(f.geometry(f.lowerId),lower));
    CHECK(s.history().undoDepth()==depth+1 && d->contentState()!=content);
    CHECK(rasterBytes(*d->layer(copy))==bytes && rasterBytes(*d->layer(f.upperId))==bytes);
    CHECK(std::get<core::RasterLayer>(d->layer(copy)->payload).surface
        !=std::get<core::RasterLayer>(d->layer(f.upperId)->payload).surface);
    CHECK(!f.canvas->transformDragging()); checkLayerHighlights(f);

    undo->trigger(); settleEvents();
    CHECK(d->tree()==tree && s.layerSelectionState()==before && d->contentState()==content);
    CHECK(!d->containsItem(copy));
    const auto redoDepth=s.history().redoDepth();
    dragOnCanvas(*f.canvas,press,press,Qt::AltModifier);
    CHECK(s.history().redoDepth()==redoDepth);
    redo->trigger(); settleEvents();
    CHECK(s.activeLayer()==copy && sameTransform(f.geometry(copy),copied));
    undo->trigger(); settleEvents();

    // Escape cancels the whole duplicate/move, not merely its translation.
    sendMouse(*f.canvas,QEvent::MouseButtonPress,press,Qt::LeftButton,Qt::LeftButton,Qt::AltModifier);
    sendMouse(*f.canvas,QEvent::MouseMove,press+QPointF(50,30),Qt::NoButton,Qt::LeftButton);
    CHECK(d->layers().size()==3);
    QTest::keyClick(f.canvas,Qt::Key_Escape); settleEvents();
    sendMouse(*f.canvas,QEvent::MouseButtonRelease,press+QPointF(50,30),Qt::LeftButton,Qt::NoButton);
    CHECK(d->tree()==tree && s.layerSelectionState()==before && d->contentState()==content);
    CHECK(s.history().redoDepth()==redoDepth && s.history().undoDepth()==depth);
    CHECK(!f.canvas->transformDragging());
    CHECK(f.pointerRouter->captureDomain()==ui::CrossWindowPointerRouter::CaptureDomain::None);

    // Alt pressed only after an ordinary drag starts must not convert it.
    sendMouse(*f.canvas,QEvent::MouseButtonPress,press,Qt::LeftButton,Qt::LeftButton);
    sendMouse(*f.canvas,QEvent::MouseMove,press+QPointF(50,30),Qt::NoButton,Qt::LeftButton,Qt::AltModifier);
    CHECK(d->layers().size()==2 && !sameTransform(f.geometry(f.upperId),original));
    QTest::keyClick(f.canvas,Qt::Key_Escape); settleEvents();
    sendMouse(*f.canvas,QEvent::MouseButtonRelease,press+QPointF(50,30),Qt::LeftButton,Qt::NoButton);
    CHECK(d->tree()==tree && s.history().redoDepth()==redoDepth);
    // Shift's existing selection-toggle precedence remains intact.
    dragOnCanvas(*f.canvas,logicalPoint(f,{32,40}),logicalPoint(f,{52,50}),Qt::AltModifier|Qt::ShiftModifier);
    CHECK(d->layers().size()==2 && s.selectedLayers().size()==2);
}

void altDragMixedSelectionSupportsCrossPanelReleaseAndFocusCancellation()
{
    MixedLayerFixture f; CHECK(f.valid()); if (!f.valid()) return;
    f.selectGroup();
    auto* activeOnly=f.window.findChild<QAbstractButton*>(QStringLiteral("MoveActiveLayerOnly"));
    auto* panel=f.window.findChild<QWidget*>(QStringLiteral("LayersPanel"));
    auto* undo=actionWithShortcut(f.window,QKeySequence::Undo);
    CHECK(activeOnly && panel && undo); if (!activeOnly || !panel || !undo) return;
    activeOnly->click();
    const auto& s=f.window.editorSession(); const auto* d=s.document();
    const auto tree=d->tree(); const auto before=s.layerSelectionState();
    const auto originals=f.matrices(); const auto depth=s.history().undoDepth();
    const auto content=d->contentState();
    const auto press=logicalPoint(f,{230,160}); // Active-only keeps mixed targets.
    sendMouse(*f.canvas,QEvent::MouseButtonPress,press,Qt::LeftButton,Qt::LeftButton,Qt::AltModifier);
    sendMouse(*f.canvas,QEvent::MouseMove,press+QPointF(40,25),Qt::NoButton,Qt::LeftButton);
    CHECK(d->layers().size()==8 && s.selectedLayers().size()==3);
    const auto copies=s.selectedLayers();
    const QPointF panelPoint(panel->rect().center());
    sendMouse(*panel,QEvent::MouseMove,panelPoint,Qt::NoButton,Qt::LeftButton);
    sendMouse(*panel,QEvent::MouseButtonRelease,panelPoint,Qt::LeftButton,Qt::NoButton);
    settleEvents();
    CHECK(s.history().undoDepth()==depth+1 && s.selectedLayers()==copies);
    CHECK(!f.canvas->transformDragging());
    CHECK(f.pointerRouter->captureDomain()==ui::CrossWindowPointerRouter::CaptureDomain::None);
    checkMatrices(f.matrices(),originals); checkLayerHighlights(f);
    core::Vec2d delta;
    for (std::size_t i=0;i<copies.size();++i) {
        const auto* copy=d->layer(copies[i]); const auto* source=d->layer(f.group[i]);
        CHECK(copy && copy->payload.index()==source->payload.index()); if (!copy) continue;
        if (i==0) delta={copy->localToDocument.m02-originals[i].m02,copy->localToDocument.m12-originals[i].m12};
        auto expected=originals[i]; expected.m02+=delta.x; expected.m12+=delta.y;
        CHECK(sameTransform(copy->localToDocument,expected));
        if (const auto* text=std::get_if<core::TextLayer>(&source->payload)) CHECK(*text==std::get<core::TextLayer>(copy->payload));
        if (const auto* shape=std::get_if<core::ShapeLayer>(&source->payload)) CHECK(*shape==std::get<core::ShapeLayer>(copy->payload));
    }
    undo->trigger(); settleEvents();
    CHECK(d->tree()==tree && s.layerSelectionState()==before && d->contentState()==content);
    const auto redo=s.history().redoDepth();
    sendMouse(*f.canvas,QEvent::MouseButtonPress,press,Qt::LeftButton,Qt::LeftButton,Qt::AltModifier);
    sendMouse(*f.canvas,QEvent::MouseMove,press+QPointF(40,25),Qt::NoButton,Qt::LeftButton);
    CHECK(d->layers().size()==8);
    QFocusEvent focusLoss(QEvent::FocusOut,Qt::OtherFocusReason);
    QCoreApplication::sendEvent(f.canvas,&focusLoss); settleEvents();
    sendMouse(*f.canvas,QEvent::MouseButtonRelease,press+QPointF(40,25),Qt::LeftButton,Qt::NoButton);
    CHECK(d->tree()==tree && s.layerSelectionState()==before && d->contentState()==content);
    CHECK(s.history().redoDepth()==redo && s.history().undoDepth()==depth);
}

void movementSnappingModifiersPreferencesAndGuideCleanup()
{
    // Start aligned with the canvas. Transparent storage margins are no
    // longer snap anchors; the padded fixture is covered by bounds tests.
    RasterFixture f(false); CHECK(f.valid()); if (!f.valid()) return;
    auto* snapping=f.window.findChild<QAction*>("SnappingAction");
    auto* layers=f.window.findChild<QAction*>("SnapLayersAction");
    auto* canvas=f.window.findChild<QAction*>("SnapCanvasAction");
    auto* panel=f.window.findChild<QWidget*>("LayersPanel");
    CHECK(snapping && layers && canvas && panel); if (!snapping || !layers || !canvas || !panel) return;
    snapping->setChecked(true); layers->setChecked(false);
    const auto& s=f.window.editorSession(); const auto original=f.geometry(f.upperId);
    const auto depth=s.history().undoDepth(), content=s.document()->contentState();
    const auto start=logicalPoint(f,{96,40}), close=start+QPointF(4,3);
    sendMouse(*f.canvas,QEvent::MouseButtonPress,start,Qt::LeftButton,Qt::LeftButton);
    sendMouse(*f.canvas,QEvent::MouseMove,close,Qt::NoButton,Qt::LeftButton);
    CHECK(sameTransform(f.geometry(f.upperId),original));
    CHECK(f.canvas->scene().snapGuides[0] && f.canvas->scene().snapGuides[1]);
    // Control transitions reach the native canvas without a new pointer event.
    QTest::keyPress(f.canvas,Qt::Key_Control);
    CHECK(!f.canvas->scene().snapGuides[0] && !f.canvas->scene().snapGuides[1]);
    CHECK(near(f.geometry(f.upperId).m02,original.m02+4/f.canvas->zoom()));
    QTest::keyRelease(f.canvas,Qt::Key_Control);
    CHECK(sameTransform(f.geometry(f.upperId),original) && f.canvas->scene().snapGuides[0]);
    snapping->setChecked(false);
    CHECK(!f.canvas->scene().snapGuides[0] && !sameTransform(f.geometry(f.upperId),original));
    snapping->setChecked(true); CHECK(f.canvas->scene().snapGuides[0]);
    canvas->setChecked(false); CHECK(!f.canvas->scene().snapGuides[0]);
    canvas->setChecked(true); CHECK(f.canvas->scene().snapGuides[0]);
    sendMouse(*f.canvas,QEvent::MouseButtonRelease,close,Qt::LeftButton,Qt::NoButton);
    CHECK(!f.canvas->scene().snapGuides[0] && !f.canvas->scene().snapGuides[1]);
    CHECK(s.history().undoDepth()==depth && s.document()->contentState()==content);
    // Ctrl+T translation uses the same result; no snapped baseline is retained.
    f.startWithAction();
    const auto mid=centerOf(f.canvas->logicalTransformHandles());
    sendMouse(*f.canvas,QEvent::MouseButtonPress,mid,Qt::LeftButton,Qt::LeftButton);
    sendMouse(*f.canvas,QEvent::MouseMove,mid+QPointF(4,3),Qt::NoButton,Qt::LeftButton);
    CHECK(f.canvas->scene().snapGuides[0] && sameTransform(f.geometry(f.upperId),original));
    QTest::keyPress(panel,Qt::Key_Control);
    CHECK(!f.canvas->scene().snapGuides[0] && !sameTransform(f.geometry(f.upperId),original));
    QTest::keyRelease(panel,Qt::Key_Control);
    CHECK(f.canvas->scene().snapGuides[0] && sameTransform(f.geometry(f.upperId),original));
    QTest::keyClick(f.canvas,Qt::Key_Escape); settleEvents();
    CHECK(!f.canvas->scene().snapGuides[0] && sameTransform(f.geometry(f.upperId),original));
    sendMouse(*f.canvas,QEvent::MouseButtonRelease,mid,Qt::LeftButton,Qt::NoButton);
    // A cross-panel release owns the same drag and always removes guides.
    sendMouse(*f.canvas,QEvent::MouseButtonPress,start,Qt::LeftButton,Qt::LeftButton);
    sendMouse(*f.canvas,QEvent::MouseMove,close,Qt::NoButton,Qt::LeftButton);
    CHECK(f.canvas->scene().snapGuides[0]);
    const auto local=panel->mapFromGlobal(f.canvas->mapToGlobal(close.toPoint()));
    sendMouse(*panel,QEvent::MouseButtonRelease,local,Qt::LeftButton,Qt::NoButton);
    CHECK(!f.canvas->scene().snapGuides[0] && !f.canvas->transformDragging());
    CHECK(f.pointerRouter->captureDomain()==ui::CrossWindowPointerRouter::CaptureDomain::None);
}

void snappingPreferencesReloadIndependentlyOfDocument()
{
    QSettings settings;
    settings.remove(QStringLiteral("view/snapping"));
    {
        ui::MainWindow window(nullptr,true,false);
        window.setUnsavedPromptEnabled(false);
        const auto* document=window.editorSession().document();
        const auto content=document?document->contentState():0;
        const auto depth=window.editorSession().history().undoDepth();
        auto* master=window.findChild<QAction*>("SnappingAction");
        auto* canvas=window.findChild<QAction*>("SnapCanvasAction");
        auto* layers=window.findChild<QAction*>("SnapLayersAction");
        CHECK(master && canvas && layers); if (!master || !canvas || !layers) return;
        CHECK(master->isChecked() && canvas->isChecked() && layers->isChecked());
        canvas->setChecked(false); master->setChecked(false);
        CHECK(!settings.value("view/snapping/enabled").toBool());
        CHECK(!settings.value("view/snapping/canvas").toBool() && settings.value("view/snapping/layers").toBool());
        CHECK(window.editorSession().history().undoDepth()==depth);
        CHECK(!document || document->contentState()==content);
    }
    {
        ui::MainWindow window(nullptr,true,false);
        window.setUnsavedPromptEnabled(false);
        CHECK(!window.findChild<QAction*>("SnappingAction")->isChecked());
        CHECK(!window.findChild<QAction*>("SnapCanvasAction")->isChecked());
        CHECK(window.findChild<QAction*>("SnapLayersAction")->isChecked());
    }
    settings.remove(QStringLiteral("view/snapping"));
}

void panelRangesUseDisplayedRowsAndEyeControlsPreserveSelection()
{
    MixedLayerFixture f;
    CHECK(f.valid()); if (!f.valid()) return;
    const auto& session = f.window.editorSession();
    auto* view = layerView(f);
    CHECK(view && view->model()->rowCount() == 5); if (!view) return;
    const auto depth = session.history().undoDepth();
    const auto content = session.document()->contentState();
    // Display order is spare, shape, text, upper raster, lower raster.
    clickLayerRow(f, f.spare);
    clickLayerRow(f, f.upperId, Qt::ShiftModifier);
    CHECK(session.selectedLayers().size() == 4 && session.activeLayer() == f.upperId);
    CHECK(session.selectionAnchor() == f.spare);
    CHECK(!session.isLayerSelected(f.lowerId));
    for (const auto id : {f.spare, f.group[2], f.group[1], f.upperId}) CHECK(session.isLayerSelected(id));
    checkLayerHighlights(f);
    clickLayerRow(f, f.group[2], Qt::ControlModifier);
    CHECK(session.selectedLayers().size() == 3 && !session.isLayerSelected(f.group[2]));
    const auto rapidPoint=view->visualRect(layerRow(f,f.group[2])).center();
    sendMouse(*view->viewport(),QEvent::MouseButtonDblClick,rapidPoint,Qt::LeftButton,Qt::LeftButton,Qt::ControlModifier);
    sendMouse(*view->viewport(),QEvent::MouseButtonRelease,rapidPoint,Qt::LeftButton,Qt::NoButton,Qt::ControlModifier);
    CHECK(session.isLayerSelected(f.group[2]));
    clickLayerRow(f,f.group[2],Qt::ControlModifier);
    checkLayerHighlights(f);
    CHECK(session.history().undoDepth() == depth && session.document()->contentState() == content);
    const auto selected = session.selectedLayers();
    const auto primary = session.activeLayer(), anchor = session.selectionAnchor();
    clickLayerRow(f, f.lowerId, Qt::NoModifier, true);
    CHECK(!session.document()->layer(f.lowerId)->visible);
    CHECK(session.selectedLayers() == selected && session.activeLayer() == primary && session.selectionAnchor() == anchor);
    clickLayerRow(f, f.lowerId, Qt::NoModifier, true);
    CHECK(session.document()->layer(f.lowerId)->visible);
    CHECK(session.selectedLayers() == selected && session.activeLayer() == primary);
    checkLayerHighlights(f);
    CHECK(session.history().undoDepth() == depth + 2);
    clickLayerRow(f, f.group[1]);
    CHECK(session.selectedLayers().size() == 1 && session.activeLayer() == f.group[1]);
    checkLayerHighlights(f);
}

void transformDefaultsToLockedWithShiftFreeScaling()
{
    RasterFixture f;CHECK(f.valid());if(!f.valid())return;
    auto* lock=f.window.findChild<QToolButton*>("TransformAspectLock");
    CHECK(lock && lock->isChecked());if(!lock)return;
    f.startWithAction();
    const auto original=f.transform();
    const auto handles=f.canvas->logicalTransformHandles();
    const QPointF press(handles[4].x,handles[4].y),end=press+QPointF(43,7);
    sendMouse(*f.canvas,QEvent::MouseButtonPress,press,Qt::LeftButton,Qt::LeftButton);
    sendMouse(*f.canvas,QEvent::MouseMove,end,Qt::NoButton,Qt::LeftButton);
    const auto locked=f.transform();CHECK(!sameTransform(locked,original));CHECK(near(locked.m00,locked.m11));
    sendMouse(*f.canvas,QEvent::MouseMove,end,Qt::NoButton,Qt::LeftButton,Qt::ShiftModifier);
    CHECK(!near(f.transform().m00,f.transform().m11));CHECK(lock->isChecked());
    sendMouse(*f.canvas,QEvent::MouseMove,end,Qt::NoButton,Qt::LeftButton);
    CHECK(sameTransform(f.transform(),locked));
    f.canvas->cancelTransformInput();CHECK(sameTransform(f.transform(),original));
    auto* width=f.window.findChild<QDoubleSpinBox*>("TransformScaleXControl");
    auto* height=f.window.findChild<QDoubleSpinBox*>("TransformScaleYControl");
    CHECK(width && height);if(width && height){width->setValue(125);settleEvents();CHECK(near(height->value(),125));}
    QTest::keyClick(f.canvas,Qt::Key_Escape);settleEvents();
    CHECK(sameTransform(f.transform(),original));
}

void mixedGroupTransformUsesOneCommonDeltaAndRetainsIndividualActions()
{
    MixedLayerFixture f;
    CHECK(f.valid()); if (!f.valid()) return;
    f.selectGroup();
    const auto& session = f.window.editorSession();
    auto* undo = actionWithShortcut(f.window, QKeySequence::Undo);
    auto* redo = actionWithShortcut(f.window, QKeySequence::Redo);
    auto* x = f.window.findChild<QDoubleSpinBox*>(QStringLiteral("TransformXControl"));
    auto* scale = f.window.findChild<QDoubleSpinBox*>(QStringLiteral("TransformScaleXControl"));
    auto* angle = f.window.findChild<QDoubleSpinBox*>(QStringLiteral("TransformAngleControl"));
    auto* flip = f.window.findChild<QToolButton*>(QStringLiteral("TransformFlipHorizontal"));
    auto* apply = f.window.findChild<QToolButton*>(QStringLiteral("TransformApply"));
    CHECK(undo && redo && x && scale && angle && flip && apply);
    if (!undo || !redo || !x || !scale || !angle || !flip || !apply) return;
    const auto before = f.matrices();
    const auto unselected = f.geometry(f.spare);
    const auto depth = session.history().undoDepth();
    const auto text = std::get<core::TextLayer>(session.document()->layer(f.group[1])->payload);
    const auto shape = std::get<core::ShapeLayer>(session.document()->layer(f.group[2])->payload);
    const auto bytes = rasterBytes(*session.document()->layer(f.lowerId));
    f.startWithAction();
    CHECK(activeTransform(f)); if (!activeTransform(f)) return;
    const auto frameBefore = f.canvas->scene().transformOverlay->localToDocument;
    const auto inverseFrame = frameBefore.inverted();
    CHECK(inverseFrame); if (!inverseFrame) return;
    const auto checkSharedDelta = [&] {
        const auto delta = core::composeAffine(f.canvas->scene().transformOverlay->localToDocument, *inverseFrame);
        const auto actual = f.matrices();
        for (std::size_t i = 0; i < before.size(); ++i)
            CHECK(sameTransform(actual[i], core::composeAffine(delta, before[i])));
        CHECK(sameTransform(f.geometry(f.spare), unselected));
        CHECK(std::get<core::TextLayer>(session.document()->layer(f.group[1])->payload) == text);
        CHECK(std::get<core::ShapeLayer>(session.document()->layer(f.group[2])->payload) == shape);
        CHECK(rasterBytes(*session.document()->layer(f.lowerId)) == bytes);
        CHECK(session.selectedLayers().size() == 3);
    };
    std::vector<std::array<core::AffineTransform,3>> actions {before};
    x->setValue(x->value() + 11); settleEvents(); actions.push_back(f.matrices()); checkSharedDelta();
    scale->setValue(145); settleEvents(); actions.push_back(f.matrices()); checkSharedDelta();
    angle->setValue(27); settleEvents(); actions.push_back(f.matrices()); checkSharedDelta();
    flip->click(); settleEvents(); actions.push_back(f.matrices()); checkSharedDelta();
    CHECK(session.history().undoDepth() == depth);
    for (std::size_t i = actions.size()-1; i > 0; --i) {
        undo->trigger(); settleEvents(); CHECK(activeTransform(f)); checkMatrices(f.matrices(), actions[i-1]);
    }
    for (std::size_t i = 1; i < actions.size(); ++i) {
        redo->trigger(); settleEvents(); CHECK(activeTransform(f)); checkMatrices(f.matrices(), actions[i]);
    }
    apply->click(); settleEvents();
    CHECK(!activeTransform(f) && session.history().undoDepth() == depth + 4);
    for (std::size_t i = actions.size()-1; i > 0; --i) {
        undo->trigger(); settleEvents(); checkMatrices(f.matrices(), actions[i-1]);
    }
    const auto redoDepth = session.history().redoDepth();
    f.startWithAction();
    x->setValue(x->value() + 13); flip->click(); settleEvents();
    QTest::keyClick(f.canvas, Qt::Key_Escape); settleEvents();
    checkMatrices(f.matrices(), before);
    CHECK(session.history().undoDepth() == depth && session.history().redoDepth() == redoDepth);
    redo->trigger(); settleEvents(); checkMatrices(f.matrices(), actions[1]);
}

void rightClickAppliesGroupTransformsAndConsumesLatePointerEvents()
{
    RasterFixture f;
    CHECK(f.valid()); if (!f.valid()) return;
    clickLayerRow(f, f.lowerId);
    clickLayerRow(f, f.upperId, Qt::ControlModifier);
    auto* x = f.window.findChild<QDoubleSpinBox*>(QStringLiteral("TransformXControl"));
    CHECK(x); if (!x) return;
    const auto& session = f.window.editorSession();
    const auto depth = session.history().undoDepth();
    f.startWithAction();
    x->setValue(x->value() + 9); settleEvents();
    const auto idleLower = f.geometry(f.lowerId), idleUpper = f.geometry(f.upperId);
    const auto elsewhere = logicalPoint(f, {240,180});
    const auto rightPress = [&](Qt::MouseButtons otherButtons) {
        sendMouse(*f.canvas, QEvent::MouseButtonPress, elsewhere, Qt::RightButton, otherButtons | Qt::RightButton);
    };
    rightPress(Qt::NoButton);
    sendMouse(*f.canvas, QEvent::MouseButtonRelease, elsewhere, Qt::RightButton, Qt::NoButton);
    settleEvents();
    CHECK(!activeTransform(f) && session.history().undoDepth() == depth + 1);
    CHECK(sameTransform(f.geometry(f.lowerId), idleLower) && sameTransform(f.geometry(f.upperId), idleUpper));
    f.startWithAction();
    const auto center = centerOf(f.canvas->logicalTransformHandles());
    sendMouse(*f.canvas, QEvent::MouseButtonPress, center, Qt::LeftButton, Qt::LeftButton);
    sendMouse(*f.canvas, QEvent::MouseMove, center + QPointF(23,17), Qt::NoButton, Qt::LeftButton);
    CHECK(f.canvas->transformDragging());
    const auto draggedLower = f.geometry(f.lowerId), draggedUpper = f.geometry(f.upperId);
    rightPress(Qt::LeftButton);
    CHECK(!f.canvas->transformDragging());
    QContextMenuEvent menu(QContextMenuEvent::Mouse, elsewhere.toPoint(), f.canvas->mapToGlobal(elsewhere.toPoint()));
    menu.setAccepted(false); QCoreApplication::sendEvent(f.canvas, &menu); CHECK(menu.isAccepted());
    sendMouse(*f.canvas, QEvent::MouseButtonRelease, elsewhere, Qt::RightButton, Qt::LeftButton);
    sendMouse(*f.canvas, QEvent::MouseMove, elsewhere, Qt::NoButton, Qt::LeftButton);
    sendMouse(*f.canvas, QEvent::MouseButtonRelease, elsewhere, Qt::LeftButton, Qt::NoButton);
    settleEvents();
    CHECK(!activeTransform(f) && session.history().undoDepth() == depth + 2);
    CHECK(sameTransform(f.geometry(f.lowerId), draggedLower) && sameTransform(f.geometry(f.upperId), draggedUpper));
    CHECK(session.selectedLayers().size() == 2);
    CHECK(f.pointerRouter->captureDomain() == ui::CrossWindowPointerRouter::CaptureDomain::None);
    auto* undo = actionWithShortcut(f.window, QKeySequence::Undo);
    CHECK(undo); if (!undo) return;
    undo->trigger(); settleEvents();
    CHECK(sameTransform(f.geometry(f.lowerId), idleLower) && sameTransform(f.geometry(f.upperId), idleUpper));
}

std::vector<std::byte> rasterBytes(const core::Layer& layer)
{
    const auto& raster = std::get<core::RasterLayer>(layer.payload);
    const auto extent = raster.surface->extent();
    std::vector<std::byte> result(static_cast<std::size_t>(extent.width)
        * static_cast<std::size_t>(extent.height) * 4);
    raster.surface->copyRgba8({0, 0, static_cast<int>(extent.width),
        static_cast<int>(extent.height)}, result,
        static_cast<std::size_t>(extent.width) * 4);
    return result;
}

struct MoveControls {
    ui::TransformOptionsPage* page;
    QDoubleSpinBox* x;
    QDoubleSpinBox* y;
    QDoubleSpinBox* angle;
    QToolButton* flipH;
    QToolButton* flipV;
    QToolButton* activeOnly;
    QToolButton* underMouse;

    explicit MoveControls(Fixture& fixture)
        : page(dynamic_cast<ui::TransformOptionsPage*>(fixture.window.findChild<QWidget*>(
              QStringLiteral("MoveOptionsPage"))))
        , x(fixture.window.findChild<QDoubleSpinBox*>(QStringLiteral("MoveXControl")))
        , y(fixture.window.findChild<QDoubleSpinBox*>(QStringLiteral("MoveYControl")))
        , angle(fixture.window.findChild<QDoubleSpinBox*>(QStringLiteral("MoveAngleControl")))
        , flipH(fixture.window.findChild<QToolButton*>(QStringLiteral("MoveFlipHorizontal")))
        , flipV(fixture.window.findChild<QToolButton*>(QStringLiteral("MoveFlipVertical")))
        , activeOnly(fixture.window.findChild<QToolButton*>(QStringLiteral("MoveActiveLayerOnly")))
        , underMouse(fixture.window.findChild<QToolButton*>(QStringLiteral("MoveSelectUnderMouse")))
    {
    }

    [[nodiscard]] bool valid() const
    {
        return page && x && y && angle && flipH && flipV && activeOnly && underMouse;
    }

    void checkMatches(const Fixture& fixture) const
    {
        const auto bounds=core::layerInteractionBounds(*fixture.activeLayer());
        const auto frame=core::composeTransform(fixture.transform(),{1,0,bounds.x,0,1,bounds.y});
        const auto values = core::geometryValuesFromTransform(frame,core::Extent2d{bounds.width,bounds.height});
        CHECK(values.has_value());
        if (values) {
            CHECK(near(x->value(), values->center.x, 0.011));
            CHECK(near(y->value(), values->center.y, 0.011));
            CHECK(near(std::remainder(angle->value() - values->rotationDegrees, 360.0), 0, 0.011));
        }
        CHECK(fixture.window.editorSession().activeTool() == core::ToolId::Move);
        CHECK(!fixture.canvas->scene().transformOverlay);
        CHECK(page->isVisible());
    }
};

void moveOptionsStayCachedAndFollowSelectionAndCanvasGeometry()
{
    RasterFixture fixture;
    CHECK(fixture.valid());
    if (!fixture.valid()) return;
    MoveControls controls(fixture);
    CHECK(controls.valid());
    if (!controls.valid()) return;
    const auto workspaceGeometry = fixture.workspace->geometry();
    const auto canvasGeometry = fixture.workspace->canvasContainer()->geometry();
    const auto toolbarGeometry = fixture.optionsBar->geometry();
    CHECK(fixture.optionsBar->pageForTool(core::ToolId::Move) == controls.page);
    CHECK(controls.page->findChildren<QDoubleSpinBox*>().size() == 3);
    for (const auto* name : {"MoveScaleXControl", "MoveScaleYControl",
             "MoveAspectLock", "MoveApply", "MoveCancel"}) {
        CHECK(!fixture.window.findChild<QWidget*>(QString::fromLatin1(name)));
    }
    for (auto* button : {controls.flipH, controls.flipV, controls.activeOnly, controls.underMouse}) {
        CHECK(button->property("toolOptionsButton").toBool());
        CHECK(!button->autoRaise());
    }
    controls.checkMatches(fixture);
    const auto originalDepth = fixture.window.editorSession().history().undoDepth();
    dragOnCanvas(*fixture.canvas, logicalPoint(fixture, {32, 40}),
        logicalPoint(fixture, {32, 40}));
    CHECK(fixture.window.editorSession().activeLayer() == fixture.lowerId);
    controls.checkMatches(fixture);
    CHECK(fixture.window.editorSession().history().undoDepth() == originalDepth);
    dragOnCanvas(*fixture.canvas, logicalPoint(fixture, {96, 40}),
        logicalPoint(fixture, {108, 47}));
    CHECK(fixture.window.editorSession().activeLayer() == fixture.upperId);
    controls.checkMatches(fixture);
    CHECK(fixture.window.editorSession().history().undoDepth() == originalDepth + 1);

    auto* brush = fixture.window.findChild<QAction*>(QStringLiteral("ToolAction_brush"));
    auto* move = fixture.window.findChild<QAction*>(QStringLiteral("ToolAction_move"));
    CHECK(brush && move);
    if (!brush || !move) return;
    for (int i = 0; i < 3; ++i) {
        brush->trigger();
        settleEvents();
        CHECK(!controls.page->isVisible());
        move->trigger();
        settleEvents();
        controls.checkMatches(fixture);
        CHECK(fixture.window.findChild<QWidget*>(QStringLiteral("MoveOptionsPage")) == controls.page);
        CHECK(fixture.window.findChild<QDoubleSpinBox*>(QStringLiteral("MoveXControl")) == controls.x);
        CHECK(fixture.workspace->geometry() == workspaceGeometry);
        CHECK(fixture.workspace->canvasContainer()->geometry() == canvasGeometry);
        CHECK(fixture.optionsBar->geometry() == toolbarGeometry);
    }
}

void moveNumericEditsAndFlipsHaveIndependentGlobalUndoAndPreservePixels()
{
    RasterFixture fixture;
    CHECK(fixture.valid());
    if (!fixture.valid()) return;
    MoveControls controls(fixture);
    auto* undo = actionWithShortcut(fixture.window, QKeySequence::Undo);
    auto* redo = actionWithShortcut(fixture.window, QKeySequence::Redo);
    CHECK(controls.valid() && undo && redo);
    if (!controls.valid() || !undo || !redo) return;
    auto* scaleX = fixture.window.findChild<QDoubleSpinBox*>(QStringLiteral("TransformScaleXControl"));
    auto* scaleY = fixture.window.findChild<QDoubleSpinBox*>(QStringLiteral("TransformScaleYControl"));
    auto* angle = fixture.window.findChild<QDoubleSpinBox*>(QStringLiteral("TransformAngleControl"));
    auto* apply = fixture.window.findChild<QToolButton*>(QStringLiteral("TransformApply"));
    CHECK(scaleX && scaleY && angle && apply);
    if (!scaleX || !scaleY || !angle || !apply) return;
    // The reduced Move page must retain affine components it does not expose:
    // editing position/angle cannot reset a pre-existing nonuniform flip.
    fixture.startWithAction();
    scaleX->setValue(-125);
    scaleY->setValue(67);
    angle->setValue(31);
    apply->click();
    settleEvents();
    controls.checkMatches(fixture);
    const auto lower = fixture.geometry(fixture.lowerId);
    const auto bytes = rasterBytes(*fixture.activeLayer());
    const auto surfaceRevision = fixture.surfaceRevision();
    const auto& surface = std::get<core::RasterLayer>(fixture.activeLayer()->payload).surface;
    const auto surfaceId = surface->id();
    const auto initialValues = core::valuesFromTransform(fixture.transform(), surface->extent());
    CHECK(initialValues.has_value());
    if (!initialValues) return;
    const auto depth = fixture.window.editorSession().history().undoDepth();
    std::vector<core::AffineTransform> states {fixture.transform()};
    const auto record = [&] {
        settleEvents();
        states.push_back(fixture.transform());
        CHECK(fixture.window.editorSession().history().undoDepth() == depth + states.size() - 1);
        CHECK(sameTransform(fixture.geometry(fixture.lowerId), lower));
        CHECK(fixture.window.editorSession().activeLayer() == fixture.upperId);
        CHECK(fixture.surfaceRevision() == surfaceRevision);
        CHECK(surface->id() == surfaceId);
        CHECK(rasterBytes(*fixture.activeLayer()) == bytes);
        const auto values = core::valuesFromTransform(fixture.transform(), surface->extent());
        CHECK(values.has_value());
        if (values) {
            CHECK(near(std::abs(values->scaleX), std::abs(initialValues->scaleX)));
            CHECK(near(std::abs(values->scaleY), std::abs(initialValues->scaleY)));
            CHECK(near(values->shear, initialValues->shear));
        }
        controls.checkMatches(fixture);
    };
    controls.x->setValue(controls.x->value() + 12.25);
    record();
    controls.y->setValue(controls.y->value() - 7.5);
    record();
    controls.angle->setValue(37);
    record();
    const auto contentBounds=core::layerInteractionBounds(*fixture.activeLayer());
    const core::Extent2d contentSize{contentBounds.width,contentBounds.height};
    const core::AffineTransform toFrame{1,0,contentBounds.x,0,1,contentBounds.y};
    const core::AffineTransform fromFrame{1,0,-contentBounds.x,0,1,-contentBounds.y};
    const auto beforeFlip = core::geometryValuesFromTransform(core::composeTransform(fixture.transform(),toFrame),contentSize);
    CHECK(beforeFlip.has_value());
    if (!beforeFlip) return;
    auto expectedFlip = *beforeFlip;
    expectedFlip.scaleX *= -1;
    controls.flipH->click();
    record();
    CHECK(sameTransform(fixture.transform(), core::composeTransform(core::transformFromGeometryValues(expectedFlip,contentSize),fromFrame)));
    const auto beforeVerticalFlip = core::geometryValuesFromTransform(core::composeTransform(fixture.transform(),toFrame),contentSize);
    CHECK(beforeVerticalFlip.has_value());
    if (!beforeVerticalFlip) return;
    expectedFlip = *beforeVerticalFlip;
    expectedFlip.scaleY *= -1;
    controls.flipV->click();
    record();
    CHECK(sameTransform(fixture.transform(), core::composeTransform(core::transformFromGeometryValues(expectedFlip,contentSize),fromFrame)));

    for (std::size_t i = states.size() - 1; i > 0; --i) {
        undo->trigger();
        settleEvents();
        CHECK(sameTransform(fixture.transform(), states[i - 1]));
        CHECK(fixture.window.editorSession().history().undoDepth() == depth + i - 1);
        controls.checkMatches(fixture);
    }
    const auto redoDepth = fixture.window.editorSession().history().redoDepth();
    CHECK(redoDepth == states.size() - 1);
    // Equivalent angles are a geometry no-op even when the displayed number
    // changes before canonicalization; neither those nor the targeting toggle
    // are allowed to destroy a usable redo branch.
    controls.angle->setValue(controls.angle->value() + 360);
    controls.activeOnly->click();
    controls.underMouse->click();
    CHECK(fixture.window.editorSession().history().undoDepth() == depth);
    CHECK(fixture.window.editorSession().history().redoDepth() == redoDepth);
    for (std::size_t i = 1; i < states.size(); ++i) {
        redo->trigger();
        settleEvents();
        CHECK(sameTransform(fixture.transform(), states[i]));
        controls.checkMatches(fixture);
    }
    undo->trigger();
    settleEvents();
    controls.y->setValue(controls.y->value() + 3);
    CHECK(fixture.window.editorSession().history().redoDepth() == 0);
    CHECK(fixture.surfaceRevision() == surfaceRevision);
    CHECK(rasterBytes(*fixture.activeLayer()) == bytes);
}

void moveNumericTypingAndHeldStepsCommitOnlyAtActionBoundaries()
{
    Fixture fixture;
    CHECK(fixture.valid());
    if (!fixture.valid()) return;
    MoveControls controls(fixture);
    auto* undo = actionWithShortcut(fixture.window, QKeySequence::Undo);
    auto* redo = actionWithShortcut(fixture.window, QKeySequence::Redo);
    CHECK(controls.valid() && undo && redo);
    if (!controls.valid() || !undo || !redo) return;
    const auto depth = fixture.window.editorSession().history().undoDepth();
    const auto original = fixture.transform();
    const auto desired = controls.x->value() + 16.25;
    controls.x->setFocus();
    controls.x->selectAll();
    settleEvents();
    for (const auto key : {Qt::Key_B, Qt::Key_E, Qt::Key_I, Qt::Key_V}) {
        QTest::keyClick(controls.x, key);
        CHECK(fixture.window.editorSession().activeTool() == core::ToolId::Move);
    }
    controls.x->selectAll();
    QTest::keyClicks(controls.x, QString::number(desired, 'f', 2));
    CHECK(sameTransform(fixture.transform(), original));
    CHECK(fixture.window.editorSession().history().undoDepth() == depth);
    QTest::keyClick(controls.x, Qt::Key_Return);
    settleEvents();
    const auto typed = fixture.transform();
    CHECK(near(controls.x->value(), desired, 0.011));
    CHECK(fixture.window.editorSession().history().undoDepth() == depth + 1);

    controls.x->setFocus();
    settleEvents();
    QTest::keyPress(controls.x, Qt::Key_Up);
    CHECK(QApplication::focusWidget() == controls.x
        || controls.x->isAncestorOf(QApplication::focusWidget()));
    for (int i = 0; i < 8; ++i) {
        QKeyEvent repeat(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier, QString(), true, 1);
        QCoreApplication::sendEvent(controls.x, &repeat);
        CHECK(fixture.window.editorSession().history().undoDepth() == depth + 1);
    }
    CHECK(!sameTransform(fixture.transform(), typed));
    QTest::keyRelease(controls.x, Qt::Key_Up);
    settleEvents();
    controls.x->clearFocus();
    CHECK(fixture.window.editorSession().history().undoDepth() == depth + 2);
    const auto stepped = fixture.transform();
    undo->trigger();
    settleEvents();
    CHECK(sameTransform(fixture.transform(), typed));
    redo->trigger();
    settleEvents();
    CHECK(sameTransform(fixture.transform(), stepped));

    // Hold the native spin button through its repeat delay. The exact count
    // is style/timer-dependent; one history command per physical hold is not.
    const QPointF up(controls.x->width() - 8, 7);
    sendMouse(*controls.x, QEvent::MouseButtonPress, up, Qt::LeftButton, Qt::LeftButton);
    QTest::qWait(650);
    CHECK(!sameTransform(fixture.transform(), stepped));
    CHECK(fixture.window.editorSession().history().undoDepth() == depth + 2);
    sendMouse(*controls.x, QEvent::MouseButtonRelease, up, Qt::LeftButton, Qt::NoButton);
    settleEvents();
    controls.x->clearFocus();
    CHECK(fixture.window.editorSession().history().undoDepth() == depth + 3);
    undo->trigger();
    settleEvents();
    CHECK(sameTransform(fixture.transform(), stepped));
    undo->trigger();
    settleEvents();
    CHECK(sameTransform(fixture.transform(), typed));
    undo->trigger();
    settleEvents();
    CHECK(sameTransform(fixture.transform(), original));
    controls.checkMatches(fixture);
}

void moveCanvasSelectionFlushesTextToItsOriginalLayerBeforeTheDrag()
{
    RasterFixture fixture;
    CHECK(fixture.valid());
    if (!fixture.valid()) return;
    MoveControls controls(fixture);
    auto* undo = actionWithShortcut(fixture.window, QKeySequence::Undo);
    auto* redo = actionWithShortcut(fixture.window, QKeySequence::Redo);
    CHECK(controls.valid() && undo && redo);
    if (!controls.valid() || !undo || !redo) return;
    const auto upperOriginal = fixture.geometry(fixture.upperId);
    const auto lowerOriginal = fixture.geometry(fixture.lowerId);
    const auto depth = fixture.window.editorSession().history().undoDepth();
    const auto desiredX = controls.x->value() + 11;
    controls.x->setFocus();
    controls.x->selectAll();
    QTest::keyClicks(controls.x, QString::number(desiredX, 'f', 2));
    settleEvents();
    CHECK(sameTransform(fixture.geometry(fixture.upperId), upperOriginal));
    const auto from = logicalPoint(fixture, {32, 40});
    const auto to = logicalPoint(fixture, {40, 50});
    sendMouse(*fixture.canvas, QEvent::MouseButtonPress, from,
        Qt::LeftButton, Qt::LeftButton);
    settleEvents();
    CHECK(fixture.canvas->transformDragging());
    CHECK(fixture.window.editorSession().activeLayer() == fixture.lowerId);
    CHECK(near(fixture.geometry(fixture.upperId).m02, upperOriginal.m02 + 11));
    CHECK(fixture.window.editorSession().history().undoDepth() == depth + 1);
    CHECK(!controls.x->hasFocus());
    CHECK(!QApplication::focusWidget() || !controls.x->isAncestorOf(QApplication::focusWidget()));
    controls.checkMatches(fixture);
    sendMouse(*fixture.canvas, QEvent::MouseMove, to,
        Qt::NoButton, Qt::LeftButton);
    sendMouse(*fixture.canvas, QEvent::MouseButtonRelease, to,
        Qt::LeftButton, Qt::NoButton);
    settleEvents();
    const auto lowerMoved = fixture.geometry(fixture.lowerId);
    CHECK(near(lowerMoved.m02, lowerOriginal.m02 + 8));
    CHECK(near(lowerMoved.m12, lowerOriginal.m12 + 10));
    CHECK(fixture.window.editorSession().history().undoDepth() == depth + 2);
    undo->trigger();
    settleEvents();
    CHECK(sameTransform(fixture.geometry(fixture.lowerId), lowerOriginal));
    CHECK(near(fixture.geometry(fixture.upperId).m02, upperOriginal.m02 + 11));
    undo->trigger();
    settleEvents();
    CHECK(sameTransform(fixture.geometry(fixture.upperId), upperOriginal));
    CHECK(fixture.window.editorSession().activeLayer() == fixture.lowerId);
    controls.checkMatches(fixture);
    redo->trigger();
    redo->trigger();
    settleEvents();
    CHECK(sameTransform(fixture.geometry(fixture.lowerId), lowerMoved));
    CHECK(near(fixture.geometry(fixture.upperId).m02, upperOriginal.m02 + 11));
    controls.checkMatches(fixture);
}

void moveOptionsDoNotClaimTypingFocusBeforeTheUserEditsAField()
{
    Fixture fixture;
    CHECK(fixture.valid());
    if (!fixture.valid()) return;
    MoveControls controls(fixture);
    CHECK(controls.valid());
    if (!controls.valid()) return;
    auto* focus = QApplication::focusWidget();
    const bool numericFocus = focus == controls.x || focus == controls.y || focus == controls.angle
        || (focus && (controls.x->isAncestorOf(focus) || controls.y->isAncestorOf(focus)
            || controls.angle->isAncestorOf(focus)));
    if (numericFocus)
        std::cerr << "Unexpected initial Move numeric focus: " << focus->metaObject()->className()
                  << " " << focus->objectName().toStdString() << '\n';
    CHECK(!numericFocus);
    QTest::keyClick(focus ? focus : &fixture.window, Qt::Key_B);
    settleEvents();
    CHECK(fixture.window.editorSession().activeTool() == core::ToolId::Brush);
    auto* move = fixture.window.findChild<QAction*>(QStringLiteral("ToolAction_move"));
    CHECK(move);
    if (!move) return;
    move->trigger();
    settleEvents();
    focus = QApplication::focusWidget();
    QTest::keyClick(focus ? focus : &fixture.window, Qt::Key_T, Qt::ControlModifier);
    settleEvents();
    CHECK(activeTransform(fixture));
    if (activeTransform(fixture)) {
        QTest::keyClick(fixture.canvas, Qt::Key_Escape);
        settleEvents();
    }
}

void movePendingTextFinishesBeforeToolbarAndLayerListTargetChanges()
{
    RasterFixture fixture;
    CHECK(fixture.valid());
    if (!fixture.valid()) return;
    MoveControls controls(fixture);
    auto* rail = fixture.window.findChild<QToolBar*>(QStringLiteral("ToolRail"));
    auto* brush = fixture.window.findChild<QAction*>(QStringLiteral("ToolAction_brush"));
    auto* move = fixture.window.findChild<QAction*>(QStringLiteral("ToolAction_move"));
    auto* layers = fixture.window.findChild<QListView*>(QStringLiteral("LayerList"));
    CHECK(controls.valid() && rail && brush && move && layers);
    if (!controls.valid() || !rail || !brush || !move || !layers) return;
    auto* brushButton = rail->widgetForAction(brush);
    CHECK(brushButton);
    if (!brushButton) return;
    const auto depth = fixture.window.editorSession().history().undoDepth();
    const auto upperOriginal = fixture.geometry(fixture.upperId);
    const auto lowerOriginal = fixture.geometry(fixture.lowerId);
    controls.x->setFocus();
    controls.x->selectAll();
    QTest::keyClicks(controls.x, QString::number(controls.x->value() + 13));
    settleEvents();
    QTest::mouseClick(brushButton, Qt::LeftButton);
    settleEvents();
    CHECK(fixture.window.editorSession().activeTool() == core::ToolId::Brush);
    CHECK(near(fixture.geometry(fixture.upperId).m02, upperOriginal.m02 + 13));
    CHECK(sameTransform(fixture.geometry(fixture.lowerId), lowerOriginal));
    CHECK(fixture.window.editorSession().history().undoDepth() == depth + 1);
    const auto afterToolChange = fixture.geometry(fixture.upperId);
    controls.x->clearFocus();
    settleEvents();
    CHECK(sameTransform(fixture.geometry(fixture.upperId), afterToolChange));

    move->trigger();
    settleEvents();
    controls.checkMatches(fixture);
    const auto oldY = controls.y->value();
    controls.y->setFocus();
    controls.y->selectAll();
    QTest::keyClicks(controls.y, QString::number(oldY + 17));
    settleEvents();
    const auto lowerRow = layers->model()->index(1, 0);
    CHECK(lowerRow.isValid());
    const auto lowerItem = layers->visualRect(lowerRow);
    CHECK(!lowerItem.isEmpty());
    QTest::mouseClick(layers->viewport(), Qt::LeftButton, Qt::NoModifier,
        QPoint(lowerItem.center().x(), lowerItem.center().y()));
    settleEvents();
    CHECK(fixture.window.editorSession().activeLayer() == fixture.lowerId);
    CHECK(near(fixture.geometry(fixture.upperId).m12, upperOriginal.m12 + 17));
    CHECK(sameTransform(fixture.geometry(fixture.lowerId), lowerOriginal));
    CHECK(fixture.window.editorSession().history().undoDepth() == depth + 2);
    const auto finalUpper = fixture.geometry(fixture.upperId);
    controls.y->clearFocus();
    settleEvents();
    CHECK(sameTransform(fixture.geometry(fixture.upperId), finalUpper));
    CHECK(sameTransform(fixture.geometry(fixture.lowerId), lowerOriginal));
    controls.checkMatches(fixture);
}

void ordinaryMoveSelectsRenderedLayerAndCreatesOnlyOneDragCommand()
{
    RasterFixture fixture;
    CHECK(fixture.valid());
    if (!fixture.valid()) return;
    auto* undo = actionWithShortcut(fixture.window, QKeySequence::Undo);
    auto* redo = actionWithShortcut(fixture.window, QKeySequence::Redo);
    auto* move = fixture.window.findChild<QAction*>(QStringLiteral("ToolAction_move"));
    auto* lock = fixture.window.findChild<QAbstractButton*>(QStringLiteral("MoveActiveLayerOnly"));
    auto* underMouse = fixture.window.findChild<QAbstractButton*>(QStringLiteral("MoveSelectUnderMouse"));
    CHECK(undo && redo && move && lock && underMouse);
    if (!undo || !redo || !move || !lock || !underMouse) return;
    move->trigger();
    settleEvents();
    CHECK(!lock->isChecked());
    CHECK(underMouse->isChecked());
    CHECK(lock->isVisible());
    CHECK(!fixture.canvas->scene().transformOverlay);
    const auto original = fixture.geometry(fixture.upperId);
    const auto lowerOriginal = fixture.geometry(fixture.lowerId);
    const auto depth = fixture.window.editorSession().history().undoDepth();
    const auto bytes = rasterBytes(*fixture.activeLayer());
    const auto revision = fixture.surfaceRevision();

    // Transparent pixels of the top layer must not steal selection from the
    // opaque layer below. A selection-only click never creates history.
    dragOnCanvas(*fixture.canvas, logicalPoint(fixture, {32, 40}),
        logicalPoint(fixture, {32, 40}));
    CHECK(fixture.window.editorSession().activeLayer() == fixture.lowerId);
    CHECK(fixture.window.editorSession().history().undoDepth() == depth);
    dragOnCanvas(*fixture.canvas, logicalPoint(fixture, {96, 40}),
        logicalPoint(fixture, {96, 40}));
    CHECK(fixture.window.editorSession().activeLayer() == fixture.upperId);
    CHECK(fixture.window.editorSession().history().undoDepth() == depth);

    const auto from = logicalPoint(fixture, {96, 40});
    const auto to = logicalPoint(fixture, {119, 57});
    sendMouse(*fixture.canvas, QEvent::MouseButtonPress, from,
        Qt::LeftButton, Qt::LeftButton);
    for (int i = 1; i <= 12; ++i) {
        sendMouse(*fixture.canvas, QEvent::MouseMove,
            from + (to - from) * (static_cast<double>(i) / 12.0),
            Qt::NoButton, Qt::LeftButton);
        CHECK(fixture.window.editorSession().history().undoDepth() == depth);
    }
    sendMouse(*fixture.canvas, QEvent::MouseButtonRelease, to,
        Qt::LeftButton, Qt::NoButton);
    settleEvents();
    const auto moved = fixture.geometry(fixture.upperId);
    CHECK(near(moved.m02, original.m02 + 23));
    CHECK(near(moved.m12, original.m12 + 17));
    CHECK(sameTransform(fixture.geometry(fixture.lowerId), lowerOriginal));
    CHECK(fixture.window.editorSession().history().undoDepth() == depth + 1);
    CHECK(fixture.window.editorSession().activeTool() == core::ToolId::Move);
    CHECK(!fixture.canvas->scene().transformOverlay);
    CHECK(fixture.surfaceRevision() == revision);
    CHECK(rasterBytes(*fixture.activeLayer()) == bytes);

    undo->trigger();
    settleEvents();
    CHECK(sameTransform(fixture.geometry(fixture.upperId), original));
    const auto redoDepth = fixture.window.editorSession().history().redoDepth();
    dragOnCanvas(*fixture.canvas, logicalPoint(fixture, {32, 40}),
        logicalPoint(fixture, {32, 40}));
    CHECK(fixture.window.editorSession().activeLayer() == fixture.lowerId);
    CHECK(fixture.window.editorSession().history().redoDepth() == redoDepth);
    dragOnCanvas(*fixture.canvas, logicalPoint(fixture, {96, 40}),
        logicalPoint(fixture, {96, 40}));
    CHECK(fixture.window.editorSession().activeLayer() == fixture.upperId);
    CHECK(fixture.window.editorSession().history().redoDepth() == redoDepth);
    redo->trigger();
    settleEvents();
    CHECK(sameTransform(fixture.geometry(fixture.upperId), moved));

    // Locking selection keeps the top layer active while dragging over the
    // lower layer; the ordinary Move operation has no transform overlay.
    lock->click();
    CHECK(lock->isChecked());
    CHECK(!underMouse->isChecked());
    dragOnCanvas(*fixture.canvas, logicalPoint(fixture, {15, 150}),
        logicalPoint(fixture, {25, 156}));
    CHECK(fixture.window.editorSession().activeLayer() == fixture.upperId);
    CHECK(near(fixture.geometry(fixture.upperId).m02, moved.m02 + 10));
    CHECK(near(fixture.geometry(fixture.upperId).m12, moved.m12 + 6));
    CHECK(sameTransform(fixture.geometry(fixture.lowerId), lowerOriginal));
    CHECK(fixture.window.editorSession().history().undoDepth() == depth + 2);
    // Modes are exclusive, not independently switchable check boxes. Clicking
    // the selected one cannot leave the Move tool without a selection policy.
    lock->click();
    CHECK(lock->isChecked() && !underMouse->isChecked());
    underMouse->click();
    CHECK(!lock->isChecked() && underMouse->isChecked());
    CHECK(fixture.window.editorSession().history().undoDepth() == depth + 2);
}

void ordinaryMoveRecoversOffCanvasLayersAndRetainsGroupSelection()
{
    RasterFixture f;
    CHECK(f.valid()); if (!f.valid()) return;
    auto& session = const_cast<core::EditorSession&>(f.window.editorSession());
    auto& document = *session.document();
    auto* move = f.window.findChild<QAction*>(QStringLiteral("ToolAction_move"));
    auto* underMouse = f.window.findChild<QAbstractButton*>(QStringLiteral("MoveSelectUnderMouse"));
    auto* activeOnly = f.window.findChild<QAbstractButton*>(QStringLiteral("MoveActiveLayerOnly"));
    auto* undo = actionWithShortcut(f.window, QKeySequence::Undo);
    auto* redo = actionWithShortcut(f.window, QKeySequence::Redo);
    CHECK(move && underMouse && activeOnly && undo && redo);
    if (!move || !underMouse || !activeOnly || !undo || !redo) return;
    move->trigger(); underMouse->click(); settleEvents();
    const auto bytes = rasterBytes(*document.layer(f.upperId));
    const auto lower = f.geometry(f.lowerId);
    for (const auto offset : {core::Vec2d {-140, 0}, core::Vec2d {0, -110},
             core::Vec2d {270, 0}, core::Vec2d {0, 210}}) {
        const core::AffineTransform original {.m02 = offset.x, .m12 = offset.y};
        CHECK(document.setLayerTransform(f.upperId, original));
        clickLayerRow(f, f.lowerId);
        const auto depth = session.history().undoDepth();
        const core::Vec2d point {offset.x + 96, offset.y + 40};
        dragOnCanvas(*f.canvas, logicalPoint(f, point), logicalPoint(f, {96, 40}));
        CHECK(session.activeLayer() == f.upperId);
        CHECK(near(f.geometry(f.upperId).m02, 0));
        CHECK(near(f.geometry(f.upperId).m12, 0));
        CHECK(sameTransform(f.geometry(f.lowerId), lower));
        CHECK(session.history().undoDepth() == depth + 1);
        CHECK(rasterBytes(*document.layer(f.upperId)) == bytes);
        undo->trigger(); settleEvents();
        CHECK(sameTransform(f.geometry(f.upperId), original));
        redo->trigger(); settleEvents();
        CHECK(near(f.geometry(f.upperId).m02, 0));
        CHECK(near(f.geometry(f.upperId).m12, 0));
    }
    CHECK(document.setLayerTransform(f.upperId, {.m02 = -140}));
    clickLayerRow(f, f.lowerId);
    clickLayerRow(f, f.upperId, Qt::ControlModifier);
    CHECK(session.selectedLayers().size() == 2);
    dragOnCanvas(*f.canvas, logicalPoint(f, {-44, 40}), logicalPoint(f, {-24, 50}));
    CHECK(session.selectedLayers().size() == 2);
    CHECK(near(f.geometry(f.upperId).m02, -120));
    CHECK(near(f.geometry(f.lowerId).m02, lower.m02 + 20));
    CHECK(near(f.geometry(f.lowerId).m12, lower.m12 + 10));

    // A panel-selected layer can also be recovered from empty pasteboard in
    // Active Layer Only mode; ordinary auto-picking still needs actual pixels.
    clickLayerRow(f, f.upperId);
    activeOnly->click();
    const auto original = f.geometry(f.upperId);
    dragOnCanvas(*f.canvas, logicalPoint(f, {-60, -40}), logicalPoint(f, {-50, -32}));
    CHECK(near(f.geometry(f.upperId).m02, original.m02 + 10));
    CHECK(near(f.geometry(f.upperId).m12, original.m12 + 8));
    CHECK(rasterBytes(*document.layer(f.upperId)) == bytes);
    underMouse->click();
}

void spacePanRoutesFromWidgetKeyRecipientsWithoutEditingTheLayer()
{
    RasterFixture fixture;
    CHECK(fixture.valid());
    if (!fixture.valid()) return;
    const auto original = fixture.transform();
    const auto bytes = rasterBytes(*fixture.activeLayer());
    const auto revision = fixture.surfaceRevision();
    const auto depth = fixture.window.editorSession().history().undoDepth();
    const auto active = fixture.window.editorSession().activeLayer();
    auto* panel = fixture.window.findChild<QWidget*>(QStringLiteral("LayersPanel"));
    CHECK(panel);
    if (!panel) return;

    // The native canvas frequently is not the keyboard focus recipient:
    // application/overlay QWidget delivery must arm the very same pan path.
    for (auto* recipient : {static_cast<QWidget*>(&fixture.window), fixture.workspace->panelOverlay(), panel}) {
        if (auto* focus = QApplication::focusWidget()) focus->clearFocus();
        const auto start = logicalPoint(fixture, {96, 40});
        sendMouse(*fixture.canvas, QEvent::MouseMove, start, Qt::NoButton, Qt::NoButton);
        const auto before = fixture.canvas->scene().viewport.pan();
        QKeyEvent override(QEvent::ShortcutOverride, Qt::Key_Space, Qt::NoModifier);
        override.setAccepted(false);
        QCoreApplication::sendEvent(recipient, &override);
        CHECK(override.isAccepted());
        QTest::keyPress(recipient, Qt::Key_Space);
        CHECK(fixture.canvas->cursor().shape() == Qt::OpenHandCursor);
        QKeyEvent repeatRelease(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier,
            QStringLiteral(" "), true, 1);
        QCoreApplication::sendEvent(recipient, &repeatRelease);
        QKeyEvent repeatPress(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier,
            QStringLiteral(" "), true, 1);
        QCoreApplication::sendEvent(recipient, &repeatPress);
        CHECK(fixture.canvas->cursor().shape() == Qt::OpenHandCursor);
        // Merely holding Space and moving the pointer must not pan.
        sendMouse(*fixture.canvas, QEvent::MouseMove, start + QPointF(3, 4), Qt::NoButton, Qt::NoButton);
        CHECK(fixture.canvas->scene().viewport.pan() == before);
        sendMouse(*fixture.canvas, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
        CHECK(fixture.canvas->cursor().shape() == Qt::ClosedHandCursor);
        sendMouse(*fixture.canvas, QEvent::MouseMove, start + QPointF(37, 21), Qt::NoButton, Qt::LeftButton);
        CHECK(near(fixture.canvas->scene().viewport.pan().x, before.x + 37));
        CHECK(near(fixture.canvas->scene().viewport.pan().y, before.y + 21));
        sendMouse(*fixture.canvas, QEvent::MouseButtonRelease, start + QPointF(37, 21),
            Qt::LeftButton, Qt::NoButton);
        CHECK(fixture.canvas->cursor().shape() == Qt::OpenHandCursor);
        QTest::keyRelease(recipient, Qt::Key_Space);
        CHECK(fixture.canvas->cursor().shape() != Qt::OpenHandCursor);
        CHECK(fixture.canvas->cursor().shape() != Qt::ClosedHandCursor);
        CHECK(fixture.pointerRouter->captureDomain() == ui::CrossWindowPointerRouter::CaptureDomain::None);
        CHECK(sameTransform(fixture.transform(), original));
        CHECK(fixture.window.editorSession().activeLayer() == active);
        CHECK(fixture.window.editorSession().history().undoDepth() == depth);
    }

    // The established cross-window capture also owns Space+left drags until
    // release even if the pointer travels back over a QWidget panel.
    sendMouse(*fixture.canvas, QEvent::MouseMove, {120, 130}, Qt::NoButton, Qt::NoButton);
    QTest::keyPress(&fixture.window, Qt::Key_Space);
    const auto start = logicalPoint(fixture, {96, 40});
    const auto before = fixture.canvas->scene().viewport.pan();
    sendMouse(*fixture.canvas, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    const QPointF panelPoint(panel->rect().center());
    const QPointF canvasPoint = fixture.canvas->mapFromGlobal(panel->mapToGlobal(panelPoint.toPoint()));
    sendMouse(*panel, QEvent::MouseMove, panelPoint, Qt::NoButton, Qt::LeftButton);
    sendMouse(*panel, QEvent::MouseButtonRelease, panelPoint, Qt::LeftButton, Qt::NoButton);
    CHECK(near(fixture.canvas->scene().viewport.pan().x, before.x + canvasPoint.x() - start.x(), 1.1));
    CHECK(near(fixture.canvas->scene().viewport.pan().y, before.y + canvasPoint.y() - start.y(), 1.1));
    QTest::keyRelease(panel, Qt::Key_Space);
    CHECK(fixture.pointerRouter->captureDomain() == ui::CrossWindowPointerRouter::CaptureDomain::None);

    // Releasing the temporary key first must not hand an in-flight left drag
    // to Move: capture and navigation stay latched until that button releases.
    sendMouse(*fixture.canvas, QEvent::MouseMove, {120, 130}, Qt::NoButton, Qt::NoButton);
    QTest::keyPress(&fixture.window, Qt::Key_Space);
    sendMouse(*fixture.canvas, QEvent::MouseButtonPress, {120, 130}, Qt::LeftButton, Qt::LeftButton);
    QTest::keyRelease(&fixture.window, Qt::Key_Space);
    CHECK(fixture.canvas->cursor().shape() == Qt::ClosedHandCursor);
    const auto latchedBefore = fixture.canvas->scene().viewport.pan();
    sendMouse(*fixture.canvas, QEvent::MouseMove, {151, 153}, Qt::NoButton, Qt::LeftButton);
    CHECK(near(fixture.canvas->scene().viewport.pan().x, latchedBefore.x + 31));
    CHECK(near(fixture.canvas->scene().viewport.pan().y, latchedBefore.y + 23));
    sendMouse(*fixture.canvas, QEvent::MouseButtonRelease, {151, 153}, Qt::LeftButton, Qt::NoButton);
    CHECK(fixture.canvas->cursor().shape() != Qt::OpenHandCursor);
    CHECK(fixture.canvas->cursor().shape() != Qt::ClosedHandCursor);

    // Middle-button navigation stays independent of the temporary Space mode.
    const auto middleBefore = fixture.canvas->scene().viewport.pan();
    sendMouse(*fixture.canvas, QEvent::MouseButtonPress, {120, 130}, Qt::MiddleButton, Qt::MiddleButton);
    CHECK(fixture.canvas->cursor().shape() == Qt::ClosedHandCursor);
    sendMouse(*fixture.canvas, QEvent::MouseMove, {141, 149}, Qt::NoButton, Qt::MiddleButton);
    sendMouse(*fixture.canvas, QEvent::MouseButtonRelease, {141, 149}, Qt::MiddleButton, Qt::NoButton);
    CHECK(near(fixture.canvas->scene().viewport.pan().x, middleBefore.x + 21));
    CHECK(near(fixture.canvas->scene().viewport.pan().y, middleBefore.y + 19));
    CHECK(fixture.canvas->cursor().shape() != Qt::ClosedHandCursor);
    CHECK(sameTransform(fixture.transform(), original));
    CHECK(fixture.surfaceRevision() == revision);
    CHECK(rasterBytes(*fixture.activeLayer()) == bytes);
    CHECK(fixture.window.editorSession().history().undoDepth() == depth);
}

void spacePanRespectsTextFocusAndClearsOnLostFocusOrDeactivation()
{
    RasterFixture fixture;
    CHECK(fixture.valid());
    if (!fixture.valid()) return;
    QLineEdit editor(&fixture.window);
    editor.setGeometry(60, 90, 140, 30);
    editor.show();
    editor.setFocus();
    settleEvents();
    CHECK(editor.hasFocus());
    editor.setText(QStringLiteral("one"));
    editor.setCursorPosition(3);
    QTest::keyClick(&editor, Qt::Key_Space);
    CHECK(editor.text() == QStringLiteral("one "));
    CHECK(fixture.canvas->cursor().shape() != Qt::OpenHandCursor);
    editor.clearFocus();
    editor.hide();
    QPushButton standardButton(QStringLiteral("Keyboard activation"), &fixture.window);
    standardButton.setGeometry(60, 90, 170, 30);
    standardButton.show();
    standardButton.setFocus();
    settleEvents();
    CHECK(standardButton.hasFocus());
    int buttonClicks = 0;
    QObject::connect(&standardButton, &QPushButton::clicked, [&] { ++buttonClicks; });
    sendMouse(*fixture.canvas, QEvent::MouseMove, {120, 130}, Qt::NoButton, Qt::NoButton);
    QTest::keyClick(&standardButton, Qt::Key_Space);
    CHECK(buttonClicks == 1);
    CHECK(fixture.canvas->cursor().shape() != Qt::OpenHandCursor);
    standardButton.clearFocus();
    standardButton.hide();
    const auto depth = fixture.window.editorSession().history().undoDepth();
    const auto original = fixture.transform();

    sendMouse(*fixture.canvas, QEvent::MouseMove, {120, 130}, Qt::NoButton, Qt::NoButton);
    for (const auto modifiers : {Qt::ControlModifier, Qt::AltModifier}) {
        QTest::keyPress(&fixture.window, Qt::Key_Space, modifiers);
        CHECK(fixture.canvas->cursor().shape() != Qt::OpenHandCursor);
        QTest::keyRelease(&fixture.window, Qt::Key_Space, modifiers);
    }

    for (const auto termination : {QEvent::FocusOut, QEvent::ApplicationDeactivate, QEvent::WindowDeactivate}) {
        sendMouse(*fixture.canvas, QEvent::MouseMove, {120, 130}, Qt::NoButton, Qt::NoButton);
        QTest::keyPress(&fixture.window, Qt::Key_Space);
        CHECK(fixture.canvas->cursor().shape() == Qt::OpenHandCursor);
        sendMouse(*fixture.canvas, QEvent::MouseButtonPress, {120, 130}, Qt::LeftButton, Qt::LeftButton);
        sendMouse(*fixture.canvas, QEvent::MouseMove, {145, 151}, Qt::NoButton, Qt::LeftButton);
        CHECK(fixture.canvas->cursor().shape() == Qt::ClosedHandCursor);
        const auto stopped = fixture.canvas->scene().viewport.pan();
        if (termination == QEvent::FocusOut) {
            QFocusEvent loss(QEvent::FocusOut, Qt::OtherFocusReason);
            QCoreApplication::sendEvent(fixture.canvas, &loss);
        } else {
            QEvent loss(termination);
            QCoreApplication::sendEvent(&fixture.window, &loss);
        }
        CHECK(fixture.canvas->cursor().shape() != Qt::OpenHandCursor);
        CHECK(fixture.canvas->cursor().shape() != Qt::ClosedHandCursor);
        sendMouse(*fixture.canvas, QEvent::MouseMove, {178, 180}, Qt::NoButton, Qt::LeftButton);
        CHECK(fixture.canvas->scene().viewport.pan() == stopped);
        sendMouse(*fixture.canvas, QEvent::MouseButtonRelease, {178, 180}, Qt::LeftButton, Qt::NoButton);
        QTest::keyRelease(&fixture.window, Qt::Key_Space);
        CHECK(fixture.pointerRouter->captureDomain() == ui::CrossWindowPointerRouter::CaptureDomain::None);
        CHECK(sameTransform(fixture.transform(), original));
        CHECK(fixture.window.editorSession().history().undoDepth() == depth);
    }
}

void ordinaryMoveCancellationAndCrossPanelCapturePreserveGeometry()
{
    RasterFixture fixture;
    CHECK(fixture.valid());
    if (!fixture.valid()) return;
    auto* panel = fixture.window.findChild<QWidget*>(QStringLiteral("LayersPanel"));
    CHECK(panel);
    if (!panel) return;
    const auto original = fixture.transform();
    const auto depth = fixture.window.editorSession().history().undoDepth();
    const auto press = logicalPoint(fixture, {96, 40});
    const auto moved = press + QPointF(50, 30);
    sendMouse(*fixture.canvas, QEvent::MouseButtonPress, press,
        Qt::LeftButton, Qt::LeftButton);
    sendMouse(*fixture.canvas, QEvent::MouseMove, moved,
        Qt::NoButton, Qt::LeftButton);
    CHECK(!sameTransform(fixture.transform(), original));
    QTest::keyClick(fixture.canvas, Qt::Key_Escape);
    settleEvents();
    CHECK(sameTransform(fixture.transform(), original));
    CHECK(!fixture.canvas->transformDragging());
    CHECK(fixture.pointerRouter->captureDomain()
        == ui::CrossWindowPointerRouter::CaptureDomain::None);
    sendMouse(*fixture.canvas, QEvent::MouseButtonRelease, moved,
        Qt::LeftButton, Qt::NoButton);
    CHECK(fixture.window.editorSession().history().undoDepth() == depth);

    sendMouse(*fixture.canvas, QEvent::MouseButtonPress, press,
        Qt::LeftButton, Qt::LeftButton);
    sendMouse(*fixture.canvas, QEvent::MouseMove, moved,
        Qt::NoButton, Qt::LeftButton);
    QFocusEvent focusLoss(QEvent::FocusOut, Qt::OtherFocusReason);
    QCoreApplication::sendEvent(fixture.canvas, &focusLoss);
    settleEvents();
    CHECK(sameTransform(fixture.transform(), original));
    CHECK(!fixture.canvas->transformDragging());
    sendMouse(*fixture.canvas, QEvent::MouseButtonRelease, moved,
        Qt::LeftButton, Qt::NoButton);
    CHECK(fixture.window.editorSession().history().undoDepth() == depth);

    sendMouse(*fixture.canvas, QEvent::MouseButtonPress, press,
        Qt::LeftButton, Qt::LeftButton);
    const QPointF panelPoint(panel->rect().center());
    sendMouse(*panel, QEvent::MouseMove, panelPoint,
        Qt::NoButton, Qt::LeftButton);
    sendMouse(*panel, QEvent::MouseButtonRelease, panelPoint,
        Qt::LeftButton, Qt::NoButton);
    settleEvents();
    CHECK(!sameTransform(fixture.transform(), original));
    CHECK(!fixture.canvas->transformDragging());
    CHECK(fixture.pointerRouter->captureDomain()
        == ui::CrossWindowPointerRouter::CaptureDomain::None);
    CHECK(fixture.window.editorSession().history().undoDepth() == depth + 1);
    CHECK(fixture.window.editorSession().activeTool() == core::ToolId::Move);
}

void rotatedFlippedHitTestingAndTransformTargetIsolation()
{
    RasterFixture fixture;
    CHECK(fixture.valid());
    if (!fixture.valid()) return;
    auto* x = fixture.window.findChild<QDoubleSpinBox*>(QStringLiteral("TransformXControl"));
    auto* y = fixture.window.findChild<QDoubleSpinBox*>(QStringLiteral("TransformYControl"));
    auto* width = fixture.window.findChild<QDoubleSpinBox*>(QStringLiteral("TransformScaleXControl"));
    auto* height = fixture.window.findChild<QDoubleSpinBox*>(QStringLiteral("TransformScaleYControl"));
    auto* angle = fixture.window.findChild<QDoubleSpinBox*>(QStringLiteral("TransformAngleControl"));
    auto* apply = fixture.window.findChild<QToolButton*>(QStringLiteral("TransformApply"));
    CHECK(x && y && width && height && angle && apply);
    if (!x || !y || !width || !height || !angle || !apply) return;
    fixture.startWithAction();
    x->setValue(135);
    y->setValue(90);
    width->setValue(-120);
    height->setValue(65);
    angle->setValue(37);
    apply->click();
    settleEvents();
    const auto transformed = fixture.geometry(fixture.upperId);
    const auto depth = fixture.window.editorSession().history().undoDepth();
    const auto transparent = logicalPoint(fixture, transformed.map({24, 40}));
    const auto opaque = logicalPoint(fixture, transformed.map({96, 40}));
    dragOnCanvas(*fixture.canvas, transparent, transparent);
    CHECK(fixture.window.editorSession().activeLayer() == fixture.lowerId);
    dragOnCanvas(*fixture.canvas, opaque, opaque);
    CHECK(fixture.window.editorSession().activeLayer() == fixture.upperId);
    CHECK(fixture.window.editorSession().history().undoDepth() == depth);

    fixture.startWithAction();
    const auto outside = logicalPoint(fixture, {15, 170});
    dragOnCanvas(*fixture.canvas, outside, outside + QPointF(7, 3));
    CHECK(activeTransform(fixture));
    CHECK(fixture.window.editorSession().activeLayer() == fixture.upperId);
    CHECK(sameTransform(fixture.transform(), transformed));
    QTest::keyClick(fixture.canvas, Qt::Key_Escape);
    settleEvents();
}

void sessionCancelAndEmptyApplyPreservePreexistingRedoBranch()
{
    Fixture fixture;
    CHECK(fixture.valid());
    if (!fixture.valid()) return;
    auto* undo = actionWithShortcut(fixture.window, QKeySequence::Undo);
    auto* redo = actionWithShortcut(fixture.window, QKeySequence::Redo);
    auto* x = fixture.window.findChild<QDoubleSpinBox*>(QStringLiteral("TransformXControl"));
    auto* flip = fixture.window.findChild<QToolButton*>(QStringLiteral("TransformFlipHorizontal"));
    auto* apply = fixture.window.findChild<QToolButton*>(QStringLiteral("TransformApply"));
    CHECK(undo && redo && x && flip && apply);
    if (!undo || !redo || !x || !flip || !apply) return;
    const auto original = fixture.transform();
    fixture.startWithAction();
    x->setValue(x->value() + 21);
    apply->click();
    settleEvents();
    const auto previousFuture = fixture.transform();
    undo->trigger();
    settleEvents();
    CHECK(sameTransform(fixture.transform(), original));
    const auto undoDepth = fixture.window.editorSession().history().undoDepth();
    const auto redoDepth = fixture.window.editorSession().history().redoDepth();
    const auto redoLabel = std::string(fixture.window.editorSession().history().redoLabel());
    CHECK(redoDepth == 1);

    fixture.startWithAction();
    const auto center = centerOf(fixture.canvas->logicalTransformHandles());
    dragOnCanvas(*fixture.canvas, center, center + QPointF(15, 10));
    flip->click();
    undo->trigger();
    settleEvents();
    CHECK(activeTransform(fixture));
    CHECK(!sameTransform(fixture.transform(), original));
    CHECK(fixture.window.editorSession().history().undoDepth() == undoDepth);
    CHECK(fixture.window.editorSession().history().redoDepth() == redoDepth);
    QTest::keyClick(fixture.canvas, Qt::Key_Escape);
    settleEvents();
    CHECK(sameTransform(fixture.transform(), original));
    CHECK(fixture.window.editorSession().history().undoDepth() == undoDepth);
    CHECK(fixture.window.editorSession().history().redoDepth() == redoDepth);
    CHECK(fixture.window.editorSession().history().redoLabel() == redoLabel);

    // A click-only action and an entirely empty Apply are not divergent edits.
    fixture.startWithAction();
    const auto click = centerOf(fixture.canvas->logicalTransformHandles());
    dragOnCanvas(*fixture.canvas, click, click);
    apply->click();
    settleEvents();
    CHECK(fixture.window.editorSession().history().redoDepth() == redoDepth);
    fixture.startWithAction();
    apply->click();
    settleEvents();
    CHECK(fixture.window.editorSession().history().redoDepth() == redoDepth);
    redo->trigger();
    settleEvents();
    CHECK(sameTransform(fixture.transform(), previousFuture));
}

void newGestureAfterUndoUsesRestoredBaselineAndDiscardsOnlyPendingRedo()
{
    Fixture fixture;
    CHECK(fixture.valid());
    if (!fixture.valid()) return;
    auto* undo = actionWithShortcut(fixture.window, QKeySequence::Undo);
    auto* redo = actionWithShortcut(fixture.window, QKeySequence::Redo);
    auto* apply = fixture.window.findChild<QToolButton*>(QStringLiteral("TransformApply"));
    auto* x = fixture.window.findChild<QDoubleSpinBox*>(QStringLiteral("TransformXControl"));
    auto* y = fixture.window.findChild<QDoubleSpinBox*>(QStringLiteral("TransformYControl"));
    CHECK(undo && redo && apply && x && y);
    if (!undo || !redo || !apply || !x || !y) return;
    const auto original = fixture.transform();
    const auto depth = fixture.window.editorSession().history().undoDepth();
    fixture.startWithAction();
    auto center = centerOf(fixture.canvas->logicalTransformHandles());
    dragOnCanvas(*fixture.canvas, center, center + QPointF(21, 12));
    const auto first = fixture.transform();
    center = centerOf(fixture.canvas->logicalTransformHandles());
    dragOnCanvas(*fixture.canvas, center, center + QPointF(13, 17));
    undo->trigger();
    settleEvents();
    CHECK(activeTransform(fixture));
    CHECK(sameTransform(fixture.transform(), first));
    CHECK(redo->isEnabled());
    const auto* raster = std::get_if<core::RasterLayer>(&fixture.activeLayer()->payload);
    const auto values = core::valuesFromTransform(first, raster->surface->extent());
    CHECK(values.has_value());
    if (values) {
        CHECK(near(x->value(), values->center.x, 0.011));
        CHECK(near(y->value(), values->center.y, 0.011));
    }
    center = centerOf(fixture.canvas->logicalTransformHandles());
    dragOnCanvas(*fixture.canvas, center, center + QPointF(-7, 31));
    const auto divergent = fixture.transform();
    CHECK(!sameTransform(divergent, first));
    CHECK(!redo->isEnabled());
    apply->click();
    settleEvents();
    CHECK(fixture.window.editorSession().history().undoDepth() == depth + 2);
    undo->trigger();
    settleEvents();
    CHECK(sameTransform(fixture.transform(), first));
    undo->trigger();
    settleEvents();
    CHECK(sameTransform(fixture.transform(), original));
    redo->trigger();
    redo->trigger();
    settleEvents();
    CHECK(sameTransform(fixture.transform(), divergent));
}

void numericTypingAndHeldStepsEachCreateOneAction()
{
    Fixture fixture;
    CHECK(fixture.valid());
    if (!fixture.valid()) return;
    auto* x = fixture.window.findChild<QDoubleSpinBox*>(QStringLiteral("TransformXControl"));
    auto* undo = actionWithShortcut(fixture.window, QKeySequence::Undo);
    auto* redo = actionWithShortcut(fixture.window, QKeySequence::Redo);
    auto* apply = fixture.window.findChild<QToolButton*>(QStringLiteral("TransformApply"));
    CHECK(x && undo && redo && apply);
    if (!x || !undo || !redo || !apply) return;
    const auto original = fixture.transform();
    const auto depth = fixture.window.editorSession().history().undoDepth();
    fixture.startWithAction();
    x->setFocus();
    x->selectAll();
    const auto desired = x->value() + 63.25;
    QTest::keyClicks(x, QString::number(desired, 'f', 2));
    CHECK(sameTransform(fixture.transform(), original));
    QTest::keyClick(x, Qt::Key_Return);
    settleEvents();
    const auto typed = fixture.transform();
    CHECK(near(x->value(), desired, 0.011));
    CHECK(!sameTransform(typed, original));
    undo->trigger();
    settleEvents();
    CHECK(activeTransform(fixture));
    CHECK(sameTransform(fixture.transform(), original));
    redo->trigger();
    settleEvents();
    CHECK(sameTransform(fixture.transform(), typed));

    x->setFocus();
    settleEvents();
    QTest::keyPress(x, Qt::Key_Up);
    CHECK(QApplication::focusWidget() == x
        || x->isAncestorOf(QApplication::focusWidget()));
    CHECK(near(x->value(), desired + 1, 0.011));
    for (int i = 0; i < 8; ++i) {
        QKeyEvent repeat(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier,
            QString(), true, 1);
        QCoreApplication::sendEvent(x, &repeat);
    }
    QTest::keyRelease(x, Qt::Key_Up);
    settleEvents();
    const auto stepped = fixture.transform();
    // Qt's spinbox seeds an additional step on its initial autorepeat
    // transition. The exact native repeat count is not our grouping contract.
    CHECK(x->value() >= desired + 9);
    CHECK(near(x->value() - desired, std::round(x->value() - desired), 0.011));
    x->clearFocus();
    undo->trigger();
    settleEvents();
    CHECK(activeTransform(fixture));
    CHECK(sameTransform(fixture.transform(), typed));
    redo->trigger();
    settleEvents();
    CHECK(sameTransform(fixture.transform(), stepped));
    apply->click();
    settleEvents();
    CHECK(fixture.window.editorSession().history().undoDepth() == depth + 2);
    undo->trigger();
    settleEvents();
    CHECK(sameTransform(fixture.transform(), typed));
    undo->trigger();
    settleEvents();
    CHECK(sameTransform(fixture.transform(), original));
}

void mouseGeometrySteppersReleaseTextFocusAndRouteImmediateUndo()
{
    for (const bool transformMode : {false, true}) {
        Fixture fixture;
        CHECK(fixture.valid());
        if (!fixture.valid()) continue;
        if (transformMode) fixture.startWithAction();
        auto* angle = fixture.window.findChild<QDoubleSpinBox*>(transformMode
                ? QStringLiteral("TransformAngleControl")
                : QStringLiteral("MoveAngleControl"));
        CHECK(angle);
        if (!angle) continue;
        auto* editor = angle->findChild<QLineEdit*>();
        CHECK(editor);
        if (!editor) continue;
        const auto original = fixture.transform();
        const auto originalAngle = angle->value();
        const auto depth = fixture.window.editorSession().history().undoDepth();
        const auto revision = fixture.surfaceRevision();
        const auto assertPointerMode = [&] {
            auto* focus = QApplication::focusWidget();
            CHECK(focus != angle);
            CHECK(!focus || !angle->isAncestorOf(focus));
            CHECK(!editor->hasSelectedText());
        };
        const auto keyUndo = [&] {
            // Do not trigger QAction directly or clear focus from the test:
            // the mouse action itself must leave global/session Undo reachable.
            auto* receiver = QApplication::focusWidget();
            QTest::keyClick(receiver ? receiver : &fixture.window,
                Qt::Key_Z, Qt::ControlModifier);
            settleEvents();
            CHECK(activeTransform(fixture) == transformMode);
        };

        for (const bool increment : {true, false}) {
            // Starting with selected text reproduces the reported state, and
            // exercises both up/down rather than only a fresh unfocused field.
            angle->setFocus();
            angle->selectAll();
            settleEvents();
            CHECK(editor->hasSelectedText());
            const QPoint point(angle->width() - 8,
                increment ? 7 : angle->height() - 7);
            // Go through QWidgetWindow so Qt records its implicit press owner,
            // just as for hardware input (direct QWidget delivery skips this).
            auto* inputWindow = angle->window()->windowHandle();
            const auto windowPoint = angle->mapTo(angle->window(), point);
            QTest::mousePress(inputWindow, Qt::LeftButton, Qt::NoModifier, windowPoint);
            settleEvents();
            assertPointerMode();
            CHECK(fixture.pointerRouter->captureDomain()
                == ui::CrossWindowPointerRouter::CaptureDomain::Widget);
            CHECK(fixture.pointerRouter->captureOwner() == angle);
            CHECK(near(angle->value(), originalAngle + (increment ? 1 : -1), 0.011));
            CHECK(fixture.window.editorSession().history().undoDepth() == depth);
            QTest::mouseRelease(inputWindow, Qt::LeftButton, Qt::NoModifier, windowPoint);
            settleEvents();
            assertPointerMode();
            CHECK(fixture.pointerRouter->captureDomain()
                == ui::CrossWindowPointerRouter::CaptureDomain::None);
            CHECK(fixture.window.editorSession().history().undoDepth()
                == depth + (transformMode ? 0 : 1));
            keyUndo();
            CHECK(sameTransform(fixture.transform(), original));
            CHECK(fixture.window.editorSession().history().undoDepth() == depth);
        }

        // Native auto-repeat must remain one gesture after removing focus;
        // transient focus changes must not split it into per-repeat actions.
        const QPoint up(angle->width() - 8, 7);
        angle->setFocus();
        angle->selectAll();
        QTest::mousePress(angle->window()->windowHandle(), Qt::LeftButton,
            Qt::NoModifier, angle->mapTo(angle->window(), up));
        QTest::qWait(650);
        assertPointerMode();
        CHECK(fixture.pointerRouter->captureDomain()
            == ui::CrossWindowPointerRouter::CaptureDomain::Widget);
        CHECK(fixture.pointerRouter->captureOwner() == angle);
        CHECK(angle->value() >= originalAngle + 2);
        CHECK(fixture.window.editorSession().history().undoDepth() == depth);
        // Clearing editor focus must not cancel the application-level capture:
        // crossing to the Vulkan surface still routes the release to this hold.
        const auto routedBefore = fixture.pointerRouter->routedEventCount();
        const QPointF canvasPoint(fixture.canvas->width() / 2.0,
            fixture.canvas->height() / 2.0);
        sendMouse(*fixture.canvas, QEvent::MouseMove, canvasPoint,
            Qt::NoButton, Qt::LeftButton);
        CHECK(fixture.pointerRouter->captureOwner() == angle);
        CHECK(fixture.window.editorSession().history().undoDepth() == depth);
        sendMouse(*fixture.canvas, QEvent::MouseButtonRelease, canvasPoint,
            Qt::LeftButton, Qt::NoButton);
        settleEvents();
        assertPointerMode();
        CHECK(fixture.pointerRouter->captureDomain()
            == ui::CrossWindowPointerRouter::CaptureDomain::None);
        CHECK(fixture.pointerRouter->routedEventCount() >= routedBefore + 2U);
        CHECK(QWidget::mouseGrabber() == nullptr);
        CHECK(fixture.window.editorSession().history().undoDepth()
            == depth + (transformMode ? 0 : 1));
        keyUndo();
        CHECK(sameTransform(fixture.transform(), original));
        CHECK(fixture.surfaceRevision() == revision);
    }
}

void typedGeometryThenMouseStepCommitsTextBeforeTheArrowAction()
{
    for (const bool transformMode : {false, true}) {
        Fixture fixture;
        CHECK(fixture.valid());
        if (!fixture.valid()) continue;
        if (transformMode) fixture.startWithAction();
        auto* angle = fixture.window.findChild<QDoubleSpinBox*>(transformMode
                ? QStringLiteral("TransformAngleControl")
                : QStringLiteral("MoveAngleControl"));
        CHECK(angle);
        if (!angle) continue;
        auto* editor = angle->findChild<QLineEdit*>();
        CHECK(editor);
        if (!editor) continue;
        const auto original = fixture.transform();
        const auto desired = angle->value() + 23.25;
        const auto depth = fixture.window.editorSession().history().undoDepth();
        angle->setFocus();
        angle->selectAll();
        QTest::keyClicks(angle, QString::number(desired, 'f', 2));
        CHECK(sameTransform(fixture.transform(), original));
        CHECK(fixture.window.editorSession().history().undoDepth() == depth);
        const QPoint up(angle->width() - 8, 7);
        QTest::mouseClick(angle->window()->windowHandle(), Qt::LeftButton,
            Qt::NoModifier, angle->mapTo(angle->window(), up));
        settleEvents();
        auto* focus = QApplication::focusWidget();
        CHECK(focus != angle);
        CHECK(!focus || !angle->isAncestorOf(focus));
        CHECK(!editor->hasSelectedText());
        CHECK(near(angle->value(), desired + 1, 0.011));
        CHECK(fixture.window.editorSession().history().undoDepth()
            == depth + (transformMode ? 0 : 2));

        const auto keyUndo = [&] {
            auto* receiver = QApplication::focusWidget();
            QTest::keyClick(receiver ? receiver : &fixture.window,
                Qt::Key_Z, Qt::ControlModifier);
            settleEvents();
            CHECK(activeTransform(fixture) == transformMode);
        };
        keyUndo();
        CHECK(near(angle->value(), desired, 0.011));
        CHECK(!sameTransform(fixture.transform(), original));
        keyUndo();
        CHECK(sameTransform(fixture.transform(), original));
        CHECK(fixture.window.editorSession().history().undoDepth() == depth);
    }
}

void topBarButtonsShareVisibleRestingChromeAndCachedInstances()
{
    Fixture fixture;
    CHECK(fixture.valid());
    if (!fixture.valid()) return;
    for (const auto* name : {"MoveActiveLayerOnly", "MoveSelectUnderMouse", "BrushDirectionButton",
             "BrushEraseModeButton", "TransformAspectLock", "TransformFlipHorizontal",
             "TransformFlipVertical", "TransformApply", "TransformCancel"}) {
        auto* button = fixture.window.findChild<QToolButton*>(QString::fromLatin1(name));
        CHECK(button);
        if (button) {
            CHECK(button->property("toolOptionsButton").toBool());
            CHECK(!button->autoRaise());
        }
    }
    auto* lock = fixture.window.findChild<QToolButton*>(QStringLiteral("MoveActiveLayerOnly"));
    auto* brush = fixture.window.findChild<QAction*>(QStringLiteral("ToolAction_brush"));
    auto* move = fixture.window.findChild<QAction*>(QStringLiteral("ToolAction_move"));
    CHECK(lock && brush && move);
    if (!lock || !brush || !move) return;
    move->trigger();
    settleEvents();
    CHECK(lock->isVisible());
    CHECK(!lock->isChecked());
    CHECK(lock->height() == 28);
    CHECK(!lock->icon().isNull());
    CHECK(lock->toolButtonStyle() == Qt::ToolButtonIconOnly);
    const auto workspaceGeometry = fixture.workspace->geometry();
    const auto canvasGeometry = fixture.workspace->canvasContainer()->geometry();
    const auto historyDepth = fixture.window.editorSession().history().undoDepth();
    const auto unchecked = lock->grab().toImage();
    const QPoint quietPixel(unchecked.width() / 2,
        static_cast<int>(4 * unchecked.devicePixelRatio()));
    const auto restFill = unchecked.pixelColor(quietPixel);
    const auto barImage = fixture.optionsBar->grab().toImage();
    const auto barFill = barImage.pixelColor(4, barImage.height() / 2);
    // The unchecked control must have a real button surface; previously it
    // inherited transparent QToolButton styling and looked like plain text.
    CHECK(restFill != barFill);
    lock->click();
    settleEvents();
    CHECK(lock->isChecked());
    CHECK(lock->grab().toImage().pixelColor(quietPixel) != restFill);
    CHECK(fixture.window.editorSession().history().undoDepth() == historyDepth);
    brush->trigger();
    settleEvents();
    CHECK(!lock->isVisible());
    move->trigger();
    settleEvents();
    CHECK(fixture.window.findChild<QToolButton*>(QStringLiteral("MoveActiveLayerOnly")) == lock);
    CHECK(lock->isVisible());
    CHECK(lock->isChecked());
    CHECK(fixture.workspace->geometry() == workspaceGeometry);
    CHECK(fixture.workspace->canvasContainer()->geometry() == canvasGeometry);
}

void cornerResizeCursorsStayDiagonalOnWideTallRotatedAndFlippedFrames()
{
    RasterFixture f;
    CHECK(f.valid());
    if (!f.valid()) return;
    auto* width = f.window.findChild<QDoubleSpinBox*>("TransformScaleXControl");
    auto* height = f.window.findChild<QDoubleSpinBox*>("TransformScaleYControl");
    auto* angle = f.window.findChild<QDoubleSpinBox*>("TransformAngleControl");
    auto* lock = f.window.findChild<QToolButton*>("TransformAspectLock");
    auto* flip = f.window.findChild<QToolButton*>("TransformFlipHorizontal");
    CHECK(width && height && angle && lock && flip);
    if (!width || !height || !angle || !lock || !flip) return;
    f.startWithAction();
    lock->setChecked(false);
    for (bool tall : {false, true}) {
        width->setValue(tall ? 40 : 180);
        height->setValue(tall ? 180 : 40);
        for (double degrees : {0.0, 37.0, 90.0, 125.0, -40.0}) {
            angle->setValue(degrees);
            for (int mirrored = 0; mirrored < 2; ++mirrored) {
                settleEvents();
                const auto handles = f.canvas->logicalTransformHandles();
                for (std::size_t index : {0u, 2u, 4u, 6u}) {
                    const QPointF p(handles[index].x, handles[index].y);
                    sendMouse(*f.canvas, QEvent::MouseMove, p, Qt::NoButton, Qt::NoButton);
                    CHECK(f.canvas->scene().transformHighlight == core::TransformHandle(index));
                    const auto cursor = f.canvas->cursor().shape();
                    CHECK(cursor == Qt::SizeFDiagCursor || cursor == Qt::SizeBDiagCursor);
                    const auto opposite = handles[(index + 4) % 8];
                    const bool sameDirection = (p.x() > opposite.x) == (p.y() > opposite.y);
                    CHECK(cursor == (sameDirection ? Qt::SizeFDiagCursor : Qt::SizeBDiagCursor));
                    // Ctrl-corner distortion is still distinct from resizing.
                    sendMouse(*f.canvas, QEvent::MouseMove, p, Qt::NoButton, Qt::NoButton, Qt::ControlModifier);
                    CHECK(f.canvas->cursor().shape() == Qt::SizeAllCursor);
                }
                if (degrees == 0.0) {
                    for (std::size_t index : {1u, 3u, 5u, 7u}) {
                        sendMouse(*f.canvas, QEvent::MouseMove, {handles[index].x, handles[index].y}, Qt::NoButton, Qt::NoButton);
                        CHECK(f.canvas->cursor().shape() == (index % 4 == 1 ? Qt::SizeVerCursor : Qt::SizeHorCursor));
                    }
                }
                flip->click();
            }
        }
    }
    QTest::keyClick(f.canvas, Qt::Key_Escape);
    settleEvents();
}

void distortedSideHandlesRemainHoverableAndDraggable()
{
    // Exercise the full-support extreme quad, without a transparent margin
    // crossing the projective horizon outside the new tight handle frame.
    RasterFixture f(false);
    CHECK(f.valid());
    if (!f.valid()) return;
    f.startWithAction();
    auto* lock=f.window.findChild<QToolButton*>("TransformAspectLock");
    auto* undo=actionWithShortcut(f.window,QKeySequence::Undo);
    CHECK(lock && undo);
    if (!lock || !undo) return;
    lock->setChecked(false);
    const auto original=f.transform();
    const auto pixels=f.surfaceRevision();
    const auto initial=f.canvas->logicalTransformHandles();
    const auto corner=initial[4];
    // Lift bottom-right towards top-right, with the other corners fixed.
    const auto distortedCorner=initial[2]+core::Vec2d{
        (initial[2].x-initial[0].x)*.15,(initial[4].y-initial[2].y)*.22};
    dragOnCanvas(*f.canvas,{corner.x,corner.y},{distortedCorner.x,distortedCorner.y},Qt::ControlModifier);
    const auto distorted=f.transform();
    CHECK(!distorted.isAffine());
    for (std::size_t edge=0;edge<4;++edge) {
        const auto handles=f.canvas->logicalTransformHandles();
        // Do not test only the hit-testing helper's own positions: derive the
        // marker center independently as transform.frag does from its corners.
        const auto middle=(handles[edge*2]+handles[(edge*2+2)%8])*.5;
        const QPointF point(middle.x,middle.y);
        sendMouse(*f.canvas,QEvent::MouseMove,point,Qt::NoButton,Qt::NoButton);
        CHECK(f.canvas->scene().transformHighlight==core::TransformHandle(edge*2+1));
        const auto cursor=f.canvas->cursor().shape();
        CHECK(cursor==Qt::SizeHorCursor || cursor==Qt::SizeVerCursor
            || cursor==Qt::SizeFDiagCursor || cursor==Qt::SizeBDiagCursor);
        sendMouse(*f.canvas,QEvent::MouseButtonPress,point,Qt::LeftButton,Qt::LeftButton);
        CHECK(f.canvas->transformDragging());
        CHECK(f.canvas->scene().transformHighlight==core::TransformHandle(edge*2+1));
        CHECK(f.transform()==distorted); // No press-time jump.
        sendMouse(*f.canvas,QEvent::MouseMove,point+QPointF(11,8),Qt::NoButton,Qt::LeftButton);
        sendMouse(*f.canvas,QEvent::MouseButtonRelease,point+QPointF(11,8),Qt::LeftButton,Qt::NoButton);
        settleEvents();
        CHECK(!f.canvas->transformDragging());
        CHECK(f.transform()!=distorted && !f.transform().isAffine());
        CHECK(f.surfaceRevision()==pixels);
        undo->trigger();settleEvents();
        CHECK(f.transform()==distorted);
    }
    QTest::keyClick(f.canvas,Qt::Key_Escape);settleEvents();
    CHECK(f.transform()==original && f.surfaceRevision()==pixels);
}

void nearCollapsedPerspectiveShiftResizeDoesNotJump()
{
    RasterFixture f;
    CHECK(f.valid());if(!f.valid())return;
    QImage image(1024,768,QImage::Format_RGBA8888);image.fill(QColor(200,70,20));
    const auto path=f.assets.filePath(QStringLiteral("near-triangle.png"));
    CHECK(image.save(path));CHECK(f.window.openImageFromPath(path));settleEvents();
    const auto matrix=core::rectangleToQuad({0,0,1024,768},
        std::array<core::Vec2d,4>{{{254,179},{624,145},{817,132},{96,393}}});
    CHECK(matrix);if(!matrix)return;
    auto& session=const_cast<core::EditorSession&>(f.window.editorSession());
    CHECK(session.document()->setLayerTransform(*session.activeLayer(),*matrix));
    const auto revision=f.surfaceRevision();
    f.startWithAction();
    auto* lock=f.window.findChild<QToolButton*>("TransformAspectLock");
    auto* undo=actionWithShortcut(f.window,QKeySequence::Undo);
    CHECK(lock && undo);if(!lock || !undo)return;
    lock->setChecked(true);
    const auto h=core::transformHandles(*matrix,{1024,768});
    const auto press=(h[0]+h[2])*.5;
    const auto inverse=matrix->inverted();CHECK(inverse);if(!inverse)return;
    const double horizonOffset=-inverse->denominator(press)/inverse->m21;
    const auto start=logicalPoint(f,press);
    sendMouse(*f.canvas,QEvent::MouseMove,start,Qt::NoButton,Qt::NoButton);
    CHECK(f.canvas->scene().transformHighlight==core::TransformHandle::Top);
    sendMouse(*f.canvas,QEvent::MouseButtonPress,start,Qt::LeftButton,Qt::LeftButton);
    for(double dy:{horizonOffset-1.,horizonOffset-.01,horizonOffset,horizonOffset+.01,horizonOffset+1.,0.}) {
        sendMouse(*f.canvas,QEvent::MouseMove,logicalPoint(f,press+core::Vec2d{0,dy}),
            Qt::NoButton,Qt::LeftButton,Qt::ShiftModifier);
        CHECK(f.canvas->transformDragging() && !f.transform().isAffine());
        const auto changed=core::transformHandles(f.transform(),{1024,768});
        for(std::size_t i:{0u,2u,4u,6u})CHECK(std::hypot(changed[i].x-h[i].x,changed[i].y-h[i].y)<20);
        CHECK(f.surfaceRevision()==revision);
    }
    const auto returned=core::transformHandles(f.transform(),{1024,768});
    for(std::size_t i=0;i<8;++i)CHECK(near(returned[i].x,h[i].x) && near(returned[i].y,h[i].y));
    const auto end=logicalPoint(f,press+core::Vec2d{0,horizonOffset-1.});
    sendMouse(*f.canvas,QEvent::MouseMove,end,Qt::NoButton,Qt::LeftButton,Qt::ShiftModifier);
    sendMouse(*f.canvas,QEvent::MouseButtonRelease,end,Qt::LeftButton,Qt::NoButton,Qt::ShiftModifier);
    CHECK(!f.canvas->transformDragging());
    CHECK(f.transform()!=*matrix);
    undo->trigger();settleEvents();CHECK(f.transform()==*matrix);
    QTest::keyClick(f.canvas,Qt::Key_Escape);settleEvents();
    CHECK(f.transform()==*matrix && f.surfaceRevision()==revision);
}

void rotationCursorFacesEveryTransformedCornerAndWrapsCachedAngles()
{
    Fixture fixture;
    CHECK(fixture.valid());
    if (!fixture.valid()) return;
    auto* angle = fixture.window.findChild<QDoubleSpinBox*>(QStringLiteral("TransformAngleControl"));
    auto* flip = fixture.window.findChild<QToolButton*>(QStringLiteral("TransformFlipHorizontal"));
    CHECK(angle && flip);
    if (!angle || !flip) return;
    fixture.startWithAction();
    constexpr std::array<std::size_t, 4> corners {0, 2, 4, 6};
    const auto visitCorners = [&] {
        std::array<qint64, 4> keys {};
        const auto handles = fixture.canvas->logicalTransformHandles();
        for (std::size_t i = 0; i < corners.size(); ++i) {
            const auto point = rotationPointForCorner(handles, corners[i]);
            sendMouse(*fixture.canvas, QEvent::MouseMove, point,
                Qt::NoButton, Qt::NoButton);
            CHECK(fixture.canvas->scene().transformHighlight == core::TransformHandle::Rotate);
            checkRotationCursorFacesCorner(*fixture.canvas, corners[i]);
            keys[i] = fixture.canvas->cursor().pixmap().cacheKey();
            sendMouse(*fixture.canvas, QEvent::MouseMove, point + QPointF(0.1, 0.1),
                Qt::NoButton, Qt::NoButton);
            CHECK(fixture.canvas->cursor().pixmap().cacheKey() == keys[i]);
        }
        for (std::size_t i = 0; i < keys.size(); ++i) {
            for (std::size_t j = i + 1; j < keys.size(); ++j)
                CHECK(keys[i] != keys[j]);
        }
        return keys;
    };
    const auto initialKeys = visitCorners();
    angle->setValue(37);
    settleEvents();
    const auto rotatedKeys = visitCorners();
    CHECK(initialKeys != rotatedKeys);
    flip->click();
    settleEvents();
    const auto flippedKeys = visitCorners();
    CHECK(flippedKeys[0] == rotatedKeys[1]);
    CHECK(flippedKeys[1] == rotatedKeys[0]);
    CHECK(flippedKeys[2] == rotatedKeys[3]);
    CHECK(flippedKeys[3] == rotatedKeys[2]);

    // Equivalent rotations straddling the signed-angle seam must reuse the
    // same quantized variants rather than allocating a second 360° family.
    angle->setValue(179.99);
    settleEvents();
    const auto beforeWrap = visitCorners();
    angle->setValue(-179.99);
    settleEvents();
    CHECK(visitCorners() == beforeWrap);
    QTest::keyClick(fixture.canvas, Qt::Key_Escape);
    settleEvents();
    CHECK(fixture.canvas->cursor().shape() == Qt::ArrowCursor);
}

void rotationCursorTracksResolvedDragAngleAndRestoresOutsideItsHitZone()
{
    Fixture fixture;
    CHECK(fixture.valid());
    if (!fixture.valid()) return;
    fixture.startWithAction();
    const auto handles = fixture.canvas->logicalTransformHandles();
    const auto center = centerOf(handles);
    const auto original = fixture.transform();
    const auto rotationPoint = rotationPointForCorner(handles, 2);
    sendMouse(*fixture.canvas, QEvent::MouseMove, rotationPoint,
        Qt::NoButton, Qt::NoButton);
    CHECK(fixture.canvas->scene().transformHighlight == core::TransformHandle::Rotate);
    CHECK(fixture.canvas->cursor().shape() == Qt::BitmapCursor);
    const auto cursor = fixture.canvas->cursor();
    CHECK(cursor.hotSpot() == QPoint(16, 16));
    CHECK(!cursor.pixmap().isNull());
    CHECK(near(cursor.pixmap().devicePixelRatio(), fixture.canvas->devicePixelRatio(), 0.011));
    const auto key = cursor.pixmap().cacheKey();
    CHECK(key != 0);
    for (int i = 0; i < 5; ++i) {
        sendMouse(*fixture.canvas, QEvent::MouseMove, rotationPoint + QPointF(i * 0.1, 0),
            Qt::NoButton, Qt::NoButton);
        CHECK(fixture.canvas->cursor().pixmap().cacheKey() == key);
    }
    sendMouse(*fixture.canvas, QEvent::MouseMove, center,
        Qt::NoButton, Qt::NoButton);
    CHECK(fixture.canvas->cursor().shape() == Qt::OpenHandCursor);
    sendMouse(*fixture.canvas, QEvent::MouseMove, rotationPoint,
        Qt::NoButton, Qt::NoButton);
    CHECK(fixture.canvas->cursor().pixmap().cacheKey() == key);

    const auto end = rotatePoint(rotationPoint, center, 31);
    sendMouse(*fixture.canvas, QEvent::MouseButtonPress, rotationPoint,
        Qt::LeftButton, Qt::LeftButton);
    sendMouse(*fixture.canvas, QEvent::MouseMove, end,
        Qt::NoButton, Qt::LeftButton, Qt::ShiftModifier);
    CHECK(fixture.canvas->transformDragging());
    CHECK(fixture.canvas->scene().transformHighlight == core::TransformHandle::Rotate);
    checkRotationCursorFacesCorner(*fixture.canvas, 2);
    const auto rotatedKey = fixture.canvas->cursor().pixmap().cacheKey();
    CHECK(rotatedKey != key);
    const auto& raster = std::get<core::RasterLayer>(fixture.activeLayer()->payload);
    const auto values = core::valuesFromTransform(fixture.transform(), raster.surface->extent());
    CHECK(values.has_value());
    if (values) CHECK(near(values->rotationDegrees, 30));
    // Raw input continues while snapping holds the resolved geometry still.
    // No new cursor variant or matrix should be generated for this update.
    const auto snapped = fixture.transform();
    const auto nearby = rotatePoint(rotationPoint, center, 32);
    sendMouse(*fixture.canvas, QEvent::MouseMove, nearby,
        Qt::NoButton, Qt::LeftButton, Qt::ShiftModifier);
    CHECK(sameTransform(fixture.transform(), snapped));
    CHECK(fixture.canvas->cursor().pixmap().cacheKey() == rotatedKey);
    // Return to the actual snapped corner zone for release. At a large layer
    // size even a two-degree pointer/geometry difference can legitimately put
    // the pointer beyond the fixed logical-pixel hover hit radius.
    const auto resolvedPoint = rotatePoint(rotationPoint, center, 30);
    sendMouse(*fixture.canvas, QEvent::MouseMove, resolvedPoint,
        Qt::NoButton, Qt::LeftButton, Qt::ShiftModifier);
    sendMouse(*fixture.canvas, QEvent::MouseButtonRelease, resolvedPoint,
        Qt::LeftButton, Qt::NoButton, Qt::ShiftModifier);
    settleEvents();
    CHECK(!fixture.canvas->transformDragging());
    CHECK(fixture.canvas->cursor().shape() == Qt::BitmapCursor);
    CHECK(fixture.canvas->cursor().pixmap().cacheKey() == rotatedKey);

    // Cancelling a second live rotation restores both its geometry and the
    // hover cursor selected from that restored geometry, not its stale angle.
    const auto nextPoint = rotationPointForCorner(fixture.canvas->logicalTransformHandles(), 2);
    sendMouse(*fixture.canvas, QEvent::MouseMove, nextPoint,
        Qt::NoButton, Qt::NoButton);
    sendMouse(*fixture.canvas, QEvent::MouseButtonPress, nextPoint,
        Qt::LeftButton, Qt::LeftButton);
    sendMouse(*fixture.canvas, QEvent::MouseMove, rotatePoint(nextPoint, center, -22),
        Qt::NoButton, Qt::LeftButton);
    CHECK(fixture.canvas->cursor().pixmap().cacheKey() != rotatedKey);
    fixture.canvas->cancelTransformInput();
    CHECK(!fixture.canvas->transformDragging());
    CHECK(sameTransform(fixture.transform(), snapped));
    sendMouse(*fixture.canvas, QEvent::MouseMove, nextPoint,
        Qt::NoButton, Qt::NoButton);
    CHECK(fixture.canvas->cursor().pixmap().cacheKey() == rotatedKey);
    sendMouse(*fixture.canvas, QEvent::MouseMove,
        centerOf(fixture.canvas->logicalTransformHandles()), Qt::NoButton, Qt::NoButton);
    CHECK(fixture.canvas->cursor().shape() == Qt::OpenHandCursor);
    QEvent leave(QEvent::Leave);
    QCoreApplication::sendEvent(fixture.canvas, &leave);
    CHECK(fixture.canvas->cursor().shape() == Qt::ArrowCursor);
    QTest::keyClick(fixture.canvas, Qt::Key_Escape);
    settleEvents();
    CHECK(fixture.canvas->cursor().shape() == Qt::ArrowCursor);
    CHECK(!fixture.canvas->scene().transformOverlay);
    CHECK(sameTransform(fixture.transform(), original));
}

void applicationAccentInvalidatesOnlyOverlayStyle()
{
    Fixture fixture;
    CHECK(fixture.valid());
    if (!fixture.valid()) return;
    fixture.startWithAction();
    const auto oldPalette = QApplication::palette();
    const auto originalAccent = fixture.canvas->scene().overlayAccent;
    CHECK(originalAccent == ui::editorAccent());
    const auto geometry = fixture.transform();
    const auto surfaceRevision = fixture.surfaceRevision();
    const auto documentRevision = fixture.window.editorSession().document()->revision();
    const auto& raster = std::get<core::RasterLayer>(fixture.activeLayer()->payload);
    const auto surfaceId = raster.surface->id();
    const auto bytes = rasterBytes(*fixture.activeLayer());
    auto changed = oldPalette;
    changed.setColor(QPalette::Highlight, QColor(217, 130, 49, 255));
    QApplication::setPalette(changed);
    settleEvents();
    const core::Rgba8 expected {217, 130, 49, 255};
    CHECK(fixture.canvas->scene().overlayAccent == expected);
    CHECK(ui::editorAccent() == expected);
    CHECK(sameTransform(fixture.transform(), geometry));
    CHECK(fixture.surfaceRevision() == surfaceRevision);
    CHECK(raster.surface->id() == surfaceId);
    CHECK(rasterBytes(*fixture.activeLayer()) == bytes);
    CHECK(fixture.window.editorSession().document()->revision() == documentRevision);
    QApplication::setPalette(oldPalette);
    settleEvents();
    CHECK(fixture.canvas->scene().overlayAccent == originalAccent);
    QTest::keyClick(fixture.canvas, Qt::Key_Escape);
    settleEvents();
}

void ctrlTUsesOneCachedPageWithoutRelayout()
{
    Fixture fixture;
    CHECK(fixture.valid());
    if (!fixture.valid()) {
        return;
    }

    auto* context = fixture.window.findChild<QLabel*>(
        QStringLiteral("ToolOptionsContext"));
    auto* cancel = fixture.window.findChild<QToolButton*>(
        QStringLiteral("TransformCancel"));
    CHECK(context && cancel);
    if (!context || !cancel) {
        return;
    }

    const auto* pageIdentity = fixture.transformPage;
    const auto workspaceGeometry = fixture.workspace->geometry();
    const auto canvasGeometry = fixture.workspace->canvasContainer()->geometry();
    const auto toolbarGeometry = fixture.optionsBar->geometry();
    const auto historyDepth = fixture.window.editorSession().history().undoDepth();
    const auto original = fixture.transform();

    CHECK(!fixture.transformPage->isVisible());
    fixture.startWithShortcut();
    CHECK(activeTransform(fixture));
    CHECK(context->text() == QStringLiteral("TRANSFORM"));
    CHECK(fixture.optionsBar->pageForTool(core::ToolId::Transform)
        == pageIdentity);
    CHECK(fixture.workspace->geometry() == workspaceGeometry);
    CHECK(fixture.workspace->canvasContainer()->geometry() == canvasGeometry);
    CHECK(fixture.optionsBar->geometry() == toolbarGeometry);
    CHECK(fixture.window.editorSession().history().undoDepth() == historyDepth);
    CHECK(sameTransform(fixture.transform(), original));

    // Ctrl+T is idempotent while the same persistent session is active.
    fixture.startWithShortcut();
    CHECK(activeTransform(fixture));
    CHECK(fixture.window.findChild<QWidget*>(QStringLiteral("TransformOptionsPage"))
        == pageIdentity);
    CHECK(fixture.workspace->canvasContainer()->geometry() == canvasGeometry);

    QTest::mouseClick(cancel, Qt::LeftButton);
    settleEvents();
    CHECK(fixture.window.editorSession().activeTool() == core::ToolId::Move);
    CHECK(!fixture.canvas->scene().transformOverlay.has_value());
    CHECK(!fixture.transformPage->isVisible());
    CHECK(sameTransform(fixture.transform(), original));
    CHECK(fixture.window.editorSession().history().undoDepth() == historyDepth);
    CHECK(fixture.workspace->canvasContainer()->geometry() == canvasGeometry);

    fixture.startWithAction();
    CHECK(activeTransform(fixture));
    CHECK(fixture.window.findChild<QWidget*>(QStringLiteral("TransformOptionsPage"))
        == pageIdentity);
    CHECK(fixture.workspace->canvasContainer()->geometry() == canvasGeometry);
    QTest::keyClick(fixture.canvas, Qt::Key_Escape);
    settleEvents();
}

void moveScaleAndRotateKeepIndividualUndoStepsInsideAndAfterApply()
{
    Fixture fixture;
    CHECK(fixture.valid());
    if (!fixture.valid()) {
        return;
    }
    auto* apply = fixture.window.findChild<QToolButton*>(
        QStringLiteral("TransformApply"));
    auto* undo = actionWithShortcut(fixture.window, QKeySequence::Undo);
    auto* redo = actionWithShortcut(fixture.window, QKeySequence::Redo);
    CHECK(apply && undo && redo);
    if (!apply || !undo || !redo) {
        return;
    }

    const auto original = fixture.transform();
    const auto surfaceRevision = fixture.surfaceRevision();
    const auto historyDepth = fixture.window.editorSession().history().undoDepth();
    fixture.startWithAction();
    CHECK(activeTransform(fixture));

    auto handles = fixture.canvas->logicalTransformHandles();
    const auto center = centerOf(handles);
    dragOnCanvas(*fixture.canvas, center, center + QPointF(42.0, 23.0));
    const auto moved = fixture.transform();
    CHECK(activeTransform(fixture));
    CHECK(!fixture.canvas->transformDragging());
    CHECK(!sameTransform(moved, original));
    CHECK(fixture.window.editorSession().history().undoDepth() == historyDepth);
    CHECK(fixture.surfaceRevision() == surfaceRevision);

    handles = fixture.canvas->logicalTransformHandles();
    const QPointF bottomRight(handles[4].x, handles[4].y);
    dragOnCanvas(*fixture.canvas, bottomRight,
        bottomRight + QPointF(39.0, 27.0));
    const auto scaled = fixture.transform();
    CHECK(activeTransform(fixture));
    CHECK(!fixture.canvas->transformDragging());
    CHECK(!sameTransform(scaled, moved));
    CHECK(fixture.window.editorSession().history().undoDepth() == historyDepth);
    CHECK(fixture.surfaceRevision() == surfaceRevision);

    handles = fixture.canvas->logicalTransformHandles();
    const auto rotationCenter = centerOf(handles);
    const QPointF corner(handles[2].x, handles[2].y);
    auto outward = corner - rotationCenter;
    const auto outwardLength = std::hypot(outward.x(), outward.y());
    CHECK(outwardLength > 0.0);
    if (outwardLength <= 0.0) {
        return;
    }
    outward *= 15.0 / outwardLength;
    const auto rotationPress = corner + outward;
    sendMouse(*fixture.canvas, QEvent::MouseMove, rotationPress,
        Qt::NoButton, Qt::NoButton);
    CHECK(fixture.canvas->scene().transformHighlight
        == core::TransformHandle::Rotate);
    const auto rotationEnd = rotatePoint(rotationPress, rotationCenter, 31.0);
    dragOnCanvas(*fixture.canvas, rotationPress, rotationEnd);
    const auto rotated = fixture.transform();
    CHECK(activeTransform(fixture));
    CHECK(!fixture.canvas->transformDragging());
    CHECK(!sameTransform(rotated, scaled));
    CHECK(fixture.window.editorSession().history().undoDepth() == historyDepth);
    CHECK(fixture.surfaceRevision() == surfaceRevision);

    // Session actions are pending, but Undo/Redo must already walk them while
    // leaving the cached page, selected layer and interaction mode intact.
    undo->trigger();
    settleEvents();
    CHECK(activeTransform(fixture));
    CHECK(sameTransform(fixture.transform(), scaled));
    const auto restoredHandles = fixture.canvas->logicalTransformHandles();
    CHECK(near(centerOf(restoredHandles).x(), centerOf(handles).x()));
    CHECK(near(centerOf(restoredHandles).y(), centerOf(handles).y()));
    undo->trigger();
    settleEvents();
    CHECK(activeTransform(fixture));
    CHECK(sameTransform(fixture.transform(), moved));
    undo->trigger();
    settleEvents();
    CHECK(activeTransform(fixture));
    CHECK(sameTransform(fixture.transform(), original));
    CHECK(!undo->isEnabled());
    for (const auto& expected : {moved, scaled, rotated}) {
        CHECK(redo->isEnabled());
        redo->trigger();
        settleEvents();
        CHECK(activeTransform(fixture));
        CHECK(sameTransform(fixture.transform(), expected));
    }
    CHECK(!redo->isEnabled());

    QTest::mouseClick(apply, Qt::LeftButton);
    settleEvents();
    CHECK(fixture.window.editorSession().activeTool() == core::ToolId::Move);
    CHECK(!fixture.canvas->scene().transformOverlay.has_value());
    CHECK(fixture.window.editorSession().history().undoDepth()
        == historyDepth + 3U);
    CHECK(sameTransform(fixture.transform(), rotated));
    CHECK(fixture.surfaceRevision() == surfaceRevision);

    for (const auto& expected : {scaled, moved, original}) {
        undo->trigger();
        settleEvents();
        CHECK(sameTransform(fixture.transform(), expected));
        CHECK(fixture.surfaceRevision() == surfaceRevision);
    }
    for (const auto& expected : {moved, scaled, rotated}) {
        redo->trigger();
        settleEvents();
        CHECK(sameTransform(fixture.transform(), expected));
        CHECK(fixture.surfaceRevision() == surfaceRevision);
    }
}

void enterAppliesExactlyOneUndoCommand()
{
    Fixture fixture;
    CHECK(fixture.valid());
    if (!fixture.valid()) {
        return;
    }
    auto* x = fixture.window.findChild<QDoubleSpinBox*>(
        QStringLiteral("TransformXControl"));
    CHECK(x);
    if (!x) {
        return;
    }

    const auto original = fixture.transform();
    const auto historyDepth = fixture.window.editorSession().history().undoDepth();
    fixture.startWithAction();
    x->setValue(x->value() + 37.25);
    settleEvents();
    const auto preview = fixture.transform();
    CHECK(!sameTransform(preview, original));
    CHECK(fixture.window.editorSession().history().undoDepth() == historyDepth);

    QTest::keyClick(fixture.canvas, Qt::Key_Return);
    settleEvents();
    CHECK(fixture.window.editorSession().activeTool() == core::ToolId::Move);
    CHECK(fixture.window.editorSession().history().undoDepth()
        == historyDepth + 1U);
    CHECK(sameTransform(fixture.transform(), preview));
    QTest::keyClick(fixture.canvas, Qt::Key_Return);
    settleEvents();
    CHECK(fixture.window.editorSession().history().undoDepth()
        == historyDepth + 1U);

    auto* undo = actionWithShortcut(fixture.window, QKeySequence::Undo);
    CHECK(undo);
    if (undo) {
        undo->trigger();
        settleEvents();
        CHECK(sameTransform(fixture.transform(), original));
    }
}

void escapeCancelsAndRestoresThePreviousTool()
{
    Fixture fixture;
    CHECK(fixture.valid());
    if (!fixture.valid()) {
        return;
    }
    auto* brush = fixture.window.findChild<QAction*>(
        QStringLiteral("ToolAction_brush"));
    auto* x = fixture.window.findChild<QDoubleSpinBox*>(
        QStringLiteral("TransformXControl"));
    CHECK(brush && x);
    if (!brush || !x) {
        return;
    }
    brush->trigger();
    settleEvents();
    CHECK(fixture.window.editorSession().activeTool() == core::ToolId::Brush);
    const auto original = fixture.transform();
    const auto historyDepth = fixture.window.editorSession().history().undoDepth();

    fixture.startWithShortcut();
    x->setValue(x->value() + 81.0);
    settleEvents();
    CHECK(!sameTransform(fixture.transform(), original));
    QTest::keyClick(fixture.canvas, Qt::Key_Escape);
    settleEvents();
    CHECK(fixture.window.editorSession().activeTool() == core::ToolId::Brush);
    CHECK(!fixture.canvas->scene().transformOverlay.has_value());
    CHECK(sameTransform(fixture.transform(), original));
    CHECK(fixture.window.editorSession().history().undoDepth() == historyDepth);
}

void numericEditorsOwnTheirFirstTerminalKeyAndToolShortcuts()
{
    Fixture fixture;
    CHECK(fixture.valid());
    if (!fixture.valid()) {
        return;
    }
    auto* x = fixture.window.findChild<QDoubleSpinBox*>(
        QStringLiteral("TransformXControl"));
    auto* y = fixture.window.findChild<QDoubleSpinBox*>(
        QStringLiteral("TransformYControl"));
    CHECK(x && y);
    if (!x || !y) {
        return;
    }
    const auto original = fixture.transform();
    const auto historyDepth = fixture.window.editorSession().history().undoDepth();
    fixture.startWithAction();

    x->setFocus(Qt::OtherFocusReason);
    x->selectAll();
    settleEvents();
    for (const auto key : {Qt::Key_B, Qt::Key_E, Qt::Key_I, Qt::Key_V}) {
        QTest::keyClick(x, key);
        settleEvents();
        CHECK(activeTransform(fixture));
        if (!activeTransform(fixture)) {
            return;
        }
    }
    QTest::keyClick(x, Qt::Key_T, Qt::ControlModifier);
    settleEvents();
    CHECK(activeTransform(fixture));
    if (!activeTransform(fixture)) {
        return;
    }

    const auto changedX = x->value() + 123.5;
    x->selectAll();
    QTest::keyClicks(x, QString::number(changedX, 'f', 2));
    CHECK(activeTransform(fixture));
    CHECK(sameTransform(fixture.transform(), original));
    QTest::keyClick(x, Qt::Key_Return);
    settleEvents();
    CHECK(activeTransform(fixture));
    CHECK(!sameTransform(fixture.transform(), original));
    CHECK(near(x->value(), changedX, 0.011));
    CHECK(fixture.window.editorSession().history().undoDepth() == historyDepth);

    const auto afterX = fixture.transform();
    const auto changedY = y->value() - 77.25;
    y->setFocus(Qt::OtherFocusReason);
    y->selectAll();
    QTest::keyClicks(y, QString::number(changedY, 'f', 2));
    QTest::keyClick(y, Qt::Key_Escape);
    settleEvents();
    // The approved compact-input convention treats a field Escape like Enter:
    // finish that field first, while retaining the persistent transform.
    CHECK(activeTransform(fixture));
    CHECK(!sameTransform(fixture.transform(), afterX));
    CHECK(near(y->value(), changedY, 0.011));
    CHECK(fixture.window.editorSession().history().undoDepth() == historyDepth);

    QTest::keyClick(fixture.canvas, Qt::Key_Escape);
    settleEvents();
    CHECK(fixture.window.editorSession().activeTool() == core::ToolId::Move);
    CHECK(sameTransform(fixture.transform(), original));
    CHECK(fixture.window.editorSession().history().undoDepth() == historyDepth);
}

void canvasFocusLossEndsOnlyTheGestureAndApplicationDeactivateFinishesNumericText()
{
    Fixture fixture;
    CHECK(fixture.valid());
    if (!fixture.valid()) {
        return;
    }
    auto* x = fixture.window.findChild<QDoubleSpinBox*>(
        QStringLiteral("TransformXControl"));
    auto* editor = x ? x->findChild<QLineEdit*>() : nullptr;
    CHECK(x && editor);
    if (!x || !editor) {
        return;
    }

    const auto original = fixture.transform();
    const auto historyDepth = fixture.window.editorSession().history().undoDepth();
    fixture.startWithAction();
    const auto press = centerOf(fixture.canvas->logicalTransformHandles());
    sendMouse(*fixture.canvas, QEvent::MouseMove, press,
        Qt::NoButton, Qt::NoButton);
    sendMouse(*fixture.canvas, QEvent::MouseButtonPress, press,
        Qt::LeftButton, Qt::LeftButton);
    sendMouse(*fixture.canvas, QEvent::MouseMove, press + QPointF(34.0, 19.0),
        Qt::NoButton, Qt::LeftButton);
    CHECK(fixture.canvas->transformDragging());
    CHECK(!sameTransform(fixture.transform(), original));

    QFocusEvent canvasFocusOut(QEvent::FocusOut, Qt::OtherFocusReason);
    QCoreApplication::sendEvent(fixture.canvas, &canvasFocusOut);
    settleEvents();
    CHECK(!fixture.canvas->transformDragging());
    CHECK(activeTransform(fixture));
    // Losing canvas focus aborts just the transient gesture and restores the
    // value from before that press. Editing the cached top page must not tear
    // down the persistent transform session.
    CHECK(sameTransform(fixture.transform(), original));
    x->setFocus(Qt::MouseFocusReason);
    settleEvents();
    CHECK(activeTransform(fixture));

    // Switching apps finishes a valid numeric edit just like ordinary focus
    // loss, but neither cancels the persistent session nor publishes it into
    // the global history before Apply.
    editor->selectAll();
    QTest::keyClicks(editor, QStringLiteral("391.25"));
    CHECK(sameTransform(fixture.transform(), original));
    QEvent deactivate(QEvent::ApplicationDeactivate);
    QCoreApplication::sendEvent(qApp, &deactivate);
    settleEvents();
    CHECK(activeTransform(fixture));
    CHECK(!sameTransform(fixture.transform(), original));
    CHECK(near(x->value(), 391.25, 0.011));
    CHECK(fixture.window.editorSession().history().undoDepth() == historyDepth);
    const auto edited = fixture.transform();
    auto* undo = actionWithShortcut(fixture.window, QKeySequence::Undo);
    auto* redo = actionWithShortcut(fixture.window, QKeySequence::Redo);
    CHECK(undo && redo);
    if (!undo || !redo) return;
    undo->trigger();
    settleEvents();
    CHECK(activeTransform(fixture));
    CHECK(sameTransform(fixture.transform(), original));
    redo->trigger();
    settleEvents();
    CHECK(sameTransform(fixture.transform(), edited));
    QTest::keyClick(fixture.canvas, Qt::Key_Escape);
    settleEvents();
    CHECK(sameTransform(fixture.transform(), original));
    CHECK(fixture.window.editorSession().history().undoDepth() == historyDepth);
}

void applicationSwitchAndMinimizePreserveLayerSessionActionsAndCancelOnlyLiveDrag()
{
    // Each lifecycle route must retain the same session, including its local
    // redo branch. A press that has not completed remains cancellable on focus
    // loss, independently of earlier completed actions.
    for (const int lifecycle : { 0, 1, 2 }) {
        Fixture fixture;
        CHECK(fixture.valid());
        if (!fixture.valid()) return;
        auto* undo = actionWithShortcut(fixture.window, QKeySequence::Undo);
        auto* redo = actionWithShortcut(fixture.window, QKeySequence::Redo);
        auto* x = fixture.window.findChild<QDoubleSpinBox*>(QStringLiteral("TransformXControl"));
        auto* flip = fixture.window.findChild<QToolButton*>(QStringLiteral("TransformFlipHorizontal"));
        auto* apply = fixture.window.findChild<QToolButton*>(QStringLiteral("TransformApply"));
        auto* cancel = fixture.window.findChild<QToolButton*>(QStringLiteral("TransformCancel"));
        CHECK(undo && redo && x && flip && apply && cancel);
        if (!undo || !redo || !x || !flip || !apply || !cancel) return;
        const auto original = fixture.transform();
        const auto rasterRevision = fixture.surfaceRevision();
        fixture.startWithAction();
        x->setValue(x->value() + 21);
        apply->click();
        settleEvents();
        const auto oldFuture = fixture.transform();
        undo->trigger();
        settleEvents();
        const auto undoDepth = fixture.window.editorSession().history().undoDepth();
        const auto redoDepth = fixture.window.editorSession().history().redoDepth();
        const auto redoLabel = std::string(fixture.window.editorSession().history().redoLabel());
        CHECK(redoDepth == 1);
        fixture.startWithAction();
        auto center = centerOf(fixture.canvas->logicalTransformHandles());
        dragOnCanvas(*fixture.canvas, center, center + QPointF(18, 11));
        const auto moved = fixture.transform();
        flip->click();
        settleEvents();
        const auto flipped = fixture.transform();
        undo->trigger();
        settleEvents();
        CHECK(sameTransform(fixture.transform(), moved));
        CHECK(redo->isEnabled());

        const auto deactivateAndRestore = [&] {
            if (lifecycle == 0) {
                QEvent loss(QEvent::ApplicationDeactivate);
                QCoreApplication::sendEvent(qApp, &loss);
            } else if (lifecycle == 1) {
                QEvent loss(QEvent::WindowDeactivate);
                QCoreApplication::sendEvent(&fixture.window, &loss);
            } else {
                fixture.window.showMinimized();
            }
            settleEvents();
            CHECK(fixture.window.editorSession().activeTool() == core::ToolId::Transform);
            CHECK(fixture.canvas->scene().transformOverlay.has_value());
            CHECK(!fixture.canvas->transformDragging());
            CHECK(fixture.pointerRouter->captureDomain()
                == ui::CrossWindowPointerRouter::CaptureDomain::None);
            if (lifecycle == 2) fixture.window.showNormal();
            QEvent appActive(QEvent::ApplicationActivate);
            QCoreApplication::sendEvent(qApp, &appActive);
            fixture.window.activateWindow();
            fixture.canvas->requestActivate();
            settleEvents();
            CHECK(activeTransform(fixture));
        };

        deactivateAndRestore();
        CHECK(sameTransform(fixture.transform(), moved));
        CHECK(redo->isEnabled());
        redo->trigger();
        settleEvents();
        CHECK(sameTransform(fixture.transform(), flipped));
        undo->trigger();
        settleEvents();
        CHECK(sameTransform(fixture.transform(), moved));

        center = centerOf(fixture.canvas->logicalTransformHandles());
        sendMouse(*fixture.canvas, QEvent::MouseButtonPress, center, Qt::LeftButton, Qt::LeftButton);
        sendMouse(*fixture.canvas, QEvent::MouseMove, center + QPointF(42, 23),
            Qt::NoButton, Qt::LeftButton);
        CHECK(fixture.canvas->transformDragging());
        CHECK(!sameTransform(fixture.transform(), moved));
        deactivateAndRestore();
        CHECK(sameTransform(fixture.transform(), moved));
        CHECK(redo->isEnabled()); // interrupted preview did not make a divergent action
        redo->trigger();
        settleEvents();
        CHECK(sameTransform(fixture.transform(), flipped));
        undo->trigger();
        settleEvents();

        center = centerOf(fixture.canvas->logicalTransformHandles());
        dragOnCanvas(*fixture.canvas, center, center + QPointF(-8, 16));
        const auto resumed = fixture.transform();
        CHECK(!sameTransform(resumed, moved));
        undo->trigger();
        settleEvents();
        CHECK(sameTransform(fixture.transform(), moved));
        redo->trigger();
        settleEvents();
        CHECK(sameTransform(fixture.transform(), resumed));
        CHECK(fixture.window.editorSession().history().undoDepth() == undoDepth);
        CHECK(fixture.window.editorSession().history().redoDepth() == redoDepth);
        CHECK(fixture.surfaceRevision() == rasterRevision);
        cancel->click();
        settleEvents();
        CHECK(!activeTransform(fixture));
        CHECK(sameTransform(fixture.transform(), original));
        CHECK(fixture.window.editorSession().history().undoDepth() == undoDepth);
        CHECK(fixture.window.editorSession().history().redoDepth() == redoDepth);
        CHECK(fixture.window.editorSession().history().redoLabel() == redoLabel);
        redo->trigger();
        settleEvents();
        CHECK(sameTransform(fixture.transform(), oldFuture));
    }
}

void modalWidgetsOwnTerminalKeysWithoutCancellingTheTransform()
{
    Fixture fixture;
    CHECK(fixture.valid());
    if (!fixture.valid()) {
        return;
    }
    auto* x = fixture.window.findChild<QDoubleSpinBox*>(
        QStringLiteral("TransformXControl"));
    CHECK(x);
    if (!x) {
        return;
    }

    const auto original = fixture.transform();
    fixture.startWithAction();
    x->setValue(x->value() + 28.0);
    settleEvents();
    const auto preview = fixture.transform();
    CHECK(!sameTransform(preview, original));

    QDialog dialog(&fixture.window);
    dialog.setWindowModality(Qt::ApplicationModal);
    dialog.show();
    settleEvents();
    CHECK(QApplication::activeModalWidget() == &dialog);
    QTest::keyClick(&dialog, Qt::Key_Escape);
    settleEvents();
    CHECK(activeTransform(fixture));
    CHECK(sameTransform(fixture.transform(), preview));

    dialog.close();
    settleEvents();
    QTest::keyClick(fixture.canvas, Qt::Key_Escape);
    settleEvents();
    CHECK(!activeTransform(fixture));
    CHECK(sameTransform(fixture.transform(), original));
}

void numericControlsRepublishCanonicalCoreValues()
{
    Fixture fixture;
    CHECK(fixture.valid());
    if (!fixture.valid()) {
        return;
    }
    auto* angle = fixture.window.findChild<QDoubleSpinBox*>(
        QStringLiteral("TransformAngleControl"));
    auto* scaleX = fixture.window.findChild<QDoubleSpinBox*>(
        QStringLiteral("TransformScaleXControl"));
    CHECK(angle && scaleX);
    if (!angle || !scaleX) {
        return;
    }

    fixture.startWithAction();
    angle->setValue(370.0);
    settleEvents();
    CHECK(near(angle->value(), 10.0, 0.011));
    const auto* layer = fixture.activeLayer();
    const auto* raster = layer
        ? std::get_if<core::RasterLayer>(&layer->payload) : nullptr;
    const auto values = raster && raster->surface
        ? core::valuesFromTransform(fixture.transform(), raster->surface->extent())
        : std::nullopt;
    CHECK(values.has_value());
    if (values) {
        CHECK(near(values->rotationDegrees, 10.0, 0.011));
    }

    scaleX->setValue(0.0);
    settleEvents();
    CHECK(near(scaleX->value(), core::kMinimumLayerScale * 100.0,
        0.000051));
    QTest::keyClick(fixture.canvas, Qt::Key_Escape);
    settleEvents();
}

void flipCommitsPendingNumericTextBeforeChangingScale()
{
    Fixture fixture;
    CHECK(fixture.valid());
    if (!fixture.valid()) {
        return;
    }
    auto* x = fixture.window.findChild<QDoubleSpinBox*>(
        QStringLiteral("TransformXControl"));
    auto* flip = fixture.window.findChild<QToolButton*>(
        QStringLiteral("TransformFlipHorizontal"));
    CHECK(x && flip);
    if (!x || !flip) {
        return;
    }
    const auto original = fixture.transform();
    const auto* layer = fixture.activeLayer();
    const auto* raster = layer
        ? std::get_if<core::RasterLayer>(&layer->payload) : nullptr;
    const auto originalValues = raster && raster->surface
        ? core::valuesFromTransform(original, raster->surface->extent())
        : std::nullopt;
    CHECK(originalValues.has_value());
    if (!originalValues) {
        return;
    }

    fixture.startWithAction();
    const auto desiredX = originalValues->center.x + 111.25;
    x->setFocus(Qt::OtherFocusReason);
    x->selectAll();
    QTest::keyClicks(x, QString::number(desiredX, 'f', 2));
    CHECK(sameTransform(fixture.transform(), original));

    QTest::mouseClick(flip, Qt::LeftButton);
    settleEvents();

    const auto changed = core::valuesFromTransform(
        fixture.transform(), raster->surface->extent());
    CHECK(activeTransform(fixture));
    CHECK(changed.has_value());
    if (changed) {
        CHECK(near(changed->center.x, desiredX, 0.011));
        const auto originalDeterminant = original.m00 * original.m11
            - original.m01 * original.m10;
        const auto changedMatrix = fixture.transform();
        const auto changedDeterminant = changedMatrix.m00 * changedMatrix.m11
            - changedMatrix.m01 * changedMatrix.m10;
        CHECK(originalDeterminant * changedDeterminant < 0.0);
    }
    CHECK(!x->hasFocus());
    CHECK(!QApplication::focusWidget()
        || !x->isAncestorOf(QApplication::focusWidget()));
    QTest::keyClick(fixture.canvas, Qt::Key_Escape);
    settleEvents();
    CHECK(sameTransform(fixture.transform(), original));
}

void nativeCanvasPressFlushesNumericTextBeforeBeginningTheGesture()
{
    Fixture fixture;
    CHECK(fixture.valid());
    if (!fixture.valid()) {
        return;
    }
    auto* x = fixture.window.findChild<QDoubleSpinBox*>(
        QStringLiteral("TransformXControl"));
    CHECK(x);
    if (!x) {
        return;
    }
    const auto original = fixture.transform();
    const auto* layer = fixture.activeLayer();
    const auto* raster = layer
        ? std::get_if<core::RasterLayer>(&layer->payload) : nullptr;
    const auto originalValues = raster && raster->surface
        ? core::valuesFromTransform(original, raster->surface->extent())
        : std::nullopt;
    CHECK(originalValues.has_value());
    if (!originalValues) {
        return;
    }

    fixture.startWithAction();
    const auto press = centerOf(fixture.canvas->logicalTransformHandles());
    const auto desiredX = originalValues->center.x + 1.0;
    x->setFocus(Qt::OtherFocusReason);
    x->selectAll();
    QTest::keyClicks(x, QString::number(desiredX, 'f', 2));
    // Let the focus transition from the native canvas to the QWidget editor
    // finish before starting the next physical-style press sequence.
    settleEvents();
    CHECK(sameTransform(fixture.transform(), original));

    bool beginCalled = false;
    bool beginAccepted = false;
    auto originalBegin = fixture.canvas->onTransformBegan;
    fixture.canvas->onTransformBegan = [&, originalBegin](
                                               core::TransformHandle handle,
                                               core::Vec2d position) {
        beginCalled = true;
        beginAccepted = originalBegin && originalBegin(handle, position);
        return beginAccepted;
    };

    sendMouse(*fixture.canvas, QEvent::MouseMove, press,
        Qt::NoButton, Qt::NoButton);
    sendMouse(*fixture.canvas, QEvent::MouseButtonPress, press,
        Qt::LeftButton, Qt::LeftButton);
    settleEvents();
    CHECK(beginCalled);
    CHECK(beginAccepted);
    CHECK(fixture.canvas->transformDragging());
    CHECK(activeTransform(fixture));
    CHECK(!x->hasFocus());
    CHECK(!QApplication::focusWidget()
        || !x->isAncestorOf(QApplication::focusWidget()));
    const auto flushed = core::valuesFromTransform(
        fixture.transform(), raster->surface->extent());
    CHECK(flushed.has_value());
    if (flushed) {
        CHECK(near(flushed->center.x, desiredX, 0.011));
    }

    sendMouse(*fixture.canvas, QEvent::MouseButtonRelease, press,
        Qt::LeftButton, Qt::NoButton);
    settleEvents();
    CHECK(!fixture.canvas->transformDragging());
    CHECK(activeTransform(fixture));
    QTest::keyClick(fixture.canvas, Qt::Key_Escape);
    settleEvents();
    CHECK(sameTransform(fixture.transform(), original));
}

void conflictingEditsCancelThePreviewBeforeTheirOwnCommand()
{
    {
        Fixture fixture;
        CHECK(fixture.valid());
        if (!fixture.valid()) {
            return;
        }
        auto* x = fixture.window.findChild<QDoubleSpinBox*>(
            QStringLiteral("TransformXControl"));
        auto* opacity = fixture.window.findChild<QDoubleSpinBox*>(
            QStringLiteral("LayerOpacitySlider"));
        CHECK(x && opacity);
        if (!x || !opacity) {
            return;
        }
        const auto original = fixture.transform();
        const auto historyDepth = fixture.window.editorSession().history().undoDepth();
        fixture.startWithAction();
        x->setValue(x->value() + 54.0);
        CHECK(!sameTransform(fixture.transform(), original));
        opacity->setValue(63);
        settleEvents();
        CHECK(fixture.window.editorSession().activeTool() == core::ToolId::Move);
        CHECK(!fixture.canvas->scene().transformOverlay.has_value());
        CHECK(sameTransform(fixture.transform(), original));
        CHECK(near(fixture.activeLayer()->opacity, 0.63, 0.011));
        CHECK(fixture.window.editorSession().history().undoDepth()
            == historyDepth + 1U);
        CHECK(fixture.window.editorSession().history().undoLabel()
            == std::string_view("Layer opacity"));
    }

    {
        Fixture fixture;
        CHECK(fixture.valid());
        if (!fixture.valid()) {
            return;
        }
        auto* x = fixture.window.findChild<QDoubleSpinBox*>(
            QStringLiteral("TransformXControl"));
        auto* brush = fixture.window.findChild<QAction*>(
            QStringLiteral("ToolAction_brush"));
        CHECK(x && brush);
        if (!x || !brush) {
            return;
        }
        const auto original = fixture.transform();
        const auto historyDepth = fixture.window.editorSession().history().undoDepth();
        fixture.startWithAction();
        x->setValue(x->value() - 93.0);
        CHECK(!sameTransform(fixture.transform(), original));
        brush->trigger();
        settleEvents();
        CHECK(fixture.window.editorSession().activeTool() == core::ToolId::Brush);
        CHECK(!fixture.canvas->scene().transformOverlay.has_value());
        CHECK(sameTransform(fixture.transform(), original));
        CHECK(fixture.window.editorSession().history().undoDepth() == historyDepth);
    }
}

void canvasPressRoutesAcrossTheNativePanelSurfaceUntilRelease()
{
    Fixture fixture;
    CHECK(fixture.valid());
    if (!fixture.valid()) {
        return;
    }
    auto* panel = fixture.window.findChild<QWidget*>(
        QStringLiteral("LayersPanel"));
    CHECK(panel);
    if (!panel) {
        return;
    }
    const auto original = fixture.transform();
    const auto routedBefore = fixture.pointerRouter->routedEventCount();
    fixture.startWithAction();

    const auto handles = fixture.canvas->logicalTransformHandles();
    const auto press = centerOf(handles);
    sendMouse(*fixture.canvas, QEvent::MouseMove, press,
        Qt::NoButton, Qt::NoButton);
    sendMouse(*fixture.canvas, QEvent::MouseButtonPress, press,
        Qt::LeftButton, Qt::LeftButton);
    CHECK(fixture.canvas->transformDragging());
    CHECK(fixture.pointerRouter->captureDomain()
        == ui::CrossWindowPointerRouter::CaptureDomain::NativeWindow);

    const QPointF panelPoint(panel->rect().center());
    sendMouse(*panel, QEvent::MouseMove, panelPoint,
        Qt::NoButton, Qt::LeftButton);
    sendMouse(*panel, QEvent::MouseButtonRelease, panelPoint,
        Qt::LeftButton, Qt::NoButton);
    settleEvents();
    CHECK(!fixture.canvas->transformDragging());
    CHECK(activeTransform(fixture));
    CHECK(!sameTransform(fixture.transform(), original));
    CHECK(fixture.pointerRouter->captureDomain()
        == ui::CrossWindowPointerRouter::CaptureDomain::None);
    CHECK(fixture.pointerRouter->routedEventCount() >= routedBefore + 2U);
    CHECK(QWidget::mouseGrabber() == nullptr);

    QTest::keyClick(fixture.canvas, Qt::Key_Escape);
    settleEvents();
    CHECK(sameTransform(fixture.transform(), original));
}

struct UploadCounters {
    std::uint64_t fullUploads {0};
    std::uint64_t regionalUploadBatches {0};
    std::uint64_t regionalDirtyRegions {0};
    std::uint64_t regionalUploads {0};
    std::uint64_t uploadedBytes {0};
    std::uint64_t stagingBufferAllocations {0};

    friend bool operator==(const UploadCounters&, const UploadCounters&) = default;
};

UploadCounters uploads(const render::RendererStats& stats)
{
    return {
        stats.fullUploads,
        stats.regionalUploadBatches,
        stats.regionalDirtyRegions,
        stats.regionalUploads,
        stats.uploadedBytes,
        stats.stagingBufferAllocations,
    };
}

int captureNativeTransformPreview(const QString& output)
{
    if (QGuiApplication::platformName() != QStringLiteral("wayland")) {
        std::cerr << "Native transform preview requires QT_QPA_PLATFORM=wayland\n";
        return 77;
    }
    const auto spectacle = QStandardPaths::findExecutable(
        QStringLiteral("spectacle"));
    if (spectacle.isEmpty()) {
        std::cerr << "Native transform preview requires Spectacle\n";
        return EXIT_FAILURE;
    }

    int validationWarnings=0, validationErrors=0;
    QVulkanInstance instance;
    instance.setApiVersion(QVersionNumber(1, 2));
    if (instance.supportedLayers().contains(
            QByteArrayLiteral("VK_LAYER_KHRONOS_validation"))) {
        instance.setLayers({QByteArrayLiteral("VK_LAYER_KHRONOS_validation")});
    } else {
        std::cerr << "Native transform validation requires VK_LAYER_KHRONOS_validation\n";
        return EXIT_FAILURE;
    }
    instance.installDebugOutputFilter([&](QVulkanInstance::DebugMessageSeverityFlags flags,
        QVulkanInstance::DebugMessageTypeFlags types,const void* message) {
        if(!types.testFlag(QVulkanInstance::ValidationMessage)) return false;
        if(flags.testFlag(QVulkanInstance::ErrorSeverity)) ++validationErrors;
        else if(flags.testFlag(QVulkanInstance::WarningSeverity)) ++validationWarnings;
        else return false;
        const auto* data=static_cast<const VkDebugUtilsMessengerCallbackDataEXT*>(message);
        std::cerr << (data?data->pMessage:"Vulkan validation message") << '\n';
        return false;
    });
    if (!instance.create()) {
        std::cerr << "Native transform preview could not create Vulkan\n";
        return EXIT_FAILURE;
    }

    QTemporaryDir assets;
    if (!assets.isValid()) {
        return EXIT_FAILURE;
    }
    QImage reference(512, 320, QImage::Format_RGBA8888);
    reference.fill(QColor(22, 28, 42, 255));
    {
        QPainter painter(&reference);
        painter.fillRect(0, 0, 256, 160, QColor(232, 72, 57, 255));
        painter.fillRect(256, 0, 256, 160, QColor(245, 188, 56, 255));
        painter.fillRect(0, 160, 256, 160, QColor(54, 176, 126, 255));
        painter.fillRect(256, 160, 256, 160, QColor(93, 91, 215, 255));
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(Qt::white, 14, Qt::SolidLine,
            Qt::RoundCap, Qt::RoundJoin));
        painter.drawLine(QPointF(70, 250), QPointF(390, 75));
        painter.drawLine(QPointF(390, 75), QPointF(337, 78));
        painter.drawLine(QPointF(390, 75), QPointF(367, 124));
        painter.setPen(QPen(QColor(15, 18, 26), 6));
        painter.drawText(QRect(18, 12, 120, 45), Qt::AlignCenter,
            QStringLiteral("TOP LEFT"));
    }
    const auto imagePath = assets.filePath(
        QStringLiteral("asymmetric-transform-reference.png"));
    if (!reference.save(imagePath)) {
        return EXIT_FAILURE;
    }
    QImage transparentPixel(1, 1, QImage::Format_RGBA8888);
    transparentPixel.fill(Qt::transparent);
    const auto groupImportPath = assets.filePath(QStringLiteral("group-refresh.png"));
    if (!transparentPixel.save(groupImportPath)) return EXIT_FAILURE;

    ui::MainWindow window(&instance, false, false);
    window.setUnsavedPromptEnabled(false);
    window.resize(1280, 820);
    if (!window.openImageFromPath(imagePath)) {
        return EXIT_FAILURE;
    }
    window.show();
    window.activateWindow();

    QEventLoop loop;
    QProcess capture;
    QTimer poll;
    poll.setInterval(40);
    QTimer groupPoll;
    groupPoll.setInterval(40);
    render::CanvasWindow* canvas = nullptr;
    std::optional<UploadCounters> baselineUploads;
    core::Revision surfaceRevision = 0;
    core::AffineTransform original;
    core::AffineTransform preview;
    std::uint64_t baselineFrames = 0;
    int stableUploadPolls = 0;
    bool previewStarted = false;
    bool accentRestored = false;
    bool captureStarted = false;
    bool passed = false;
    QString failure = QStringLiteral("native transform preview timed out");
    enum class GroupStage { Inactive, Warmup, Preview, Undone, Redone, Cancelled };
    GroupStage groupStage {GroupStage::Inactive};
    std::array<core::LayerId,3> groupIds {};
    std::array<core::AffineTransform,3> groupEntry {}, groupPreview {};
    std::optional<UploadCounters> groupUploads;
    std::uint64_t groupResourceGeneration {0}, groupSwapchainGeneration {0}, groupFrames {0};
    std::size_t groupHistoryDepth {0};
    int groupStablePolls {0};

    const auto fail = [&](QString detail) {
        failure = std::move(detail);
        loop.quit();
    };
    const auto finish = [&](bool success, QString detail = {}) {
        passed = success;
        failure = std::move(detail);
        loop.quit();
    };

    const auto groupMatrices = [&] {
        std::array<core::AffineTransform,3> matrices {};
        const auto* document = window.editorSession().document();
        for (std::size_t i=0; i<groupIds.size(); ++i) {
            const auto* layer = document ? document->layer(groupIds[i]) : nullptr;
            if (layer) matrices[i] = layer->localToDocument;
        }
        return matrices;
    };
    const auto sameGroup = [&](const std::array<core::AffineTransform,3>& expected) {
        const auto* document = window.editorSession().document();
        if (!document) return false;
        const auto matrices = groupMatrices();
        for (std::size_t i=0; i<groupIds.size(); ++i)
            if (!document->containsLayer(groupIds[i]) || !sameTransform(matrices[i], expected[i])) return false;
        return true;
    };
    const auto beginGroupExercise = [&] {
        auto& session = const_cast<core::EditorSession&>(window.editorSession());
        auto* document = session.document();
        if (!document || !session.activeLayer()) {
            fail(QStringLiteral("Missing native group document")); return;
        }
        core::TextLayer text;
        text.utf8 = "Shared transform";
        text.defaultStyle.sizePixels = 26;
        auto textLayer = core::Layer::text("Native group text", text);
        textLayer.localToDocument = {1, 0.15, 170, 0.1, 1, 40};
        core::ShapeLayer shape;
        shape.kind = core::ShapeKind::Ellipse;
        shape.size = {92.5,64.25};
        shape.fillColor = {38,206,222,210};
        shape.strokeEnabled = true;
        shape.strokeWidth = 5;
        auto shapeLayer = core::Layer::shape("Native group shape", shape);
        shapeLayer.localToDocument = {-1,0.2,420,0.1,1,205};
        groupIds = {*session.activeLayer(),textLayer.id,shapeLayer.id};
        if (!document->insertLayer(document->layers().size(),std::move(textLayer))
            || !document->insertLayer(document->layers().size(),std::move(shapeLayer))
            || !window.importImageAsLayerFromPath(groupImportPath)) {
            fail(QStringLiteral("Could not prepare native mixed group")); return;
        }
        // The normal import refreshes caches and the layer model. Allow those
        // initial textures to settle before measuring metadata-only group edits.
        session.setLayerSelection(groupIds,groupIds[0],groupIds[0]);
        groupEntry = groupMatrices();
        groupHistoryDepth = session.history().undoDepth();
        groupFrames = canvas->rendererStats().framesSubmitted;
        groupStage = GroupStage::Warmup;
        groupPoll.start();
    };

    QObject::connect(&groupPoll, &QTimer::timeout, &loop, [&] {
        if (!canvas || groupStage==GroupStage::Inactive) return;
        const auto stats = canvas->rendererStats();
        auto* action = window.findChild<QAction*>(QStringLiteral("LayerTransformAction"));
        auto* x = window.findChild<QDoubleSpinBox*>(QStringLiteral("TransformXControl"));
        auto* flip = window.findChild<QToolButton*>(QStringLiteral("TransformFlipHorizontal"));
        auto* apply = window.findChild<QToolButton*>(QStringLiteral("TransformApply"));
        auto* cancel = window.findChild<QToolButton*>(QStringLiteral("TransformCancel"));
        auto* undo = actionWithShortcut(window,QKeySequence::Undo);
        auto* redo = actionWithShortcut(window,QKeySequence::Redo);
        if (!action || !x || !flip || !apply || !cancel || !undo || !redo) {
            fail(QStringLiteral("Missing native group controls")); return;
        }
        if (groupStage==GroupStage::Warmup) {
            if (stats.framesSubmitted<=groupFrames) return;
            if (groupUploads && *groupUploads==uploads(stats)) ++groupStablePolls;
            else { groupUploads=uploads(stats); groupStablePolls=0; }
            if (groupStablePolls<3) return;
            const auto* document=window.editorSession().document();
            for (std::size_t i=1; i<groupIds.size(); ++i) {
                const auto* layer=document->layer(groupIds[i]);
                if (!layer || !layer->renderCache || !layer->renderCache->surface) {
                    fail(QStringLiteral("Native group text/shape cache was not rendered")); return;
                }
            }
            groupResourceGeneration=stats.resourceGeneration;
            groupSwapchainGeneration=stats.swapchainGeneration;
            groupFrames=stats.framesSubmitted;
            action->trigger();
            if (!canvas->scene().transformOverlay) {
                fail(QStringLiteral("Native group transform did not open")); return;
            }
            const auto frame=canvas->scene().transformOverlay->localToDocument.inverted();
            if (!frame) { fail(QStringLiteral("Native group frame was singular")); return; }
            x->setValue(x->value()+17);
            flip->click();
            groupPreview=groupMatrices();
            const auto delta=core::composeAffine(canvas->scene().transformOverlay->localToDocument,*frame);
            for (std::size_t i=0; i<groupIds.size(); ++i) {
                if (!sameTransform(groupPreview[i],core::composeAffine(delta,groupEntry[i]))) {
                    fail(QStringLiteral("Native mixed group did not share its pivot/delta")); return;
                }
            }
            groupStage=GroupStage::Preview;
            return;
        }
        if (stats.framesSubmitted<=groupFrames) return;
        const auto& session=window.editorSession();
        const auto* layer=session.document()->layer(groupIds[0]);
        const auto* raster=layer?std::get_if<core::RasterLayer>(&layer->payload):nullptr;
        if (!groupUploads || uploads(stats)!=*groupUploads
            || stats.resourceGeneration!=groupResourceGeneration
            || stats.swapchainGeneration!=groupSwapchainGeneration
            || !raster || !raster->surface || raster->surface->revision()!=surfaceRevision
            || session.selectedLayers().size()!=3) {
            fail(QStringLiteral("Native group transform reuploaded content or reset Vulkan resources")); return;
        }
        groupFrames=stats.framesSubmitted;
        if (groupStage==GroupStage::Preview) {
            if (!sameGroup(groupPreview) || session.history().undoDepth()!=groupHistoryDepth) {
                fail(QStringLiteral("Native group preview/history diverged")); return;
            }
            apply->click();
            if (session.history().undoDepth()!=groupHistoryDepth+2) {
                fail(QStringLiteral("Native group Apply lost individual actions")); return;
            }
            undo->trigger(); undo->trigger();
            groupStage=GroupStage::Undone;
        } else if (groupStage==GroupStage::Undone) {
            if (!sameGroup(groupEntry) || session.history().undoDepth()!=groupHistoryDepth) {
                fail(QStringLiteral("Native group undo was not atomic")); return;
            }
            redo->trigger(); redo->trigger();
            groupStage=GroupStage::Redone;
        } else if (groupStage==GroupStage::Redone) {
            if (!sameGroup(groupPreview)) { fail(QStringLiteral("Native group redo failed")); return; }
            action->trigger();
            x->setValue(x->value()-31); flip->click();
            cancel->click();
            groupStage=GroupStage::Cancelled;
        } else if (groupStage==GroupStage::Cancelled) {
            const bool valid=sameGroup(groupPreview)
                && session.history().undoDepth()==groupHistoryDepth+2
                && !canvas->scene().transformOverlay;
            groupPoll.stop();
            if (valid) std::cout << "Native Vulkan mixed-group transform: frame, common pivot, Apply/undo/redo/cancel, texture/resource reuse passed\n";
            finish(valid,valid?QString{}:QStringLiteral("Native group Cancel failed to restore its entry"));
        }
    });

    QObject::connect(&capture, &QProcess::errorOccurred, &loop,
        [&](QProcess::ProcessError) { fail(capture.errorString()); });
    QObject::connect(&capture, &QProcess::finished, &loop,
        [&](int exitCode, QProcess::ExitStatus status) {
            if (status != QProcess::NormalExit || exitCode != 0) {
                fail(QStringLiteral("Spectacle failed: %1")
                    .arg(QString::fromUtf8(capture.readAllStandardError())));
                return;
            }
            const QImage screenshot(output);
            const QSize expected {
                static_cast<int>(std::lround(
                    window.width() * window.devicePixelRatioF())),
                static_cast<int>(std::lround(
                    window.height() * window.devicePixelRatioF())),
            };
            if (screenshot.isNull() || screenshot.size() != expected) {
                fail(QStringLiteral("Spectacle captured another window or no image"));
                return;
            }

            auto* apply = window.findChild<QToolButton*>(
                QStringLiteral("TransformApply"));
            auto* undo = actionWithShortcut(window, QKeySequence::Undo);
            auto* redo = actionWithShortcut(window, QKeySequence::Redo);
            if (!apply || !undo || !redo || !canvas || !baselineUploads) {
                fail(QStringLiteral("Missing transform apply/history controls"));
                return;
            }
            apply->click();
            for (int i = 0; i < 5; ++i) undo->trigger();
            const auto* restoredDocument = window.editorSession().document();
            const auto restoredId = window.editorSession().activeLayer();
            const auto* restoredLayer = restoredDocument && restoredId
                ? restoredDocument->layer(*restoredId) : nullptr;
            if (!restoredLayer || !sameTransform(restoredLayer->localToDocument, original)) {
                fail(QStringLiteral("Per-action native transform undo lost entry geometry"));
                return;
            }
            for (int i = 0; i < 5; ++i) redo->trigger();
            QTimer::singleShot(220, &window, [&] {
                const auto* document = window.editorSession().document();
                const auto layerId = window.editorSession().activeLayer();
                const auto* layer = document && layerId
                    ? document->layer(*layerId) : nullptr;
                const auto* raster = layer
                    ? std::get_if<core::RasterLayer>(&layer->payload) : nullptr;
                if (!layer || !raster || !raster->surface) {
                    finish(false, QStringLiteral("Transform target disappeared"));
                    return;
                }
                const auto stats = canvas->rendererStats();
                const bool valid = sameTransform(layer->localToDocument, preview)
                    && raster->surface->revision() == surfaceRevision
                    && uploads(stats) == *baselineUploads
                    && stats.framesSubmitted > baselineFrames
                    && window.editorSession().history().undoDepth() == 5;
                if (!valid) {
                    finish(false, QStringLiteral("Transform changed raster upload/revision/history invariants"));
                    return;
                }
                // Keep the existing review screenshot unchanged; exercise the
                // mixed selection only after its single-target checks pass.
                beginGroupExercise();
            });
        });

    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (!canvas) {
            canvas = findCanvas();
            if (!canvas) {
                return;
            }
        }
        const auto stats = canvas->rendererStats();
        const auto currentUploads = uploads(stats);
        if (!previewStarted) {
            if (stats.framesSubmitted == 0 || stats.fullUploads == 0) {
                return;
            }
            if (baselineUploads && *baselineUploads == currentUploads) {
                ++stableUploadPolls;
            } else {
                baselineUploads = currentUploads;
                stableUploadPolls = 0;
            }
            if (stableUploadPolls < 3) {
                return;
            }

            const auto* document = window.editorSession().document();
            const auto layerId = window.editorSession().activeLayer();
            const auto* layer = document && layerId
                ? document->layer(*layerId) : nullptr;
            const auto* raster = layer
                ? std::get_if<core::RasterLayer>(&layer->payload) : nullptr;
            auto* action = window.findChild<QAction*>(
                QStringLiteral("LayerTransformAction"));
            auto* x = window.findChild<QDoubleSpinBox*>(
                QStringLiteral("TransformXControl"));
            auto* y = window.findChild<QDoubleSpinBox*>(
                QStringLiteral("TransformYControl"));
            auto* scaleX = window.findChild<QDoubleSpinBox*>(
                QStringLiteral("TransformScaleXControl"));
            auto* scaleY = window.findChild<QDoubleSpinBox*>(
                QStringLiteral("TransformScaleYControl"));
            auto* angle = window.findChild<QDoubleSpinBox*>(
                QStringLiteral("TransformAngleControl"));
            if (!layer || !raster || !raster->surface || !action
                || !x || !y || !scaleX || !scaleY || !angle) {
                fail(QStringLiteral("Missing native transform fixture controls"));
                return;
            }
            original = layer->localToDocument;
            surfaceRevision = raster->surface->revision();
            baselineFrames = stats.framesSubmitted;
            action->trigger();
            x->setValue(x->value() + 34.0);
            y->setValue(y->value() + 18.0);
            scaleX->setValue(78.0);
            scaleY->setValue(92.0);
            angle->setValue(27.0);
            const auto* changed = document->layer(*layerId);
            if (!changed || sameTransform(changed->localToDocument, original)
                || raster->surface->revision() != surfaceRevision) {
                fail(QStringLiteral("Native transform preview did not remain metadata-only"));
                return;
            }
            preview = changed->localToDocument;
            canvas->setOverlayAccent({217, 130, 49, 255});
            previewStarted = true;
            return;
        }

        if (!captureStarted && stats.framesSubmitted > baselineFrames) {
            if (!baselineUploads || uploads(stats) != *baselineUploads) {
                fail(QStringLiteral("Transform preview caused a raster upload"));
                return;
            }
            // Present a changed overlay style before restoring the shared
            // accent for review; neither style pass may upload raster data.
            if (!accentRestored) {
                canvas->setOverlayAccent(ui::editorAccent());
                baselineFrames = stats.framesSubmitted;
                accentRestored = true;
                return;
            }
            captureStarted = true;
            poll.stop();
            window.activateWindow();
            QTimer::singleShot(140, &capture, [&] {
                capture.start(spectacle, {QStringLiteral("--background"),
                    QStringLiteral("--nonotify"),
                    QStringLiteral("--activewindow"),
                    QStringLiteral("--no-decoration"),
                    QStringLiteral("--no-shadow"),
                    QStringLiteral("--output"), output});
            });
        }
    });

    poll.start();
    QTimer::singleShot(18000, &loop, &QEventLoop::quit);
    loop.exec();
    poll.stop();
    groupPoll.stop();
    if (capture.state() != QProcess::NotRunning) {
        capture.kill();
        capture.waitForFinished(1000);
    }
    window.logRendererDiagnostics();
    window.close();
    std::cout << "Group transform Vulkan warnings=" << validationWarnings << ", errors=" << validationErrors << '\n';
    if (!passed || validationWarnings || validationErrors) {
        std::cerr << failure.toStdString() << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "Native Vulkan transform preview: "
              << output.toStdString() << '\n';
    return EXIT_SUCCESS;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("ImageEditorTests"));
    QCoreApplication::setApplicationName(
        QStringLiteral("LayerTransformInteractions"));
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settingsDirectory;
    CHECK(settingsDirectory.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
        settingsDirectory.path());
    ui::applyEditorTheme(application);

    const auto nativePreview = qEnvironmentVariable(
        "IMAGEEDITOR_TEST_TRANSFORM_PREVIEW_NATIVE");
    if (!nativePreview.isEmpty()) {
        return captureNativeTransformPreview(nativePreview);
    }
    if (application.arguments().contains(QStringLiteral("--off-canvas-only"))) {
        ordinaryMoveRecoversOffCanvasLayersAndRetainsGroupSelection();
        std::cout << "Off-canvas Move interactions: " << failures << " failures\n";
        return failures ? EXIT_FAILURE : EXIT_SUCCESS;
    }
    if (application.arguments().contains(QStringLiteral("--distorted-handles-only"))) {
        distortedSideHandlesRemainHoverableAndDraggable();
        nearCollapsedPerspectiveShiftResizeDoesNotJump();
        std::cout << "Distorted handle interactions: " << failures << " failures\n";
        return failures ? EXIT_FAILURE : EXIT_SUCCESS;
    }

    transformDefaultsToLockedWithShiftFreeScaling();
    moveOptionsStayCachedAndFollowSelectionAndCanvasGeometry();
    moveNumericEditsAndFlipsHaveIndependentGlobalUndoAndPreservePixels();
    moveNumericTypingAndHeldStepsCommitOnlyAtActionBoundaries();
    moveCanvasSelectionFlushesTextToItsOriginalLayerBeforeTheDrag();
    moveOptionsDoNotClaimTypingFocusBeforeTheUserEditsAField();
    movePendingTextFinishesBeforeToolbarAndLayerListTargetChanges();
    ordinaryMoveSelectsRenderedLayerAndCreatesOnlyOneDragCommand();
    ordinaryMoveRecoversOffCanvasLayersAndRetainsGroupSelection();
    altDragDuplicatesOnceAndMovesCopiesWithAtomicHistory();
    altDragMixedSelectionSupportsCrossPanelReleaseAndFocusCancellation();
    movementSnappingModifiersPreferencesAndGuideCleanup();
    snappingPreferencesReloadIndependentlyOfDocument();
    canvasShiftSelectionAndGroupMoveShareThePanelSelection();
    panelRangesUseDisplayedRowsAndEyeControlsPreserveSelection();
    mixedGroupTransformUsesOneCommonDeltaAndRetainsIndividualActions();
    rightClickAppliesGroupTransformsAndConsumesLatePointerEvents();
    ordinaryMoveCancellationAndCrossPanelCapturePreserveGeometry();
    spacePanRoutesFromWidgetKeyRecipientsWithoutEditingTheLayer();
    spacePanRespectsTextFocusAndClearsOnLostFocusOrDeactivation();
    rotatedFlippedHitTestingAndTransformTargetIsolation();
    sessionCancelAndEmptyApplyPreservePreexistingRedoBranch();
    newGestureAfterUndoUsesRestoredBaselineAndDiscardsOnlyPendingRedo();
    numericTypingAndHeldStepsEachCreateOneAction();
    mouseGeometrySteppersReleaseTextFocusAndRouteImmediateUndo();
    typedGeometryThenMouseStepCommitsTextBeforeTheArrowAction();
    topBarButtonsShareVisibleRestingChromeAndCachedInstances();
    cornerResizeCursorsStayDiagonalOnWideTallRotatedAndFlippedFrames();
    distortedSideHandlesRemainHoverableAndDraggable();
    nearCollapsedPerspectiveShiftResizeDoesNotJump();
    rotationCursorFacesEveryTransformedCornerAndWrapsCachedAngles();
    rotationCursorTracksResolvedDragAngleAndRestoresOutsideItsHitZone();
    applicationAccentInvalidatesOnlyOverlayStyle();
    ctrlTUsesOneCachedPageWithoutRelayout();
    moveScaleAndRotateKeepIndividualUndoStepsInsideAndAfterApply();
    enterAppliesExactlyOneUndoCommand();
    escapeCancelsAndRestoresThePreviousTool();
    numericEditorsOwnTheirFirstTerminalKeyAndToolShortcuts();
    canvasFocusLossEndsOnlyTheGestureAndApplicationDeactivateFinishesNumericText();
    applicationSwitchAndMinimizePreserveLayerSessionActionsAndCancelOnlyLiveDrag();
    modalWidgetsOwnTerminalKeysWithoutCancellingTheTransform();
    numericControlsRepublishCanonicalCoreValues();
    flipCommitsPendingNumericTextBeforeChangingScale();
    nativeCanvasPressFlushesNumericTextBeforeBeginningTheGesture();
    conflictingEditsCancelThePreviewBeforeTheirOwnCommand();
    canvasPressRoutesAcrossTheNativePanelSurfaceUntilRelease();

    if (failures != 0) {
        std::cerr << failures
                  << " layer-transform interaction assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All layer-transform interaction tests passed\n";
    return EXIT_SUCCESS;
}
