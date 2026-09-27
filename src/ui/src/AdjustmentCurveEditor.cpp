#include "imageeditor/ui/AdjustmentCurveEditor.hpp"
#include "imageeditor/ui/Theme.hpp"
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>

namespace imageeditor::ui {
AdjustmentCurveEditor::AdjustmentCurveEditor(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("AdjustmentCurveEditor"));
    setMinimumSize(180, 160);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setFixedHeight(190);
    setFocusPolicy(Qt::StrongFocus);
}
QRectF AdjustmentCurveEditor::plot() const { return QRectF(rect()).adjusted(10, 10, -10, -10); }
QPointF AdjustmentCurveEditor::toView(core::Vec2d p) const
{
    const auto r = plot();
    return {r.left() + p.x * r.width(), r.bottom() - p.y * r.height()};
}
core::Vec2d AdjustmentCurveEditor::fromView(QPointF p) const
{
    const auto r = plot();
    return {std::clamp((p.x() - r.left()) / r.width(), 0., 1.),
        std::clamp((r.bottom() - p.y()) / r.height(), 0., 1.)};
}
int AdjustmentCurveEditor::pointAt(QPointF p) const
{
    int result = -1;
    double distance = 9.;
    for (std::size_t i = 0; i < points_.size(); ++i) {
        const auto d = QLineF(toView(points_[i]), p).length();
        if (d < distance) { distance = d; result = int(i); }
    }
    return result;
}
void AdjustmentCurveEditor::setPoints(const std::vector<core::Vec2d>& points)
{
    points_ = points;
    selected_ = std::clamp(selected_, 0, std::max(0, int(points_.size()) - 1));
    update();
}
void AdjustmentCurveEditor::setHistogram(const AdjustmentHistogram& histogram, int channel)
{
    histogram_ = histogram; channel_ = std::clamp(channel, 0, 3); update();
}
void AdjustmentCurveEditor::setHistogramOnly(bool value)
{
    histogramOnly_ = value;
    setFixedHeight(value ? 100 : 190);
    setMinimumHeight(value ? 100 : 160);
    update();
}
void AdjustmentCurveEditor::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const auto r = plot();
    painter.setPen(QPen(themeColor(ThemeColor::Border), 1));
    painter.setBrush(themeColor(ThemeColor::Background));
    painter.drawRoundedRect(r, 4, 4);
    painter.save(); painter.setClipRect(r);
    auto grid = themeColor(ThemeColor::Text); grid.setAlpha(25);
    painter.setPen(grid);
    for (int i = 1; i < 4; ++i) {
        painter.drawLine(QPointF(r.left()+r.width()*i/4, r.top()), QPointF(r.left()+r.width()*i/4, r.bottom()));
        if (!histogramOnly_) painter.drawLine(QPointF(r.left(), r.top()+r.height()*i/4), QPointF(r.right(),r.top()+r.height()*i/4));
    }
    const auto& bins = histogram_[std::size_t(channel_)];
    const auto maximum = *std::max_element(bins.begin(), bins.end());
    if (maximum > 0) {
        QPainterPath bars; bars.moveTo(r.bottomLeft());
        for (std::size_t i = 0; i < bins.size(); ++i)
            bars.lineTo(r.left() + r.width() * double(i)/255., r.bottom() - r.height()*std::sqrt(bins[i]/maximum));
        bars.lineTo(r.bottomRight()); bars.closeSubpath();
        auto tint = themeColor(ThemeColor::SecondaryText); tint.setAlpha(80);
        painter.fillPath(bars, tint);
    }
    if (!histogramOnly_) {
        auto ref = themeColor(ThemeColor::Text); ref.setAlpha(80);
        painter.setPen(QPen(ref, 1, Qt::DashLine)); painter.drawLine(r.bottomLeft(), r.topRight());
        QPainterPath curve;
        for (int i=0;i<=256;++i) {
            const double x=double(i)/256.;
            const auto y=evaluate ? evaluate(x) : x;
            const auto p=toView({x,std::clamp(y,0.,1.)});
            if(i==0)curve.moveTo(p);else curve.lineTo(p);
        }
        // The graph is a stroke, not a filled polygon. Do not inherit the
        // background brush used for the plot; an open path is implicitly
        // closed when filled and would cover its histogram and grid.
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(themeColor(ThemeColor::Accent),1.6)); painter.drawPath(curve);
        for (std::size_t i=0;i<points_.size();++i) {
            painter.setBrush(int(i)==selected_ ? themeColor(ThemeColor::Accent) : themeColor(ThemeColor::Surface));
            painter.setPen(QPen(themeColor(ThemeColor::Text),1));
            painter.drawEllipse(toView(points_[i]),4,4);
        }
    }
    painter.restore();
}
void AdjustmentCurveEditor::moveSelected(core::Vec2d p)
{
    if(selected_<0||selected_>=int(points_.size()))return;
    if(selected_==0||selected_==int(points_.size())-1)p.x=points_[std::size_t(selected_)].x;
    else {
        const auto previous=points_[std::size_t(selected_-1)].x;
        const auto next=points_[std::size_t(selected_+1)].x;
        // Loaded/API curves may contain a narrower valid interval than the UI's
        // normal insertion spacing. Never pass reversed bounds to std::clamp.
        const auto separation=std::min(1./1024.,std::max(1e-6,(next-previous)/4));
        const auto lower=previous+separation,upper=next-separation;
        p.x=lower<=upper?std::clamp(p.x,lower,upper):points_[std::size_t(selected_)].x;
    }
    p.y=std::clamp(p.y,0.,1.);
    if(points_[std::size_t(selected_)]==p)return;
    points_[std::size_t(selected_)]=p;
    if(onPointsChanged)onPointsChanged(points_);
    if(onPointSelected)onPointSelected(selected_);
    update();
}
void AdjustmentCurveEditor::setSelectedPoint(core::Vec2d p) { moveSelected(p); }
void AdjustmentCurveEditor::mousePressEvent(QMouseEvent* event)
{
    if(histogramOnly_){event->ignore();return;}
    if(event->button()==Qt::LeftButton||event->button()==Qt::RightButton)setFocus(Qt::MouseFocusReason);
    if(event->button()==Qt::RightButton) {
        const auto hit=pointAt(event->position());
        if(hit>=0){selected_=hit;removeSelectedPoint();} event->accept();return;
    }
    if(event->button()!=Qt::LeftButton){event->ignore();return;}
    const auto hit=pointAt(event->position());
    if(hit<0 && points_.size()>=16){event->accept();return;}
    if(onInteractionStarted&&!onInteractionStarted()){event->accept();return;}
    before_=points_; dragging_=true; setFocus(Qt::MouseFocusReason);
    if(hit>=0)selected_=hit;
    else {
        auto p=fromView(event->position());
        auto it=std::lower_bound(points_.begin(),points_.end(),p.x,[](const auto& a,double x){return a.x<x;});
        if(it==points_.begin())selected_=0;
        else if(it==points_.end())selected_=int(points_.size())-1;
        else if(p.x-(it-1)->x<1./1024.)selected_=int(it-points_.begin())-1;
        else if(it->x-p.x<1./1024.)selected_=int(it-points_.begin());
        else {selected_=int(it-points_.begin());points_.insert(it,p);if(onPointsChanged)onPointsChanged(points_);}
    }
    if(onPointSelected)onPointSelected(selected_);
    update(); event->accept();
}
void AdjustmentCurveEditor::mouseMoveEvent(QMouseEvent* event)
{
    if(dragging_){moveSelected(fromView(event->position()));event->accept();}else QWidget::mouseMoveEvent(event);
}
void AdjustmentCurveEditor::mouseReleaseEvent(QMouseEvent* event)
{
    if(dragging_&&event->button()==Qt::LeftButton){dragging_=false;if(onInteractionFinished)onInteractionFinished(true);event->accept();}
    else QWidget::mouseReleaseEvent(event);
}
void AdjustmentCurveEditor::cancelInteraction()
{
    finishInteraction(false);
}
void AdjustmentCurveEditor::finishInteraction(bool commit)
{
    if(!dragging_)return;
    dragging_=false;if(!commit)points_=before_;if(onInteractionFinished)onInteractionFinished(commit);update();
}
void AdjustmentCurveEditor::removeSelectedPoint()
{
    if(selected_<=0||selected_>=int(points_.size())-1)return;
    if(onInteractionStarted&&!onInteractionStarted())return;
    points_.erase(points_.begin()+selected_);selected_=std::min(selected_,int(points_.size())-1);
    if(onPointsChanged)onPointsChanged(points_);
    if(onInteractionFinished)onInteractionFinished(true);
    if(onPointSelected)onPointSelected(selected_);
    update();
}
void AdjustmentCurveEditor::keyPressEvent(QKeyEvent* event)
{
    if(event->key()==Qt::Key_Escape&&dragging_){cancelInteraction();event->accept();return;}
    if(shortcutMatches(shortcuts_, "RemovePointAction", *event)){removeSelectedPoint();event->accept();return;}
    // Delete is not point removal by default, but must never bubble out of
    // this editing surface and erase the layer underneath it.
    if(event->key()==Qt::Key_Delete&&event->modifiers()==Qt::NoModifier){event->accept();return;}
    QWidget::keyPressEvent(event);
}
bool AdjustmentCurveEditor::event(QEvent* event)
{
    if (!histogramOnly_ && event->type()==QEvent::ShortcutOverride) {
        const auto key=static_cast<QKeyEvent*>(event)->key();
        if(shortcutMatches(shortcuts_, "RemovePointAction", *static_cast<QKeyEvent*>(event))||(key==Qt::Key_Escape&&dragging_)) {
            event->accept();return true;
        }
    }
    if(event->type()==QEvent::TouchCancel||event->type()==QEvent::Hide)cancelInteraction();
    return QWidget::event(event);
}
} // namespace imageeditor::ui
