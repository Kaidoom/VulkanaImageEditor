#include "imageeditor/core/ColorMath.hpp"
#include "imageeditor/core/SelectionMask.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/ColorSelector.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/PreferencesDialog.hpp"
#include "imageeditor/ui/OverlayDockWorkspace.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/ui/ToolOptionsBar.hpp"

#include <QAction>
#include <QApplication>
#include <QCursor>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QToolBar>
#include <QToolButton>
#include <QTimer>
#include <QPushButton>
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
void check(bool condition, const char* expression, int line)
{
    if (!condition) {
        std::cerr << "FAIL line " << line << ": " << expression << '\n';
        ++failures;
    }
}
#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

void settle()
{
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
}
bool waitFor(const std::function<bool()>& predicate, int timeout = 5000)
{
    QElapsedTimer timer;
    timer.start();
    do {
        settle();
        if (predicate())
            return true;
        QTest::qWait(2);
    } while (timer.elapsed() < timeout);
    return predicate();
}
void sendMouse(render::CanvasWindow& canvas, QEvent::Type type, QPointF point, Qt::MouseButton button,
    Qt::MouseButtons buttons, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    QMouseEvent event(
        type, point, point, QPointF(canvas.mapToGlobal(point.toPoint())), button, buttons, modifiers);
    QCoreApplication::sendEvent(&canvas, &event);
}
QAction* shortcut(ui::MainWindow& window, const char* sequence)
{
    for (auto* action : window.findChildren<QAction*>())
        if (action->shortcuts().contains(QKeySequence(QString::fromLatin1(sequence))))
            return action;
    return nullptr;
}

struct Fixture {
    QTemporaryDir assets;
    ui::MainWindow window;
    render::CanvasWindow* canvas { };
    ui::OverlayDockWorkspace* workspace { };
    ui::ToolOptionsBar* options { };

