#include "imageeditor/core/LayerTransform.hpp"
#include "imageeditor/core/LayerGeometry.hpp"
#include "imageeditor/core/TransformSupport.hpp"
#include <cmath>
#include <memory>
#include <numbers>

namespace imageeditor::core {
namespace {
    constexpr double radians = std::numbers::pi / 180.0;
    constexpr std::array<Vec2d, 8> unitHandles { { { 0, 0 }, { 0.5, 0 }, { 1, 0 }, { 1, 0.5 },
        { 1, 1 }, { 0.5, 1 }, { 0, 1 }, { 0, 0.5 } } };
    double dot(Vec2d a, Vec2d b)
    {
        return a.x * b.x + a.y * b.y;
    }
    double length(Vec2d v)
    {
        return std::hypot(v.x, v.y);
    }
    bool finite(Vec2d v)
    {
        return std::isfinite(v.x) && std::isfinite(v.y);
    }
    double safeScale(double value, double previous)
    {
        const auto sign = value == 0.0 ? previous : value;
        return std::copysign(
            std::clamp(std::abs(value), kMinimumLayerScale, kMaximumLayerScale), sign);
    }
    std::optional<ResizedGeometryBox> resolveProjectiveResize(Extent2d extent,
        const TransformValues& values, TransformHandle handle, Vec2d delta,
        TransformModifiers modifiers, bool aspectLocked)
    {
        const auto unit = unitHandles[std::size_t(handle)];
        const Vec2d anchorUnit = modifiers.alt ? Vec2d{.5,.5} : Vec2d{1-unit.x,1-unit.y};
        const Vec2d anchor{anchorUnit.x*extent.width,anchorUnit.y*extent.height};
        // Resizing scales the affine factor AFTER the retained distortion.
        // Its handle trajectory is linear in the scale factors. Inverting the
        // full homography at a free pointer instead crosses its inverse horizon
        // just outside a thin quad, producing enormous, sign-changing deltas.
        const auto handles = geometryTransformHandles(values.distortion,extent);
        const auto span = handles[std::size_t(handle)]-values.distortion.map(anchor);
        const auto c=std::cos(values.rotationDegrees*radians), s=std::sin(values.rotationDegrees*radians);
        const Vec2d x{c*values.scaleX*span.x,s*values.scaleX*span.x};
        const Vec2d y{(c*values.shear-s)*values.scaleY*span.y,
            (s*values.shear+c)*values.scaleY*span.y};
        if (!finite(x) || !finite(y)) return {};
        double fx=1,fy=1;
        const bool uniform=aspectLocked!=modifiers.shift;
        if (uniform || unit.x==.5 || unit.y==.5) {
            const auto direction=uniform ? x+y : unit.x==.5 ? y : x;
            const auto denominator=dot(direction,direction);
            if (!std::isfinite(denominator) || denominator<1e-12) return {};
            const auto factor=1+dot(delta,direction)/denominator;
            if (uniform || unit.x!=.5) fx=factor;
            if (uniform || unit.y!=.5) fy=factor;
        } else {
            // Two-axis corner resize: solve the affine basis, never an inverse
            // projective map of the pointer. A collapsed basis keeps the last
            // valid state through the caller's existing rejection policy.
            const auto inverse=AffineTransform{x.x,y.x,0,x.y,y.y,0}.inverted();
            if (!inverse) return {};
            const auto factors=inverse->map(delta);
            fx+=factors.x;fy+=factors.y;
        }
        const Extent2d next{extent.width*fx,extent.height*fy};
        if (!std::isfinite(next.width) || !std::isfinite(next.height)) return {};
        return ResizedGeometryBox{next,anchorUnit};
    }
    bool equivalent(const AffineTransform& a, const AffineTransform& b, Extent2d extent)
    {
        for (auto point :
            { Vec2d { }, Vec2d { double(extent.width), 0 }, Vec2d { double(extent.width), double(extent.height) }, Vec2d { 0, double(extent.height) } }) {
            if (length(a.map(point) - b.map(point)) > 1.0e-8)
                return false;
        }
        return true;
    }
    bool target(const Document& doc, LayerId id, SurfaceId surface, Extent2u extent)
    {
        const auto* layer = doc.layer(id);
        if (surface == 0)
            return layer && (std::holds_alternative<TextLayer>(layer->payload) || std::holds_alternative<ShapeLayer>(layer->payload));
        const auto* raster = layer ? std::get_if<RasterLayer>(&layer->payload) : nullptr;
        return raster && raster->surface && raster->surface->id() == surface
                && raster->surface->extent() == extent
            ? true
            : false;
    }
    class TransformCommand final : public Command {
    public:
        TransformCommand(const std::vector<LayerTransformTarget>& targets,
            std::vector<LayerTransformUpdate> updates)
            : targets_(targets)
            , updates_(std::move(updates))
        {
        }
        bool apply(Document& doc) override
        {
            return available(doc) && doc.setLayerTransforms(updates_);
        }
        bool undo(Document& doc) override
        {
            if (!available(doc))
                return false;
            for (auto& update : updates_)
                std::swap(update.before, update.after);
            const auto result = doc.setLayerTransforms(updates_);
            for (auto& update : updates_)
                std::swap(update.before, update.after);
            return result;
        }
        bool canAdoptApplied(const Document& doc) const noexcept override
        {
            if (!available(doc))
                return false;
            return std::ranges::all_of(updates_, [&doc](const auto& update) {
                return doc.layer(update.id)->localToDocument == update.after;
            });
        }
        std::string_view label() const noexcept override { return targets_.size() > 1 ? "Transform layers" : "Layer transform"; }
        std::size_t memoryCost() const noexcept override { return sizeof(*this)
            + targets_.capacity() * sizeof(LayerTransformTarget) + updates_.capacity() * sizeof(LayerTransformUpdate); }

