#include "imageeditor/ui/ScreenshotDialog.hpp"
#include "imageeditor/ui/ScreenshotPreview.hpp"
#include "imageeditor/ui/CompactValueControl.hpp"
#include "imageeditor/ui/Localization.hpp"
#include "imageeditor/ui/ToolIcons.hpp"
#include "imageeditor/ui/RasterLimits.hpp"
#include <QComboBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <algorithm>

namespace imageeditor::ui {
ScreenshotDialog::ScreenshotDialog(QWidget& host,QWidget* parent,ScreenCapture* capture)
    :QDialog(parent,Qt::SubWindow),host_(&host),capture_(capture?capture:new ScreenCapture(this))
{
    setObjectName(QStringLiteral("ScreenshotDialog"));
    i18n::setWindowTitle(this,i18n::text("screenshot.title"));
    setSizeGripEnabled(false);setAttribute(Qt::WA_StyledBackground);
    setProperty("workspacePreferredSize",QSize(780,620));resize(780,620);
    auto* layout=new QVBoxLayout(this);layout->setContentsMargins(16,12,16,16);layout->setSpacing(10);
    layout->setSizeConstraint(QLayout::SetNoConstraint);
    auto* header=new QHBoxLayout;
    auto* title=i18n::make<QLabel>(i18n::text("screenshot.title"),this);
    auto font=title->font();font.setBold(true);title->setFont(font);header->addWidget(title,1);
    auto* close=new QPushButton(toolGlyph(ToolGlyph::Close),{},this);close->setObjectName("ScreenshotClose");
    i18n::setToolTip(close,i18n::text("screenshot.close"));i18n::setAccessibleName(close,i18n::text("screenshot.close"));
    close->setFixedSize(30,30);close->setAutoDefault(false);connect(close,&QPushButton::clicked,this,&QDialog::reject);
    header->addWidget(close);layout->addLayout(header);
    auto* options=new QHBoxLayout;
    target_=new QComboBox(this);target_->setObjectName("ScreenshotTarget");
    if(capture_->usesDesktopChooser())i18n::addItem(target_,i18n::text("screenshot.desktop.chooser"),int(ScreenCapture::Target::DesktopChooser));
    else {
        i18n::addItem(target_,i18n::text("screenshot.entire.desktop"),int(ScreenCapture::Target::EntireDesktop));
        i18n::addItem(target_,i18n::text("screenshot.active.window"),int(ScreenCapture::Target::ActiveWindow));
    }
    i18n::setAccessibleName(target_,i18n::text("screenshot.target"));options->addWidget(target_,1);
    delay_=new CompactValueControl(this);delay_->setObjectName("ScreenshotDelay");
    delay_->setRange(0,10);delay_->setDecimals(0);delay_->setSingleStep(1);delay_->setValue(capture_->usesDesktopChooser()?0:1);
    i18n::setPrefix(delay_,i18n::text("screenshot.delay"));delay_->setSuffix(QStringLiteral(" s"));
    i18n::setAccessibleName(delay_,i18n::text("screenshot.delay"));delay_->setFixedWidth(150);options->addWidget(delay_);
    captureButton_=new QPushButton(toolGlyph(ToolGlyph::Camera),{},this);captureButton_->setObjectName("ScreenshotCapture");
    captureButton_->setAutoDefault(false);options->addWidget(captureButton_);layout->addLayout(options);
    auto* hint=i18n::make<QLabel>(capture_->usesDesktopChooser()?i18n::text("screenshot.chooser.hint"):i18n::text("screenshot.native.hint"),this);
    hint->setWordWrap(true);hint->setObjectName(QStringLiteral("MutedLabel"));layout->addWidget(hint);
    preview_=new ScreenshotPreview(this);layout->addWidget(preview_,1);
    auto* cropRow=new QHBoxLayout;
    dimensions_=new QLabel(this);dimensions_->setObjectName("ScreenshotDimensions");cropRow->addWidget(dimensions_,1);
    zoom_=new QLabel(this);zoom_->setObjectName("ScreenshotZoom");
    zoom_->setMinimumWidth(zoom_->fontMetrics().horizontalAdvance(QStringLiteral("6400.0%")));
    zoom_->setAlignment(Qt::AlignRight|Qt::AlignVCenter);cropRow->addWidget(zoom_);
    resetViewButton_=i18n::make<QPushButton>(toolGlyph(ToolGlyph::FitCanvas),i18n::text("screenshot.reset.zoom"),this);
    resetViewButton_->setObjectName("ScreenshotResetZoom");resetViewButton_->setAutoDefault(false);cropRow->addWidget(resetViewButton_);
    resetButton_=i18n::make<QPushButton>(toolGlyph(ToolGlyph::ResetCrop),i18n::text("screenshot.reset.crop"),this);
    resetButton_->setObjectName("ScreenshotResetCrop");resetButton_->setAutoDefault(false);cropRow->addWidget(resetButton_);layout->addLayout(cropRow);
    auto* cropHint=i18n::make<QLabel>(i18n::text("screenshot.crop.hint"),this);cropHint->setWordWrap(true);layout->addWidget(cropHint);
    status_=new QLabel(this);status_->setObjectName("ScreenshotStatus");status_->setWordWrap(true);layout->addWidget(status_);
    auto* footer=new QHBoxLayout;footer_=footer;
    stopButton_=i18n::make<QPushButton>(i18n::text("screenshot.stop"),this);stopButton_->setObjectName("ScreenshotStop");stopButton_->setAutoDefault(false);
    footer->addWidget(stopButton_);footer->addStretch();
    auto* cancel=i18n::make<QPushButton>(i18n::text("screenshot.cancel"),this);cancel->setObjectName("ScreenshotCancel");cancel->setAutoDefault(false);
    footer->addWidget(cancel);
    layerButton_=i18n::make<QPushButton>(i18n::text("screenshot.add.layer"),this);
    layerButton_->setObjectName("ScreenshotAddLayer");layerButton_->setAutoDefault(false);footer->addWidget(layerButton_);
    i18n::setToolTip(layerButton_,i18n::text("screenshot.add.layer.hint"));
    openButton_=i18n::make<QPushButton>(i18n::text("screenshot.open"),this);openButton_->setObjectName("ScreenshotOpen");openButton_->setDefault(true);footer->addWidget(openButton_);
    layout->addLayout(footer);
    connect(cancel,&QPushButton::clicked,this,&QDialog::reject);
    connect(openButton_,&QPushButton::clicked,this,[this]{output_=Output::NewDocument;accept();});
    connect(layerButton_,&QPushButton::clicked,this,[this]{output_=Output::Layer;accept();});
    connect(captureButton_,&QPushButton::clicked,this,&ScreenshotDialog::beginCapture);
    connect(stopButton_,&QPushButton::clicked,this,[this]{cancelCapture(true);});
    connect(resetButton_,&QPushButton::clicked,this,[this]{preview_->resetCrop();});
    connect(resetViewButton_,&QPushButton::clicked,this,[this]{preview_->resetView();});
    preview_->onCropChanged=[this]{refresh();};
    preview_->onViewChanged=[this]{refresh();};
    timer_.setInterval(50);
    connect(&timer_,&QTimer::timeout,this,[this]{
        if(!capturing_||!capture_)return;
        const auto left=delayMs_-clock_.elapsed();
        if(left>0){i18n::setText(status_,i18n::text("screenshot.countdown").arg((left+999)/1000));return;}
        timer_.stop();i18n::setText(status_,i18n::text("screenshot.waiting"));
        capture_->start(generation_,ScreenCapture::Target(target_->currentData().toInt()));
    });
    connect(capture_,&ScreenCapture::finished,this,[this](quint64 id,QImage image,QString error,bool cancelled){
        if(!capturing_||id!=generation_)return;
        capturing_=false;timer_.stop();restoreHost(true);
        if(!error.isEmpty())showError(error);
        else if(cancelled)i18n::setText(status_,i18n::text("screenshot.cancelled"));
        else if(image.isNull()||!rasterExtentWithinLimits(std::uint32_t(image.width()),std::uint32_t(image.height())))
            showError(QStringLiteral("The captured image is empty or exceeds the image limits."));
        else {preview_->setImage(std::move(image));status_->clear();}
        refresh();
    });
    connect(capture_,&QObject::destroyed,this,[this]{cancelCapture(false);showError(QStringLiteral("The screenshot service closed."));});
    refresh();
}
ScreenshotDialog::~ScreenshotDialog()
{
    // An owned capture service is deleted by QObject after our members. Its
    // destroyed/finished callbacks must not touch the timer or controls then.
    if(capture_)disconnect(capture_,nullptr,this,nullptr);
    cancelCapture(false);
}
bool ScreenshotDialog::event(QEvent* event)
{
    const bool handled=QDialog::event(event);
    if(footer_&&(event->type()==QEvent::LayoutRequest||event->type()==QEvent::Resize)) {
        // A local pack or fallback font can make the actions wider than the
        // workspace. Stack them rather than clipping their translated labels.
        int required=0,count=0;
        for(int n=0;n<footer_->count();++n)if(auto* widget=footer_->itemAt(n)->widget();widget&&!widget->isHidden()) {
            required+=widget->minimumSizeHint().width();++count;
        }
        required+=std::max(0,count-1)*footer_->spacing();
        const auto margins=layout()->contentsMargins();
        const auto direction=required>width()-margins.left()-margins.right()
            ?QBoxLayout::TopToBottom:QBoxLayout::LeftToRight;
        if(footer_->direction()!=direction)footer_->setDirection(direction);
    }
    return handled;
}
void ScreenshotDialog::beginCapture()
{
    if(capturing_||!capture_)return;
    ++generation_;capturing_=true;delay_->finishEditing();delayMs_=std::max(qint64(250),qint64(delay_->value()*1000));
    refresh();i18n::setText(status_,i18n::text("screenshot.waiting"));
    const auto generation=generation_;
    // Defer the window-manager request until this button/close event settles.
    // An immediate Cancel must not leave a late minimize request in flight.
    QTimer::singleShot(0,this,[this,generation]{
        if(!capturing_||generation_!=generation)return;
        if(host_&&host_->isVisible()&&!host_->isMinimized()) {
            previousState_=host_->windowState();minimizedHost_=true;host_->showMinimized();
        }
        clock_.start();timer_.start();
    });
}
void ScreenshotDialog::restoreHost(bool activate)
{
    if(!minimizedHost_)return;
    minimizedHost_=false;
    if(host_) {
        host_->setWindowState(previousState_);
        host_->show();
        if(activate){host_->raise();host_->activateWindow();}
    }
}
void ScreenshotDialog::cancelCapture(bool showStatus)
{
    const bool pending=capturing_;++generation_;capturing_=false;timer_.stop();
    if(capture_)capture_->cancel();
    restoreHost(showStatus);
    if(pending&&showStatus)i18n::setText(status_,i18n::text("screenshot.cancelled"));
    refresh();
}
void ScreenshotDialog::refresh()
{
    const bool hasImage=!preview_->image().isNull();
    preview_->setEnabled(!capturing_);target_->setEnabled(!capturing_&&target_->count()>1);delay_->setEnabled(!capturing_);
    captureButton_->setEnabled(!capturing_&&capture_);stopButton_->setEnabled(capturing_);
    resetButton_->setEnabled(!capturing_&&hasImage&&preview_->crop()!=preview_->image().rect());openButton_->setEnabled(!capturing_&&hasImage);
    layerButton_->setEnabled(!capturing_&&hasImage&&canAddLayer_);
    resetViewButton_->setEnabled(!capturing_&&hasImage&&!preview_->viewIsFitted());
    zoom_->setText(hasImage?QString::number(preview_->zoom()*100,'f',1)+QLatin1Char('%'):QString{});
    i18n::setText(captureButton_,hasImage?i18n::text("screenshot.retake"):i18n::text("screenshot.capture"));
    if(hasImage)i18n::setText(dimensions_,i18n::text("screenshot.dimensions").arg(preview_->crop().width()).arg(preview_->crop().height()));
    else dimensions_->clear();
}
QImage ScreenshotDialog::selectedImage() const{return preview_->croppedImage();}
void ScreenshotDialog::setCanAddLayer(bool value){canAddLayer_=value;refresh();}
void ScreenshotDialog::showError(const QString& error){i18n::setText(status_,i18n::text("screenshot.failed").arg(error));}
void ScreenshotDialog::done(int result)
{
    if(result==Accepted&&(capturing_||preview_->image().isNull()||(output_==Output::Layer&&!canAddLayer_)))return;
    cancelCapture(false);QDialog::done(result);
}
}
