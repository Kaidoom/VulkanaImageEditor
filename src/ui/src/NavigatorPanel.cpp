#include "imageeditor/ui/NavigatorPanel.hpp"
#include "imageeditor/ui/Localization.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/ui/ToolIcons.hpp"
#include <QHideEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSlider>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace imageeditor::ui {
class NavigatorView final : public QWidget {
public:
    explicit NavigatorView(QWidget* parent) : QWidget(parent) {
        setObjectName(QStringLiteral("NavigatorView"));
        setMinimumSize(80, 60);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setCursor(Qt::OpenHandCursor);
        i18n::setToolTip(this, i18n::text("navigator.pan.hint"));
        i18n::property(this, "accessibleName", i18n::text("navigator.title"));
    }
    QImage image;
    core::Extent2u extent;
    QRectF visible;
    QString error;
    bool dragging = false;
    QPointF dragOffset;
    std::function<void(core::Vec2d)> navigate;
    QRectF imageRect() const {
        if (extent.empty()) return {};
        const auto area = QRectF(rect()).adjusted(4, 4, -4, -4);
        const double scale = std::min(area.width()/extent.width, area.height()/extent.height);
        const QSizeF size(extent.width*scale, extent.height*scale);
        return {area.center()-QPointF(size.width()/2, size.height()/2), size};
    }
    QPointF toDocument(QPointF p) const {
        const auto r = imageRect();
        return {(p.x()-r.x())/r.width()*extent.width, (p.y()-r.y())/r.height()*extent.height};
    }
    QRectF visibleRect() const {
        const auto r = imageRect();
        if (r.isEmpty()) return {};
        return {r.x()+visible.x()/extent.width*r.width(), r.y()+visible.y()/extent.height*r.height(),
            visible.width()/extent.width*r.width(), visible.height()/extent.height*r.height()};
    }
    void moveTo(QPointF p) {
        p = toDocument(p) + dragOffset;
        if (navigate) navigate({p.x(), p.y()});
    }
protected:
    bool event(QEvent* event) override {
        if (event->type()==QEvent::Hide || event->type()==QEvent::WindowDeactivate
            || event->type()==QEvent::TouchCancel || event->type()==QEvent::FocusOut) {
            dragging=false; setCursor(Qt::OpenHandCursor);
        }
        return QWidget::event(event);
    }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), themeColor(ThemeColor::Canvas));
        const auto r=imageRect();
        if (r.isEmpty()) return;
        p.save(); p.setClipRect(r);
        for(int y=int(r.top()); y<r.bottom(); y+=8)
            for(int x=int(r.left()); x<r.right(); x+=8)
                p.fillRect(QRect(x,y,8,8), ((x-int(r.left()))/8+(y-int(r.top()))/8)%2
                    ?themeColor(ThemeColor::CheckerDark):themeColor(ThemeColor::CheckerLight));
        if (!image.isNull()) {
            p.setRenderHint(QPainter::SmoothPixmapTransform);
            p.drawImage(r,image);
        } else {
            p.fillRect(r,themeColor(ThemeColor::Surface));
            p.setPen(palette().color(QPalette::Text));
            p.drawText(r,Qt::AlignCenter|Qt::TextWordWrap, error.isEmpty()
                ?i18n::text("navigator.preparing"):i18n::text("navigator.unavailable"));
        }
        p.restore(); p.setPen(themeColor(ThemeColor::Border)); p.drawRect(r);
        // The viewport can include pasteboard. Draw its actual rectangle in
        // the whole navigator, without reshaping it to the document boundary.
        const auto outline=visibleRect();
        if (!outline.isEmpty()) {
            p.setPen(QPen(QColor(0,0,0,210),3)); p.drawRect(outline);
            p.setPen(QPen(themeColor(ThemeColor::Accent),1.5)); p.drawRect(outline);
        }
    }
    void mousePressEvent(QMouseEvent* e) override {
        if (e->button()!=Qt::LeftButton || imageRect().isEmpty() || !rect().contains(e->position().toPoint())) {
            QWidget::mousePressEvent(e); return;
        }
        dragging=true;
        dragOffset=visibleRect().contains(e->position()) ? visible.center()-toDocument(e->position()) : QPointF{};
        setCursor(Qt::ClosedHandCursor); moveTo(e->position()); e->accept();
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (dragging && e->buttons().testFlag(Qt::LeftButton)) {moveTo(e->position());e->accept();}
        else QWidget::mouseMoveEvent(e);
    }
    void mouseReleaseEvent(QMouseEvent* e) override {
        if (e->button()==Qt::LeftButton && dragging) {
            moveTo(e->position()); dragging=false;setCursor(Qt::OpenHandCursor);e->accept();
        } else QWidget::mouseReleaseEvent(e);
    }
};

