#include "imageeditor/core/CreativeBrushes.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/PropertiesPanel.hpp"
#include "imageeditor/ui/Theme.hpp"

#include <QAction>
#include <QApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QImage>
#include <QMouseEvent>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QVulkanInstance>
#include <algorithm>
#include <atomic>
#include <iomanip>
#include <iostream>
#include <stdexcept>

namespace c = imageeditor::core;
namespace r = imageeditor::render;
namespace u = imageeditor::ui;

int main(int argc, char** argv)
{
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("ImageEditorTests");
    QCoreApplication::setApplicationName("BrushNativeBenchmark");
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir files;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, files.path());
    u::applyEditorTheme(app);
    std::atomic_uint validationMessages {0};
    QVulkanInstance instance;
    instance.setApiVersion(QVersionNumber(1, 2));
    const bool validation = app.arguments().contains("--validation");
    if (validation) {
        if (!instance.supportedLayers().contains("VK_LAYER_KHRONOS_validation")) return 1;
        instance.setLayers({"VK_LAYER_KHRONOS_validation"});
        instance.installDebugOutputFilter([&](auto severity, auto type, const void* message) {
            if (type.testFlag(QVulkanInstance::ValidationMessage)
                && (severity.testFlag(QVulkanInstance::WarningSeverity) || severity.testFlag(QVulkanInstance::ErrorSeverity))) {
                ++validationMessages;
                std::cerr << static_cast<const VkDebugUtilsMessengerCallbackDataEXT*>(message)->pMessage << '\n';
            }
            return false;
        });
    }
    if (!instance.create()) return 1;
    {
        u::MainWindow window(&instance, false, false);
        window.setUnsavedPromptEnabled(false);
        window.resize(1500, 950);
        window.show();
        QTest::qWait(100);
        r::CanvasWindow* canvas = nullptr;
        for (auto* candidate : QGuiApplication::allWindows())
            if (candidate->objectName() == "VulkanCanvasWindow") canvas = dynamic_cast<r::CanvasWindow*>(candidate);
        u::PropertiesPanel* properties = nullptr;
        for (auto* candidate : window.findChildren<QWidget*>())
            if (auto* panel = dynamic_cast<u::PropertiesPanel*>(candidate)) properties = panel;
        if (!canvas || !properties) throw std::runtime_error("Missing workspace");
        QImage original(1024, 1024, QImage::Format_RGBA8888);
        original.fill(QColor(31, 56, 78, 255));
        const auto path = files.filePath("brush.png");
        if (!original.save(path) || !window.openImageFromPath(path)) return 1;
        window.findChild<QAction*>("ToolAction_brush")->trigger();
        QElapsedTimer ready;
        ready.start();
        while (!canvas->rendererStats().fullUploads && ready.elapsed() < 5000) QTest::qWait(5);
        if (!canvas->rendererStats().fullUploads) return 1;
        (void)c::builtinBrushAssetResolver().cacheStats();
        std::cout << "platform=" << QGuiApplication::platformName().toStdString()
            << " device=" << canvas->rendererStats().deviceName << " validation=" << validation << '\n';
        std::cout << "brush,size,iteration,stroke_ms,input_median_ms,input_p95_ms,input_max_ms,frames,"
                     "max_frame_gap_ms,upload_ms,lifetime_max_upload_ms,upload_bytes,full_uploads,sha256\n" << std::fixed << std::setprecision(3);
        auto& session = const_cast<c::EditorSession&>(window.editorSession());
        for (const auto size : {200., 500.}) for (int iteration = 0; iteration < 3; ++iteration) {
            const auto presets = c::creativeBrushPresets();
            auto settings = std::find_if(presets.begin(), presets.end(), [](const auto& p) { return p.displayName == "Chalk"; })->settings;
            settings.sizePixels = size;
            settings.smoothing = c::BrushSmoothingMode::None;
            properties->onBrushSettingsChanged(settings);
            QTest::qWait(100); // Let setup/previous Undo present, outside measured work.
            const auto before = canvas->rendererStats();
            const auto move = [&](QEvent::Type type, int index) {
                const int leg = index / 30;
                const double t = double(index % 30) / 30;
                const auto p = canvas->scene().viewport.documentToViewport(
                    {128 + 768 * (leg % 2 ? 1 - t : t), 128 + 5.5 * index}, {1024, 1024}, canvas->scene().logicalViewport);
                const QPointF local(p.x, p.y);
                QMouseEvent event(type, local, local, QPointF(canvas->mapToGlobal(local.toPoint())),
                    type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                    type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
                QCoreApplication::sendEvent(canvas, &event);
            };
            std::vector<double> inputs;
            auto observedFrames = before.framesSubmitted;
            QElapsedTimer elapsed;
            elapsed.start();
            double lastFrame = 0, maximumGap = 0;
            QTimer frames;
            frames.setTimerType(Qt::PreciseTimer);
            QObject::connect(&frames, &QTimer::timeout, [&] {
                if (canvas->rendererStats().framesSubmitted == observedFrames) return;
                const auto now = double(elapsed.nsecsElapsed()) / 1.e6;
                maximumGap = std::max(maximumGap, now - lastFrame);
                lastFrame = now;
                observedFrames = canvas->rendererStats().framesSubmitted;
            });
            frames.start(1);
            move(QEvent::MouseButtonPress, 0);
            QEventLoop loop;
            QTimer input;
            input.setTimerType(Qt::PreciseTimer);
            int index = 0;
            QObject::connect(&input, &QTimer::timeout, [&] {
                QElapsedTimer dispatch;
                dispatch.start();
                ++index;
                move(index == 120 ? QEvent::MouseButtonRelease : QEvent::MouseMove, index);
                inputs.push_back(double(dispatch.nsecsElapsed()) / 1.e6);
                if (index == 120) { input.stop(); frames.stop(); loop.quit(); }
            });
            input.start(4); // 250 Hz, preserving every event instead of skipping delayed samples.
            loop.exec();
            const auto total = double(elapsed.nsecsElapsed()) / 1.e6;
            QTest::qWait(100);
            const auto after = canvas->rendererStats();
            std::sort(inputs.begin(), inputs.end());
            const auto& surface = *std::get<c::RasterLayer>(session.document()->layer(*session.activeLayer())->payload).surface;
            QByteArray bytes(1024 * 1024 * 4, Qt::Uninitialized);
            surface.copyRgba8({0, 0, 1024, 1024}, {reinterpret_cast<std::byte*>(bytes.data()), std::size_t(bytes.size())}, 1024 * 4);
            const auto hash = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
            std::cout << "chalk," << size << ',' << iteration << ',' << total << ',' << inputs[60] << ',' << inputs[114] << ',' << inputs.back()
                << ',' << after.framesSubmitted - before.framesSubmitted << ',' << maximumGap
                << ',' << double(after.uploadPreparationNanoseconds - before.uploadPreparationNanoseconds) / 1.e6
                << ',' << double(after.maximumUploadPreparationNanoseconds) / 1.e6
                << ',' << after.uploadedBytes - before.uploadedBytes << ',' << after.fullUploads - before.fullUploads
                << ',' << hash.constData() << std::endl;
            if (!session.undo()) return 1;
            canvas->setDocument(session.document()->snapshot(), false);
        }
        window.close();
        QCoreApplication::processEvents();
    }
    instance.destroy();
    std::cout << "validation_messages=" << validationMessages << '\n';
    return validationMessages ? 1 : 0;
}
