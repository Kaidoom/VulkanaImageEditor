#include "imageeditor/core/DocumentCommands.hpp"
#include "imageeditor/core/SnapshotTimeline.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/FlattenedDocument.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/ProjectFile.hpp"
#include "imageeditor/ui/SnapshotTimelinePanel.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/ui/GifExportDialog.hpp"
#include "imageeditor/ui/CompactValueControl.hpp"
#include "NativeGraphicsStartup.hpp"
#include "GifFixture.hpp"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QImageReader>
#include <QPushButton>
#include <QTimer>
#include <QToolButton>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QDropEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QScrollBar>
#include <QSettings>
#include <QSpinBox>
#include <QStyleOptionSpinBox>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QVulkanInstance>

#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace c = imageeditor::core;
namespace u = imageeditor::ui;
namespace r = imageeditor::render;
namespace {
int failures {};
#define CHECK(...) do { if (!(__VA_ARGS__)) { ++failures; \
    std::cerr << __LINE__ << ": " #__VA_ARGS__ "\n"; } } while (false)

void settle()
{
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
}

template<class F> bool waitFor(F predicate, int timeout = 5000)
{
    QElapsedTimer timer;
    timer.start();
    do {
        settle();
        if (predicate()) return true;
        QTest::qWait(10);
    } while (timer.elapsed() < timeout);
    return false;
}

template<class T> T& child(QObject& owner, const char* name)
{
    auto* result = owner.findChild<T*>(QString::fromLatin1(name));
    if (!result) throw std::runtime_error(std::string("Missing control: ") + name);
    return *result;
}

QImage pixels(const c::RasterSurface& surface)
{
    const auto extent = surface.extent();
    QImage result(int(extent.width), int(extent.height), QImage::Format_RGBA8888);
    surface.copyRgba8({0, 0, result.width(), result.height()},
        {reinterpret_cast<std::byte*>(result.bits()), std::size_t(result.sizeInBytes())},
        std::size_t(result.bytesPerLine()));
    return result;
}

QImage image(const c::SnapshotFrame& frame)
{
    if (!frame.image) throw std::runtime_error("Expected a captured frame");
    return QImage::fromData(reinterpret_cast<const uchar*>(frame.image->png.data()),
        int(frame.image->png.size()), "PNG").convertToFormat(QImage::Format_RGBA8888);
}

QString makeFixture(const QTemporaryDir& temp)
{
    c::Document document({{80, 60}, 144, c::CompositingSpace::EncodedSRgb});
    auto backdrop = c::Layer::raster("Backdrop",
        std::make_shared<c::ContiguousRasterSurface>(c::Extent2u{80, 60}, c::Rgba8{20, 60, 160, 135}));
    CHECK(document.insertLayer(0, std::move(backdrop)));
    auto hidden = c::Layer::raster("Hidden",
        std::make_shared<c::ContiguousRasterSurface>(c::Extent2u{80, 60}, c::Rgba8{255, 0, 255, 255}));
    hidden.visible = false;
    CHECK(document.insertLayer(1, std::move(hidden)));
    auto foreground = c::Layer::raster("Masked foreground",
        std::make_shared<c::ContiguousRasterSurface>(c::Extent2u{32, 24}, c::Rgba8{220, 85, 30, 221}));
    foreground.localToDocument = {1, 0, 11, 0, 1, 9};
    foreground.opacity = .71F;
    foreground.fillOpacity = .8F;
    foreground.blendMode = c::BlendMode::Screen;
    auto mask = std::make_shared<c::LayerMask>();
    mask->coverage = c::SelectionMask::rectangle({32, 24}, {3, 2, 25, 19}, 193);
    mask->outside = 0;
    foreground.mask = mask;
    auto effects = std::make_shared<c::LayerEffectStack>();
    auto& stroke = effects->items[std::size_t(c::LayerEffectType::Stroke)];
    stroke.enabled = true;
    stroke.color = {190, 20, 65, 255};
    stroke.size = 3;
    foreground.effects = effects;
    CHECK(document.insertLayer(2, std::move(foreground)));
    CHECK(document.setSelection(c::SelectionMask::rectangle({80, 60}, {16, 14, 13, 12}, 177)));
    const auto path = temp.filePath("snapshot-source.vulkana");
    const auto saved = u::saveProject(path, document);
    if (!saved) throw std::runtime_error(saved.error.toStdString());
    return path;
}

struct Fixture {
    QTemporaryDir files;
    u::MainWindow window;
    r::CanvasWindow* canvas {};
    QStringList errors;
    QString exportPath;
    QString savePath;