NavigatorPanel::NavigatorPanel(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("NavigatorPanel"));
    auto* layout=new QVBoxLayout(this);layout->setContentsMargins(6,4,6,2);layout->setSpacing(2);
    view_=new NavigatorView(this);layout->addWidget(view_,1);
    auto* row=new QHBoxLayout;row->setSpacing(6);
    zoom_=new QSlider(Qt::Horizontal,this);zoom_->setObjectName(QStringLiteral("NavigatorZoom"));
    zoom_->setLayoutDirection(Qt::LeftToRight);zoom_->setRange(0,1000);
    zoom_->setSingleStep(10);zoom_->setPageStep(50);
    // Keep a comfortable input area around the slimmer painted track/handle.
    zoom_->setFixedHeight(22);
    i18n::setToolTip(zoom_,i18n::text("navigator.zoom"));
    i18n::property(zoom_,"accessibleName",i18n::text("navigator.zoom"));
    percentage_=new QLabel(this);percentage_->setObjectName(QStringLiteral("NavigatorZoomPercent"));
    percentage_->setLayoutDirection(Qt::LeftToRight);
    percentage_->setAlignment(Qt::AlignRight|Qt::AlignVCenter);
    percentage_->setMinimumWidth(percentage_->fontMetrics().horizontalAdvance(QStringLiteral("6400.0%")));
    reset_=new QToolButton(this);reset_->setObjectName(QStringLiteral("NavigatorResetView"));
    reset_->setIcon(toolGlyph(ToolGlyph::ResetCrop));reset_->setIconSize({16,16});
    reset_->setFixedSize(22,22);reset_->setAutoRaise(true);reset_->setFocusPolicy(Qt::NoFocus);
    reset_->setStyleSheet(QStringLiteral("QToolButton { padding: 1px; border-radius: 4px; }"));
    i18n::setToolTip(reset_,i18n::text("navigator.reset.view"));
    i18n::property(reset_,"accessibleName",i18n::text("navigator.reset.view"));
    connect(reset_,&QToolButton::clicked,this,[this]{if(onResetView)onResetView();});
    row->addWidget(zoom_,1);row->addWidget(percentage_);row->addWidget(reset_);layout->addLayout(row);
    connect(zoom_,&QSlider::valueChanged,this,[this](int value){
        if(onZoom)onZoom(.01*std::pow(6400.0,value/1000.0));
    });
    view_->navigate=[this](core::Vec2d point){if(onNavigate)onNavigate(point);};
    preview_.onReady=[this](auto surface,QString error){
        view_->image={};view_->error=std::move(error);
        i18n::setToolTip(view_,view_->error.isEmpty()?i18n::text("navigator.pan.hint")
            :i18n::text("navigator.unavailable")+QStringLiteral("\n")+view_->error);
        if(surface) {
            const auto e=surface->extent();
            QImage image(int(e.width),int(e.height),QImage::Format_RGBA8888);
            surface->copyRgba8({0,0,int(e.width),int(e.height)},
                {reinterpret_cast<std::byte*>(image.bits()),std::size_t(image.sizeInBytes())},std::size_t(image.bytesPerLine()));
            view_->image=std::move(image);
        }
        view_->update();
    };
    setDocument(0,{});
}
NavigatorPanel::~NavigatorPanel(){shutdown();}
void NavigatorPanel::shutdown()
{
    onSourceRequested={};onNavigate={};onZoom={};onResetView={};preview_.onReady={};preview_.setEnabled(false);
}
void NavigatorPanel::setDocument(std::uint64_t instance,const core::DocumentSnapshot& snapshot)
{
    if(instance_!=instance) {view_->dragging=false;view_->setCursor(Qt::OpenHandCursor);}
    instance_=instance;extent_=snapshot.canvas.extent;
    preview_.setDocumentInstance(instance);
    view_->extent=extent_;zoom_->setEnabled(!extent_.empty());view_->setEnabled(!extent_.empty());
    reset_->setEnabled(!extent_.empty());
    if(extent_.empty()){view_->image={};percentage_->clear();}
    if(isVisible())preview_.request(snapshot);
    view_->update();
}
void NavigatorPanel::setView(core::ViewportState state,core::Extent2d viewport,QRectF available)
{
    available_=available;
    const core::Extent2d document{double(extent_.width),double(extent_.height)};
    const auto top=state.viewportToDocument({available.x(),available.y()},document,viewport);
    const QRectF visible(top.x,top.y,available.width()/state.zoom(),available.height()/state.zoom());
    if(view_->visible!=visible){view_->visible=visible;view_->update();}
    const QSignalBlocker blocker(zoom_);
    zoom_->setValue(int(std::lround(std::log(state.zoom()/.01)/std::log(6400.0)*1000)));
    if(!extent_.empty())percentage_->setText(QString::number(state.zoom()*100,'f',state.zoom()<10?1:0)+QLatin1Char('%'));
}
void NavigatorPanel::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);preview_.setEnabled(true);
    if(onSourceRequested)onSourceRequested();
}
void NavigatorPanel::hideEvent(QHideEvent* e)
{
    preview_.setEnabled(false);QWidget::hideEvent(e);
}
QRectF NavigatorPanel::imageRect() const {return view_->imageRect();}
QRectF NavigatorPanel::visibleRect() const {return view_->visibleRect();}
}
