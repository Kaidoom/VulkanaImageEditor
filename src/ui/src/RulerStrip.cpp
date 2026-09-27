#include "imageeditor/ui/RulerStrip.hpp"

#include "imageeditor/core/RulerTicks.hpp"
#include "imageeditor/ui/Theme.hpp"

#include <QApplication>
#include <QCursor>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPointer>

#include <algorithm>
#include <cmath>

namespace imageeditor::ui {
namespace {
bool equalExtent(core::Extent2d a, core::Extent2d b)
{
    return a.width == b.width && a.height == b.height;
}
QString tickLabel(double value, double step)
{
    const auto decimals = step >= 1 ? 0 : std::clamp(int(std::ceil(-std::log10(step))), 0, 4);
    return QLocale::c().toString(value == 0 ? 0.0 : value, 'f', decimals);
}
}

RulerStrip::RulerStrip(Axis axis, QWidget* parent)
    : QWidget(parent), axis_(axis)
{
    setObjectName(axis == Axis::Horizontal ? QStringLiteral("HorizontalRuler")
                                          : QStringLiteral("VerticalRuler"));
    setAccessibleName(axis == Axis::Horizontal ? QStringLiteral("Horizontal ruler")
                                              : QStringLiteral("Vertical ruler"));
    setFocusPolicy(Qt::NoFocus);
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent);
    if (axis == Axis::Horizontal) setFixedHeight(thickness);
    else setFixedWidth(thickness);
}

RulerStrip::~RulerStrip()
{
    // No callbacks into a workspace that may already be tearing down.
    pressed_ = dragging_ = false;
    if (QWidget::mouseGrabber() == this) releaseMouse();
}

void RulerStrip::setView(core::ViewportState view, core::Extent2d document,
    core::Extent2d viewport, std::optional<core::DocumentBounds> bounds,
    std::optional<core::Vec2d> pointerDocument)
{
    if (view_.zoom() == view.zoom() && view_.pan() == view.pan()
        && equalExtent(document_, document) && equalExtent(viewport_, viewport)
        && bounds_ == bounds && pointerDocument_ == pointerDocument) return;
    view_ = view;
    document_ = document;
    viewport_ = viewport;
    bounds_ = bounds;
    pointerDocument_ = pointerDocument;
    update();
}

void RulerStrip::setTicksTowardStart(bool value)
{
    if (ticksTowardStart_ == value) return;
    ticksTowardStart_ = value;
    update();
}

QRect RulerStrip::gripRect() const noexcept
{
    return axis_ == Axis::Horizontal ? QRect(0, 0, gripExtent, height())
                                     : QRect(0, 0, width(), gripExtent);
}

bool RulerStrip::event(QEvent* event)
{
    switch (event->type()) {
    case QEvent::KeyPress:
        if (static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape && pressed_) {
            cancelDrag();
            event->accept();
            return true;
        }
        break;
    case QEvent::ApplicationDeactivate:
    case QEvent::WindowDeactivate:
    case QEvent::FocusOut:
    case QEvent::Hide:
    case QEvent::Close:
    case QEvent::TouchCancel:
        cancelDrag();
        break;
    case QEvent::Move:
        // The local tick coordinates change when the strip moves, even when
        // the view has not. The zero remains in the shared viewport frame.
        update();
        break;
    default:
        break;
    }
    // Like WorkspacePanel, an UngrabMouse from a native surface handoff is
    // not cancellation: the shared pointer router still owns that gesture.
    return QWidget::event(event);
}

