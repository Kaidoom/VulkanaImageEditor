#include "imageeditor/ui/McpController.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/ui/WorkspaceDialog.hpp"
#include "imageeditor/ui/NewDocumentDialog.hpp"
#include "imageeditor/ui/OverlayDockWorkspace.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "NativeGraphicsStartup.hpp"
#include <QApplication>
#include <QAction>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QLineEdit>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QTest>
#include <QVulkanInstance>
#include <iostream>

namespace ui = imageeditor::ui;
int failures = 0;
#define CHECK(value) do { if (!(value)) { std::cerr << "FAIL " << __LINE__ << ": " << #value << '\n'; ++failures; } } while(false)
void documentCreationDialogTests(QVulkanInstance* instance)
{
    ui::MainWindow window(instance,false,false); window.setUnsavedPromptEnabled(false); window.show();
    CHECK(imageeditor::test::waitForGraphicsStartup(window));
    ui::McpController controller(window);
    auto call = [&](const char* tool,QJsonObject args={},bool error=false) {
        const auto result=controller.registry().call(tool,args);
        if(result["isError"].toBool()!=error)std::cerr<<QJsonDocument(result).toJson().constData();
        CHECK(result["isError"].toBool()==error);return result["structuredContent"].toObject();
    };
    auto withDialog = [&](auto open,auto inspect) {
        bool handled=false;
        QTimer responder,deadline; deadline.setSingleShot(true);
        QObject::connect(&responder,&QTimer::timeout,&window,[&] {
            for(auto* dialog:window.findChildren<QDialog*>()) if(dialog->isVisible()) {
                responder.stop();handled=true;inspect(dialog);
                if(dialog->isVisible())dialog->reject();
                return;
            }
        });
        QObject::connect(&deadline,&QTimer::timeout,&window,[&] {window.close();});
        responder.start(5);deadline.start(5000);open();deadline.stop();responder.stop();CHECK(handled);
        QApplication::processEvents();
    };
    const QJsonObject requested{{"width",64},{"height",48},{"dpi",144},{"background","#123456"},{"name","Created through chooser"}};
    const auto placeholder=window.activeDocumentId();
    withDialog([&] {window.showStartupDocument();},[&](QDialog* dialog) {
        CHECK(dynamic_cast<ui::NewDocumentDialog*>(dialog));
        const auto blocked=call("vulkana_get_state");
        CHECK(!blocked["canEdit"].toBool() && blocked["canCreateDocument"].toBool());
        CHECK(blocked["blockers"].toArray().first().toObject()["resolution"].toString().contains("vulkana_document_create"));
        auto* spin=dialog->findChild<QDoubleSpinBox*>();CHECK(spin);
        if(spin) {
            auto* draft=spin->findChild<QLineEdit*>();CHECK(draft);
            if(draft){draft->setText("999");draft->setModified(true);draft->setFocus();}
        }
        CHECK(call("vulkana_get_state")["canCreateDocument"].toBool());
        const auto before=call("vulkana_get_state");
        for(const int width:{0,16384}) {
            const auto error=call("vulkana_document_create",{{"width",width},{"height",16384}},true)["error"].toObject();
            CHECK(error["code"]=="invalid_arguments" && error["details"].toObject()["noChanges"]==true);
            CHECK(dialog->isVisible() && call("vulkana_get_state")==before);
        }
        // A native confirmation over the chooser is never implicitly dismissed.
        QDialog native(dialog);native.setModal(true);native.show();QApplication::processEvents();
        CHECK(!call("vulkana_get_state")["canCreateDocument"].toBool());
        CHECK(call("vulkana_document_create",requested,true)["error"].toObject()["code"]=="blocked");
        CHECK(dialog->isVisible() && native.isVisible());native.reject();QApplication::processEvents();
        const auto created=call("vulkana_document_create",requested);
        CHECK(!dialog->isVisible() && created["canEdit"].toBool());
        CHECK(created["document"].toObject()["width"]==64 && created["document"].toObject()["height"]==48);
        CHECK(created["document"].toObject()["dpi"]==144);
    });
    CHECK(window.documentCount()==1 && window.activeDocumentId()!=placeholder);
    CHECK(window.editorSession().document()->canvas().extent==(imageeditor::core::Extent2u{64,48}));
    const auto original=window.activeDocumentId();
    call("vulkana_layer_create",{{"documentId",QString::number(original)},{"kind","shape"}});
    const auto existing=call("vulkana_get_state")["document"].toObject();
    withDialog([&] {window.findChild<QAction*>("NewDocumentAction")->trigger();},[&](QDialog* dialog) {
        CHECK(call("vulkana_get_state")["canCreateDocument"].toBool());
        call("vulkana_document_create",requested);CHECK(!dialog->isVisible());
    });
    CHECK(window.documentCount()==2 && window.activeDocumentId()!=original);
    call("vulkana_document_activate",{{"documentId",QString::number(original)}});
    CHECK(call("vulkana_get_state")["document"].toObject()==existing);
    for(const auto* action: {"ChangeCanvasSizeAction","PreferencesAction"}) {
        withDialog([&] {window.findChild<QAction*>(action)->trigger();},[&](QDialog* dialog) {
            CHECK(!call("vulkana_get_state")["canCreateDocument"].toBool());
            CHECK(call("vulkana_document_create",requested,true)["error"].toObject()["code"]=="blocked");
            CHECK(dialog->isVisible() && window.documentCount()==2);
        });
    }
    CHECK(call("vulkana_get_state")["document"].toObject()==existing);
    window.close();QApplication::processEvents();
}
int main(int argc, char** argv)
{
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication app(argc, argv);
    app.setApplicationName("McpEditorTests"); app.setOrganizationName("ImageEditorTests"); app.setApplicationVersion("test");
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir temporary;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
    ui::applyEditorTheme(app);
    QVulkanInstance instance;
    const bool native = app.arguments().contains("--native");
    if (native) { instance.setApiVersion(QVersionNumber(1, 2)); CHECK(instance.create()); if (!instance.isValid()) return 2; }
    ui::MainWindow window(native ? &instance : nullptr, false, false);
    window.setUnsavedPromptEnabled(false); window.show();
    CHECK(imageeditor::test::waitForGraphicsStartup(window));
    ui::McpController controller(window);
    if (app.arguments().contains("--serve")) {
        const auto index = app.arguments().indexOf("--serve");
        if (index + 1 >= app.arguments().size()) return 2;
        QString error;
        if (!controller.start(app.arguments()[index + 1], &error)) { std::cerr << error.toStdString() << '\n'; return 2; }
        std::cerr << "MCP test editor ready\n";
        QTimer::singleShot(900000, &app, &QApplication::quit);
        return app.exec();
    }
    auto& registry = controller.registry();
    CHECK(registry.list().size() >= 40);
    auto call = [&](const char* name, QJsonObject args = {}, bool success = true) {
        const auto reply = registry.call(QLatin1String(name), args);
        if (reply["isError"].toBool() == success) std::cerr << QJsonDocument(reply).toJson().constData();
        CHECK(reply["isError"].toBool() != success);
        return reply;
    };
    auto data = [](const QJsonObject& reply) { return reply["structuredContent"].toObject(); };
    const auto created = data(call("vulkana_document_create", {{"width", 96}, {"height", 64}, {"background", "#ffffff"}, {"name", "MCP test"}}));
    const auto doc = created["document"].toObject();
    const auto id = doc["documentId"].toString();
    CHECK(doc["width"].toInt() == 96 && doc["height"].toInt() == 64);
    QJsonObject args{{"documentId", id}, {"expectedRevision", doc["revision"]}};
    args["width"] = 80; args["height"] = 50;
    call("vulkana_canvas_resize", args);
    const auto revision = window.editorSession().document()->revision();
    call("vulkana_canvas_resize", args, false);
    CHECK(window.editorSession().document()->revision() == revision);
    call("vulkana_canvas_resize", {{"documentId", "999999999"}, {"width", 30}, {"height", 30}}, false);
    call("vulkana_canvas_resize", {{"documentId", id}, {"width", 1.5}, {"height", 30}}, false);
    call("vulkana_canvas_resize", {{"documentId", id}, {"width", 30}, {"height", 30}, {"typo", 1}}, false);
    call("vulkana_history", {{"documentId", id}, {"operation", "undo"}});
    CHECK(window.editorSession().document()->canvas().extent.width == 96);
    const auto preview = call("vulkana_canvas_preview", {{"documentId", id}, {"maxSize", 96}, {"quality", "exact"}, {"background", "transparent"}});
    const auto content = preview["content"].toArray();
    CHECK(content.size() == 2 && content[1].toObject()["type"] == "image");
    const auto image = QImage::fromData(QByteArray::fromBase64(content[1].toObject()["data"].toString().toLatin1()), "PNG");
    CHECK(image.size() == QSize(96, 64) && image.pixelColor(40, 30) == Qt::white);
    // Both qualities honor the same advertised range, including the omitted
    // maxSize/quality defaults. Decode the actual MCP PNG, not just metadata.
    const auto beforePreview = data(call("vulkana_get_state"))["document"].toObject();
    const auto checkPreview = [&](QJsonObject options, QSize expected) {
        if (!options.contains("documentId")) options["documentId"] = id;
        const auto reply = call("vulkana_canvas_preview", options);
        const auto blocks = reply["content"].toArray();
        CHECK(blocks.size() == 2);
        if (blocks.size() != 2) return;
        const auto png = QImage::fromData(QByteArray::fromBase64(blocks[1].toObject()["data"].toString().toLatin1()), "PNG");
        CHECK(png.size() == expected);
        CHECK(data(reply)["width"] == expected.width() && data(reply)["height"] == expected.height());
        CHECK(png.pixelColor(png.width()/2, png.height()/2) == Qt::white);
    };
    checkPreview({}, QSize(1024, 682));
    for (const auto quality : {"overview", "exact"}) for (int size : {32, 512, 1024, 2048})
        checkPreview({{"maxSize", size}, {"quality", quality}, {"background", "transparent"}},
            QSize(96,64).scaled(size,size,Qt::KeepAspectRatio));
    for (int size : {31, 2049}) {
        const auto error = data(call("vulkana_canvas_preview", {{"documentId", id}, {"maxSize", size}}, false))["error"].toObject();
        CHECK(error["code"] == "invalid_arguments");
        CHECK(error["message"].toString().contains(QString::number(size == 31 ? 32 : 2048)));
    }
    CHECK(data(call("vulkana_get_state"))["document"].toObject() == beforePreview);
    if (native) {
        imageeditor::render::CanvasWindow* canvas = nullptr;
        for (auto* item : QGuiApplication::allWindows())
            if (auto* value = dynamic_cast<imageeditor::render::CanvasWindow*>(item)) canvas = value;
        CHECK(canvas);
        if (canvas) {
            const auto frame = canvas->rendererStats().framesSubmitted;
            canvas->scheduleFrame();
            CHECK(QTest::qWaitFor([&] { return canvas->rendererStats().framesSubmitted > frame; }, 10000));
        }
    }
    QDialog modal(&window); modal.setWindowTitle("Test blocking dialog"); modal.setModal(true); modal.show();
    QApplication::processEvents();
    CHECK(!data(call("vulkana_get_state"))["canEdit"].toBool());
    call("vulkana_canvas_resize", {{"documentId", id}, {"width", 30}, {"height", 30}}, false);
    modal.reject(); QApplication::processEvents();
    if (!native) {
        QLineEdit pending(&window); pending.show();
        QT_WARNING_PUSH
        QT_WARNING_DISABLE_DEPRECATED
        QApplication::setActiveWindow(&window);
        QT_WARNING_POP
        pending.setFocus();
        CHECK(pending.hasFocus());
        pending.setText("unfinished"); pending.setModified(true);
        CHECK(!data(call("vulkana_get_state"))["canEdit"].toBool());
        call("vulkana_canvas_resize", {{"documentId", id}, {"width", 30}, {"height", 30}}, false);
        pending.clearFocus(); pending.hide(); QApplication::processEvents();
    }
    const auto path = temporary.filePath("saved.vulkana");
    call("vulkana_document_save", {{"documentId", id}, {"path", path}});
    CHECK(QFileInfo::exists(path) && !window.editorSession().document()->isModified());
    QFile saved(path); CHECK(saved.open(QIODevice::ReadOnly)); const auto originalBytes = saved.readAll(); saved.close();
    call("vulkana_document_save", {{"documentId", id}, {"path", path}}, false);
    CHECK(saved.open(QIODevice::ReadOnly)); CHECK(saved.readAll() == originalBytes); saved.close();
    const auto exportPath = temporary.filePath("export.png");
    call("vulkana_export_image", {{"documentId", id}, {"path", exportPath}, {"format", "png"}});
    CHECK(QImage(exportPath).pixelColor(40, 30) == Qt::white);
    call("vulkana_export_image", {{"documentId", id}, {"path", exportPath}, {"format", "png"}}, false);
    call("vulkana_document_open", {{"path", temporary.filePath("missing.psd")}}, false);
    const auto reopened = data(call("vulkana_document_open", {{"path", path}}))["document"].toObject();
    CHECK(reopened["documentId"] != id && reopened["width"] == 96);
    const auto other = reopened["documentId"].toString();
    call("vulkana_document_activate", {{"documentId", id}});
    call("vulkana_canvas_resize", {{"documentId", id}, {"width", 80}, {"height", 50}});
    call("vulkana_document_close", {{"documentId", id}}, false);
    call("vulkana_document_close", {{"documentId", id}, {"discardChanges", true}});
    CHECK(window.activeDocumentId() == other.toULongLong());
    call("vulkana_document_close", {{"documentId", other}});
    CHECK(window.documentCount() == 0);
    CHECK(!data(call("vulkana_get_state"))["canEdit"].toBool());
    const auto portrait = data(call("vulkana_document_create", {{"width", 64}, {"height", 96}, {"background", "#ffffff"}}))["document"].toObject().value("documentId");
    for (const auto quality : {"overview", "exact"})
        checkPreview({{"documentId", portrait}, {"maxSize", 2048}, {"quality", quality}}, QSize(64,96).scaled(2048,2048,Qt::KeepAspectRatio));
    window.close(); QApplication::processEvents();
    documentCreationDialogTests(native ? &instance : nullptr);
    std::cout << "MCP editor tests: " << failures << " failures\n";
    return failures ? 1 : 0;
}
