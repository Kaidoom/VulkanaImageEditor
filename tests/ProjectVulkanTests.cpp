#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/ProjectFile.hpp"
#include "imageeditor/ui/TextController.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/core/RichText.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include <QApplication>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QVulkanInstance>
#include <iostream>

namespace c = imageeditor::core;
namespace u = imageeditor::ui;
int main(int argc, char** argv)
{
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication app(argc,argv);
    app.setQuitOnLastWindowClosed(false);
    QStandardPaths::setTestModeEnabled(true);
    app.setOrganizationName("ImageEditorTests"); app.setApplicationName("ProjectVulkan");
    QTemporaryDir files;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,files.path());
    u::applyEditorTheme(app);
    int failures = 0, warnings = 0, errors = 0;
    auto check = [&](bool ok,const char* what) { if (!ok) { ++failures; std::cerr << what << '\n'; } };
    QVulkanInstance instance;
    instance.setApiVersion(QVersionNumber(1,2));
    instance.setLayers({"VK_LAYER_KHRONOS_validation"});
    instance.installDebugOutputFilter([&](QVulkanInstance::DebugMessageSeverityFlags flags,
        QVulkanInstance::DebugMessageTypeFlags types,const void* message) {
        if (!types.testFlag(QVulkanInstance::ValidationMessage)) return false;
        if (flags.testFlag(QVulkanInstance::ErrorSeverity)) ++errors;
        else if (flags.testFlag(QVulkanInstance::WarningSeverity)) ++warnings;
        else return false;
        const auto* data = static_cast<const VkDebugUtilsMessengerCallbackDataEXT*>(message);
        std::cerr << (data ? data->pMessage : "Validation message") << '\n'; return false;
    });
    if (!instance.create()) return 1;
    auto wait = [&](auto predicate) {
        QElapsedTimer timeout; timeout.start();
        while (!predicate() && timeout.elapsed() < 5000) QTest::qWait(10);
        return predicate();
    };
    {
        c::Document source({{320,240},144});
        auto raster = c::Layer::raster("Off-canvas pixels",std::make_shared<c::ContiguousRasterSurface>(
            c::Extent2u{384,256},c::Rgba8{190,35,90,180}));
        raster.localToDocument = {-0.8,0.1,310,0.1,0.8,-8};
        source.insertLayer(0,std::move(raster));
        auto hidden = c::Layer::raster("Hidden",std::make_shared<c::ContiguousRasterSurface>(
            c::Extent2u{20,20},c::Rgba8{90,77,33,0}));
        hidden.visible = false; source.insertLayer(1,std::move(hidden));
        c::TextLayer text; text.utf8 = "Editable project\nVulkana";
        text.defaultStyle.sizePixels = 22;
        auto title = c::Layer::text("Rich text",text);
        title.localToDocument = {0.97,-0.1,40,0.1,0.97,80}; const auto id = title.id;
        source.insertLayer(2,std::move(title));
        const auto path = files.filePath("render.vulkana");
        check(bool(u::saveProject(path,source)),"Initial project save");
        u::MainWindow window(&instance,false);
        window.setUnsavedPromptEnabled(false);
        u::MainWindow::FileInteractions hooks;
        hooks.reportError = [&](const QString& e) { check(false,qPrintable(e)); };
        window.setFileInteractions(std::move(hooks));
        window.resize(1280,800); window.show();
        check(window.openImageFromPath(path),"Load into native canvas");
        imageeditor::render::CanvasWindow* canvas = nullptr;
        u::TextController* editor = nullptr;
        for (auto* w : QGuiApplication::allWindows())
            if (auto* c = dynamic_cast<imageeditor::render::CanvasWindow*>(w)) canvas = c;
        for (auto* child : window.children())
            if (auto* t = dynamic_cast<u::TextController*>(child)) editor = t;
        check(canvas && editor,"Canvas/controller available");
        if (canvas && editor) {
            check(wait([&]{return canvas->rendererStats().fullUploads >= 2
                && canvas->rendererStats().framesSubmitted > 0;}),"Loaded raster and text caches uploaded");
            // First exposure may precede Wayland's final configure and the
            // resolution-aware text cache. Establish a settled baseline.
            auto observed = canvas->rendererStats().uploadedBytes;
            QElapsedTimer stable; stable.start();
            check(wait([&] {
                const auto now = canvas->rendererStats().uploadedBytes;
                if (now != observed) { observed = now; stable.restart(); }
                return stable.elapsed() >= 180;
            }),"Initial viewport/cache settled");
            const auto before = canvas->rendererStats();
            const auto cache = window.editorSession().document()->layer(id)->renderCache;
            check(window.saveDocument(),"Save loaded document without texture changes");
            QTest::qWait(80);
            check(window.editorSession().document()->layer(id)->renderCache == cache,"Save reuses text cache");
            check(canvas->rendererStats().uploadedBytes == before.uploadedBytes,"Save must not upload/rebuild textures");
            check(editor->editLayer(id),"Loaded text remains editable");
            QKeyEvent end(QEvent::KeyPress,Qt::Key_End,Qt::ControlModifier);
            editor->keyEvent(&end,false);
            QKeyEvent insert(QEvent::KeyPress,Qt::Key_Exclam,Qt::NoModifier,"!");
            editor->keyEvent(&insert,false);
            const auto depth = window.editorSession().history().undoDepth();
            check(depth > 0,"Typing history admitted");
            check(window.saveDocument(),"Save settles active text edit");
            check(window.editorSession().history().undoDepth() == depth,"Saving preserves text history");
            auto loaded = u::loadProject(path);
            check(bool(loaded),"Reload edited text project");
            if (loaded) check(std::get<c::TextLayer>(loaded.document->layer(id)->payload).utf8.ends_with('!'),"Committed text saved");
            // Typing queued a source upload and a 100 ms density-cache update.
            // Finish that work before measuring an unrelated same-tab focus.
            observed = canvas->rendererStats().uploadedBytes;
            stable.restart();
            check(wait([&] {
                const auto now = canvas->rendererStats().uploadedBytes;
                if (now != observed) { observed = now; stable.restart(); }
                return stable.elapsed() >= 180;
            }),"Edited text upload/cache settled before focus measurement");
            const auto editedCache = window.editorSession().document()->layer(id)->renderCache;
            const auto uploads = canvas->rendererStats().fullUploads;
            check(window.openImageFromPath(path),"Focus existing project without loading new surfaces");
            QTest::qWait(80);
            check(window.editorSession().document()->layer(id)->renderCache == editedCache,"Focusing keeps the edited text cache");
            check(canvas->rendererStats().fullUploads == uploads,"Focusing existing project reuses GPU surfaces");
            check(window.editorSession().history().undoDepth() == depth,"Focusing existing project retains history");
            check(!window.editorSession().document()->isModified(),"Loaded project is clean");
            window.logRendererDiagnostics();
        }
        window.close();
    }
    instance.destroy();
    check(warnings == 0 && errors == 0,"Vulkan validation must be clean");
    std::cout << "Project Vulkan: " << failures << " failures; " << warnings << " warnings; " << errors << " errors\n";
    return failures ? 1 : 0;
}
