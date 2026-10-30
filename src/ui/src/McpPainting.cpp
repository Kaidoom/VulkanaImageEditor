#include "imageeditor/ui/McpController.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/FlattenedDocument.hpp"
#include "imageeditor/core/BasicPixelBrushStroke.hpp"
#include "imageeditor/core/ColorSampler.hpp"
#include "imageeditor/core/EllipseCoverageRasterizer.hpp"
#include "imageeditor/core/FillOperation.hpp"
#include "imageeditor/core/LayerGeometry.hpp"
#include "imageeditor/core/LayerMaskEdit.hpp"
#include "imageeditor/core/LayerStructureCommands.hpp"
#include "imageeditor/core/PolygonCoverageRasterizer.hpp"
#include "imageeditor/core/SelectionCommands.hpp"
#include "McpToolHelpers.hpp"

#include <QElapsedTimer>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace imageeditor::ui {
namespace {
using namespace mcpdetail;
constexpr int maximumSamples = 4096;
constexpr std::uint64_t maximumPixels = 64ULL * 1024 * 1024;
constexpr qint64 operationMilliseconds = 5000;
constexpr int maximumBatchStrokes = 128;

struct BrushRequestBudget {
    QElapsedTimer timer;
    std::uint64_t dabs {};
    BrushRequestBudget() { timer.start(); }
};

// The ordinary brush engine remains authoritative. A wrapper only limits a
// single remote request; unwinding the stroke restores its journalled pixels.
class BoundedBrushEngine final : public core::IBrushEngine, private core::BrushDabSink {
public:
    explicit BoundedBrushEngine(BrushRequestBudget* request = nullptr) : request_(request) {}
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
        if (timer_.elapsed() > operationMilliseconds
            || (request_ && request_->timer.elapsed() > operationMilliseconds))
            throw std::runtime_error("Brush request exceeded five seconds; stroke rolled back. Use a shorter path or smaller brush.");
    }
    void emitDab(const core::BrushDab& dab) override
    {
        checkTime();
        if (++count_ > 65536 || (request_ && ++request_->dabs > 65536))
            throw std::runtime_error("Brush request exceeded 65536 dabs; stroke rolled back. Use a shorter path or larger spacing.");
        sink_->emitDab(dab);
    }
    core::BasicPixelBrushEngine engine_;
    core::BrushDabSink* sink_ {};
    QElapsedTimer timer_;
    std::uint64_t count_ {};
    BrushRequestBudget* request_ {};
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

QJsonObject strokeProperties()
{
    return {{"points", array(pointSchema(true), 1, maximumSamples)},
        {"presetId", string("Default: builtin.preset.hard-round.v1; query vulkana_brush_presets.")},
        {"mode", choice({"paint", "erase"}, "Default paint.")},
        {"color", colorSchema()}, {"size", number(.25, 1000, "Brush diameter in document pixels.")},
        {"hardness", number(0, 1)}, {"opacity", number(0, 1)}, {"flow", number(0, 1)},
        {"spacing", number(1, 200, "Dab spacing as percent of brush diameter.")},
        {"pressureToSize", boolean()}, {"pressureToFlow", boolean()}};
}
struct PreparedStroke {
    core::BrushSettings settings;
    std::vector<core::NormalizedPointerSample> points;
    bool erase {};
};
PreparedStroke prepareStroke(const QJsonObject& args, bool mask, core::Rgba8 foreground,
    std::span<const core::BrushPresetRecord> presets, BrushAssetLibrary& assets)
{
    PreparedStroke result;
    result.erase = args.value("mode").toString("paint") == "erase";
    const auto presetId = args.value("presetId").toString("builtin.preset.hard-round.v1").toStdString();
    const auto preset = std::find_if(presets.begin(), presets.end(),
        [&](const auto& entry) { return entry.id == presetId; });
    require(preset != presets.end(), "Unknown brush preset ID; query vulkana_brush_presets");
    auto& settings = result.settings;
    settings = preset->settings;
    for (const auto& [name, field] : std::array<std::pair<const char*, double*>, 5>{{
             {"size", &settings.sizePixels}, {"hardness", &settings.hardness},
             {"opacity", &settings.opacity}, {"flow", &settings.flow}, {"spacing", &settings.spacingPercent}}})
        if (args.contains(QLatin1String(name))) *field = args.value(QLatin1String(name)).toDouble();
    if (args.contains("pressureToSize")) settings.pressureToSize = args.value("pressureToSize").toBool();
    if (args.contains("pressureToFlow")) settings.pressureToFlow = args.value("pressureToFlow").toBool();
    settings.foreground = args.contains("color") ? color(args.value("color").toString()) : foreground;
    if (mask && result.erase) settings.foreground = {0, 0, 0, 255};
    auto& points = result.points;
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
    require(!points.empty(), "A brush stroke requires at least one point");
    require(length <= 1000000, "Stroke path exceeds one million document pixels; split it into smaller strokes");
    QString assetError;
    require(assets.prepareSettings(settings, &assetError), assetError);
    return result;
}
struct StrokeResult {
    bool changed {};
    core::BrushStrokeStats stats;
};
StrokeResult paintStroke(core::Document& document, core::History& history, core::LayerId id,
    bool mask, const PreparedStroke& prepared, BrushAssetLibrary& assets, BrushRequestBudget* request = nullptr)
{
    std::unique_ptr<core::LayerMaskEdit> maskEdit;
    if (mask) maskEdit = std::make_unique<core::LayerMaskEdit>(document, id, "Paint layer mask");
    core::RasterEditTransactionOptions options; options.coverageValues = mask;
    core::BasicPixelBrushStroke stroke(mask ? maskEdit->proxy() : document, id, prepared.settings,
        mask ? core::BrushCompositeMode::MaskCoverage : prepared.erase ? core::BrushCompositeMode::Erase : core::BrushCompositeMode::Paint,
        std::make_unique<BoundedBrushEngine>(request), std::unique_ptr<core::IBrushTip>{},
        std::unique_ptr<core::IBrushGrain>{}, &assets, options);
    require(stroke.valid() && stroke.begin(prepared.points.front()), "Unable to begin stroke; layer geometry or raster work limit rejected it");
    for (std::size_t i = 1; i + 1 < prepared.points.size(); ++i)
        require(stroke.append(prepared.points[i]), "Stroke rejected; no pixels were committed");
    auto committed = stroke.end(prepared.points.back(), mask ? maskEdit->provisionalHistory() : history);
    require(committed == core::RasterEditCommitResult::Committed || committed == core::RasterEditCommitResult::NoChanges,
        "Stroke could not commit; no pixels were committed");
    if (mask) committed = maskEdit->commit(history);
    require(committed == core::RasterEditCommitResult::Committed || committed == core::RasterEditCommitResult::NoChanges,
        "Mask stroke could not commit");
    return {committed == core::RasterEditCommitResult::Committed, stroke.stats()};
}
bool regionalPixelsDiffer(const core::RegionalRasterSurface& after, const core::RasterSurface& before)
{
    if (after.extent() != before.extent()) return true;
    constexpr int side = core::RegionalRasterSurface::tileSide;
    std::array<std::byte, std::size_t(side) * side * 4> a {}, b {};
    const auto bounds = after.state().bounds;
    for (const auto& [key, tile] : after.state().tiles) {
        (void)tile;
        const auto region = core::RectI{key.first * side - bounds.x, key.second * side - bounds.y, side, side}
            .clippedTo({0, 0, bounds.width, bounds.height});
        if (region.empty()) continue;
        const auto stride = std::size_t(region.width) * 4;
        const auto count = stride * std::size_t(region.height);
        before.copyRgba8(region, std::span(a).first(count), stride);
        after.copyRgba8(region, std::span(b).first(count), stride);
        if (!std::equal(a.begin(), a.begin() + std::ptrdiff_t(count), b.begin())) return true;
    }
    return false;
}
class BrushBatchCommand final : public core::Command {
public:
    BrushBatchCommand(const core::Document& document, core::Layer after, core::LayerSelectionState selection,
        std::shared_ptr<core::RegionalRasterSurface> pixels)
        : pixels_(std::move(pixels)), state_(pixels_ ? pixels_->state() : core::RegionalRasterSurface::State{})
    {
        const auto id = after.id;
        operation_ = std::make_unique<core::LayerStructureCommand>("Paint strokes", document, document.tree(),
            std::vector<core::LayerId>{id}, std::vector<core::Layer>{std::move(after)}, selection, selection);
        cost_ = sizeof(*this) + operation_->memoryCost() + state_.tiles.size() * 96;
        // Structural history charges logical raster extents. COW tiles have
        // a minimum 64x64 allocation even for a 1x1 image, and can pin a larger
        // base. Charge that ownership too; this intentionally errs high rather
        // than undercounting hidden history memory.
        std::vector<std::shared_ptr<const core::RasterSurface>> pending;
        if (pixels_) pending.push_back(pixels_);
        if (const auto* raster = std::get_if<core::RasterLayer>(&document.layer(id)->payload)) pending.push_back(raster->surface);
        std::unordered_set<const core::RasterSurface*> seen;
        while (!pending.empty()) {
            const auto surface = std::move(pending.back()); pending.pop_back();
            if (!surface || !seen.insert(surface.get()).second) continue;
            if (const auto* regional = dynamic_cast<const core::RegionalRasterSurface*>(surface.get())) {
                cost_ += sizeof(core::RegionalRasterSurface) + regional->retainedBytes();
                pending.push_back(regional->baseSurface());
            } else {
                const auto extent = surface->extent();
                cost_ += std::size_t(extent.width) * extent.height * 4;
            }
        }
    }
    bool apply(core::Document& document) override
    {
        if (pixels_) pixels_->restore(state_);
        return operation_->apply(document);
    }
    bool undo(core::Document& document) override { return operation_->undo(document); }
    std::string_view label() const noexcept override { return operation_->label(); }
    std::size_t memoryCost() const noexcept override { return cost_; }
    const core::LayerSelectionState* layerSelectionAfter(bool undo) const noexcept override
    { return operation_->layerSelectionAfter(undo); }
private:
    std::unique_ptr<core::LayerStructureCommand> operation_;
    std::shared_ptr<core::RegionalRasterSurface> pixels_;
    core::RegionalRasterSurface::State state_;
    std::size_t cost_ {};
};

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
    add("vulkana_brush_presets", "List brush presets and their defaults. available means the tip and grain assets are supported; prepared reports whether their runtime masks are already loaded. Available load_on_use presets prepare automatically for a stroke. Listing never prepares assets. Presets are read-only; painting overrides apply to one stroke, not preferences.",
        {}, {}, Access::Query, [this](const QJsonObject&) {
            QJsonArray presets;
            for (const auto& preset : window_.brushPresets_) {
                auto entry = brushSettings(preset.settings);
                entry.insert("id", QString::fromStdString(preset.id));
                entry.insert("name", QString::fromStdString(preset.displayName));
                const auto& assets = *window_.brushAssets_;
                QString unavailable;
                for (const auto& [assetId, type] : std::array{
                         std::pair{preset.settings.tip.assetId, core::BrushAssetType::Tip},
                         std::pair{preset.settings.grain.assetId, core::BrushAssetType::Grain}}) {
                    if (assets.supports(assetId, type)) continue;
                    const auto* component = assets.component(assetId);
                    unavailable = component && !component->unavailableReason.isEmpty() ? component->unavailableReason
                        : QStringLiteral("Brush %1 is unavailable: %2")
                            .arg(type == core::BrushAssetType::Tip ? "tip" : "grain", QString::fromStdString(assetId));
                    break;
                }
                const bool available = unavailable.isEmpty();
                const bool prepared = available && assets.isPrepared(preset.settings.tip.assetId, core::BrushAssetType::Tip)
                    && assets.isPrepared(preset.settings.grain.assetId, core::BrushAssetType::Grain);
                entry.insert("available", available); entry.insert("prepared", prepared);
                entry.insert("status", available ? prepared ? "ready" : "load_on_use" : "unavailable");
                if (!available) entry.insert("reason", unavailable);
                presets.append(entry);
            }
            return QJsonObject {{"presets", presets}, {"defaultPreset", "builtin.preset.hard-round.v1"},
                {"maximumPathPoints", maximumSamples}, {"maximumDabs", 65536}, {"timeBudgetMs", int(operationMilliseconds)}};
        });

