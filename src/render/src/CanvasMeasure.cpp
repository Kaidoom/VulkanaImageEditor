#include "imageeditor/render/CanvasWindow.hpp"
#include <QGuiApplication>
#include <cmath>

namespace imageeditor::render {
bool CanvasWindow::pointerGestureActive() const noexcept
{
    return brushing_ || picking_ || panning_ || transforming_ || selecting_ || selectingText_
        || shaping_ || shapeConstruction_ || selectionConstruction_ || measuring_
        || completedGestureButtons_ != Qt::NoButton;
}
bool CanvasWindow::measureActive() const noexcept
{
    return temporaryMeasure_ || scene_.activeTool == core::ToolId::Measure;
}
bool CanvasWindow::setTemporaryMeasure(bool held)
{
    if (held == temporaryMeasure_) return true;
    if (held && (pointerGestureActive() || scene_.activeTool == core::ToolId::Measure)) return false;
    if (!held) finishMeasureInput(true, true);
    temporaryMeasure_ = held;
    temporaryMeasurement_.reset();
    if (held) cancelColorSampling();
    refreshMeasurement();
    refreshColorSample();
    updateCursorForTool();
    if (onTemporaryMeasureChanged) onTemporaryMeasureChanged(held);
    return true;
}
int CanvasWindow::measureEndpointAt(core::Vec2d logical) const
{
    const auto& line = temporaryMeasure_ ? temporaryMeasurement_ : regularMeasurement_;
    if (!line) return -1;
    const auto a = scene_.viewport.documentToViewport(line->a, documentExtent(), viewportExtent()) - logical;
    const auto b = scene_.viewport.documentToViewport(line->b, documentExtent(), viewportExtent()) - logical;
    const auto da = std::hypot(a.x, a.y), db = std::hypot(b.x, b.y);
    if (std::min(da, db) > 9.0) return -1;
    return da < db ? 0 : 1;
}
void CanvasWindow::beginMeasureInput(QPointF p)
{
    auto& line = temporaryMeasure_ ? temporaryMeasurement_ : regularMeasurement_;
    measureStart_ = line;
    const auto point = documentPositionForLogical(p);
    if (!core::measurementValues({point, point})) return;
    measureEndpoint_ = measureEndpointAt({p.x(), p.y()});
    if (measureEndpoint_ < 0) { line = core::MeasurementLine {point, point}; measureEndpoint_ = 1; }
    measurePointerDocument_ = point;
    measureConstrained_ = selectionModifiers_.testFlag(Qt::ShiftModifier);
    measuring_ = true;
    if (QGuiApplication::platformName() != QStringLiteral("wayland"))
        explicitMeasureGrab_ = setMouseGrabEnabled(true);
    refreshMeasurement(); updateCursorForTool();
}
void CanvasWindow::moveMeasureInput(QPointF p, Qt::KeyboardModifiers modifiers)
{
    const auto point = documentPositionForLogical(p);
    if (!measuring_ || !core::measurementValues({point, point})) return;
    measurePointerDocument_ = point;
    measureConstrained_ = modifiers.testFlag(Qt::ShiftModifier);
    resolveMeasurePointer();
}
void CanvasWindow::resolveMeasurePointer()
{
    auto& line = temporaryMeasure_ ? temporaryMeasurement_ : regularMeasurement_;
    if (!measuring_ || !line) return;
    const auto anchor = measureEndpoint_ == 0 ? line->b : line->a;
    (measureEndpoint_ == 0 ? line->a : line->b) = measureConstrained_
        ? core::constrainLineEndpoint(anchor, measurePointerDocument_) : measurePointerDocument_;
    refreshMeasurement();
}
void CanvasWindow::updateMeasureModifiers(Qt::KeyboardModifiers modifiers)
{
    const bool constrained = modifiers.testFlag(Qt::ShiftModifier);
    if (!measuring_ || measureConstrained_ == constrained) return;
    measureConstrained_ = constrained;
    // Resolve from the original pointer, never from the snapped endpoint.
    // Press/release Shift updates immediately without waiting for mouse motion.
    resolveMeasurePointer();
}
void CanvasWindow::setAdvancedMeasurementReadout(bool enabled)
{
    if (advancedMeasurementReadout_ == enabled) return;
    advancedMeasurementReadout_ = enabled;
    if (measureActive()) refreshMeasurement();
}
void CanvasWindow::finishMeasureInput(bool cancel, bool suppressRelease)
{
    if (!measuring_) return;
    measuring_ = false;
    auto& line = temporaryMeasure_ ? temporaryMeasurement_ : regularMeasurement_;
    if (cancel) line = measureStart_;
    measureStart_.reset();
    const bool grabbed = explicitMeasureGrab_;
    explicitMeasureGrab_ = false;
    if (grabbed) setMouseGrabEnabled(false);
    if (suppressRelease) completedGestureButtons_ |= Qt::LeftButton;
    refreshMeasurement(); updateCursorForTool();
}
void CanvasWindow::cancelMeasureInput() { finishMeasureInput(true, true); }
void CanvasWindow::clearMeasurement()
{
    cancelMeasureInput();
    (temporaryMeasure_ ? temporaryMeasurement_ : regularMeasurement_).reset();
    refreshMeasurement();
}
void CanvasWindow::refreshMeasurement()
{
    const bool wasMeasure = scene_.measureActive;
    scene_.measureActive = measureActive();
    if (scene_.measureActive != wasMeasure) refreshLayerOutlines();
    scene_.measurement = measureActive() ? (temporaryMeasure_ ? temporaryMeasurement_ : regularMeasurement_)
                                        : std::optional<core::MeasurementLine> {};
    if (measureActive()) {
        std::string text;
        if (scene_.measurement) if (const auto value = core::measurementValues(*scene_.measurement)) {
            const auto number = [](double n) { return QString::number(std::abs(n) < .05 ? 0.0 : n, 'f', 1); };
            const auto delta = [&](double n) { return (n >= .05 ? QStringLiteral("+") : QString{}) + number(n); };
            auto label = QStringLiteral("Distance: %1 px  ·  Angle: %2")
                .arg(number(value->distance), value->angleDegrees
                    ? number(*value->angleDegrees) + QStringLiteral("°") : QStringLiteral("—"));
            if (advancedMeasurementReadout_)
                label += QStringLiteral("  ·  ΔX: %1 px  ΔY: %2 px").arg(delta(value->delta.x), delta(value->delta.y));
            text = label.toStdString();
        }
        setPointerTooltip(std::move(text));
    } else if (wasMeasure) setPointerTooltip({});
    scheduleFrame();
}
}
