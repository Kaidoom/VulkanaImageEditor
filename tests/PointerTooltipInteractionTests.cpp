#include "imageeditor/core/LayerTransform.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/render/PointerTooltip.hpp"
#include "imageeditor/ui/CrossWindowPointerRouter.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/OverlayDockWorkspace.hpp"
#include "imageeditor/ui/Theme.hpp"

#include <QAction>
#include <QApplication>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFocusEvent>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <QVulkanInstance>

#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <numbers>

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
    for (int pass = 0; pass < 3; ++pass) {
        QCoreApplication::sendPostedEvents();
        QCoreApplication::processEvents();
    }
}
bool waitFor(const std::function<bool()>& predicate, int timeout = 5000)
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
template<class Receiver>
void mouse(Receiver& receiver, QEvent::Type type, QPointF position, Qt::MouseButton button,
    Qt::MouseButtons buttons, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    QMouseEvent event(type, position, position, QPointF(receiver.mapToGlobal(position.toPoint())),
        button, buttons, modifiers);
    QCoreApplication::sendEvent(&receiver, &event);
}
struct Fixture {
    QTemporaryDir assets;
    ui::MainWindow window;
    render::CanvasWindow* canvas {};
    ui::OverlayDockWorkspace* workspace {};
    ui::CrossWindowPointerRouter* router {};
    explicit Fixture(QVulkanInstance* instance = nullptr) : window(instance, false, false)
    {
        window.setUnsavedPromptEnabled(false);
        window.resize(1700, 950);
        window.show();
        settle();
        for (auto* candidate : QGuiApplication::allWindows())
            if (candidate->objectName() == QStringLiteral("VulkanCanvasWindow"))
                canvas = dynamic_cast<render::CanvasWindow*>(candidate);
        workspace = dynamic_cast<ui::OverlayDockWorkspace*>(
            window.findChild<QWidget*>(QStringLiteral("CanvasWorkspace")));
        router = dynamic_cast<ui::CrossWindowPointerRouter*>(
            window.findChild<QObject*>(QStringLiteral("CrossWindowPointerRouter")));
        QImage image(128, 96, QImage::Format_RGBA8888);
        image.fill(QColor(74, 146, 219, 255));
        const auto path = assets.filePath(QStringLiteral("tooltip-source.png"));
        CHECK(image.save(path));
        CHECK(window.openImageFromPath(path));
        shortcut("M");
        button("SelectModeRectangle");
        settle();
    }
    ~Fixture() { window.close(); settle(); }
    bool valid() const { return canvas && workspace && router && window.editorSession().document(); }
    const core::Document& document() const { return *window.editorSession().document(); }
    const core::Layer& layer() const { return *document().layer(*window.editorSession().activeLayer()); }
    const core::RasterSurface& surface() const { return *std::get<core::RasterLayer>(layer().payload).surface; }
    const std::string& text() const { return canvas->scene().pointerTooltip; }
    template<class T> T* find(const char* name)
    {
        auto* result = window.findChild<T*>(QString::fromLatin1(name));
        CHECK(result);
        return result;
    }
    void shortcut(const char* sequence)
    {
        for (auto* action : window.findChildren<QAction*>()) {
            if (!action->shortcuts().contains(QKeySequence(QString::fromLatin1(sequence)))) continue;
            action->trigger(); settle(); return;
        }
        CHECK(false && "Missing shortcut");
    }
    void button(const char* name)
    {
        if (auto* target = find<QToolButton>(name)) target->click();
        settle();
    }
    void number(const char* name, double value)
    {
        if (auto* target = find<QDoubleSpinBox>(name)) target->setValue(value);
        settle();
    }
    QPointF logical(core::Vec2d point) const
    {
        const auto extent = document().canvas().extent;
        const auto& scene = canvas->scene();
        const auto result = scene.viewport.documentToViewport(point,
            {double(extent.width), double(extent.height)}, scene.logicalViewport);
        return {result.x, result.y};
    }
    void press(QPointF point, Qt::KeyboardModifiers mods = Qt::NoModifier)
    {
        mouse(*canvas, QEvent::MouseMove, point, Qt::NoButton, Qt::NoButton, mods);
        mouse(*canvas, QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton, mods);
    }
    void move(QPointF point, Qt::KeyboardModifiers mods = Qt::NoModifier)
    { mouse(*canvas, QEvent::MouseMove, point, Qt::NoButton, Qt::LeftButton, mods); }
    void release(QPointF point, Qt::KeyboardModifiers mods = Qt::NoModifier)
    { mouse(*canvas, QEvent::MouseButtonRelease, point, Qt::LeftButton, Qt::NoButton, mods); }
    void escape()
    {
        QKeyEvent key(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QCoreApplication::sendEvent(canvas, &key);
        settle();
    }
    void idle()
    {
        CHECK(waitFor([&] {
            const auto* timer = window.findChild<QTimer*>(QStringLiteral("SelectionRasterizationTimer"));
            return !canvas->selectionDragging() && (!timer || !timer->isActive());
        }));
    }
    void rectangle(core::Vec2d first, core::Vec2d last)
    {
        press(logical(first)); move(logical(last)); release(logical(last)); idle();
    }
    void checkSizeFromTransform() const
    {
        CHECK(canvas->scene().transformOverlay);
        if (!canvas->scene().transformOverlay) return;
        const auto& overlay = *canvas->scene().transformOverlay;
        const auto& m = overlay.localToDocument;
        const core::Extent2d size {overlay.extent.width * std::hypot(m.m00, m.m10),
            overlay.extent.height * std::hypot(m.m01, m.m11)};
        CHECK(text() == render::pointerSizeText(size));
        CHECK(text().find('-') == std::string::npos);
    }
};

void rectangleAndEllipseUseTheirActualGeometryAtEveryZoom()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid()) return;
    const auto revision = f.surface().revision();
    const auto history = f.window.editorSession().history().undoDepth();
    for (const bool oneToOne : {false, true}) {
        if (oneToOne) f.canvas->resetTo100Percent();
        for (const bool reversed : {false, true}) {
            f.button("SelectModeRectangle");
            const auto a = f.logical(reversed ? core::Vec2d{30.6, 26.6} : core::Vec2d{10.25, 8.25});
            const auto b = f.logical(reversed ? core::Vec2d{10.25, 8.25} : core::Vec2d{30.6, 26.6});
            f.press(a); f.move(b);
            CHECK(f.text() == "H: 19 px, W: 21 px");
            CHECK(std::abs(f.canvas->scene().cursorLogical.x - b.x()) < 1e-6);
            CHECK(std::abs(f.canvas->scene().cursorLogical.y - b.y()) < 1e-6);
            f.escape(); CHECK(f.text().empty());
        }
        f.press(f.logical({-10, -7})); f.move(f.logical({30, 26}));
        CHECK(f.text() == "H: 26 px, W: 30 px"); // Rectangle already clips its aligned geometry.
        f.escape();
        f.button("SelectModeEllipse");
        f.press(f.logical({-10.25, -7.25})); f.move(f.logical({30.5, 26.5}));
        CHECK(f.text() == "H: 34 px, W: 41 px"); // Ellipse geometry is never squeezed to the canvas.
        f.escape();
        f.press(f.logical({10, 10}), Qt::ShiftModifier);
        f.move(f.logical({37, 22}), Qt::ShiftModifier);
        CHECK(f.text() == "H: 27 px, W: 27 px");
        f.release(f.logical({37, 22}), Qt::ShiftModifier);
        CHECK(f.text().empty()); // Hide on release, not after asynchronous mask completion.
        f.idle(); f.shortcut("Ctrl+Z");
    }
    CHECK(f.surface().revision() == revision);
    CHECK(f.window.editorSession().history().undoDepth() == history);
}