    auto singleStroke = strokeProperties();
    singleStroke.insert("layerId", identifier());
    singleStroke.insert("target", choice({"pixels", "mask"}, "Default pixels."));
    add("vulkana_brush_stroke", "Paint one undoable stroke using Vulkana's brush engine. Coordinates are document pixels, not viewport pixels. Honors selection, layer transforms and crop. Pixel target requires raster; mask target requires an existing mask. Erasing a mask paints black. Preserves active layer, tool and brush settings. Paths over 4096 points, 65536 dabs or five seconds fail with rollback.", singleStroke,
        {"layerId", "points"}, Access::Document, [this](const QJsonObject& args) {
            const auto& target = layer(args);
            const bool mask = args.value("target").toString("pixels") == "mask";
            paintTarget(target, mask);
            const auto prepared = prepareStroke(args, mask, session().foregroundColor(), window_.brushPresets_, *window_.brushAssets_);
            const auto painted = paintStroke(document(), session().history(), target.id, mask, prepared, *window_.brushAssets_);
            if (painted.changed) changed(false);
            auto data = result(painted.changed);
            data.insert("layerId", id(target.id));
            data.insert("dabs", double(painted.stats.emittedDabs));
            data.insert("changedPixels", double(painted.stats.changedPixels));
            return data;
        });

