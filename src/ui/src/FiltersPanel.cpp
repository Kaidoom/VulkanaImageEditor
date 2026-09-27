#include "imageeditor/ui/FiltersPanel.hpp"
#include <algorithm>
#include "imageeditor/ui/CompactValueControl.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QHideEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QShowEvent>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace imageeditor::ui {
namespace {
using Type = core::SpatialFilterType;
constexpr std::size_t slot(Type type) { return std::size_t(type); }
QLabel* note(QVBoxLayout* layout, const QString& text)
{
    auto* label = new QLabel(text);
    label->setObjectName(QStringLiteral("MutedLabel"));
    label->setTextFormat(Qt::PlainText); label->setWordWrap(true);
    layout->addWidget(label); return label;
}
}
FiltersPanel::FiltersPanel(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("FiltersPanelContent"));
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(10,8,10,8); outer->setSpacing(8);
    targetLabel_ = new QLabel;
    targetLabel_->setObjectName(QStringLiteral("FilterTarget"));
    targetLabel_->setTextFormat(Qt::PlainText); targetLabel_->setWordWrap(true);
    outer->addWidget(targetLabel_);
    compare_ = new QPushButton(tr("Hold for Before"));
    compare_->setObjectName(QStringLiteral("FilterCompare"));
    compare_->setToolTip(tr("Bypass this layer's spatial filters on canvas only. Color adjustments remain applied. Release to return to After."));
    outer->addWidget(compare_);
    connect(compare_,&QPushButton::pressed,this,[this]{finishEditing();if(onComparison)onComparison(true);});
    connect(compare_,&QPushButton::released,this,[this]{if(onComparison)onComparison(false);});
    compare_->installEventFilter(this);
    navigation_ = new QComboBox;
    navigation_->setObjectName(QStringLiteral("FilterNavigation"));
    for (auto type : core::allSpatialFilterTypes)
        navigation_->addItem(QString::fromUtf8(core::spatialFilterName(type)),int(type));
    navigation_->setMaxVisibleItems(int(core::spatialFilterCount));
    auto* modeRow=new QHBoxLayout;modeRow->setSpacing(8);
    modeRow->addWidget(navigation_,1);
    auto* enabledStack=new QStackedWidget;
    enabledStack->setObjectName(QStringLiteral("FilterEnabledStack"));
    enabledStack->setSizePolicy(QSizePolicy::Fixed,QSizePolicy::Fixed);
    modeRow->addWidget(enabledStack,0,Qt::AlignVCenter);outer->addLayout(modeRow);
    auto* scroll = new QScrollArea;
    scroll->setFrameShape(QFrame::NoFrame); scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    stack_ = new QStackedWidget;
    stack_->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);
    scroll->setWidget(stack_); outer->addWidget(scroll,1);
    for (auto type : core::allSpatialFilterTypes) enabledStack->addWidget(makePage(type).enabled);
    connect(navigation_,&QComboBox::currentIndexChanged,this,[this,enabledStack](int index){
        finishEditing(); stack_->setCurrentIndex(index);enabledStack->setCurrentIndex(index);refresh();
    });
    auto& gaussian = pages_[slot(Type::Gaussian)];
    linkedControl_ = new QCheckBox(tr("Link horizontal / vertical"));
    linkedControl_->setObjectName(QStringLiteral("FilterGaussianLinked"));
    linkedControl_->setChecked(true); gaussian.optionsRow->addWidget(linkedControl_);
    connect(linkedControl_,&QCheckBox::toggled,this,[this](bool value){if(!updating_){finishEditing();linked_=value;}});
    number(gaussian,"FilterGaussianRadiusX",tr("Horizontal"),core::maximumSpatialRadius,1,tr(" px"),
        [this]{return std::get<core::GaussianBlurParameters>(item(Type::Gaussian).parameters).radiusX;},
        [this](double value){auto& p=std::get<core::GaussianBlurParameters>(item(Type::Gaussian).parameters);p.radiusX=value;if(linked_)p.radiusY=value;});
    number(gaussian,"FilterGaussianRadiusY",tr("Vertical"),core::maximumSpatialRadius,1,tr(" px"),
        [this]{return std::get<core::GaussianBlurParameters>(item(Type::Gaussian).parameters).radiusY;},
        [this](double value){auto& p=std::get<core::GaussianBlurParameters>(item(Type::Gaussian).parameters);p.radiusY=value;if(linked_)p.radiusX=value;});
    note(gaussian.layout,tr("Smooth, separable Gaussian. Radius = 3σ; zero is unchanged. Amounts use layer-local pixels, so transforms carry the effect with the layer."));
    auto& motion = pages_[slot(Type::Motion)];
    number(motion,"FilterMotionDistance",tr("Distance"),core::maximumMotionDistance,1,tr(" px"),
        [this]{return std::get<core::MotionBlurParameters>(item(Type::Motion).parameters).distance;},
        [this](double value){std::get<core::MotionBlurParameters>(item(Type::Motion).parameters).distance=value;});
    number(motion,"FilterMotionAngle",tr("Angle"),180,1,QStringLiteral("°"),
        [this]{return std::get<core::MotionBlurParameters>(item(Type::Motion).parameters).angle;},
        [this](double value){std::get<core::MotionBlurParameters>(item(Type::Motion).parameters).angle=value;},-180);
    note(motion.layout,tr("Distance is the full centered trail length in local pixels. 0° runs horizontally; 90° vertically. The layer origin does not move."));
    auto& lens = pages_[slot(Type::Lens)];
    number(lens,"FilterLensRadius",tr("Radius"),core::maximumSpatialRadius,1,tr(" px"),
        [this]{return std::get<core::LensBlurParameters>(item(Type::Lens).parameters).radius;},
        [this](double value){std::get<core::LensBlurParameters>(item(Type::Lens).parameters).radius=value;});
    aperture_ = new QComboBox;
    aperture_->setObjectName(QStringLiteral("FilterLensAperture"));
    aperture_->addItem(tr("Circular aperture"),0);
    for(int blades=3;blades<=12;++blades)aperture_->addItem(tr("%1 blades").arg(blades),blades);
    aperture_->setMaxVisibleItems(aperture_->count()); lens.layout->addWidget(aperture_);
    connect(aperture_,&QComboBox::currentIndexChanged,this,[this](int index){
        if(updating_)return;
        change(Type::Lens,[this,index](auto& filter){std::get<core::LensBlurParameters>(filter.parameters).blades=std::uint32_t(aperture_->itemData(index).toUInt());},true);
    });
    number(lens,"FilterLensRotation",tr("Aperture rotation"),180,1,QStringLiteral("°"),
        [this]{return std::get<core::LensBlurParameters>(item(Type::Lens).parameters).rotation;},
        [this](double value){std::get<core::LensBlurParameters>(item(Type::Lens).parameters).rotation=value;},-180);
    note(lens.layout,tr("Uniform aperture-shaped defocus, not depth-aware lens simulation. Radius is the aperture's outer radius; polygon rotation is clockwise."));
    for (auto& page : pages_) {
        page.optionsRow->addStretch();
        note(page.layout,tr("Transparent outside the source. Captured masks restrict the result, not the input neighborhood. Order: adjustments → Gaussian → Motion → Lens → effects → final crop/opacity."));
        page.layout->addStretch();
    }
    progressRow_=new QWidget;
    auto* progressLayout=new QVBoxLayout(progressRow_);progressLayout->setContentsMargins(0,0,0,0);
    progressText_=new QLabel;progressText_->setObjectName(QStringLiteral("FilterProcessingStatus"));
    progressText_->setTextFormat(Qt::PlainText);progressText_->setWordWrap(true);progressLayout->addWidget(progressText_);
    progress_=new QProgressBar;progress_->setRange(0,100);progress_->setTextVisible(false);progressLayout->addWidget(progress_);
    auto* cancel=new QPushButton(tr("Cancel processing"));cancel->setObjectName(QStringLiteral("FilterCancelProcessing"));progressLayout->addWidget(cancel);
    connect(cancel,&QPushButton::clicked,this,[this]{if(onCancelProcessing)onCancelProcessing();});
    outer->addWidget(progressRow_);progressRow_->hide();
    setTarget(nullptr,false);
}
FiltersPanel::Page& FiltersPanel::makePage(Type type)
{
    auto& page=pages_[slot(type)];page.widget=new QWidget;page.layout=new QVBoxLayout(page.widget);
    page.layout->setContentsMargins(0,0,0,8);page.layout->setSpacing(8);
    page.enabled=new QCheckBox(tr("Enabled"));page.enabled->setObjectName(QStringLiteral("FilterEnabled%1").arg(slot(type)));
    auto* reset=new QPushButton(tr("Reset"));reset->setObjectName(QStringLiteral("FilterReset%1").arg(slot(type)));
    connect(page.enabled,&QCheckBox::toggled,this,[this,type](bool value){change(type,[value](auto& filter){filter.enabled=value;});});
    reset->setToolTip(tr("Disable this filter and reset its settings and captured selection."));
    connect(reset,&QPushButton::clicked,this,[this,type]{change(type,[type](auto& filter){filter=core::defaultSpatialFilter(type);});});
    page.preserveAlpha=new QCheckBox(tr("Preserve Alpha"));
    page.preserveAlpha->setObjectName(QStringLiteral("FilterPreserveAlpha%1").arg(slot(type)));
    page.preserveAlpha->setToolTip(tr("Keep original transparency; soften only color detail using alpha-weighted neighbors. Off allows silhouettes and trails to spread into transparency."));
    connect(page.preserveAlpha,&QCheckBox::toggled,this,[this,type](bool value){change(type,[value](auto& filter){filter.preserveAlpha=value;});});
    auto* scope=new QHBoxLayout;scope->setSpacing(6);
    page.scope=new QComboBox;page.scope->addItems({tr("Whole Layer"),tr("Captured Selection")});
    page.scope->setObjectName(QStringLiteral("FilterScope%1").arg(slot(type)));page.scope->setMaxVisibleItems(2);
    page.capture=new QPushButton(tr("Capture"));page.capture->setObjectName(QStringLiteral("FilterCapture%1").arg(slot(type)));
    scope->addWidget(page.scope,1);scope->addWidget(page.capture);scope->addWidget(reset);page.layout->addLayout(scope);
    page.maskStatus=note(page.layout,{});
    page.showRegion=new QCheckBox(tr("Show captured region"));page.showRegion->setObjectName(QStringLiteral("FilterShowRegion%1").arg(slot(type)));page.layout->addWidget(page.showRegion);
    page.optionsRow=new QHBoxLayout;page.optionsRow->setSpacing(8);
    page.optionsRow->addWidget(page.preserveAlpha);page.layout->addLayout(page.optionsRow);
    connect(page.showRegion,&QCheckBox::toggled,this,[this](bool show){if(!updating_){showCaptured_=show;refresh();}});
    connect(page.scope,&QComboBox::currentIndexChanged,this,[this,type](int index){
        if(updating_)return;
        finishEditing();
        if(index==0)change(type,[](auto& filter){filter.mask.reset();});
        else if(!item(type).mask&&hasSelection_&&onCaptureSelection)onCaptureSelection(type);
        refresh();
    });
    connect(page.capture,&QPushButton::clicked,this,[this,type]{finishEditing();if(onCaptureSelection)onCaptureSelection(type);});
    stack_->addWidget(page.widget);return page;
}
void FiltersPanel::number(Page& page,const char* name,const QString& label,double maximum,int decimals,
    const QString& suffix,std::function<double()> read,std::function<void(double)> write,double minimum)
{
    auto* control=new CompactValueControl;
    control->setObjectName(QString::fromLatin1(name));control->setPrefix(label+QStringLiteral(": "));control->setSuffix(suffix);
    control->setDecimals(decimals);control->setRange(minimum,maximum);control->setSingleStep(1);
    control->setFixedHeight(30);control->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);control->setAccessibleName(label);
    page.layout->addWidget(control);page.numbers.push_back(control);
    const auto type=Type(&page-pages_.data());const auto previous=page.refresh;
    page.refresh=[previous,control,read]{if(previous)previous();if(!control->interactionActive()){const QSignalBlocker guard(control);control->setValue(read());}};
    control->onInteractionStarted=[this,type]{if(!updating_)begin(type);};
    control->onInteractionFinished=[this]{if(!updating_)finishEditing();};
    connect(control,&QDoubleSpinBox::valueChanged,this,[this,type,write,control](double value){
        if(updating_||!target_)return;
        if(!begin(type)){refresh();return;}
        write(value);item(type).enabled=true;autoEnabled_=true;
        if(onPreview)onPreview(std::make_shared<const core::SpatialFilterStack>(working_));
        refresh();if(!control->interactionActive())finishEditing();
    });
    control->installEventFilter(this);for(auto* child:control->findChildren<QLineEdit*>())child->installEventFilter(this);
}
core::LayerSpatialFilter& FiltersPanel::item(Type type){return working_.items[slot(type)];}
Type FiltersPanel::currentType() const{return Type(navigation_->currentData().toUInt());}
void FiltersPanel::selectType(Type type){navigation_->setCurrentIndex(int(type));}
void FiltersPanel::setEmbedded()
{
    // The owning Adjustments workspace supplies one shared target/action header.
    targetLabel_->hide();compare_->hide();
    layout()->setContentsMargins(8,8,8,8);
}
void FiltersPanel::resetAll()
{
    finishEditing();if(!begin(currentType()))return;
    working_=core::SpatialFilterStack{};
    if(onPreview)onPreview(std::make_shared<const core::SpatialFilterStack>(working_));
    finishEditing();refresh();
}
bool FiltersPanel::begin(Type type)
{
    if(updating_||finishing_||!target_)return false;
    if(editing_&&editingType_==type)return true;
    if(editing_)finishEditing();
    if(onInteractionStarted&&!onInteractionStarted(type))return false;
    before_=working_;autoEnabled_=false;editing_=true;editingType_=type;return true;
}
void FiltersPanel::change(Type type,const std::function<void(core::LayerSpatialFilter&)>& action,bool enable)
{
    if(updating_)return;
    finishEditing();if(!begin(type))return;
    action(item(type));if(enable)item(type).enabled=true;
    if(onPreview)onPreview(std::make_shared<const core::SpatialFilterStack>(working_));
    finishEditing();refresh();
}
void FiltersPanel::finishEditing(bool commit)
{
    if(finishing_)return;
    const QScopedValueRollback guard(finishing_,true);
    const bool had=editing_;editing_=false;
    if(had&&commit&&autoEnabled_) {
        auto candidate=working_;candidate.items[slot(editingType_)].enabled=before_.items[slot(editingType_)].enabled;
        if(core::equivalentSpatialFilters(std::make_shared<const core::SpatialFilterStack>(candidate),
            std::make_shared<const core::SpatialFilterStack>(before_))) {
            working_=std::move(candidate);
            if(onPreview)onPreview(std::make_shared<const core::SpatialFilterStack>(working_));
        }
    }
    for(auto& page:pages_)for(auto* control:page.numbers)control->finishEditing(commit,true);
    if(had&&!commit)working_=before_;
    if(had&&onInteractionFinished)onInteractionFinished(commit);
}
void FiltersPanel::setTarget(const core::Layer* layer,bool hasSelection)
{
    const auto next=layer?std::optional(layer->id):std::nullopt;
    if(next!=target_){finishEditing(false);if(onComparison)onComparison(false);}
    target_=next;hasSelection_=hasSelection;
    working_=layer&&layer->filters?*layer->filters:core::SpatialFilterStack{};
    targetLabel_->setText(layer?tr("%1 · primary layer").arg(QString::fromStdString(layer->name)):tr("Select a raster, text, or shape layer"));
    navigation_->setEnabled(layer);stack_->setEnabled(layer);compare_->setEnabled(layer);refresh();
}
void FiltersPanel::refresh()
{
    const QScopedValueRollback guard(updating_,true);
    for(std::size_t i=0;i<pages_.size();++i) {
        auto& page=pages_[i];const auto& filter=working_.items[i];
        page.enabled->setEnabled(target_.has_value());
        page.enabled->setChecked(filter.enabled);page.preserveAlpha->setChecked(filter.preserveAlpha);
        page.scope->setCurrentIndex(filter.mask?1:0);page.capture->setEnabled(target_&&hasSelection_);
        page.maskStatus->setText(filter.mask?(filter.mask->coverage->bounds().empty()?tr("Captured empty mask · no affected pixels"):tr("Captured mask · follows this layer")):QString{});
        page.maskStatus->setVisible(filter.mask.has_value());page.showRegion->setVisible(filter.mask.has_value());
        page.showRegion->setChecked(showCaptured_);page.showRegion->setEnabled(filter.mask&&!filter.mask->coverage->bounds().empty());
        if(page.refresh)page.refresh();
    }
    if(linkedControl_)linkedControl_->setChecked(linked_);
    if(aperture_)aperture_->setCurrentIndex(aperture_->findData(int(std::get<core::LensBlurParameters>(item(Type::Lens).parameters).blades)));
    if(onCapturedRegionChanged)onCapturedRegionChanged();
}
void FiltersPanel::setProcessing(bool busy,double progress,const QString& message)
{
    progressRow_->setVisible(busy||!message.isEmpty());progress_->setVisible(busy);
    progress_->setValue(qRound(std::clamp(progress,0.0,1.0)*100));
    progressText_->setText(message.isEmpty()?tr("Preparing layer filters…"):message);
    progressRow_->findChild<QPushButton*>()->setVisible(busy);
}
bool FiltersPanel::eventFilter(QObject* watched,QEvent* event)
{
    if(watched==compare_&&(event->type()==QEvent::TouchCancel||event->type()==QEvent::Hide||event->type()==QEvent::WindowDeactivate)) {
        compare_->setDown(false);if(onComparison)onComparison(false);
    }
    if(editing_&&event->type()==QEvent::KeyPress&&static_cast<QKeyEvent*>(event)->key()==Qt::Key_Escape){finishEditing(false);event->accept();return true;}
    return QWidget::eventFilter(watched,event);
}
void FiltersPanel::hideEvent(QHideEvent* event)
{
    finishEditing();if(onComparison)onComparison(false);QWidget::hideEvent(event);if(onCapturedRegionChanged)onCapturedRegionChanged();
}
void FiltersPanel::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);if(onCapturedRegionChanged)onCapturedRegionChanged();
}
}
