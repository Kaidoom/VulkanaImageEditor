#include "imageeditor/core/BoundsSnapping.hpp"

#include <algorithm>
#include <cmath>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace imageeditor::core {
namespace {
double coordinate(const DocumentBounds& b, unsigned axis, unsigned anchor)
{
    const double lo = axis == 0 ? b.minimum.x : b.minimum.y;
    const double hi = axis == 0 ? b.maximum.x : b.maximum.y;
    return anchor == 0 ? lo : anchor == 2 ? hi : lo + (hi - lo) * .5;
}
void unite(std::optional<DocumentBounds>& into, const DocumentBounds& b)
{
    if (!into) { into = b; return; }
    into->minimum.x = std::min(into->minimum.x, b.minimum.x);
    into->minimum.y = std::min(into->minimum.y, b.minimum.y);
    into->maximum.x = std::max(into->maximum.x, b.maximum.x);
    into->maximum.y = std::max(into->maximum.y, b.maximum.y);
}
}

void BoundsSnapping::clear() noexcept
{
    start_.reset(); moving_.clear(); active_ = {};
    for (auto& axis : anchors_) axis.clear();
    revision_ = 0;
}
bool BoundsSnapping::begin(const Document& document, std::span<const LayerId> moving)
{
    clear();
    start_ = selectedLayerBounds(document, moving);
    if (!start_) return false;
    moving_ = document.expandedLayers(moving);
    rebuild(document);
    return true;
}
bool BoundsSnapping::begin(const Document& document,DocumentBounds bounds,std::span<const LayerId> excluded)
{
    clear();start_=bounds;moving_=document.expandedLayers(excluded);rebuild(document);return true;
}
void BoundsSnapping::rebuild(const Document& document)
{
    active_ = {};
    for (auto& axis : anchors_) axis.clear();
    const auto add = [&](LayerId id, const DocumentBounds& bounds) {
        for (unsigned axis = 0; axis < 2; ++axis)
            for (unsigned kind = 0; kind < 3; ++kind)
                anchors_[axis].push_back({coordinate(bounds, axis, kind), kind, id, bounds});
    };
    add(0, {{0, 0}, {double(document.canvas().extent.width), double(document.canvas().extent.height)}});
    std::unordered_map<LayerId, const Layer*> layers;
    std::unordered_map<LayerId, const LayerContainer*> containers;
    for (const auto& layer : document.layers()) layers.emplace(layer.id, &layer);
    for (const auto& item : document.tree().containers) containers.emplace(item.id, &item);
    const std::unordered_set<LayerId> moving(moving_.begin(), moving_.end());
    struct Aggregate { std::optional<DocumentBounds> bounds; bool containsMoving {false}; };
    const auto visit = [&](auto&& self, LayerId id, bool visible) -> Aggregate {
        if (const auto it = layers.find(id); it != layers.end()) {
            const auto& layer = *it->second;
            const bool excluded = moving.contains(id);
            if (!visible || !layer.visible || !(layer.opacity > 0) || !std::isfinite(layer.opacity))
                return {{}, excluded};
            const auto bounds = layerDocumentBounds(layer);
            if (bounds && !excluded) add(id, *bounds);
            return {bounds, excluded};
        }
        const auto it = containers.find(id);
        if (it == containers.end()) return {};
        const auto& item = *it->second;
        Aggregate result;
        for (const auto child : item.children) {
            const auto part = self(self, child, visible && item.visible);
            if (part.bounds) unite(result.bounds, *part.bounds);
            result.containsMoving |= part.containsMoving;
        }
        // Folders are organizational only. Groups have a visible-content
        // aggregate, but no ancestor containing a mover can attract itself.
        if (isGroup(item.kind) && result.bounds && !result.containsMoving)
            add(id, *result.bounds);
        return result;
    };
    for (const auto id : document.tree().roots) visit(visit, id, true);
    for (auto& axis : anchors_)
        std::sort(axis.begin(), axis.end(), [](const Anchor& a, const Anchor& b) {
            return std::tie(a.coordinate, a.id, a.kind) < std::tie(b.coordinate, b.id, b.kind);
        });
    revision_ = document.revision();
    ++targetBuildCount_;
}

