#pragma once
#include <QImage>
#include <QWidget>
#include <functional>
#include "imageeditor/core/ViewportState.hpp"

namespace imageeditor::ui {
// Cropping is a rectangle over immutable captured pixels, not a resampled
// thumbnail. Widget resizing, RTL and display DPR never alter that rectangle.
class ScreenshotPreview final : public QWidget {
public:
    explicit ScreenshotPreview(QWidget* parent=nullptr);
    void setImage(QImage);
    const QImage& image() const { return image_; }
    QRect crop() const { return crop_; }
    void setCrop(QRect);
    void resetCrop();
    void resetView();
    double zoom() const { return view_.zoom(); }
    bool viewIsFitted() const { return fitted_; }
    QImage croppedImage() const;
    QRectF imageRect() const;
    std::function<void()> onCropChanged;
    std::function<void()> onViewChanged;
protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void leaveEvent(QEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void resizeEvent(QResizeEvent*) override;
private:
    QPoint imagePoint(QPointF) const;
    QRectF cropRect() const;
    int handleAt(QPointF) const;
    void updateDrag(QPointF);
    QImage image_;
    QRect crop_, before_;
    QPoint anchor_, start_;
    core::ViewportState view_, beforeView_;
    QPointF panStart_;
    bool fitted_{true}, beforeFitted_{true};
    enum class Drag { None, Draw, Move, Resize, Pan } drag_{Drag::None};
};
}
