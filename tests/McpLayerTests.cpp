#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/McpController.hpp"
#include "imageeditor/core/AdjustmentCommands.hpp"
#include "imageeditor/core/FilterCommands.hpp"
#include "imageeditor/core/LayerGeometry.hpp"
#include "imageeditor/core/LayerTransform.hpp"
#include "NativeGraphicsStartup.hpp"
#include <QApplication>
#include <QJsonDocument>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <cmath>
#include <iostream>

namespace c = imageeditor::core;
namespace u = imageeditor::ui;
namespace {
int failures = 0;
#define CHECK(value) do { if (!(value)) { ++failures; std::cerr << "FAIL " << __LINE__ << ": " << #value << '\n'; } } while (false)

struct Fixture {
    u::MainWindow window {nullptr, false, false};
    u::McpController controller {window};
    Fixture() { window.setUnsavedPromptEnabled(false); window.show(); CHECK(imageeditor::test::waitForGraphicsStartup(window)); QCoreApplication::processEvents(); }
    ~Fixture() { window.close(); QCoreApplication::processEvents(); }
    c::EditorSession& session() { return const_cast<c::EditorSession&>(window.editorSession()); }
    c::Document& doc() { return *session().document(); }
    QString documentId() const { return QString::number(window.activeDocumentId()); }
    QJsonObject request(const char* name, QJsonObject args = {}, bool expectedError = false, bool scoped = true) {
        if (scoped && !args.contains("documentId")) args["documentId"] = documentId();
        auto response = controller.registry().call(QString::fromLatin1(name), args);
        if (response["isError"].toBool() != expectedError)
            std::cerr << name << ": " << QJsonDocument(response).toJson(QJsonDocument::Compact).constData() << '\n';
        CHECK(response["isError"].toBool() == expectedError);
        return response["structuredContent"].toObject();
    }
    QString create(const QString& kind, QJsonObject args = {}) {
        args["kind"] = kind;
        const auto value = request("vulkana_layer_create", args)["layerId"].toString(); CHECK(!value.isEmpty()); return value;
    }
    const c::Layer& layer(const QString& id) { const auto* value = doc().layer(id.toULongLong()); if (!value) throw std::runtime_error("Missing test layer"); return *value; }
};

void creationAndHistory()
{
    Fixture f;
    const auto initial = f.doc().layers().size();
    const auto raster = f.create("raster", {{"name", "Base"}, {"width", 32}, {"height", 24}, {"color", "#20406080"}, {"x", 8}, {"y", 10}});
    CHECK(f.doc().layers().size() == initial + 1);
    const auto& pixels = std::get<c::RasterLayer>(f.layer(raster).payload).surface;
    CHECK((pixels->extent() == c::Extent2u{32, 24}));
    CHECK(f.layer(raster).localToDocument.m02 == 8 && f.layer(raster).localToDocument.m12 == 10);
    const auto text = f.create("text", {{"name", "Caption"}, {"text", QJsonObject{{"text", "Hello\nVulkana"}, {"fontSize", 36}, {"color", "#aabbcc"}}}});
    CHECK(std::get<c::TextLayer>(f.layer(text).payload).utf8 == "Hello\nVulkana");
    CHECK(std::get<c::TextLayer>(f.layer(text).payload).defaultStyle.sizePixels == 36);
    f.request("vulkana_text_set", {{"layerId", text}, {"properties", QJsonObject{{"fontSize", 48}, {"alignment", "right"}}}});
    CHECK(std::get<c::TextLayer>(f.layer(text).payload).defaultStyle.sizePixels == 48);
    for (const auto& p : std::get<c::TextLayer>(f.layer(text).payload).paragraphs) CHECK(p.alignment == c::TextAlignment::Right);
    CHECK(f.session().undo()); CHECK(std::get<c::TextLayer>(f.layer(text).payload).defaultStyle.sizePixels == 36);
    const auto revision = f.doc().revision();
    f.request("vulkana_text_set", {{"layerId", text}, {"properties", QJsonObject{{"fontFamily", "Definitely-Not-An-Installed-Font-293762"}}}}, true);
    CHECK(f.doc().revision() == revision);
    f.request("vulkana_layer_create", {{"kind", "raster"}, {"shape", QJsonObject{{"kind", "ellipse"}}}}, true);
    CHECK(f.doc().revision() == revision);
    f.request("vulkana_layer_set_opacity", {{"layerId", raster}, {"opacity", .5}, {"unexpected", 42}}, true);
    CHECK(f.doc().revision() == revision);
    f.request("vulkana_layer_set_opacity", {{"layerId", raster}, {"opacity", .5}});
    CHECK(f.layer(raster).opacity == .5F);
    f.request("vulkana_layer_set_fill_opacity", {{"layerId", raster}, {"opacity", .25}});
    CHECK(f.layer(raster).fillOpacity == .25F);
    f.request("vulkana_layer_set_blend_mode", {{"layerId", raster}, {"mode", "multiply"}});
    CHECK(f.layer(raster).blendMode == c::BlendMode::Multiply);
    f.request("vulkana_layer_select", {{"layerIds", QJsonArray{text, raster}}});
    CHECK(f.session().activeLayer() == raster.toULongLong());
    const auto oldSelection = f.session().layerSelectionState();
    const auto duplicate = f.request("vulkana_layer_duplicate", {{"layerIds", QJsonArray{text}}})["layerIds"].toArray();
    CHECK(duplicate.size() == 1 && duplicate[0] != text);
    CHECK(f.session().undo()); CHECK(f.session().layerSelectionState() == oldSelection);
}

void shapesTransformsAndGroups()
{
    Fixture f;
    const auto shape = f.create("shape", {{"shape", QJsonObject{{"kind", "ellipse"}, {"width", 80}, {"height", 60}, {"fillColor", "#ff0000"}}}});
    f.request("vulkana_shape_set", {{"layerId", shape}, {"properties", QJsonObject{{"width", 100}, {"strokeEnabled", true}, {"strokeWidth", 4}}}});
    CHECK(std::get<c::ShapeLayer>(f.layer(shape).payload).size.width == 100);
    const auto before = f.layer(shape).localToDocument;
    f.request("vulkana_layer_transform", {{"layerIds", QJsonArray{shape}}, {"centerX", 160}, {"centerY", 120}, {"rotation", 30}});
    const auto values = c::valuesFromTransform(f.layer(shape).localToDocument, c::layerGeometryExtent(f.layer(shape)));
    CHECK(values && std::abs(values->center.x - 160) < 1e-6 && std::abs(values->center.y - 120) < 1e-6);
    CHECK(f.session().undo()); CHECK(f.layer(shape).localToDocument == before);
    const auto revision = f.doc().revision();
    f.request("vulkana_layer_transform", {{"layerIds", QJsonArray{shape}}, {"scaleX", 0}}, true);
    CHECK(f.doc().revision() == revision && f.layer(shape).localToDocument == before);
    const auto other = f.create("raster", {{"width", 16}, {"height", 16}});
    const auto group = f.request("vulkana_layer_group", {{"layerIds", QJsonArray{shape, other}}, {"kind", "clipping_group"}, {"name", "Clipped"}})["layerId"].toString();
    CHECK(f.doc().tree().container(group.toULongLong()));
    CHECK((f.doc().tree().container(group.toULongLong())->children == std::vector<c::LayerId>{shape.toULongLong(), other.toULongLong()}));
    f.request("vulkana_layer_set_visibility", {{"layerId", group}, {"visible", false}});
    f.request("vulkana_layer_ungroup", {{"layerId", group}});
    CHECK(!f.doc().tree().container(group.toULongLong()));
    CHECK(!f.layer(shape).visible && !f.layer(other).visible);
    CHECK(f.session().undo()); CHECK(f.doc().tree().container(group.toULongLong()));
    const auto tree = f.doc().tree();
    f.request("vulkana_layer_reparent", {{"layerIds", QJsonArray{group}}, {"parentId", group}, {"index", 0}}, true);
    CHECK(f.doc().tree() == tree);
    f.request("vulkana_layer_delete", {{"layerIds", QJsonArray{group}}});
    CHECK(!f.doc().containsItem(group.toULongLong()) && !f.doc().containsLayer(shape.toULongLong()));
    CHECK(f.session().undo()); CHECK(f.doc().tree() == tree);
}

void masksAndStyles()
{
    Fixture f;
    const auto raster = f.create("raster", {{"width", 32}, {"height", 24}, {"color", "#ffffffff"}});
    f.request("vulkana_layer_mask", {{"layerId", raster}, {"operation", "reveal_all"}});
    CHECK(f.layer(raster).mask && f.layer(raster).mask->outside == 255);
    const auto mask = f.layer(raster).mask;
    const auto revision = f.doc().revision();
    f.request("vulkana_layer_mask", {{"layerId", raster}, {"operation", "hide_all"}}, true);
    CHECK(f.doc().revision() == revision && f.layer(raster).mask == mask);
    f.request("vulkana_layer_mask", {{"layerId", raster}, {"operation", "invert"}});
    CHECK(f.layer(raster).mask->outside == 0);
    CHECK(f.session().undo()); CHECK(f.layer(raster).mask == mask);
    f.request("vulkana_layer_select", {{"layerIds", QJsonArray{raster}}, {"target", "mask"}});
    CHECK(f.session().editingLayerMask());
    f.request("vulkana_layer_select", {{"layerIds", QJsonArray{raster}}, {"target", "content"}});
    CHECK(!f.session().editingLayerMask());
    CHECK(f.request("vulkana_effect_catalog", {}, false, false)["effects"].toArray().size() == 8);
    f.request("vulkana_effect_set", {{"layerId", raster}, {"type", "stroke"}, {"parameters", QJsonObject{{"size", 7}, {"color", "#ff0000"}}}});
    CHECK(f.layer(raster).effects && f.layer(raster).effects->items[0].enabled && f.layer(raster).effects->items[0].size == 7);
    const auto beforeEffects = f.layer(raster).effects;
    f.request("vulkana_effect_set", {{"layerId", raster}, {"type", "stroke"}, {"parameters", QJsonObject{{"distance", 10}}}}, true);
    CHECK(f.layer(raster).effects == beforeEffects);
    const QJsonArray badStops {QJsonObject{{"position", 1}, {"color", "#ffffff"}}, QJsonObject{{"position", 0}, {"color", "#000000"}}};
    f.request("vulkana_effect_set", {{"layerId", raster}, {"type", "gradient-overlay"}, {"parameters", QJsonObject{{"gradientStops", badStops}}}}, true);
    CHECK(f.layer(raster).effects == beforeEffects);
    f.request("vulkana_effect_blending", {{"layerId", raster}, {"pixelAlphaShapesEffects", false}, {"maskEffectsAfterRendering", true}});
    CHECK(!f.layer(raster).effects->pixelAlphaShapesEffects && f.layer(raster).effects->maskEffectsAfterRendering);
    CHECK(f.layer(raster).effects->items[0].size == 7);
    CHECK(f.request("vulkana_adjustment_catalog", {}, false, false)["adjustments"].toArray().size() == 10);
    CHECK(f.request("vulkana_filter_catalog", {}, false, false)["filters"].toArray().size() == 3);
    const c::AdjustmentMask capture{c::SelectionMask::filled({32, 24}, 128), {}};
    auto adjustments = std::make_shared<c::AdjustmentStack>(); adjustments->items[0].mask = capture;
    CHECK(f.session().execute(std::make_unique<c::SetLayerAdjustmentsCommand>(raster.toULongLong(), nullptr, adjustments)));
    auto filters = std::make_shared<c::SpatialFilterStack>(); filters->items[0].mask = capture;
    CHECK(f.session().execute(std::make_unique<c::SetLayerFiltersCommand>(raster.toULongLong(), nullptr, filters)));
    f.request("vulkana_adjustment_set", {{"layerId", raster}, {"type", "exposure"}, {"parameters", QJsonObject{{"stops", 1}}}});
    CHECK(std::get<c::ExposureParameters>(f.layer(raster).adjustments->items[0].parameters).stops == 1);
    CHECK(f.layer(raster).adjustments->items[0].mask->coverage == capture.coverage);
    f.request("vulkana_filter_set", {{"layerId", raster}, {"type", "gaussian-blur"}, {"parameters", QJsonObject{{"radiusX", 2}, {"radiusY", 3}}}});
    CHECK(std::get<c::GaussianBlurParameters>(f.layer(raster).filters->items[0].parameters).radiusY == 3);
    CHECK(f.layer(raster).filters->items[0].mask->coverage == capture.coverage);
    const auto info = f.request("vulkana_layer_get", {{"layerId", raster}});
    CHECK(info["effects"].toArray().size() == 8 && info["filters"].toArray().size() == 3);
    f.request("vulkana_layer_get", {{"documentId", "999999999"}, {"layerId", raster}}, true);
    const auto shifted = f.create("raster", {{"width", 16}, {"height", 12}, {"x", 50}, {"y", 30}, {"color", "#ffffffff"}});
    const auto selected = c::SelectionMask::rectangle(f.doc().canvas().extent, {50, 30, 8, 12});
    CHECK(f.doc().setSelection(selected));
    f.request("vulkana_layer_mask", {{"layerId", shifted}, {"operation", "from_selection"}});
    CHECK(f.doc().selection() == selected);
    CHECK(c::layerMaskCoverage(f.layer(shifted).mask, {2.5, 2.5}) == 1);
    CHECK(c::layerMaskCoverage(f.layer(shifted).mask, {12.5, 2.5}) == 0);
    const auto adjustment = f.create("adjustment", {{"scope", "this_group"}});
    CHECK(std::get<c::AdjustmentLayer>(f.layer(adjustment).payload).scope == c::AdjustmentScope::ThisGroup);
    f.request("vulkana_adjustment_scope", {{"layerId", adjustment}, {"scope", "all_below"}});
    CHECK(std::get<c::AdjustmentLayer>(f.layer(adjustment).payload).scope == c::AdjustmentScope::AllBelow);
    f.request("vulkana_effect_set", {{"layerId", adjustment}, {"type", "stroke"}, {"parameters", QJsonObject{{"size", 5}}}}, true);
}
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("ImageEditorTests");
    QCoreApplication::setApplicationName("McpLayers");
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    creationAndHistory(); shapesTransformsAndGroups(); masksAndStyles();
    std::cout << "MCP layer checks: " << failures << " failures\n";
    return failures ? 1 : 0;
}