    explicit Fixture(QVulkanInstance* instance = nullptr)
        : window(instance, false, false)
    {
        window.setUnsavedPromptEnabled(false);
        window.resize(1400, 900);
        window.show();
        window.activateWindow();
        CHECK(imageeditor::test::waitForGraphicsStartup(window));
        for (auto* candidate : QGuiApplication::allWindows())
            if (candidate->objectName() == "VulkanCanvasWindow")
                canvas = dynamic_cast<r::CanvasWindow*>(candidate);
        if (!canvas) throw std::runtime_error("Canvas window is unavailable");
        u::MainWindow::FileInteractions hooks;
        hooks.chooseSavePath = [this] { return savePath; };
        hooks.chooseSnapshotExportPath = [this] { return exportPath; };
        hooks.chooseGifExportPath = [this] { return exportPath; };
        hooks.confirmReplace = [](const QString&) { return true; };
        hooks.reportError = [this](const QString& error) { errors.push_back(error); };
        window.setFileInteractions(std::move(hooks));
        CHECK(window.openImageFromPath(makeFixture(files)));
        settle();
        if (instance) CHECK(waitFor([&] {
            return canvas->rendererStats().framesSubmitted > 0
                && !canvas->presentationSuppressedForResize();
        }, 30000));
    }
    ~Fixture() { window.close(); settle(); }
    c::EditorSession& session() { return const_cast<c::EditorSession&>(window.editorSession()); }
    c::Document& document() { return *session().document(); }
    const std::vector<c::SnapshotFrame>& frames() { return document().snapshotTimeline().frames; }
    QWidget& panel() { return child<QWidget>(window, "SnapshotTimelinePanel"); }
    QListWidget& list() { return child<QListWidget>(window, "SnapshotTimelineFrames"); }
    QAbstractButton& button(const char* name) { return child<QAbstractButton>(window, name); }
    void click(const char* name) { button(name).click(); settle(); }
    void history(bool redo = false)
    {
        const QKeySequence key = redo ? QKeySequence("Ctrl+Shift+Z") : QKeySequence(QKeySequence::Undo);
        for (auto* action : window.findChildren<QAction*>()) {
            if (!action->shortcuts().contains(key)) continue;
            CHECK(action->isEnabled());
            action->trigger();
            settle();
            return;
        }
        throw std::runtime_error("History action is unavailable");
    }
    bool previewVisible()
    {
        // Closed overlays are deleteLater() objects until Qt returns to its
        // outer event loop. Inspect the live presentation, not the first child.
        int visible = 0;
        for (auto* overlay : window.findChildren<QWidget*>("SnapshotTimelinePreviewOverlay"))
            if (overlay->isVisible()) ++visible;
        CHECK(visible <= 1);
        return visible == 1;
    }
    QWidget* preview()
    {
        for (auto* overlay : window.findChildren<QWidget*>("SnapshotTimelinePreviewOverlay"))
            if (overlay->isVisible()) return overlay;
        return nullptr;
    }
    void dragFrameToStart(int row)
    {
        auto& view = list();
        view.setCurrentRow(row);
        view.scrollToItem(view.item(row));
        window.activateWindow();
        settle();
        const auto id = frames()[std::size_t(row)].id;
        const auto start = view.visualItemRect(view.item(row)).center();
        auto* native = view.window()->windowHandle();
        CHECK(native);
        if (!native) return;
        const auto source = view.viewport()->mapToGlobal(start);
        const auto destination = view.viewport()->mapToGlobal(QPoint{3, start.y()});
        // This explicit diagnostic is driven by real platform pointer input,
        // manually or by an external driver. Native DnD requires compositor
        // serials/OLE state that synthetic QWidget events cannot reproduce.
        std::cout << "SNAPSHOT_DRAG " << qulonglong(native->winId()) << ' '
                  << source.x() << ' ' << source.y() << ' '
                  << destination.x() << ' ' << destination.y() << std::endl;
        CHECK(waitFor([&] { return frames().front().id == id; }, 20000));
        CHECK(QWidget::mouseGrabber() == nullptr);
    }
};

struct UnchangedDocument {
    c::Document* document;
    c::Revision revision;
    std::uint64_t contentState;
    bool modified;
    std::size_t undoDepth;
    c::SelectionState selection;
    c::LayerSelectionState layers;
    c::DocumentSnapshot scene;
    explicit UnchangedDocument(Fixture& f)
        : document(&f.document()), revision(document->revision()), contentState(document->contentState()),
          modified(document->isModified()), undoDepth(f.session().history().undoDepth()),
          selection(document->selection()), layers(f.session().layerSelectionState()), scene(document->snapshot()) {}
    void verify(Fixture& f) const
    {
        CHECK(&f.document() == document && document->revision() == revision);
        CHECK(document->contentState() == contentState && document->isModified() == modified);
        CHECK(f.session().history().undoDepth() == undoDepth);
        CHECK(document->selection() == selection && f.session().layerSelectionState() == layers);
        CHECK(f.canvas->scene().document.documentRevision == scene.documentRevision);
        CHECK(f.canvas->scene().document.canvas == scene.canvas);
        CHECK(f.canvas->scene().document.layersBottomToTop.size() == scene.layersBottomToTop.size());
        for (std::size_t i = 0; i < scene.layersBottomToTop.size(); ++i) {
            const auto& expected = scene.layersBottomToTop[i];
            const auto& actual = f.canvas->scene().document.layersBottomToTop[i];
            CHECK(actual.id == expected.id && actual.localToDocument == expected.localToDocument);
            CHECK(actual.mask == expected.mask && actual.effects == expected.effects);
            CHECK(std::get<c::RasterLayerSnapshot>(actual.payload).surface
                == std::get<c::RasterLayerSnapshot>(expected.payload).surface);
        }
    }
};

void timeline(QVulkanInstance* instance = nullptr)
{
    Fixture f(instance);
    auto& toggle = child<QAction>(f.window, "SnapshotTimelineVisibilityAction");
    CHECK(toggle.isCheckable() && !toggle.isChecked() && !f.panel().isVisible());
    bool inPanelsMenu = false;
    for (auto* menu : f.window.findChildren<QMenu*>())
        if (menu->actions().contains(&toggle) && menu->title().remove('&') == "Panels") inPanelsMenu = true;
    CHECK(inPanelsMenu);
    toggle.trigger();
    settle();
    CHECK(toggle.isChecked() && f.panel().isVisible());
    CHECK(f.panel().minimumHeight() == f.panel().maximumHeight());
    const auto checkToolbar = [&] {
        const auto& label = child<QWidget>(f.panel(), "SnapshotTimelineDurationLabel");
        const auto& duration = child<QSpinBox>(f.panel(), "SnapshotTimelineDuration");
        const auto transport = f.button("SnapshotTimelinePreview").geometry()
            .united(f.button("SnapshotTimelinePlay").geometry())
            .united(f.button("SnapshotTimelineLoop").geometry());
        CHECK(label.x() == 8 && duration.width() == 100);
        CHECK(duration.geometry().left() > label.geometry().right());
        CHECK(duration.geometry().right() < f.button("SnapshotTimelineCapture").x());
        CHECK(std::abs(transport.center().x() - f.panel().rect().center().x()) <= 1);
        CHECK(f.button("SnapshotTimelineDelete").geometry().right() < transport.left());
        CHECK(f.button("SnapshotTimelinePreview").geometry().right() < f.button("SnapshotTimelinePlay").x());
        CHECK(f.button("SnapshotTimelinePlay").geometry().right() < f.button("SnapshotTimelineLoop").x());
        CHECK(f.button("SnapshotTimelineExportGif").x() > transport.right());
        CHECK(f.button("SnapshotTimelineExportGif").geometry().right() < f.button("SnapshotTimelineClose").x());
    };
    checkToolbar();
    const auto fixedHeight = f.panel().height();
    f.window.resize(1150, 760);
    settle();
    CHECK(f.panel().height() == fixedHeight);
    checkToolbar();
    f.window.resize(1400, 900);
    settle();
    checkToolbar();
    CHECK(f.frames().size() == 1 && !f.frames()[0].image && f.frames()[0].durationMs == 100);
    CHECK(f.list().count() == 1 && f.list().currentRow() == 0);
    CHECK(f.list().dragEnabled() && f.list().acceptDrops() && f.list().viewport()->acceptDrops());
    CHECK(f.list().item(0)->flags().testFlag(Qt::ItemIsDragEnabled));
    CHECK(f.list().item(0)->data(Qt::AccessibleTextRole).toString().contains("Frame 0"));
    CHECK(!f.button("SnapshotTimelinePreview").isEnabled());
    CHECK(!f.button("SnapshotTimelineExportGif").isEnabled());
    CHECK(!f.button("SnapshotTimelineInsertLayer").isEnabled());
    CHECK(!f.button("SnapshotTimelineExport").isEnabled());

    const auto expected = u::flattenDocument(f.document());
    CHECK(expected);
    const auto selection = f.document().selection();
    const auto layerSelection = f.session().layerSelectionState();
    const auto layers = f.document().layers().size();
    const auto depth = f.session().history().undoDepth();
    f.click("SnapshotTimelineCapture");
    CHECK(waitFor([&] { return f.frames()[0].image != nullptr; }));
    CHECK(image(f.frames()[0]) == expected.image);
    CHECK(f.frames()[0].image->extent == c::Extent2u(80, 60));
    CHECK(f.frames()[0].image->dotsPerInch == 144);
    if (const auto path = qEnvironmentVariable("VULKANA_TIMELINE_LAYOUT_SCREENSHOT"); !path.isEmpty())
        CHECK(f.panel().grab().save(path));
    CHECK(f.document().layers().size() == layers && f.document().selection() == selection);
    CHECK(f.session().layerSelectionState() == layerSelection);
    CHECK(f.session().history().undoDepth() == depth + 1 && f.document().isModified());
    const auto frozen = f.frames()[0].image;
    const auto frozenBytes = frozen->png;
    f.history();
    CHECK(!f.frames()[0].image);
    f.history(true);
    CHECK(f.frames()[0].image == frozen);

    auto& duration = child<QSpinBox>(f.window, "SnapshotTimelineDuration");
    duration.setValue(175);
    QMetaObject::invokeMethod(&duration, "editingFinished", Qt::DirectConnection);
    settle();
    CHECK(f.frames()[0].durationMs == 175);
    f.history();
    CHECK(f.frames()[0].durationMs == 100);
    f.history(true);
    CHECK(f.frames()[0].durationMs == 175);

    {
        UnchangedDocument before(f);
        f.click("SnapshotTimelinePreview");
        CHECK(f.previewVisible());
        before.verify(f);
        const auto viewport = f.canvas->scene().viewport;
        auto* overlay = f.preview();
        CHECK(overlay);
        if (!overlay) return;
        const auto imageRect = overlay->property("SnapshotPreviewImageRect").toRect();
        const QPointF imagePoint = f.canvas->mapFromGlobal(overlay->mapToGlobal(imageRect.center()));
        const auto sendPointer = [&](QEvent::Type type, QPointF point,
                                     Qt::MouseButton button, Qt::MouseButtons buttons) {
            QMouseEvent event(type, point, f.canvas->mapToGlobal(point.toPoint()),
                button, buttons, Qt::NoModifier);
            QCoreApplication::sendEvent(f.canvas, &event);
        };
        sendPointer(QEvent::MouseButtonPress, imagePoint, Qt::LeftButton, Qt::LeftButton);
        sendPointer(QEvent::MouseMove, imagePoint + QPointF(10, 10), Qt::NoButton, Qt::LeftButton);
        sendPointer(QEvent::MouseButtonRelease, imagePoint + QPointF(10, 10), Qt::LeftButton, Qt::NoButton);
        sendPointer(QEvent::MouseButtonPress, imagePoint, Qt::MiddleButton, Qt::MiddleButton);
        sendPointer(QEvent::MouseMove, imagePoint + QPointF(10, 10), Qt::NoButton, Qt::MiddleButton);
        sendPointer(QEvent::MouseButtonRelease, imagePoint + QPointF(10, 10), Qt::MiddleButton, Qt::NoButton);
        settle();
        CHECK(f.previewVisible() && !f.canvas->panDragging());
        CHECK(f.canvas->scene().viewport.pan() == viewport.pan()
            && f.canvas->scene().viewport.zoom() == viewport.zoom());
        before.verify(f); // Native canvas events must not edit behind its overlay.
        if (const auto path = qEnvironmentVariable("VULKANA_SNAPSHOT_TIMELINE_SCREENSHOT"); !path.isEmpty()) {
            QTest::qWait(250);
            CHECK(f.window.grab().save(path));
            CHECK(f.panel().grab().save(path + ".timeline.png"));
            for (auto* overlay : f.window.findChildren<QWidget*>("SnapshotTimelinePreviewOverlay"))
                if (overlay->isVisible()) CHECK(overlay->grab().save(path + ".preview.png"));
        }
        f.click("SnapshotTimelineExitPreview");
        CHECK(!f.previewVisible());
        before.verify(f);
        f.click("SnapshotTimelinePreview");
        QTest::keyClick(&f.window, Qt::Key_Escape);
        settle();
        CHECK(!f.previewVisible());
        before.verify(f);
        // Closing the panel before the deferred button operation runs must
        // invalidate it; no preview may appear above a closed timeline.
        f.button("SnapshotTimelinePreview").click();
        f.button("SnapshotTimelineClose").click();
        settle();
        CHECK(!f.previewVisible() && !f.panel().isVisible() && !toggle.isChecked());
        before.verify(f);
        toggle.trigger();
        settle();
        f.click("SnapshotTimelinePreview");
        f.click("SnapshotTimelineClose");
        CHECK(!f.previewVisible() && !f.panel().isVisible() && !toggle.isChecked());
        before.verify(f);
        toggle.trigger();
        settle();
    }

    // A normal raster edit must dismiss presentation-only preview, not edit its pixels.
    f.click("SnapshotTimelinePreview");
    f.session().setForegroundColor({10, 240, 80, 255});
    child<QAction>(f.window, "FillForegroundAction").trigger();
    CHECK(waitFor([&] { return f.session().history().undoDepth() > depth + 2; }));
    CHECK(!f.previewVisible());
    CHECK(f.frames()[0].image == frozen && frozen->png == frozenBytes);
    const auto changed = u::flattenDocument(f.document());
    CHECK(changed && changed.image != expected.image);
    f.click("SnapshotTimelineCapture");
    CHECK(waitFor([&] { return f.frames()[0].image != frozen; }));
    CHECK(image(f.frames()[0]) == changed.image);
    CHECK(f.frames()[0].durationMs == 175);
    f.history();
    CHECK(f.frames()[0].image == frozen && image(f.frames()[0]) == expected.image);

    f.exportPath = f.files.filePath("frame.png");
    {
        UnchangedDocument before(f);
        f.click("SnapshotTimelineExport");
        QFile exported(f.exportPath);
        CHECK(exported.open(QIODevice::ReadOnly));
        CHECK(exported.readAll() == QByteArray(reinterpret_cast<const char*>(frozenBytes.data()),
            qsizetype(frozenBytes.size())));
        CHECK(QImage(f.exportPath).convertToFormat(QImage::Format_RGBA8888) == expected.image);
        before.verify(f);
        f.exportPath.clear(); // Cancelling the chooser leaves both history and document unchanged.
        f.click("SnapshotTimelineExport");
        before.verify(f);
    }
    const auto insertDepth = f.session().history().undoDepth();
    f.click("SnapshotTimelineInsertLayer");
    CHECK(f.document().layers().size() == layers + 1);
    CHECK(f.session().history().undoDepth() == insertDepth + 1);
    CHECK(f.document().selection() == selection);
    const auto inserted = f.session().activeLayer();
    CHECK(inserted);
    const auto* layer = f.document().layer(*inserted);
    CHECK(layer && layer->localToDocument == c::AffineTransform{} && layer->rasterOrigin == c::Vec2d{});
    CHECK(layer && pixels(*std::get<c::RasterLayer>(layer->payload).surface) == expected.image);
    f.session().setForegroundColor({245, 10, 30, 255});
    child<QAction>(f.window, "FillForegroundAction").trigger();
    CHECK(waitFor([&] { return f.session().history().undoDepth() == insertDepth + 2; }));
    CHECK(frozen->png == frozenBytes && image(f.frames()[0]) == expected.image);
    f.history();
    f.history();
    CHECK(f.document().layers().size() == layers && !f.document().containsLayer(*inserted));

    // Empty and captured slots share the same ordered model; it grows beyond the viewport.
    const auto firstId = f.frames()[0].id;
    for (int i = 0; i < 20; ++i) f.click("SnapshotTimelineAdd");
    CHECK(f.frames().size() == 21 && f.list().count() == 21);
    CHECK(f.list().horizontalScrollBar()->maximum() > 0);
    CHECK(f.panel().height() == fixedHeight);
    const auto lastId = f.frames().back().id;
    f.list().setCurrentRow(20);
    f.window.activateWindow();
    f.list().setFocus();
    CHECK(waitFor([&] { return f.list().hasFocus(); }));
    {
        UnchangedDocument before(f);
        QTest::keyClick(&f.list(), Qt::Key_Left);
        settle();
        CHECK(f.list().currentRow() == 19);
        before.verify(f); // Frame navigation must not nudge the selected artwork.
    }
    QTest::keyClick(&f.list(), Qt::Key_Delete);
    settle();
    CHECK(f.frames().size() == 20 && f.document().layers().size() == layers);
    CHECK(f.document().selection() == selection);
    f.history();
    CHECK(f.frames().size() == 21 && f.frames().back().id == lastId);
    // The drag view publishes its final destination to this same callback;
    // only the document command may reorder the authoritative frame model.
    auto* timelinePanel = dynamic_cast<u::SnapshotTimelinePanel*>(&f.panel());
    CHECK(timelinePanel && timelinePanel->onReorder);
    if (!timelinePanel || !timelinePanel->onReorder) return;
    if (QCoreApplication::arguments().contains("--platform-drag")) f.dragFrameToStart(20);
    else timelinePanel->onReorder(lastId, 0);
    settle();
    CHECK(f.frames()[0].id == lastId && f.frames()[1].id == firstId);
    CHECK(f.list().item(0)->data(Qt::AccessibleTextRole).toString().contains("Frame 0"));
    f.history();
    CHECK(f.frames()[0].id == firstId && f.frames().back().id == lastId);
    f.history(true);
    CHECK(f.frames()[0].id == lastId && f.frames()[1].image == frozen);
    f.list().setCurrentRow(0);
    f.click("SnapshotTimelineDelete");
    CHECK(f.frames().size() == 20 && f.frames()[0].id == firstId);
    f.history();
    CHECK(f.frames().size() == 21 && f.frames()[0].id == lastId);

    auto canvasSpec = f.document().canvas();
    canvasSpec.extent = {120, 90};
    CHECK(f.session().execute(std::make_unique<c::ChangeCanvasSpecCommand>(canvasSpec)));
    CHECK(f.frames()[1].image == frozen && image(f.frames()[1]).size() == QSize(80, 60));
    f.history(); // The ordinary UI history path also refreshes the document viewport.
    CHECK(f.document().canvas().extent == c::Extent2u(80, 60));
    f.savePath = f.files.filePath("timeline.vulkana");
    CHECK(f.window.saveDocument(true));
    const auto savedTimeline = f.document().snapshotTimeline();
    const auto savedId = f.window.activeDocumentId();
    QImage other(13, 17, QImage::Format_RGBA8888);
    other.fill(QColor(40, 70, 20));
    const auto otherPath = f.files.filePath("other.png");
    CHECK(other.save(otherPath));
    f.list().setCurrentRow(1);
    f.click("SnapshotTimelinePreview");
    CHECK(f.previewVisible());
    CHECK(f.window.openImageFromPath(otherPath));
    CHECK(!f.previewVisible());
    CHECK(f.frames().size() == 1 && !f.frames()[0].image);
    CHECK(f.window.closeDocument(savedId));
    CHECK(f.window.openImageFromPath(f.savePath));
    CHECK(f.frames().size() == savedTimeline.frames.size());
    for (std::size_t i = 0; i < f.frames().size(); ++i) {
        CHECK(f.frames()[i].id == savedTimeline.frames[i].id);
        CHECK(f.frames()[i].durationMs == savedTimeline.frames[i].durationMs);
        CHECK(bool(f.frames()[i].image) == bool(savedTimeline.frames[i].image));
        if (f.frames()[i].image) CHECK(f.frames()[i].image->png == savedTimeline.frames[i].image->png);
    }
    CHECK(!f.document().isModified());
    f.list().setCurrentRow(1);
    f.window.activateWindow();
    f.list().setFocus();
    CHECK(waitFor([&] { return f.list().hasFocus(); }));
    {
        UnchangedDocument before(f);
        QTest::keyClick(&f.list(), Qt::Key_Return);
        settle();
        CHECK(f.previewVisible() && !f.canvas->spacePanHeld());
        QTest::keyClick(&f.list(), Qt::Key_Return);
        settle();
        CHECK(!f.previewVisible() && !f.canvas->spacePanHeld());
        before.verify(f);
    }
    const auto restoredCapture = f.frames()[1].image;
    f.click("SnapshotTimelineDelete");
    CHECK(f.frames().size() + 1 == savedTimeline.frames.size());
    CHECK(std::none_of(f.frames().begin(), f.frames().end(),
        [](const auto& frame) { return frame.image != nullptr; }));
    f.history();
    CHECK(f.frames()[1].image == restoredCapture && image(f.frames()[1]) == expected.image);
    for (int i = 0; i < 21 && f.frames().size() > 1; ++i) {
        f.list().setCurrentRow(0);
        f.click("SnapshotTimelineDelete");
    }
    f.click("SnapshotTimelineDelete");
    CHECK(f.frames().size() == 1 && !f.frames()[0].image);
    CHECK(f.list().count() == 1 && f.list().currentRow() == 0);
    CHECK(f.errors.isEmpty());
    if (!f.errors.isEmpty()) std::cerr << f.errors.join('\n').toStdString() << '\n';
}

void focusedPreview(QVulkanInstance* instance = nullptr)
{
    Fixture f(instance);
    child<QAction>(f.window, "SnapshotTimelineVisibilityAction").trigger();
    settle();
    f.click("SnapshotTimelineCapture");
    CHECK(waitFor([&] { return bool(f.frames()[0].image); }));
    UnchangedDocument before(f);
    const auto startPreview = [&] {
        f.click("SnapshotTimelinePreview");
        CHECK(f.previewVisible());
        return f.preview();
    };
    auto* overlay = startPreview();
    if (!overlay) return;
    auto& closePreview = child<QToolButton>(*overlay, "SnapshotTimelineExitPreview");
    const auto& closeTimeline = child<QToolButton>(f.window, "SnapshotTimelineClose");
    CHECK(closePreview.text().isEmpty() && !closePreview.icon().isNull());
    CHECK(closePreview.size() == closeTimeline.size());
    CHECK(closePreview.iconSize() == closeTimeline.iconSize());
    CHECK(closePreview.autoRaise() == closeTimeline.autoRaise());
    CHECK(!closePreview.accessibleName().isEmpty());
    CHECK(overlay->geometry() == overlay->parentWidget()->rect());
    CHECK(child<QWidget>(f.window, "SnapshotTimelinePreviewTopShade").isVisible());
    const auto imageRect = overlay->property("SnapshotPreviewImageRect").toRect();
    CHECK(!imageRect.isEmpty() && overlay->rect().contains(imageRect));
    QTest::mouseClick(overlay, Qt::LeftButton, {}, imageRect.center());
    CHECK(f.previewVisible());
    const auto tool = f.session().activeTool();
    QTest::keyClick(&f.window, Qt::Key_B);
    QTest::keyClick(&f.window, Qt::Key_Z, Qt::ControlModifier);
    CHECK(f.previewVisible() && f.session().activeTool() == tool);
    const auto documentCount = f.window.documentCount();
    QMimeData dragged;
    dragged.setUrls({QUrl::fromLocalFile(f.files.filePath("snapshot-source.vulkana"))});
    QDropEvent drop(QPointF(2, 2), Qt::CopyAction | Qt::MoveAction, &dragged, Qt::LeftButton, Qt::NoModifier);
    drop.setAccepted(true);
    QCoreApplication::sendEvent(f.canvas, &drop);
    CHECK(!drop.isAccepted() && f.window.documentCount() == documentCount && f.previewVisible());
    f.list().setFocus();
    for (int i = 0; i < 12; ++i) {
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Tab);
        CHECK(f.panel().isAncestorOf(QApplication::focusWidget()));
    }
    before.verify(f);