    explicit Fixture(QVulkanInstance* instance = nullptr)
        : window(instance, false, false)
    {
        window.setUnsavedPromptEnabled(false);
        window.resize(1480, 880);
        window.show();
        settle();
        for (auto* candidate : QGuiApplication::allWindows())
            if (candidate->objectName() == QStringLiteral("VulkanCanvasWindow"))
                canvas = dynamic_cast<render::CanvasWindow*>(candidate);
        workspace = dynamic_cast<ui::OverlayDockWorkspace*>(
            window.findChild<QWidget*>(QStringLiteral("CanvasWorkspace")));
        options = dynamic_cast<ui::ToolOptionsBar*>(
            window.findChild<QToolBar*>(QStringLiteral("ToolOptionsBar")));
        QImage image(64, 48, QImage::Format_RGBA8888);
        image.fill(Qt::transparent);
        open(image);
        colors({ { 225, 75, 40, 255 }, { 30, 110, 235, 255 }, core::ColorSlot::Primary });
    }
    ~Fixture()
    {
        window.close();
        settle();
    }
    bool valid() const { return canvas && workspace && options && window.editorSession().document(); }
    const core::Document& document() const { return *window.editorSession().document(); }
    const core::History& history() const { return window.editorSession().history(); }
    const core::Layer& layer() const { return *document().layer(*window.editorSession().activeLayer()); }
    const core::RasterSurface& surface() const
    {
        return *std::get<core::RasterLayer>(layer().payload).surface;
    }
    void open(const QImage& image)
    {
        const auto path = assets.filePath(QStringLiteral("fill-source.png"));
        CHECK(image.save(path));
        CHECK(window.openImageFromPath(path));
        settle();
    }
    void colors(core::EditorColors values)
    {
        auto* selector = dynamic_cast<ui::ColorSelector*>(
            window.findChild<QWidget*>(QStringLiteral("ColorPanelColors")));
        CHECK(selector && selector->onColorsChanged);
        if (selector && selector->onColorsChanged)
            selector->onColorsChanged(values);
    }
    QAction* action(const char* name) const
    {
        return window.findChild<QAction*>(QString::fromLatin1(name));
    }
    QToolButton* button(const char* name) const
    {
        return window.findChild<QToolButton*>(QString::fromLatin1(name));
    }
    void trigger(const char* name)
    {
        auto* a = action(name);
        CHECK(a);
        if (a)
            a->trigger();
        settle();
    }
    void click(const char* name)
    {
        auto* b = button(name);
        CHECK(b);
        if (b)
            b->click();
        settle();
    }
    void number(const char* name, double value)
    {
        auto* field = window.findChild<QDoubleSpinBox*>(QString::fromLatin1(name));
        CHECK(field);
        if (field)
            field->setValue(value);
        settle();
    }
    void undo()
    {
        auto* a = shortcut(window, "Ctrl+Z");
        CHECK(a);
        if (a)
            a->trigger();
        settle();
    }
    void redo()
    {
        auto* a = shortcut(window, "Ctrl+Shift+Z");
        CHECK(a);
        if (a)
            a->trigger();
        settle();
    }
    void waitIdle()
    {
        CHECK(waitFor([this] {
            const auto* foreground = action("FillForegroundAction");
            return foreground && foreground->isEnabled();
        }));
    }
    QPointF logical(core::Vec2d point) const
    {
        const auto extent = document().canvas().extent;
        const auto& scene = canvas->scene();
        const auto position = scene.viewport.documentToViewport(
            point, { double(extent.width), double(extent.height) }, scene.logicalViewport);
        return { position.x, position.y };
    }
    void canvasClick(core::Vec2d point)
    {
        sendMouse(*canvas, QEvent::MouseMove, logical(point), Qt::NoButton, Qt::NoButton);
        sendMouse(*canvas, QEvent::MouseButtonPress, logical(point), Qt::LeftButton, Qt::LeftButton);
        sendMouse(*canvas, QEvent::MouseButtonRelease, logical(point), Qt::LeftButton, Qt::NoButton);
        settle();
    }
    void rectangle(core::Vec2d a, core::Vec2d b, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
    {
        trigger("ToolAction_marquee");
        sendMouse(*canvas, QEvent::MouseMove, logical(a), Qt::NoButton, Qt::NoButton, modifiers);
        sendMouse(*canvas, QEvent::MouseButtonPress, logical(a), Qt::LeftButton, Qt::LeftButton, modifiers);
        sendMouse(*canvas, QEvent::MouseMove, logical(b), Qt::NoButton, Qt::LeftButton, modifiers);
        sendMouse(*canvas, QEvent::MouseButtonRelease, logical(b), Qt::LeftButton, Qt::NoButton, modifiers);
        settle();
    }
    std::vector<std::byte> pixels() const
    {
        const auto extent = surface().extent();
        std::vector<std::byte> result(std::size_t(extent.width) * extent.height * 4);
        surface().copyRgba8(
            { 0, 0, int(extent.width), int(extent.height) }, result, std::size_t(extent.width) * 4);
        return result;
    }
    core::Rgba8 pixel(int x, int y) const
    {
        std::array<std::byte, 4> bytes { };
        surface().copyRgba8({ x, y, 1, 1 }, bytes, 4);
        return { std::to_integer<std::uint8_t>(bytes[0]), std::to_integer<std::uint8_t>(bytes[1]),
            std::to_integer<std::uint8_t>(bytes[2]), std::to_integer<std::uint8_t>(bytes[3]) };
    }
};

void keyboardEraseAndNudges(Fixture& f)
{
    QImage image(64, 48, QImage::Format_RGBA8888);
    image.fill(QColor(80, 120, 160, 255));
    f.open(image);
    f.rectangle({8,8}, {40,35});
    f.rectangle({14,14}, {20,20}, Qt::AltModifier);
    if (auto* focus = QApplication::focusWidget()) focus->clearFocus();
    const auto mask = f.document().selection();
    const auto pixels = f.pixels();
    const auto matrix = f.layer().localToDocument;
    const auto depth = f.history().undoDepth();
    QTest::keyClick(f.canvas, Qt::Key_Delete);
    f.waitIdle();
    CHECK(f.history().undoDepth() == depth+1);
    CHECK(f.pixel(10,10).alpha == 0 && f.pixel(16,16).alpha == 255 && f.pixel(0,0).alpha == 255);
    CHECK(f.document().selection() == mask && f.layer().localToDocument == matrix);
    const auto erased = f.pixels();
    const auto revision = f.surface().revision();
    QTest::keyClick(f.canvas, Qt::Key_Delete); f.waitIdle();
    CHECK(f.history().undoDepth() == depth+1 && f.surface().revision() == revision);
    f.undo(); CHECK(f.pixels() == pixels);
    f.redo(); CHECK(f.pixels() == erased);
    f.undo();
    QTest::keyClick(f.canvas, Qt::Key_Right);
    CHECK(f.document().selection()->bounds().x == mask->bounds().x+1);
    CHECK(f.layer().localToDocument == matrix && f.pixels() == pixels);
    QTest::keyClick(f.canvas, Qt::Key_Down, Qt::ShiftModifier);
    CHECK(f.document().selection()->bounds().y == mask->bounds().y+10);
    f.undo(); f.undo(); CHECK(f.document().selection() == mask);
    // General preferences applies the larger step immediately, persists it,
    // and leaves ordinary arrows at one pixel.
    QTimer::singleShot(0, &f.window, [&] {
        auto* field = f.window.findChild<QDoubleSpinBox*>("ShiftNudgePixels");
        auto* ok = f.window.findChild<QPushButton*>("PreferencesOk");
        CHECK(field && ok);
        if (field && ok) { CHECK(field->value() == 10); field->setValue(7); ok->click(); }
    });
    f.trigger("PreferencesAction");
    CHECK(QSettings().value("preferences/canvas/shiftNudgePixels").toInt() == 7);
    if (auto* focus = QApplication::focusWidget()) focus->clearFocus();
    f.trigger("ToolAction_move");
    QTest::keyClick(f.canvas, Qt::Key_Left, Qt::ShiftModifier);
    CHECK(std::abs(f.layer().localToDocument.m02 - (matrix.m02-7)) < 1e-8);
    QTest::keyClick(f.canvas, Qt::Key_Up);
    CHECK(std::abs(f.layer().localToDocument.m12 - (matrix.m12-1)) < 1e-8);
    CHECK(f.document().selection() == mask && f.pixels() == pixels);
    f.undo(); f.undo(); CHECK(f.layer().localToDocument == matrix);
    // An explicit transform retains each nudge in its pending history, and
    // whole-session cancellation restores the pre-existing redo branch.
    const auto redo = f.history().redoDepth();
    f.trigger("LayerTransformAction");
    QTest::keyClick(f.canvas, Qt::Key_Right);
    CHECK(std::abs(f.layer().localToDocument.m02 - (matrix.m02+1)) < 1e-8);
    f.undo(); CHECK(f.layer().localToDocument == matrix);
    f.redo();
    QTest::keyClick(f.canvas, Qt::Key_Escape);
    CHECK(f.layer().localToDocument == matrix && f.history().redoDepth() == redo);
    f.trigger("ToolAction_move");
    // Use a plain text field on the same panel plane to check key ownership.
    QLineEdit field(f.workspace->panelOverlay());
    field.setText("123"); field.show(); field.setFocus(); field.setCursorPosition(1);
    const auto beforeField = f.history().undoDepth();
    QTest::keyClick(&field, Qt::Key_Delete);
    QTest::keyClick(&field, Qt::Key_Right);
    CHECK(field.text() == "13" && field.cursorPosition() == 2);
    CHECK(f.history().undoDepth() == beforeField && f.layer().localToDocument == matrix);
    field.clearFocus(); field.hide();
    f.trigger("DeselectAction");
    CHECK(!f.document().selection() && f.document().lastSelection());
    QTest::keyClick(f.canvas,Qt::Key_D,Qt::ControlModifier|Qt::ShiftModifier);
    CHECK(f.document().selection() && f.document().selection()->equivalent(*mask));
    f.trigger("DeselectAction");
    QTest::keyClick(f.canvas,Qt::Key_Delete); f.waitIdle();
    CHECK(f.pixel(0,0).alpha==0 && f.pixel(16,16).alpha==0 && f.document().layers().size()==1);
    f.undo(); CHECK(f.pixels()==pixels);
    // Group movement: one atomic translation, preserving an existing affine.
    auto& session = const_cast<core::EditorSession&>(f.window.editorSession());
    auto second = core::Layer::raster("Second", std::make_shared<core::ContiguousRasterSurface>(core::Extent2u{16,16}));
    const auto secondId = second.id;
    second.localToDocument = {.m00=-.8, .m01=.3, .m02=12, .m10=.2, .m11=1.4, .m12=9};
    const auto secondMatrix = second.localToDocument;
    CHECK(session.document()->insertLayer(0, std::move(second)));
    const std::array ids{f.layer().id, secondId};
    session.setLayerSelection(ids, ids.front());
    const auto groupDepth = f.history().undoDepth();
    QTest::keyClick(f.canvas, Qt::Key_Right);
    CHECK(f.history().undoDepth() == groupDepth+1);
    auto translated = secondMatrix; translated.m02 += 1;
    const auto moved = f.document().layer(secondId)->localToDocument;
    CHECK(std::abs(moved.m02-translated.m02)<1e-8 && std::abs(moved.m00-translated.m00)<1e-8);
    f.undo(); CHECK(f.document().layer(secondId)->localToDocument == secondMatrix);
}

void controlsAreCachedAndDocumentingTheImplementedTools()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid())
        return;
    const auto geometry = f.workspace->canvasContainer()->geometry();
    auto* opacity = f.window.findChild<QDoubleSpinBox*>(QStringLiteral("FillOpacityControl"));
    auto* tolerance = f.window.findChild<QDoubleSpinBox*>(QStringLiteral("FillToleranceControl"));
    CHECK(opacity && tolerance);
    auto* fillPage = f.options->pageForTool(core::ToolId::Fill);
    auto* pickerPage = f.options->pageForTool(core::ToolId::Eyedropper);
    CHECK(fillPage && pickerPage && fillPage != pickerPage);
    for (const auto* name : { "FillModeSelection", "FillModeContiguous", "MoveTransform",
             "SelectionTransform", "TransformModeActive" }) {
        auto* button = f.button(name);
        CHECK(button && button->property("toolOptionsButton").toBool());
        if (button)
            CHECK(!button->toolTip().isEmpty());
    }
    for (const auto* name : { "FillModeSelection", "FillModeContiguous", "SelectionModeReplace",
             "SelectionModeAdd", "SelectionModeSubtract", "SelectionModeIntersect", "MoveTransform",
             "SelectionTransform", "TransformModeActive" }) {
        auto* button = f.button(name);
        CHECK(button && button->toolButtonStyle() == Qt::ToolButtonIconOnly);
        if (button) {
            CHECK(!button->icon().isNull());
            CHECK(!button->accessibleName().isEmpty());
        }
    }
    for (const auto* name : { "FillSourceForeground", "FillSourceBackground", "FillApply" })
        CHECK(!f.window.findChild<QWidget*>(QString::fromLatin1(name)));
    bool hasModeLabel = false;
    for (auto* label : fillPage->findChildren<QLabel*>())
        hasModeLabel |= label->text() == QStringLiteral("Fill mode");
    CHECK(hasModeLabel);
    f.trigger("ToolAction_fill");
    if (opacity && tolerance) {
        const auto controlRect
            = [&f](QWidget* widget) { return QRect(widget->mapTo(&f.window, QPoint { }), widget->size()); };
        CHECK(tolerance->isVisible() && !tolerance->isEnabled());
        const auto pageRect = controlRect(fillPage);
        const auto opacityRect = controlRect(opacity);
        const auto toleranceRect = controlRect(tolerance);
        const auto selectionRect = controlRect(f.button("FillModeSelection"));
        const auto contiguousRect = controlRect(f.button("FillModeContiguous"));
        for (int pass = 0; pass < 3; ++pass) {
            f.click("FillModeContiguous");
            CHECK(f.button("FillModeContiguous")->isChecked());
            CHECK(!f.button("FillModeSelection")->isChecked());
            CHECK(tolerance->isVisible() && tolerance->isEnabled());
            f.number("FillToleranceControl", 42);
            CHECK(controlRect(fillPage) == pageRect);
            CHECK(controlRect(opacity) == opacityRect);
            CHECK(controlRect(tolerance) == toleranceRect);
            CHECK(controlRect(f.button("FillModeSelection")) == selectionRect);
            CHECK(controlRect(f.button("FillModeContiguous")) == contiguousRect);
            f.click("FillModeSelection");
            CHECK(f.button("FillModeSelection")->isChecked());
            CHECK(!f.button("FillModeContiguous")->isChecked());
            CHECK(tolerance->isVisible() && !tolerance->isEnabled());
            CHECK(tolerance->value() == 42);
            CHECK(controlRect(fillPage) == pageRect);
            CHECK(controlRect(opacity) == opacityRect);
            CHECK(controlRect(tolerance) == toleranceRect);
            CHECK(controlRect(f.button("FillModeSelection")) == selectionRect);
            CHECK(controlRect(f.button("FillModeContiguous")) == contiguousRect);
            CHECK(f.workspace->canvasContainer()->geometry() == geometry);
        }
    }
    CHECK(!f.window.findChild<QWidget*>(QStringLiteral("StartLayerTransform")));
    for (int pass = 0; pass < 3; ++pass) {
        for (const auto* name :
            { "ToolAction_fill", "ToolAction_eyedropper", "ToolAction_move", "ToolAction_marquee" })
            f.trigger(name);
        CHECK(f.options->pageForTool(core::ToolId::Fill) == fillPage);
        CHECK(f.window.findChild<QDoubleSpinBox*>(QStringLiteral("FillOpacityControl")) == opacity);
        CHECK(f.window.findChild<QDoubleSpinBox*>(QStringLiteral("FillToleranceControl")) == tolerance);
        CHECK(f.workspace->canvasContainer()->geometry() == geometry);
    }
    f.trigger("ToolAction_move");
    const auto xOf = [&f](const char* name) {
        auto* control = f.window.findChild<QWidget*>(QString::fromLatin1(name));
        CHECK(control);
        return control ? control->mapTo(&f.window, QPoint { }).x() : 0;
    };
    CHECK(xOf("MoveActiveLayerOnly") < xOf("MoveXControl"));
    CHECK(xOf("MoveSelectUnderMouse") < xOf("MoveXControl"));
    auto* movePage = f.options->pageForTool(core::ToolId::Move);
    bool hasSelectionLabel = false;
    if (movePage) {
        for (auto* label : movePage->findChildren<QLabel*>())
            hasSelectionLabel |= label->text() == QStringLiteral("Selection mode");
    }
    CHECK(hasSelectionLabel);
    auto* activeOnly = f.button("MoveActiveLayerOnly");
    auto* underMouse = f.button("MoveSelectUnderMouse");
    CHECK(activeOnly && underMouse);
    if (activeOnly && underMouse) {
        CHECK(underMouse->isChecked() && !activeOnly->isChecked());
        for (auto* mode : {activeOnly, underMouse}) {
            CHECK(!mode->icon().isNull());
            CHECK(!mode->toolTip().isEmpty());
            CHECK(!mode->accessibleName().isEmpty());
            CHECK(mode->toolButtonStyle() == Qt::ToolButtonIconOnly);
            CHECK(mode->property("toolOptionsButton").toBool());
        }
        const auto historyDepth = f.history().undoDepth();
        const auto pageGeometry = movePage->geometry();
        f.click("MoveActiveLayerOnly");
        CHECK(activeOnly->isChecked() && !underMouse->isChecked());
        f.click("MoveActiveLayerOnly");
        CHECK(activeOnly->isChecked() && !underMouse->isChecked());
        f.click("MoveSelectUnderMouse");
        CHECK(underMouse->isChecked() && !activeOnly->isChecked());
        CHECK(movePage->geometry() == pageGeometry);
        CHECK(f.workspace->canvasContainer()->geometry() == geometry);
        CHECK(f.history().undoDepth() == historyDepth);
    }
    CHECK(xOf("MoveFlipHorizontal") < xOf("MoveFlipVertical"));
    CHECK(xOf("MoveFlipVertical") < xOf("MoveTransform"));
    f.click("MoveTransform");
    CHECK(f.window.editorSession().activeTool() == core::ToolId::Transform);
    CHECK(f.button("TransformModeActive") && f.button("TransformModeActive")->isChecked());
    f.click("TransformCancel");
    CHECK(!f.button("MoveTransform")->isChecked());
    f.rectangle({ 8, 6 }, { 22, 20 });
    auto* transform = shortcut(f.window, "Ctrl+T");
    CHECK(transform);
    if (transform)
        transform->trigger();
    settle();
    CHECK(f.button("TransformModeActive") && f.button("TransformModeActive")->isChecked());
    f.click("TransformCancel");
    CHECK(f.window.editorSession().activeTool() == core::ToolId::Marquee);
}