void RulerStrip::paintEvent(QPaintEvent* event)
{
    QPainter painter(this);
    painter.setClipRegion(event->region());
    painter.fillRect(rect(), themeColor(ThemeColor::Surface));
    const bool horizontal = axis_ == Axis::Horizontal;
    const double length = horizontal ? width() : height();
    const double breadth = horizontal ? height() : width();
    const auto position = parentWidget() ? mapTo(parentWidget(), QPoint()) : QPoint();
    const double start = horizontal ? position.x() : position.y();
    const auto origin = view_.documentToViewport({}, document_, viewport_);
    const double zero = horizontal ? origin.x : origin.y;
    const double documentLength = horizontal ? document_.width : document_.height;
    const auto ticks = core::makeRulerTicks(view_.zoom(), zero, start, length, documentLength);
    const auto point = [horizontal](double along, double cross) {
        return horizontal ? QPointF(along, cross) : QPointF(cross, along);
    };
    const auto axisRect = [horizontal](double along, double cross, double span, double thick) {
        return horizontal ? QRectF(along, cross, span, thick) : QRectF(cross, along, thick, span);
    };
    const auto dpr = devicePixelRatioF();
    const auto crisp = [dpr](double coordinate) { return (std::floor(coordinate * dpr) + .5) / dpr; };
    const double baseline = crisp(ticksTowardStart_ ? 0 : breadth - 1 / dpr);
    const double inward = ticksTowardStart_ ? 1 : -1;
    const auto visibleStart = std::clamp(zero - start, 0.0, length);
    const auto visibleEnd = std::clamp(zero + documentLength * view_.zoom() - start, 0.0, length);
    if (std::isfinite(visibleStart) && std::isfinite(visibleEnd) && visibleEnd > visibleStart)
        painter.fillRect(axisRect(visibleStart, 0, visibleEnd - visibleStart, breadth), themeColor(ThemeColor::Control));

    painter.setClipRect(axisRect(gripExtent, 0, std::max(0.0, length - gripExtent), breadth), Qt::IntersectClip);
    if (bounds_) {
        const double first = horizontal ? bounds_->minimum.x : bounds_->minimum.y;
        const double last = horizontal ? bounds_->maximum.x : bounds_->maximum.y;
        const double begin = std::clamp(zero + first * view_.zoom() - start, 0.0, length);
        const double end = std::clamp(zero + last * view_.zoom() - start, 0.0, length);
        if (std::isfinite(begin) && std::isfinite(end) && end > begin) {
            auto color = themeColor(ThemeColor::Accent);
            color.setAlphaF(.22F);
            painter.fillRect(axisRect(begin, 0, end - begin, breadth), color);
            color.setAlphaF(.55F);
            painter.fillRect(axisRect(begin, ticksTowardStart_ ? 0 : breadth - 2, end - begin, 2), color);
        }
    }

    auto rulerFont = font();
    rulerFont.setPixelSize(10);
    painter.setFont(rulerFont);
    const QFontMetricsF metrics(rulerFont);
    double lastLabelEnd = gripExtent;
    for (const auto& tick : ticks.ticks) {
        const double coordinate = crisp(tick.logical - start);
        auto color = themeColor(ThemeColor::SecondaryText);
        color.setAlphaF(tick.major ? .9F : .48F);
        painter.setPen(QPen(color, 1 / dpr));
        painter.drawLine(point(coordinate, baseline), point(coordinate, baseline + inward * (tick.major ? 8 : 4)));
        if (!tick.major) continue;
        const auto text = tickLabel(tick.document, ticks.majorStep);
        const double textWidth = metrics.horizontalAdvance(text);
        // Keep label density bounded even for long coordinates at high zoom.
        const double labelStart = coordinate + 3;
        if (labelStart < lastLabelEnd + 4 || labelStart + textWidth > length - 2) continue;
        painter.setPen(themeColor(ThemeColor::Text));
        if (horizontal) {
            painter.drawText(QPointF(labelStart, ticksTowardStart_ ? breadth - 3 : metrics.ascent() + 1), text);
        } else {
            painter.save();
            painter.translate(ticksTowardStart_ ? breadth - 3 : metrics.ascent() + 1,
                labelStart + textWidth);
            painter.rotate(-90);
            painter.drawText(QPointF(), text);
            painter.restore();
        }
        lastLabelEnd = labelStart + textWidth;
    }

    if (pointerDocument_) {
        const double value = horizontal ? pointerDocument_->x : pointerDocument_->y;
        const double coordinate = zero + value * view_.zoom() - start;
        if (std::isfinite(coordinate) && coordinate >= gripExtent && coordinate <= length) {
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(Qt::NoPen);
            painter.setBrush(themeColor(ThemeColor::Accent));
            painter.drawPolygon(QPolygonF({point(coordinate, baseline),
                point(coordinate - 4, baseline + inward * 5), point(coordinate + 4, baseline + inward * 5)}));
            painter.setRenderHint(QPainter::Antialiasing, false);
        }
    }

    painter.setClipping(false);
    painter.setClipRegion(event->region());
    painter.setPen(QPen(themeColor(ThemeColor::Border), 1 / dpr));
    painter.drawLine(point(0, baseline), point(length, baseline));
    painter.fillRect(gripRect(), themeColor(gripHovered_ || dragging_ ? ThemeColor::Control : ThemeColor::Surface));
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(themeColor(dragging_ ? ThemeColor::Accent : ThemeColor::SecondaryText));
    for (int along : {6, 11}) for (int cross : {8, 12, 16})
        painter.drawEllipse(point(along, cross), .85, .85);
}

void RulerStrip::refreshCursor(QPoint local)
{
    const bool hovered = gripRect().contains(local);
    if (gripHovered_ != hovered) { gripHovered_ = hovered; update(gripRect()); }
    setCursor(pressed_ ? Qt::ClosedHandCursor : hovered ? Qt::OpenHandCursor : Qt::ArrowCursor);
    setToolTip(hovered ? QStringLiteral("Drag ruler to its opposite workspace edge") : QString());
}

void RulerStrip::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && gripRect().contains(event->position().toPoint())) {
        pressed_ = true;
        dragging_ = false;
        pressGlobal_ = event->globalPosition().toPoint();
        refreshCursor(event->position().toPoint());
        update(gripRect());
    }
    // Rulers own only their slim footprint. A tick click is not a canvas edit.
    event->accept();
}

void RulerStrip::mouseMoveEvent(QMouseEvent* event)
{
    const auto global = event->globalPosition().toPoint();
    refreshCursor(event->position().toPoint());
    if (pressed_) {
        if (!dragging_ && (global - pressGlobal_).manhattanLength() >= QApplication::startDragDistance()) {
            dragging_ = true;
            update(gripRect());
            if (onDragStarted) onDragStarted(pressGlobal_);
        }
        if (dragging_ && onDragMoved) onDragMoved(global);
    }
    event->accept();
}

void RulerStrip::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && pressed_) finishDrag(event->globalPosition().toPoint(), false);
    event->accept();
}

void RulerStrip::finishDrag(QPoint global, bool cancelled)
{
    const bool notify = dragging_;
    pressed_ = dragging_ = false;
    refreshCursor(mapFromGlobal(global));
    update(gripRect());
    const QPointer<RulerStrip> guard(this);
    if (notify && onDragFinished) onDragFinished(global, cancelled);
    if (guard && QWidget::mouseGrabber() == this) releaseMouse();
}

void RulerStrip::cancelDrag()
{
    if (pressed_) finishDrag(QCursor::pos(), true);
}

void RulerStrip::leaveEvent(QEvent* event)
{
    if (!pressed_) refreshCursor(QPoint(-1, -1));
    QWidget::leaveEvent(event);
}

} // namespace imageeditor::ui