    // A click on dimmed chrome dismisses without reaching the underlying tool.
    auto& color = f.button("RailPrimaryColor");
    int clicks = 0;
    const auto connection = QObject::connect(&color, &QAbstractButton::clicked, [&] { ++clicks; });
    QTest::mouseClick(&color, Qt::LeftButton);
    settle();
    CHECK(!f.previewVisible() && clicks == 0);
    CHECK(!child<QWidget>(f.window, "SnapshotTimelinePreviewTopShade").isVisible());
    QObject::disconnect(connection);
    before.verify(f);

    overlay = startPreview();
    if (!overlay) return;
    QTest::mouseClick(overlay, Qt::LeftButton, {}, QPoint(4, 4));
    settle();
    CHECK(!f.previewVisible());
    before.verify(f);

    overlay = startPreview();
    auto* menu = f.window.menuBar()->actions().front()->menu();
    CHECK(menu);
    if (menu) {
        menu->popup(f.window.menuBar()->mapToGlobal(QPoint(4, f.window.menuBar()->height())));
        settle();
        CHECK(!f.previewVisible());
        CHECK(menu->isVisible());
        menu->close();
        settle();
    }
    before.verify(f);

    startPreview();
    auto& duration = child<QSpinBox>(f.window, "SnapshotTimelineDuration");
    QStyleOptionSpinBox spinStyle;
    spinStyle.initFrom(&duration);
    spinStyle.frame = duration.hasFrame();
    spinStyle.buttonSymbols = duration.buttonSymbols();
    spinStyle.stepEnabled = QAbstractSpinBox::StepUpEnabled | QAbstractSpinBox::StepDownEnabled;
    const auto step = [&](QStyle::SubControl control, int expected) {
        const bool previewing = f.previewVisible();
        const auto buttonRect = duration.style()->subControlRect(QStyle::CC_SpinBox, &spinStyle, control, &duration);
        CHECK(!buttonRect.isEmpty());
        auto* native = duration.window()->windowHandle();
        CHECK(native);
        if (!native) return;
        const auto point = native->mapFromGlobal(duration.mapToGlobal(buttonRect.center()));
        QTest::mousePress(native, Qt::LeftButton, Qt::NoModifier, point);
        QTest::qWait(80); // The spinner changes on press, while it still owns capture.
        CHECK(duration.value() == expected && f.frames()[0].durationMs == std::uint32_t(expected));
        CHECK(f.previewVisible() == previewing);
        QTest::mouseRelease(native, Qt::LeftButton, Qt::NoModifier, point);
        settle();
        CHECK(duration.value() == expected && f.frames()[0].durationMs == std::uint32_t(expected));
    };
    step(QStyle::SC_SpinBoxUp, 110);
    step(QStyle::SC_SpinBoxDown, 100);
    const auto upRect = duration.style()->subControlRect(QStyle::CC_SpinBox, &spinStyle, QStyle::SC_SpinBoxUp, &duration);
    auto* spinWindow = duration.window()->windowHandle();
    const auto upPoint = spinWindow->mapFromGlobal(duration.mapToGlobal(upRect.center()));
    QTest::mousePress(spinWindow, Qt::LeftButton, Qt::NoModifier, upPoint);
    CHECK(waitFor([&] { return f.frames()[0].durationMs > 110; }, 2000));
    QTest::mouseRelease(spinWindow, Qt::LeftButton, Qt::NoModifier, upPoint);
    settle();
    CHECK(f.previewVisible() && std::uint32_t(duration.value()) == f.frames()[0].durationMs);
    f.click("SnapshotTimelineExitPreview");
    while (f.session().history().undoDepth() > before.undoDepth) f.history();
    CHECK(f.frames()[0].durationMs == 100);
    // Same spinner, with the live canvas visible rather than a snapshot.
    step(QStyle::SC_SpinBoxUp, 110);
    step(QStyle::SC_SpinBoxDown, 100);
    f.history(); f.history();
    CHECK(f.frames()[0].durationMs == 100);
    startPreview();
    QTest::mouseClick(&duration, Qt::LeftButton);
    CHECK(f.previewVisible());
    duration.setValue(240);
    settle();
    CHECK(f.frames()[0].durationMs == 240 && f.previewVisible());
    f.click("SnapshotTimelineExitPreview");
    f.history();
    CHECK(f.frames()[0].durationMs == 100);
    CHECK(f.document().contentState() == before.contentState);
    CHECK(f.session().history().undoDepth() == before.undoDepth);

