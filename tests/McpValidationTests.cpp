#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/McpController.hpp"
#include "imageeditor/core/LayerCommands.hpp"
#include "../src/ui/src/McpToolHelpers.hpp"
#include "NativeGraphicsStartup.hpp"
#include <QApplication>
#include <QFile>
#include <QJsonDocument>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <iostream>

namespace imageeditor::ui {
// Exercise the real dispatch boundary with deliberate failures. No test tools
// are registered by production builds and no client-facing hooks are added.
class McpControllerTestAccess {
public:
    static void add(McpController& controller, QString name,
        std::function<QJsonObject(const QJsonObject&)> handler)
    {
        controller.add(std::move(name), "Validation contract test", {}, {},
            McpController::Access::Document, std::move(handler));
    }
    static bool execute(McpController& controller, std::unique_ptr<core::Command> command)
    { return controller.execute(std::move(command)); }
};
}
namespace c = imageeditor::core;
namespace u = imageeditor::ui;
namespace d = imageeditor::ui::mcpdetail;
namespace {
int failures = 0;
#define CHECK(v) do { if (!(v)) { ++failures; std::cerr << "FAIL " << __LINE__ << ": " << #v << '\n'; } } while(false)
struct Fixture {
    u::MainWindow window{nullptr,false,false};
    u::McpController controller{window};
    Fixture() {
        window.setUnsavedPromptEnabled(false); window.show();
        CHECK(imageeditor::test::waitForGraphicsStartup(window));
        const auto reply = controller.registry().call("vulkana_document_create", {{"width",32},{"height",24}});
        CHECK(!reply["isError"].toBool());
    }
    ~Fixture() { window.close(); QCoreApplication::processEvents(); }
    c::EditorSession& session() { return const_cast<c::EditorSession&>(window.editorSession()); }
    c::Document& doc() { return *session().document(); }
    QJsonObject call(const QString& tool, QJsonObject args = {}) {
        args["documentId"] = QString::number(window.activeDocumentId());
        return controller.registry().call(tool,args);
    }
    QJsonObject error(const QString& tool, QJsonObject args = {}) {
        const auto reply = call(tool,args); CHECK(reply["isError"].toBool());
        const auto data = reply["structuredContent"].toObject();
        CHECK(QJsonDocument::fromJson(reply["content"].toArray()[0].toObject()["text"].toString().toUtf8()).object() == data);
        return data["error"].toObject();
    }
};
void guaranteed(const QJsonObject& error, const QString& code)
{
    CHECK(error["code"] == code);
    const auto details = error["details"].toObject();
    CHECK(details["changed"] == false && details["noChanges"] == true);
    CHECK(details["recovery"].toString().contains("No changes were made"));
    CHECK(!details["recovery"].toString().contains("may have committed"));
}
void uncertain(const QJsonObject& error, bool changed)
{
    CHECK(error["code"] == "operation_failed");
    const auto details = error["details"].toObject();
    CHECK(details["changed"] == changed);
    CHECK(!details["noChanges"].toBool());
    CHECK(details["recovery"].toString().contains("may have committed"));
}
void schemaAndSemanticPreflight()
{
    Fixture f;
    const auto before = f.controller.state(false);
    guaranteed(f.error("vulkana_layer_create", {{"newKind","raster"}}), "invalid_arguments");
    guaranteed(f.error("vulkana_layer_create", {{"kind","raster"},{"shape",QJsonObject{{"kind","ellipse"}}}}), "invalid_arguments");
    guaranteed(f.error("vulkana_layers_create", {{"layers",QJsonArray{
        QJsonObject{{"kind","raster"},{"width",2},{"height",2}},
        QJsonObject{{"kind","text"},{"text",QJsonObject{{"fontFamily","Not-An-Installed-Validation-Test-Font"}}}}}}}), "invalid_arguments");
    const auto target = QString::number(*f.session().activeLayer());
    guaranteed(f.error("vulkana_layers_style", {{"layerIds",QJsonArray{target,"999999999"}},{"opacity",.2}}), "invalid_arguments");
    guaranteed(f.error("vulkana_layers_style", {{"layerIds",QJsonArray{target}},{"effect",QJsonObject{
        {"type","stroke"},{"parameters",QJsonObject{{"distance",20}}}}}}), "invalid_arguments");
    for (const auto* tool : {"vulkana_text_set", "vulkana_shape_set"})
        guaranteed(f.error(tool, {{"layerId","18446744073709551616"},{"properties",QJsonObject{}}}), "invalid_arguments");
    CHECK(f.controller.state(false) == before);
}
void phaseAndMutationFailures()
{
    Fixture f;
    const auto target = *f.session().activeLayer();
    const auto before = f.controller.state(false);
    u::McpControllerTestAccess::add(f.controller,"test.pure.validation", [](const auto&) {
        return d::validateBeforeMutation([]() -> QJsonObject {
            d::require(false,"arguments.items[1].name: name is required"); return {};
        });
    });
    guaranteed(f.error("test.pure.validation"),"invalid_arguments");
    u::McpControllerTestAccess::add(f.controller,"test.pure.preparation", [](const auto&) {
        return d::validateBeforeMutation([]() -> QJsonObject { throw std::bad_alloc(); });
    });
    guaranteed(f.error("test.pure.preparation"),"operation_failed");
    u::McpControllerTestAccess::add(f.controller,"test.preflight.cannot.execute", [&](const auto&) {
        return d::validateBeforeMutation([&]() -> QJsonObject {
            (void)u::McpControllerTestAccess::execute(f.controller,std::make_unique<c::SetLayerOpacityCommand>(target,.25F)); return {};
        });
    });
    guaranteed(f.error("test.preflight.cannot.execute"),"operation_failed");
    CHECK(f.controller.state(false) == before);
    u::McpControllerTestAccess::add(f.controller,"test.postcommit.failure", [&](const auto&) -> QJsonObject {
        CHECK(u::McpControllerTestAccess::execute(f.controller,std::make_unique<c::SetLayerOpacityCommand>(target,.5F)));
        throw std::invalid_argument("Presentation failed after the edit");
    });
    uncertain(f.error("test.postcommit.failure"),true);
    CHECK(f.doc().layer(target)->opacity == .5F && f.session().history().undoDepth() == 1);
    CHECK(f.session().undo());
    // A later preflight wrapper cannot relabel a call that already attempted
    // publication, even when the native command itself was a no-op.
    u::McpControllerTestAccess::add(f.controller,"test.after.publication", [&](const auto&) {
        CHECK(!u::McpControllerTestAccess::execute(f.controller,std::make_unique<c::SetLayerOpacityCommand>(target,1.F)));
        return d::validateBeforeMutation([]() -> QJsonObject { throw std::invalid_argument("late validation"); });
    });
    uncertain(f.error("test.after.publication"),false);
    // Session colors do not change the revision. Defensive observed-state
    // checks reject a misplaced typed guarantee after such a change.
    const auto revision = f.doc().revision();
    u::McpControllerTestAccess::add(f.controller,"test.session.failure", [&](const auto&) {
        f.session().setForegroundColor({31,67,101,255});
        return d::validateBeforeMutation([]() -> QJsonObject { throw std::invalid_argument("late validation"); });
    });
    uncertain(f.error("test.session.failure"),true);
    CHECK(f.doc().revision() == revision);
    // Unclassified work can write files without changing any editor token.
    // It must never acquire a no-change guarantee merely from changed:false.
    QTemporaryDir output;
    u::McpControllerTestAccess::add(f.controller,"test.file.failure", [&](const auto&) -> QJsonObject {
        QFile file(output.filePath("written.txt")); CHECK(file.open(QIODevice::WriteOnly));
        CHECK(file.write("written") == 7); file.close();
        throw std::invalid_argument("A later export step failed");
    });
    uncertain(f.error("test.file.failure"),false);
    CHECK(QFile::exists(output.filePath("written.txt")));
}
void staleRevisionGuarantee()
{
    Fixture f;
    const auto stale = f.controller.state(false)["document"].toObject()["revision"].toString();
    const auto target = QString::number(*f.session().activeLayer());
    CHECK(!f.call("vulkana_layer_set_opacity", {{"layerId",target},{"opacity",.5}})["isError"].toBool());
    CHECK(f.session().undo());
    const auto before = f.controller.state(false);
    CHECK(f.session().history().redoDepth() == 1);
    const auto error = f.error("vulkana_layer_create", {{"kind","raster"},{"expectedRevision",stale}});
    guaranteed(error,"stale_revision");
    CHECK(error["details"].toObject()["document"] == before["document"]);
    CHECK(f.controller.state(false) == before);
    CHECK(!f.call("vulkana_layer_create", {{"kind","raster"},
        {"expectedRevision",before["document"].toObject()["revision"]}})["isError"].toBool());
}
}
int main(int argc,char** argv)
{
    QApplication app(argc,argv); app.setOrganizationName("ImageEditorTests"); app.setApplicationName("McpValidation");
    QStandardPaths::setTestModeEnabled(true); QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat); QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings.path());
    schemaAndSemanticPreflight(); phaseAndMutationFailures(); staleRevisionGuarantee();
    std::cout << "MCP validation: " << failures << " failures\n"; return failures?1:0;
}