void selectionMoveAndLassoConstructionDoNotShowSize()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid()) return;
    f.rectangle({20, 20}, {70, 65});
    const auto mask = f.document().selection();
    f.press(f.logical({30, 30})); f.move(f.logical({34, 32}));
    CHECK(f.text().empty());
    f.escape(); CHECK(f.document().selection() == mask);
    f.shortcut("L");
    f.press(f.logical({85, 10})); f.move(f.logical({105, 18}));
    f.move(f.logical({110, 42})); CHECK(f.text().empty());
    f.escape(); CHECK(f.text().empty());
    f.shortcut("V");
    f.press(f.logical({25, 25})); f.move(f.logical({34, 30}));
    CHECK(f.text().empty());
    f.escape();
}

QPointF center(const std::array<core::Vec2d, 8>& handles)
{
    return {(handles[0].x + handles[4].x) * 0.5, (handles[0].y + handles[4].y) * 0.5};
}
QPointF rotationPoint(const std::array<core::Vec2d, 8>& handles)
{
    const auto c = center(handles);
    const QPointF corner(handles[0].x, handles[0].y);
    const auto direction = corner - c;
    return corner + direction * (15.0 / std::hypot(direction.x(), direction.y()));
}
QPointF rotated(QPointF point, QPointF pivot, double degrees)
{
    const auto d = point - pivot;
    const auto r = degrees * std::numbers::pi / 180.0;
    return pivot + QPointF(std::cos(r) * d.x() - std::sin(r) * d.y(),
        std::sin(r) * d.x() + std::cos(r) * d.y());
}

