#include "imageeditor/ui/McpController.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/QtShapeRenderService.hpp"
#include "imageeditor/ui/QtTextLayoutService.hpp"
#include "imageeditor/core/AdjustmentCommands.hpp"
#include "imageeditor/core/EffectCommands.hpp"
#include "imageeditor/core/FilterCommands.hpp"
#include "imageeditor/core/LayerCommands.hpp"
#include "imageeditor/core/LayerGeometry.hpp"
#include "imageeditor/core/LayerMaskEdit.hpp"
#include "imageeditor/core/LayerStructureCommands.hpp"
#include "imageeditor/core/LayerTransform.hpp"
#include "imageeditor/core/RichText.hpp"
#include "imageeditor/core/ShapeCommands.hpp"
#include "AdjustmentCodec.hpp"
#include "LayerEffectCodec.hpp"
#include "SpatialFilterCodec.hpp"
#include "McpToolHelpers.hpp"
#include <QFontDatabase>
#include <algorithm>
#include <cmath>

namespace imageeditor::ui {
namespace {
namespace c = core;
using namespace mcpdetail;
QString text(std::string_view s) { return QString::fromUtf8(s.data(), qsizetype(s.size())); }
QJsonArray blendIds()
{
    QJsonArray values;
    for (const auto mode : c::allBlendModes) values.append(text(c::blendModeId(mode)));
    return values;
}
QJsonArray ids(std::span<const c::LayerId> values)
{
    QJsonArray result;
    for (const auto value : values) result.append(id(value));
    return result;
}
std::vector<c::LayerId> selectedIds(const c::Document& document, const QJsonObject& args, bool normalize = true)
{
    std::vector<c::LayerId> values;
    for (const auto value : args["layerIds"].toArray()) {
        const auto target = id(QJsonObject{{"id", value}}, "id");
        require(document.containsItem(target), "Unknown layer or container ID: " + id(target));
        require(std::find(values.begin(), values.end(), target) == values.end(), "Duplicate layer IDs");
        values.push_back(target);
    }
    require(!values.empty(), "At least one layer ID is required");
    return normalize ? document.tree().normalize(values) : values;
}
// Explicit MCP targets need not already be selected in the editor. Preserve
// the user's prior selection when undoing commands built for those targets.
class SelectionRestoringCommand final : public c::Command {
public:
    SelectionRestoringCommand(std::unique_ptr<c::Command> operation, c::LayerSelectionState before)
        : operation_(std::move(operation)), before_(std::move(before)) {}
    bool apply(c::Document& doc) override { return operation_->apply(doc); }
    bool undo(c::Document& doc) override { return operation_->undo(doc); }
    std::string_view label() const noexcept override { return operation_->label(); }
    std::size_t memoryCost() const noexcept override { return sizeof(*this) + before_.ids.capacity() * sizeof(c::LayerId) + operation_->memoryCost(); }
    const c::LayerSelectionState* layerSelectionAfter(bool undo) const noexcept override
    { return undo ? &before_ : operation_->layerSelectionAfter(false); }
private:
    std::unique_ptr<c::Command> operation_;
    c::LayerSelectionState before_;
};
c::LayerSelectionState selection(std::vector<c::LayerId> values)
{
    const auto primary = values.empty() ? std::optional<c::LayerId>{} : values.back();
    return {std::move(values), primary, primary};
}
void removeSubtree(c::LayerTree& tree, c::LayerId target)
{
    if (const auto* container = tree.container(target)) {
        const auto children = container->children;
        for (const auto child : children) removeSubtree(tree, child);
        std::erase_if(tree.containers, [target](const auto& item) { return item.id == target; });
    }
    if (const auto placement = tree.placement(target)) std::erase(*tree.children(placement->parent), target);
}
QJsonObject textProperties()
{
    return {{"text", string("Plain Unicode text; LF creates a new line. Replacing content resets character runs.")},
        {"fontFamily", string("Installed family name. Query vulkana_font_list before choosing a font.")},
        {"fontStyle", string()}, {"fontSize", number(.25, 2048, "Font em size in document pixels, not points.")},
        {"weight", integer(1, 1000)}, {"italic", boolean()}, {"color", colorSchema()},
        {"alignment", choice({"left", "center", "right"})}};
}
c::TextLayer editText(c::TextLayer current, const QJsonObject& patch)
{
    if (patch.contains("fontFamily")) {
        const auto family = patch["fontFamily"].toString();
        require(QFontDatabase::families().contains(family, Qt::CaseInsensitive), "Font family is not installed: " + family);
    }
    if (patch.contains("fontStyle") && !patch["fontStyle"].toString().isEmpty()) {
        const auto family = patch["fontFamily"].toString(text(current.defaultStyle.font.family));
        require(QFontDatabase::styles(family).contains(patch["fontStyle"].toString(), Qt::CaseInsensitive), "Font style is not installed for family: " + family);
    }
    if (patch.contains("text")) {
        current.utf8 = patch["text"].toString().toUtf8().toStdString();
        require(current.utf8.size() <= c::kMaximumTextBytes, "Text exceeds the supported byte limit");
        current.runs.clear();
        current.paragraphs = {{}};
    }
    c::TextStylePatch style;
    if (patch.contains("fontFamily")) style.family = patch["fontFamily"].toString().toStdString();
    if (patch.contains("fontStyle")) style.style = patch["fontStyle"].toString().toStdString();
    if (patch.contains("fontSize")) style.sizePixels = patch["fontSize"].toDouble();
    if (patch.contains("weight")) style.weight = patch["weight"].toInt();
    if (patch.contains("italic")) style.italic = patch["italic"].toBool();
    if (patch.contains("color")) style.color = color(patch["color"].toString());
    current = c::normalizedText(std::move(current));
    current = c::formatTextRange(current, 0, current.utf8.size(), style);
    current.defaultStyle = c::patchedTextStyle(current.defaultStyle, style);
    if (patch.contains("alignment")) {
        const auto value = patch["alignment"].toString();
        const auto alignment = value == "center" ? c::TextAlignment::Center : value == "right" ? c::TextAlignment::Right : c::TextAlignment::Left;
        for (auto& paragraph : current.paragraphs) paragraph.alignment = alignment;
    }
    return c::normalizedText(std::move(current));
}
QString alignment(c::TextAlignment value)
{ return value == c::TextAlignment::Center ? "center" : value == c::TextAlignment::Right ? "right" : "left"; }
QJsonObject styleData(const c::TextStyle& style)
{
    return {{"fontFamily", text(style.font.family)}, {"fontStyle", text(style.font.style)},
        {"fontSize", style.sizePixels}, {"weight", style.font.weight}, {"italic", style.font.italic}, {"color", color(style.color)}};
}
QJsonObject textData(const c::TextLayer& value)
{
    auto result = styleData(value.defaultStyle);
    result["text"] = text(value.utf8);
    QJsonArray runs, paragraphs;
    for (const auto& run : value.runs)
        runs.append(QJsonObject{{"startUtf8Byte", double(run.start)}, {"lengthUtf8Bytes", double(run.length)}, {"style", styleData(run.style)}});
    for (const auto& paragraph : value.paragraphs)
        paragraphs.append(QJsonObject{{"startUtf8Byte", double(paragraph.start)}, {"alignment", alignment(paragraph.alignment)}});
    result["runs"] = runs;
    result["paragraphs"] = paragraphs;
    return result;
}
const QStringList shapeKinds {"rectangle", "rounded_rectangle", "ellipse", "triangle", "line", "polygon"};
QJsonObject shapeProperties()
{
    return {{"kind", choice(QJsonArray::fromStringList(shapeKinds))},
        {"width", number(0, c::maximumShapeDimension)}, {"height", number(0, c::maximumShapeDimension)},
        {"points", array(object({{"x", number(0, c::maximumShapeDimension)}, {"y", number(0, c::maximumShapeDimension)}}, {"x", "y"}), 0, 4096)},
        {"cornerRadius", number(0, c::maximumShapeStyleDimension)}, {"fillEnabled", boolean()}, {"fillColor", colorSchema()},
        {"strokeEnabled", boolean()}, {"strokeColor", colorSchema()}, {"strokeWidth", number(0, c::maximumShapeStyleDimension)},
        {"strokeJoin", choice({"round", "miter", "bevel"})}, {"strokeCap", choice({"round", "butt", "square"})},
        {"strokeMiterLimit", number(.5, 1000)}};
}
c::ShapeLayer editShape(c::ShapeLayer value, const QJsonObject& patch)
{
    if (patch.contains("kind")) value.kind = c::ShapeKind(shapeKinds.indexOf(patch["kind"].toString()));
    if (patch.contains("width")) value.size.width = patch["width"].toDouble();
    if (patch.contains("height")) value.size.height = patch["height"].toDouble();
    if (patch.contains("points")) {
        value.points.clear();
        for (const auto point : patch["points"].toArray()) value.points.push_back({point.toObject()["x"].toDouble(), point.toObject()["y"].toDouble()});
    }
    if (patch.contains("cornerRadius")) value.cornerRadius = patch["cornerRadius"].toDouble();
    if (patch.contains("fillEnabled")) value.fillEnabled = patch["fillEnabled"].toBool();
    if (patch.contains("fillColor")) value.fillColor = color(patch["fillColor"].toString());
    if (patch.contains("strokeEnabled")) value.strokeEnabled = patch["strokeEnabled"].toBool();
    if (patch.contains("strokeColor")) value.strokeColor = color(patch["strokeColor"].toString());
    if (patch.contains("strokeWidth")) value.strokeWidth = patch["strokeWidth"].toDouble();
    if (patch.contains("strokeJoin")) value.strokeJoin = c::ShapeJoin(QStringList{"round", "miter", "bevel"}.indexOf(patch["strokeJoin"].toString()));
    if (patch.contains("strokeCap")) value.strokeCap = c::ShapeCap(QStringList{"round", "butt", "square"}.indexOf(patch["strokeCap"].toString()));
    if (patch.contains("strokeMiterLimit")) value.strokeMiterLimit = patch["strokeMiterLimit"].toDouble();
    require(c::validShape(value) && c::hasShapeGeometry(value), "Invalid or empty shape geometry; lines need two points and polygons at least three within width/height");
    return value;
}
QJsonObject shapeData(const c::ShapeLayer& value)
{
    QJsonArray points;
    for (const auto p : value.points) points.append(QJsonObject{{"x", p.x}, {"y", p.y}});
    return {{"kind", shapeKinds.at(int(value.kind))}, {"width", value.size.width}, {"height", value.size.height},
        {"points", points}, {"cornerRadius", value.cornerRadius}, {"fillEnabled", value.fillEnabled}, {"fillColor", color(value.fillColor)},
        {"strokeEnabled", value.strokeEnabled}, {"strokeColor", color(value.strokeColor)}, {"strokeWidth", value.strokeWidth},
        {"strokeJoin", QStringList{"round", "miter", "bevel"}.at(int(value.strokeJoin))},
        {"strokeCap", QStringList{"round", "butt", "square"}.at(int(value.strokeCap))}, {"strokeMiterLimit", value.strokeMiterLimit}};
}

QJsonObject effectProperties(c::LayerEffectType type)
{
    QJsonObject properties{{"enabled", boolean()}, {"blendMode", choice(blendIds())}, {"opacity", number(0, 1)}, {"color", colorSchema()}};
    switch (type) {
    case c::LayerEffectType::Stroke:
        properties["size"] = number(0, c::maximumEffectSize, "Stroke width in layer-local pixels");
        properties["position"] = choice({"inside", "center", "outside"}); break;
    case c::LayerEffectType::DropShadow:
    case c::LayerEffectType::InnerShadow:
        properties["angle"] = number(-180, 180); properties["distance"] = number(0, c::maximumEffectDistance);
        [[fallthrough]];
    case c::LayerEffectType::OuterGlow:
    case c::LayerEffectType::InnerGlow:
        properties["size"] = number(0, c::maximumEffectSize); properties["spread"] = number(0, 1); break;
    case c::LayerEffectType::ColorOverlay: break;
    case c::LayerEffectType::GradientOverlay:
        properties["secondColor"] = colorSchema(); properties["angle"] = number(-180, 180);
        properties["scale"] = number(.01, 10); properties["reverse"] = boolean();
        properties["gradient"] = choice({"linear", "radial"});
        properties["gradientStops"] = array(object({{"position", number(0, 1)}, {"color", colorSchema()}}, {"position", "color"}), 0, int(c::maximumGradientStops));
        break;
    case c::LayerEffectType::BevelEmboss: {
        const auto contour = object({{"enabled", boolean()}, {"interpolation", choice({"linear", "smooth"})},
            {"points", array(object({{"input", number(0, 1)}, {"output", number(0, 1)}, {"corner", boolean()}}, {"input", "output"}), 2, 16)}}, {"enabled", "interpolation", "points"});
        properties["size"] = number(0, c::maximumEffectSize); properties["angle"] = number(-180, 180); properties["secondColor"] = colorSchema();
        properties["bevel"] = object({{"style", choice({"inner", "outer", "emboss"})}, {"depth", number(0, 10)},
            {"soften", number(0, 64)}, {"altitude", number(0, 90)}, {"down", boolean()}, {"shadowOpacity", number(0, 1)},
            {"shadowBlend", choice(blendIds())}, {"surface", contour}, {"gloss", contour}});
        break;
    }
    }
    return properties;
}
QJsonObject contourData(const c::EffectContour& value)
{
    QJsonArray points;
    for (const auto& p : value.points) points.append(QJsonObject{{"input", p.input}, {"output", p.output}, {"corner", p.corner}});
    return {{"enabled", value.enabled}, {"interpolation", value.interpolation == c::EffectContourInterpolation::Linear ? "linear" : "smooth"}, {"points", points}};
}
c::EffectContour parseContour(const QJsonObject& object)
{
    c::EffectContour value;
    value.enabled = object["enabled"].toBool();
    value.interpolation = object["interpolation"] == "linear" ? c::EffectContourInterpolation::Linear : c::EffectContourInterpolation::Smooth;
    value.points.clear();
    for (const auto p : object["points"].toArray()) {
        const auto point = p.toObject();
        value.points.push_back({point["input"].toDouble(), point["output"].toDouble(), point["corner"].toBool()});
    }
    require(c::validEffectContour(value), "Contour input points must be strictly increasing and include endpoints 0 and 1");
    return value;
}
QJsonObject effectData(c::LayerEffectType type, const c::LayerEffect& e)
{
    QJsonObject all{{"enabled", e.enabled}, {"blendMode", text(c::blendModeId(e.blendMode))}, {"opacity", e.opacity}, {"color", color(e.color)},
        {"secondColor", color(e.secondColor)}, {"size", e.size}, {"angle", e.angle}, {"distance", e.distance}, {"spread", e.spread},
        {"position", QStringList{"inside", "center", "outside"}.at(int(e.position))},
        {"gradient", e.gradient == c::GradientType::Linear ? "linear" : "radial"}, {"scale", e.scale}, {"reverse", e.reverse}};
    QJsonArray stops;
    for (const auto& stop : e.gradientStops) stops.append(QJsonObject{{"position", stop.position}, {"color", color(stop.color)}});
    all["gradientStops"] = stops;
    const auto& b = e.bevel;
    all["bevel"] = QJsonObject{{"style", QStringList{"inner", "outer", "emboss"}.at(int(b.style))}, {"depth", b.depth},
        {"soften", b.soften}, {"altitude", b.altitude}, {"down", b.down}, {"shadowOpacity", b.shadowOpacity},
        {"shadowBlend", text(c::blendModeId(b.shadowBlend))}, {"surface", contourData(b.surface)}, {"gloss", contourData(b.gloss)}};
    QJsonObject result;
    const auto properties = effectProperties(type);
    for (auto it = properties.begin(); it != properties.end(); ++it) result[it.key()] = all[it.key()];
    return result;
}
void patchEffect(c::LayerEffect& e, const QJsonObject& p)
{
    if (p.contains("enabled")) e.enabled = p["enabled"].toBool();
    if (p.contains("blendMode")) e.blendMode = *c::blendModeFromId(p["blendMode"].toString().toStdString());
    if (p.contains("opacity")) e.opacity = p["opacity"].toDouble();
    if (p.contains("color")) e.color = color(p["color"].toString());
    if (p.contains("secondColor")) e.secondColor = color(p["secondColor"].toString());
    if (p.contains("size")) e.size = p["size"].toDouble();
    if (p.contains("angle")) e.angle = p["angle"].toDouble();
    if (p.contains("distance")) e.distance = p["distance"].toDouble();
    if (p.contains("spread")) e.spread = p["spread"].toDouble();
    if (p.contains("position")) e.position = c::StrokePosition(QStringList{"inside", "center", "outside"}.indexOf(p["position"].toString()));
    if (p.contains("gradient")) e.gradient = p["gradient"] == "radial" ? c::GradientType::Radial : c::GradientType::Linear;
    if (p.contains("scale")) e.scale = p["scale"].toDouble();
    if (p.contains("reverse")) e.reverse = p["reverse"].toBool();
    if (p.contains("gradientStops")) {
        e.gradientStops.clear();
        for (const auto stop : p["gradientStops"].toArray()) e.gradientStops.push_back({stop.toObject()["position"].toDouble(), color(stop.toObject()["color"].toString())});
        require(e.gradientStops.empty() || e.gradientStops.size() >= 2, "A gradient needs at least two stops; an empty array restores two-color mode");
    }
    if (p.contains("bevel")) {
        auto& b = e.bevel; const auto q = p["bevel"].toObject();
        if (q.contains("style")) b.style = c::BevelStyle(QStringList{"inner", "outer", "emboss"}.indexOf(q["style"].toString()));
        if (q.contains("depth")) b.depth = q["depth"].toDouble();
        if (q.contains("soften")) b.soften = q["soften"].toDouble();
        if (q.contains("altitude")) b.altitude = q["altitude"].toDouble();
        if (q.contains("down")) b.down = q["down"].toBool();
        if (q.contains("shadowOpacity")) b.shadowOpacity = q["shadowOpacity"].toDouble();
        if (q.contains("shadowBlend")) b.shadowBlend = *c::blendModeFromId(q["shadowBlend"].toString().toStdString());
        if (q.contains("surface")) b.surface = parseContour(q["surface"].toObject());
        if (q.contains("gloss")) b.gloss = parseContour(q["gloss"].toObject());
    }
}
QJsonObject adjustmentProperties(c::AdjustmentType type)
{
    switch (type) {
    case c::AdjustmentType::Exposure: return {{"stops", number(-20, 20)}};
    case c::AdjustmentType::BrightnessContrast: return {{"brightness", number(-1, 1)}, {"contrast", number(-1, 1)}};
    case c::AdjustmentType::Levels: return {{"channels", array(object({{"inputBlack", number(0, 1)}, {"gamma", number(.1, 10)},
        {"inputWhite", number(0, 1)}, {"outputBlack", number(0, 1)}, {"outputWhite", number(0, 1)}},
        {"inputBlack", "gamma", "inputWhite", "outputBlack", "outputWhite"}), 4, 4)}};
    case c::AdjustmentType::Curves: return {{"channels", array(array(array(number(0, 1), 2, 2), 2, 16), 4, 4)}};
    case c::AdjustmentType::HueSaturation: return {{"ranges", array(object({{"hue", number(-180, 180)}, {"saturation", number(-1, 1)}, {"lightness", number(-1, 1)}}, {"hue", "saturation", "lightness"}), 7, 7)},
        {"colorize", boolean()}, {"colorizeHue", number(0, 360)}, {"colorizeSaturation", number(0, 1)}};
    case c::AdjustmentType::Vibrance: return {{"amount", number(-1, 1)}};
    case c::AdjustmentType::ColorBalance: return {{"tones", array(array(number(-1, 1), 3, 3), 3, 3)}, {"preserveLuminosity", boolean()}};
    case c::AdjustmentType::WarmthTint: return {{"warmth", number(-1, 1)}, {"tint", number(-1, 1)}};
    case c::AdjustmentType::BlackWhite: return {{"contributions", array(number(-1, 1), 6, 6)}, {"tintStrength", number(0, 1)}, {"tintRgba", array(integer(0, 255), 4, 4)}};
    case c::AdjustmentType::Invert: return {};
    }
    throw std::invalid_argument("Unknown adjustment type");
}
QJsonObject filterProperties(c::SpatialFilterType type)
{
    switch (type) {
    case c::SpatialFilterType::Gaussian: return {{"radiusX", number(0, c::maximumSpatialRadius)}, {"radiusY", number(0, c::maximumSpatialRadius)}};
    case c::SpatialFilterType::Motion: return {{"distance", number(0, c::maximumMotionDistance)}, {"angle", number(-180, 180)}};
    case c::SpatialFilterType::Lens: {
        QJsonArray blades {0, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
        return {{"radius", number(0, c::maximumSpatialRadius)}, {"blades", QJsonObject{{"type", "integer"}, {"enum", blades}}}, {"rotation", number(-180, 180)}};
    }
    }
    throw std::invalid_argument("Unknown filter type");
}
void validatePatch(const QJsonObject& properties, const QJsonObject& patch)
{
    const auto error = mcp::validateArguments(object({{"parameters", object(properties)}}), {{"parameters", patch}});
    require(error.isEmpty(), error);
}
QJsonObject parameterUnion(const std::vector<QJsonObject>& variants)
{
    QJsonArray choices;
    for (const auto& properties : variants) choices.append(object(properties));
    return {{"anyOf", choices}};
}
} // namespace

void McpController::registerLayerTools()
{
    const auto layerArgument = QJsonObject{{"layerId", identifier()}};
    const auto layersArgument = QJsonObject{{"layerIds", array(identifier(), 1)}};
    add("vulkana_font_list", "List installed font families and styles. No font installation or application settings are exposed.",
        {{"query", string("Optional case-insensitive substring")}}, {}, Access::Query, [](const QJsonObject& args) {
            QJsonArray fonts;
            for (const auto& family : QFontDatabase::families()) if (family.contains(args["query"].toString(), Qt::CaseInsensitive))
                fonts.append(QJsonObject{{"family", family}, {"styles", QJsonArray::fromStringList(QFontDatabase::styles(family))}});
            return QJsonObject{{"fonts", fonts}};
        });
    add("vulkana_layer_get", "Read one layer or container including editable text/shape properties, transform, masks and full effect/adjustment/filter settings. IDs are decimal strings.",
        {{"layerId", identifier()}}, {"layerId"}, Access::DocumentQuery, [this](const QJsonObject& args) {
            const auto target = id(args, "layerId");
            if (const auto* container = document().tree().container(target)) {
                auto data = result(false);
                data["layerId"] = id(target); data["name"] = text(container->name);
                data["type"] = container->kind == c::ContainerKind::Folder ? "folder" : container->kind == c::ContainerKind::Group ? "group" : "clipping_group";
                data["visible"] = container->visible; data["children"] = ids(container->children);
                return data;
            }
            const auto& item = layer(args);
            const auto& t = item.localToDocument;
            auto data = result(false);
            data["layerId"] = id(item.id); data["name"] = text(item.name); data["visible"] = item.visible;
            data["opacity"] = item.opacity; data["fillOpacity"] = item.fillOpacity; data["blendMode"] = text(c::blendModeId(item.blendMode));
            data["transform"] = QJsonArray{t.m00, t.m01, t.m02, t.m10, t.m11, t.m12, t.m20, t.m21, t.m22};
            const auto extent = c::layerGeometryExtent(item);
            data["width"] = double(extent.width); data["height"] = double(extent.height);
            const auto bounds = c::layerInteractionBounds(item);
            data["localBounds"] = QJsonObject{{"x", bounds.x}, {"y", bounds.y}, {"width", bounds.width}, {"height", bounds.height}};
            data["rasterOrigin"] = QJsonObject{{"x", item.rasterOrigin.x}, {"y", item.rasterOrigin.y}};
            const auto frame = c::composeAffine(t, {1, 0, bounds.x, 0, 1, bounds.y});
            if (const auto values = c::geometryValuesFromTransform(frame, {bounds.width, bounds.height})) data["transformValues"] = QJsonObject{{"centerX", values->center.x}, {"centerY", values->center.y}, {"scaleX", values->scaleX}, {"scaleY", values->scaleY}, {"rotation", values->rotationDegrees}};
            if (const auto* value = std::get_if<c::TextLayer>(&item.payload)) { data["type"] = "text"; data["text"] = textData(*value); }
            else if (const auto* value = std::get_if<c::ShapeLayer>(&item.payload)) { data["type"] = "shape"; data["shape"] = shapeData(*value); }
            else if (const auto* value = std::get_if<c::AdjustmentLayer>(&item.payload)) { data["type"] = "adjustment"; data["scope"] = value->scope == c::AdjustmentScope::ThisGroup ? "this_group" : "all_below"; }
            else data["type"] = "raster";
            data["mask"] = item.mask ? QJsonValue(QJsonObject{{"enabled", item.mask->enabled}, {"outside", item.mask->outside}, {"width", int(item.mask->coverage->extent().width)}, {"height", int(item.mask->coverage->extent().height)}}) : QJsonValue(QJsonValue::Null);
            const c::LayerEffectStack defaults;
            const auto& effects = item.effects ? *item.effects : defaults;
            QJsonArray effectItems;
            for (std::size_t i = 0; i < c::layerEffectCount; ++i) effectItems.append(QJsonObject{{"type", text(c::layerEffectIdentifier(c::LayerEffectType(i)))}, {"parameters", effectData(c::LayerEffectType(i), effects.items[i])}});
            data["effects"] = effectItems;
            data["pixelAlphaShapesEffects"] = effects.pixelAlphaShapesEffects;
            data["maskEffectsAfterRendering"] = effects.maskEffectsAfterRendering;
            auto adjustments = detail::encodeAdjustments(item.adjustments ? *item.adjustments : c::AdjustmentStack{}, item.id)["items"].toArray();
            auto filters = detail::encodeSpatialFilters(item.filters ? *item.filters : c::SpatialFilterStack{}, item.id)["items"].toArray();
            // Internal project ZIP paths are not useful as MCP file references.
            for (auto* entries : {&adjustments, &filters}) for (qsizetype i = 0; i < entries->size(); ++i) {
                auto entry = entries->at(i).toObject(); entry["hasCapturedSelection"] = entry.contains("mask"); entry.remove("mask"); (*entries)[i] = entry;
            }
            data["adjustments"] = adjustments; data["filters"] = filters;
            return data;
        });
    add("vulkana_layer_create", "Create an independent raster, text, shape, or adjustment layer and select it. Raster color defaults to transparent; x/y locate the layer-local origin in document pixels. Text styles affect the whole text. New layers go at the top level by default, or into an explicit parent. One undoable action.",
        {{"kind", choice({"raster", "text", "shape", "adjustment"})}, {"name", string()}, {"width", integer(1, 32768)}, {"height", integer(1, 32768)},
            {"x", number(-1000000, 1000000)}, {"y", number(-1000000, 1000000)}, {"color", colorSchema()}, {"text", object(textProperties())},
            {"shape", object(shapeProperties())}, {"scope", choice({"all_below", "this_group"})},
            {"parentId", identifier()}, {"index", integer(0, 4096)}}, {"kind"}, Access::Document,
        [this](const QJsonObject& args) {
            const auto kind = args["kind"].toString();
            require(kind == "text" || !args.contains("text"), "text parameters require kind=text");
            require(kind == "shape" || !args.contains("shape"), "shape parameters require kind=shape");
            require(kind == "adjustment" || !args.contains("scope"), "scope requires kind=adjustment");
            require(kind == "raster" || (!args.contains("width") && !args.contains("height") && !args.contains("color")), "width/height/color are raster parameters; use nested text or shape properties for other layer types");
            const auto name = args["name"].toString(kind == "raster" ? "Layer" : kind == "text" ? "Text" : kind == "shape" ? "Shape" : "Adjustment").trimmed();
            require(!name.isEmpty(), "Layer name must not be empty");
            const auto parent = args.contains("parentId") ? id(args, "parentId") : 0;
            const auto* children = document().tree().children(parent);
            require(children != nullptr, "Unknown parent container");
            const auto index = args.contains("index") ? std::size_t(args["index"].toInt()) : children->size();
            require(index <= children->size(), "Insertion index exceeds sibling count (bottom first)");
            c::Layer value;
            if (kind == "raster") {
                const auto canvas = document().canvas().extent;
                const c::Extent2u size{std::uint32_t(args["width"].toInt(int(canvas.width))), std::uint32_t(args["height"].toInt(int(canvas.height)))};
                require(std::uint64_t(size.width) * size.height <= 64ULL * 1024 * 1024, "Raster layer creation exceeds 64 megapixels");
                value = c::Layer::raster(name.toStdString(), std::make_shared<c::ContiguousRasterSurface>(size, args.contains("color") ? color(args["color"].toString()) : c::Rgba8{}));
            } else if (kind == "text") value = c::Layer::text(name.toStdString(), editText({}, args["text"].toObject()));
            else if (kind == "shape") value = c::Layer::shape(name.toStdString(), editShape({}, args["shape"].toObject()));
            else value = c::Layer::adjustment(name.toStdString(), args["scope"] == "this_group" ? c::AdjustmentScope::ThisGroup : c::AdjustmentScope::AllBelow);
            value.localToDocument.m02 = args["x"].toDouble(); value.localToDocument.m12 = args["y"].toDouble();
            const auto target = value.id;
            auto tree = document().tree();
            auto* siblings = tree.children(parent); siblings->insert(siblings->begin() + std::ptrdiff_t(index), target);
            const bool applied = execute(std::make_unique<c::LayerStructureCommand>("Add layer", document(), std::move(tree), std::vector<c::LayerId>{}, std::vector<c::Layer>{std::move(value)}, session().layerSelectionState(), selection({target})));
            require(applied, "Layer creation was rejected; no layer was added");
            auto data = result(applied); data["layerId"] = id(target); return data;
        });
    add("vulkana_layer_select", "Select explicit layers or containers; the last ID is primary. target=mask requires exactly one existing layer mask. Returns the selectedLayers, activeLayer and editingMask. changed reports a selection/target change; selection is session state, not an undo entry or document revision change.",
        {{"layerIds", array(identifier(), 1)}, {"target", choice({"content", "mask"})}}, {"layerIds"}, Access::Document,
        [this](const QJsonObject& args) {
            const auto values = selectedIds(document(), args, false);
            const bool mask = args["target"] == "mask";
            require(!mask || (values.size() == 1 && document().layer(values[0]) && document().layer(values[0])->mask), "Mask editing requires exactly one layer with a mask");
            const auto before = session().layerSelectionState();
            const bool beforeMask = session().editingLayerMask();
            session().setLayerSelection(values, values.back(), values.back()); session().setEditingLayerMask(mask);
            window_.synchronizeUi(true, false);
            auto data = result(before != session().layerSelectionState() || beforeMask != session().editingLayerMask());
            data["selectedLayers"] = ids(session().selectedLayers());
            data["activeLayer"] = session().activeLayer() ? QJsonValue(id(*session().activeLayer())) : QJsonValue::Null;
            data["editingMask"] = session().editingLayerMask();
            return data;
        });
    add("vulkana_layer_rename", "Rename one layer or container without changing its color label.",
        {{"layerId", identifier()}, {"name", string()}}, {"layerId", "name"}, Access::Document, [this](const QJsonObject& args) {
            const auto target = id(args, "layerId"); const auto name = args["name"].toString().trimmed();
            require(document().containsItem(target), "Unknown layer/container ID; query vulkana_get_state for current IDs");
            require(!name.isEmpty(), "arguments.name must contain a non-whitespace layer name");
            const auto* item = document().layer(target); const auto* container = document().tree().container(target);
            return result(execute(std::make_unique<c::SetItemMetadataCommand>(target, name.toStdString(), item ? c::ColorLabel(item->colorLabel) : container->colorLabel)));
        });
    add("vulkana_layer_delete", "Delete explicit layers/containers and their contents, in one undoable operation. The document must retain at least one content layer.", layersArgument, {"layerIds"}, Access::Document,
        [this](const QJsonObject& args) {
            const auto targets = selectedIds(document(), args); auto tree = document().tree();
            const auto removed = document().expandedLayers(targets);
            require(removed.size() < document().layers().size(), "A document must keep at least one content layer");
            for (const auto target : targets) removeSubtree(tree, target);
            return result(execute(std::make_unique<c::LayerStructureCommand>("Delete layers", document(), std::move(tree), removed, std::vector<c::Layer>{}, session().layerSelectionState(), selection({}))));
        }, true);
    add("vulkana_layer_duplicate", "Duplicate explicit layers/containers, preserving typed content and independent raster pixels. New copies become selected. One undoable action.", layersArgument, {"layerIds"}, Access::Document,
        [this](const QJsonObject& args) { const auto targets = selectedIds(document(), args); auto command = c::duplicateLayerItems(document(), selection(targets)); require(bool(command), "Selected items cannot be duplicated");
            const bool applied = execute(std::make_unique<SelectionRestoringCommand>(std::move(command), session().layerSelectionState())); auto data = result(applied); data["layerIds"] = ids(session().selectedLayers()); return data; });
    add("vulkana_layer_reparent", "Move items to a parent container or document root. Index is an insertion slot in the original bottom-first sibling order. Omitting parentId means root. One undoable action.",
        {{"layerIds", array(identifier(), 1)}, {"parentId", identifier()}, {"index", integer(0, 4096)}}, {"layerIds", "index"}, Access::Document,
        [this](const QJsonObject& args) { const auto targets = selectedIds(document(), args); auto tree = document().tree(); const auto parent = args.contains("parentId") ? id(args, "parentId") : 0;
            require(tree.children(parent) && std::size_t(args["index"].toInt()) <= tree.children(parent)->size(), "Invalid parent or insertion index");
            for (const auto target : targets) require(target != parent && !tree.isAncestor(target, parent), "Cannot move a container into itself or its descendants");
            const bool moved = tree.reparent(targets, {parent, std::size_t(args["index"].toInt())});
            return result(moved && execute(std::make_unique<c::LayerStructureCommand>("Reorder layers", document(), std::move(tree), std::vector<c::LayerId>{}, std::vector<c::Layer>{}, session().layerSelectionState(), selection(targets)))); });
    add("vulkana_layer_group", "Wrap consecutive sibling items in a folder, ordinary group, or clipping group. Clipping uses the bottommost child as its base. Layers keep their document-space positions.",
        {{"layerIds", array(identifier(), 1)}, {"kind", choice({"folder", "group", "clipping_group"})}, {"name", string()}}, {"layerIds", "kind"}, Access::Document,
        [this](const QJsonObject& args) { const auto targets = selectedIds(document(), args); auto tree = document().tree(); require(tree.consecutiveSiblings(targets), "Grouping requires consecutive siblings");
            const auto position = tree.placement(targets.front()); require(position.has_value(), "Missing layer placement");
            const auto target = c::makeLayerId(); const auto kind = args["kind"].toString();
            const auto name = args["name"].toString(kind == "folder" ? "Folder" : "Group").trimmed(); require(!name.isEmpty(), "Group name cannot be empty");
            auto* siblings = tree.children(position->parent);
            for (const auto child : targets) std::erase(*siblings, child);
            siblings->insert(siblings->begin() + std::ptrdiff_t(position->index), target);
            tree.containers.push_back({target, name.toStdString(), kind == "folder" ? c::ContainerKind::Folder : kind == "group" ? c::ContainerKind::Group : c::ContainerKind::ClippingMaskGroup, c::ColorLabel::None, targets});
            auto data = result(execute(std::make_unique<c::LayerStructureCommand>("Group layers", document(), std::move(tree), std::vector<c::LayerId>{}, std::vector<c::Layer>{}, session().layerSelectionState(), selection({target})))); data["layerId"] = id(target); return data; });
    add("vulkana_layer_ungroup", "Dissolve one folder/group while keeping all contents. Preserves effective hidden state using the editor's normal ungroup operation.", layerArgument, {"layerId"}, Access::Document,
        [this](const QJsonObject& args) { const auto target = id(args, "layerId"); require(document().tree().container(target), "Layer is not a container"); const auto revision = document().revision(); window_.dissolveLayerItem(target); require(!document().containsItem(target), "Container could not be dissolved"); return result(revision != document().revision()); });
    add("vulkana_layer_set_visibility", "Set visibility of one layer or container. A hidden ancestor still hides its children.",
        {{"layerId", identifier()}, {"visible", boolean()}}, {"layerId", "visible"}, Access::Document,
        [this](const QJsonObject& args) { const auto target = id(args, "layerId"); require(document().containsItem(target), "Unknown layer/container"); const std::array targets{target}; return result(execute(std::make_unique<c::SetItemsVisibilityCommand>(document(), targets, args["visible"].toBool() ? c::VisibilityOperation::Show : c::VisibilityOperation::Hide))); });
    add("vulkana_layer_set_opacity", "Set overall layer opacity from 0 to 1, including effects.", {{"layerId", identifier()}, {"opacity", number(0, 1)}}, {"layerId", "opacity"}, Access::Document,
        [this](const QJsonObject& args) { const auto target = layer(args).id; return result(execute(std::make_unique<c::SetLayerOpacityCommand>(target, float(args["opacity"].toDouble())))); });
    add("vulkana_layer_set_fill_opacity", "Set pixel opacity (Photoshop Fill) from 0 to 1; effects retain their opacity.", {{"layerId", identifier()}, {"opacity", number(0, 1)}}, {"layerId", "opacity"}, Access::Document,
        [this](const QJsonObject& args) { const auto target = layer(args).id; return result(execute(std::make_unique<c::SetLayerFillOpacityCommand>(target, float(args["opacity"].toDouble())))); });
    add("vulkana_layer_set_blend_mode", "Set a supported layer blend mode by its stable identifier.", {{"layerId", identifier()}, {"mode", choice(blendIds())}}, {"layerId", "mode"}, Access::Document,
        [this](const QJsonObject& args) { const auto target = layer(args).id; return result(execute(std::make_unique<c::SetLayerBlendModeCommand>(target, *c::blendModeFromId(args["mode"].toString().toStdString())))); });
    add("vulkana_layer_transform", "Transform one layer or multiple items as a unit. centerX/centerY are document-pixel coordinates; scaleX/scaleY are signed absolute scales relative to the transform frame; rotation is clockwise degrees. Omitted fields retain their current values. One undoable operation, with masks and effects following the layer.",
        {{"layerIds", array(identifier(), 1)}, {"centerX", number(-1000000, 1000000)}, {"centerY", number(-1000000, 1000000)},
            {"scaleX", number(-c::kMaximumLayerScale, c::kMaximumLayerScale)}, {"scaleY", number(-c::kMaximumLayerScale, c::kMaximumLayerScale)}, {"rotation", number(-360000, 360000)}}, {"layerIds"}, Access::Document,
        [this](const QJsonObject& args) { const auto targets = selectedIds(document(), args); c::LayerTransformSession edit(document(), targets); require(edit.active(), "Layer geometry cannot be transformed");
            auto values = edit.values();
            if (args.contains("centerX")) values.center.x = args["centerX"].toDouble();
            if (args.contains("centerY")) values.center.y = args["centerY"].toDouble();
            if (args.contains("scaleX")) values.scaleX = args["scaleX"].toDouble();
            if (args.contains("scaleY")) values.scaleY = args["scaleY"].toDouble();
            if (args.contains("rotation")) values.rotationDegrees = args["rotation"].toDouble();
            require(std::abs(values.scaleX) >= c::kMinimumLayerScale && std::abs(values.scaleY) >= c::kMinimumLayerScale, "Scale cannot be zero or near-zero");
            require(edit.setValues(values), "Transform is invalid over the layer's content and effects");
            const auto committed = edit.commit(session().history()); require(committed != c::TransformCommitResult::TargetUnavailable, "Transform target became unavailable"); changed(); return result(committed == c::TransformCommitResult::Committed); });
    add("vulkana_text_set", "Edit an existing text layer. Content replacement resets character runs. Each explicitly supplied formatting property applies to all characters; other mixed formatting is preserved. Alignment applies to every paragraph.",
        {{"layerId", identifier()}, {"properties", object(textProperties())}}, {"layerId", "properties"}, Access::Document,
        [this](const QJsonObject& args) { const auto& item = layer(args); const auto* value = std::get_if<c::TextLayer>(&item.payload); require(value, "Layer is not editable text");
            const auto next = editText(*value, args["properties"].toObject());
            if (next == *value) return result(false);
            const bool applied = execute(std::make_unique<c::TextEditCommand>(item.id, *value, next, c::TextEditHint{}, c::TextEditHint{}));
            require(applied, "Text changes are not valid for this layer geometry"); return result(applied); });
    add("vulkana_shape_set", "Edit existing shape geometry/paint in layer-local pixels without rasterizing it. For lines/polygons supply points within the size frame; resizing does not silently rescale those points.",
        {{"layerId", identifier()}, {"properties", object(shapeProperties())}}, {"layerId", "properties"}, Access::Document,
        [this](const QJsonObject& args) { const auto& item = layer(args); const auto* value = std::get_if<c::ShapeLayer>(&item.payload); require(value, "Layer is not an editable shape");
            const auto next = editShape(*value, args["properties"].toObject());
            if (next == *value) return result(false);
            const bool applied = execute(std::make_unique<c::SetShapeCommand>(item.id, *value, next));
            require(applied, "Shape changes are not valid for this layer geometry"); return result(applied); });
    add("vulkana_layer_mask", "Add a reveal-all/hide-all mask, capture the current selection, remove/invert an existing mask, or set enabled state. Existing masks are never replaced implicitly. from_selection preserves active selection and document coordinates. One undoable action.",
        {{"layerId", identifier()}, {"operation", choice({"reveal_all", "hide_all", "from_selection", "remove", "invert", "enable", "disable"})}}, {"layerId", "operation"}, Access::Document,
        [this](const QJsonObject& args) {
            const auto& item = layer(args); const auto operation = args["operation"].toString(); const auto before = item.mask; c::LayerMaskState after;
            if (operation == "remove") require(bool(before), "Layer has no mask");
            else if (operation == "invert" || operation == "enable" || operation == "disable") {
                require(bool(before), "Layer has no mask"); auto mask = std::make_shared<c::LayerMask>(*before);
                if (operation == "invert") { mask->coverage = mask->coverage->inverted(); mask->outside = std::uint8_t(255 - mask->outside); }
                else mask->enabled = operation == "enable";
                after = mask;
            } else {
                require(!before, "Layer already has a mask; remove it explicitly before creating another");
                const bool fromSelection = operation == "from_selection";
                require(!fromSelection || (document().selection() && !document().selection()->bounds().empty()), "No nonempty active selection");
                auto bounds = c::layerMaskEditingBounds(item, document().canvas().extent);
                if (bounds.empty()) {
                    if (const auto* shape = std::get_if<c::ShapeLayer>(&item.payload)) bounds = QtShapeRenderService().documentBounds(*shape, {});
                    else if (const auto* value = std::get_if<c::TextLayer>(&item.payload)) bounds = QtTextLayout(*value).documentBounds({});
                }
                require(!bounds.empty(), "Layer has no editable mask bounds");
                const double x = std::floor(bounds.x), y = std::floor(bounds.y), w = std::ceil(bounds.right()) - x, h = std::ceil(bounds.bottom()) - y;
                require(std::isfinite(w) && std::isfinite(h) && w >= 1 && h >= 1 && w <= 32768 && h <= 32768 && w * h <= 64.0 * 1024 * 1024, "Layer mask exceeds the supported 64-megapixel frame");
                const c::Extent2u size{std::uint32_t(w), std::uint32_t(h)}; auto mask = std::make_shared<c::LayerMask>(); mask->localToMask = {1, 0, -x, 0, 1, -y};
                if (fromSelection) {
                    std::vector<std::uint8_t> coverage(std::size_t(size.width) * size.height);
                    auto selected = std::make_shared<c::LayerMask>(c::LayerMask{document().selection(), {}, 0, true});
                    for (std::uint32_t row = 0; row < size.height; ++row) for (std::uint32_t col = 0; col < size.width; ++col) coverage[std::size_t(row) * size.width + col] = std::uint8_t(std::lround(c::layerMaskCoverage(selected, item.localToDocument.map({x + col + .5, y + row + .5})) * 255));
                    mask->coverage = c::SelectionMask::fromR8(size, coverage, size.width); mask->outside = 0;
                } else { mask->outside = operation == "hide_all" ? 0 : 255; mask->coverage = c::SelectionMask::filled(size, mask->outside); }
                after = mask;
            }
            return result(execute(std::make_unique<c::LayerMaskCommand>(item.id, before, after, "Layer mask")));
        }, true);

    std::vector<QJsonObject> effectVariants, adjustmentVariants, filterVariants;
    QJsonArray effectTypes, adjustmentTypes, filterTypes;
    for (std::size_t i = 0; i < c::layerEffectCount; ++i) { effectVariants.push_back(effectProperties(c::LayerEffectType(i))); effectTypes.append(text(c::layerEffectIdentifier(c::LayerEffectType(i)))); }
    for (const auto type : c::allAdjustmentTypes) { adjustmentVariants.push_back(adjustmentProperties(type)); adjustmentTypes.append(text(c::adjustmentIdentifier(type))); }
    for (const auto type : c::allSpatialFilterTypes) { filterVariants.push_back(filterProperties(type)); filterTypes.append(text(c::spatialFilterIdentifier(type))); }
    add("vulkana_effect_catalog", "Discover every layer effect with exact parameter schema and defaults. Sizes are layer-local pixels; opacity/spread use 0..1; gradient stop positions must be sorted. Effects are not available on adjustment layers.", {}, {}, Access::Query,
        [](const QJsonObject&) { QJsonArray values; for (std::size_t i = 0; i < c::layerEffectCount; ++i) { const auto type = c::LayerEffectType(i); values.append(QJsonObject{{"type", text(c::layerEffectIdentifier(type))}, {"name", text(c::layerEffectName(type))}, {"schema", object(effectProperties(type))}, {"defaults", effectData(type, c::defaultLayerEffect(type))}}); } return QJsonObject{{"effects", values}}; });
    add("vulkana_effect_set", "Enable or patch one layer effect while preserving every other effect and all unspecified settings. Enabled defaults to true unless supplied explicitly. Query vulkana_effect_catalog for type-specific properties.",
        {{"layerId", identifier()}, {"type", choice(effectTypes)}, {"parameters", parameterUnion(effectVariants)}}, {"layerId", "type", "parameters"}, Access::Document,
        [this](const QJsonObject& args) { const auto& item = layer(args); require(!std::holds_alternative<c::AdjustmentLayer>(item.payload), "Adjustment layers cannot have effects"); const auto type = *c::layerEffectFromIdentifier(args["type"].toString().toStdString()); const auto patch = args["parameters"].toObject(); validatePatch(effectProperties(type), patch);
            auto next = std::make_shared<c::LayerEffectStack>(item.effects ? *item.effects : c::LayerEffectStack{}); auto& effect = next->items[std::size_t(type)]; if (!patch.contains("enabled")) effect.enabled = true; patchEffect(effect, patch);
            require(c::validLayerEffects(*next), "Invalid effect parameters or unsorted gradient stops");
            if (c::equivalentLayerEffects(item.effects, next)) return result(false);
            const bool applied = execute(std::make_unique<c::SetLayerEffectsCommand>(item.id, item.effects, next));
            require(applied, "Effects are not valid over this layer's transformed geometry"); return result(applied); });
    add("vulkana_effect_remove", "Disable one effect, retaining its settings for later re-enabling. Other effects are preserved.", {{"layerId", identifier()}, {"type", choice(effectTypes)}}, {"layerId", "type"}, Access::Document,
        [this](const QJsonObject& args) { const auto& item = layer(args); auto next = std::make_shared<c::LayerEffectStack>(item.effects ? *item.effects : c::LayerEffectStack{}); const auto type = *c::layerEffectFromIdentifier(args["type"].toString().toStdString()); next->items[std::size_t(type)].enabled = false; return result(execute(std::make_unique<c::SetLayerEffectsCommand>(item.id, item.effects, next))); });
    add("vulkana_effect_blending", "Set layer-wide effect geometry options; neither changes layer opacity or pixel opacity.",
        {{"layerId", identifier()}, {"pixelAlphaShapesEffects", boolean()}, {"maskEffectsAfterRendering", boolean()}}, {"layerId"}, Access::Document,
        [this](const QJsonObject& args) { const auto& item = layer(args); require(!std::holds_alternative<c::AdjustmentLayer>(item.payload), "Adjustment layers cannot have effects"); auto next = std::make_shared<c::LayerEffectStack>(item.effects ? *item.effects : c::LayerEffectStack{});
            if (args.contains("pixelAlphaShapesEffects")) next->pixelAlphaShapesEffects = args["pixelAlphaShapesEffects"].toBool();
            if (args.contains("maskEffectsAfterRendering")) next->maskEffectsAfterRendering = args["maskEffectsAfterRendering"].toBool();
            return result(execute(std::make_unique<c::SetLayerEffectsCommand>(item.id, item.effects, next))); });
    add("vulkana_adjustment_catalog", "Discover adjustment schemas/defaults. Channel arrays are composite/red/green/blue; hue ranges are master/reds/yellows/greens/cyans/blues/magentas. Existing captured-selection masks are preserved when updating parameters.", {}, {}, Access::Query,
        [](const QJsonObject&) { QJsonArray values; const auto items = detail::encodeAdjustments(c::AdjustmentStack{}, 1)["items"].toArray(); for (const auto type : c::allAdjustmentTypes) values.append(QJsonObject{{"type", text(c::adjustmentIdentifier(type))}, {"name", text(c::adjustmentName(type))}, {"schema", object(adjustmentProperties(type))}, {"defaults", items[int(type)].toObject()["parameters"]}}); return QJsonObject{{"adjustments", values}}; });
    add("vulkana_adjustment_set", "Enable or patch one adjustment without replacing other adjustments or captured masks. Optional enabled defaults true. Arrays replace the entire specified array; omitted parameters retain current values.",
        {{"layerId", identifier()}, {"type", choice(adjustmentTypes)}, {"parameters", parameterUnion(adjustmentVariants)}, {"enabled", boolean()}}, {"layerId", "type", "parameters"}, Access::Document,
        [this](const QJsonObject& args) { const auto& item = layer(args); const auto type = *c::adjustmentFromIdentifier(args["type"].toString().toStdString()); const auto patch = args["parameters"].toObject(); validatePatch(adjustmentProperties(type), patch);
            const auto before = item.adjustments ? *item.adjustments : c::AdjustmentStack{}; auto encoded = detail::encodeAdjustments(before, item.id); auto entries = encoded["items"].toArray();
            for (qsizetype i = 0; i < entries.size(); ++i) { auto entry = entries[i].toObject(); entry.remove("mask"); if (i == int(type)) { auto parameters = entry["parameters"].toObject(); for (auto it = patch.begin(); it != patch.end(); ++it) parameters[it.key()] = it.value(); entry["parameters"] = parameters; entry["enabled"] = args["enabled"].toBool(true); } entries[i] = entry; }
            encoded["items"] = entries; auto next = detail::decodeAdjustments(encoded, item.id).stack;
            for (std::size_t i = 0; i < c::adjustmentCount; ++i) next->items[i].mask = before.items[i].mask;
            return result(execute(std::make_unique<c::SetLayerAdjustmentsCommand>(item.id, item.adjustments, next))); });
    add("vulkana_adjustment_remove", "Disable one adjustment, retaining its parameters and captured mask.", {{"layerId", identifier()}, {"type", choice(adjustmentTypes)}}, {"layerId", "type"}, Access::Document,
        [this](const QJsonObject& args) { const auto& item = layer(args); auto next = std::make_shared<c::AdjustmentStack>(item.adjustments ? *item.adjustments : c::AdjustmentStack{}); next->items[std::size_t(*c::adjustmentFromIdentifier(args["type"].toString().toStdString()))].enabled = false; return result(execute(std::make_unique<c::SetLayerAdjustmentsCommand>(item.id, item.adjustments, next))); });
    add("vulkana_adjustment_scope", "Choose whether an adjustment layer affects all lower content or lower content in its current group. Does not apply to raster/text/shape layers.",
        {{"layerId", identifier()}, {"scope", choice({"all_below", "this_group"})}}, {"layerId", "scope"}, Access::Document,
        [this](const QJsonObject& args) { const auto& item = layer(args); const auto* value = std::get_if<c::AdjustmentLayer>(&item.payload); require(value, "Layer is not an adjustment layer");
            return result(execute(std::make_unique<c::SetAdjustmentScopeCommand>(item.id, value->scope, args["scope"] == "this_group" ? c::AdjustmentScope::ThisGroup : c::AdjustmentScope::AllBelow))); });
    add("vulkana_filter_catalog", "Discover Gaussian, motion and lens blur parameters/defaults. Radii/distances are layer-local pixels. Updating a filter preserves its captured-selection mask.", {}, {}, Access::Query,
        [](const QJsonObject&) { QJsonArray values; const auto items = detail::encodeSpatialFilters(c::SpatialFilterStack{}, 1)["items"].toArray(); for (const auto type : c::allSpatialFilterTypes) values.append(QJsonObject{{"type", text(c::spatialFilterIdentifier(type))}, {"name", text(c::spatialFilterName(type))}, {"schema", object(filterProperties(type))}, {"defaults", items[int(type)].toObject()["parameters"]}}); return QJsonObject{{"filters", values}}; });
    add("vulkana_filter_set", "Enable or patch one spatial filter without replacing other filters or captured masks. enabled defaults true; preserveAlpha is optional. Filters are unavailable on adjustment layers.",
        {{"layerId", identifier()}, {"type", choice(filterTypes)}, {"parameters", parameterUnion(filterVariants)}, {"enabled", boolean()}, {"preserveAlpha", boolean()}}, {"layerId", "type", "parameters"}, Access::Document,
        [this](const QJsonObject& args) { const auto& item = layer(args); require(!std::holds_alternative<c::AdjustmentLayer>(item.payload), "Adjustment layers cannot have spatial filters"); const auto type = *c::spatialFilterFromIdentifier(args["type"].toString().toStdString()); const auto patch = args["parameters"].toObject(); validatePatch(filterProperties(type), patch);
            const auto before = item.filters ? *item.filters : c::SpatialFilterStack{}; auto encoded = detail::encodeSpatialFilters(before, item.id); auto entries = encoded["items"].toArray();
            for (qsizetype i = 0; i < entries.size(); ++i) { auto entry = entries[i].toObject(); entry.remove("mask"); if (i == int(type)) { auto parameters = entry["parameters"].toObject(); for (auto it = patch.begin(); it != patch.end(); ++it) parameters[it.key()] = it.value(); entry["parameters"] = parameters; entry["enabled"] = args["enabled"].toBool(true); if (args.contains("preserveAlpha")) entry["preserveAlpha"] = args["preserveAlpha"]; } entries[i] = entry; }
            encoded["items"] = entries; auto next = detail::decodeSpatialFilters(encoded, item.id).stack; for (std::size_t i = 0; i < c::spatialFilterCount; ++i) next->items[i].mask = before.items[i].mask;
            if (c::equivalentSpatialFilters(item.filters, next)) return result(false);
            const bool applied = execute(std::make_unique<c::SetLayerFiltersCommand>(item.id, item.filters, next));
            require(applied, "Filters are not valid over this layer's transformed geometry"); return result(applied); });
    add("vulkana_filter_remove", "Disable one spatial filter, retaining its parameters and captured mask.", {{"layerId", identifier()}, {"type", choice(filterTypes)}}, {"layerId", "type"}, Access::Document,
        [this](const QJsonObject& args) { const auto& item = layer(args); auto next = std::make_shared<c::SpatialFilterStack>(item.filters ? *item.filters : c::SpatialFilterStack{}); next->items[std::size_t(*c::spatialFilterFromIdentifier(args["type"].toString().toStdString()))].enabled = false; return result(execute(std::make_unique<c::SetLayerFiltersCommand>(item.id, item.filters, next))); });
}
} // namespace imageeditor::ui
