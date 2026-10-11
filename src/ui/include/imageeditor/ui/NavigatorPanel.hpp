#pragma once

#include "imageeditor/ui/PixelPreview.hpp"
#include "imageeditor/core/ViewportState.hpp"
#include <QWidget>
#include <QImage>

class QLabel;
class QSlider;
class QToolButton;

namespace imageeditor::ui {
class NavigatorView;

class NavigatorPanel final : public QWidget {
public:
    explicit NavigatorPanel(QWidget* parent = nullptr);
    ~NavigatorPanel() override;
    void setDocument(std::uint64_t instance, const core::DocumentSnapshot&);
    void setView(core::ViewportState, core::Extent2d viewport, QRectF availableCanvas);
    void shutdown();
    QSize sizeHint() const override { return {300, 180}; }
    QRectF imageRect() const;
    QRectF visibleRect() const;
    QPointF viewAnchor() const { return available_.center(); }
    std::uint64_t completedRenders() const { return preview_.completedRenders(); }
    std::function<void()> onSourceRequested;
    std::function<void(core::Vec2d)> onNavigate;
    std::function<void(double)> onZoom;
    std::function<void()> onResetView;
protected:
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;
private:
    NavigatorView* view_ {};
    QSlider* zoom_ {};
    QToolButton* reset_ {};
    QLabel* percentage_ {};
    PixelPreview preview_ {nullptr, PixelPreview::Purpose::Navigator};
    std::uint64_t instance_ {};
    core::Extent2u extent_ {};
    QRectF available_;
};
}
