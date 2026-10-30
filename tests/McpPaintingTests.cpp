#include "imageeditor/ui/McpController.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/FlattenedDocument.hpp"
#include "imageeditor/core/ColorSampler.hpp"
#include "imageeditor/core/LayerMask.hpp"
#include "imageeditor/core/RegionalRasterSurface.hpp"
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

void brushCatalogDistinguishesPreparation()
{
    Fixture f;
    const auto before = f.call("vulkana_brush_presets", {}, true, false)["presets"].toArray();
    QString loadOnUse;
    for (const auto value : before) {
        const auto preset = value.toObject();
        CHECK(preset["available"].isBool() && preset["prepared"].isBool());
        if (preset["available"].toBool()) {
            CHECK(!preset.contains("reason"));
            CHECK(preset["status"] == (preset["prepared"].toBool() ? "ready" : "load_on_use"));
            if (!preset["prepared"].toBool()) loadOnUse = preset["id"].toString();
        } else {
            CHECK(preset["status"] == "unavailable" && !preset["prepared"].toBool() && !preset["reason"].toString().isEmpty());
        }
    }
    CHECK(!loadOnUse.isEmpty()); // Packaged optional tips start unloaded.
    CHECK(f.call("vulkana_brush_presets", {}, true, false)["presets"].toArray() == before);
    if (loadOnUse.isEmpty()) return;
    auto stroke = f.stroke({point(48,48)}, "#ff0000", 24); stroke["presetId"] = loadOnUse;
    f.call("vulkana_brush_stroke", stroke);
    const auto after = f.call("vulkana_brush_presets", {}, true, false)["presets"].toArray();
    bool ready = false;
    for (const auto value : after) {
        const auto preset = value.toObject();
        if (preset["id"] == loadOnUse) ready = preset["available"].toBool() && preset["prepared"].toBool() && preset["status"] == "ready";
    }
    CHECK(ready);
}

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

void batchPaintingIsAtomicAndOneUndo()
{
    Fixture f;
    const auto single = [&](QJsonArray points, const char* color) {
        return QJsonObject{{"points", points}, {"color", color}, {"size", 12}};
    };
    const auto batch = [&](QJsonArray strokes, const char* target = "pixels") {
        auto args = f.targetArgs(); args["target"] = target; args["strokes"] = strokes; return args;
    };
    const auto red = single({point(12,20),point(70,20)}, "#ff0000");
    const auto blue = single({point(12,40),point(70,40)}, "#0000ff");
    f.call("vulkana_brush_stroke", f.stroke({point(20,80)}, "#00ff00"));
    CHECK(f.session().undo()); // Keep an existing redo branch through failures/no-ops.
    const auto source = std::get<c::RasterLayer>(f.document().layer(f.target)->payload).surface;
    const auto sourceRevision = source->revision();
    const auto revision = f.document().revision();
    const auto token = f.controller->state(false)["document"].toObject()["revision"].toString();
    const auto depth = f.session().history().undoDepth(), redo = f.session().history().redoDepth();
    const auto selected = f.session().layerSelectionState();
    const auto colors = f.session().colors(); const auto tool = f.session().activeTool();
    auto missing = blue; missing["presetId"] = "not-an-installed-preset";
    f.call("vulkana_brush_strokes", batch({red, missing}), false);
    auto bad = blue; bad["unknown"] = 1;
    f.call("vulkana_brush_strokes", batch({red, bad}), false);
    auto tooLong = single({point(.5,60.5),point(20000,60.5)}, "#00ff00");
    tooLong["size"] = .25; tooLong["spacing"] = 1;
    // The first stroke completes in private storage before the second exceeds
    // its dab budget. Neither stroke can leak into the live document.
    f.call("vulkana_brush_strokes", batch({red, tooLong}), false);
    CHECK(f.sample(40,20).alpha == 0 && f.sample(10,60).alpha == 0);
    CHECK(f.document().revision() == revision && source->revision() == sourceRevision);
    CHECK(f.controller->state(false)["document"].toObject()["revision"] == token);
    CHECK(f.session().history().undoDepth() == depth && f.session().history().redoDepth() == redo);
    auto invisible = red; invisible["opacity"] = 0;
    CHECK(!f.call("vulkana_brush_strokes", batch({invisible}))["changed"].toBool());
    CHECK(f.session().history().redoDepth() == redo && source->revision() == sourceRevision);

    const auto painted = f.call("vulkana_brush_strokes", batch({red, blue}));
    CHECK(painted["changed"].toBool() && painted["strokeCount"] == 2 && painted["dabs"].toDouble() > 0);
    CHECK(f.sample(40,20) == (c::Rgba8{255,0,0,255}));
    CHECK(f.sample(40,40) == (c::Rgba8{0,0,255,255}));
    CHECK(f.session().history().undoDepth() == depth+1 && !f.session().history().canRedo());
    CHECK(f.session().layerSelectionState() == selected && f.session().colors() == colors && f.session().activeTool() == tool);
    CHECK(source->revision() == sourceRevision); // The original surface is still frozen.
    CHECK(f.session().undo());
    CHECK(std::get<c::RasterLayer>(f.document().layer(f.target)->payload).surface == source);
    CHECK(f.sample(40,20).alpha == 0 && f.sample(40,40).alpha == 0);
    CHECK(f.session().redo()); CHECK(f.sample(40,20).red == 255 && f.sample(40,40).blue == 255);

    // A subsequent ordinary stroke remains independently undoable over the COW result.
    f.call("vulkana_brush_stroke", f.stroke({point(40,20)}, "#00ff00"));
    CHECK(f.sample(40,20).green == 255);
    CHECK(f.session().undo()); CHECK(f.sample(40,20).red == 255);
    CHECK(f.session().undo()); CHECK(f.sample(40,20).alpha == 0);
    CHECK(f.session().redo()); CHECK(f.sample(40,20).red == 255);
}

