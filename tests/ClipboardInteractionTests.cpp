#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/NewDocumentDialog.hpp"
#include "imageeditor/ui/OverlayDockWorkspace.hpp"
#include "imageeditor/ui/TextController.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/core/LayerCommands.hpp"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QElapsedTimer>
#include <QImage>
#include <QLineEdit>
#include <QMimeData>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QVulkanInstance>
#include <iostream>

namespace c = imageeditor::core;
namespace u = imageeditor::ui;
namespace {
int failures = 0;
void check(bool value, const char* expression, int line)
{ if (!value) { ++failures; std::cerr << "FAIL " << line << ": " << expression << '\n'; } }
#define CHECK(x) check(bool(x), #x, __LINE__)
template<class F> bool waitFor(F predicate)
{
    QElapsedTimer clock; clock.start();
    while (!predicate() && clock.elapsed() < 4000) QTest::qWait(10);
    return predicate();
}
}

int main(int argc, char** argv)
{
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    const bool systemClipboard = app.arguments().contains("--system-clipboard");
    QTemporaryDir files;
    QStandardPaths::setTestModeEnabled(true);
    app.setOrganizationName("ImageEditorTests"); app.setApplicationName("ClipboardInteraction");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, files.path());
    u::applyEditorTheme(app);
    QVulkanInstance vulkan;
    int validation = 0;
    if (systemClipboard) {
        vulkan.setApiVersion(QVersionNumber(1, 2));
        vulkan.setLayers({"VK_LAYER_KHRONOS_validation"});
        vulkan.installDebugOutputFilter([&](QVulkanInstance::DebugMessageSeverityFlags flags,
            QVulkanInstance::DebugMessageTypeFlags types, const void* message) {
            if (!types.testFlag(QVulkanInstance::ValidationMessage)
                || !(flags & (QVulkanInstance::WarningSeverity | QVulkanInstance::ErrorSeverity))) return false;
            ++validation;
            std::cerr << static_cast<const VkDebugUtilsMessengerCallbackDataEXT*>(message)->pMessage << '\n';
            return false;
        });
        if (!vulkan.create()) return 1;
    }
    {
        u::MainWindow window(systemClipboard ? &vulkan : nullptr, false);
        window.setUnsavedPromptEnabled(false);
        window.show(); window.activateWindow();
        auto& session = const_cast<c::EditorSession&>(window.editorSession());
        auto* document = session.document();
        auto* paste = window.findChild<QAction*>("PasteImageAsLayerAction");
        imageeditor::render::CanvasWindow* canvas = nullptr;
        u::TextController* text = nullptr;
        for (auto* w : QGuiApplication::allWindows())
            if (auto* cw = dynamic_cast<imageeditor::render::CanvasWindow*>(w)) canvas = cw;
        for (auto* child : window.children())
            if (auto* tc = dynamic_cast<u::TextController*>(child)) text = tc;
        CHECK(document && paste && canvas && text);
        if (!document || !paste || !canvas || !text) return 1;
        QTest::qWait(100);
        if (systemClipboard) {
            CHECK(waitFor([&] { return QGuiApplication::focusWindow() == window.windowHandle()
                && canvas->rendererStats().framesSubmitted > 0; }));
            const auto* mime = QApplication::clipboard()->mimeData();
            CHECK(mime);
            if (!mime) return 1;
            std::cout << "Current clipboard offers: " << mime->formats().join(", ").toStdString() << '\n';
        } else {
            QImage image(47, 29, QImage::Format_RGBA8888);
            image.fill(QColor(47, 103, 209, 123));
            QApplication::clipboard()->setImage(image);
        }
        const auto spec = document->canvas();
        const auto originalLayers = document->layers().size();
        const auto originalActive = session.activeLayer();
        const auto selection = document->selection();
        const auto zoom = canvas->zoom();
        const auto uploads = canvas->rendererStats().fullUploads;
        auto key = [&](Qt::Key k, Qt::KeyboardModifiers modifiers = Qt::ControlModifier) {
            if (systemClipboard) QTest::keyClick(window.windowHandle(), k, modifiers);
            else QTest::keyClick(canvas, k, modifiers);
        };
        key(Qt::Key_V);
        CHECK(waitFor([&] { return document->layers().size() == originalLayers + 1; }));
        if (document->layers().size() == originalLayers + 1) {
            const auto layer = document->layers().back();
            const auto surface = std::get<c::RasterLayer>(layer.payload).surface;
            CHECK(session.activeLayer() == layer.id);
            CHECK(layer.localToDocument == c::AffineTransform {});
            CHECK(document->canvas() == spec && canvas->zoom() == zoom);
            CHECK(document->selection() == selection);
            CHECK(session.history().undoDepth() == 1);
            CHECK(session.undo());
            CHECK(document->layers().size() == originalLayers && session.activeLayer() == originalActive);
            CHECK(session.redo());
            CHECK(document->layers().back().id == layer.id && session.activeLayer() == layer.id);
            CHECK(std::get<c::RasterLayer>(document->layers().back().payload).surface == surface);
            if (systemClipboard) {
                // Refresh through the real menu route after direct history checks.
                key(Qt::Key_Z); key(Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
                CHECK(waitFor([&] { return canvas->rendererStats().fullUploads > uploads; }));
                std::cout << "Pasted " << surface->extent().width << 'x' << surface->extent().height
                    << " RGBA image; one history entry; undo/redo and Vulkan upload verified.\n";
            }
        }
        if (!systemClipboard) {
            // Empty/web clipboard preserves redo. Never route a URL into Open.
            CHECK(session.undo());
            const auto depth = session.history().undoDepth();
            QApplication::clipboard()->setText("https://example.invalid/photo.png");
            paste->trigger();
            CHECK(document->layers().size() == originalLayers);
            CHECK(session.history().undoDepth() == depth && session.history().canRedo());
            const auto path = files.filePath(QStringLiteral("local image #1.png"));
            QImage image(71, 39, QImage::Format_RGBA8888); image.fill(Qt::green);
            CHECK(image.save(path));
            QApplication::clipboard()->setText('"' + path + '"');
            paste->trigger();
            CHECK(document->layers().size() == originalLayers + 1 && !session.history().canRedo());
            CHECK(document->layers().back().name == "local image #1");
            CHECK(document->canvas() == spec);
            const auto count = document->layers().size();
            // Numeric/editable-widget paste is normal text paste, even when
            // that text happens to name an existing image.
            QLineEdit field(&window); field.show(); field.setFocus();
            QTest::keyClick(&field, Qt::Key_V, Qt::ControlModifier);
            CHECK(field.text() == '"' + path + '"');
            CHECK(document->layers().size() == count);
            field.clearFocus(); field.hide();
            // Text editing owns paste and must not create a raster layer.
            text->press({50, 50}, {}, false);
            QApplication::clipboard()->setText("Pasted text");
            key(Qt::Key_V);
            CHECK(text->active());
            const auto* textLayer = document->layer(text->layerId());
            CHECK(textLayer && std::get<c::TextLayer>(textLayer->payload).utf8 == "Pasted text");
            const auto textCount = document->layers().size();
            QApplication::clipboard()->setImage(image);
            key(Qt::Key_V);
            CHECK(document->layers().size() == textCount);
            text->finish();
            // New/Resize cards never add a clipboard layer, even via a direct
            // QAction trigger (in addition to the modal keyboard barrier).
            for (const char* name : {"NewDocumentAction", "ChangeCanvasSizeAction"}) {
                auto* action = window.findChild<QAction*>(name); CHECK(action);
                bool visited = false;
                QTimer watchdog; watchdog.setSingleShot(true);
                QObject::connect(&watchdog, &QTimer::timeout, [&] {
                    CHECK(false);
                    for (auto* dialog : window.findChildren<QDialog*>()) dialog->reject();
                });
                watchdog.start(2000);
                QTimer::singleShot(0, &window, [&] {
                    for (auto* d : window.findChildren<QDialog*>()) {
                        auto* card = dynamic_cast<u::NewDocumentDialog*>(d);
                        if (!card || card->isHidden()) continue;
                        visited = true;
                        paste->trigger(); QTest::keyClick(card, Qt::Key_V, Qt::ControlModifier);
                        CHECK(document->layers().size() == textCount);
                        card->reject(); return;
                    }
                });
                if (action) action->trigger();
                watchdog.stop(); CHECK(visited);
            }
            QApplication::clipboard()->clear(); // offscreen clipboard only
        }
        window.close();
    }
    CHECK(validation == 0);
    std::cout << "Clipboard interaction: " << failures << " failures, " << validation << " Vulkan validation messages\n";
    return failures ? 1 : 0;
}
