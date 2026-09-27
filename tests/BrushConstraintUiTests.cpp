#include "imageeditor/core/EditorSession.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/Theme.hpp"

#include <QAction>
#include <QApplication>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFocusEvent>
#include <QImage>
#include <QMouseEvent>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QVulkanInstance>
#include <atomic>
#include <cmath>
#include <iostream>

namespace c = imageeditor::core;
namespace r = imageeditor::render;
namespace u = imageeditor::ui;
namespace {
int failures = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { ++failures; std::cerr << "FAIL " << __LINE__ << ": " << #__VA_ARGS__ << '\n'; } } while (false)
void settle() { QCoreApplication::sendPostedEvents(); QCoreApplication::processEvents(); }
void exercise(QVulkanInstance* vulkan)
{
    QTemporaryDir files;
    u::MainWindow window(vulkan, false, false);
    window.setUnsavedPromptEnabled(false);
    window.resize(1500, 950); window.show(); settle();
    r::CanvasWindow* canvas = nullptr;
    for (auto* candidate : QGuiApplication::allWindows())
        if (candidate->objectName() == "VulkanCanvasWindow") canvas = dynamic_cast<r::CanvasWindow*>(candidate);
    CHECK(canvas); if (!canvas) return;
    QImage image(160, 120, QImage::Format_RGBA8888); image.fill(QColor(180, 175, 165));
    const auto path = files.filePath("brush.png"); CHECK(image.save(path)); CHECK(window.openImageFromPath(path));
    auto& session = const_cast<c::EditorSession&>(window.editorSession());
    const auto activate = [&](Qt::Key shortcut) {
        for (auto* action : window.findChildren<QAction*>())
            if (action->shortcuts().contains(QKeySequence(shortcut))) { action->trigger(); break; }
        settle();
    };
    activate(Qt::Key_B);
    auto* size = window.findChild<QDoubleSpinBox*>("BrushSizeControl"); CHECK(size); if (!size) return;
    size->setValue(6);
    const auto logical = [&](c::Vec2d point) {
        const auto p = canvas->scene().viewport.documentToViewport(point, {160, 120}, canvas->scene().logicalViewport);
        return QPointF(p.x, p.y);
    };
    const auto mouse = [&](QEvent::Type type, c::Vec2d point, Qt::KeyboardModifiers modifiers) {
        const auto p = logical(point), g = QPointF(canvas->mapToGlobal(p.toPoint()));
        const auto button = type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
        const auto buttons = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton;
        QMouseEvent event(type, p, p, g, button, buttons, modifiers);
        QCoreApplication::sendEvent(canvas, &event); settle();
    };
    const auto snapshot = [&] {
        const auto& surface = *std::get<c::RasterLayer>(session.document()->layer(*session.activeLayer())->payload).surface;
        std::vector<std::byte> data(160 * 120 * 4);
        surface.copyRgba8({0, 0, 160, 120}, data, 160 * 4); return data;
    };
    if (vulkan) {
        QElapsedTimer timer; timer.start();
        while (!canvas->rendererStats().fullUploads && timer.elapsed() < 5000) QTest::qWait(10);
        CHECK(canvas->rendererStats().fullUploads > 0);
    }
    for (bool erase : {false, true}) {
        if (erase) activate(Qt::Key_E);
        CHECK(session.activeTool() == (erase ? c::ToolId::Eraser : c::ToolId::Brush));
        for (bool oneToOne : {false, true}) {
            if (oneToOne) canvas->resetTo100Percent();
            const auto before = snapshot();
            const auto history = session.history().undoDepth();
            mouse(QEvent::MouseButtonPress, {20.25, 30.5}, Qt::ShiftModifier);
            mouse(QEvent::MouseMove, {24.25, 30.75}, Qt::ShiftModifier);
            mouse(QEvent::MouseMove, {90.25, 55.5}, Qt::ShiftModifier);
            const auto position = canvas->scene().constrainedBrushPosition;
            CHECK(position && std::abs(position->x - 90.25) < 1e-6 && std::abs(position->y - 30.5) < 1e-6);
            CHECK(session.history().undoDepth() == history);
            mouse(QEvent::MouseButtonRelease, {90.25, 55.5}, Qt::NoModifier);
            CHECK(!canvas->scene().constrainedBrushPosition && !canvas->pointerGestureActive());
            CHECK(session.history().undoDepth() == history + 1);
            const auto after = snapshot(); CHECK(before != after);
            CHECK(session.undo()); CHECK(snapshot() == before);
            CHECK(session.redo()); CHECK(snapshot() == after);
            CHECK(session.undo()); // Retain a redo branch for cancellation.
            canvas->setDocument(session.document()->snapshot(), false);
            const auto redo = session.history().redoDepth();
            mouse(QEvent::MouseButtonPress, {30, 50}, Qt::ShiftModifier);
            mouse(QEvent::MouseMove, {70, 53}, Qt::ShiftModifier);
            if (oneToOne) {
                QFocusEvent lost(QEvent::FocusOut); QCoreApplication::sendEvent(canvas, &lost);
            } else QTest::keyClick(canvas, Qt::Key_Escape);
            settle();
            CHECK(!canvas->scene().constrainedBrushPosition && !canvas->pointerGestureActive());
            CHECK(snapshot() == before && session.history().redoDepth() == redo);
        }
    }
    if (vulkan) { QTest::qWait(100); CHECK(canvas->rendererStats().framesSubmitted > 0); }
    window.close(); settle();
}
}
int main(int argc, char** argv)
{
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("ImageEditorTests");
    QCoreApplication::setApplicationName("BrushConstraintTests");
    QStandardPaths::setTestModeEnabled(true); QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    u::applyEditorTheme(app);
    if (app.arguments().contains("--native")) {
        if (QGuiApplication::platformName() != "wayland") return 77;
        std::atomic_int messages {0}; QVulkanInstance instance;
        instance.setApiVersion(QVersionNumber(1, 2));
        instance.setLayers({QByteArrayLiteral("VK_LAYER_KHRONOS_validation")});
        instance.installDebugOutputFilter([&](auto severity, auto type, const void* raw) {
            if (type.testFlag(QVulkanInstance::ValidationMessage)
                && (severity.testFlag(QVulkanInstance::WarningSeverity) || severity.testFlag(QVulkanInstance::ErrorSeverity))) {
                ++messages;
                const auto* data = static_cast<const VkDebugUtilsMessengerCallbackDataEXT*>(raw);
                std::cerr << "Vulkan: " << data->pMessage << '\n';
            }
            return false;
        });
        CHECK(instance.create()); if (instance.isValid()) exercise(&instance);
        instance.destroy(); settle(); CHECK(messages == 0);
        std::cout << "Brush constraint Vulkan validation messages: " << messages << '\n';
    } else exercise(nullptr);
    return failures ? 1 : 0;
}