void maskBatchPreservesPixelsAndSelection()
{
    Fixture f;
    CHECK(f.document().setLayerTransform(f.target, {2,0,16,0,2,10}));
    f.call("vulkana_selection", {{"shape", "rectangle"}, {"from", coordinate(30,25)}, {"to", coordinate(50,45)}});
    const auto selection = f.document().selection();
    auto addMask = f.targetArgs(); addMask["operation"] = "reveal_all";
    f.call("vulkana_layer_mask", addMask);
    f.call("vulkana_effect_set", {{"layerId", QString::number(f.target)}, {"type", "stroke"}, {"parameters", QJsonObject{{"size", 2}, {"color", "#ff0000"}}}});
    f.call("vulkana_filter_set", {{"layerId", QString::number(f.target)}, {"type", "gaussian-blur"}, {"parameters", QJsonObject{{"radiusX", 1}, {"radiusY", 1}}}});
    f.call("vulkana_adjustment_set", {{"layerId", QString::number(f.target)}, {"type", "exposure"}, {"parameters", QJsonObject{{"stops", .5}}}});
    const auto before = f.document().layer(f.target)->mask;
    const auto effects = f.document().layer(f.target)->effects;
    const auto filters = f.document().layer(f.target)->filters;
    const auto adjustments = f.document().layer(f.target)->adjustments;
    const auto pixels = std::get<c::RasterLayer>(f.document().layer(f.target)->payload).surface;
    const auto depth = f.session().history().undoDepth();
    auto args = f.targetArgs(); args["target"] = "mask";
    args["strokes"] = QJsonArray{
        QJsonObject{{"points", QJsonArray{point(36,35)}}, {"color", "#000000"}, {"size", 8}},
        QJsonObject{{"points", QJsonArray{point(46,35)}}, {"mode", "erase"}, {"size", 8}}};
    const auto result = f.call("vulkana_brush_strokes", args);
    CHECK(result["changed"].toBool() && f.session().history().undoDepth() == depth+1);
    const auto* layer = f.document().layer(f.target);
    CHECK(c::layerMaskCoverage(layer->mask, layer->localToDocument.inverted()->map({36,35})) < .01f);
    CHECK(c::layerMaskCoverage(layer->mask, layer->localToDocument.inverted()->map({46,35})) < .01f);
    CHECK(c::layerMaskCoverage(layer->mask, layer->localToDocument.inverted()->map({25,35})) == 1);
    CHECK(f.document().selection() == selection && std::get<c::RasterLayer>(layer->payload).surface == pixels);
    CHECK(layer->effects == effects && layer->filters == filters && layer->adjustments == adjustments);
    CHECK(f.session().history().latestUndoMemoryCost() <= before->coverage->memoryCost() + layer->mask->coverage->memoryCost() + 4096);
    CHECK(f.session().undo()); CHECK(f.document().layer(f.target)->mask == before);
}