void selectionFillPreservesHolesMaskGeometryAndOneHistoryStep()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid())
        return;
    f.rectangle({ 4, 4 }, { 30, 25 });
    f.rectangle({ 10, 10 }, { 17, 18 }, Qt::AltModifier);
    f.rectangle({ 43, 6 }, { 58, 20 }, Qt::ShiftModifier);
    const auto mask = f.document().selection();
    const auto revision = f.document().selectionRevision();
    const auto geometry = f.layer().localToDocument;
    const auto before = f.pixels();
    const auto depth = f.history().undoDepth();
    f.trigger("FillForegroundAction");
    f.waitIdle();
    CHECK(f.history().undoDepth() == depth + 1);
    CHECK(f.document().selection() == mask && f.document().selectionRevision() == revision);
    CHECK(f.layer().localToDocument == geometry);
    CHECK(f.window.editorSession().activeTool() == core::ToolId::Marquee);
    const auto color = f.window.editorSession().foregroundColor();
    for (int y = 0; y < 48; ++y)
        for (int x = 0; x < 64; ++x)
            CHECK(f.pixel(x, y) == (mask->coverageAtDocumentPixel(x, y) ? color : core::Rgba8 { }));
    const auto after = f.pixels();
    f.undo();
    CHECK(f.pixels() == before);
    CHECK(f.document().selection() == mask);
    f.redo();
    CHECK(f.pixels() == after);
    // A repeated opaque fill must preserve an existing redo branch.
    f.trigger("InvertSelectionAction");
    f.undo();
    const auto undo = f.history().undoDepth(), redo = f.history().redoDepth();
    const auto rasterRevision = f.surface().revision();
    f.trigger("FillForegroundAction");
    f.waitIdle();
    CHECK(f.history().undoDepth() == undo && f.history().redoDepth() == redo);
    CHECK(f.surface().revision() == rasterRevision);
    CHECK(f.pixels() == after);
}

