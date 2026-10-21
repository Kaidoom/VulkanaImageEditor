#include "imageeditor/ui/BackgroundRemovalMask.hpp"
#include "imageeditor/ui/BackgroundRemovalPanel.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/OverlayDockWorkspace.hpp"
#include "imageeditor/ui/ProjectFile.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/render/CanvasWindow.hpp"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTest>
#include <QVulkanInstance>
#include <QWheelEvent>

#include <iostream>
#include <stdexcept>

namespace c = imageeditor::core;
namespace u = imageeditor::ui;
namespace {
int failures = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { ++failures; \
    std::cerr << __LINE__ << ": " #__VA_ARGS__ "\n"; } } while (false)

void spin()
{
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

bool wait(const std::function<bool()>& predicate, int milliseconds = 5000)
{
    QElapsedTimer timer;
    timer.start();
    do {
        spin();
        if (predicate()) return true;
        QTest::qWait(10);
    } while (timer.elapsed() < milliseconds);
    return false;
}

template<class T> T* child(QObject& owner, const char* name)
{
    auto* found = owner.findChild<T*>(QString::fromLatin1(name));
    if (!found) throw std::runtime_error(std::string("Missing UI object: ") + name);
    return found;
}

u::BackgroundRemovalPanel* panel(u::MainWindow& window)
{
    for (auto* widget : window.findChildren<QWidget*>("BackgroundRemovalPanel"))
        if (auto* result = dynamic_cast<u::BackgroundRemovalPanel*>(widget);
            result && !result->isHidden()) return result;
    return nullptr;
}

u::BackgroundRemovalPanel& activePanel(u::MainWindow& window)
{
    auto* active = panel(window);
    if (!active) throw std::runtime_error("Background removal panel did not open");
    return *active;
}

imageeditor::render::CanvasWindow& canvas()
{
    for (auto* window : QGuiApplication::allWindows())
        if (window->objectName() == "VulkanCanvasWindow")
            if (auto* result = dynamic_cast<imageeditor::render::CanvasWindow*>(window)) return *result;
    throw std::runtime_error("Canvas window is unavailable");
}

QImage sourceImage(const QString& path)
{
    if (!path.isEmpty()) {
        QImage image(path);
        if (image.isNull()) throw std::runtime_error("Cannot read the optional test image");
        return image.convertToFormat(QImage::Format_RGBA8888);
    }
    QImage image(160, 112, QImage::Format_RGBA8888);
    image.fill(QColor(210, 230, 245));
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(220, 170, 130));
    painter.drawEllipse(QRectF(54, 12, 48, 48));
    painter.setBrush(QColor(45, 70, 180));
    painter.drawRoundedRect(QRectF(35, 56, 85, 68), 18, 18);
    painter.setBrush(QColor(45, 30, 20));
    painter.drawEllipse(QRectF(68, 30, 4, 4));
    painter.drawEllipse(QRectF(86, 30, 4, 4));
    return image;
}

QString fixture(const QTemporaryDir& temp, const QImage& image)
{
    const c::Extent2u extent{std::uint32_t(image.width()), std::uint32_t(image.height())};
    auto surface = std::make_shared<c::ContiguousRasterSurface>(extent);
    (void)surface->replaceRgba8({0, 0, image.width(), image.height()},
        {reinterpret_cast<const std::byte*>(image.constBits()), std::size_t(image.sizeInBytes())},
        std::size_t(image.bytesPerLine()));
    c::Document document({{extent.width + 40, extent.height + 30}, 96});
    CHECK(document.insertLayer(0, c::Layer::raster("Unrelated background",
        std::make_shared<c::ContiguousRasterSurface>(document.canvas().extent, c::Rgba8{90, 20, 10, 255}))));
    auto layer = c::Layer::raster("Foreground", surface);
    layer.localToDocument = {1, -.05, 16, .05, 1, 7};
    layer.rasterOrigin = {-3, 5};
    layer.opacity = .7F;
    layer.fillOpacity = .8F;
    auto mask = std::make_shared<c::LayerMask>();
    mask->coverage = c::SelectionMask::filled(extent, 211);
    mask->localToMask = {1, 0, 3, 0, 1, -5};
    mask->outside = 255;
    layer.mask = mask;
    CHECK(document.insertLayer(1, layer));
    CHECK(document.setSelection(c::SelectionMask::rectangle(document.canvas().extent, {2, 4, 13, 17}, 91)));
    const auto path = temp.filePath("background-removal-source.vulkana");
    const auto saved = u::saveProject(path, document);
    if (!saved) throw std::runtime_error(saved.error.toStdString());
    return path;
}

struct DocumentBefore {
    c::Document* document {};
    c::LayerId id {};
    c::LayerMaskState mask;
    c::SelectionState selection;
    c::LayerSelectionState layerSelection;
    c::AffineTransform transform;
    c::Vec2d origin;
    QImage pixels;
    c::Revision surfaceRevision {};
    std::uint64_t contentState {};
    std::size_t undoDepth {};
    std::size_t layers {};
};

DocumentBefore remember(u::MainWindow& window)
{
    auto& session = const_cast<c::EditorSession&>(window.editorSession());
    auto* document = session.document();
    const auto id = *session.activeLayer();
    const auto* layer = document->layer(id);
    const auto source = u::backgroundRemovalSource(*layer);
    return {document, id, layer->mask, document->selection(), session.layerSelectionState(),
        layer->localToDocument, layer->rasterOrigin, source.rgba, source.sourceRevision,
        document->contentState(), session.history().undoDepth(), document->layers().size()};
}

void preserved(u::MainWindow& window, const DocumentBefore& before, bool originalMask = true)
{
    const auto& session = window.editorSession();
    CHECK(session.document() == before.document);
    const auto* layer = before.document->layer(before.id);
    CHECK(layer);
    if (!layer) return;
    if (originalMask) {
        CHECK(layer->mask == before.mask);
        CHECK(before.document->contentState() == before.contentState);
        CHECK(session.history().undoDepth() == before.undoDepth);
    }
    CHECK(before.document->selection() == before.selection);
    CHECK(session.layerSelectionState() == before.layerSelection);
    CHECK(layer->localToDocument == before.transform && layer->rasterOrigin == before.origin);
    CHECK(before.document->layers().size() == before.layers);
    const auto source = u::backgroundRemovalSource(*layer);
    CHECK(source.rgba == before.pixels && source.sourceRevision == before.surfaceRevision);
}

void start(u::MainWindow& window)
{
    auto* action = child<QAction>(window, "RemoveBackgroundAction");
    if (!action->isEnabled()) throw std::runtime_error("Remove Background action is unexpectedly disabled");
    action->trigger();
    (void)activePanel(window);
}

void cancel(u::MainWindow& window)
{
    child<QPushButton>(activePanel(window), "BackgroundRemovalCancel")->click();
    CHECK(wait([&] { return !panel(window); }));
    CHECK(wait([&] { return child<QAction>(window, "RemoveBackgroundAction")->isEnabled(); }));
    auto* workspace = dynamic_cast<u::OverlayDockWorkspace*>(child<QWidget>(window, "CanvasWorkspace"));
    CHECK(workspace && !workspace->panelsSuppressed());
    CHECK(canvas().scene().document.layersBottomToTop.size() == window.editorSession().document()->layers().size());
    CHECK(canvas().scene().document.selection == window.editorSession().document()->selection());
}

void navigationBarrier(u::MainWindow& window, const DocumentBefore& before)
{
    auto& surface = canvas();
    const auto viewport = surface.scene().viewport;
    const auto measurement = surface.scene().measurement;
    const QPointF start{180, 150}, end{207, 169};
    const auto sendMouse = [&](QEvent::Type type, QPointF point,
                               Qt::MouseButton button, Qt::MouseButtons buttons) {
        const QPointF global = surface.mapToGlobal(point.toPoint());
        QMouseEvent event(type, point, global, button, buttons, Qt::NoModifier);
        QCoreApplication::sendEvent(&surface, &event);
    };
    sendMouse(QEvent::MouseButtonPress, start, Qt::MiddleButton, Qt::MiddleButton);
    CHECK(surface.panDragging());
    sendMouse(QEvent::MouseMove, end, Qt::NoButton, Qt::MiddleButton);
    CHECK(surface.scene().viewport.pan() == viewport.pan() + c::Vec2d{27, 19});
    sendMouse(QEvent::MouseButtonRelease, end, Qt::MiddleButton, Qt::NoButton);
    CHECK(!surface.panDragging());
    const auto zoom = surface.zoom();
    QWheelEvent wheel(start, surface.mapToGlobal(start.toPoint()), {}, {0, 120},
        Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(&surface, &wheel);
    CHECK(surface.zoom() > zoom);

    // Space begins over the canvas, but its release can reach a panel child
    // after focus changes. It must clear the canvas latch before panel routing.
    QTest::keyPress(&surface, Qt::Key_Space);
    CHECK(surface.spacePanHeld());
    auto* original = child<QPushButton>(activePanel(window), "BackgroundRemovalOriginal");
    QWidget* control = original->isEnabled() ? static_cast<QWidget*>(original)
        : child<QComboBox>(activePanel(window), "BackgroundRemovalView");
    control->setFocus(Qt::MouseFocusReason);
    QTest::mouseMove(control);
    QTest::keyRelease(control, Qt::Key_Space);
    CHECK(!surface.spacePanHeld());
    const auto beforeLeft = surface.scene().viewport;
    sendMouse(QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    CHECK(!surface.panDragging());
    sendMouse(QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton);
    sendMouse(QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);
    CHECK(!surface.panDragging() && surface.scene().viewport.pan() == beforeLeft.pan());
    CHECK(surface.zoom() == beforeLeft.zoom());
    preserved(window, before);
    surface.restoreDocumentView(viewport, measurement);
    control->clearFocus();
}

void inputBarrier(u::MainWindow& window, const DocumentBefore& before,
    u::DocumentInstanceId other)
{
    const auto active = window.activeDocumentId();
    CHECK(!window.activateDocument(other));
    CHECK(window.activeDocumentId() == active);
    QTest::keyClick(&window, Qt::Key_Delete);
    QTest::keyClick(&window, Qt::Key_A, Qt::ControlModifier);
    child<QAction>(window, "DeleteSelectedLayersAction")->trigger();
    spin();
    preserved(window, before);
    navigationBarrier(window, before);
}

void cancelBeforePreparation(u::MainWindow& window, const DocumentBefore& before,
    u::DocumentInstanceId other)
{
    // Cancellation before preparation yields cannot publish even a partial copy.
    start(window);
    inputBarrier(window, before, other);
    cancel(window);
    preserved(window, before);

    const auto target = window.activeDocumentId();
    CHECK(window.activateDocument(other));
    CHECK(window.activateDocument(target));
}

void realModel(u::MainWindow& window, const DocumentBefore& before,
    u::DocumentInstanceId other)
{
    start(window);
    inputBarrier(window, before, other);
    auto* model = child<QComboBox>(activePanel(window), "BackgroundRemovalModel");
    CHECK(model->currentData().toString() == "u2netp");
    // Let preparation/inference start, then obsolete it with the other model.
    QTest::qWait(40);
    model->setCurrentIndex(1);
    if (!wait([&] {
            auto* p = panel(window);
            return p && !child<QProgressBar>(*p, "BackgroundRemovalProgress")->isVisible();
        }, 120000))
        throw std::runtime_error("Cancelled model inference did not settle");
    CHECK(child<QPushButton>(activePanel(window), "BackgroundRemovalApply")->isEnabled());
    preserved(window, before);
    model->setCurrentIndex(0);
    if (!wait([&] {
            auto* p = panel(window);
            return p && !child<QProgressBar>(*p, "BackgroundRemovalProgress")->isVisible();
        }, 180000))
        throw std::runtime_error("U2NetP preview did not finish");
    CHECK(model->count() == 2);
    {
        CHECK(model->itemData(1).toString() == "birefnet-lite-int8");
        // A pending quantized graph may not publish over the selected U2NetP
        // result. Switching back then exercises Apply/undo with the INT8 mask.
        model->setCurrentIndex(1);
        QTest::qWait(40);
        model->setCurrentIndex(0);
        for (const int index : { 0, 1 }) {
            model->setCurrentIndex(index);
            if (!wait([&] {
                    auto* p = panel(window);
                    return p && !child<QProgressBar>(*p, "BackgroundRemovalProgress")->isVisible();
                }, 180000))
                throw std::runtime_error("BiRefNet comparison preview did not finish");
            if (!child<QPushButton>(activePanel(window), "BackgroundRemovalApply")->isEnabled())
                throw std::runtime_error(child<QLabel>(activePanel(window), "BackgroundRemovalStatus")->text().toStdString());
            preserved(window, before);
        }
    }
    auto& p = activePanel(window);
    if (!child<QPushButton>(p, "BackgroundRemovalApply")->isEnabled())
        throw std::runtime_error(child<QLabel>(p, "BackgroundRemovalStatus")->text().toStdString());
    navigationBarrier(window, before);
    CHECK(!child<QLabel>(p, "BackgroundRemovalTiming")->text().isEmpty());
    if (!qEnvironmentVariableIsEmpty("VULKANA_BACKGROUND_REMOVAL_CAPTURE"))
        CHECK(p.grab().save(qEnvironmentVariable("VULKANA_BACKGROUND_REMOVAL_CAPTURE")));
    const auto& initialPreview = canvas().scene().document;
    CHECK(initialPreview.layersBottomToTop.size() == 1 && !initialPreview.selection);
    if (initialPreview.layersBottomToTop.size() != 1)
        throw std::runtime_error("Canvas did not isolate the foreground preview");
    const auto previewMask = initialPreview.layersBottomToTop.front().mask;
    CHECK(previewMask && previewMask != before.mask);
    CHECK(initialPreview.layersBottomToTop.front().localToDocument == before.transform);
    CHECK(initialPreview.layersBottomToTop.front().rasterOrigin == before.origin);
    CHECK(initialPreview.layersBottomToTop.front().opacity == 1
        && initialPreview.layersBottomToTop.front().fillOpacity == 1);
    auto* views = child<QComboBox>(p, "BackgroundRemovalView");
    for (int index = 0; index < views->count(); ++index) {
        views->setCurrentIndex(index);
        spin();
        preserved(window, before);
        const auto& scene = canvas().scene();
        CHECK(scene.document.layersBottomToTop.size() == 1);
        if (index == 0) CHECK(scene.checkerLight != scene.checkerDark);
        if (index == 1 || index == 3)
            CHECK(scene.checkerLight == c::Rgba8{0, 0, 0, 255} && scene.checkerDark == scene.checkerLight);
        if (index == 2)
            CHECK(scene.checkerLight == c::Rgba8{255, 255, 255, 255} && scene.checkerDark == scene.checkerLight);
        if (index == 3) CHECK(!scene.document.layersBottomToTop.front().mask);
        else CHECK(scene.document.layersBottomToTop.front().mask == previewMask);
    }
    auto* original = child<QPushButton>(p, "BackgroundRemovalOriginal");
    // Synthetic canvas gestures leave native child-window focus events queued.
    // Settle that hand-off before beginning the hold, as a real click does.
    original->setFocus(Qt::MouseFocusReason);
    spin();
    QTest::mousePress(original, Qt::LeftButton);
    spin();
    preserved(window, before);
    CHECK(canvas().scene().document.layersBottomToTop.front().mask == before.mask);
    QTest::mouseRelease(original, Qt::LeftButton);
    spin();
    views->setCurrentIndex(0);
    child<QPushButton>(p, "BackgroundRemovalApply")->click();
    CHECK(wait([&] { return !panel(window); }));
    preserved(window, before, false);
    auto& session = const_cast<c::EditorSession&>(window.editorSession());
    CHECK(session.history().undoDepth() == before.undoDepth + 1);
    const auto applied = session.document()->layer(before.id)->mask;
    CHECK(applied && applied != before.mask && applied->outside == 0 && applied->enabled);
    CHECK(applied == previewMask);
    CHECK(applied->coverage->extent() == c::Extent2u{
        std::uint32_t(before.pixels.width()), std::uint32_t(before.pixels.height())});
    CHECK(applied->localToMask == c::AffineTransform{1, 0, -before.origin.x, 0, 1, -before.origin.y});
    CHECK(session.undo());
    preserved(window, before);
    CHECK(session.redo());
    preserved(window, before, false);
    CHECK(session.document()->layer(before.id)->mask == applied);
    CHECK(session.undo());
    preserved(window, before);
    start(window);
    cancel(window);
    preserved(window, before);
}

QString argument(const QStringList& arguments, const QString& name)
{
    const auto index = arguments.indexOf(name);
    if (index < 0) return {};
    if (index + 1 >= arguments.size()) throw std::runtime_error("Missing command-line test argument");
    return arguments[index + 1];
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir temporary;
    if (!temporary.isValid()) return 1;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
    u::applyEditorTheme(app);
    const auto previousModelRoot = qgetenv("VULKANA_BACKGROUND_REMOVAL_MODELS");
    try {
        // Shipping entry points ignore the former research-directory override.
        qputenv("VULKANA_BACKGROUND_REMOVAL_MODELS", "/not-a-model-directory");
        QVulkanInstance instance;
        QVulkanInstance* vulkan = nullptr;
        if (app.arguments().contains("--native")) {
            instance.setApiVersion(QVersionNumber(1, 2));
            instance.setLayers({"VK_LAYER_KHRONOS_validation"});
            if (!instance.create()) throw std::runtime_error("Cannot initialize native Vulkan test");
            vulkan = &instance;
            instance.installDebugOutputFilter([](auto severity, auto type, const void* data) {
                if (type.testFlag(QVulkanInstance::ValidationMessage)
                    && (severity.testFlag(QVulkanInstance::WarningSeverity)
                        || severity.testFlag(QVulkanInstance::ErrorSeverity))) {
                    ++failures;
                    std::cerr << static_cast<const VkDebugUtilsMessengerCallbackDataEXT*>(data)->pMessage << '\n';
                }
                return false;
            });
        }
        u::MainWindow window(vulkan, false, false);
        window.setUnsavedPromptEnabled(false);
        window.resize(1280, 860);
        window.show();
        spin();
        if (vulkan && !wait([&] { return window.startupGraphicsReady(); }, 60000))
            throw std::runtime_error("Graphics startup did not finish");
        auto image = sourceImage(argument(app.arguments(), "--image"));
        const auto second = temporary.filePath("other.png");
        CHECK(image.save(second));
        CHECK(window.openImageFromPath(second));
        const auto other = window.activeDocumentId();
        CHECK(window.openImageFromPath(fixture(temporary, image)));
        spin();
        child<QAction>(window, "SelectAllAction")->trigger();
        spin();
        const auto before = remember(window);
        CHECK(before.selection);
        cancelBeforePreparation(window, before, other);
        realModel(window, before, other);
        // Enter actual model initialization before closing, rather than merely
        // cancelling the source-copy timer before any worker has been started.
        start(window);
        child<QComboBox>(activePanel(window), "BackgroundRemovalModel")->setCurrentIndex(1);
        CHECK(wait([&] {
            return child<QLabel>(activePanel(window), "BackgroundRemovalStatus")->text()
                .contains(QStringLiteral("Removing background"));
        }, 20000));
        CHECK(child<QProgressBar>(activePanel(window), "BackgroundRemovalProgress")->isVisible());
        window.close();
        CHECK(wait([&] { return !window.isVisible(); }, 20000));
        preserved(window, before);
    } catch (const std::exception& error) {
        ++failures;
        std::cerr << error.what() << '\n';
    }
    if (previousModelRoot.isNull()) qunsetenv("VULKANA_BACKGROUND_REMOVAL_MODELS");
    else qputenv("VULKANA_BACKGROUND_REMOVAL_MODELS", previousModelRoot);
    std::cout << (failures ? "Background-removal interaction checks FAILED.\n"
                          : "Background-removal interaction checks passed.\n");
    return failures ? 1 : 0;
}
