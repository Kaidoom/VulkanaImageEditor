#pragma once

#include "imageeditor/core/Geometry.hpp"
#include "imageeditor/ui/EditorShortcuts.hpp"
#include <QWidget>
#include <array>
#include <functional>
#include <vector>

namespace imageeditor::ui {

using AdjustmentHistogram = std::array<std::array<double, 256>, 4>;

// Persistent, logical-pixel curve canvas. It never owns document state/history.
// Points use normalized unassociated sRGB coordinates, not viewport pixels.
class AdjustmentCurveEditor final : public QWidget {
public:
    explicit AdjustmentCurveEditor(QWidget* parent = nullptr);
    void setPoints(const std::vector<core::Vec2d>& points);
    void setHistogram(const AdjustmentHistogram&, int channel);
    void setHistogramOnly(bool);
    void setSelectedPoint(core::Vec2d);
    void removeSelectedPoint();
    void setShortcutBindings(const ShortcutBindings& bindings) { shortcuts_ = bindings; }
    void cancelInteraction();
    void finishInteraction(bool commit);
    [[nodiscard]] const std::vector<core::Vec2d>& points() const { return points_; }
    [[nodiscard]] int selectedIndex() const { return selected_; }
    [[nodiscard]] bool interactionActive() const { return dragging_; }
    std::function<bool()> onInteractionStarted;
    std::function<void(const std::vector<core::Vec2d>&)> onPointsChanged;
    std::function<void(bool)> onInteractionFinished;
    std::function<void(int)> onPointSelected;
    // Supplied from the same curve evaluator used to build compositor LUTs.
    std::function<double(double)> evaluate;

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    bool event(QEvent*) override;

private:
    ShortcutBindings shortcuts_ = defaultShortcutBindings();
    [[nodiscard]] QRectF plot() const;
    [[nodiscard]] QPointF toView(core::Vec2d) const;
    [[nodiscard]] core::Vec2d fromView(QPointF) const;
    [[nodiscard]] int pointAt(QPointF) const;
    void moveSelected(core::Vec2d);
    std::vector<core::Vec2d> points_ {{0, 0}, {1, 1}}, before_;
    AdjustmentHistogram histogram_ {};
    int selected_ {0}, channel_ {0};
    bool dragging_ {false}, histogramOnly_ {false};
};

} // namespace imageeditor::ui