void canvasFillsUseTheHighlightedColorWithoutSeparateSourceControls()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid())
        return;
    const core::EditorColors colors { { 225, 75, 40, 255 }, { 30, 110, 235, 128 },
        core::ColorSlot::Secondary };
    f.colors(colors);
    f.trigger("ToolAction_fill");
    CHECK(f.button("FillModeSelection")->isChecked());
    f.number("FillOpacityControl", 50);
    f.canvasClick({ 3.5, 3.5 });
    f.waitIdle();
    CHECK(f.pixel(3, 3) == core::Rgba8({ 30, 110, 235, 64 }));
    f.undo();
    auto primarySelected = colors;
    primarySelected.active = core::ColorSlot::Primary;
    f.colors(primarySelected);
    f.canvasClick({ 3.5, 3.5 });
    f.waitIdle();
    CHECK(f.pixel(3, 3) == core::Rgba8({ 225, 75, 40, 128 }));
    CHECK(f.window.editorSession().colors() == primarySelected);
    f.undo();
    f.colors(colors);
    f.number("FillOpacityControl", 100);
    // Explicit Edit commands remain available, but canvas fills have no
    // separate source state: they always follow the shared highlighted swatch.
    f.trigger("FillBackgroundAction");
    f.waitIdle();
    CHECK(f.pixel(3, 3) == colors.primary);
    f.undo();
    f.rectangle({ 12, 10 }, { 12, 10 });
    CHECK(f.document().selection() && f.document().selection()->bounds().empty());
    const auto before = f.pixels();
    const auto depth = f.history().undoDepth();
    const auto revision = f.surface().revision();
    f.trigger("FillForegroundAction");
    f.waitIdle();
    CHECK(f.history().undoDepth() == depth && f.surface().revision() == revision);
    CHECK(f.pixels() == before);
}