SnapResult BoundsSnapping::resolve(const Document& document, Vec2d raw, const SnapOptions& options)
{
    SnapResult result {raw, {}};
    if (!start_ || !std::isfinite(raw.x) || !std::isfinite(raw.y)
        || !std::isfinite(options.logicalScale) || options.logicalScale <= 0) {
        active_ = {}; return result;
    }
    if (revision_ != document.revision()) {
        for (const auto id : moving_)
            if (!document.containsLayer(id)) { clear(); return result; }
        rebuild(document);
    }
    if (!options.enabled || options.bypass) { active_ = {}; return result; }
    const auto capture = capturePixels / options.logicalScale;
    const auto release = releasePixels / options.logicalScale;
    for (unsigned axis = 0; axis < 2; ++axis) {
        if (!(axis == 0 ? options.allowX : options.allowY)) { active_[axis].reset(); continue; }
        const double delta = axis == 0 ? raw.x : raw.y;
        const auto available = [&](const Anchor& a, unsigned mover) {
            return (a.id == 0 ? options.canvas : options.layers)
                && ((a.kind == 1) == (mover == 1)); // Centers to centers; either edge to either edge.
        };
        const auto correction = [&](const Alignment& alignment) {
            return anchors_[axis][alignment.target].coordinate
                - (coordinate(*start_, axis, alignment.movingAnchor) + delta);
        };
        if (active_[axis]) {
            const auto a = *active_[axis];
            if (!available(anchors_[axis][a.target], a.movingAnchor) || std::abs(correction(a)) > release)
                active_[axis].reset();
        }
        if (!active_[axis]) {
            std::optional<std::tuple<double, LayerId, unsigned, unsigned>> best;
            for (unsigned mover = 0; mover < 3; ++mover) {
                const double position = coordinate(*start_, axis, mover) + delta;
                const auto& anchors = anchors_[axis];
                auto it = std::lower_bound(anchors.begin(), anchors.end(), position - capture,
                    [](const Anchor& a, double value) { return a.coordinate < value; });
                for (; it != anchors.end() && it->coordinate <= position + capture; ++it) {
                    if (!available(*it, mover)) continue;
                    const auto rank = std::tuple {std::abs(it->coordinate - position), it->id, mover, it->kind};
                    if (!best || rank < *best) {
                        best = rank;
                        active_[axis] = Alignment {std::size_t(it - anchors.begin()), mover};
                    }
                }
            }
        }
        if (active_[axis]) (axis == 0 ? result.translation.x : result.translation.y) += correction(*active_[axis]);
    }
    // Both axes are resolved first: guides span the actual corrected bounds.
    const DocumentBounds placed {start_->minimum + result.translation, start_->maximum + result.translation};
    for (unsigned axis = 0; axis < 2; ++axis) if (active_[axis]) {
        const auto& target = anchors_[axis][active_[axis]->target];
        const auto& b = target.bounds;
        const double c = target.coordinate;
        result.guides[axis] = axis == 0
            ? SnapGuide {{c, std::min(b.minimum.y, placed.minimum.y)}, {c, std::max(b.maximum.y, placed.maximum.y)}, target.id}
            : SnapGuide {{std::min(b.minimum.x, placed.minimum.x), c}, {std::max(b.maximum.x, placed.maximum.x), c}, target.id};
    }
    return result;
}

HandleSnapResult BoundsSnapping::resolveHandle(const Document& document, Vec2d point, Vec2d direction, const SnapOptions& options)
{
    HandleSnapResult result {point, {}};
    const double length = std::hypot(direction.x, direction.y);
    if (!start_ || !std::isfinite(point.x) || !std::isfinite(point.y)
        || !std::isfinite(length) || length < 1e-9 || !std::isfinite(options.logicalScale)
        || options.logicalScale <= 0 || !options.enabled || options.bypass) {
        active_ = {}; return result;
    }
    if (revision_ != document.revision()) {
        for (const auto id : moving_)
            if (!document.containsLayer(id)) { clear(); return result; }
        rebuild(document);
    }
    direction = direction * (1.0 / length);
    if ((!options.allowX && std::abs(direction.x) > 1e-9)
        || (!options.allowY && std::abs(direction.y) > 1e-9)) { active_ = {}; return result; }
    const double capture = capturePixels / options.logicalScale, release = releasePixels / options.logicalScale;
    const auto available = [&](const Anchor& a) { return a.id == 0 ? options.canvas : options.layers; };
    const auto distance = [&](unsigned axis, std::size_t target) {
        return (anchors_[axis][target].coordinate - (axis == 0 ? point.x : point.y))
            / (axis == 0 ? direction.x : direction.y);
    };
    using Rank = std::tuple<double, LayerId, unsigned, unsigned>;
    std::optional<Rank> best;
    unsigned chosenAxis = 0;
    std::size_t chosenTarget = 0;
    const auto consider = [&](unsigned axis, std::size_t target, double limit) {
        const auto& anchor = anchors_[axis][target];
        const auto shift = distance(axis, target);
        const Rank rank {std::abs(shift), anchor.id, axis, anchor.kind};
        if (available(anchor) && std::abs(shift) <= limit && (!best || rank < *best)) {
            best = rank; chosenAxis = axis; chosenTarget = target;
        }
    };
    for (unsigned axis = 0; axis < 2; ++axis) {
        if (std::abs(axis == 0 ? direction.x : direction.y) < 1e-9) { active_[axis].reset(); continue; }
        if (active_[axis]) consider(axis, active_[axis]->target, release);
    }
    if (!best) for (unsigned axis = 0; axis < 2; ++axis) {
        const auto component = axis == 0 ? direction.x : direction.y;
        if (std::abs(component) < 1e-9) continue;
        const auto coordinate = axis == 0 ? point.x : point.y;
        const auto radius = capture * std::abs(component);
        const auto& anchors = anchors_[axis];
        auto it = std::lower_bound(anchors.begin(), anchors.end(), coordinate - radius,
            [](const Anchor& a, double value) { return a.coordinate < value; });
        for (; it != anchors.end() && it->coordinate <= coordinate + radius; ++it)
            consider(axis, std::size_t(it - anchors.begin()), capture);
    }
    active_ = {};
    if (!best) return result;
    result.position = point + direction * distance(chosenAxis, chosenTarget);
    active_[chosenAxis] = Alignment {chosenTarget, 0};
    const auto& target = anchors_[chosenAxis][chosenTarget];
    const auto& b = target.bounds;
    const auto c = target.coordinate;
    result.guides[chosenAxis] = chosenAxis == 0
        ? SnapGuide {{c, std::min(b.minimum.y, result.position.y)}, {c, std::max(b.maximum.y, result.position.y)}, target.id}
        : SnapGuide {{std::min(b.minimum.x, result.position.x), c}, {std::max(b.maximum.x, result.position.x), c}, target.id};
    return result;
}
} // namespace imageeditor::core