    UnchangedDocument afterDuration(f);
    startPreview();
    CHECK(f.window.close());
    settle();
    CHECK(!f.previewVisible() && !f.window.isVisible());
    afterDuration.verify(f);
}
void gifPlayback(QVulkanInstance* instance = nullptr)
{
    Fixture f(instance);
    const auto path = f.files.filePath("animation.gif");
    {
        QFile file(path); CHECK(file.open(QIODevice::WriteOnly));
        file.write(imageeditor::test::gifFixture());
    }
    CHECK(f.window.openImageFromPath(path));
    CHECK(f.panel().isVisible() && f.frames().size() == 5);
    CHECK(f.list().currentRow() == 0 && !f.previewVisible());
    CHECK(f.session().activeLayer() == f.document().layers().back().id);
    CHECK(f.document().layers().back().visible);
    UnchangedDocument before(f);
    f.click("SnapshotTimelinePlay");
    CHECK(f.previewVisible() && f.button("SnapshotTimelinePlay").isChecked());
    CHECK(!child<QSpinBox>(f.panel(), "SnapshotTimelineDuration").isEnabled());
    const QPointer<QWidget> overlay = f.preview();
    CHECK(waitFor([&] { return f.list().currentRow() != 0; }));
    CHECK(f.preview() == overlay); // No overlay/filter rebuild for each frame.
    auto& play = f.button("SnapshotTimelinePlay");
    auto* native = play.window()->windowHandle();
    CHECK(native);
    const auto point = native->mapFromGlobal(play.mapToGlobal(play.rect().center()));
    QTest::mousePress(native, Qt::LeftButton, Qt::NoModifier, point);
    QTest::qWait(180); // Exercise frame updates between physical press/release.
    CHECK(f.preview() == overlay && play.isChecked());
    QTest::mouseRelease(native, Qt::LeftButton, Qt::NoModifier, point);
    settle();
    CHECK(f.previewVisible() && !f.button("SnapshotTimelinePlay").isChecked());
    CHECK(child<QSpinBox>(f.panel(), "SnapshotTimelineDuration").isEnabled());
    const int paused = f.list().currentRow();
    QTest::qWait(150);
    CHECK(f.list().currentRow() == paused);
    f.list().setCurrentRow(4); settle();
    CHECK(f.list().currentRow() == 4 && f.previewVisible());
    QTest::qWait(150);
    CHECK(f.list().currentRow() == 4);
    f.list().setFocus();
    QTest::keyClick(&f.list(), Qt::Key_Space); settle();
    CHECK(f.button("SnapshotTimelinePlay").isChecked() && !f.canvas->spacePanHeld());
    CHECK(waitFor([&] { return f.list().currentRow() == 0; }));
    f.list().setCurrentRow(3); settle();
    CHECK(f.list().currentRow() == 3 && f.button("SnapshotTimelinePlay").isChecked());
    f.click("SnapshotTimelineExitPreview");
    const auto exited = f.list().currentRow();
    QTest::qWait(200);
    CHECK(!f.previewVisible() && !f.button("SnapshotTimelinePlay").isChecked());
    CHECK(f.list().currentRow() == exited);
    before.verify(f);
    CHECK(f.button("SnapshotTimelineLoop").isChecked());
    f.click("SnapshotTimelineLoop");
    f.list().setCurrentRow(0);f.click("SnapshotTimelinePlay");
    CHECK(waitFor([&]{return !f.button("SnapshotTimelinePlay").isChecked();},3000));
    CHECK(f.list().currentRow()==4 && f.previewVisible());
    QTest::qWait(150);CHECK(f.list().currentRow()==4);
    f.click("SnapshotTimelinePlay");
    CHECK(f.list().currentRow()==0 && f.button("SnapshotTimelinePlay").isChecked());
    // Natural completion while Pause is held must not restart on release.
    QTest::mousePress(native,Qt::LeftButton,Qt::NoModifier,point);
    QTest::qWait(500);
    QTest::mouseRelease(native,Qt::LeftButton,Qt::NoModifier,point);settle();
    CHECK(!f.button("SnapshotTimelinePlay").isChecked() && f.list().currentRow()==4);
    f.click("SnapshotTimelineLoop");f.click("SnapshotTimelineExitPreview");
    before.verify(f);
    // File failure must leave the existing document and snapshots intact.
    CHECK(!f.window.openImageFromPath(f.files.filePath("missing.gif")));
    CHECK(f.errors.size() == 1);
    before.verify(f);
    f.click("SnapshotTimelinePlay");
    CHECK(f.previewVisible());
    CHECK(f.window.close()); settle();
    CHECK(!f.previewVisible() && !f.button("SnapshotTimelinePlay").isChecked());
    before.verify(f);
}