void bucketCursorIsCachedAndRestoredAfterPanningAndToolChanges()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid())
        return;
    f.trigger("ToolAction_fill");
    CHECK(f.canvas->cursor().shape() == Qt::BitmapCursor);
    const auto bucket = f.canvas->cursor();
    CHECK(!bucket.pixmap().isNull());
    CHECK(bucket.pixmap().rect().contains(bucket.hotSpot()));
    const auto point = f.logical({ 24, 20 });
    for (int i = 0; i < 10; ++i)
        sendMouse(*f.canvas, QEvent::MouseMove, point + QPointF(i, i), Qt::NoButton, Qt::NoButton);
    CHECK(f.canvas->cursor().pixmap().cacheKey() == bucket.pixmap().cacheKey());
    sendMouse(*f.canvas, QEvent::MouseButtonPress, point, Qt::MiddleButton, Qt::MiddleButton);
    CHECK(f.canvas->cursor().shape() == Qt::ClosedHandCursor);
    sendMouse(*f.canvas, QEvent::MouseMove, point + QPointF(20, 10), Qt::NoButton, Qt::MiddleButton);
    sendMouse(
        *f.canvas, QEvent::MouseButtonRelease, point + QPointF(20, 10), Qt::MiddleButton, Qt::NoButton);
    CHECK(f.canvas->cursor().shape() == Qt::BitmapCursor);
    CHECK(f.canvas->cursor().pixmap().cacheKey() == bucket.pixmap().cacheKey());
    f.trigger("ToolAction_marquee");
    CHECK(f.canvas->cursor().shape() == Qt::CrossCursor);
    f.trigger("ToolAction_fill");
    CHECK(f.canvas->cursor().shape() == Qt::BitmapCursor);
    CHECK(f.canvas->cursor().pixmap().cacheKey() == bucket.pixmap().cacheKey());
    CHECK(f.history().undoDepth() == 0);
}

void contiguousClickIsFourConnectedAndDoesNotLeakThroughSelectionHoles()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid())
        return;
    QImage image(64, 48, QImage::Format_RGBA8888);
    image.fill(QColor(80, 80, 80, 255));
    for (int y = 4; y < 44; ++y) {
        for (int x = 4; x < 28; ++x)
            image.setPixelColor(x, y, QColor(10, 10, 10, 255));
        for (int x = 36; x < 60; ++x)
            image.setPixelColor(x, y, QColor(10, 10, 10, 255));
    }
    f.open(image);
    f.rectangle({ 1, 1 }, { 63, 47 });
    f.rectangle({ 10, 10 }, { 18, 18 }, Qt::AltModifier);
    f.trigger("ToolAction_fill");
    f.click("FillModeContiguous");
    f.number("FillToleranceControl", 0);
    const auto mask = f.document().selection();
    const auto depth = f.history().undoDepth();
    const auto before = f.pixels();
    f.canvasClick({ 6.5, 6.5 });
    f.waitIdle();
    CHECK(f.history().undoDepth() == depth + 1);
    CHECK(f.pixel(6, 6) == f.window.editorSession().foregroundColor());
    CHECK(f.pixel(12, 12) == core::Rgba8({ 10, 10, 10, 255 }));
    CHECK(f.pixel(40, 12) == core::Rgba8({ 10, 10, 10, 255 }));
    CHECK(f.pixel(31, 12) == core::Rgba8({ 80, 80, 80, 255 }));
    CHECK(f.document().selection() == mask);
    const auto filled = f.pixels();
    f.undo();
    CHECK(f.pixels() == before);
    f.redo();
    CHECK(f.pixels() == filled);
    f.undo();
    f.number("FillToleranceControl", 100);
    f.canvasClick({ 6.5, 6.5 });
    f.waitIdle();
    CHECK(f.pixel(40, 12) == f.window.editorSession().foregroundColor());
    CHECK(f.pixel(31, 12) == f.window.editorSession().foregroundColor());
    CHECK(f.pixel(12, 12) == core::Rgba8({ 10, 10, 10, 255 }));
}