void batchBaseSurvivesEarlierHistoryAndRepeatedBatches()
{
    Fixture f;
    f.call("vulkana_brush_stroke", f.stroke({point(80,80)}, "#ff0000"));
    auto args = f.targetArgs();
    args["strokes"] = QJsonArray{QJsonObject{{"points", QJsonArray{point(10,20)}}, {"color", "#00ff00"}, {"size", 8}}};
    f.call("vulkana_brush_strokes", args);
    args["strokes"] = QJsonArray{QJsonObject{{"points", QJsonArray{point(80,20)}}, {"color", "#0000ff"}, {"size", 8}}};
    f.call("vulkana_brush_strokes", args);
    CHECK(f.sample(80,80).red == 255 && f.sample(10,20).green == 255 && f.sample(80,20).blue == 255);
    CHECK(f.session().undo()); CHECK(f.sample(80,20).alpha == 0 && f.sample(10,20).green == 255);
    CHECK(f.session().undo()); CHECK(f.sample(10,20).alpha == 0 && f.sample(80,80).red == 255);
    CHECK(f.session().undo()); CHECK(f.sample(80,80).alpha == 0);
    // Redo an older raster journal before reusing the later batches' pinned
    // base and COW tiles; none of the three saved appearances may drift.
    CHECK(f.session().redo()); CHECK(f.sample(80,80).red == 255 && f.sample(10,20).alpha == 0);
    CHECK(f.session().redo()); CHECK(f.sample(10,20).green == 255 && f.sample(80,20).alpha == 0);
    CHECK(f.session().redo()); CHECK(f.sample(80,20).blue == 255 && f.sample(80,80).red == 255);
}

void tinyBatchAccountsForTilesAndNetNoOp()
{
    Fixture f;
    const auto id = f.call("vulkana_layer_create", {{"kind", "raster"}, {"width", 1}, {"height", 1}})["layerId"].toString();
    f.target = id.toULongLong();
    // Cover the entire one-pixel target. A 1px circular dab has fractional
    // coverage, so painting then partially erasing it is correctly not a no-op.
    const QJsonObject paint{{"points", QJsonArray{point(.5,.5)}}, {"size", 4}, {"hardness", 1}, {"color", "#ff0000"}};
    auto args = f.targetArgs(); args["strokes"] = QJsonArray{paint};
    const auto painted = f.call("vulkana_brush_strokes", args);
    CHECK(painted["changed"].toBool());
    if (!painted["changed"].toBool()) return; // Do not undo creation after a failed paint assertion.
    const auto surface = std::get<c::RasterLayer>(f.document().layer(f.target)->payload).surface;
    const auto* regional = dynamic_cast<const c::RegionalRasterSurface*>(surface.get());
    CHECK(regional && f.session().history().latestUndoMemoryCost() >= regional->retainedBytes());
    CHECK(surface->extent().width > 1 && surface->extent().height > 1);
    CHECK(f.session().undo());
    CHECK(f.document().layer(f.target));
    if (!f.document().layer(f.target)) return;
    CHECK((std::get<c::RasterLayer>(f.document().layer(f.target)->payload).surface->extent() == c::Extent2u{1,1}));
    CHECK(f.session().redo()); CHECK(f.sample(.5,.5).red == 255);

    // Test exact cancellation inside existing storage. A one-pixel selection
    // excludes the circular dab's fractional edge. Erasing preserves hidden
    // RGB, so black paint matches the original transparent-black bytes exactly.
    // Erasing does not shrink grown storage; growth is tested separately above.
    Fixture g;
    g.call("vulkana_selection", {{"shape","rectangle"},{"from",coordinate(30,30)},{"to",coordinate(31,31)}});
    g.call("vulkana_brush_stroke", g.stroke({point(30.5,30.5)}, "#00ff00", 4));
    CHECK(g.session().undo());
    const auto redo = g.session().history().redoDepth(), depth = g.session().history().undoDepth();
    const auto revision = g.document().revision();
    auto selectedPaint = paint; selectedPaint["points"] = QJsonArray{point(30.5,30.5)}; selectedPaint["color"] = "#000000";
    auto selectedErase = selectedPaint; selectedErase["mode"] = "erase";
    auto selectedArgs = g.targetArgs(); selectedArgs["strokes"] = QJsonArray{selectedPaint,selectedErase};
    const auto unchanged = g.call("vulkana_brush_strokes", selectedArgs);
    CHECK(!unchanged["changed"].toBool());
    CHECK(g.sample(30.5,30.5).alpha == 0 && g.document().revision() == revision);
    CHECK(g.session().history().undoDepth() == depth && g.session().history().redoDepth() == redo);
}

