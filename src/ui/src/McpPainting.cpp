#include "imageeditor/ui/McpController.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/FlattenedDocument.hpp"
#include "imageeditor/core/BasicPixelBrushStroke.hpp"
#include "imageeditor/core/ColorSampler.hpp"
#include "imageeditor/core/EllipseCoverageRasterizer.hpp"
#include "imageeditor/core/FillOperation.hpp"
#include "imageeditor/core/LayerGeometry.hpp"
#include "imageeditor/core/LayerMaskEdit.hpp"
#include "imageeditor/core/PolygonCoverageRasterizer.hpp"
#include "imageeditor/core/SelectionCommands.hpp"
#include "McpToolHelpers.hpp"

#include <QElapsedTimer>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace imageeditor::ui {
namespace {
using namespace mcpdetail;
constexpr int maximumSamples = 4096;
constexpr std::uint64_t maximumPixels = 64ULL * 1024 * 1024;
constexpr qint64 operationMilliseconds = 5000;

// The ordinary brush engine remains authoritative. A wrapper only limits a
// single remote request; unwinding the stroke restores its journalled pixels.
class BoundedBrushEngine final : public core::IBrushEngine, private core::BrushDabSink {
public:
    bool beginStroke(const core::BrushSettings& settings,
        const core::NormalizedPointerSample& sample, core::BrushDabSink& sink) override
    {
        timer_.start(); count_ = 0; sink_ = &sink;
        return engine_.beginStroke(settings, sample, *this);
    }
    bool appendSample(const core::NormalizedPointerSample& sample, core::BrushDabSink&) override
    { checkTime(); return engine_.appendSample(sample, *this); }
    bool endStroke(const core::NormalizedPointerSample& sample, core::BrushDabSink&) override
    { checkTime(); return engine_.endStroke(sample, *this); }
    void cancelStroke() noexcept override { engine_.cancelStroke(); }
    bool active() const noexcept override { return engine_.active(); }
private:
    void checkTime() const
    {
        if (timer_.elapsed() > operationMilliseconds)
            throw std::runtime_error("Brush request exceeded five seconds; stroke rolled back. Use a shorter path or smaller brush.");
    }
    void emitDab(const core::BrushDab& dab) override
    {
        checkTime();
        if (++count_ > 65536)
            throw std::runtime_error("Brush request exceeded 65536 dabs; stroke rolled back. Use a shorter path or larger spacing.");
        sink_->emitDab(dab);
    }
    core::BasicPixelBrushEngine engine_;
    core::BrushDabSink* sink_ {};
    QElapsedTimer timer_;
    std::uint64_t count_ {};
};

QJsonObject pointSchema(bool pressure = false)
{
    QJsonObject properties {{"x", number(-1000000, 1000000, "Document-pixel X, independent of zoom.")},
        {"y", number(-1000000, 1000000, "Document-pixel Y, increasing downwards.")}};
    if (pressure) properties.insert("pressure", number(0, 1, "Pen pressure; defaults to 1."));
    return object(properties, {"x", "y"});
}
core::Vec2d point(const QJsonObject& value)
{ return {value.value("x").toDouble(), value.value("y").toDouble()}; }

QJsonObject brushSettings(const core::BrushSettings& value)
{
    return {{"size", value.sizePixels}, {"hardness", value.hardness}, {"opacity", value.opacity},
        {"flow", value.flow}, {"spacing", value.spacingPercent}, {"pressureToSize", value.pressureToSize},
        {"pressureToFlow", value.pressureToFlow}, {"tipId", QString::fromStdString(value.tip.assetId)},
        {"grainId", QString::fromStdString(value.grain.assetId)}};
}

void paintTarget(const core::Layer& value, bool mask)
{
    require(bool(value.localToDocument.inverted()), "Layer transform is not invertible");
    if (mask) require(bool(value.mask), "Layer has no mask; add one explicitly before painting it");
    else require(std::holds_alternative<core::RasterLayer>(value.payload), "Pixel painting requires a raster layer");
}

core::SelectionOperation selectionOperation(const QJsonObject& args)
{
    const auto value = args.value("operation").toString("replace");
    if (value == "add") return core::SelectionOperation::Add;
    if (value == "subtract") return core::SelectionOperation::Subtract;
    if (value == "intersect") return core::SelectionOperation::Intersect;
    return core::SelectionOperation::Replace;
}

QJsonObject selectionData(const core::Document& doc)
{
    const auto selection = doc.selection();
    QJsonObject data {{"unrestricted", !selection}, {"selectionRevision", id(doc.selectionRevision())}};
    if (selection) {
        const auto bounds = selection->bounds();
        data.insert("empty", bounds.empty());
        data.insert("bounds", QJsonObject {{"x", bounds.x}, {"y", bounds.y},
            {"width", bounds.width}, {"height", bounds.height}});
    }
    return data;
}

core::SelectionState rasterize(core::Extent2u extent, core::SelectionCoverageRasterizer& rasterizer)
{
    QElapsedTimer timer; timer.start();
    while (!rasterizer.finished()) {
        rasterizer.step();
        require(timer.elapsed() <= operationMilliseconds,
            "Selection request exceeded five seconds; no selection changed. Use simpler or smaller geometry.");
    }
    return core::SelectionMask::fromR8Region(extent, rasterizer.region(), rasterizer.coverage(), rasterizer.stride());
}

core::SelectionState alphaSelection(const core::Document& doc, const core::Layer& layer)
{
    const auto extent = doc.canvas().extent;
    require(std::uint64_t(extent.width) * extent.height <= maximumPixels,
        "Layer transparency selection supports canvases up to 64 megapixels");
    require(!std::holds_alternative<core::AdjustmentLayer>(layer.payload),
        "Adjustment layers have no intrinsic pixel transparency");
    auto source = layer;
    source.visible = true; source.opacity = 1; source.fillOpacity = 1;
    source.blendMode = core::BlendMode::Normal;
    source.mask.reset(); source.adjustments.reset(); source.filters.reset();
    source.filterCache.reset(); source.effects.reset(); source.effectCache.reset();
    if (!std::holds_alternative<core::RasterLayer>(source.payload))
        source.renderCache = prepareDocumentSampleCache(source, maximumPixels);
    const auto surface = core::renderedSurface(source);
    require(bool(surface), "Unable to prepare intrinsic layer transparency");
    const auto size = surface->extent();
    const auto mapping = core::renderTransform(source);
    require(bool(mapping.inverted()) && mapping.validOver({0, 0, double(size.width), double(size.height)}),
        "Invalid layer transparency transform");
    double left = extent.width, top = extent.height, right = 0, bottom = 0;
    for (const auto p : std::array<core::Vec2d, 4>{{{0, 0}, {double(size.width), 0},
             {0, double(size.height)}, {double(size.width), double(size.height)}}}) {
        const auto mapped = mapping.map(p);
        left = std::min(left, mapped.x); top = std::min(top, mapped.y);
        right = std::max(right, mapped.x); bottom = std::max(bottom, mapped.y);
    }
    const int x0 = int(std::floor(std::clamp(left - 1, 0., double(extent.width))));
    const int y0 = int(std::floor(std::clamp(top - 1, 0., double(extent.height))));
    const int x1 = int(std::ceil(std::clamp(right + 1, 0., double(extent.width))));
    const int y1 = int(std::ceil(std::clamp(bottom + 1, 0., double(extent.height))));
    if (x1 <= x0 || y1 <= y0) return {};
    const core::RectI region {x0, y0, x1 - x0, y1 - y0};
    const std::array<const core::Layer*, 1> sources {&source};
    core::PinnedDocumentSampler sampler(sources, extent);
    std::vector<core::PremultipliedColor> row(std::size_t(region.width));
    std::vector<std::byte> scratch(row.size() * 4);
    std::vector<std::uint8_t> coverage(row.size() * std::size_t(region.height));
    QElapsedTimer timer; timer.start();
    bool nonempty = false;
    for (int y = 0; y < region.height; ++y) {
        require(timer.elapsed() <= operationMilliseconds,
            "Layer transparency request exceeded five seconds; selection was not changed");
        sampler.sampleRow(region.x, region.y + y, row, scratch);
        for (std::size_t x = 0; x < row.size(); ++x) {
            const auto value = std::uint8_t(std::clamp(std::lround(row[x][3] * 255), 0L, 255L));
            coverage[std::size_t(y) * row.size() + x] = value;
            nonempty |= value != 0;
        }
    }
    if (!nonempty) return {}; // Empty sources preserve the old selection and redo.
    return core::SelectionMask::fromR8Region(extent, region, coverage, row.size());
}
} // namespace