void transformedTargetsKeepTheirGeometryAndUseDocumentSpaceCoverage()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid())
        return;
    f.trigger("ToolAction_move");
    f.number("MoveAngleControl", 33);
    f.click("MoveFlipHorizontal");
    f.rectangle({ 9, 8 }, { 53, 39 });
    f.number("SelectionAngleControl", 17);
    const auto mask = f.document().selection();
    CHECK(mask);
    const auto matrix = f.layer().localToDocument;
    const auto before = f.pixels();
    const auto depth = f.history().undoDepth();
    f.trigger("FillForegroundAction");
    f.waitIdle();
    CHECK(f.history().undoDepth() == depth + 1);
    CHECK(f.layer().localToDocument == matrix);
    CHECK(f.document().selection() == mask);
    int transparent = 0, partial = 0, solid = 0;
    for (int y = 0; y < 48; ++y)
        for (int x = 0; x < 64; ++x) {
            const auto point = matrix.map({ double(x) + .5, double(y) + .5 });
            const auto coverage = point.x >= 0 && point.y >= 0 && point.x < 64 && point.y < 48
                ? mask->coverageAtDocumentPixel(int(std::floor(point.x)), int(std::floor(point.y)))
                : 0;
            const auto pixel = f.pixel(x, y);
            CHECK(pixel.alpha == coverage);
            if (!coverage)
                ++transparent;
            else if (coverage != 255)
                ++partial;
            else
                ++solid;
        }
    CHECK(transparent > 0 && partial > 0 && solid > 0);
    const auto after = f.pixels();
    f.undo();
    CHECK(f.pixels() == before);
    f.redo();
    CHECK(f.pixels() == after);
}

void shortcutsRespectTextFieldsAndMenuFillsDoNotChangeTools()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid())
        return;
    auto* foreground = f.action("FillForegroundAction");
    auto* background = f.action("FillBackgroundAction");
    CHECK(foreground && background);
    if (!foreground || !background)
        return;
    CHECK(foreground->shortcuts().contains(QKeySequence(QStringLiteral("Alt+Backspace"))));
    CHECK(background->shortcuts().contains(QKeySequence(QStringLiteral("Ctrl+Backspace"))));
    f.canvas->requestActivate();
    settle();
    QTest::keyClick(f.canvas, Qt::Key_G);
    CHECK(f.window.editorSession().activeTool() == core::ToolId::Fill);
    f.trigger("ToolAction_move");
    QTest::keyClick(f.canvas, Qt::Key_Backspace, Qt::AltModifier);
    f.waitIdle();
    CHECK(f.pixel(2, 2) == f.window.editorSession().foregroundColor());
    CHECK(f.window.editorSession().activeTool() == core::ToolId::Move);
    f.undo();
    QTest::keyClick(f.canvas, Qt::Key_Backspace, Qt::ControlModifier);
    f.waitIdle();
    CHECK(f.pixel(2, 2) == f.window.editorSession().colors().secondary);
    f.undo();
    QLineEdit field(&f.window);
    field.setGeometry(80, 80, 190, 32);
    field.setText(QStringLiteral("two words"));
    field.show();
    f.window.activateWindow();
    field.setFocus();
    settle();
    CHECK(field.hasFocus());
    const auto before = f.pixels();
    const auto depth = f.history().undoDepth();
    const auto redo = f.history().redoDepth();
    QTest::keyClick(&field, Qt::Key_G);
    QTest::keyClick(&field, Qt::Key_Backspace, Qt::ControlModifier);
    QTest::keyClick(&field, Qt::Key_Backspace, Qt::AltModifier);
    settle();
    CHECK(f.window.editorSession().activeTool() == core::ToolId::Move);
    CHECK(f.pixels() == before);
    CHECK(f.history().undoDepth() == depth && f.history().redoDepth() == redo);
}

void cancellingChunkedFillsRestoresPixelsAndPreservesHistory()
{
    for (const bool escape : { false, true }) {
        Fixture f;
        CHECK(f.valid());
        if (!f.valid())
            continue;
        QImage image(1024, 768, QImage::Format_RGBA8888);
        image.fill(QColor(15, 45, 85, 191));
        f.open(image);
        f.trigger("SelectAllAction");
        f.trigger("InvertSelectionAction");
        f.undo();
        const auto mask = f.document().selection();
        const auto selectionRevision = f.document().selectionRevision();
        const auto geometry = f.layer().localToDocument;
        const auto before = f.pixels();
        const auto revision = f.surface().revision();
        const auto canvasGeometry = f.workspace->canvasContainer()->geometry();
        const auto undo = f.history().undoDepth(), redo = f.history().redoDepth();
        const auto memory = f.history().memoryUsed();
        CHECK(redo > 0);
        f.trigger("ToolAction_fill");
        f.canvasClick({ 400.5, 300.5 });
        // Observe a genuinely live, partially applied multi-chunk operation,
        // not just cancellation before its first timer callback.
        CHECK(waitFor([&] { return f.surface().revision() > revision; }));
        auto* progress = f.window.findChild<QWidget*>(QStringLiteral("FillProgress"));
        auto* cancel = f.button("FillCancel");
        CHECK(progress && progress->isVisible() && cancel);
        CHECK(!f.action("FillForegroundAction")->isEnabled());
        CHECK(f.workspace->canvasContainer()->geometry() == canvasGeometry);
        CHECK(f.history().undoDepth() == undo && f.history().redoDepth() == redo);
        CHECK(f.pixels() != before);
        if (escape) {
            f.canvas->requestActivate();
            settle();
            QTest::keyClick(f.canvas, Qt::Key_Escape);
        } else if (cancel) {
            QTest::mouseClick(cancel, Qt::LeftButton);
        }
        f.waitIdle();
        CHECK(f.pixels() == before);
        CHECK(f.document().selection() == mask);
        CHECK(f.document().selectionRevision() == selectionRevision);
        CHECK(f.layer().localToDocument == geometry);
        CHECK(f.history().undoDepth() == undo && f.history().redoDepth() == redo);
        CHECK(f.history().memoryUsed() == memory);
        CHECK(progress && !progress->isVisible());
        CHECK(f.workspace->canvasContainer()->geometry() == canvasGeometry);
        const auto restoredRevision = f.surface().revision();
        // A stopped single-shot must not later apply another queued span.
        QTest::qWait(25);
        CHECK(f.surface().revision() == restoredRevision);
        CHECK(f.pixels() == before);
        CHECK(f.history().undoDepth() == undo && f.history().redoDepth() == redo);
        f.redo();
        CHECK(f.document().selection() && f.document().selection()->bounds().empty());
    }
}