void layerAndSelectionTransformsShowSizeOrAngleOnlyDuringRelevantDrags()
{
    for (const bool selection : {false, true}) {
        Fixture f;
        CHECK(f.valid());
        if (!f.valid()) return;
        if (selection) f.rectangle({20, 20}, {90, 75});
        else f.shortcut("V");
        const auto revision = f.surface().revision();
        f.shortcut("Ctrl+T");
        CHECK(f.text().empty());
        f.number("TransformAngleControl", 32);
        f.button("TransformFlipHorizontal");
        CHECK(f.text().empty());
        auto handles = f.canvas->logicalTransformHandles();
        const QPointF corner(handles[4].x, handles[4].y);
        f.press(corner); f.move(corner + QPointF(17, 9));
        CHECK(f.canvas->transformDragging());
        f.checkSizeFromTransform();
        f.release(corner + QPointF(17, 9));
        CHECK(f.text().empty());

        handles = f.canvas->logicalTransformHandles();
        const auto middle = center(handles);
        f.press(middle); f.move(middle + QPointF(10, 5));
        CHECK(f.canvas->transformDragging());
        CHECK(f.text().empty());
        f.release(middle + QPointF(10, 5));

        handles = f.canvas->logicalTransformHandles();
        const auto start = rotationPoint(handles);
        const auto end = rotated(start, center(handles), 24);
        f.press(start); f.move(end);
        CHECK(f.canvas->transformDragging());
        CHECK(f.canvas->scene().transformHighlight == core::TransformHandle::Rotate);
        CHECK(f.canvas->scene().transformOverlay);
        if (f.canvas->scene().transformOverlay)
            CHECK(f.text() == render::pointerAngleText(f.canvas->scene().transformOverlay->rotationDegrees));
        CHECK(f.text().starts_with("Angle: "));
        f.release(end); CHECK(f.text().empty());
        CHECK(f.surface().revision() == revision);
        f.button("TransformCancel"); CHECK(f.text().empty());
    }
}