void McpController::registerPaintingTools()
{
    using namespace mcpdetail;
    add("vulkana_brush_presets", "List available brush presets and their default settings. Presets are read-only; painting overrides apply to one stroke, not preferences.",
        {}, {}, Access::Query, [this](const QJsonObject&) {
            QJsonArray presets;
            for (const auto& preset : window_.brushPresets_) {
                auto entry = brushSettings(preset.settings);
                entry.insert("id", QString::fromStdString(preset.id));
                entry.insert("name", QString::fromStdString(preset.displayName));
                const auto unavailable = window_.brushAssets_->unavailableMessage(preset.settings);
                entry.insert("available", unavailable.isEmpty());
                if (!unavailable.isEmpty()) entry.insert("reason", unavailable);
                presets.append(entry);
            }
            return QJsonObject {{"presets", presets}, {"defaultPreset", "builtin.preset.hard-round.v1"},
                {"maximumPathPoints", maximumSamples}, {"maximumDabs", 65536}, {"timeBudgetMs", int(operationMilliseconds)}};
        });

    add("vulkana_brush_stroke", "Paint one undoable stroke using Vulkana's brush engine. Coordinates are document pixels, not viewport pixels. Honors selection, layer transforms and crop. Pixel target requires raster; mask target requires an existing mask. Erasing a mask paints black. Preserves active layer, tool and brush settings. Paths over 4096 points, 65536 dabs or five seconds fail with rollback.",
        {{"layerId", identifier()}, {"points", array(pointSchema(true), 1, maximumSamples)},
            {"presetId", string("Default: builtin.preset.hard-round.v1; query vulkana_brush_presets.")},
            {"target", choice({"pixels", "mask"}, "Default pixels.")},
            {"mode", choice({"paint", "erase"}, "Default paint.")},
            {"color", colorSchema()}, {"size", number(.25, 1000, "Brush diameter in document pixels.")},
            {"hardness", number(0, 1)}, {"opacity", number(0, 1)}, {"flow", number(0, 1)},
            {"spacing", number(1, 200, "Dab spacing as percent of brush diameter.")},
            {"pressureToSize", boolean()}, {"pressureToFlow", boolean()}},
        {"layerId", "points"}, Access::Document, [this](const QJsonObject& args) {
            const auto& target = layer(args);
            const bool mask = args.value("target").toString("pixels") == "mask";
            const bool erase = args.value("mode").toString("paint") == "erase";
            paintTarget(target, mask);
            const auto presetId = args.value("presetId").toString("builtin.preset.hard-round.v1").toStdString();
            const auto preset = std::find_if(window_.brushPresets_.begin(), window_.brushPresets_.end(),
                [&](const auto& entry) { return entry.id == presetId; });
            require(preset != window_.brushPresets_.end(), "Unknown brush preset ID");
            auto settings = preset->settings;
            for (const auto& [name, field] : std::array<std::pair<const char*, double*>, 5>{{
                     {"size", &settings.sizePixels}, {"hardness", &settings.hardness},
                     {"opacity", &settings.opacity}, {"flow", &settings.flow}, {"spacing", &settings.spacingPercent}}})
                if (args.contains(QLatin1String(name))) *field = args.value(QLatin1String(name)).toDouble();
            if (args.contains("pressureToSize")) settings.pressureToSize = args.value("pressureToSize").toBool();
            if (args.contains("pressureToFlow")) settings.pressureToFlow = args.value("pressureToFlow").toBool();
            settings.foreground = args.contains("color") ? color(args.value("color").toString()) : session().foregroundColor();
            if (mask && erase) settings.foreground = {0, 0, 0, 255};
            std::vector<core::NormalizedPointerSample> points;
            const auto path = args.value("points").toArray();
            points.reserve(std::size_t(path.size()));
            double length = 0;
            for (const auto& value : path) {
                const auto entry = value.toObject();
                const auto position = point(entry);
                if (!points.empty()) length += std::hypot(position.x - points.back().documentPosition.x,
                    position.y - points.back().documentPosition.y);
                points.push_back({.documentPosition = position,
                    .timestampMicroseconds = std::uint64_t(points.size() + 1) * 10000,
                    .pressure = entry.value("pressure").toDouble(1), .pointerType = core::PointerType::Pen,
                    .buttons = core::PointerButtonPrimary});
            }
            // Bound even zero-opacity/zero-pressure paths that emit no dabs.
            require(length <= 1000000, "Stroke path exceeds one million document pixels; split it into smaller strokes");
            QString assetError;
            require(window_.brushAssets_->prepareSettings(settings, &assetError), assetError);
            std::unique_ptr<core::LayerMaskEdit> maskEdit;
            if (mask) maskEdit = std::make_unique<core::LayerMaskEdit>(document(), target.id, "Paint layer mask");
            core::RasterEditTransactionOptions options; options.coverageValues = mask;
            core::BasicPixelBrushStroke stroke(mask ? maskEdit->proxy() : document(), target.id, settings,
                mask ? core::BrushCompositeMode::MaskCoverage : erase ? core::BrushCompositeMode::Erase : core::BrushCompositeMode::Paint,
                std::make_unique<BoundedBrushEngine>(), std::unique_ptr<core::IBrushTip>{},
                std::unique_ptr<core::IBrushGrain>{}, window_.brushAssets_.get(), options);
            require(stroke.valid() && stroke.begin(points.front()), "Unable to begin stroke; layer geometry or raster work limit rejected it");
            for (std::size_t i = 1; i + 1 < points.size(); ++i)
                require(stroke.append(points[i]), "Stroke rejected; no pixels were committed");
            auto committed = stroke.end(points.back(), mask ? maskEdit->provisionalHistory() : session().history());
            require(committed == core::RasterEditCommitResult::Committed || committed == core::RasterEditCommitResult::NoChanges,
                "Stroke could not commit; no pixels were committed");
            if (mask) committed = maskEdit->commit(session().history());
            require(committed == core::RasterEditCommitResult::Committed || committed == core::RasterEditCommitResult::NoChanges,
                "Mask stroke could not commit");
            if (committed == core::RasterEditCommitResult::Committed) changed(false);
            auto data = result(committed == core::RasterEditCommitResult::Committed);
            data.insert("layerId", id(target.id));
            data.insert("dabs", double(stroke.stats().emittedDabs));
            data.insert("changedPixels", double(stroke.stats().changedPixels));
            return data;
        });

    add("vulkana_fill", "Fill a raster layer or existing mask in one undoable operation. mode=selection fills active selection, or all layer pixels when unrestricted; mode=contiguous flood-fills from seed with tolerance. Honors transforms/crop/selection. Keeps editor tool settings. Work over five seconds rolls back.",
        {{"layerId", identifier()}, {"target", choice({"pixels", "mask"})},
            {"mode", choice({"selection", "contiguous"})}, {"color", colorSchema()},
            {"opacity", number(0, 1)}, {"tolerance", integer(0, 255)}, {"seed", pointSchema()}},
        {"layerId", "color"}, Access::Document, [this](const QJsonObject& args) {
            const auto& target = layer(args);
            const bool mask = args.value("target").toString("pixels") == "mask";
            paintTarget(target, mask);
            core::FillOptions options;
            options.mode = args.value("mode").toString("selection") == "contiguous"
                ? core::FillMode::Contiguous : core::FillMode::SelectionOrLayer;
            require(options.mode != core::FillMode::Contiguous || args.contains("seed"), "Contiguous fill requires seed");
            require(options.mode == core::FillMode::Contiguous || (!args.contains("seed") && !args.contains("tolerance")),
                "seed and tolerance only apply to contiguous fill");
            options.color = color(args.value("color").toString());
            options.opacity = args.value("opacity").toDouble(1);
            options.tolerance = std::uint8_t(args.value("tolerance").toInt(16));
            options.seed = point(args.value("seed").toObject());
            options.coverageValues = mask;
            std::unique_ptr<core::LayerMaskEdit> maskEdit;
            if (mask) maskEdit = std::make_unique<core::LayerMaskEdit>(document(), target.id, "Fill layer mask");
            core::FillOperation fill(mask ? maskEdit->proxy() : document(), target.id, options);
            QElapsedTimer timer; timer.start();
            while (fill.state() == core::FillState::Discovering || fill.state() == core::FillState::Applying) {
                fill.step();
                require(timer.elapsed() <= operationMilliseconds, "Fill exceeded five seconds; rolled back. Select a smaller region.");
            }
            require(fill.state() == core::FillState::Ready,
                QStringLiteral("Fill failed: ") + QString::fromUtf8(fill.error().data(), qsizetype(fill.error().size())));
            auto committed = fill.commit(mask ? maskEdit->provisionalHistory() : session().history());
            require(committed == core::RasterEditCommitResult::Committed || committed == core::RasterEditCommitResult::NoChanges,
                "Fill could not commit");
            if (mask) committed = maskEdit->commit(session().history());
            require(committed == core::RasterEditCommitResult::Committed || committed == core::RasterEditCommitResult::NoChanges,
                "Mask fill could not commit");
            if (committed == core::RasterEditCommitResult::Committed) changed(false);
            return result(committed == core::RasterEditCommitResult::Committed);
        });

    add("vulkana_selection", "Change the pixel selection with one undo step. Shapes use document-pixel coordinates and optional replace/add/subtract/intersect. 'none' removes restrictions (painting can affect the whole layer); it is different from an empty selection. Existing layers are unchanged.",
        {{"shape", choice({"rectangle", "ellipse", "polygon", "all", "none", "invert"})},
            {"operation", choice({"replace", "add", "subtract", "intersect"})},
            {"from", pointSchema()}, {"to", pointSchema()}, {"points", array(pointSchema(), 3, 4096)}},
        {"shape"}, Access::Document, [this](const QJsonObject& args) {
            const auto shape = args.value("shape").toString();
            const auto extent = document().canvas().extent;
            core::SelectionState selection;
            if (shape == "none" || shape == "all" || shape == "invert") {
                require(!args.contains("from") && !args.contains("to") && !args.contains("points") && !args.contains("operation"),
                    "all, none and invert do not accept geometry or combination mode");
                if (shape == "all") selection = core::SelectionMask::filled(extent, 255);
                if (shape == "invert") selection = document().selection()
                    ? document().selection()->inverted() : core::SelectionMask::filled(extent, 255);
            } else {
                if (shape == "polygon") {
                    require(args.contains("points") && !args.contains("from") && !args.contains("to"),
                        "Polygon selection needs points, not from/to");
                    std::vector<core::Vec2d> points;
                    for (const auto& value : args.value("points").toArray()) points.push_back(point(value.toObject()));
                    core::PolygonCoverageRasterizer rasterizer(extent, points);
                    selection = rasterize(extent, rasterizer);
                } else {
                    require(args.contains("from") && args.contains("to") && !args.contains("points"),
                        "Rectangle/ellipse selection needs from and to corners");
                    const auto from = point(args.value("from").toObject()), to = point(args.value("to").toObject());
                    if (shape == "rectangle") selection = core::SelectionMask::rectangle(extent, core::alignedSelectionRectangle(from, to, extent));
                    else {
                        core::EllipseCoverageRasterizer rasterizer(extent, core::EllipseGeometry(from, to));
                        selection = rasterize(extent, rasterizer);
                    }
                }
                selection = core::combineSelection(document().selection(), selection, selectionOperation(args));
            }
            const bool edited = execute(std::make_unique<core::SetSelectionCommand>(selection));
            auto data = result(edited); data.insert("selection", selectionData(document())); return data;
        });

    add("vulkana_selection_from_alpha", "Select intrinsic layer transparency, like Ctrl-clicking its thumbnail: ignores layer opacity, fill opacity, mask, styles and clipping-group contributions, but retains transform/crop. Supports raster/text/shape. Replaces only if source coverage is nonempty; empty sources leave selection and redo unchanged.",
        {{"layerId", identifier()}}, {"layerId"}, Access::Document, [this](const QJsonObject& args) {
            const auto selection = alphaSelection(document(), layer(args));
            const bool edited = selection && execute(std::make_unique<core::SetSelectionCommand>(selection));
            auto data = result(edited); data.insert("sourceWasEmpty", !selection);
            data.insert("selection", selectionData(document())); return data;
        });

    add("vulkana_colors", "Set session drawing colors (not theme/settings). Color values use sRGB #RRGGBB or #RRGGBBAA. The active slot supplies foreground to drawing tools; not a document undo step.",
        {{"primary", colorSchema()}, {"secondary", colorSchema()}, {"active", choice({"primary", "secondary"})}},
        {}, Access::Document, [this](const QJsonObject& args) {
            require(args.contains("primary") || args.contains("secondary") || args.contains("active"), "Supply a drawing color or active slot");
            auto colors = session().colors();
            if (args.contains("primary")) colors.primary = color(args.value("primary").toString());
            if (args.contains("secondary")) colors.secondary = color(args.value("secondary").toString());
            if (args.contains("active")) colors.active = args.value("active").toString() == "primary"
                ? core::ColorSlot::Primary : core::ColorSlot::Secondary;
            const bool edited = colors != session().colors();
            window_.setColors(colors);
            auto data = result(edited);
            data.insert("primary", color(colors.primary)); data.insert("secondary", color(colors.secondary));
            data.insert("active", colors.active == core::ColorSlot::Primary ? "primary" : "secondary");
            return data;
        });

    add("vulkana_sample_color", "Read one document-pixel color. source=merged samples composited visible artwork; source=layer samples intrinsic raster source without opacity/visibility/adjustments. Does not change drawing colors.",
        {{"point", pointSchema()}, {"source", choice({"merged", "layer"})}, {"layerId", identifier()}},
        {"point"}, Access::DocumentQuery, [this](const QJsonObject& args) {
            const bool active = args.value("source").toString("merged") == "layer";
            require(active || !args.contains("layerId"), "layerId is only used with source=layer");
            std::optional<core::LayerId> layerId;
            if (active) layerId = layer(args).id;
            const auto sampled = core::sampleDocumentColor(document(), layerId, point(args.value("point").toObject()),
                active ? core::ColorSampleSource::ActiveLayer : core::ColorSampleSource::MergedVisible);
            require(sampled.available(), "Color is unavailable: point outside canvas or unsupported layer source");
            auto data = result(false); data.insert("color", color(sampled.color)); return data;
        });

    add("vulkana_tool_select", "Select an ordinary editing tool without simulating a pointer gesture or opening dialogs. Use specific MCP editing tools for strokes, geometry and effects; this changes UI tool selection only.",
        {{"tool", choice({"move", "brush", "eraser", "fill", "eyedropper", "text", "shape", "rectangle_selection", "lasso", "measure", "smart_select", "color_select", "clone", "local_blur"})}},
        {"tool"}, Access::Document, [this](const QJsonObject& args) {
            static const std::array<std::pair<const char*, core::ToolId>, 14> tools {{
                {"move", core::ToolId::Move}, {"brush", core::ToolId::Brush}, {"eraser", core::ToolId::Eraser},
                {"fill", core::ToolId::Fill}, {"eyedropper", core::ToolId::Eyedropper}, {"text", core::ToolId::Text},
                {"shape", core::ToolId::Shape}, {"rectangle_selection", core::ToolId::Marquee}, {"lasso", core::ToolId::Lasso},
                {"measure", core::ToolId::Measure}, {"smart_select", core::ToolId::SmartSelect},
                {"color_select", core::ToolId::SelectByColor}, {"clone", core::ToolId::Cloning}, {"local_blur", core::ToolId::LocalBlur}}};
            const auto name = args.value("tool").toString();
            const auto found = std::find_if(tools.begin(), tools.end(), [&](const auto& item) { return name == QLatin1String(item.first); });
            require(found != tools.end(), "Unknown tool");
            const bool edited = session().activeTool() != found->second;
            window_.setActiveTool(found->second);
            require(session().activeTool() == found->second, "Editor did not switch tools; query blocking state");
            auto data = result(edited); data.insert("tool", name); return data;
        });
}
} // namespace imageeditor::ui
