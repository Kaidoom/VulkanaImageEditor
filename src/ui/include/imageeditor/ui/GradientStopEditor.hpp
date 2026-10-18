#pragma once
#include "imageeditor/core/LayerEffects.hpp"
#include <QWidget>
#include <functional>

namespace imageeditor::ui {
// A normalized color ramp editor. The owning panel supplies document transactions.
class GradientStopEditor final : public QWidget {
public:
    explicit GradientStopEditor(QWidget* parent = nullptr);
    void setStops(const std::vector<core::GradientStop>&);
    const std::vector<core::GradientStop>& stops() const { return stops_; }
    bool interactionActive() const { return dragging_; }
    void finishInteraction(bool commit);
    QRectF rampRect() const;
    QPointF stopPosition(std::size_t) const;
    std::function<bool()> onInteractionStarted;
    std::function<void(const std::vector<core::GradientStop>&)> onStopsChanged;
    std::function<void(bool)> onInteractionFinished;
    std::function<void(std::size_t)> onColorRequested;
protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    bool event(QEvent*) override;
private:
    int stopAt(QPointF) const;
    double positionAt(double) const;
    bool begin();
    void moveSelected(double);
    void removeSelected();
    std::vector<core::GradientStop> stops_{{0,{255,255,255,255}}, {1,{0,0,0,255}}}, before_;
    int selected_{0}, beforeSelected_{0}, pressed_{-1};
    QPointF pressPosition_;
    bool dragging_{}, moved_{};
};
} // namespace imageeditor::ui