void cancellationAndCrossPanelRoutingCleanUpTooltip()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid()) return;
    QWidget receiver(f.workspace->panelOverlay());
    receiver.setGeometry(15, 80, 160, 100);
    receiver.show();
    f.press(f.logical({10, 10})); f.move(f.logical({30, 20}));
    CHECK(!f.text().empty());
    auto local = f.logical({45, 35});
    auto global = f.canvas->mapToGlobal(local.toPoint());
    const auto routed = f.router->routedEventCount();
    mouse(receiver, QEvent::MouseMove, QPointF(receiver.mapFromGlobal(global)), Qt::NoButton, Qt::LeftButton);
    CHECK(!f.text().empty());
    CHECK(f.router->routedEventCount() > routed);
    CHECK(std::abs(f.canvas->scene().cursorLogical.x - local.x()) <= 1.0);
    CHECK(std::abs(f.canvas->scene().cursorLogical.y - local.y()) <= 1.0);
    mouse(receiver, QEvent::MouseButtonRelease, QPointF(receiver.mapFromGlobal(global)), Qt::LeftButton, Qt::NoButton);
    CHECK(f.text().empty());
    f.idle(); f.shortcut("Ctrl+Z");

    f.press(f.logical({10, 10})); f.move(f.logical({30, 20}));
    QFocusEvent focus(QEvent::FocusOut, Qt::ActiveWindowFocusReason);
    QCoreApplication::sendEvent(f.canvas, &focus);
    settle(); CHECK(f.text().empty());
    f.press(f.logical({10, 10})); f.move(f.logical({30, 20}));
    f.shortcut("B"); CHECK(f.text().empty());

    f.shortcut("V"); f.shortcut("Ctrl+T");
    const auto handles = f.canvas->logicalTransformHandles();
    const QPointF corner(handles[4].x, handles[4].y);
    f.press(corner); f.move(corner + QPointF(15, 10));
    CHECK(!f.text().empty());
    QFocusEvent transformFocus(QEvent::FocusOut, Qt::ActiveWindowFocusReason);
    QCoreApplication::sendEvent(f.canvas, &transformFocus);
    settle(); CHECK(f.text().empty());
    f.button("TransformCancel");
}

