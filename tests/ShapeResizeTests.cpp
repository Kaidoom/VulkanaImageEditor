#include "imageeditor/core/ShapeResize.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>

namespace c = imageeditor::core;
namespace {
int failures = 0;
void check(bool condition, std::string_view message)
{
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}
void near(double a, double b, std::string_view message)
{
    check(std::isfinite(a) && std::isfinite(b) && std::abs(a - b) <= 1e-8, message);
}
void near(c::Vec2d a, c::Vec2d b, std::string_view message)
{
    near(a.x, b.x, message);
    near(a.y, b.y, message);
}
constexpr std::array<c::Vec2d, 8> units { { { 0, 0 }, { .5, 0 }, { 1, 0 }, { 1, .5 },
    { 1, 1 }, { .5, 1 }, { 0, 1 }, { 0, .5 } } };
c::Vec2d linear(const c::AffineTransform& t, c::Vec2d p)
{
    return { t.m00 * p.x + t.m01 * p.y, t.m10 * p.x + t.m11 * p.y };
}
c::ShapeLayer shape(c::ShapeKind kind = c::ShapeKind::Rectangle)
{
    c::ShapeLayer result;
    result.kind = kind;
    result.size = { 100.125, 50.75 };
    result.strokeEnabled = true;
    result.strokeWidth = 4;
    result.cornerRadius = 13.5;
    if (kind == c::ShapeKind::Line)
        result.points = { { 2.25, 47.125 }, { 95.625, 1.5 } };
    if (kind == c::ShapeKind::Polygon)
        result.points = { { 0, 0 }, { 100.125, 0 }, { 100.125, 50.75 },
            { 50, 10.5 }, { 0, 50.75 } };
    return result;
}
c::ShapeGeometryState state(const c::Document& doc, c::LayerId id)
{
    const auto* layer = doc.layer(id);
    return { std::get<c::ShapeLayer>(layer->payload), layer->localToDocument };
}

void handlesAnchorsAndConstraints()
{
    const auto original = shape(c::ShapeKind::RoundedRectangle);
    for (const c::AffineTransform baseline : { c::AffineTransform { },
             { 0, -1, 170.25, 1, 0, -33.5 }, { -1, 0, 40, 0, -1, 75 },
             { -1.5, .3, 400.125, .4, .6, -75.25 } }) {
        const auto handles = c::geometryTransformHandles(baseline, original.size);
        for (std::size_t index = 0; index < units.size(); ++index) {
            const auto unit = units[index];
            near(handles[index], baseline.map({ unit.x * original.size.width, unit.y * original.size.height }), "handles use the exact subpixel geometry frame");
            for (const bool center : { false, true }) {
                const c::Vec2d anchorUnit = center ? c::Vec2d { .5, .5 }
                                                   : c::Vec2d { 1 - unit.x, 1 - unit.y };
                const c::Vec2d direction { (unit.x - anchorUnit.x) * original.size.width,
                    (unit.y - anchorUnit.y) * original.size.height };
                for (const bool constrained : { false, true }) {
                    for (const double factor : { .35, 1.75, -.65 }) {
                        const double fx = constrained || unit.x != .5 ? factor : 1;
                        const double fy = constrained || unit.y != .5 ? factor : 1;
                        // Press need not be at the mathematical center of the
                        // handle: comfortable hit tolerance must not jump it.
                        const auto press = handles[index] + c::Vec2d { .7, -.3 };
                        const auto point = press + linear(baseline, { direction.x * (fx - 1), direction.y * (fy - 1) });
                        c::ShapeResizeGesture gesture(original, baseline,
                            static_cast<c::TransformHandle>(index), press);
                        const auto resolved = gesture.resolve(point, { constrained, center });
                        check(resolved.has_value(), "all shape handles resolve under every baseline affine");
                        if (!resolved)
                            continue;
                        near(resolved->shape.size.width, original.size.width * std::abs(fx),
                            "resize magnitude goes into authoritative width");
                        near(resolved->shape.size.height, original.size.height * std::abs(fy),
                            "resize magnitude goes into authoritative height");
                        const auto& t = resolved->transform;
                        near(t.m00, baseline.m00 * std::copysign(1., fx), "first affine column retains only intentional flip");
                        near(t.m10, baseline.m10 * std::copysign(1., fx), "rotated first column is not resized");
                        near(t.m01, baseline.m01 * std::copysign(1., fy), "second affine column retains prior shear/scale");
                        near(t.m11, baseline.m11 * std::copysign(1., fy), "rotated second column is not resized");
                        near(t.map({ anchorUnit.x * resolved->shape.size.width,
                                 anchorUnit.y * resolved->shape.size.height }),
                            baseline.map({ anchorUnit.x * original.size.width,
                                anchorUnit.y * original.size.height }),
                            "opposite handle or Alt center remains anchored even across flips");
                        check(resolved->shape.strokeWidth == 4 && resolved->shape.cornerRadius == 13.5,
                            "intrinsic resize retains configured stroke width and requested radius");
                        check(t.inverted().has_value(), "geometry resize never requires a singular inverse");
                        const auto returned = gesture.resolve(press, { constrained, center });
                        check(returned && *returned == gesture.before(), "returning to press recovers exact original bits");
                    }
                }
            }
        }
    }
}

void shapesAndPreviewBaselines()
{
    const c::AffineTransform baseline { -.8, .25, 121.5, .6, 1.2, -19.25 };
    for (const auto kind : { c::ShapeKind::Rectangle, c::ShapeKind::RoundedRectangle,
             c::ShapeKind::Ellipse, c::ShapeKind::Triangle, c::ShapeKind::Line, c::ShapeKind::Polygon }) {
        auto original = shape(kind);
        auto current = c::ShapeGeometryState { original, baseline };
        // Widely varying previews all resolve from the starting geometry,
        // never from the last raster cache or already-rounded preview points.
        const auto press = baseline.map({ original.size.width, original.size.height });
        c::ShapeResizeGesture gesture(original, baseline, c::TransformHandle::BottomRight, press);
        for (int i = 1; i <= 80; ++i) {
            const double fx = .2 + double(i % 17) / 5;
            const double fy = .3 + double(i % 11) / 3;
            const auto endpoint = baseline.map({ original.size.width * fx, original.size.height * fy });
            const auto next = gesture.resolve(endpoint, { });
            check(next.has_value(), "long preview trace resolves from original geometry");
            if (!next)
                continue;
            near(next->shape.size.width, original.size.width * fx, "preview does not accumulate width scale");
            near(next->shape.size.height, original.size.height * fy, "preview does not accumulate height scale");
            for (std::size_t n = 0; n < original.points.size(); ++n)
                near(next->shape.points[n], { original.points[n].x * fx, original.points[n].y * fy },
                    "line endpoints/polygon vertices are resized from original coordinates");
            current = *next;
        }
        for (int i = 0; i < 12; ++i) {
            const auto start = current.transform.map({ current.shape.size.width, current.shape.size.height });
            c::ShapeResizeGesture repeated(current.shape, current.transform,
                c::TransformHandle::BottomRight, start);
            const auto next = repeated.resolve(start + linear(current.transform, { 7.125, -1.25 }), { });
            check(next.has_value(), "successive completed geometry resizes remain editable");
            if (!next)
                continue;
            near(next->shape.size.width, current.shape.size.width + 7.125,
                "each completed resize starts from current authoritative geometry");
            check(next->transform == baseline, "repeated bottom-right geometry resizing never accumulates affine scale");
            check(next->shape.strokeWidth == original.strokeWidth && next->shape.cornerRadius == original.cornerRadius,
                "repeated resize does not drift style dimensions");
            current = *next;
        }
    }
}

void zeroCrossingsAndLines()
{
    auto original = shape();
    original.size = { 100, 50 };
    c::ShapeResizeGesture gesture(original, { }, c::TransformHandle::Right, { 100, 25 });
    for (const double x : { 100., 0., -10., 0., 20., 100. }) {
        const auto next = gesture.resolve({ x, 25 }, { });
        check(next && next->transform.inverted(), "crossing zero leaves invertible affine even for degenerate geometry");
        if (next) {
            near(next->shape.size.width, std::abs(x), "signed width crosses exact zero without a scale clamp");
            near(next->transform.map({ next->shape.size.width, 25 }), { x, 25 },
                "crossing and returning keeps the dragged edge under its document-space pointer");
        }
    }
    const auto originalAgain = gesture.resolve({ 100, 25 }, { });
    check(originalAgain && *originalAgain == gesture.before(), "zero/flip/return is an exact no-op");

    for (const bool vertical : { false, true }) {
        auto line = shape(c::ShapeKind::Line);
        line.size = vertical ? c::Extent2d { 0, 100 } : c::Extent2d { 100, 0 };
        line.points = { { 0, 0 }, { line.size.width, line.size.height } };
        const auto press = c::Vec2d { line.size.width, line.size.height };
        c::ShapeResizeGesture lineGesture(line, { }, c::TransformHandle::BottomRight, press);
        for (const double amount : { 30., -15., 0., 20. }) {
            const auto point = press + (vertical ? c::Vec2d { amount, 0 } : c::Vec2d { 0, amount });
            const auto next = lineGesture.resolve(point, { });
            check(next && next->transform.inverted(), "horizontal/vertical line acquires and crosses a missing span safely");
            if (next) {
                near(next->transform.map(next->shape.points[0]), { 0, 0 }, "first endpoint remains anchored");
                near(next->transform.map(next->shape.points[1]), point, "ordered second endpoint controls the new zero-axis span");
                check(next->shape.strokeWidth == 4, "zero-axis line resize preserves round stroke width");
            }
        }
    }
    const auto invalid = gesture.resolve({ std::numeric_limits<double>::infinity(), 20 }, { });
    check(!invalid, "nonfinite pointer input is rejected");
    check(!gesture.resolve({ c::maximumShapeDimension + 1, 25 }, { }), "oversized shape geometry is rejected before mutation");
}

void atomicHistoryAndCancellation()
{
    c::Document document({ { 1024, 768 }, 96 });
    auto layer = c::Layer::shape("Geometry command", shape(c::ShapeKind::Polygon));
    layer.localToDocument = { -.8, .25, 121.5, .6, 1.2, 30 };
    const auto id = layer.id;
    check(document.insertLayer(0, std::move(layer)), "test layer inserted");
    const auto before = state(document, id);
    const auto start = before.transform.map({ 0, 0 });
    c::ShapeResizeGesture gesture(before.shape, before.transform, c::TransformHandle::TopLeft, start);
    const auto after = gesture.resolve(start + linear(before.transform, { -15, -60 }), { });
    check(after.has_value(), "history resize resolves");
    if (!after)
        return;
    c::History history;
    const auto revision = document.revision(), shapeRevision = document.layer(id)->shapeRevision;
    auto command = std::make_unique<c::ResizeShapeCommand>(id, before, *after);
    const auto expectedMemory = sizeof(c::ResizeShapeCommand)
        + (before.shape.points.size() + after->shape.points.size()) * sizeof(c::Vec2d);
    check(command->memoryCost() == expectedMemory, "history memory exactly counts two geometry snapshots and command storage");
    check(history.execute(document, std::move(command)), "one completed resize creates an undo command");
    check(state(document, id) == *after && document.revision() == revision + 1
            && document.layer(id)->shapeRevision == shapeRevision + 1,
        "geometry and changed position publish atomically with one document revision");
    check(history.undoDepth() == 1 && history.memoryUsed() == expectedMemory, "resize is one exactly-accounted history action");
    check(history.undo(document) && state(document, id) == before, "undo restores geometry and affine bits together");
    check(history.redo(document) && state(document, id) == *after, "redo restores geometry and affine bits together");
    check(history.undo(document), "redo branch prepared");
    const auto savedContent = document.contentState();
    check(!history.execute(document, std::make_unique<c::ResizeShapeCommand>(id, before, before)),
        "no-op resize creates no command");
    check(history.redoDepth() == 1 && document.contentState() == savedContent, "no-op preserves redo and modified state");

    auto cache = std::make_shared<c::LayerRenderCache>();
    cache->surface = std::make_shared<c::ContiguousRasterSurface>(c::Extent2u { 2, 2 });
    document.layer(id)->renderCache = cache;
    check(document.setLayerShapeGeometry(id, after->shape, after->transform), "live resize preview applied");
    check(!document.layer(id)->renderCache, "geometry mutation invalidates disposable shape cache");
    check(document.setLayerShapeGeometry(id, before.shape, before.transform), "cancellation restores exact starting state");
    check(state(document, id) == before && history.redoDepth() == 1 && document.contentState() == savedContent,
        "cancelled preview creates no history and preserves pre-existing redo");

    check(document.setLayerShapeGeometry(id, after->shape, after->transform), "live edit ready to adopt");
    std::unique_ptr<c::Command> applied = std::make_unique<c::ResizeShapeCommand>(id, before, *after);
    check(history.adoptApplied(document, applied), "continuous preview adopted once without replay");
    check(!applied && history.undoDepth() == 1 && !history.canRedo(), "divergent resize clears redo only on commit");
    check(history.memoryUsed() == expectedMemory, "undo/redo/adoption do not inflate snapshot accounting");
    check(history.undo(document) && state(document, id) == before, "adopted resize undoes atomically");

    const auto removed = document.takeLayer(id);
    check(removed.has_value(), "target removed during a pending action");
    const auto absentRevision = document.revision();
    check(!history.redo(document), "redo safely rejects a missing shape target");
    check(!history.execute(document, std::make_unique<c::ResizeShapeCommand>(id, before, *after)),
        "commit safely rejects a layer deleted during gesture");
    check(document.revision() == absentRevision && history.redoDepth() == 1, "missing target leaves remaining history/document unchanged");
    history.clear();
    check(history.memoryUsed() == 0, "clearing history releases all geometry snapshot accounting");
}

void explicitWholeLayerTransform()
{
    c::Document document({ { 1024, 768 }, 96 });
    auto layer = c::Layer::shape("Separate Ctrl+T", shape());
    const auto id = layer.id;
    check(document.insertLayer(0, std::move(layer)), "whole-layer transform fixture inserted");
    const auto original = state(document, id);
    c::History history;
    c::LayerTransformSession whole(document, id);
    auto values = whole.values();
    values.scaleX = -2;
    values.scaleY = 3;
    values.rotationDegrees = 27;
    check(whole.setValues(values) && whole.completeAction(), "explicit Ctrl+T still modifies whole-layer scale/flip/rotation");
    check(whole.commit(history) == c::TransformCommitResult::Committed, "whole-layer action commits");
    const auto transformed = state(document, id);
    check(transformed.shape == original.shape, "Ctrl+T does not rewrite authoritative local shape/style");
    const auto press = transformed.transform.map({ transformed.shape.size.width, transformed.shape.size.height });
    c::ShapeResizeGesture geometry(transformed.shape, transformed.transform,
        c::TransformHandle::BottomRight, press);
    const auto resized = geometry.resolve(press + linear(transformed.transform, { 0, 100 }), { });
    check(resized && resized->transform == transformed.transform,
        "later shape-mode resize preserves existing Ctrl+T matrix exactly rather than normalizing it");
    if (resized) {
        near(resized->shape.size.height, original.shape.size.height + 100, "shape-mode geometry is not double-scaled after Ctrl+T");
        check(resized->shape.strokeWidth == original.shape.strokeWidth, "configured stroke survives both independent workflows");
    }
}
} // namespace

int main()
{
    try {
        handlesAnchorsAndConstraints();
        shapesAndPreviewBaselines();
        zeroCrossingsAndLines();
        atomicHistoryAndCancellation();
        explicitWholeLayerTransform();
    } catch (const std::exception& exception) {
        ++failures;
        std::cerr << "EXCEPTION: " << exception.what() << '\n';
    }
    if (!failures)
        std::cout << "Shape intrinsic resizing, shared anchors, zero crossings, history and Ctrl+T separation passed\n";
    return failures ? 1 : 0;
}
