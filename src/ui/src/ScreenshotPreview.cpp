#include "imageeditor/ui/ScreenshotPreview.hpp"
#include "imageeditor/ui/Localization.hpp"
#include "imageeditor/ui/Theme.hpp"
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>
#include <QResizeEvent>
#include <algorithm>
#include <array>
#include <cmath>

namespace imageeditor::ui {
ScreenshotPreview::ScreenshotPreview(QWidget* parent):QWidget(parent)
{
    setObjectName(QStringLiteral("ScreenshotPreview"));
    setFocusPolicy(Qt::StrongFocus);setMouseTracking(true);
    setMinimumSize(160,140);setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Expanding);
    setLayoutDirection(Qt::LeftToRight);
    i18n::setAccessibleName(this,i18n::text("screenshot.preview"));
    i18n::setToolTip(this,i18n::text("screenshot.crop.hint"));
}
void ScreenshotPreview::setImage(QImage image)
{
    image_=std::move(image);image_.setDevicePixelRatio(1);
    drag_=Drag::None;crop_=image_.rect();resetView();if(onCropChanged)onCropChanged();
}
void ScreenshotPreview::setCrop(QRect rect)
{
    if(image_.isNull())return;
    const auto bounded=rect.normalized().intersected(image_.rect());
    if(bounded.isEmpty()||bounded==crop_)return;
    crop_=bounded;update();if(onCropChanged)onCropChanged();
}
void ScreenshotPreview::resetCrop(){drag_=Drag::None;setCrop(image_.rect());}
void ScreenshotPreview::resetView()
{
    drag_=Drag::None;fitted_=true;unsetCursor();
    view_.fit({double(image_.width()),double(image_.height())},{double(width()),double(height())},12);
    update();if(onViewChanged)onViewChanged();
}
void ScreenshotPreview::resizeEvent(QResizeEvent* e)
{
    if(fitted_)resetView();
    QWidget::resizeEvent(e);
}
QImage ScreenshotPreview::croppedImage() const
{
    if(image_.isNull())return {};
    return crop_==image_.rect()?image_:image_.copy(crop_);
}
QRectF ScreenshotPreview::imageRect() const
{
    if(image_.isNull())return {};
    const auto origin=view_.documentToViewport({0,0},{double(image_.width()),double(image_.height())},
        {double(width()),double(height())});
    return {origin.x,origin.y,image_.width()*view_.zoom(),image_.height()*view_.zoom()};
}
QRectF ScreenshotPreview::cropRect() const
{
    const auto image=imageRect();if(image.isEmpty())return {};
    const auto scale=image.width()/image_.width();
    return {image.left()+crop_.x()*scale,image.top()+crop_.y()*scale,crop_.width()*scale,crop_.height()*scale};
}
QPoint ScreenshotPreview::imagePoint(QPointF p) const
{
    const auto r=imageRect();if(r.isEmpty())return {};
    return {int(std::clamp(std::floor((p.x()-r.x())*image_.width()/r.width()),0.,double(image_.width()-1))),
        int(std::clamp(std::floor((p.y()-r.y())*image_.height()/r.height()),0.,double(image_.height()-1)))};
}
int ScreenshotPreview::handleAt(QPointF point) const
{
    if(image_.isNull()||crop_==image_.rect())return -1;
    const auto r=cropRect();const std::array corners{r.topLeft(),r.topRight(),r.bottomRight(),r.bottomLeft()};
    for(int i=0;i<4;++i)if(QLineF(point,corners[std::size_t(i)]).length()<=7)return i;
    return -1;
}
void ScreenshotPreview::paintEvent(QPaintEvent*)
{
    QPainter p(this);p.fillRect(rect(),themeColor(ThemeColor::Canvas));
    const auto r=imageRect();
    if(image_.isNull()) {
        p.setPen(palette().color(QPalette::PlaceholderText));
        p.drawText(rect().adjusted(20,20,-20,-20),Qt::AlignCenter|Qt::TextWordWrap,i18n::text("screenshot.empty"));return;
    }
    p.setRenderHint(QPainter::SmoothPixmapTransform,view_.zoom()<1);p.drawImage(r,image_);
    const auto c=cropRect();
    if(crop_!=image_.rect()) {
        const QColor shade(0,0,0,135);
        p.fillRect(QRectF(r.topLeft(),QPointF(r.right(),c.top())),shade);
        p.fillRect(QRectF(QPointF(r.left(),c.bottom()),r.bottomRight()),shade);
        p.fillRect(QRectF(QPointF(r.left(),c.top()),QPointF(c.left(),c.bottom())),shade);
        p.fillRect(QRectF(QPointF(c.right(),c.top()),QPointF(r.right(),c.bottom())),shade);
        p.setPen(QPen(themeColor(ThemeColor::Accent),1.5));p.setBrush(Qt::NoBrush);p.drawRect(c);
        p.setBrush(themeColor(ThemeColor::Surface));
        for(auto point:{c.topLeft(),c.topRight(),c.bottomLeft(),c.bottomRight()})p.drawRect(QRectF(point-QPointF(3,3),QSizeF(6,6)));
    }
}
void ScreenshotPreview::mousePressEvent(QMouseEvent* e)
{
    if(e->button()==Qt::MiddleButton&&!image_.isNull()&&drag_==Drag::None) {
        setFocus(Qt::MouseFocusReason);drag_=Drag::Pan;beforeView_=view_;beforeFitted_=fitted_;
        panStart_=e->position();setCursor(Qt::ClosedHandCursor);e->accept();return;
    }
    if(drag_!=Drag::None){e->ignore();return;}
    if(e->button()!=Qt::LeftButton||image_.isNull()||!imageRect().contains(e->position())){e->ignore();return;}
    setFocus(Qt::MouseFocusReason);before_=crop_;start_=imagePoint(e->position());
    const int handle=handleAt(e->position());
    if(handle>=0) {
        drag_=Drag::Resize;
        const std::array corners{crop_.bottomRight(),crop_.bottomLeft(),crop_.topLeft(),crop_.topRight()};
        anchor_=corners[std::size_t(handle)];
    } else if(crop_!=image_.rect()&&cropRect().contains(e->position()))drag_=Drag::Move;
    else {drag_=Drag::Draw;anchor_=start_;}
    e->accept();
}
void ScreenshotPreview::updateDrag(QPointF p)
{
    if(drag_==Drag::Pan) {
        const auto delta=p-panStart_;view_=beforeView_;view_.panBy({delta.x(),delta.y()});
        fitted_=false;update();if(onViewChanged)onViewChanged();return;
    }
    const auto point=imagePoint(p);
    if(drag_==Drag::Draw||drag_==Drag::Resize) {
        // QRect::normalized() adjusts inverted inclusive edges. Sort the pixel
        // indices first so dragging left/up includes the same endpoint pixels.
        setCrop(QRect(QPoint(std::min(anchor_.x(),point.x()),std::min(anchor_.y(),point.y())),
            QPoint(std::max(anchor_.x(),point.x()),std::max(anchor_.y(),point.y()))));
    }
    else if(drag_==Drag::Move) {
        const auto delta=point-start_;
        setCrop(QRect(QPoint(std::clamp(before_.x()+delta.x(),0,image_.width()-before_.width()),
            std::clamp(before_.y()+delta.y(),0,image_.height()-before_.height())),before_.size()));
    }
}
void ScreenshotPreview::mouseMoveEvent(QMouseEvent* e)
{
    if(drag_!=Drag::None){updateDrag(e->position());e->accept();return;}
    const int handle=handleAt(e->position());
    setCursor(handle>=0?(handle%2?Qt::SizeBDiagCursor:Qt::SizeFDiagCursor)
        :crop_!=image_.rect()&&cropRect().contains(e->position())?Qt::SizeAllCursor:Qt::CrossCursor);
}
void ScreenshotPreview::mouseReleaseEvent(QMouseEvent* e)
{
    if((e->button()==Qt::MiddleButton&&drag_==Drag::Pan)
        ||(e->button()==Qt::LeftButton&&drag_!=Drag::None&&drag_!=Drag::Pan)) {
        updateDrag(e->position());drag_=Drag::None;unsetCursor();e->accept();return;
    }
    QWidget::mouseReleaseEvent(e);
}
void ScreenshotPreview::leaveEvent(QEvent* e){if(drag_==Drag::None)unsetCursor();QWidget::leaveEvent(e);}
void ScreenshotPreview::keyPressEvent(QKeyEvent* e)
{
    if(e->key()==Qt::Key_Escape&&drag_==Drag::Pan) {
        drag_=Drag::None;view_=beforeView_;fitted_=beforeFitted_;unsetCursor();update();
        if(onViewChanged)onViewChanged();
        e->accept();return;
    }
    if(e->key()==Qt::Key_Escape&&drag_!=Drag::None){drag_=Drag::None;setCrop(before_);e->accept();return;}
    QPoint delta;
    switch(e->key()) {
    case Qt::Key_Left:delta={-1,0};break;case Qt::Key_Right:delta={1,0};break;
    case Qt::Key_Up:delta={0,-1};break;case Qt::Key_Down:delta={0,1};break;
    default:QWidget::keyPressEvent(e);return;
    }
    if(!image_.isNull()) {
        if(e->modifiers().testFlag(Qt::ShiftModifier))delta*=10;
        setCrop(QRect(QPoint(std::clamp(crop_.x()+delta.x(),0,image_.width()-crop_.width()),
            std::clamp(crop_.y()+delta.y(),0,image_.height()-crop_.height())),crop_.size()));
    }
    e->accept();
}
void ScreenshotPreview::wheelEvent(QWheelEvent* e)
{
    if(drag_!=Drag::None){e->accept();return;}
    if(image_.isNull()||e->angleDelta().y()==0){e->ignore();return;}
    // Same cursor-anchored mapping, bounds and wheel cadence as the canvas.
    view_.zoomAround({e->position().x(),e->position().y()},std::pow(1.16,double(e->angleDelta().y())/120.),
        {double(image_.width()),double(image_.height())},{double(width()),double(height())});
    fitted_=false;update();if(onViewChanged)onViewChanged();e->accept();
}
}