void cancellingDiscoveryAndClosingTheWindowNeverLeavesPartialEdits()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid())
        return;
    QImage image(1024, 768, QImage::Format_RGBA8888);
    image.fill(QColor(35, 55, 95, 255));
    f.open(image);
    f.trigger("ToolAction_fill");
    f.click("FillModeContiguous");
    f.number("FillToleranceControl", 0);
    const auto before = f.pixels();
    const auto depth = f.history().undoDepth();
    const auto revision = f.surface().revision();
    f.canvasClick({ 400.5, 300.5 });
    CHECK(!f.action("FillForegroundAction")->isEnabled());
    CHECK(f.surface().revision() == revision);
    f.click("FillCancel");
    f.waitIdle();
    CHECK(f.pixels() == before && f.history().undoDepth() == depth);
    CHECK(f.surface().revision() == revision);
    f.click("FillModeSelection");
    f.canvasClick({ 400.5, 300.5 });
    CHECK(waitFor([&] { return f.surface().revision() > revision; }));
    CHECK(!f.action("FillForegroundAction")->isEnabled());
    f.window.close();
    settle();
    CHECK(f.pixels() == before && f.history().undoDepth() == depth);
    const auto restored = f.surface().revision();
    QTest::qWait(25);
    CHECK(f.surface().revision() == restored);
}