    add("vulkana_brush_strokes", "Paint multiple independent strokes on one layer or existing mask, in order, with one undo step. Each stroke accepts the same points/settings as vulkana_brush_stroke. The whole batch is staged privately and publishes only after success; failure preserves pixels, selection and redo. Limits per batch: 128 strokes, 4096 total points, 65536 total dabs and five seconds of painting. No-op batches preserve history.",
        {{"layerId", identifier()}, {"target", choice({"pixels", "mask"}, "Default pixels.")},
            {"strokes", array(object(strokeProperties(), {"points"}), 1, maximumBatchStrokes)}},
        {"layerId", "strokes"}, Access::Document, [this](const QJsonObject& args) {
            const auto& target = layer(args);
            const bool mask = args.value("target").toString("pixels") == "mask";
            paintTarget(target, mask);
            const auto targetId = target.id;
            std::vector<PreparedStroke> strokes;
            const auto entries = args["strokes"].toArray();
            strokes.reserve(std::size_t(entries.size()));
            std::size_t samples = 0;
            for (qsizetype i=0; i<entries.size(); ++i) {
                samples += std::size_t(entries[i].toObject()["points"].toArray().size());
                require(samples <= std::size_t(maximumSamples), "arguments.strokes exceeds 4096 total points; split into smaller batches");
                try { strokes.push_back(prepareStroke(entries[i].toObject(), mask, session().foregroundColor(), window_.brushPresets_, *window_.brushAssets_)); }
                catch (const std::exception& error) { throw std::invalid_argument(QStringLiteral("arguments.strokes[%1]: %2").arg(i).arg(QString::fromUtf8(error.what())).toStdString()); }
            }
            // Only the target's metadata is copied, immutable masks remain
            // shared, and modified raster tiles acquire private storage.
            // Original pixels/revisions/history never need rollback on failure.
            core::Document staged(document().canvas());
            require(staged.insertLayer(0, target), "Could not prepare batch target");
            staged.setSelection(document().selection());
            auto* stagedLayer = staged.layer(targetId);
            stagedLayer->renderCache.reset(); stagedLayer->filterCache.reset(); stagedLayer->effectCache.reset();
            std::shared_ptr<core::RegionalRasterSurface> pixels;
            std::shared_ptr<core::RasterSurface> originalPixels;
            if (!mask) {
                originalPixels = std::get<core::RasterLayer>(target.payload).surface;
                pixels = std::make_shared<core::RegionalRasterSurface>(originalPixels, target.rasterOrigin);
                require(staged.setLayerRasterStorage(targetId, pixels, target.rasterOrigin, target.rasterEffectFrame), "Could not prepare batch raster storage");
            }
            core::History provisional;
            BrushRequestBudget budget;
            std::uint64_t dabs = 0, changedPixels = 0;
            bool any = false;
            for (std::size_t i=0; i<strokes.size(); ++i) {
                try {
                    const auto painted = paintStroke(staged, provisional, targetId, mask, strokes[i], *window_.brushAssets_, &budget);
                    any |= painted.changed; dabs += painted.stats.emittedDabs; changedPixels += painted.stats.changedPixels;
                    provisional.clear(); // Final pixels/mask own the result; retain no per-stroke undo journals.
                } catch (const std::exception& error) {
                    throw std::runtime_error(QStringLiteral("arguments.strokes[%1]: %2; the entire batch was discarded").arg(i).arg(QString::fromUtf8(error.what())).toStdString());
                }
            }
            if (any) {
                const auto& final = *staged.layer(targetId);
                if (mask) any = !core::equivalentLayerMasks(target.mask, final.mask);
                else {
                    // Paint can grow storage and replace it more than once.
                    // Compare and retain the FINAL surface, not the initial
                    // staging wrapper, including its local origin/effect frame.
                    pixels = std::dynamic_pointer_cast<core::RegionalRasterSurface>(std::get<core::RasterLayer>(final.payload).surface);
                    require(bool(pixels), "Brush batch lost its private raster storage; no pixels were committed");
                    any = final.rasterOrigin != target.rasterOrigin || final.rasterEffectFrame != target.rasterEffectFrame
                        || regionalPixelsDiffer(*pixels, *originalPixels);
                }
            }
            if (any) {
                std::unique_ptr<core::Command> command;
                if (mask) command = std::make_unique<core::LayerMaskCommand>(targetId, target.mask,
                    staged.layer(targetId)->mask, "Paint strokes");
                else command = std::make_unique<BrushBatchCommand>(document(), *staged.layer(targetId), session().layerSelectionState(), pixels);
                require(execute(std::move(command)), "Could not publish brush batch; no pixels were committed");
            }
            auto data = result(any); data["layerId"] = id(targetId); data["strokeCount"] = int(strokes.size());
            data["dabs"] = double(dabs); data["changedPixels"] = double(changedPixels);
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