void gifSample(const QString& path, QVulkanInstance* instance = nullptr)
{
    Fixture f(instance);
    CHECK(f.window.openImageFromPath(path));
    if (!f.errors.isEmpty()) { std::cerr << f.errors.join('\n').toStdString(); return; }
    CHECK(f.frames().size() > 1 && f.panel().isVisible());
    UnchangedDocument before(f);
    f.click("SnapshotTimelinePlay");
    CHECK(f.previewVisible());
    int last = f.list().currentRow(), loops = 0;
    CHECK(waitFor([&] {
        const auto row = f.list().currentRow();
        if (row < last) ++loops;
        last = row;
        return loops >= 2;
    }, 15000));
    f.click("SnapshotTimelinePlay");
    CHECK(f.previewVisible());
    if (const auto output = qEnvironmentVariable("VULKANA_SNAPSHOT_TIMELINE_SCREENSHOT"); !output.isEmpty()) {
        CHECK(f.preview()->grab().save(output + ".gif-preview.png"));
        CHECK(f.panel().grab().save(output + ".gif-timeline.png"));
    }
    f.click("SnapshotTimelineExitPreview");
    before.verify(f);
}

void gifExportWorkflow(QVulkanInstance* instance = nullptr)
{
    Fixture f(instance);
    child<QAction>(f.window,"SnapshotTimelineVisibilityAction").trigger();settle();
    CHECK(!f.button("SnapshotTimelineExportGif").isEnabled());
    f.click("SnapshotTimelineCapture");
    CHECK(f.button("SnapshotTimelineExportGif").isEnabled());
    UnchangedDocument before(f);
    f.exportPath=f.files.filePath("animation.gif");
    QTimer inspect,watchdog;int stage=0;
    QObject::connect(&inspect,&QTimer::timeout,&f.window,[&]{
        auto* dialog=dynamic_cast<u::GifExportDialog*>(f.window.findChild<QDialog*>("GifExportDialog"));
        if(!dialog || !dialog->isVisible() || !dialog->previewReady())return;
        if(stage==0){
            inspect.stop(); // Minimize/restore processes events; don't reenter this stage.
            CHECK(!dialog->isWindow() && dialog->windowType()==Qt::SubWindow);
            CHECK(dialog->parentWidget()->objectName()=="WorkspaceDialogShield");
            CHECK(!(dialog->windowFlags() & (Qt::WindowMinimizeButtonHint | Qt::WindowMaximizeButtonHint
                | Qt::WindowCloseButtonHint)));
            CHECK(!dialog->isSizeGripEnabled());
            auto* plane=dialog->window()->windowHandle();
            CHECK(plane && plane!=dialog->windowHandle());
            f.window.showMinimized();QTest::qWait(120);
            if(instance){
                CHECK(waitFor([&]{return !f.window.windowHandle()->isExposed();}));
                CHECK(waitFor([&]{return !plane->isExposed();}));
            }else CHECK(f.window.isMinimized());
            f.window.showNormal();f.window.activateWindow();QTest::qWait(120);
            CHECK(dialog->isVisible());
            if(instance)CHECK(waitFor([&]{return plane->isExposed();}));
            CHECK(dialog->settings().colors==256 && dialog->settings().loop);
            auto& mode=child<QComboBox>(*dialog,"GifExportDither");
            auto& strength=dynamic_cast<u::CompactValueControl&>(child<QDoubleSpinBox>(*dialog,"GifExportStrength"));
            auto& colors=dynamic_cast<u::CompactValueControl&>(child<QDoubleSpinBox>(*dialog,"GifExportColors"));
            CHECK(colors.minimum()==2 && colors.maximum()==256 && colors.decimals()==0);
            CHECK(strength.minimum()==0 && strength.maximum()==100 && strength.decimals()==0);
            const auto drag=[&](u::CompactValueControl& control){
                const auto field=control.valueFieldRect();
                const auto left=field.topLeft()+QPoint(1,field.height()/2);
                const auto middle=field.center();
                if(QGuiApplication::platformName()=="offscreen"){
                    // Headless Qt has no native focus handoff; native runs below
                    // exercise dispatch through the actual panel-plane window.
                    control.setFocus();
                    QTest::mousePress(&control,Qt::LeftButton,Qt::NoModifier,left);
                    CHECK(control.isSliding());
                    QTest::mouseMove(&control,middle);
                    QTest::mouseRelease(&control,Qt::LeftButton,Qt::NoModifier,middle);
                }else{
                    QTest::mousePress(plane,Qt::LeftButton,Qt::NoModifier,plane->mapFromGlobal(control.mapToGlobal(left)));
                    CHECK(control.isSliding());
                    QTest::mouseMove(plane,plane->mapFromGlobal(control.mapToGlobal(middle)));
                    QTest::mouseRelease(plane,Qt::LeftButton,Qt::NoModifier,plane->mapFromGlobal(control.mapToGlobal(middle)));
                }
                CHECK(!control.isSliding());
                CHECK(control.value()==qRound(control.valueFromPosition(middle.x())));
            };
            drag(colors);drag(strength);
            CHECK(dialog->settings().colors==qRound(colors.value()));
            CHECK(dialog->settings().strength==qRound(strength.value()));
            mode.setCurrentIndex(2);CHECK(!strength.isEnabled());
            mode.setCurrentIndex(3);CHECK(!strength.isEnabled());
            mode.setCurrentIndex(1);CHECK(strength.isEnabled());
            mode.setCurrentIndex(0);CHECK(!strength.isEnabled());
            colors.setValue(8);
            child<QCheckBox>(*dialog,"GifExportTransparency").setChecked(false);
            CHECK(child<QPushButton>(*dialog,"GifExportBackground").isEnabled());
            child<QCheckBox>(*dialog,"GifExportLoop").setChecked(false);
            child<QSpinBox>(*dialog,"GifExportWidth").setValue(40);
            CHECK(child<QSpinBox>(*dialog,"GifExportHeight").value()==30);
            CHECK(!child<QPushButton>(*dialog,"GifExportWrite").isEnabled());
            stage=1;inspect.start();
        } else if(stage==1){
            CHECK(dialog->settings().size==QSize(40,30) && !dialog->settings().loop);
            if(const auto path=qEnvironmentVariable("VULKANA_GIF_EXPORT_SCREENSHOT");!path.isEmpty())CHECK(dialog->grab().save(path));
            stage=2;child<QPushButton>(*dialog,"GifExportWrite").click();
        }
    });
    QObject::connect(&watchdog,&QTimer::timeout,&f.window,[&]{
        CHECK(false);if(auto* dialog=f.window.findChild<QDialog*>("GifExportDialog"))dialog->reject();
    });
    inspect.start(10);watchdog.setSingleShot(true);watchdog.start(20000);
    f.click("SnapshotTimelineExportGif");inspect.stop();watchdog.stop();
    CHECK(stage==2 && f.errors.isEmpty());
    QFile file(f.exportPath);CHECK(file.open(QIODevice::ReadOnly));const auto bytes=file.readAll();
    CHECK(bytes.startsWith("GIF89a") && !bytes.contains("NETSCAPE2.0"));
    QImageReader reader(f.exportPath);CHECK(reader.read().size()==QSize(40,30));
    before.verify(f);

    // Preview cancellation publishes no file and never changes document/history.
    f.exportPath=f.files.filePath("cancelled.gif");
    QTimer close;
    QObject::connect(&close,&QTimer::timeout,&f.window,[&]{
        if(auto* dialog=f.window.findChild<QDialog*>("GifExportDialog");dialog && dialog->isVisible())dialog->reject();
    });
    close.start(10);f.click("SnapshotTimelineExportGif");close.stop();
    CHECK(!QFile::exists(f.exportPath));before.verify(f);
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    app.setOrganizationName("ImageEditorTests");
    app.setApplicationName("SnapshotTimelineInteraction");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    u::applyEditorTheme(app);
    QString gif;
    for (const auto& argument : app.arguments()) if (argument.startsWith("--gif=")) gif = argument.mid(6);
    try {
        if (app.arguments().contains("--native")) {
            QVulkanInstance instance;
            instance.setApiVersion(QVersionNumber(1, 2));
            CHECK(instance.create());
            if (instance.isValid()) {
                timeline(&instance); focusedPreview(&instance); gifPlayback(&instance); gifExportWorkflow(&instance);
                if (!gif.isEmpty()) gifSample(gif, &instance);
            }
        } else { timeline(); focusedPreview(); gifPlayback(); gifExportWorkflow(); if (!gif.isEmpty()) gifSample(gif); }
    }
    catch (const std::exception& error) { ++failures; std::cerr << error.what() << '\n'; }
    return failures ? 1 : 0;
}