int nativeFillValidation()
{
    if (QGuiApplication::platformName() != QStringLiteral("wayland")
        && QGuiApplication::platformName() != QStringLiteral("xcb"))
        return 77;
    std::atomic_uint64_t warnings { 0 }, errors { 0 };
    QVulkanInstance instance;
    instance.setApiVersion(QVersionNumber(1, 2));
    if (!instance.supportedLayers().contains(QByteArrayLiteral("VK_LAYER_KHRONOS_validation"))) {
        std::cerr << "Fill native validation requires VK_LAYER_KHRONOS_validation\n";
        return EXIT_FAILURE;
    }
    instance.setLayers({ QByteArrayLiteral("VK_LAYER_KHRONOS_validation") });
    instance.installDebugOutputFilter(
        [&](QVulkanInstance::DebugMessageSeverityFlags severity,
            QVulkanInstance::DebugMessageTypeFlags type, const void* message) {
            if (!type.testFlag(QVulkanInstance::ValidationMessage))
                return false;
            if (severity.testFlag(QVulkanInstance::ErrorSeverity))
                ++errors;
            else if (severity.testFlag(QVulkanInstance::WarningSeverity))
                ++warnings;
            else
                return false;
            const auto* data = static_cast<const VkDebugUtilsMessengerCallbackDataEXT*>(message);
            std::cerr << "Vulkan fill validation: " << (data ? data->pMessage : "unknown") << '\n';
            return true;
        });
    if (!instance.create())
        return EXIT_FAILURE;
    {
        Fixture f(&instance);
        CHECK(f.valid());
        if (!f.valid())
            return EXIT_FAILURE;
        CHECK(waitFor([&] { return f.canvas->rendererStats().fullUploads >= 1; }));
        // A QWidgetWindow can be the real Wayland keyboard recipient even
        // while the pointer is over the Vulkan surface. Exercise both native
        // shell surfaces, not only direct CanvasWindow key delivery.
        f.trigger("ToolAction_fill");
        const auto panPixels = f.pixels();
        const auto panRevision = f.surface().revision();
        const auto panLayerTransform = f.layer().localToDocument;
        const auto panDepth = f.history().undoDepth();
        const auto panStats = f.canvas->rendererStats();
        const auto originalPan = f.canvas->scene().viewport.pan();
        const auto originalColors = f.window.editorSession().colors();
        for (auto* receiver : {f.window.windowHandle(), f.workspace->panelOverlay()->windowHandle()}) {
            CHECK(receiver);
            if (!receiver) continue;
            if (auto* focus = QApplication::focusWidget()) focus->clearFocus();
            const auto pickPoint = f.logical({20.5, 20.5});
            sendMouse(*f.canvas, QEvent::MouseMove, pickPoint, Qt::NoButton, Qt::NoButton);
            QTest::keyPress(receiver, Qt::Key_Alt);
            CHECK(f.canvas->scene().eyedropperActive);
            sendMouse(*f.canvas, QEvent::MouseButtonPress, pickPoint, Qt::LeftButton, Qt::LeftButton, Qt::AltModifier);
            CHECK(f.canvas->scene().eyedropperSampleValid);
            QTest::keyRelease(receiver, Qt::Key_Alt);
            CHECK(f.canvas->scene().eyedropperActive);
            sendMouse(*f.canvas, QEvent::MouseButtonRelease, pickPoint, Qt::LeftButton, Qt::NoButton);
            CHECK(!f.canvas->scene().eyedropperActive);
            CHECK(f.window.editorSession().activeTool() == core::ToolId::Fill);
            CHECK(f.pixels() == panPixels && f.surface().revision() == panRevision);
            CHECK(f.history().undoDepth() == panDepth);
            f.colors(originalColors);
            for (const double direction : {1.0, -1.0}) {
                const QPointF start(160, 150);
                const QPointF finish = start + QPointF(35 * direction, 22 * direction);
                sendMouse(*f.canvas, QEvent::MouseMove, start, Qt::NoButton, Qt::NoButton);
                const auto beforePan = f.canvas->scene().viewport.pan();
                QTest::keyPress(receiver, Qt::Key_Space);
                CHECK(f.canvas->cursor().shape() == Qt::OpenHandCursor);
                sendMouse(*f.canvas, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
                CHECK(f.canvas->cursor().shape() == Qt::ClosedHandCursor);
                sendMouse(*f.canvas, QEvent::MouseMove, finish, Qt::NoButton, Qt::LeftButton);
                CHECK(f.canvas->scene().viewport.pan() == beforePan + core::Vec2d(35 * direction, 22 * direction));
                sendMouse(*f.canvas, QEvent::MouseButtonRelease, finish, Qt::LeftButton, Qt::NoButton);
                QTest::keyRelease(receiver, Qt::Key_Space);
                CHECK(f.canvas->cursor().shape() == Qt::BitmapCursor);
            }
        }
        CHECK(waitFor([&] { return f.canvas->rendererStats().framesSubmitted > panStats.framesSubmitted; }));
        CHECK(f.canvas->scene().viewport.pan() == originalPan);
        CHECK(f.pixels() == panPixels && f.surface().revision() == panRevision);
        CHECK(f.layer().localToDocument == panLayerTransform && f.history().undoDepth() == panDepth);
        CHECK(f.canvas->rendererStats().uploadedBytes == panStats.uploadedBytes);
        CHECK(f.canvas->rendererStats().fullUploads == panStats.fullUploads);
        CHECK(f.canvas->rendererStats().regionalUploadBatches == panStats.regionalUploadBatches);
        CHECK(f.canvas->rendererStats().swapchainGeneration == panStats.swapchainGeneration);
        f.rectangle({ 10, 8 }, { 40, 30 });
        const auto before = f.pixels();
        const auto stats = f.canvas->rendererStats();
        const auto canvasGeometry = f.workspace->canvasContainer()->geometry();
        f.trigger("ToolAction_fill");
        CHECK(f.canvas->cursor().shape() == Qt::BitmapCursor);
        f.canvasClick({ 20.5, 20.5 });
        f.waitIdle();
        CHECK(waitFor(
            [&] { return f.canvas->rendererStats().regionalUploadBatches > stats.regionalUploadBatches; }));
        CHECK(f.canvas->rendererStats().fullUploads == stats.fullUploads);
        CHECK(f.workspace->canvasContainer()->geometry() == canvasGeometry);
        CHECK(f.canvas->rendererStats().swapchainGeneration == stats.swapchainGeneration);
        CHECK(f.canvas->rendererStats().uploadedBytes - stats.uploadedBytes <= 64U * 48U * 4U);
        const auto after = f.pixels();
        const auto filled = f.canvas->rendererStats();
        f.canvasClick({ 20.5, 20.5 });
        f.waitIdle();
        QTest::qWait(50);
        CHECK(f.canvas->rendererStats().regionalUploadBatches == filled.regionalUploadBatches);
        CHECK(f.canvas->rendererStats().uploadedBytes == filled.uploadedBytes);
        f.undo();
        CHECK(f.pixels() == before);
        CHECK(waitFor([&] {
            return f.canvas->rendererStats().regionalUploadBatches > filled.regionalUploadBatches;
        }));
        const auto undone = f.canvas->rendererStats();
        f.redo();
        CHECK(f.pixels() == after);
        CHECK(waitFor([&] {
            return f.canvas->rendererStats().regionalUploadBatches > undone.regionalUploadBatches;
        }));
        CHECK(f.canvas->rendererStats().fullUploads == stats.fullUploads);
        keyboardEraseAndNudges(f);
        f.window.logRendererDiagnostics();
    }
    CHECK(warnings == 0 && errors == 0);
    std::cout << "Native fill validation (" << QGuiApplication::platformName().toStdString()
        << "): warnings=" << warnings << " errors=" << errors << '\n';
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}

void toolbarReviewSheet(const QString& output)
{
    Fixture f;
    f.window.resize(1600, 900);
    QTest::qWait(60);
    f.rectangle({ 10, 10 }, { 40, 35 });
    QImage sheet(1600, 300, QImage::Format_ARGB32_Premultiplied);
    sheet.fill(QColor("#14171F"));
    QPainter painter(&sheet);
    int y = 0;
    for (const auto* tool :
        { "ToolAction_move", "ToolAction_marquee", "ToolAction_eyedropper", "ToolAction_fill" }) {
        f.trigger(tool);
        QTest::qWait(25);
        painter.drawPixmap(0, y, f.options->grab());
        y += 60;
    }
    f.click("FillModeContiguous");
    QTest::qWait(25);
    painter.drawPixmap(0, y, f.options->grab());
    painter.end();
    CHECK(sheet.save(output));
    CHECK(f.canvas->cursor().pixmap().save(output + QStringLiteral(".cursor.png")));
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("ImageEditorTests"));
    QCoreApplication::setApplicationName(QStringLiteral("FillInteractions"));
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    CHECK(settings.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    ui::applyEditorTheme(application);
    if (qEnvironmentVariableIsSet("IMAGEEDITOR_TEST_FILL_NATIVE"))
        return nativeFillValidation();
    if (const auto preview = qEnvironmentVariable("IMAGEEDITOR_TEST_TOOLBARS_PREVIEW");
        !preview.isEmpty()) {
        toolbarReviewSheet(preview);
        return failures ? EXIT_FAILURE : EXIT_SUCCESS;
    }
    { Fixture f; CHECK(f.valid()); if (f.valid()) keyboardEraseAndNudges(f); }
    controlsAreCachedAndDocumentingTheImplementedTools();
    selectionFillPreservesHolesMaskGeometryAndOneHistoryStep();
    canvasFillsUseTheHighlightedColorWithoutSeparateSourceControls();
    bucketCursorIsCachedAndRestoredAfterPanningAndToolChanges();
    contiguousClickIsFourConnectedAndDoesNotLeakThroughSelectionHoles();
    transformedTargetsKeepTheirGeometryAndUseDocumentSpaceCoverage();
    shortcutsRespectTextFieldsAndMenuFillsDoNotChangeTools();
    cancellingChunkedFillsRestoresPixelsAndPreservesHistory();
    cancellingDiscoveryAndClosingTheWindowNeverLeavesPartialEdits();
    if (failures)
        std::cerr << failures << " fill interaction assertion(s) failed\n";
    else
        std::cout << "All fill interaction tests passed\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
