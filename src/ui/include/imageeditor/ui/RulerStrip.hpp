#pragma once

#include "imageeditor/core/Measurement.hpp"
#include "imageeditor/core/ViewportState.hpp"

#include <QWidget>

#include <functional>
#include <optional>

namespace imageeditor::ui {

// Thin workspace chrome, not document content. The parent has the canvas's
// viewport origin; moving/docking this widget never alters ViewportState.
class RulerStrip final : public QWidget {
public:
    enum class Axis { Horizontal, Vertical };
    static constexpr int thickness = 24;
    static constexpr int gripExtent = 18;

    explicit RulerStrip(Axis, QWidget* parent = nullptr);
    ~RulerStrip() override;

    [[nodiscard]] Axis axis() const noexcept { return axis_; }
    void setView(core::ViewportState, core::Extent2d document,
        core::Extent2d viewport, std::optional<core::DocumentBounds> = {},
        std::optional<core::Vec2d> pointerDocument = {});
    // Top/left rulers face the positive cross-axis by default; bottom/right
    // rulers face the negative cross-axis toward the canvas instead.
    void setTicksTowardStart(bool);
    [[nodiscard]] bool ticksTowardStart() const noexcept { return ticksTowardStart_; }
    [[nodiscard]] bool isDragging() const noexcept { return dragging_; }
    [[nodiscard]] QRect gripRect() const noexcept;
    void cancelDrag();

    std::function<void(QPoint global)> onDragStarted;
    std::function<void(QPoint global)> onDragMoved;
    std::function<void(QPoint global, bool cancelled)> onDragFinished;

protected:
    bool event(QEvent*) override;
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override;

private:
    void finishDrag(QPoint global, bool cancelled);
    void refreshCursor(QPoint local);

    Axis axis_;
    core::ViewportState view_;
    core::Extent2d document_;
    core::Extent2d viewport_;
    std::optional<core::DocumentBounds> bounds_;
    std::optional<core::Vec2d> pointerDocument_;
    QPoint pressGlobal_;
    bool ticksTowardStart_ {false};
    bool pressed_ {false};
    bool dragging_ {false};
    bool gripHovered_ {false};
};

} // namespace imageeditor::ui
