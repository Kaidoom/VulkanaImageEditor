#include "imageeditor/core/LayerCommands.hpp"
#include "imageeditor/core/SelectionCommands.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/CloningOptionsPage.hpp"
#include "imageeditor/ui/CompactValueControl.hpp"
#include "imageeditor/ui/ImageExport.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/ProjectFile.hpp"
#include "imageeditor/ui/Theme.hpp"

#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFocusEvent>
#include <QImage>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QSettings>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QPushButton>
#include <QWheelEvent>
#include <QVulkanInstance>

#include <atomic>
#include <cmath>
#include <chrono>
#include <iomanip>
#include <iostream>

namespace c = imageeditor::core;
namespace r = imageeditor::render;
namespace u = imageeditor::ui;
namespace {
int failures = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { ++failures; \
    std::cerr << "FAIL " << __LINE__ << ": " << #__VA_ARGS__ << '\n'; } } while (false)

void settle()
{
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
}

bool waitFor(const std::function<bool()>& predicate, int timeout = 10000)
{
    QElapsedTimer timer;
    timer.start();
    do {
        settle();
        if (predicate()) return true;
        QTest::qWait(2);
    } while (timer.elapsed() < timeout);
    return predicate();
}

bool near(const std::optional<c::Vec2d>& point, c::Vec2d expected)
{
    return point && std::abs(point->x - expected.x) < 1e-6
        && std::abs(point->y - expected.y) < 1e-6;
}

QImage pixels(const c::Document& document, c::LayerId id)
{
    const auto* layer = document.layer(id);
    CHECK(layer && std::holds_alternative<c::RasterLayer>(layer->payload));
    if (!layer || !std::holds_alternative<c::RasterLayer>(layer->payload)) return {};
    const auto& surface = *std::get<c::RasterLayer>(layer->payload).surface;
    const auto extent = surface.extent();
    QImage result(static_cast<int>(extent.width), static_cast<int>(extent.height), QImage::Format_RGBA8888);
    surface.copyRgba8({0, 0, result.width(), result.height()},
        {reinterpret_cast<std::byte*>(result.bits()), static_cast<std::size_t>(result.sizeInBytes())},
        static_cast<std::size_t>(result.bytesPerLine()));
    return result;
}

struct Fixture {
    QTemporaryDir files;
    u::MainWindow window;
    r::CanvasWindow* canvas {nullptr};
    QImage original {160, 120, QImage::Format_RGBA8888};
    QString imagePath;
    c::LayerId originalLayer {0};

    explicit Fixture(QVulkanInstance* instance) : window(instance, false, false)
    {
        window.setUnsavedPromptEnabled(false);
        window.resize(1800, 940);
        window.show();
        settle();
        for (auto* candidate : QGuiApplication::allWindows())
            if (candidate->objectName() == QStringLiteral("VulkanCanvasWindow"))
                canvas = dynamic_cast<r::CanvasWindow*>(candidate);
        CHECK(canvas);
        // A textured source with different tone and illumination from the
        // destination; integer source colors make Stamp correspondence exact.
        for (int y = 0; y < original.height(); ++y)
            for (int x = 0; x < original.width(); ++x) {
                const int texture = ((x * 13 + y * 7) % 19) - 9;
                original.setPixelColor(x, y, x < 64
                    ? QColor(95 + texture, 125 + texture + y / 12, 150 + texture)
                    : QColor(170 + texture / 3 + x / 16, 145 + texture / 3, 112 + texture / 3));
            }
        imagePath = files.filePath(QStringLiteral("cloning-fixture.png"));
        CHECK(original.save(imagePath));
    }
    ~Fixture() { window.close(); settle(); }
    c::EditorSession& session() { return const_cast<c::EditorSession&>(window.editorSession()); }
    c::Document& document() { return *session().document(); }
    template<typename T> T* find(const char* name)
    {
        auto* result = dynamic_cast<T*>(window.findChild<QObject*>(QString::fromLatin1(name)));
        CHECK(result);
        return result;
    }
    QAction* shortcut(const QString& sequence, bool trigger = true)
    {
        for (auto* action : window.findChildren<QAction*>())
            if (action->shortcuts().contains(QKeySequence(sequence))) {
                if (trigger) { action->trigger(); settle(); }
                return action;
            }
        CHECK(false);
        return nullptr;
    }
    void button(const char* name)
    {
        if (auto* control = find<QToolButton>(name)) control->click();
        settle();
    }
    bool reset()
    {
        CHECK(window.openImageFromPath(imagePath));
        if (!canvas || !session().document() || !session().activeLayer()) return false;
        originalLayer = *session().activeLayer();
        shortcut(QStringLiteral("S"));
        CHECK(session().activeTool() == c::ToolId::Cloning);
        for (const auto& [name, value] : {
                 std::pair{"CloneSizeControl", 12.0}, {"CloneHardnessControl", 100.0},
                 {"CloneOpacityControl", 100.0}, {"CloneFlowControl", 100.0},
                 {"CloneSpacingControl", 10.0}})
            if (auto* control = find<QDoubleSpinBox>(name)) control->setValue(value);
        button("CloneSourceLayer");
        if (auto* aligned = find<QToolButton>("CloneAlignedButton")) aligned->setChecked(true);
        button("CloneModeStamp");
        if (auto* focus = QApplication::focusWidget()) focus->clearFocus();
        canvas->requestActivate();
        settle();
        document().markSaved();
        return session().activeTool() == c::ToolId::Cloning;
    }
    QPointF logical(c::Vec2d point) const
    {
        const auto extent = window.editorSession().document()->canvas().extent;
        const auto& scene = canvas->scene();
        const auto result = scene.viewport.documentToViewport(point,
            {double(extent.width), double(extent.height)}, scene.logicalViewport);
        return {result.x, result.y};
    }
    void mouse(QEvent::Type type, c::Vec2d point, Qt::KeyboardModifiers modifiers = {}, ulong timestamp = 0)
    {
        const auto local = logical(point);
        const auto button = type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
        const auto buttons = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton;
        QMouseEvent event(type, local, local, QPointF(canvas->mapToGlobal(local.toPoint())),
            button, buttons, modifiers);
        if (timestamp) event.setTimestamp(timestamp);
        QCoreApplication::sendEvent(canvas, &event);
        settle();
    }
    void click(c::Vec2d point, Qt::KeyboardModifiers modifiers = {})
    {
        mouse(QEvent::MouseButtonPress, point, modifiers);
        mouse(QEvent::MouseButtonRelease, point, modifiers);
    }
    void pick(c::Vec2d point)
    {
        click(point, Qt::AltModifier);
        CHECK(near(canvas->scene().cloneSourceAnchor, point));
        CHECK(!canvas->pointerGestureActive());
    }
    void refresh()
    {
        canvas->setDocument(document().snapshot(), false);
        shortcut(QStringLiteral("S"));
        settle();
    }
    c::LayerId retouch()
    {
        shortcut(QStringLiteral("Ctrl+Shift+N"));
        CHECK(session().activeLayer() && *session().activeLayer() != originalLayer);
        return session().activeLayer().value_or(0);
    }
    QImage currentPixels() { return pixels(document(), *session().activeLayer()); }
};

void pickingAndNoSourceAreSessionState(Fixture& f, bool vulkan)
{
    if (!f.reset()) return;
    const auto before = f.currentPixels();
    const auto depth = f.session().history().undoDepth();
    const auto foreground = f.session().foregroundColor();
    const auto revision = f.document().revision();
    if (vulkan) CHECK(waitFor([&] { return f.canvas->rendererStats().fullUploads > 0; }));
    f.click({100.5, 40.5});
    CHECK(f.currentPixels() == before && f.session().history().undoDepth() == depth);
    CHECK(f.window.statusBar()->currentMessage().contains(QStringLiteral("Alt"), Qt::CaseInsensitive));
    CHECK(!f.canvas->scene().cloneSourceAnchor && !f.document().isModified());
    f.pick({20.5, 40.5});
    CHECK(f.session().foregroundColor() == foreground);
    CHECK(f.currentPixels() == before && f.session().history().undoDepth() == depth);
    CHECK(f.document().revision() == revision && !f.document().isModified());
    CHECK(!f.canvas->scene().eyedropperActive);
    f.button("CloneModeHeal");
    CHECK(near(f.canvas->scene().cloneSourceAnchor, {20.5, 40.5}));
    f.button("CloneModeStamp");
    CHECK(near(f.canvas->scene().cloneSourceAnchor, {20.5, 40.5}));
    CHECK(f.session().history().undoDepth() == depth);
    // Cloning a pixel onto itself must not dirty content or upload new pixels.
    if (vulkan) QTest::qWait(40);
    const auto uploads = f.canvas->rendererStats().uploadedBytes;
    f.click({20.5, 40.5});
    if (vulkan) QTest::qWait(40);
    CHECK(f.currentPixels() == before && f.session().history().undoDepth() == depth);
    CHECK(!f.document().isModified());
    if (vulkan) CHECK(f.canvas->rendererStats().uploadedBytes == uploads);
}

void rawStampSelectionAndRetouch(Fixture& f)
{
    for (const bool separate : {false, true}) {
        if (!f.reset()) return;
        f.pick({20.5, 40.5});
        if (separate) f.retouch();
        const auto sourceIdentity = f.find<QWidget>("CloneSourceModes")->accessibleDescription();
        CHECK(f.find<QLabel>("ToolContextStatus")->text() == sourceIdentity);
        std::vector<std::uint8_t> mask(160U * 120U, 0);
        for (int y = 36; y < 46; ++y)
            for (int x = 96; x < 106; ++x)
                mask[static_cast<std::size_t>(y * 160 + x)] = 255;
        mask[40U * 160U + 102U] = 0;
        CHECK(f.session().execute(std::make_unique<c::SetSelectionCommand>(
            c::SelectionMask::fromR8({160, 120}, mask, 160))));
        f.refresh();
        const auto before = f.currentPixels();
        const auto history = f.session().history().undoDepth();
        f.click({100.5, 40.5});
        CHECK(waitFor([&] { return f.session().history().undoDepth() == history + 1; }));
        const auto after = f.currentPixels();
        CHECK(after.pixelColor(100, 40) == f.original.pixelColor(20, 40));
        CHECK(after.pixelColor(102, 40) == before.pixelColor(102, 40));
        CHECK(after.pixelColor(94, 40) == before.pixelColor(94, 40));
        CHECK(f.find<QWidget>("CloneSourceModes")->accessibleDescription() == sourceIdentity);
        CHECK(f.document().isModified());
        if (separate) CHECK(pixels(f.document(), f.originalLayer) == f.original);
        f.shortcut(QStringLiteral("Ctrl+Z"));
        CHECK(f.currentPixels() == before);
        f.shortcut(QStringLiteral("Ctrl+Shift+Z"));
        CHECK(f.currentPixels() == after);
    }
    for (const int sourceMode : {1, 2}) {
        if (!f.reset()) return;
        f.pick({20.5, 40.5});
        f.retouch();
        f.button(sourceMode == 1 ? "CloneSourceCurrentBelow" : "CloneSourceAllVisible");
        const auto history = f.session().history().undoDepth();
        f.click({100.5, 40.5});
        CHECK(waitFor([&] { return f.session().history().undoDepth() == history + 1; }));
        CHECK(f.currentPixels().pixelColor(100, 40) == f.original.pixelColor(20, 40));
        CHECK(f.currentPixels().pixelColor(10, 10).alpha() == 0);
        CHECK(pixels(f.document(), f.originalLayer) == f.original);
    }
}

void sourceStatusStaysBoundAndCompact(Fixture& f)
{
    if (!f.reset()) return;
    auto* status = f.find<QLabel>("ToolContextStatus");
    auto* documentStatus = f.find<QLabel>("DocumentStatus");
    auto* selectionStatus = f.find<QLabel>("SelectionStatus");
    auto* notification = f.find<QLabel>("StatusNotification");
    if (!status || !documentStatus || !selectionStatus || !notification) return;
    CHECK(status->isVisible());
    CHECK(status->text().contains(QStringLiteral("Alt-click")));
    CHECK(status->alignment() == Qt::AlignCenter);
    const auto checkCenteredAndVisible = [&] {
        settle();
        const double center=status->mapTo(f.window.statusBar(),QPoint{}).x()+status->width()/2.0;
        CHECK(std::abs(center-f.window.statusBar()->width()/2.0)<=.5);
        CHECK(documentStatus->isVisible() && selectionStatus->isVisible() && status->isVisible());
        const QRect left(documentStatus->mapTo(f.window.statusBar(),QPoint{}),documentStatus->size());
        const QRect middle(status->mapTo(f.window.statusBar(),QPoint{}),status->size());
        const QRect right(selectionStatus->mapTo(f.window.statusBar(),QPoint{}),selectionStatus->size());
        CHECK(!left.intersects(middle) && !middle.intersects(right));
        const auto* lane = selectionStatus->parentWidget();
        const auto* layout = lane->layout();
        const auto* zoom = layout->itemAt(layout->count()-1)->widget();
        CHECK(zoom->geometry().right() == lane->width()-1);
        CHECK(zoom->width() == zoom->sizeHint().width());
    };
    checkCenteredAndVisible();
    const auto canvasSize = f.canvas->size();
    const auto windowSize = f.window.size();
    const auto minimumWidth = f.window.minimumSizeHint().width();
    const auto longName = QStringLiteral("Source photograph — ")
        + QStringLiteral("a very long descriptive source name ").repeated(12);
    CHECK(f.document().renameLayer(f.originalLayer, longName.toStdString()));
    f.refresh();
    f.pick({20.5, 40.5});
    CHECK(f.window.statusBar()->currentMessage().isEmpty());
    CHECK(notification->isHidden());
    const auto expected = QStringLiteral("Source: ") + longName;
    CHECK(status->text() == expected);
    CHECK(status->toolTip().contains(longName));
    CHECK(status->fontMetrics().horizontalAdvance(status->text()) > status->contentsRect().width());
    CHECK(status->sizePolicy().horizontalPolicy() == QSizePolicy::Ignored);
    CHECK(f.canvas->size() == canvasSize && f.window.size() == windowSize);
    CHECK(f.window.minimumSizeHint().width() == minimumWidth);
    f.retouch();
    CHECK(status->text() == expected);
    // Selecting/revealing a second layer and setting a new source updates only
    // the persistent source readout. No success-message timeout hides info.
    const auto documentInfo=documentStatus->text();
    const auto selectionInfo=selectionStatus->text();
    f.pick({30.5,40.5});
    CHECK(f.window.statusBar()->currentMessage().isEmpty());
    CHECK(documentStatus->text()==documentInfo && selectionStatus->text()==selectionInfo);
    checkCenteredAndVisible();
    f.session().setActiveLayer(f.originalLayer);f.refresh();f.pick({20.5,40.5});
    // Real failures still display, but in a passive overlay above the bar.
    f.window.statusBar()->showMessage(QStringLiteral("Fixture operation warning"),20);
    checkCenteredAndVisible();
    CHECK(notification->isVisible() && notification->text()==QStringLiteral("Fixture operation warning"));
    CHECK(notification->testAttribute(Qt::WA_TransparentForMouseEvents));
    CHECK(status->text()==expected);
    QTest::qWait(30);settle();
    CHECK(notification->isHidden());
    checkCenteredAndVisible();
    for (const int width : {1201,1800,2560,4096}) {
        f.window.resize(width,windowSize.height());
        checkCenteredAndVisible();
    }
    f.window.resize(windowSize);settle();
    f.shortcut(QStringLiteral("V"));
    CHECK(status->text().isEmpty() && status->toolTip().isEmpty());
    f.shortcut(QStringLiteral("S"));
    CHECK(status->text() == expected);
    CHECK(f.canvas->size() == canvasSize && f.window.size() == windowSize);
}

void alignedOffsetsCancellationAndZoom(Fixture& f)
{
    for (const bool aligned : {false, true}) {
        if (!f.reset()) return;
        f.find<QToolButton>("CloneAlignedButton")->setChecked(aligned);
        f.pick({20.5, 40.5});
        f.click({100.5, 40.5});
        f.canvas->resetTo100Percent();
        f.click({112.5, 40.5});
        CHECK(f.currentPixels().pixelColor(112, 40) == f.original.pixelColor(aligned ? 32 : 20, 40));
        if (aligned) CHECK(near(f.canvas->scene().cloneSourceOffset, {-80, 0}));
    }
    for (const bool focusLoss : {false, true}) {
        if (!f.reset()) return;
        f.pick({20.5, 40.5});
        const auto before = f.currentPixels();
        const auto history = f.session().history().undoDepth();
        f.mouse(QEvent::MouseButtonPress, {100.5, 40.5});
        f.mouse(QEvent::MouseMove, {106.5, 40.5});
        CHECK(f.currentPixels() != before);
        if (focusLoss) {
            QFocusEvent lost(QEvent::FocusOut);
            QCoreApplication::sendEvent(f.canvas, &lost);
        } else QTest::keyClick(f.canvas, Qt::Key_Escape);
        settle();
        CHECK(!f.canvas->pointerGestureActive());
        CHECK(f.currentPixels() == before && f.session().history().undoDepth() == history);
        CHECK(!f.document().isModified());
        CHECK(!f.canvas->scene().cloneSourceOffset);
        f.click({112.5, 40.5});
        CHECK(f.currentPixels().pixelColor(112, 40) == f.original.pixelColor(20, 40));
        CHECK(near(f.canvas->scene().cloneSourceOffset, {-92, 0}));
        const auto completed = f.currentPixels();
        f.shortcut(QStringLiteral("Ctrl+Z"));
        CHECK(f.currentPixels() == before);
        const auto redoDepth = f.session().history().redoDepth();
        f.pick({24.5, 44.5});
        f.mouse(QEvent::MouseButtonPress, {104.5, 44.5});
        QTest::keyClick(f.canvas, Qt::Key_Escape);
        settle();
        CHECK(f.currentPixels() == before && f.session().history().redoDepth() == redoDepth);
        f.shortcut(QStringLiteral("Ctrl+Shift+Z"));
        CHECK(f.currentPixels() == completed);
    }
}

void sourceInvalidationAndTyping(Fixture& f)
{
    if (!f.reset()) return;
    f.pick({20.5, 40.5});
    f.retouch();
    auto transform = f.document().layer(f.originalLayer)->localToDocument;
    transform.m02 += 7.25;
    CHECK(f.document().setLayerTransform(f.originalLayer, transform));
    f.refresh();
    const auto before = f.currentPixels();
    const auto history = f.session().history().undoDepth();
    f.click({100.5, 40.5});
    CHECK(f.currentPixels() == before && f.session().history().undoDepth() == history);
    CHECK(!f.canvas->scene().cloneSourceAnchor);
    CHECK(f.window.statusBar()->currentMessage().contains(QStringLiteral("Alt"), Qt::CaseInsensitive));
    f.session().setActiveLayer(f.originalLayer);
    f.refresh();
    f.pick({27.75, 40.5});
    const auto target = f.document().layers().back().id;
    f.session().setActiveLayer(target);
    CHECK(f.session().execute(std::make_unique<c::RemoveLayerCommand>(f.originalLayer)));
    f.refresh();
    CHECK(!f.canvas->scene().cloneSourceAnchor);
    f.click({100.5, 40.5});
    CHECK(f.currentPixels() == before);

    if (!f.reset()) return;
    f.shortcut(QStringLiteral("B"));
    auto* size = f.find<u::CompactValueControl>("BrushSizeControl");
    if (!size) return;
    size->setFocus();
    QTest::keyClick(size, Qt::Key_6);
    CHECK(size->isManualEntryActive());
    QTest::keyClick(size, Qt::Key_S);
    CHECK(f.session().activeTool() == c::ToolId::Brush);
    QTest::keyClick(size, Qt::Key_Escape);
    size->clearFocus();
    f.canvas->requestActivate();
    settle();
    QTest::keyClick(f.canvas, Qt::Key_S);
    settle();
    CHECK(f.session().activeTool() == c::ToolId::Cloning);
    CHECK(f.find<QAction>("ToolAction_cloning")->shortcut() == QKeySequence(QStringLiteral("S")));
}

void fileGuardsAndFinalHealingCancellation(Fixture& f)
{
    const auto forbiddenProject = f.files.filePath(QStringLiteral("unfinished-repair.vulkana"));
    u::MainWindow::FileInteractions hooks;
    hooks.chooseSavePath = [forbiddenProject] { return forbiddenProject; };
    hooks.confirmReplace = [](const QString&) { return true; };
    hooks.reportError = [](const QString& error) { CHECK(false); std::cerr << error.toStdString() << '\n'; };
    f.window.setFileInteractions(std::move(hooks));
    const auto guardFiles = [&] {
        CHECK(!f.window.saveDocument(true));
        CHECK(!QFileInfo::exists(forbiddenProject));
        bool openedExport = false;
        // A guard regression must fail without hanging the suite in a modal
        // export dialog. This watchdog is inactive once exportImage returns.
        QTimer watchdog;
        watchdog.setInterval(10);
        QObject::connect(&watchdog, &QTimer::timeout, &watchdog, [&] {
            for (auto* widget : QApplication::allWidgets())
                if (widget->objectName() == QStringLiteral("ExportDialog") && widget->isVisible()) {
                    openedExport = true;
                    if (auto* dialog = qobject_cast<QDialog*>(widget)) dialog->reject();
                }
        });
        watchdog.start();
        f.window.exportImage();
        watchdog.stop();
        CHECK(!openedExport);
        CHECK(f.window.statusBar()->currentMessage().contains(QStringLiteral("Finish the current stroke")));
    };
    for (const bool heal : {false, true}) {
        if (!f.reset()) return;
        f.pick({20.5, 40.5});
        if (heal) f.button("CloneModeHeal");
        const auto before = f.currentPixels();
        const auto history = f.session().history().undoDepth();
        const auto contentState = f.document().contentState();
        f.mouse(QEvent::MouseButtonPress, {100.5, 40.5});
        const auto preview = f.currentPixels();
        if (!heal) CHECK(preview != before);
        guardFiles();
        CHECK(f.canvas->pointerGestureActive());
        CHECK(f.currentPixels() == preview && f.session().history().undoDepth() == history);
        CHECK(f.document().contentState() == contentState && !f.document().isModified());
        QTest::keyClick(f.canvas, Qt::Key_Escape);
        settle();
        CHECK(f.currentPixels() == before && !f.canvas->pointerGestureActive());
    }

    // Enough texture and support to cross the cooperative event cadence even
    // in Release. The timer is queued only when final reconstruction begins,
    // so it cannot accidentally cancel source capture or pointer input.
    QImage larger(640, 448, QImage::Format_RGBA8888);
    for (int y = 0; y < larger.height(); ++y)
        for (int x = 0; x < larger.width(); ++x) {
            const int texture = ((x * 13 + y * 7) % 31) - 15;
            larger.setPixelColor(x, y, x < 320
                ? QColor(95 + texture, 120 + texture + y / 32, 145 + texture)
                : QColor(165 + texture / 2 + x / 32, 145 + texture / 2, 110 + texture / 2));
        }
    const auto largerPath = f.files.filePath(QStringLiteral("healing-final-cancellation.png"));
    CHECK(larger.save(largerPath));
    CHECK(f.window.openImageFromPath(largerPath));
    f.shortcut(QStringLiteral("S"));
    f.button("CloneModeStamp");
    f.pick({170.5, 220.5});
    const auto original = f.currentPixels();
    f.click({470.5, 220.5});
    const auto completed = f.currentPixels();
    CHECK(completed != original);
    f.shortcut(QStringLiteral("Ctrl+Z"));
    CHECK(f.currentPixels() == original);
    const auto history = f.session().history().undoDepth();
    const auto redo = f.session().history().redoDepth();
    const auto contentState = f.document().contentState();
    f.document().markSaved();
    f.pick({170.5, 220.5});
    f.button("CloneModeHeal");
    f.find<QDoubleSpinBox>("CloneSizeControl")->setValue(280);
    f.mouse(QEvent::MouseButtonPress, {470.5, 220.5});
    CHECK(f.canvas->pointerGestureActive());
    bool queued = false, processedDuringFinal = false;
    const auto connection = QObject::connect(f.window.statusBar(), &QStatusBar::messageChanged,
        &f.window, [&](const QString& message) {
            if (queued || !message.startsWith(QStringLiteral("Reconstructing Heal"))) return;
            queued = true;
            QTimer::singleShot(0, &f.window, [&] {
                processedDuringFinal = f.window.statusBar()->currentMessage()
                    .startsWith(QStringLiteral("Reconstructing Heal"));
                CHECK(processedDuringFinal);
                guardFiles();
                CHECK(f.session().history().undoDepth() == history);
                QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
                QCoreApplication::sendEvent(f.canvas, &escape);
            });
        });
    f.mouse(QEvent::MouseButtonRelease, {470.5, 220.5});
    QObject::disconnect(connection);
    settle();
    CHECK(queued && processedDuringFinal);
    CHECK(f.currentPixels() == original && !f.canvas->pointerGestureActive());
    CHECK(f.session().history().undoDepth() == history && f.session().history().redoDepth() == redo);
    CHECK(f.document().contentState() == contentState && !f.document().isModified());
    CHECK(!f.canvas->scene().cloneSourceOffset);
    CHECK(!QFileInfo::exists(forbiddenProject));
    f.shortcut(QStringLiteral("Ctrl+Shift+Z"));
    CHECK(f.currentPixels() == completed);
    f.window.setFileInteractions({});
}

void typedReferencesUseDocumentResolutionWithoutReplacingViewportCaches(Fixture& f)
{
    for (const bool text : {false, true})
        for (const bool oneToOne : {false, true}) {
            if (!f.reset()) return;
            if (oneToOne) { f.canvas->resetTo100Percent(); settle(); }
            c::TextLayer textData;
            textData.utf8 = "Oo";
            textData.defaultStyle.font.family = QApplication::font().family().toStdString();
            textData.defaultStyle.sizePixels = 37.5;
            textData.defaultStyle.color = {220, 130, 60, 177};
            c::ShapeLayer shapeData;
            shapeData.kind = c::ShapeKind::Ellipse;
            shapeData.size = {47.5, 53.25};
            shapeData.fillColor = {220, 130, 60, 177};
            shapeData.strokeEnabled = true;
            shapeData.strokeWidth = 1.25;
            shapeData.strokeColor = {30, 210, 90, 128};
            auto source = text ? c::Layer::text("Typed text source", textData)
                               : c::Layer::shape("Typed shape source", shapeData);
            source.localToDocument = {-.9, .12, 63.5, .15, 1.1, 14.25};
            CHECK(f.session().execute(std::make_unique<c::AddLayerCommand>(
                source, f.document().layers().size())));
            f.session().setActiveLayer(source.id);
            f.refresh();

            // An isolated ordinary document export independently supplies the
            // raw typed appearance, including AA and partial alpha, at 1×.
            c::Document isolated({{160, 120}});
            CHECK(isolated.insertLayer(0, source));
            const auto expected = u::flattenDocument(isolated);
            CHECK(expected);
            if (!expected) continue;
            QPoint edge(-1, -1);
            for (int y = 5; y < 110 && edge.x() < 0; ++y)
                for (int x = 5; x < 75; ++x) {
                    const auto color = expected.image.pixelColor(x, y);
                    if (color.alpha() > 12 && color.alpha() < 115) { edge = {x, y}; break; }
                }
            CHECK(edge.x() >= 0);
            if (edge.x() < 0) continue;
            f.pick({edge.x() + .5, edge.y() + .5});
            f.retouch();
            const auto* liveSource = f.document().layer(source.id);
            CHECK(liveSource && liveSource->renderCache && liveSource->renderCache->surface);
            if (!liveSource || !liveSource->renderCache || !liveSource->renderCache->surface) continue;
            const auto viewportCache = liveSource->renderCache;
            const auto cacheSurface = viewportCache->surface->id();
            const auto cacheRevision = viewportCache->surface->revision();
            f.click({edge.x() + 80.5, edge.y() + .5});
            const auto actual = f.currentPixels().pixelColor(edge.x() + 80, edge.y());
            CHECK(actual == expected.image.pixelColor(edge));
            liveSource = f.document().layer(source.id);
            CHECK(liveSource && liveSource->renderCache == viewportCache);
            if (liveSource && liveSource->renderCache) {
                CHECK(liveSource->renderCache->surface->id() == cacheSurface);
                CHECK(liveSource->renderCache->surface->revision() == cacheRevision);
                if (text) CHECK(std::get<c::TextLayer>(liveSource->payload) == std::get<c::TextLayer>(source.payload));
                else CHECK(std::get<c::ShapeLayer>(liveSource->payload) == shapeData);
            }
        }
}

void saveOptionalToolbarReview(Fixture& f)
{
    const auto path = qEnvironmentVariable("IMAGEEDITOR_CLONE_TOOLBAR_REVIEW_PATH");
    if (path.isEmpty() || !f.reset()) return;
    f.pick({20.5, 40.5});
    auto* bar = f.find<QToolBar>("ToolOptionsBar");
    if (!bar) return;
    const auto originalSize = f.window.size();
    std::vector<QImage> rows;
    for (const auto& [width, mode] : {
             std::pair{1800, "CloneModeStamp"}, {1800, "CloneModeHeal"}, {1000, "CloneModeHeal"}}) {
        f.window.resize(width, originalSize.height());
        f.button(mode);
        settle();
        rows.push_back(bar->grab().toImage());
    }
    const QStringList labels {QStringLiteral("Stamp — shared brush controls, stable source"),
        QStringLiteral("Heal — adaptation enabled, same control geometry"),
        QStringLiteral("Narrow workspace — cached modes and horizontal overflow")};
    const int rowHeight = rows.front().height() + 35;
    QImage sheet(rows.front().width(), rowHeight * static_cast<int>(rows.size()), QImage::Format_ARGB32_Premultiplied);
    sheet.fill(QApplication::palette().color(QPalette::Window));
    QPainter painter(&sheet);
    painter.setPen(QApplication::palette().color(QPalette::Text));
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const int top = static_cast<int>(i) * rowHeight;
        painter.drawText(QRect(12, top, sheet.width() - 24, 30), Qt::AlignVCenter, labels[static_cast<int>(i)]);
        painter.drawImage(QPoint(0, top + 30), rows[i]);
    }
    painter.end();
    CHECK(sheet.save(path));
    CHECK(f.window.statusBar()->grab().save(path + QStringLiteral(".status.png")));
    f.window.resize(originalSize);
    settle();
}

void healingRetouchHistoryAndFiles(Fixture& f)
{
    if (!f.reset()) return;
    f.pick({24.5, 60.5});
    const auto retouch = f.retouch();
    f.button("CloneSourceCurrentBelow");
    f.button("CloneModeHeal");
    f.find<QDoubleSpinBox>("CloneSizeControl")->setValue(18);
    const auto before = f.currentPixels();
    const auto history = f.session().history().undoDepth();
    f.mouse(QEvent::MouseButtonPress, {104.5, 60.5});
    f.mouse(QEvent::MouseMove, {108.5, 60.5});
    f.mouse(QEvent::MouseButtonRelease, {108.5, 60.5});
    CHECK(waitFor([&] { return f.session().history().undoDepth() == history + 1; }, 30000));
    const auto repaired = f.currentPixels();
    CHECK(repaired != before && repaired.pixelColor(104, 60).alpha() > 0);
    CHECK(repaired.pixelColor(10, 10).alpha() == 0);
    CHECK(pixels(f.document(), f.originalLayer) == f.original);
    f.shortcut(QStringLiteral("Ctrl+Z"));
    CHECK(f.currentPixels() == before);
    f.shortcut(QStringLiteral("Ctrl+Shift+Z"));
    CHECK(f.currentPixels() == repaired);

    const auto project = f.files.filePath(QStringLiteral("cloning-repair.vulkana"));
    u::MainWindow::FileInteractions hooks;
    hooks.chooseSavePath = [&] { return project; };
    hooks.confirmReplace = [](const QString&) { return true; };
    hooks.reportError = [](const QString& error) { CHECK(false); std::cerr << error.toStdString() << '\n'; };
    f.window.setFileInteractions(std::move(hooks));
    CHECK(f.window.saveDocument(true));
    CHECK(!f.document().isModified());
    auto loaded = u::loadProject(project);
    CHECK(loaded);
    if (!loaded) return;
    CHECK(pixels(*loaded.document, retouch) == repaired);
    CHECK(pixels(*loaded.document, f.originalLayer) == f.original);
    u::ExportSettings exportSettings;
    exportSettings.size = {160, 120};
    exportSettings.destination = f.files.filePath(QStringLiteral("cloning-repair.png"));
    const auto rendered = u::renderExport(f.document(), exportSettings);
    const auto reopened = u::renderExport(*loaded.document, exportSettings);
    CHECK(rendered && reopened);
    if (!rendered || !reopened) return;
    CHECK(rendered.image == reopened.image);
    std::atomic_bool cancel {false};
    const auto encoded = u::encodeExport(rendered.image, exportSettings, cancel);
    CHECK(encoded);
    if (encoded) {
        CHECK(u::writeExportAtomically(exportSettings.destination, encoded.bytes, cancel));
        const QImage exported(exportSettings.destination);
        CHECK(exported.convertToFormat(QImage::Format_RGBA8888)
            == rendered.image.convertToFormat(QImage::Format_RGBA8888));
    }
    CHECK(f.window.openImageFromPath(project));
    CHECK(f.canvas->scene().cloneSourceAnchor); // Reopening the same project focuses its existing clone context.
    CHECK(pixels(f.document(), retouch) == repaired);
}

void spotHealInteraction(Fixture& f, bool native)
{
    // Paired generated texture with a red blemish. Only this corrupted image
    // enters the editor/solver; quality comparisons live in the separate harness.
    QImage damaged = f.original;
    QPainter damage(&damaged); damage.setPen(Qt::NoPen); damage.setBrush(QColor(240, 8, 12));
    damage.drawEllipse(QPointF(103, 61), 3, 3); damage.end();
    CHECK(damaged.save(f.imagePath));
    if (!f.reset()) return;
    f.pick({24.5, 40.5});
    f.button("CloneModeSpotHeal");
    CHECK(f.canvas->scene().spotHealActive && !f.canvas->scene().cloneSourceAnchor);
    CHECK(!f.find<QToolButton>("CloneAlignedButton")->isEnabled());
    const auto before = f.currentPixels();
    const auto depth = f.session().history().undoDepth();
    if (native) CHECK(waitFor([&] { return f.canvas->rendererStats().fullUploads > 0; }));
    const auto uploads = f.canvas->rendererStats().uploadedBytes;
    f.mouse(QEvent::MouseButtonPress, {103, 61});
    CHECK(f.canvas->pointerGestureActive());
    CHECK(f.canvas->scene().repairRegionEdges && !f.canvas->scene().repairRegionEdges->empty());
    CHECK(f.currentPixels() == before && f.session().history().undoDepth() == depth);
    if (native) { QTest::qWait(50); CHECK(f.canvas->rendererStats().uploadedBytes == uploads); }
    f.mouse(QEvent::MouseButtonRelease, {103, 61});
    CHECK(!f.canvas->pointerGestureActive());
    CHECK(waitFor([&] { return !f.canvas->scene().repairProcessing; }, 30000));
    const auto repaired = f.currentPixels();
    CHECK(f.session().history().undoDepth() == depth + 1);
    CHECK(repaired != before && repaired.pixelColor(103, 61).green() > 60);
    for (int y = 0; y < repaired.height(); ++y) for (int x = 0; x < repaired.width(); ++x) {
        CHECK(repaired.pixelColor(x, y).alpha() == before.pixelColor(x, y).alpha());
        if (std::hypot(x + .5 - 103, y + .5 - 61) > 6.1)
            CHECK(repaired.pixel(x, y) == before.pixel(x, y));
    }
    CHECK(!f.canvas->scene().repairRegionEdges);
    f.button("CloneModeHeal"); CHECK(near(f.canvas->scene().cloneSourceAnchor, {24.5, 40.5}));
    f.button("CloneModeStamp"); CHECK(near(f.canvas->scene().cloneSourceAnchor, {24.5, 40.5}));
    f.shortcut(QStringLiteral("Ctrl+Z")); CHECK(f.currentPixels() == before);
    f.shortcut(QStringLiteral("Ctrl+Shift+Z")); CHECK(f.currentPixels() == repaired);
    f.shortcut(QStringLiteral("Ctrl+Z"));

    // Both marking and asynchronous cancellation preserve the existing redo.
    f.button("CloneModeSpotHeal");
    f.mouse(QEvent::MouseButtonPress, {103, 61});
    QTest::keyClick(f.canvas, Qt::Key_Escape); settle();
    CHECK(!f.canvas->pointerGestureActive() && !f.canvas->scene().repairRegionEdges);
    CHECK(f.currentPixels() == before && f.session().history().redoDepth() == 1);
    f.mouse(QEvent::MouseButtonRelease, {103, 61});
    f.find<QDoubleSpinBox>("CloneSizeControl")->setValue(36);
    f.mouse(QEvent::MouseButtonPress, {102, 50});
    f.mouse(QEvent::MouseMove, {116, 77});
    f.mouse(QEvent::MouseButtonRelease, {116, 77});
    CHECK(f.canvas->scene().repairProcessing);
    CHECK(!f.window.saveDocument());
    const auto active = f.session().activeLayer();
    QTest::keyClick(f.canvas, Qt::Key_B); settle();
    CHECK(f.session().activeTool() == c::ToolId::Cloning && f.session().activeLayer() == active);
    const auto panBefore = f.canvas->scene().viewport.pan();
    QTest::keyPress(f.canvas, Qt::Key_Space);
    f.mouse(QEvent::MouseButtonPress, {40, 40});
    CHECK(f.canvas->panDragging());
    f.mouse(QEvent::MouseMove, {48, 44});
    f.mouse(QEvent::MouseButtonRelease, {48, 44});
    QTest::keyRelease(f.canvas, Qt::Key_Space);
    CHECK(!f.canvas->panDragging() && f.canvas->scene().viewport.pan() != panBefore);
    const auto zoom = f.canvas->zoom();
    const auto point = f.logical({60, 50});
    QWheelEvent wheel(point, QPointF(f.canvas->mapToGlobal(point.toPoint())), {}, {0, 120},
        Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(f.canvas, &wheel);
    CHECK(f.canvas->zoom() != zoom);
    QElapsedTimer cancellation; cancellation.start();
    auto* cancelButton=f.find<QPushButton>("SpotHealCancelProcessing");
    const QPoint cancelGlobal=cancelButton->mapToGlobal(cancelButton->rect().center());
    auto* carrier=cancelButton->window()->windowHandle();
    if (carrier) {
        const QPointF cancelLocal(carrier->mapFromGlobal(cancelGlobal));
        QMouseEvent down(QEvent::MouseButtonPress,cancelLocal,cancelLocal,QPointF(cancelGlobal),Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
        QMouseEvent up(QEvent::MouseButtonRelease,cancelLocal,cancelLocal,QPointF(cancelGlobal),Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
        QCoreApplication::sendEvent(carrier,&down);QCoreApplication::sendEvent(carrier,&up);
    } else { CHECK(false); cancelButton->click(); }
    CHECK(waitFor([&] { return !f.canvas->scene().repairProcessing; }, 2000));
    std::cout << "Spot Heal UI cancellation_ms=" << cancellation.elapsed() << '\n';
    CHECK(f.currentPixels() == before && f.session().history().redoDepth() == 1);

    // Rendered appearance goes to a clean transparent retouch layer; the
    // original remains unchanged and persisted/exported results agree.
    const auto retouch = f.retouch();
    f.button("CloneSourceCurrentBelow");
    f.find<QDoubleSpinBox>("CloneSizeControl")->setValue(12);
    const auto blank = f.currentPixels();
    f.click({103, 61});
    CHECK(waitFor([&] { return !f.canvas->scene().repairProcessing; }, 30000));
    const auto retouched = f.currentPixels();
    CHECK(retouched != blank && retouched.pixelColor(103, 61).alpha() > 0);
    CHECK(retouched.pixelColor(0, 0).alpha() == 0);
    CHECK(pixels(f.document(), f.originalLayer) == before);
    f.shortcut(QStringLiteral("Ctrl+Z")); CHECK(f.currentPixels() == blank);
    f.shortcut(QStringLiteral("Ctrl+Shift+Z")); CHECK(f.currentPixels() == retouched);
    const auto project = f.files.filePath(QStringLiteral("spot-heal.vulkana"));
    CHECK(u::saveProject(project, f.document()));
    auto loaded = u::loadProject(project); CHECK(loaded);
    if (loaded) {
        CHECK(pixels(*loaded.document, retouch) == retouched);
        u::ExportSettings settings; settings.size = {160, 120};
        const auto output = u::renderExport(f.document(), settings);
        const auto reopened = u::renderExport(*loaded.document, settings);
        CHECK(output && reopened && output.image == reopened.image);
        if (output) {
            std::atomic_bool cancel {false};
            auto encoded = u::encodeExport(output.image, settings, cancel); CHECK(encoded);
            CHECK(encoded.decoded.convertToFormat(QImage::Format_RGBA8888)
                == output.image.convertToFormat(QImage::Format_RGBA8888));
        }
    }
    if (native) {
        CHECK(waitFor([&] { return f.canvas->rendererStats().regionalUploads > 0; }));
        const auto stable = f.canvas->rendererStats();
        QTest::qWait(120);
        CHECK(f.canvas->rendererStats().uploadedBytes == stable.uploadedBytes);
        CHECK(f.canvas->rendererStats().resourceGeneration == stable.resourceGeneration);
    }
}

void spotHealAcrossDocuments(Fixture& f)
{
    QImage damaged=f.original;QPainter painter(&damaged);painter.setPen(Qt::NoPen);painter.setBrush(QColor(240,8,12));
    painter.drawEllipse(QPointF(103,61),3,3);painter.end();CHECK(damaged.save(f.imagePath));
    CHECK(f.reset());f.button("CloneModeSpotHeal");
    const auto origin=f.window.activeDocumentId();const auto originLayer=*f.session().activeLayer();
    const auto oldDepth=f.session().history().undoDepth();
    const c::NormalizedPointerSample sample{{103,61},1,1};
    CHECK(f.canvas->onBrushStrokeBegan(sample));CHECK(f.canvas->onBrushStrokeEnded(sample));
    CHECK(f.canvas->scene().repairProcessing);
    CHECK(f.window.openImageFromPath(f.imagePath));const auto destination=f.window.activeDocumentId();
    CHECK(!f.canvas->scene().repairProcessing&&origin!=destination);
    CHECK(waitFor([&]{return f.window.documentContext(origin)->session.history().undoDepth()==oldDepth+1;},30000));
    CHECK(f.window.activeDocumentId()==destination&&f.session().history().undoDepth()==0);
    CHECK(pixels(*f.window.documentContext(origin)->session.document(),originLayer)!=damaged);
    CHECK(f.currentPixels()==damaged);
    // Closing an origin cooperatively cancels its repair without publishing to
    // another tab. Keeping a copy of the local IDs cannot redirect completion.
    f.button("CloneModeSpotHeal");CHECK(f.canvas->onBrushStrokeBegan(sample));CHECK(f.canvas->onBrushStrokeEnded(sample));
    CHECK(f.window.activateDocument(origin));CHECK(f.window.closeDocument(destination));
    QTest::qWait(80);CHECK(f.window.activeDocumentId()==origin);
    CHECK(f.session().history().undoDepth()==oldDepth+1);
}

// Opt-in real event/worker/publication/render probe, deliberately outside CTest
// timing gates. Supply an already-corrupted benchmark PNG and identical brush
// coordinates; setup and initial image upload are excluded. A queued Vulkan
// presentation is observable here, but compositor scanout / photons are not.
void spotHealLatency(Fixture& f, bool native)
{
    const auto arguments = QCoreApplication::arguments();
    const auto option = [&](const char* name, QString fallback = {}) {
        const auto index = arguments.indexOf(QString::fromLatin1(name));
        return index >= 0 && index + 1 < arguments.size() ? arguments[index + 1] : fallback;
    };
    const auto imagePath = option("--latency-image");
    if (!imagePath.isEmpty()) {
        f.original = QImage(imagePath).convertToFormat(QImage::Format_RGBA8888);
        CHECK(!f.original.isNull());
        if (f.original.isNull()) return;
        CHECK(f.original.save(f.imagePath));
    }
    const double size = option("--latency-size", "14").toDouble();
    const double length = option("--latency-length", "0").toDouble();
    const c::Vec2d center {
        option("--latency-x", QString::number(f.original.width() / 2.0)).toDouble(),
        option("--latency-y", QString::number(f.original.height() / 2.0)).toDouble()};
    const int repetitions = std::clamp(option("--latency-repeats", "2").toInt(), 1, 10);
    const bool retouch = arguments.contains(QStringLiteral("--retouch"));
    CHECK(size > 0 && length >= 0);
    if (size <= 0 || length < 0) return;
    const auto curve = [&](double t) {
        return c::Vec2d{center.x + length * t,
            center.y + (length > 0 ? std::min(30.0, length * .15) * std::sin(t * 3.141592653589793) : 0)};
    };
    if (imagePath.isEmpty() || arguments.contains(QStringLiteral("--latency-clean-image"))) {
        // Identical generated corruption geometry to SpotHealBenchmark; only
        // the private temporary PNG is written, never the source photograph.
        for (int y = std::max(0, int(center.y) - 4); y < std::min(f.original.height(), int(center.y) + 35); ++y)
            for (int x = std::max(0, int(center.x) - 4); x < std::min(f.original.width(), int(center.x + length) + 5); ++x) {
                const bool damage = length > 0
                    ? x >= center.x && x <= center.x + length && std::abs(y - curve((x - center.x) / length).y) < 2
                    : std::hypot(x - center.x, y - center.y) < 3;
                if (damage) f.original.setPixelColor(x, y, QColor(244, 22, 98, 255));
            }
        CHECK(f.original.save(f.imagePath));
    }
    for (int repetition = 0; repetition < repetitions; ++repetition) {
        if (!f.reset()) return;
        if (retouch) { f.retouch(); f.button("CloneSourceCurrentBelow"); }
        f.button("CloneModeSpotHeal");
        auto settings = c::proceduralBrushPreset(c::ProceduralBrushPreset::HardRound);
        settings.sizePixels = size; settings.hardness = .85;
        settings.opacity = 1; settings.flow = 1; settings.spacingPercent = 15;
        settings.pressureToSize = false; settings.pressureToFlow = false;
        f.find<u::CloningOptionsPage>("CloningOptionsPage")->onBrushSettingsChanged(settings);
        if (native) {
            const auto queued = f.canvas->rendererStats().presentQueuedFrames;
            f.canvas->scheduleFrame();
            CHECK(waitFor([&] { return f.canvas->rendererStats().presentQueuedFrames > queued; }));
        }
        const auto before = f.canvas->rendererStats();
        const auto depth = f.session().history().undoDepth();
        c::Vec2d end = curve(0);
        QElapsedTimer press; press.start();
        f.mouse(QEvent::MouseButtonPress, end, {}, 1);
        const double pressMilliseconds = static_cast<double>(press.nsecsElapsed()) / 1.0e6;
        CHECK(f.canvas->pointerGestureActive());
        if (length > 0) for (int i = 1; i < 40; ++i)
            f.mouse(QEvent::MouseMove, curve(double(i) / 40), {}, 1 + ulong(i) * 10);
        end = curve(1);
        const auto releaseStarted = std::chrono::steady_clock::now();
        const auto releaseNanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(
            releaseStarted.time_since_epoch()).count();
        const auto sinceRelease = [&] { return std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - releaseStarted).count(); };
        f.mouse(QEvent::MouseButtonRelease, end, {}, 401);
        CHECK(!f.canvas->pointerGestureActive());
        CHECK(waitFor([&] { return !f.canvas->scene().repairProcessing; }, 120000));
        const double publicationObserved = sinceRelease();
        CHECK(f.session().history().undoDepth() == depth + 1);
        double presentObserved = 0;
        if (native) {
            CHECK(waitFor([&] { return f.canvas->rendererStats().uploadedBytesAtPresent > before.uploadedBytesAtPresent; }));
            presentObserved = sinceRelease();
        }
        const auto after = f.canvas->rendererStats();
        std::cout << std::fixed << std::setprecision(3)
            << "spot_heal_pipeline sampling=" << (retouch ? "retouch" : "raw")
            << " repetition=" << repetition
            << " canvas=" << f.original.width() << 'x' << f.original.height()
            << " brush_px=" << size << " trace_px=" << length
            << " press_return_ms=" << pressMilliseconds
            << " release_to_publication_observed_ms=" << publicationObserved
            << " release_to_present_queued_observed_ms=" << presentObserved
            << " upload_preparation_ms=" << static_cast<double>(after.uploadPreparationNanoseconds - before.uploadPreparationNanoseconds) / 1.0e6
            << " uploaded_bytes=" << after.uploadedBytes - before.uploadedBytes
            << " regional_uploads=" << after.regionalUploads - before.regionalUploads
            << " full_uploads=" << after.fullUploads - before.fullUploads
            << " queued_frames=" << after.presentQueuedFrames - before.presentQueuedFrames;
        if (after.lastUploadPresentQueuedSteadyNanoseconds > static_cast<std::uint64_t>(releaseNanoseconds))
            std::cout << " release_to_present_queued_timestamp_ms="
                << static_cast<double>(after.lastUploadPresentQueuedSteadyNanoseconds - static_cast<std::uint64_t>(releaseNanoseconds)) / 1.0e6;
        std::cout << " physical_presentation=unmeasured\n";
        CHECK(after.compositionError.empty());
    }
}

void exercise(QVulkanInstance* instance)
{
    Fixture fixture(instance);
    if (!fixture.canvas) return;
    if (QCoreApplication::arguments().contains(QStringLiteral("--spot-heal-latency"))) {
        spotHealLatency(fixture, instance != nullptr); return;
    }
    if (QCoreApplication::arguments().contains(QStringLiteral("--spot-heal"))) {
        spotHealInteraction(fixture, instance != nullptr); spotHealAcrossDocuments(fixture); return;
    }
    pickingAndNoSourceAreSessionState(fixture, instance != nullptr);
    rawStampSelectionAndRetouch(fixture);
    sourceStatusStaysBoundAndCompact(fixture);
    alignedOffsetsCancellationAndZoom(fixture);
    sourceInvalidationAndTyping(fixture);
    fileGuardsAndFinalHealingCancellation(fixture);
    typedReferencesUseDocumentResolutionWithoutReplacingViewportCaches(fixture);
    saveOptionalToolbarReview(fixture);
    healingRetouchHistoryAndFiles(fixture);
    spotHealAcrossDocuments(fixture);
    if (instance) {
        CHECK(waitFor([&] { return fixture.canvas->rendererStats().framesSubmitted > 0; }));
        CHECK(fixture.canvas->rendererStats().regionalUploads > 0);
    }
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("ImageEditorTests"));
    QCoreApplication::setApplicationName(QStringLiteral("CloningInteractions"));
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    u::applyEditorTheme(application);
    if (application.arguments().contains(QStringLiteral("--native"))) {
        if (QGuiApplication::platformName() != QStringLiteral("wayland")) return 77;
        std::atomic_int messages {0};
        QVulkanInstance instance;
        instance.setApiVersion(QVersionNumber(1, 2));
        const bool validation = !application.arguments().contains(QStringLiteral("--no-validation"));
        if (validation) instance.setLayers({QByteArrayLiteral("VK_LAYER_KHRONOS_validation")});
        instance.installDebugOutputFilter([&](auto severity, auto type, const void* raw) {
            if (type.testFlag(QVulkanInstance::ValidationMessage)
                && (severity.testFlag(QVulkanInstance::WarningSeverity)
                    || severity.testFlag(QVulkanInstance::ErrorSeverity))) {
                ++messages;
                const auto* data = static_cast<const VkDebugUtilsMessengerCallbackDataEXT*>(raw);
                std::cerr << "Vulkan: " << data->pMessage << '\n';
            }
            return false;
        });
        CHECK(instance.create());
        if (instance.isValid()) exercise(&instance);
        instance.destroy();
        settle();
        CHECK(messages == 0);
        std::cout << "Cloning Vulkan validation " << (validation ? "messages: " : "disabled; messages: ") << messages << '\n';
    } else exercise(nullptr);
    if (!failures) std::cout << "Cloning interaction tests passed\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
