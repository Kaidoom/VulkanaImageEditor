#include "imageeditor/ui/McpController.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/core/ColorSampler.hpp"
#include "imageeditor/core/LayerMask.hpp"
#include "NativeGraphicsStartup.hpp"
#include <QApplication>
#include <QImage>
#include <QJsonDocument>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <iostream>

namespace c = imageeditor::core;
namespace u = imageeditor::ui;
namespace {
int failures = 0;
#define CHECK(value) do { if (!(value)) { std::cerr << "FAIL " << __LINE__ << ": " #value "\n"; ++failures; } } while (false)
QJsonObject point(double x, double y, double pressure = 1)
{ return {{"x", x}, {"y", y}, {"pressure", pressure}}; }
QJsonObject coordinate(double x, double y) { return {{"x", x}, {"y", y}}; }

struct Fixture {
    QTemporaryDir files;
    u::MainWindow window {nullptr, false, false};
    std::unique_ptr<u::McpController> controller;
    c::LayerId target {};
    Fixture()
    {
        window.setUnsavedPromptEnabled(false);
        window.show();
        CHECK(imageeditor::test::waitForGraphicsStartup(window));
        QImage image(96, 96, QImage::Format_RGBA8888);
        image.fill(Qt::transparent);
        const auto path = files.filePath("transparent.png");
        CHECK(image.save(path));
        CHECK(window.openImageFromPath(path));
        target = session().activeLayer().value_or(0);
        controller = std::make_unique<u::McpController>(window);
        CHECK(controller->state().value("blockers").toArray().isEmpty());
    }
    ~Fixture() { controller.reset(); window.close(); }
    c::EditorSession& session() { return const_cast<c::EditorSession&>(window.editorSession()); }
    c::Document& document() { return *session().document(); }
    QJsonObject call(const char* name, QJsonObject args, bool success = true, bool scoped = true)
    {
        if (scoped) {
            args.insert("documentId", QString::number(window.activeDocumentId()));
            args.insert("expectedRevision", controller->state(false)["document"].toObject()["revision"]);
        }
        auto reply = controller->registry().call(QLatin1String(name), args);
        if (reply.value("isError").toBool() == success) {
            std::cerr << name << ": " << QJsonDocument(reply).toJson(QJsonDocument::Compact).constData() << '\n';
            ++failures;
        }
        return reply.value("structuredContent").toObject();
    }
    c::Rgba8 sample(double x, double y)
    { return c::PreparedRasterSampler(*document().layer(target)).sample({x, y}); }
    QJsonObject targetArgs() { return {{"layerId", QString::number(target)}}; }
    QJsonObject stroke(QJsonArray path, QString color = "#ff0000", double size = 12)
    {
        auto args = targetArgs(); args["points"] = path; args["color"] = color; args["size"] = size;
        return args;
    }
};

void brushIsUndoableAndValidatesFirst()
{
    Fixture f;
    const auto tool = f.session().activeTool();
    const auto selected = f.session().layerSelectionState();
    const auto colors = f.session().colors();
    const auto depth = f.session().history().undoDepth();
    const auto beforeToken = f.controller->state(false)["document"].toObject()["revision"].toString();
    auto data = f.call("vulkana_brush_stroke", f.stroke({point(12, 20), point(70, 20)}));
    CHECK(data.value("changed").toBool());
    CHECK(data["revision"].toString() != beforeToken);
    auto stale = f.stroke({point(10, 50)});
    stale["documentId"] = QString::number(f.window.activeDocumentId()); stale["expectedRevision"] = beforeToken;
    CHECK(f.controller->registry().call("vulkana_brush_stroke", stale)["isError"].toBool());
    CHECK(f.sample(10, 50).alpha == 0);
    CHECK(f.sample(40, 20).red == 255 && f.sample(40, 20).alpha == 255);
    CHECK(f.sample(40, 40).alpha == 0);
    CHECK(f.session().history().undoDepth() == depth + 1);
    CHECK(f.session().activeTool() == tool && f.session().layerSelectionState() == selected);
    CHECK(f.session().colors() == colors);
    CHECK(f.session().undo()); CHECK(f.sample(40, 20).alpha == 0);
    CHECK(f.session().redo()); CHECK(f.sample(40, 20).alpha == 255);
    const auto revision = f.document().revision();
    const auto afterDepth = f.session().history().undoDepth();
    f.call("vulkana_brush_stroke", f.stroke({point(10, 50), QJsonObject{{"x", "bad"}, {"y", 50}}}), false);
    f.call("vulkana_brush_stroke", f.stroke({point(10, 50)}, "not-a-color"), false);
    f.call("vulkana_brush_stroke", f.stroke({point(-1000000, 0), point(1000000, 0)}), false);
    CHECK(f.document().revision() == revision && f.session().history().undoDepth() == afterDepth);
    CHECK(f.sample(10, 50).alpha == 0);
    // Exceed the remote work budget after crossing real editable pixels: the
    // same core stroke journal must roll back, not leave an unfinished mark.
    auto tooLong = f.stroke({point(.5, 50.5), point(20000, 50.5)}, "#00ff00", .25);
    tooLong["spacing"] = 1;
    f.call("vulkana_brush_stroke", tooLong, false);
    CHECK(f.session().history().undoDepth() == afterDepth);
    CHECK(f.sample(10.5, 50.5).alpha == 0);
    CHECK(f.sample(40, 20).red == 255 && f.sample(40, 20).alpha == 255);
    auto erase = f.stroke({point(40, 20)}, "#ffffff", 8); erase["mode"] = "erase";
    f.call("vulkana_brush_stroke", erase);
    CHECK(f.sample(40, 20).alpha == 0);
    CHECK(f.session().undo()); CHECK(f.sample(40, 20).alpha == 255);
}

void transformedBrushHonorsSelectionAndMask()
{
    Fixture f;
    CHECK(f.document().setLayerTransform(f.target, {2, 0, 16, 0, 2, 10}));
    const auto beforeSelection = f.controller->state(false)["document"].toObject()["revision"].toString();
    f.call("vulkana_selection", {{"shape", "rectangle"}, {"from", coordinate(30, 25)}, {"to", coordinate(50, 45)}});
    CHECK(f.controller->state(false)["document"].toObject()["revision"].toString() != beforeSelection);
    const auto selection = f.document().selection();
    f.call("vulkana_brush_stroke", f.stroke({point(38, 35)}, "#00ff00", 40));
    CHECK(f.sample(38, 35).green == 255 && f.sample(38, 35).alpha == 255);
    CHECK(f.sample(25, 35).alpha == 0 && f.sample(55, 35).alpha == 0);
    CHECK(f.document().selection() == selection);
    auto mask = f.targetArgs(); mask["operation"] = "reveal_all";
    f.call("vulkana_layer_mask", mask);
    const auto original = f.sample(38, 35);
    const auto before = f.document().layer(f.target)->mask;
    const auto depth = f.session().history().undoDepth();
    auto paint = f.stroke({point(38, 35)}, "#000000", 12); paint["target"] = "mask";
    f.call("vulkana_brush_stroke", paint);
    const auto* layer = f.document().layer(f.target);
    CHECK(c::layerMaskCoverage(layer->mask, layer->localToDocument.inverted()->map({38, 35})) < .01f);
    CHECK(f.sample(38, 35) == original && f.document().selection() == selection);
    CHECK(f.session().history().undoDepth() == depth + 1);
    CHECK(f.session().undo()); CHECK(f.document().layer(f.target)->mask == before);
}

void fillAndSelections()
{
    Fixture f;
    f.call("vulkana_selection", {{"shape", "rectangle"}, {"from", coordinate(10, 10)}, {"to", coordinate(40, 40)}});
    auto fill = f.targetArgs(); fill["color"] = "#336699";
    const auto depth = f.session().history().undoDepth();
    f.call("vulkana_fill", fill);
    CHECK(f.sample(20, 20) == (c::Rgba8{51, 102, 153, 255}));
    CHECK(f.sample(50, 20).alpha == 0 && f.session().history().undoDepth() == depth + 1);
    f.call("vulkana_selection", {{"shape", "none"}});
    CHECK(!f.document().selection());
    auto mask = f.targetArgs(); mask["operation"] = "hide_all";
    f.call("vulkana_layer_mask", mask);
    // Alpha selection ignores a fully hidden mask and layer opacity.
    CHECK(f.document().setLayerOpacity(f.target, .1F));
    f.call("vulkana_selection_from_alpha", f.targetArgs());
    CHECK(f.document().selection()->coverageAtDocumentPixel(20, 20) == 255);
    CHECK(f.document().selection()->coverageAtDocumentPixel(50, 20) == 0);
    f.call("vulkana_selection", {{"shape", "ellipse"}, {"from", coordinate(20, 20)}, {"to", coordinate(60, 60)}});
    CHECK(f.document().selection()->coverageAtDocumentPixel(40, 40) == 255);
    CHECK(f.document().selection()->coverageAtDocumentPixel(21, 21) == 0);
    f.call("vulkana_selection", {{"shape", "polygon"}, {"points", QJsonArray{coordinate(10, 10), coordinate(70, 10), coordinate(10, 70)}}});
    CHECK(f.document().selection()->coverageAtDocumentPixel(20, 20) == 255);
    CHECK(f.document().selection()->coverageAtDocumentPixel(60, 60) == 0);
    const auto before = f.document().selection();
    f.call("vulkana_selection", {{"shape", "rectangle"}, {"from", coordinate(0, 0)}}, false);
    CHECK(f.document().selection() == before);
    f.call("vulkana_selection", {{"shape", "all"}});
    f.call("vulkana_selection", {{"shape", "invert"}});
    CHECK(f.document().selection() && f.document().selection()->bounds().empty());
    const auto empty = f.document().selection();
    f.call("vulkana_brush_stroke", f.stroke({point(80, 80)}, "#ffffff"));
    CHECK(f.sample(80, 80).alpha == 0 && f.document().selection() == empty);
}

void emptyAlphaColorsAndSampling()
{
    Fixture f;
    f.call("vulkana_selection", {{"shape", "rectangle"}, {"from", coordinate(5, 5)}, {"to", coordinate(10, 10)}});
    const auto before = f.document().selection();
    const auto depth = f.session().history().undoDepth();
    const auto data = f.call("vulkana_selection_from_alpha", f.targetArgs());
    CHECK(data.value("sourceWasEmpty").toBool());
    CHECK(f.document().selection() == before && f.session().history().undoDepth() == depth);
    f.call("vulkana_colors", {{"primary", "#123456"}, {"secondary", "#ff000080"}, {"active", "secondary"}});
    CHECK(f.session().foregroundColor() == (c::Rgba8{255, 0, 0, 128}));
    CHECK(f.session().history().undoDepth() == depth);
    f.call("vulkana_tool_select", {{"tool", "brush"}});
    CHECK(f.session().activeTool() == c::ToolId::Brush);
    f.call("vulkana_selection", {{"shape", "none"}});
    f.call("vulkana_brush_stroke", f.stroke({point(40, 40)}, "#ff8800"));
    auto sample = f.targetArgs(); sample["point"] = coordinate(40, 40); sample["source"] = "layer";
    const auto sampled = f.call("vulkana_sample_color", sample);
    CHECK(sampled.value("color").toString() == "#ff8800ff");
    const auto presets = f.call("vulkana_brush_presets", {}, true, false);
    CHECK(!presets.value("presets").toArray().isEmpty());
}
}

int main(int argc, char** argv)
{
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication app(argc, argv);
    app.setOrganizationName("ImageEditorTests"); app.setApplicationName("McpPainting");
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    brushIsUndoableAndValidatesFirst();
    transformedBrushHonorsSelectionAndMask();
    fillAndSelections();
    emptyAlphaColorsAndSampling();
    std::cout << "MCP painting/selection failures: " << failures << '\n';
    return failures ? 1 : 0;
}
