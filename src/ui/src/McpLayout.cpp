#include "imageeditor/ui/McpController.hpp"
#include "McpLayerGeometry.hpp"
#include "McpToolHelpers.hpp"
#include "imageeditor/core/LayerStructureCommands.hpp"
#include "imageeditor/core/LayerTransform.hpp"
#include "imageeditor/core/TransformSupport.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>

namespace imageeditor::ui {
namespace {
namespace c = core;
using namespace mcpdetail;
constexpr int maximumTargets = 256, maximumCopies = 128;
constexpr std::size_t maximumLeaves = 1024, maximumCopyBytes = 256ULL * 1024 * 1024;

QJsonArray identifiers(std::span<const c::LayerId> values)
{
    QJsonArray out;
    for (auto value : values) out.append(id(value));
    return out;
}
std::vector<c::LayerId> targets(const c::Document& document, const QJsonObject& args)
{
    std::vector<c::LayerId> out;
    for (const auto value : args["layerIds"].toArray()) {
        const auto target = id(QJsonObject{{"id",value}},"id");
        require(document.containsItem(target), "Unknown layer/container ID; query vulkana_get_state");
        require(std::ranges::find(out,target) == out.end(), "Duplicate layer IDs are not allowed");
        out.push_back(target);
    }
    return document.tree().normalize(out);
}
c::RectD rectangle(const QJsonObject& value)
{
    return {value["x"].toDouble(),value["y"].toDouble(),value["width"].toDouble(),value["height"].toDouble()};
}
QJsonObject rectangle(c::RectD value)
{
    return {{"x",value.x},{"y",value.y},{"width",value.width},{"height",value.height}};
}
c::RectD unite(c::RectD a, c::RectD b)
{
    const auto x = std::min(a.x,b.x), y = std::min(a.y,b.y);
    return {x,y,std::max(a.right(),b.right())-x,std::max(a.bottom(),b.bottom())-y};
}
c::RectD itemBounds(const c::Document& document, c::LayerId item)
{
    std::optional<c::RectD> bounds;
    for (auto id : document.tree().descendants(item)) {
        const auto& layer = *document.layer(id);
        if (std::holds_alternative<c::AdjustmentLayer>(layer.payload)) continue;
        const auto data = layerGeometryData(layer);
        require(data.isObject(), "Layer geometry cannot be measured; check its transform and text/font data");
        const auto rect = rectangle(data.toObject()["documentBounds"].toObject());
        bounds = bounds ? unite(*bounds,rect) : rect;
    }
    require(bounds.has_value(), "Layout requires a raster, text or shape layer; empty and adjustment-only containers have no layout bounds");
    return *bounds;
}
c::RectD itemUnion(const c::Document& document, std::span<const c::LayerId> items)
{
    auto result = itemBounds(document,items.front());
    for (std::size_t i=1;i<items.size();++i) result=unite(result,itemBounds(document,items[i]));
    return result;
}
c::RectD referenceBounds(const c::Document& document, std::span<const c::LayerId> items, bool canvas)
{
    const auto extent = document.canvas().extent;
    return canvas ? c::RectD{0,0,double(extent.width),double(extent.height)} : itemUnion(document,items);
}
c::RectD evaluationSupport(const c::Document& document, const c::Layer& layer)
{
    if (std::holds_alternative<c::AdjustmentLayer>(layer.payload)) {
        const auto canvas = document.canvas().extent;
        return {0,0,double(canvas.width),double(canvas.height)};
    }
    const auto data = layerGeometryData(layer);
    require(data.isObject(), "Layer geometry cannot be measured for transformation");
    auto raw = rectangle(data.toObject()["localBounds"].toObject());
    if (const auto* shape = std::get_if<c::ShapeLayer>(&layer.payload); shape && shape->strokeEnabled) {
        const auto margin = shape->strokeWidth * std::max(1.,shape->strokeMiterLimit);
        raw = {raw.x-margin,raw.y-margin,raw.width+2*margin,raw.height+2*margin};
    }
    return c::transformEvaluationSupport(raw,layer.filters,layer.effects);
}
void appendMove(const c::Document& document, c::LayerId target, c::Vec2d delta,
    std::vector<c::LayerTransformUpdate>& updates)
{
    if (std::abs(delta.x) < 1e-8) delta.x = 0;
    if (std::abs(delta.y) < 1e-8) delta.y = 0;
    for (auto leaf : document.tree().descendants(target)) {
        const auto& layer = *document.layer(leaf);
        const auto after = delta == c::Vec2d{} ? layer.localToDocument
            : c::composeTransform({1,0,delta.x,0,1,delta.y},layer.localToDocument);
        require(after.validOver(evaluationSupport(document,layer)), "Layout exceeds supported coordinates or crosses a projective horizon; no layers changed");
        updates.push_back({leaf,layer.localToDocument,after});
    }
}
QJsonArray measuredItems(const c::Document& document, std::span<const c::LayerId> targets)
{
    QJsonArray out;
    for (auto target : targets) out.append(QJsonObject{{"layerId",id(target)},{"documentBounds",rectangle(itemBounds(document,target))}});
    return out;
}
double anchor(double start, double size, const QString& mode)
{
    return start + (mode == "center" ? size*.5 : mode == "right" || mode == "bottom" ? size : 0);
}
} // namespace

void McpController::registerLayoutTools()
{
    using namespace mcpdetail;
    const auto ids = array(identifier(),1,maximumTargets);
    add("vulkana_layers_align", "Align explicit layers/containers using logical document-space bounds, excluding stroke/effects/crop/masks. Containers move as units, including hidden descendants. reference=selection uses the union of supplied items (not pixel selection); canvas uses the canvas boundary. Supply horizontal and/or vertical. One undo step; no-op preserves history and current layer selection.",
        {{"layerIds",ids},{"reference",choice({"selection","canvas"},"Default selection; refers to supplied layer bounds, not the pixel selection.")},
            {"horizontal",choice({"left","center","right"})},{"vertical",choice({"top","center","bottom"})}}, {"layerIds"}, Access::Document,
        [this](const QJsonObject& args) {
            require(args.contains("horizontal") || args.contains("vertical"), "Specify horizontal and/or vertical alignment");
            const auto selected = targets(document(),args);
            const auto reference = referenceBounds(document(),selected,args["reference"] == "canvas");
            std::vector<c::LayerTransformUpdate> updates;
            for (auto target : selected) {
                const auto bounds = itemBounds(document(),target);
                c::Vec2d delta;
                if (args.contains("horizontal")) delta.x = anchor(reference.x,reference.width,args["horizontal"].toString()) - anchor(bounds.x,bounds.width,args["horizontal"].toString());
                if (args.contains("vertical")) delta.y = anchor(reference.y,reference.height,args["vertical"].toString()) - anchor(bounds.y,bounds.height,args["vertical"].toString());
                appendMove(document(),target,delta,updates);
            }
            auto command = c::makeLayerTransformCommand(document(),std::move(updates));
            const bool hasChange = bool(command);
            const bool applied = hasChange && execute(std::move(command));
            require(!hasChange || applied, "Layout could not be committed; query current state before retrying");
            auto data = result(applied); data["items"] = measuredItems(document(),selected); return data;
        });
    add("vulkana_layers_distribute", "Distribute explicit layers/containers along horizontal or vertical document axes. spacing=centers spaces centers equally; gaps spaces the gaps between logical bounds equally (negative gaps mean overlap). Sorts by position, not supplied order. reference=selection spans supplied items; canvas spans the canvas. At least two normalized items. One undo step; keeps current selection.",
        {{"layerIds",ids},{"axis",choice({"horizontal","vertical"})},{"spacing",choice({"centers","gaps"},"Default gaps.")},
            {"reference",choice({"selection","canvas"},"Default selection; supplied layer bounds, not pixel selection.")}}, {"layerIds","axis"}, Access::Document,
        [this](const QJsonObject& args) {
            auto selected = targets(document(),args);
            require(selected.size() >= 2, "Distribution requires at least two independent layers/containers after parent/child normalization");
            const bool horizontal = args["axis"] == "horizontal", centers = args["spacing"] == "centers", canvas = args["reference"] == "canvas";
            struct Item { c::LayerId id; c::RectD bounds; };
            std::vector<Item> items;
            for (auto target : selected) items.push_back({target,itemBounds(document(),target)});
            const auto start = [horizontal](c::RectD r) {return horizontal ? r.x : r.y;};
            const auto length = [horizontal](c::RectD r) {return horizontal ? r.width : r.height;};
            std::stable_sort(items.begin(),items.end(),[&](const auto& a,const auto& b) {
                return start(a.bounds)+(centers ? length(a.bounds)*.5 : 0) < start(b.bounds)+(centers ? length(b.bounds)*.5 : 0);
            });
            const auto frame = referenceBounds(document(),selected,canvas);
            double cursor = start(frame), step = 0;
            if (centers) {
                cursor = canvas ? start(frame)+length(items.front().bounds)*.5 : start(items.front().bounds)+length(items.front().bounds)*.5;
                const auto end = canvas ? start(frame)+length(frame)-length(items.back().bounds)*.5 : start(items.back().bounds)+length(items.back().bounds)*.5;
                require(end >= cursor, "Reference is too small for the end items; use selection bounds or smaller layers");
                step = (end-cursor)/double(items.size()-1);
            } else {
                double occupied = 0; for (const auto& item : items) occupied += length(item.bounds);
                step = (length(frame)-occupied)/double(items.size()-1);
            }
            std::vector<c::LayerTransformUpdate> updates;
            for (const auto& item : items) {
                const auto offset = cursor-start(item.bounds)-(centers ? length(item.bounds)*.5 : 0);
                appendMove(document(),item.id,horizontal ? c::Vec2d{offset,0} : c::Vec2d{0,offset},updates);
                cursor += step+(centers ? 0 : length(item.bounds));
            }
            auto command = c::makeLayerTransformCommand(document(),std::move(updates));
            const bool hasChange = bool(command);
            const bool applied = hasChange && execute(std::move(command));
            require(!hasChange || applied, "Layout could not be committed; query current state before retrying");
            auto data = result(applied); data["items"] = measuredItems(document(),selected); return data;
        });
    add("vulkana_layers_repeat", "Create editable linear or radial repetitions of explicit layers/containers. copies counts NEW copies; originals remain unchanged. Each repetition treats selected items as one motif. Same-parent items form ordered copy blocks above the highest selected sibling; cross-parent copies stay above their own source siblings. Linear requires stepX/stepY. Radial requires centerX/centerY/angleStep in clockwise degrees; rotateCopies defaults true, false orbits the motif center while preserving orientation. Keeps masks/effects/group structure and independent raster pixels. One undo step; selects the new root items. Maximum 128 repetitions, 1024 resulting leaves, 4096 tree items and 256 MiB copied data.",
        {{"layerIds",ids},{"mode",choice({"linear","radial"})},{"copies",integer(1,maximumCopies)},
            {"stepX",number(-1000000,1000000)},{"stepY",number(-1000000,1000000)},
            {"centerX",number(-1000000,1000000)},{"centerY",number(-1000000,1000000)},
            {"angleStep",number(-360,360)},{"rotateCopies",boolean("Radial only; default true.")}}, {"layerIds","mode","copies"}, Access::Document,
        [this](const QJsonObject& args) {
            const auto selected = targets(document(),args);
            const auto leaves = document().expandedLayers(selected);
            const auto count = std::size_t(args["copies"].toInt());
            const bool radial = args["mode"] == "radial", rotate = args["rotateCopies"].toBool(true);
            require(!leaves.empty(), "Cannot repeat empty containers");
            if (radial) require(args.contains("centerX") && args.contains("centerY") && args.contains("angleStep") && !args.contains("stepX") && !args.contains("stepY"), "Radial repetition requires centerX, centerY, angleStep; stepX/stepY are linear-only");
            else require(args.contains("stepX") && args.contains("stepY") && !args.contains("centerX") && !args.contains("centerY") && !args.contains("angleStep") && !args.contains("rotateCopies"), "Linear repetition requires stepX and stepY; center/angle/rotateCopies are radial-only");
            const auto largestSuffix = " copy " + std::to_string(count);
            for (auto target : selected) {
                const auto* layer = document().layer(target);
                const auto& name = layer ? layer->name : document().tree().container(target)->name;
                require(name.size()+largestSuffix.size() <= 4096,
                    QStringLiteral("Copy name for “%1” (ID %2) would exceed 4096 UTF-8 bytes at copy %3. Shorten the source name or reduce copies; no copies created.")
                        .arg(QString::fromStdString(name).left(64),id(target)).arg(count));
            }
            std::size_t containers = 0, bytes = 0;
            for (const auto& container : document().tree().containers)
                if (std::ranges::any_of(selected,[&](auto root) {return root == container.id || document().tree().isAncestor(root,container.id);})) {
                    ++containers; bytes += sizeof(c::LayerContainer)+container.name.capacity()+container.children.capacity()*sizeof(c::LayerId);
                }
            require(leaves.size()*count <= maximumLeaves && document().layers().size()+leaves.size()*count <= maximumLeaves,
                "Repetition exceeds 1024 resulting layers; reduce copies or the selected set");
            require(document().layers().size()+document().tree().containers.size()+(leaves.size()+containers)*count <= c::LayerTree::maxItems,
                "Repetition exceeds 4096 tree items; reduce copies or the selected set");
            for (auto leaf : leaves) {bytes += c::retainedLayerMemory(*document().layer(leaf)); require(bytes <= maximumCopyBytes/count, "Repetition exceeds 256 MiB copied data; reduce copies or source image sizes");}
            std::map<c::LayerId,c::RectD> supports;
            for (auto leaf : leaves) supports.emplace(leaf,evaluationSupport(document(),*document().layer(leaf)));
            const auto motif = radial && !rotate ? itemUnion(document(),selected) : c::RectD{};
            const c::Vec2d motifCenter{motif.x+motif.width*.5,motif.y+motif.height*.5};
            const c::Vec2d center{args["centerX"].toDouble(),args["centerY"].toDouble()};
            std::vector<c::AffineTransform> deltas;
            for (std::size_t i=1;i<=count;++i) {
                c::AffineTransform delta{1,0,double(i)*args["stepX"].toDouble(),0,1,double(i)*args["stepY"].toDouble()};
                if (radial) {
                    const auto angle = std::remainder(double(i)*args["angleStep"].toDouble(),360.)*std::numbers::pi/180.;
                    const auto cosine = std::cos(angle), sine = std::sin(angle);
                    delta = {cosine,-sine,center.x-cosine*center.x+sine*center.y,sine,cosine,center.y-sine*center.x-cosine*center.y};
                    if (!rotate) {const auto orbit = delta.map(motifCenter)-motifCenter; delta={1,0,orbit.x,0,1,orbit.y};}
                }
                for (auto leaf : leaves) require(c::composeTransform(delta,document().layer(leaf)->localToDocument).validOver(supports.at(leaf)),
                    "Repeated transform exceeds supported coordinates or crosses a projective horizon; no copies created");
                deltas.push_back(delta);
            }
            // Clone mutable pixels through the existing transfer path only
            // after the complete count, memory and transform admission pass.
            auto tree = document().tree();
            std::vector<c::Layer> added;
            std::vector<c::LayerId> newIds;
            std::map<c::LayerId,c::LayerId> previous;
            for (auto target : selected) previous[target]=target;
            const auto firstParent = document().tree().placement(selected.front())->parent;
            const bool sameParent = std::ranges::all_of(selected,[&](auto target) {return document().tree().placement(target)->parent==firstParent;});
            auto lastInstanceRoot = selected.back();
            QJsonArray instances;
            for (std::size_t i=0;i<count;++i) {
                auto transfer = c::captureLayerTransfer(document(),selected);
                auto instanceRoots = transfer.tree.roots;
                for (std::size_t j=0;j<selected.size();++j) {
                    const auto source = selected[j], copy = instanceRoots[j];
                    const bool visible = document().itemVisibility(source).value_or(true);
                    const auto suffix = " copy " + std::to_string(i+1);
                    if (auto* folder = transfer.tree.container(copy)) {folder->visible=visible;folder->name+=suffix;}
                    else {auto& layer=*std::ranges::find(transfer.layers,copy,&c::Layer::id);layer.visible=visible;layer.name+=suffix;}
                    if (!sameParent) {
                        const auto placement = *tree.placement(previous[source]);
                        auto& siblings = *tree.children(placement.parent);
                        siblings.insert(siblings.begin()+std::ptrdiff_t(placement.index+1),copy);
                        previous[source]=copy;
                    }
                    newIds.push_back(copy);
                }
                if (sameParent) {
                    const auto placement = *tree.placement(lastInstanceRoot);
                    auto& siblings = *tree.children(placement.parent);
                    siblings.insert(siblings.begin()+std::ptrdiff_t(placement.index+1),instanceRoots.begin(),instanceRoots.end());
                    lastInstanceRoot=instanceRoots.back();
                }
                tree.containers.insert(tree.containers.end(),transfer.tree.containers.begin(),transfer.tree.containers.end());
                for (auto& layer : transfer.layers) {layer.localToDocument=c::composeTransform(deltas[i],layer.localToDocument);added.push_back(std::move(layer));}
                instances.append(QJsonObject{{"index",int(i+1)},{"layerIds",identifiers(instanceRoots)}});
            }
            std::vector<c::LayerId> allLeaves; for (const auto& layer : document().layers()) allLeaves.push_back(layer.id);
            for (const auto& layer : added) allLeaves.push_back(layer.id);
            (void)tree.orderedLeaves(allLeaves);
            c::LayerSelectionState after{newIds,newIds.back(),newIds.back()};
            const bool applied = execute(std::make_unique<c::LayerStructureCommand>("Repeat layers",document(),std::move(tree),std::vector<c::LayerId>{},std::move(added),session().layerSelectionState(),std::move(after)));
            require(applied, "Repetition could not be committed; no copies created");
            auto data=result(true);data["sourceLayerIds"]=identifiers(selected);data["layerIds"]=identifiers(newIds);data["instances"]=instances;return data;
        });
}
} // namespace imageeditor::ui