void batchMatchesSequentialStrokesIncludingStorageGrowth()
{
    for (bool grow : {false,true}) {
        Fixture sequential, batch;
        for (auto* f : {&sequential,&batch}) {
            f->call("vulkana_canvas_resize",{{"width",grow?96:640},{"height",grow?96:480}});
            const auto created = f->call("vulkana_layer_create",{{"kind","raster"},{"width",grow?1:640},{"height",grow?1:480},
                {"x",grow?16:0},{"y",grow?16:0}});
            f->target=created["layerId"].toString().toULongLong();
        }
        QJsonArray strokes;
        if (grow) {
            for (const auto& p : {point(16.5,16.5),point(90.5,30.5),point(10.5,90.5)})
                strokes.append(QJsonObject{{"points",QJsonArray{p}},{"color","#ff8d54"},{"size",4}});
        } else {
            for (int y : {190,245,300}) strokes.append(QJsonObject{{"points",QJsonArray{point(362,y),point(432,y)}},
                {"color","#ff8d54"},{"size",3}});
        }
        for (const auto& entry : strokes) {
            auto args=entry.toObject();args["layerId"]=QString::number(sequential.target);
            sequential.call("vulkana_brush_stroke",args);
        }
        const auto original=std::get<c::RasterLayer>(batch.document().layer(batch.target)->payload).surface;
        const auto sourceRevision=original->revision();
        auto args=batch.targetArgs();args["strokes"]=strokes;
        const auto painted=batch.call("vulkana_brush_strokes",args);
        CHECK(painted["changed"].toBool());
        if(!painted["changed"].toBool()) continue;
        const auto& expected=*sequential.document().layer(sequential.target);
        const auto& actual=*batch.document().layer(batch.target);
        CHECK(actual.rasterOrigin==expected.rasterOrigin && actual.rasterEffectFrame==expected.rasterEffectFrame);
        CHECK(std::get<c::RasterLayer>(actual.payload).surface->extent()==std::get<c::RasterLayer>(expected.payload).surface->extent());
        const auto rendered=u::flattenDocument(sequential.document());CHECK(rendered);
        CHECK(u::flattenDocument(batch.document()).image==rendered.image);
        CHECK(original->revision()==sourceRevision);
        CHECK(batch.session().undo());
        CHECK(std::get<c::RasterLayer>(batch.document().layer(batch.target)->payload).surface==original);
        CHECK(batch.session().redo());
        CHECK(u::flattenDocument(batch.document()).image==rendered.image);
    }
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
    brushCatalogDistinguishesPreparation();
    brushIsUndoableAndValidatesFirst();
    transformedBrushHonorsSelectionAndMask();
    batchPaintingIsAtomicAndOneUndo();
    maskBatchPreservesPixelsAndSelection();
    batchBaseSurvivesEarlierHistoryAndRepeatedBatches();
    tinyBatchAccountsForTilesAndNetNoOp();
    batchMatchesSequentialStrokesIncludingStorageGrowth();
    fillAndSelections();
    emptyAlphaColorsAndSampling();
    std::cout << "MCP painting/selection failures: " << failures << '\n';
    return failures ? 1 : 0;
}