    private:
        bool available(const Document& doc) const noexcept
        {
            return std::ranges::all_of(targets_, [&doc](const auto& t) {
                return target(doc, t.id, t.surface, t.extent) && doc.layer(t.id)->payload.index() == t.payloadKind;
            });
        }
        std::vector<LayerTransformTarget> targets_;
        std::vector<LayerTransformUpdate> updates_;
    };
}

AffineTransform transformFromValues(const TransformValues& v, Extent2u extent)
{
    return transformFromGeometryValues(v, { double(extent.width), double(extent.height) });
}
AffineTransform transformFromGeometryValues(const TransformValues& v, Extent2d extent)
{
    const auto c = std::cos(v.rotationDegrees * radians), s = std::sin(v.rotationDegrees * radians);
    AffineTransform result { c * v.scaleX, (c * v.shear - s) * v.scaleY, 0, s * v.scaleX,
        (s * v.shear + c) * v.scaleY, 0 };
    const auto offset = result.map({ extent.width * 0.5, extent.height * 0.5 });
    result.m02 = v.center.x - offset.x;
    result.m12 = v.center.y - offset.y;
    return composeTransform(result,v.distortion);
}

std::optional<TransformValues> valuesFromTransform(const AffineTransform& t, Extent2u extent)
{
    return geometryValuesFromTransform(t, { double(extent.width), double(extent.height) });
}
std::optional<TransformValues> geometryValuesFromTransform(const AffineTransform& t, Extent2d extent)
{
    if (extent.width <= 0 || extent.height <= 0 || !t.inverted())
        return { };
    const auto derivative=t.derivatives({extent.width*.5,extent.height*.5});
    const auto sx = std::hypot(derivative[0].x, derivative[0].y);
    const auto sy = (derivative[0].x / sx) * derivative[1].y - (derivative[0].y / sx) * derivative[1].x;
    if (!std::isfinite(sx) || !std::isfinite(sy) || sx == 0.0 || sy == 0.0)
        return { };
    TransformValues result { t.map({ extent.width * 0.5, extent.height * 0.5 }), sx, sy,
        std::atan2(derivative[0].y, derivative[0].x) / radians,
        (derivative[0].x / sx) * (derivative[1].x / sy) + (derivative[0].y / sx) * (derivative[1].y / sy) };
    if (!finite(result.center) || !std::isfinite(result.rotationDegrees)
        || !std::isfinite(result.shear))
        return { };
    if(!t.isAffine()) {
        const auto base=transformFromGeometryValues(result,extent).inverted();
        if(!base)return {};
        result.distortion=composeTransform(*base,t);
    }
    return result;
}

std::array<Vec2d, 8> transformHandles(const AffineTransform& t, Extent2u extent)
{
    return geometryTransformHandles(t, { double(extent.width), double(extent.height) });
}

std::array<Vec2d, 8> geometryTransformHandles(const AffineTransform& t, Extent2d extent)
{
    std::array<Vec2d, 8> result;
    for (std::size_t i = 0; i < result.size(); i += 2)
        result[i] = t.map({ unitHandles[i].x * extent.width, unitHandles[i].y * extent.height });
    // A projective map preserves edges, but not their midpoints. Keep the
    // input/resize markers at the visible edge centers drawn by transform.frag.
    for (std::size_t i = 1; i < result.size(); i += 2)
        result[i] = (result[i - 1] + result[(i + 1) % result.size()]) * .5;
    return result;
}

std::optional<ResizedGeometryBox> resolveResizeGeometry(Extent2d extent,
    const AffineTransform& start, TransformHandle handle, Vec2d press, Vec2d point,
    TransformModifiers modifiers, bool aspectLocked)
{
    if (handle < TransformHandle::TopLeft || handle > TransformHandle::Left
        || !finite(press) || !finite(point) || !std::isfinite(extent.width)
        || !std::isfinite(extent.height) || extent.width < 0 || extent.height < 0)
        return { };
    const auto inverse = start.inverted();
    if (!inverse)
        return { };
    const auto unit = unitHandles[std::size_t(handle)];
    const Vec2d anchorUnit = modifiers.alt ? Vec2d { .5, .5 } : Vec2d { 1 - unit.x, 1 - unit.y };
    const auto localDelta = inverse->map(point) - inverse->map(press);
    auto next = extent;
    if (unit.x != .5)
        next.width += localDelta.x / (unit.x - anchorUnit.x);
    if (unit.y != .5)
        next.height += localDelta.y / (unit.y - anchorUnit.y);
    if (aspectLocked != modifiers.shift) {
        const Vec2d diagonalLocal { (unit.x - anchorUnit.x) * extent.width,
            (unit.y - anchorUnit.y) * extent.height };
        const Vec2d anchor{anchorUnit.x*extent.width,anchorUnit.y*extent.height};
        const auto diagonal = start.map(anchor+diagonalLocal)-start.map(anchor);
        const double denominator = dot(diagonal, diagonal);
        const double factor = denominator > 0 ? 1 + dot(point - press, diagonal) / denominator : 1;
        next = { extent.width * factor, extent.height * factor };
    }
    if (!std::isfinite(next.width) || !std::isfinite(next.height))
        return { };
    return ResizedGeometryBox { next, anchorUnit };
}

TransformHandle hitTestTransform(
    const std::array<Vec2d, 8>& handles, Vec2d point, double handleRadius, double rotationRadius)
{
    if (!finite(point))
        return TransformHandle::None;
    double closest = handleRadius;
    TransformHandle hit = TransformHandle::None;
    // Corners win exact ties when a tiny/flattened box overlaps its handles.
    for (int i : { 0, 2, 4, 6, 1, 3, 5, 7 }) {
        const auto distance = length(point - handles[std::size_t(i)]);
        if (distance < closest) {
            closest = distance;
            hit = static_cast<TransformHandle>(i);
        }
    }
    if (hit != TransformHandle::None)
        return hit;
    bool positive = false, negative = false;
    for (std::size_t i = 0; i < 8; i += 2) {
        const auto a = handles[(i + 2) % 8] - handles[i], b = point - handles[i];
        const auto cross = a.x * b.y - a.y * b.x;
        positive |= cross > 1.0e-8;
        negative |= cross < -1.0e-8;
    }
    if (positive != negative)
        return TransformHandle::Move;
    for (std::size_t i = 0; i < 8; i += 2)
        if (length(point - handles[i]) <= rotationRadius)
            return TransformHandle::Rotate;
    return TransformHandle::None;
}

LayerTransformSession::LayerTransformSession(Document& document, LayerId id)
    : LayerTransformSession(document, std::span<const LayerId>(&id, 1))
{
}
LayerTransformSession::LayerTransformSession(Document& document, std::span<const LayerId> ids)
    : document_(&document)
    , entryContentState_(document.contentState())
{
    if (ids.empty() || std::ranges::any_of(ids,[&](LayerId id) { return !document.containsItem(id); }))
        return;
    const auto normalized=document.tree().normalize(ids);
    for(auto id:normalized) {
        const auto* container=document.tree().container(id);
        if(container && container->kind==ContainerKind::Folder)return;
    }
    const auto expanded=document.expandedLayers(normalized);
    if (expanded.empty()) return;
    const auto capture=[&](auto&& self,LayerId id)->void {
        if(const auto* c=document.tree().container(id)) {
            memberships_.push_back({id,c->kind,c->children});
            for(auto child:c->children)self(self,child);
        }
    };
    for(auto id:normalized)capture(capture,id);
    Vec2d minimum { std::numeric_limits<double>::max(), std::numeric_limits<double>::max() };
    Vec2d maximum { -minimum.x, -minimum.y };
    for (const auto id : expanded) {
        if (std::ranges::any_of(targets_, [id](const auto& t) { return t.id == id; }))
            continue;
        const auto* layer = document.layer(id);
        if (!layer || !layer->localToDocument.inverted())
            return;
        const auto extent = layerGeometryExtent(*layer);
        if (extent.empty())
            return;
        const auto* raster = std::get_if<RasterLayer>(&layer->payload);
        if (raster && !raster->surface)
            return;
        targets_.push_back({ id, raster ? raster->surface->id() : 0, extent, layer->payload.index(),
            layer->localToDocument, layer->localToDocument });
        const auto bounds=layerInteractionBounds(*layer);
        // Retain empty-cropped members as transform targets (so uncropping
        // later preserves group placement), but not as frame contributors.
        if(bounds.empty())continue;
        const auto size=Extent2d{bounds.width,bounds.height};
        const auto frame=composeAffine(layer->localToDocument,{1,0,bounds.x,0,1,bounds.y});
        for (const auto p : geometryTransformHandles(frame, size)) {
            minimum.x = std::min(minimum.x, p.x);
            minimum.y = std::min(minimum.y, p.y);
            maximum.x = std::max(maximum.x, p.x);
            maximum.y = std::max(maximum.y, p.y);
        }
    }
    layerId_ = targets_.front().id;
    extent_ = targets_.front().extent;
    geometryExtent_ = { double(extent_.width), double(extent_.height) };
    original_ = targets_.front().original;
    if(!grouped() && (document.layer(layerId_)->crop || document.layer(layerId_)->rasterOrigin!=Vec2d{})){
        const auto bounds=layerInteractionBounds(*document.layer(layerId_));
        geometryExtent_={bounds.width,bounds.height};
        original_=composeAffine(original_,{1,0,bounds.x,0,1,bounds.y});
        reframed_=true;
    }
    if (grouped()) {
        geometryExtent_ = { std::max(maximum.x - minimum.x, 1.0e-6), std::max(maximum.y - minimum.y, 1.0e-6) };
        if (!finite(minimum) || !finite(maximum) || geometryExtent_.width > 1.0e9 || geometryExtent_.height > 1.0e9)
            return;
        extent_ = { static_cast<std::uint32_t>(std::ceil(geometryExtent_.width)), static_cast<std::uint32_t>(std::ceil(geometryExtent_.height)) };
        original_ = { 1, 0, minimum.x, 0, 1, minimum.y };
    }
    const auto values = geometryValuesFromTransform(original_, geometryExtent_);
    if (!values)
        return;
    // Do not silently clamp externally supplied, unsupported geometry on the
    // first move. Normal editor-created transforms always satisfy this range.
    if (std::abs(values->scaleX) < kMinimumLayerScale * (1.0 - 1e-12)
        || std::abs(values->scaleX) > kMaximumLayerScale * (1.0 + 1e-12)
        || std::abs(values->scaleY) < kMinimumLayerScale * (1.0 - 1e-12)
        || std::abs(values->scaleY) > kMaximumLayerScale * (1.0 + 1e-12))
        return;
    values_ = *values;
    current_ = actionStart_ = original_;
    actionStartValues_ = values_;
    actionValues_.push_back(values_);
    actionFrames_.push_back(current_);
    updates_.reserve(targets_.size());
    active_ = true;
}
LayerTransformSession::~LayerTransformSession()
{
    cancel();
}
bool LayerTransformSession::targetAvailable() const noexcept
{
    for(const auto& membership:memberships_) {
        const auto* container=document_->tree().container(membership.id);
        if(!container || container->kind!=membership.kind || container->children.size()!=membership.children.size())return false;
        if(container->children!=membership.children)
            for(auto id:membership.children)if(std::ranges::find(container->children,id)==container->children.end())return false;
    }
    return active_ && std::ranges::all_of(targets_, [this](const auto& t) {
        return target(*document_, t.id, t.surface, t.extent)
            && document_->layer(t.id)->payload.index() == t.payloadKind
            && document_->layer(t.id)->localToDocument == t.current;
    });
}
bool LayerTransformSession::setFrame(const AffineTransform& frame)
{
    if (!targetAvailable())
        return false;
    updates_.clear();
    const auto delta = composeAffine(frame, *original_.inverted());
    for (const auto& t : targets_) {
        // Keep single-target math and exact entry bits intact.
        const auto matrix = frame == original_ ? t.original : (grouped()||reframed_) ? composeAffine(delta, t.original)
                                                                        : frame;
        const auto& layer=*document_->layer(t.id);
        if (!matrix.inverted() || (!matrix.isAffine() && !matrix.validOver(
                transformEvaluationSupport(intrinsicLocalBounds(layer),layer.filters,layer.effects))))
            return false;
        updates_.push_back({ t.id, t.current, matrix });
    }
    const bool changed = std::ranges::any_of(updates_, [](const auto& u) { return u.before != u.after; });
    if (changed && !document_->setLayerTransforms(updates_))
        return false;
    for (std::size_t i = 0; i < targets_.size(); ++i)
        targets_[i].current = updates_[i].after;
    current_ = frame;
    return true;
}
bool LayerTransformSession::setValues(TransformValues v)
{
    if (!targetAvailable() || !finite(v.center) || std::abs(v.center.x) > 1.0e9
        || std::abs(v.center.y) > 1.0e9 || !std::isfinite(v.scaleX) || !std::isfinite(v.scaleY)
        || !std::isfinite(v.rotationDegrees))
        return false;
    v.shear = values_.shear;
    v.distortion = values_.distortion;
    v.scaleX = safeScale(v.scaleX, values_.scaleX);
    v.scaleY = safeScale(v.scaleY, values_.scaleY);
    v.rotationDegrees = std::remainder(v.rotationDegrees, 360.0);
    auto matrix = transformFromGeometryValues(v, geometryExtent_);
    if (!matrix.inverted())
        return false;
    if (equivalent(matrix, original_, geometryExtent_))
        matrix = original_;
    if (!setFrame(matrix))
        return false;
    values_ = v;
    return true;
}
bool LayerTransformSession::flip(bool horizontal)
{
    (void)completeAction();
    auto v = values_;
    (horizontal ? v.scaleX : v.scaleY) *= -1.0;
    return setValues(v) && completeAction();
}
bool LayerTransformSession::beginDrag(TransformHandle handle, Vec2d point)
{
    if (!targetAvailable() || !finite(point) || handle <= TransformHandle::None
        || handle > TransformHandle::Rotate)
        return false;
    (void)completeAction();
    dragStart_ = current_;
    dragValues_ = values_;
    handle_ = handle;
    drag_.emplace(extent_, current_, values_, handle, point, geometryExtent_);
    dragPress_ = point;
    snapMovementStarted_ = false;
    snapGuides_ = {};
    snapping_.clear();
    if (handle == TransformHandle::Move || (handle >= TransformHandle::TopLeft
        && handle <= TransformHandle::Left && int(handle) % 2 == 1)) {
        std::vector<LayerId> ids;
        ids.reserve(targets_.size());
        for (const auto& t : targets_) ids.push_back(t.id);
        (void)snapping_.begin(*document_, ids);
    }
    return true;
}
bool LayerTransformSession::dragTo(Vec2d point, TransformModifiers modifiers, bool aspectLocked, SnapOptions options)
{
    snapGuides_ = {};
    if (!dragging() || !targetAvailable() || !finite(point))
        return false;
    SnapGuides guides;
    if (handle_ == TransformHandle::Move && (snapMovementStarted_ || point != dragPress_)) {
        snapMovementStarted_ = true;
        options.bypass |= modifiers.control;
        const auto snapped = snapping_.resolve(*document_, point - dragPress_, options);
        point = dragPress_ + snapped.translation;
        guides = snapped.guides;
    }
    auto next = drag_->resolve(point, modifiers, aspectLocked, values_);
    if (next && point != dragPress_ && handle_ != TransformHandle::Move)
        guides = drag_->snapEdge(*document_, snapping_, *next, modifiers, aspectLocked, options);
    bool applied=false;
    if(next) {
        const auto frame=transformFromGeometryValues(*next,geometryExtent_);
        applied=setFrame(frame);
        if(applied)values_=*next;
    }
    if (applied) {
        snapGuides_ = guides;
        snapping_.acknowledgeTranslation(document_->revision());
    }
    return applied;
}

TransformDrag::TransformDrag(Extent2u extent, AffineTransform start, TransformValues values,
    TransformHandle handle, Vec2d press, std::optional<Extent2d> geometryExtent)
    : extent_(geometryExtent.value_or(Extent2d { double(extent.width), double(extent.height) }))
    , start_(start)
    , values_(values)
    , handle_(handle)
    , press_(press)
    , previousAngle_(std::atan2(press.y - values.center.y, press.x - values.center.x) / radians)
{
}
std::optional<TransformValues> TransformDrag::resolve(Vec2d point,
    TransformModifiers modifiers, bool aspectLocked, const TransformValues& current)
{
    if (!finite(point) || extent_.width <= 0 || extent_.height <= 0 || handle_ <= TransformHandle::None
        || handle_ > TransformHandle::Rotate)
        return { };
    auto v = values_;
    const auto delta = point - press_;
    if(modifiers.control && handle_>=TransformHandle::TopLeft && handle_<=TransformHandle::Left && int(handle_)%2==0) {
        const auto handles=geometryTransformHandles(start_,extent_);
        std::array<Vec2d,4> quad{handles[0],handles[2],handles[4],handles[6]};
        quad[std::size_t(handle_)/2]=quad[std::size_t(handle_)/2]+delta;
        const auto matrix=rectangleToQuad({0,0,extent_.width,extent_.height},quad);
        return matrix?geometryValuesFromTransform(*matrix,extent_):std::nullopt;
    }
    if (handle_ == TransformHandle::Move) {
        v.center = v.center + delta;
    } else if (handle_ == TransformHandle::Rotate) {
        if (length(point - v.center) > 1.0e-6) {
            const auto angle = std::atan2(point.y - v.center.y, point.x - v.center.x) / radians;
            accumulatedAngle_ += std::remainder(angle - previousAngle_, 360.0);
            previousAngle_ = angle;
        }
        v.rotationDegrees += accumulatedAngle_;
        if (modifiers.shift)
            v.rotationDegrees = std::round(v.rotationDegrees / 15.0) * 15.0;
    } else {
        if (!start_.isAffine() && point==press_) return v;
        const auto box = start_.isAffine()
            ? resolveResizeGeometry(extent_, start_, handle_, press_, point, modifiers, aspectLocked)
            : resolveProjectiveResize(extent_, values_, handle_, delta, modifiers, aspectLocked);
        if (!box)
            return { };
        const Vec2d anchor { box->unitAnchor.x * extent_.width, box->unitAnchor.y * extent_.height };
        double fx = box->signedExtent.width / extent_.width;
        double fy = box->signedExtent.height / extent_.height;
        if (aspectLocked != modifiers.shift) {
            const auto minimumFactor = std::max(
                kMinimumLayerScale / std::abs(v.scaleX), kMinimumLayerScale / std::abs(v.scaleY));
            const auto maximumFactor = std::min(
                kMaximumLayerScale / std::abs(v.scaleX), kMaximumLayerScale / std::abs(v.scaleY));
            if (minimumFactor <= maximumFactor) {
                const auto sign = fx == 0.0 ? current.scaleX / v.scaleX : fx;
                fx = fy
                    = std::copysign(std::clamp(std::abs(fx), minimumFactor, maximumFactor), sign);
            }
        }
        v.scaleX = safeScale(v.scaleX * fx, current.scaleX);
        v.scaleY = safeScale(v.scaleY * fy, current.scaleY);
        v.center = { 0, 0 };
        const auto linear = transformFromGeometryValues(v, extent_);
        v.center = start_.map(anchor) - linear.map(anchor);
    }
    return v;
}

SnapGuides TransformDrag::snapEdge(const Document& document, BoundsSnapping& snapping,
    TransformValues& resolved, TransformModifiers modifiers, bool aspectLocked, SnapOptions options)
{
    if (handle_ < TransformHandle::TopLeft || handle_ > TransformHandle::Left || int(handle_) % 2 == 0)
        return {}; // Corners (including Ctrl-distortion) and rotation never snap.
    const auto unit = unitHandles[std::size_t(handle_)];
    const bool horizontal = unit.x != .5, uniform = aspectLocked != modifiers.shift;
    const Vec2d anchorUnit = modifiers.alt ? Vec2d{.5,.5} : Vec2d{1-unit.x,1-unit.y};
    const Vec2d anchor {anchorUnit.x * extent_.width, anchorUnit.y * extent_.height};
    const auto fixed = start_.map(anchor);
    const auto atFactor = [&](double factor) {
        auto v = values_;
        if (uniform || horizontal) v.scaleX *= factor;
        if (uniform || !horizontal) v.scaleY *= factor;
        v.center = {};
        v.center = fixed - transformFromGeometryValues(v, extent_).map(anchor);
        return v;
    };
    const double factor = horizontal ? resolved.scaleX / values_.scaleX : resolved.scaleY / values_.scaleY;
    const auto handlePoint = [&](const TransformValues& v) {
        return geometryTransformHandles(transformFromGeometryValues(v, extent_), extent_)[std::size_t(handle_)];
    };
    const auto point = handlePoint(resolved);
    const auto direction = handlePoint(atFactor(factor + 1)) - point;
    const auto denominator = dot(direction, direction);
    if (!std::isfinite(denominator) || denominator < 1e-12) return {};
    options.bypass |= modifiers.control;
    const auto snap = snapping.resolveHandle(document, point, direction, options);
    if (!snap.guides[0] && !snap.guides[1]) return {};
    auto candidate = atFactor(factor + dot(snap.position - point, direction) / denominator);
    if (std::abs(candidate.scaleX) < kMinimumLayerScale || std::abs(candidate.scaleY) < kMinimumLayerScale
        || std::abs(candidate.scaleX) > kMaximumLayerScale || std::abs(candidate.scaleY) > kMaximumLayerScale)
        return {};
    const auto frame = transformFromGeometryValues(candidate, extent_);
    if (!frame.inverted() || length(handlePoint(candidate) - snap.position) > 1e-6) return {};
    resolved = candidate;
    return snap.guides;
}
void LayerTransformSession::cancelDrag()
{
    snapGuides_ = {};
    snapping_.clear();
    if (dragging() && targetAvailable()) {
        (void)setFrame(dragStart_);
        values_ = dragValues_;
    }
    handle_ = TransformHandle::None;
}
void LayerTransformSession::endDrag()
{
    snapGuides_ = {};
    snapping_.clear();
    handle_ = TransformHandle::None;
    (void)completeAction();
}
bool LayerTransformSession::completeAction()
{
    if (!targetAvailable())
        return false;
    if (equivalent(current_, actionStart_, geometryExtent_)) {
        // Exact command guards require the original bits, not an epsilon match.
        (void)setFrame(actionStart_);
        values_ = actionStartValues_;
        return false;
    }
    const auto next = pendingHistory_.undoDepth() + 1;
    actionValues_.reserve(next + 1);
    actionFrames_.reserve(next + 1);
    std::vector<LayerTransformUpdate> changes;
    changes.reserve(targets_.size());
    const auto beforeDelta = composeAffine(actionStart_, *original_.inverted());
    for (const auto& t : targets_)
        changes.push_back({ t.id,
            actionStart_ == original_ ? t.original : (grouped()||reframed_) ? composeAffine(beforeDelta, t.original)
                                                               : actionStart_,
            t.current });
    std::unique_ptr<Command> command = std::make_unique<TransformCommand>(targets_, std::move(changes));
    if (!pendingHistory_.adoptApplied(*document_, command))
        return false;
    actionValues_.resize(next);
    actionValues_.push_back(values_);
    actionFrames_.resize(next);
    actionFrames_.push_back(current_);
    actionStart_ = current_;
    actionStartValues_ = values_;
    return true;
}
bool LayerTransformSession::undo()
{
    if (!targetAvailable())
        return false;
    cancelDrag();
    (void)completeAction();
    if (!pendingHistory_.undo(*document_))
        return false;
    actionStartValues_ = values_ = actionValues_[pendingHistory_.undoDepth()];
    actionStart_ = current_ = actionFrames_[pendingHistory_.undoDepth()];
    synchronizeTargets();
    return true;
}
bool LayerTransformSession::redo()
{
    if (!targetAvailable())
        return false;
    cancelDrag();
    (void)completeAction();
    if (!pendingHistory_.redo(*document_))
        return false;
    actionStartValues_ = values_ = actionValues_[pendingHistory_.undoDepth()];
    actionStart_ = current_ = actionFrames_[pendingHistory_.undoDepth()];
    synchronizeTargets();
    return true;
}
TransformCommitResult LayerTransformSession::commit(History& history)
{
    if (!targetAvailable()) {
        cancel();
        return TransformCommitResult::TargetUnavailable;
    }
    endDrag();
    if (!pendingHistory_.canUndo()) {
        active_ = false;
        return TransformCommitResult::NoChange;
    }
    if (!history.publishAppliedBranch(*document_, pendingHistory_)) {
        // Publication failure is not permission to discard completed gestures.
        // Keep the session alive so the UI can retry or explicitly cancel.
        return TransformCommitResult::TargetUnavailable;
    }
    active_ = false;
    return TransformCommitResult::Committed;
}
void LayerTransformSession::cancel()
{
    snapGuides_ = {};
    snapping_.clear();
    const bool available = targetAvailable();
    if (available) {
        (void)setFrame(original_);
        document_->restoreContentState(entryContentState_);
    } else if (active_)
        (void)restoreSurvivingTargets();
    active_ = false;
    handle_ = TransformHandle::None;
    pendingHistory_.clear();
}
void LayerTransformSession::synchronizeTargets()
{
    for (auto& t : targets_)
        t.current = document_->layer(t.id)->localToDocument;
}
bool LayerTransformSession::restoreSurvivingTargets()
{
    // A removed target invalidates the group; never leave our previews on its
    // surviving peers or overwrite unrelated external edits. Do not resurrect
    // missing layers or rewind an external command's content-state marker.
    updates_.clear();
    for (const auto& t : targets_)
        if (target(*document_, t.id, t.surface, t.extent)
            && document_->layer(t.id)->payload.index() == t.payloadKind
            && document_->layer(t.id)->localToDocument == t.current)
            updates_.push_back({ t.id, t.current, t.original });
    return document_->setLayerTransforms(updates_);
}
} // namespace imageeditor::core
