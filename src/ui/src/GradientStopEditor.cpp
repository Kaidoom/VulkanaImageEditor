#include "imageeditor/ui/GradientStopEditor.hpp"
#include "imageeditor/ui/Localization.hpp"
#include "imageeditor/core/ColorMath.hpp"
#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <algorithm>
#include <cmath>

namespace imageeditor::ui {
namespace {
QColor displayColor(core::PremultipliedColor c) {
    const auto rgba=core::encodeColor(c);
    return QColor(rgba.red,rgba.green,rgba.blue,rgba.alpha);
}
}
GradientStopEditor::GradientStopEditor(QWidget* parent) : QWidget(parent) {
    setObjectName("EffectGradientStops");
    setFocusPolicy(Qt::StrongFocus);
    setMinimumWidth(140);
    setFixedHeight(64);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    i18n::setAccessibleName(this, i18n::text("effects.gradient.editor"));
    i18n::setToolTip(this, i18n::text("effects.gradient.editor.hint"));
}
QRectF GradientStopEditor::rampRect() const { return {10, 5, double(width()-20), 28}; }
QPointF GradientStopEditor::stopPosition(std::size_t index) const {
    const auto r = rampRect();
    return {r.left()+stops_[index].position*r.width(), r.bottom()+12};
}
void GradientStopEditor::setStops(const std::vector<core::GradientStop>& stops) {
    if (dragging_ || stops.size()<2 || stops_==stops) return;
    stops_=stops;
    pressed_=-1;
    selected_=std::clamp(selected_,0,int(stops_.size())-1);
    update();
}
void GradientStopEditor::paintEvent(QPaintEvent*) {
    QPainter p(this);
    const auto r=rampRect();
    core::LayerEffect effect;
    effect.gradientStops=stops_;
    const int columns=std::max(1,int(std::ceil(r.width())));
    // Use the same associated-linear evaluator as the canvas, not Qt's sRGB ramp.
    for(int x=0;x<columns;++x) {
        const auto color=displayColor(core::sampleLayerEffectGradient(effect,double(x)/std::max(1,columns-1)));
        for(int y=0;y<int(r.height());y+=6) {
            QRectF cell(r.left()+x,r.top()+y,1,std::min(6,int(r.height())-y));
            p.fillRect(cell,((x/6+y/6)%2)?QColor(105,105,105):QColor(165,165,165));
            p.fillRect(cell,color);
        }
    }
    p.setPen(palette().color(QPalette::Mid));p.setBrush(Qt::NoBrush);p.drawRect(r);
    p.setRenderHint(QPainter::Antialiasing);
    const auto drawStop=[&](int i) {
        const auto center=stopPosition(std::size_t(i));
        const auto c=stops_[std::size_t(i)].color;
        const bool selected=i==selected_;
        p.setPen(QPen(selected?palette().color(QPalette::Highlight):palette().color(QPalette::Text),selected?2:1));
        p.setBrush(QColor(c.red,c.green,c.blue));
        QPolygonF marker{{center.x(),r.bottom()+2},{center.x()+5,r.bottom()+8},
                         {center.x()+5,r.bottom()+20},{center.x()-5,r.bottom()+20},
                         {center.x()-5,r.bottom()+8}};
        p.drawPolygon(marker);
    };
    for(int i=0;i<int(stops_.size());++i)if(i!=selected_)drawStop(i);
    if(selected_>=0)drawStop(selected_);
    if(hasFocus()) {
        p.setBrush(Qt::NoBrush);p.setPen(QPen(palette().color(QPalette::Mid),1,Qt::DotLine));
        p.drawRect(rect().adjusted(1,1,-2,-2));
    }
}
int GradientStopEditor::stopAt(QPointF point) const {
    if(point.y()<rampRect().bottom() || point.y()>rampRect().bottom()+24)return -1;
    if(selected_>=0 && std::abs(stopPosition(std::size_t(selected_)).x()-point.x())<=7)return selected_;
    int nearest=-1;double distance=8;
    for(std::size_t i=0;i<stops_.size();++i) {
        const auto d=std::abs(stopPosition(i).x()-point.x());
        if(d<distance){distance=d;nearest=int(i);}
    }
    return nearest;
}
double GradientStopEditor::positionAt(double x) const {
    return std::clamp((x-rampRect().left())/rampRect().width(),0.0,1.0);
}
bool GradientStopEditor::begin() {
    if(dragging_)return true;
    if(onInteractionStarted && !onInteractionStarted())return false;
    before_=stops_;beforeSelected_=selected_;dragging_=true;return true;
}
void GradientStopEditor::finishInteraction(bool commit) {
    pressed_=-1;
    if(!dragging_)return;
    dragging_=false;pressed_=-1;
    if(!commit){stops_=before_;selected_=beforeSelected_;}
    selected_=std::clamp(selected_,0,int(stops_.size())-1);
    if(onInteractionFinished)onInteractionFinished(commit);
    update();
}
void GradientStopEditor::moveSelected(double position) {
    if(selected_<0 || stops_[std::size_t(selected_)].position==position)return;
    auto stop=stops_[std::size_t(selected_)];stop.position=position;
    stops_.erase(stops_.begin()+selected_);
    const auto where=std::upper_bound(stops_.begin(),stops_.end(),position,
        [](double value,const core::GradientStop& s){return value<s.position;});
    selected_=int(where-stops_.begin());stops_.insert(where,stop);
    if(onStopsChanged)onStopsChanged(stops_);
    update();
}
void GradientStopEditor::removeSelected() {
    if(selected_<0 || stops_.size()<=2 || !begin())return;
    stops_.erase(stops_.begin()+selected_);
    selected_=std::min(selected_,int(stops_.size())-1);
    if(onStopsChanged)onStopsChanged(stops_);
    finishInteraction(true);
}
void GradientStopEditor::mousePressEvent(QMouseEvent* e) {
    if(e->button()!=Qt::LeftButton && e->button()!=Qt::RightButton){e->ignore();return;}
    setFocus(Qt::MouseFocusReason);
    const int hit=stopAt(e->position());
    if(hit>=0) {
        selected_=hit;update();
        if(e->button()==Qt::RightButton){removeSelected();e->accept();return;}
        pressed_=hit;pressPosition_=e->position();moved_=false;
    } else if(e->button()==Qt::LeftButton && rampRect().contains(e->position())
              && stops_.size()<core::maximumGradientStops && begin()) {
        const auto position=positionAt(e->position().x());
        core::LayerEffect effect;effect.gradientStops=stops_;
        const auto color=displayColor(core::sampleLayerEffectGradient(effect,position));
        const auto where=std::upper_bound(stops_.begin(),stops_.end(),position,
            [](double value,const core::GradientStop& s){return value<s.position;});
        selected_=int(where-stops_.begin());
        stops_.insert(where,{position,{std::uint8_t(color.red()),std::uint8_t(color.green()),
            std::uint8_t(color.blue()),std::uint8_t(color.alpha())}});
        if(onStopsChanged)onStopsChanged(stops_);
        finishInteraction(true);
    }
    e->accept();
}
void GradientStopEditor::mouseMoveEvent(QMouseEvent* e) {
    if(pressed_<0 || !(e->buttons()&Qt::LeftButton)){e->ignore();return;}
    if(!moved_ && (e->position()-pressPosition_).manhattanLength()<QApplication::startDragDistance())return;
    if(!begin()){pressed_=-1;return;}
    moved_=true;moveSelected(positionAt(e->position().x()));e->accept();
}
void GradientStopEditor::mouseReleaseEvent(QMouseEvent* e) {
    if(e->button()!=Qt::LeftButton){e->ignore();return;}
    const bool editColor=pressed_>=0 && !moved_;
    pressed_=-1;
    finishInteraction(true);
    if(editColor && onColorRequested)onColorRequested(std::size_t(selected_));
    e->accept();
}
void GradientStopEditor::keyPressEvent(QKeyEvent* e) {
    if(e->key()==Qt::Key_Escape && dragging_){finishInteraction(false);e->accept();return;}
    if(e->key()==Qt::Key_Delete || e->key()==Qt::Key_Backspace){removeSelected();e->accept();return;}
    if(e->key()==Qt::Key_Up || e->key()==Qt::Key_Down) {
        selected_=std::clamp(selected_+(e->key()==Qt::Key_Up?-1:1),0,int(stops_.size())-1);
        update();e->accept();return;
    }
    if(e->key()==Qt::Key_Return || e->key()==Qt::Key_Enter || e->key()==Qt::Key_Space) {
        if(onColorRequested)onColorRequested(std::size_t(selected_));
        e->accept();return;
    }
    if(e->key()==Qt::Key_Left || e->key()==Qt::Key_Right) {
        if(begin()) {
            const double step=(e->modifiers()&Qt::ShiftModifier)?.1:.01;
            moveSelected(std::clamp(stops_[std::size_t(selected_)].position+(e->key()==Qt::Key_Left?-step:step),0.0,1.0));
            finishInteraction(true);
        }
        e->accept();return;
    }
    QWidget::keyPressEvent(e);
}
bool GradientStopEditor::event(QEvent* e) {
    if(e->type()==QEvent::ShortcutOverride) {
        const auto key=static_cast<QKeyEvent*>(e)->key();
        if(key==Qt::Key_Escape || key==Qt::Key_Left || key==Qt::Key_Right || key==Qt::Key_Up || key==Qt::Key_Down || key==Qt::Key_Delete
           || key==Qt::Key_Backspace || key==Qt::Key_Return || key==Qt::Key_Enter || key==Qt::Key_Space) {
            e->accept();return true;
        }
    }
    if(e->type()==QEvent::Hide || e->type()==QEvent::WindowDeactivate || e->type()==QEvent::UngrabMouse) {
        finishInteraction(true);pressed_=-1;
    }
    if(e->type()==QEvent::FocusIn || e->type()==QEvent::FocusOut)update();
    return QWidget::event(e);
}
} // namespace imageeditor::ui