int nativeValidation()
{
    if (QGuiApplication::platformName() != QStringLiteral("wayland")) return 77;
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
        std::cerr << "Vulkan pointer tooltip: " << (data ? data->pMessage : "unknown") << '\n';
        return true;
    });
    if (!instance.create()) return EXIT_FAILURE;
    {
        Fixture f(&instance);
        CHECK(f.valid());
        if (!f.valid()) return EXIT_FAILURE;
        CHECK(waitFor([&] { return f.canvas->rendererStats().fullUploads > 0; }));
        QTest::qWait(150);
        CHECK(waitFor([&] { return !f.canvas->presentationSuppressedForResize(); }));
        const auto original = f.canvas->rendererStats();
        const auto revision = f.surface().revision();
        f.press(f.logical({10, 10})); f.move(f.logical({50.2, 40.2}));
        CHECK(waitFor([&] { return f.canvas->rendererStats().pointerTooltipUploads > original.pointerTooltipUploads; }));
        // Each in-flight slot owns a fenced texture. Warm every slot before
        // asserting that unchanged text no longer needs an incremental upload.
        for (int i = 0; i < 8; ++i) {
            const auto frame = f.canvas->rendererStats().framesSubmitted;
            f.canvas->scheduleFrame();
            CHECK(waitFor([&] { return f.canvas->rendererStats().framesSubmitted > frame; }));
        }
        // Identical text is reused even when the pointer/subpixel endpoint moves.
        const auto cached = f.canvas->rendererStats();
        for (int i = 0; i < 6; ++i) {
            const auto frame = f.canvas->rendererStats().framesSubmitted;
            f.move(f.logical({50.2 + 0.02 * i, 40.2 + 0.02 * i}));
            f.canvas->scheduleFrame();
            CHECK(waitFor([&] { return f.canvas->rendererStats().framesSubmitted > frame; }));
            CHECK(f.canvas->rendererStats().pointerTooltipUploads == cached.pointerTooltipUploads);
            CHECK(f.canvas->rendererStats().pointerTooltipRasterizations == cached.pointerTooltipRasterizations);
            CHECK(f.canvas->rendererStats().pointerTooltipTextureAllocations == cached.pointerTooltipTextureAllocations);
        }
        if (const auto output = qEnvironmentVariable("IMAGEEDITOR_TEST_POINTER_TOOLTIP_SCREENSHOT"); !output.isEmpty()) {
            QProcess capture;
            capture.start(QStringLiteral("spectacle"), {QStringLiteral("--background"),
                QStringLiteral("--activewindow"), QStringLiteral("--nonotify"),
                QStringLiteral("--output"), output});
            CHECK(capture.waitForFinished(10000));
            CHECK(capture.exitCode() == 0);
        }
        f.escape(); CHECK(f.text().empty());
        f.shortcut("V"); f.shortcut("Ctrl+T");
        const auto handles = f.canvas->logicalTransformHandles();
        const QPointF corner(handles[4].x, handles[4].y);
        f.press(corner); f.move(corner + QPointF(30, 20));
        auto frame = f.canvas->rendererStats().framesSubmitted;
        CHECK(waitFor([&] { return f.canvas->rendererStats().framesSubmitted > frame; }));
        f.release(corner + QPointF(30, 20));
        const auto rotatedHandles = f.canvas->logicalTransformHandles();
        const auto start = rotationPoint(rotatedHandles);
        const auto end = rotated(start, center(rotatedHandles), 21);
        f.press(start); f.move(end);
        frame = f.canvas->rendererStats().framesSubmitted;
        CHECK(waitFor([&] { return f.canvas->rendererStats().framesSubmitted > frame; }));
        f.release(end);
        // Completed transform previews survive a native minimize/restore and
        // application deactivation, while Vulkan reconstructs presentation.
        for (const bool selection : {false, true}) {
            if (selection) {
                f.button("TransformCancel");
                f.shortcut("M"); f.shortcut("Ctrl+A"); f.shortcut("Ctrl+T");
                f.number("TransformAngleControl", 24.5);
            }
            CHECK(f.canvas->scene().transformOverlay);
            const auto geometry = f.canvas->scene().transformOverlay->localToDocument;
            QEvent deactivate(QEvent::ApplicationDeactivate);
            QCoreApplication::sendEvent(qApp, &deactivate);
            CHECK(f.canvas->scene().transformOverlay);
            f.window.showMinimized(); QTest::qWait(100);
            CHECK(f.canvas->scene().transformOverlay);
            CHECK(f.window.editorSession().activeTool() == core::ToolId::Transform);
            f.window.showNormal(); f.window.activateWindow(); f.canvas->requestActivate();
            frame = f.canvas->rendererStats().framesSubmitted;
            CHECK(waitFor([&] { return f.canvas->rendererStats().framesSubmitted > frame
                && !f.canvas->presentationSuppressedForResize(); }));
            CHECK(f.canvas->scene().transformOverlay
                && f.canvas->scene().transformOverlay->localToDocument == geometry);
            CHECK(f.text().empty());
        }
        f.button("TransformCancel");
        const auto after = f.canvas->rendererStats();
        CHECK(after.pointerTooltipRasterizations > original.pointerTooltipRasterizations);
        CHECK(after.pointerTooltipUploadedBytes > original.pointerTooltipUploadedBytes);
        CHECK(after.fullUploads == original.fullUploads && after.regionalUploads == original.regionalUploads);
        CHECK(f.surface().revision() == revision);
    }
    instance.destroy(); settle();
    CHECK(warnings == 0 && errors == 0);
    std::cout << "Native pointer tooltip Vulkan validation: " << warnings << " warnings, " << errors << " errors\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("ImageEditorTests"));
    QCoreApplication::setApplicationName(QStringLiteral("PointerTooltipInteractions"));
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    CHECK(settings.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    ui::applyEditorTheme(application);
    if (qEnvironmentVariableIsSet("IMAGEEDITOR_TEST_POINTER_TOOLTIP_NATIVE")) return nativeValidation();
    rectangleAndEllipseUseTheirActualGeometryAtEveryZoom();
    selectionMoveAndLassoConstructionDoNotShowSize();
    layerAndSelectionTransformsShowSizeOrAngleOnlyDuringRelevantDrags();
    cancellationAndCrossPanelRoutingCleanUpTooltip();
    if (failures) std::cerr << failures << " pointer tooltip assertion(s) failed\n";
    else std::cout << "All pointer tooltip interaction tests passed\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
