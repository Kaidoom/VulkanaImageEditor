#include "imageeditor/ui/AdjustmentsPanel.hpp"
#include "imageeditor/ui/EffectsPanel.hpp"
#include "imageeditor/ui/CompactValueControl.hpp"
#include "imageeditor/ui/FiltersPanel.hpp"
#include "imageeditor/ui/Theme.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QHideEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QShowEvent>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTabBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QWindow>
#include <algorithm>

namespace imageeditor::ui {
namespace {
// Hidden adjustment pages must not force the active page to scroll.
class AdjustmentPageStack final : public QStackedWidget {
public:
    AdjustmentPageStack()
    {
        layout()->setSizeConstraint(QLayout::SetNoConstraint);
        connect(this, &QStackedWidget::currentChanged, this, [this] { updateGeometry(); });
    }
    QSize sizeHint() const override { return currentWidget() ? currentWidget()->sizeHint() : QSize{}; }
    QSize minimumSizeHint() const override { return currentWidget() ? currentWidget()->minimumSizeHint() : QSize{}; }
    bool hasHeightForWidth() const override { return currentWidget() && currentWidget()->hasHeightForWidth(); }
    int heightForWidth(int width) const override
    {
        return hasHeightForWidth() ? currentWidget()->heightForWidth(width) : sizeHint().height();
    }
};
using Type = core::AdjustmentType;
constexpr std::size_t index(Type type) { return static_cast<std::size_t>(type); }
QString displayName(Type type) { return QString::fromUtf8(core::adjustmentName(type)); }
QLabel* explanation(QVBoxLayout* layout, const QString& text)
{
    auto* label = new QLabel(text);
    label->setObjectName(QStringLiteral("MutedLabel"));
    label->setWordWrap(true);
    label->setTextFormat(Qt::PlainText);
    layout->addWidget(label);
    return label;
}
}
AdjustmentsPanel::AdjustmentsPanel(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("AdjustmentsPanelContent"));
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(10, 8, 10, 8); outer->setSpacing(8);
    targetLabel_ = new QLabel;
    targetLabel_->setObjectName(QStringLiteral("AdjustmentTarget"));
    targetLabel_->setTextFormat(Qt::PlainText); targetLabel_->setWordWrap(true);
    outer->addWidget(targetLabel_);
    scopeLabel_=new QLabel;
    scopeLabel_->setObjectName(QStringLiteral("AdjustmentLayerScope"));
    scopeLabel_->setWordWrap(true);scopeLabel_->setTextFormat(Qt::PlainText);scopeLabel_->hide();
    outer->addWidget(scopeLabel_);
    auto* actions = new QHBoxLayout;
    actions->setSpacing(6);
    compare_ = new QPushButton(QStringLiteral("Hold for Before"));
    compare_->setObjectName(QStringLiteral("AdjustmentCompare"));
    compare_->setToolTip(QStringLiteral("Temporarily bypass this layer's adjustments on the canvas only. Release to return to After."));
    resetAll_ = new QPushButton(QStringLiteral("Reset All"));
    resetAll_->setObjectName(QStringLiteral("AdjustmentResetAll"));
    actions->addWidget(compare_,1);actions->addWidget(resetAll_);
    outer->addLayout(actions);
    connect(compare_,&QPushButton::pressed,this,[this]{
        finishEditing();
        if(effectsCategoryActive()){
            effectsPanel_->finishEditing();if(effectsPanel_->onComparison)effectsPanel_->onComparison(true);
        }else if(filtersCategoryActive()){
            filtersPanel_->finishEditing();if(filtersPanel_->onComparison)filtersPanel_->onComparison(true);
        }else if(onComparison)onComparison(true);
    });
    connect(compare_,&QPushButton::released,this,[this]{
        if(onComparison)onComparison(false);
        if(filtersPanel_->onComparison)filtersPanel_->onComparison(false);
        if(effectsPanel_->onComparison)effectsPanel_->onComparison(false);
    });
    compare_->installEventFilter(this);
    connect(resetAll_,&QPushButton::clicked,this,[this]{
        if(effectsCategoryActive()){effectsPanel_->resetAll();return;}
        if(filtersCategoryActive()){filtersPanel_->resetAll();return;}
        finishEditing(); if(!begin(currentType()))return;
        working_=core::AdjustmentStack{};
        if(onPreview)onPreview(std::make_shared<const core::AdjustmentStack>(working_));
        finishEditing();refresh();
    });
    tabs_ = new QTabWidget;
    tabs_->setObjectName(QStringLiteral("AdjustmentTabs"));
    outer->addWidget(tabs_,1);
    const std::array<QString,3> groupNames {QStringLiteral("Tone"),QStringLiteral("Color"),QStringLiteral("Monochrome")};
    const std::array<std::vector<Type>,3> groups {{
        {Type::Exposure,Type::BrightnessContrast,Type::Levels,Type::Curves},
        {Type::HueSaturation,Type::Vibrance,Type::ColorBalance,Type::WarmthTint},
        {Type::BlackWhite,Type::Invert}}};
    for(std::size_t i=0;i<groups.size();++i) {
        auto* tab=new QWidget;auto* layout=new QVBoxLayout(tab);
        layout->setContentsMargins(8,8,8,8);layout->setSpacing(8);
        auto* modeRow=new QHBoxLayout;modeRow->setSpacing(8);
        navigation_[i]=new QComboBox;
        navigation_[i]->setObjectName(QStringLiteral("AdjustmentNavigation%1").arg(i));
        for(const auto type:groups[i])navigation_[i]->addItem(displayName(type),int(type));
        navigation_[i]->setMaxVisibleItems(int(groups[i].size()));
        modeRow->addWidget(navigation_[i],1);
        auto* enabledStack=new QStackedWidget;
        enabledStack->setObjectName(QStringLiteral("AdjustmentEnabledStack%1").arg(i));
        enabledStack->setSizePolicy(QSizePolicy::Fixed,QSizePolicy::Fixed);
        modeRow->addWidget(enabledStack,0,Qt::AlignVCenter);layout->addLayout(modeRow);
        auto* scroll=new QScrollArea;
        scroll->setFrameShape(QFrame::NoFrame);scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        stacks_[i]=new AdjustmentPageStack;
        stacks_[i]->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);
        scroll->setWidget(stacks_[i]);layout->addWidget(scroll,1);
        for(const auto type:groups[i])enabledStack->addWidget(makePage(type,stacks_[i]).enabled);
        tabs_->addTab(tab,groupNames[i]);
        connect(navigation_[i],&QComboBox::currentIndexChanged,this,[this,i,enabledStack](int row){
            finishEditing();stacks_[i]->setCurrentIndex(row);enabledStack->setCurrentIndex(row);navigate();
        });
    }
    auto& exposure=pages_[index(Type::Exposure)];
    number(exposure,"AdjustmentExposure","Exposure",-20,20,2,
        [this]{return std::get<core::ExposureParameters>(item(Type::Exposure).parameters).stops;},
        [this](double v){std::get<core::ExposureParameters>(item(Type::Exposure).parameters).stops=v;},QStringLiteral(" EV"));
    explanation(exposure.layout,QStringLiteral("Linear-light exposure in stops. +1 EV doubles light; 0 EV is neutral. Original content is preserved."));

    auto& brightness=pages_[index(Type::BrightnessContrast)];
    number(brightness,"AdjustmentBrightness","Brightness",-100,100,0,
        [this]{return std::get<core::BrightnessContrastParameters>(item(Type::BrightnessContrast).parameters).brightness*100;},
        [this](double v){std::get<core::BrightnessContrastParameters>(item(Type::BrightnessContrast).parameters).brightness=v/100;});
    number(brightness,"AdjustmentContrast","Contrast",-100,100,0,
        [this]{return std::get<core::BrightnessContrastParameters>(item(Type::BrightnessContrast).parameters).contrast*100;},
        [this](double v){std::get<core::BrightnessContrastParameters>(item(Type::BrightnessContrast).parameters).contrast=v/100;});
    explanation(brightness.layout,QStringLiteral("Brightness and contrast are independent sRGB tone controls. Contrast pivots at 50%; zero is exactly neutral."));

    auto& levels=pages_[index(Type::Levels)];
    const QStringList channels {QStringLiteral("Composite RGB"),QStringLiteral("Red"),QStringLiteral("Green"),QStringLiteral("Blue")};
    levelsChannel_=choices(levels,"AdjustmentLevelsChannel",channels,[this](int){refresh();updateHistogramDisplay();});
    levelsHistogram_=new AdjustmentCurveEditor;
    levelsHistogram_->setObjectName(QStringLiteral("AdjustmentLevelsHistogram"));levelsHistogram_->setHistogramOnly(true);
    levels.layout->addWidget(levelsHistogram_);
    histogramLabels_[0]=explanation(levels.layout,QStringLiteral("Input histogram · alpha-weighted sRGB"));
    using LevelField=double core::LevelsChannel::*;
    const auto levelNumber=[this,&levels](const char* name,const QString& label,LevelField field,bool gamma=false){
        number(levels,name,label,gamma?.1:0,gamma?10:255,gamma?2:1,
            [this,field,gamma]{const auto& c=std::get<core::LevelsParameters>(item(Type::Levels).parameters).channels[std::size_t(levelsChannel_->currentIndex())];return (c.*field)*(gamma?1:255);},
            [this,field,gamma](double v){auto& c=std::get<core::LevelsParameters>(item(Type::Levels).parameters).channels[std::size_t(levelsChannel_->currentIndex())];
                auto value=v/(gamma?1:255);
                if(field==&core::LevelsChannel::inputBlack)value=std::min(value,c.inputWhite);
                if(field==&core::LevelsChannel::inputWhite)value=std::max(value,c.inputBlack);
                c.*field=value;});
    };
    levelNumber("AdjustmentLevelsInputBlack","Input black",&core::LevelsChannel::inputBlack);
    levelNumber("AdjustmentLevelsGamma","Gamma",&core::LevelsChannel::gamma,true);
    levelNumber("AdjustmentLevelsInputWhite","Input white",&core::LevelsChannel::inputWhite);
    levelNumber("AdjustmentLevelsOutputBlack","Output black",&core::LevelsChannel::outputBlack);
    levelNumber("AdjustmentLevelsOutputWhite","Output white",&core::LevelsChannel::outputWhite);
    explanation(levels.layout,QStringLiteral("Composite is evaluated first, then R/G/B. Input black/white cannot cross; coincident points form a defined threshold, never a divide-by-zero."));

    auto& curves=pages_[index(Type::Curves)];
    curvesChannel_=choices(curves,"AdjustmentCurvesChannel",channels,[this](int){refresh();updateHistogramDisplay();});
    curve_=new AdjustmentCurveEditor;
    curves.layout->addWidget(curve_);
    explanation(curves.layout,QStringLiteral("Left-click: Add · Drag: Move · Right-click / {{RemovePointAction}}: Delete"));
    histogramLabels_[1]=explanation(curves.layout,QStringLiteral("Input histogram · alpha-weighted sRGB"));
    curve_->evaluate=[this](double input){return core::evaluateCurve(std::get<core::CurvesParameters>(item(Type::Curves).parameters).channels[std::size_t(curvesChannel_->currentIndex())],float(input));};
    curve_->onInteractionStarted=[this]{return begin(Type::Curves);};
    curve_->onPointsChanged=[this](const std::vector<core::Vec2d>& points){
        if(updating_)return;
        auto& channel=std::get<core::CurvesParameters>(item(Type::Curves).parameters).channels[std::size_t(curvesChannel_->currentIndex())];
        channel.points.clear();for(const auto p:points)channel.points.push_back({p.x,p.y});
        item(Type::Curves).enabled=true;
        automaticallyEnabled_=true;
        if(onPreview)onPreview(std::make_shared<const core::AdjustmentStack>(working_));
        refreshCurvePoint();
    };
    curve_->onInteractionFinished=[this](bool commit){finishEditing(commit);};
    curve_->onPointSelected=[this](int){refreshCurvePoint();};
    const auto pointNumber=[this,&curves](const char* name,const char* label,bool input){
        return number(curves,name,QString::fromLatin1(label),0,255,1,
            [this,input]{const auto& p=curve_->points()[std::size_t(curve_->selectedIndex())];return (input?p.x:p.y)*255;},
            [this,input](double value){auto p=curve_->points()[std::size_t(curve_->selectedIndex())];(input?p.x:p.y)=value/255;curve_->setSelectedPoint(p);});
    };
    curveInput_=pointNumber("AdjustmentCurveInput","Input",true);
    curveOutput_=pointNumber("AdjustmentCurveOutput","Output",false);
    removePoint_=new QPushButton(QStringLiteral("Remove point"));
    removePoint_->setObjectName(QStringLiteral("AdjustmentCurveRemove"));curves.layout->addWidget(removePoint_);
    connect(removePoint_,&QPushButton::clicked,curve_,&AdjustmentCurveEditor::removeSelectedPoint);

    auto& hue=pages_[index(Type::HueSaturation)];
    const QStringList hues{QStringLiteral("Master"),QStringLiteral("Reds"),QStringLiteral("Yellows"),QStringLiteral("Greens"),QStringLiteral("Cyans"),QStringLiteral("Blues"),QStringLiteral("Magentas")};
    hueRange_=choices(hue,"AdjustmentHueRange",hues,[this](int){refresh();});
    using HueField=double core::HueRangeParameters::*;
    const auto hueNumber=[this,&hue](const char* name,const char* label,HueField field,bool degrees){
        number(hue,name,QString::fromLatin1(label),degrees?-180:-100,degrees?180:100,0,
            [this,field,degrees]{return (std::get<core::HueSaturationParameters>(item(Type::HueSaturation).parameters).ranges[std::size_t(hueRange_->currentIndex())].*field)*(degrees?1:100);},
            [this,field,degrees](double v){std::get<core::HueSaturationParameters>(item(Type::HueSaturation).parameters).ranges[std::size_t(hueRange_->currentIndex())].*field=v/(degrees?1:100);},degrees?QStringLiteral("°"):QString());
    };
    hueNumber("AdjustmentHue","Hue",&core::HueRangeParameters::hue,true);
    hueNumber("AdjustmentSaturation","Saturation",&core::HueRangeParameters::saturation,false);
    hueNumber("AdjustmentLightness","Lightness",&core::HueRangeParameters::lightness,false);
    colorize_=new QCheckBox(QStringLiteral("Colorize"));colorize_->setObjectName(QStringLiteral("AdjustmentColorize"));hue.layout->addWidget(colorize_);
    connect(colorize_,&QCheckBox::toggled,this,[this](bool value){if(!updating_)change(Type::HueSaturation,[value](auto& a){std::get<core::HueSaturationParameters>(a.parameters).colorize=value;});});
    number(hue,"AdjustmentColorizeHue","Colorize hue",0,360,0,
        [this]{return std::get<core::HueSaturationParameters>(item(Type::HueSaturation).parameters).colorizeHue;},
        [this](double v){std::get<core::HueSaturationParameters>(item(Type::HueSaturation).parameters).colorizeHue=v;},QStringLiteral("°"));
    number(hue,"AdjustmentColorizeSaturation","Colorize saturation",0,100,0,
        [this]{return std::get<core::HueSaturationParameters>(item(Type::HueSaturation).parameters).colorizeSaturation*100;},
        [this](double v){std::get<core::HueSaturationParameters>(item(Type::HueSaturation).parameters).colorizeSaturation=v/100;},QStringLiteral("%"));
    explanation(hue.layout,QStringLiteral("Master and six overlapping smooth hue ranges remain active together. Changing the displayed range is navigation only."));

    auto& vibrance=pages_[index(Type::Vibrance)];
    number(vibrance,"AdjustmentVibrance","Vibrance",-100,100,0,
        [this]{return std::get<core::VibranceParameters>(item(Type::Vibrance).parameters).amount*100;},
        [this](double v){std::get<core::VibranceParameters>(item(Type::Vibrance).parameters).amount=v/100;});
    explanation(vibrance.layout,QStringLiteral("Saturation-aware enhancement: muted colors respond more than saturated colors. Neutral gray is preserved; no skin-tone detection is implied."));

    auto& balance=pages_[index(Type::ColorBalance)];
    balanceRange_=choices(balance,"AdjustmentBalanceRange",{QStringLiteral("Shadows"),QStringLiteral("Midtones"),QStringLiteral("Highlights")},[this](int){refresh();});
    const std::array<QString,3> balanceNames{QStringLiteral("Cyan / Red"),QStringLiteral("Magenta / Green"),QStringLiteral("Yellow / Blue")};
    for(std::size_t c=0;c<3;++c) {
        const auto name=QStringLiteral("AdjustmentBalance%1").arg(c).toLatin1();
        number(balance,name.constData(),balanceNames[c],-100,100,0,
            [this,c]{return std::get<core::ColorBalanceParameters>(item(Type::ColorBalance).parameters).tones[std::size_t(balanceRange_->currentIndex())][c]*100;},
            [this,c](double v){std::get<core::ColorBalanceParameters>(item(Type::ColorBalance).parameters).tones[std::size_t(balanceRange_->currentIndex())][c]=v/100;});
    }
    preserveLuminosity_=new QCheckBox(QStringLiteral("Preserve luminosity"));preserveLuminosity_->setObjectName(QStringLiteral("AdjustmentPreserveLuminosity"));balance.layout->addWidget(preserveLuminosity_);
    connect(preserveLuminosity_,&QCheckBox::toggled,this,[this](bool value){if(!updating_)change(Type::ColorBalance,[value](auto& a){std::get<core::ColorBalanceParameters>(a.parameters).preserveLuminosity=value;});});
    explanation(balance.layout,QStringLiteral("Smooth tonal weighting combines all three ranges. Preserve luminosity retains the defined sRGB luminosity while correcting color."));

    auto& warmth=pages_[index(Type::WarmthTint)];
    number(warmth,"AdjustmentWarmth","Warmth",-100,100,0,
        [this]{return std::get<core::WarmthTintParameters>(item(Type::WarmthTint).parameters).warmth*100;},
        [this](double v){std::get<core::WarmthTintParameters>(item(Type::WarmthTint).parameters).warmth=v/100;});
    number(warmth,"AdjustmentTint","Tint",-100,100,0,
        [this]{return std::get<core::WarmthTintParameters>(item(Type::WarmthTint).parameters).tint*100;},
        [this](double v){std::get<core::WarmthTintParameters>(item(Type::WarmthTint).parameters).tint=v/100;});
    explanation(warmth.layout,QStringLiteral("Relative warm/cool and green/magenta correction for rendered sRGB. Zero is neutral; these are not RAW-camera Kelvin values."));

    auto& mono=pages_[index(Type::BlackWhite)];
    for(std::size_t c=0;c<6;++c){
        const auto name=QStringLiteral("AdjustmentMono%1").arg(c).toLatin1();
        number(mono,name.constData(),hues[qsizetype(c+1)],-100,100,0,
            [this,c]{return std::get<core::BlackWhiteParameters>(item(Type::BlackWhite).parameters).contributions[c]*100;},
            [this,c](double v){std::get<core::BlackWhiteParameters>(item(Type::BlackWhite).parameters).contributions[c]=v/100;});
    }
    tint_=new QPushButton; tint_->setObjectName(QStringLiteral("AdjustmentMonoTintColor"));mono.layout->addWidget(tint_);
    connect(tint_,&QPushButton::clicked,this,&AdjustmentsPanel::chooseTint);
    number(mono,"AdjustmentMonoTintStrength","Tint strength",0,100,0,
        [this]{return std::get<core::BlackWhiteParameters>(item(Type::BlackWhite).parameters).tintStrength*100;},
        [this](double v){std::get<core::BlackWhiteParameters>(item(Type::BlackWhite).parameters).tintStrength=v/100;},QStringLiteral("%"));
    explanation(mono.layout,QStringLiteral("Hue-weighted luminance offsets preserve neutral-gray stability. Tint color is independent of the global colors; source alpha is unchanged."));
    explanation(pages_[index(Type::Invert)].layout,QStringLiteral("Invert unassociated sRGB color channels while keeping alpha unchanged. Enable to apply; disabled by default."));

    for(auto& page:pages_)page.layout->addStretch(1);
    filtersPanel_=new FiltersPanel;
    filtersPanel_->setEmbedded();
    tabs_->addTab(filtersPanel_,tr("Filters"));
    effectsPanel_=new EffectsPanel;tabs_->addTab(effectsPanel_,tr("Effects"));
    connect(tabs_,&QTabWidget::currentChanged,this,[this](int){
        finishEditing();filtersPanel_->finishEditing();effectsPanel_->finishEditing();
        compare_->setDown(false);
        if(onComparison)onComparison(false);
        if(filtersPanel_->onComparison)filtersPanel_->onComparison(false);
        if(onFiltersCategoryChanged)onFiltersCategoryChanged(filtersCategoryActive());
        if(effectsPanel_->onComparison)effectsPanel_->onComparison(false);
        navigate();
    });
    histogramTimer_=new QTimer(this);histogramTimer_->setSingleShot(true);
    connect(histogramTimer_,&QTimer::timeout,this,&AdjustmentsPanel::advanceHistogram);
    setTarget(nullptr,false);
}
AdjustmentsPanel::~AdjustmentsPanel() = default;
bool AdjustmentsPanel::effectsCategoryActive() const{return effectsPanel_&&tabs_->currentWidget()==effectsPanel_;}
bool AdjustmentsPanel::filtersCategoryActive() const
{
    return filtersPanel_&&tabs_->currentWidget()==filtersPanel_;
}
void AdjustmentsPanel::showFilter(core::SpatialFilterType type)
{
    if(!tabs_->isTabEnabled(tabs_->indexOf(filtersPanel_)))return;
    tabs_->setCurrentWidget(filtersPanel_);filtersPanel_->selectType(type);
    tabs_->tabBar()->setFocus(Qt::OtherFocusReason);
}
core::Adjustment& AdjustmentsPanel::item(Type type) { return working_.items[index(type)]; }
const core::Adjustment& AdjustmentsPanel::item(Type type) const { return working_.items[index(type)]; }
AdjustmentsPanel::Page& AdjustmentsPanel::makePage(Type type,QStackedWidget* stack)
{
    auto& page=pages_[index(type)];
    page.widget=new QWidget;page.widget->setObjectName(QStringLiteral("AdjustmentPage%1").arg(int(type)));
    page.layout=new QVBoxLayout(page.widget);page.layout->setContentsMargins(0,0,0,8);page.layout->setSpacing(8);
    page.enabled=new QCheckBox(QStringLiteral("Enabled"));page.enabled->setObjectName(QStringLiteral("AdjustmentEnabled%1").arg(int(type)));
    auto* reset=new QPushButton(QStringLiteral("Reset"));reset->setObjectName(QStringLiteral("AdjustmentReset%1").arg(int(type)));
    connect(page.enabled,&QCheckBox::toggled,this,[this,type](bool enabled){if(!updating_)change(type,[enabled](auto& a){a.enabled=enabled;},false);});
    connect(reset,&QPushButton::clicked,this,[this,type]{change(type,[type](auto& a){a=core::defaultAdjustment(type);},false);});
    page.scope=new QComboBox;page.scope->setObjectName(QStringLiteral("AdjustmentScope%1").arg(int(type)));
    page.scope->addItems({QStringLiteral("Whole Layer"),QStringLiteral("Captured Selection")});
    auto* scopeRow=new QHBoxLayout;scopeRow->setSpacing(6);
    page.scope->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);
    scopeRow->addWidget(page.scope,1);
    page.capture=new QPushButton(QStringLiteral("Capture"));
    page.capture->setObjectName(QStringLiteral("AdjustmentCapture%1").arg(int(type)));
    page.capture->setToolTip(QStringLiteral("Own a snapshot of the current selection in this layer's local coordinates. Later selection changes do not alter it."));
    scopeRow->addWidget(page.capture);scopeRow->addWidget(reset);page.layout->addLayout(scopeRow);
    page.maskStatus=explanation(page.layout,QString());
    page.showRegion=new QCheckBox(QStringLiteral("Show captured region"));
    page.showRegion->setObjectName(QStringLiteral("AdjustmentShowRegion%1").arg(int(type)));
    page.showRegion->setToolTip(QStringLiteral("Show a static accent outline of this adjustment's captured coverage. It follows the layer, not the live selection; this changes only the view."));
    page.layout->addWidget(page.showRegion);
    connect(page.showRegion,&QCheckBox::toggled,this,[this](bool visible){
        if(updating_)return;
        showCapturedRegion_=visible;refresh();
    });
    connect(page.scope,&QComboBox::activated,this,[this,type](int selected){
        finishEditing();
        if(selected==0)change(type,[](auto& a){a.mask.reset();},false);
        else if(!item(type).mask&&hasSelection_&&onCaptureSelection)onCaptureSelection(type);
        refresh();
    });
    connect(page.capture,&QPushButton::clicked,this,[this,type]{finishEditing();if(onCaptureSelection)onCaptureSelection(type);});
    stack->addWidget(page.widget);return page;
}
CompactValueControl* AdjustmentsPanel::number(Page& page,const char* name,const QString& label,
    double min,double max,int decimals,std::function<double()> read,std::function<void(double)> write,const QString& suffix)
{
    auto* control=new CompactValueControl;
    control->setObjectName(QString::fromLatin1(name));control->setPrefix(label+QStringLiteral(": "));control->setSuffix(suffix);
    control->setDecimals(decimals);control->setRange(min,max);control->setSingleStep(decimals>0?.1:1);
    control->setFixedHeight(30);control->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);
    control->setAccessibleName(label);page.layout->addWidget(control);page.numbers.push_back(control);
    const auto type=Type(&page-pages_.data());
    const auto previous=page.refresh;
    page.refresh=[previous,control,read]{if(previous)previous();if(!control->interactionActive()) {const QSignalBlocker guard(control);control->setValue(read());}};
    control->onInteractionStarted=[this,type]{if(!updating_)begin(type);};
    control->onInteractionFinished=[this]{if(!updating_)finishEditing();};
    connect(control,&QDoubleSpinBox::valueChanged,this,[this,type,write,control](double value){
        if(updating_||!target_)return;
        if(!begin(type)){refresh();return;}
        write(value);item(type).enabled=true;
        automaticallyEnabled_=true;
        if(onPreview)onPreview(std::make_shared<const core::AdjustmentStack>(working_));
        if(!control->interactionActive())finishEditing();
    });
    control->installEventFilter(this);for(auto* child:control->findChildren<QLineEdit*>())child->installEventFilter(this);
    return control;
}
QComboBox* AdjustmentsPanel::choices(Page& page,const char* name,const QStringList& labels,std::function<void(int)> callback)
{
    auto* combo=new QComboBox;combo->setObjectName(QString::fromLatin1(name));combo->addItems(labels);
    combo->setMaxVisibleItems(int(labels.size()));page.layout->addWidget(combo);
    connect(combo,&QComboBox::currentIndexChanged,this,[this,callback](int value){if(!updating_){finishEditing();callback(value);}});
    return combo;
}
bool AdjustmentsPanel::begin(Type type)
{
    if(updating_||finishing_||!target_)return false;
    if(editing_&&editingType_==type)return true;
    if(editing_)finishEditing();
    if(onInteractionStarted&&!onInteractionStarted(type))return false;
    editingBefore_=working_;automaticallyEnabled_=false;
    editing_=true;editingType_=type;return true;
}
void AdjustmentsPanel::change(Type type,const std::function<void(core::Adjustment&)>& apply,bool autoEnable)
{
    if(updating_)return;
    finishEditing();if(!begin(type))return;apply(item(type));if(autoEnable)item(type).enabled=true;
    if(onPreview)onPreview(std::make_shared<const core::AdjustmentStack>(working_));
    finishEditing();refresh();
}
void AdjustmentsPanel::finishEditing(bool commit)
{
    if(finishing_)return;
    const QScopedValueRollback guard(finishing_,true);
    // Clear edit ownership before ending Qt text mode; focus-out callbacks
    // cannot create a second history boundary or resurrect a cancelled edit.
    const bool hadEdit=editing_;editing_=false;
    if(hadEdit&&commit&&automaticallyEnabled_) {
        auto candidate=working_;
        candidate.items[index(editingType_)].enabled=editingBefore_.items[index(editingType_)].enabled;
        if(core::equivalentAdjustments(std::make_shared<const core::AdjustmentStack>(candidate),
                std::make_shared<const core::AdjustmentStack>(editingBefore_))) {
            working_=std::move(candidate);
            if(onPreview)onPreview(std::make_shared<const core::AdjustmentStack>(working_));
        }
    }
    if(curve_&&curve_->interactionActive())curve_->finishInteraction(commit);
    // Finishing a gesture is a history boundary, not a keyboard-focus change.
    // Keep arrows on the last clicked slider and point commands on the graph.
    for(auto& page:pages_)for(auto* control:page.numbers)control->finishEditing(commit,true);
    if(hadEdit&&onInteractionFinished)onInteractionFinished(commit);
}
void AdjustmentsPanel::setTarget(const core::Layer* layer,bool hasSelection,const core::Document* document,std::uint64_t instance)
{
    const auto next=layer?std::optional(layer->id):std::nullopt;
    if(target_!=next || document_!=document || documentInstance_!=instance){finishEditing(false);if(onComparison)onComparison(false);histogramJob_.reset();histogramTimer_->stop();histogramCaches_={};}
    document_=document;documentInstance_=instance;
    target_=next;hasSelection_=hasSelection;
    working_=layer&&layer->adjustments?*layer->adjustments:core::AdjustmentStack{};
    targetLabel_->setText(layer?QStringLiteral("%1 · primary layer").arg(QString::fromStdString(layer->name)):QStringLiteral("Select a raster, text, or shape layer"));
    targetLabel_->setToolTip(layer?QString::fromStdString(layer->name):QString());
    const auto* adjustment=layer?std::get_if<core::AdjustmentLayer>(&layer->payload):nullptr;
    const auto placement=document&&layer?document->tree().placement(layer->id):std::nullopt;
    scopeLabel_->setVisible(adjustment);
    if(adjustment)scopeLabel_->setText(adjustment->scope==core::AdjustmentScope::ThisGroup&&placement&&placement->parent
        ?tr("This Group · lower content, including nested groups. This group composites locally, even while the correction is bypassed.")
        :tr("All Below · accumulated content in the current compositing domain."));
    filtersPanel_->setTarget(adjustment?nullptr:layer,hasSelection);
    effectsPanel_->setTarget(adjustment?nullptr:layer);
    tabs_->setTabEnabled(3,!adjustment);tabs_->setTabEnabled(4,!adjustment);
    if(adjustment && tabs_->currentIndex()>2)tabs_->setCurrentIndex(0);
    tabs_->setEnabled(layer);resetAll_->setEnabled(layer);compare_->setEnabled(layer);refresh();
}
Type AdjustmentsPanel::currentType() const
{
    const auto tab=std::clamp(tabs_->currentIndex(),0,2);
    return Type(navigation_[std::size_t(tab)]->currentData().toInt());
}
void AdjustmentsPanel::refresh()
{
    const QScopedValueRollback guard(updating_,true);
    for(std::size_t i=0;i<pages_.size();++i){
        auto& page=pages_[i];const auto& adjustment=working_.items[i];
        page.enabled->setChecked(adjustment.enabled);page.scope->setCurrentIndex(adjustment.mask?1:0);
        page.capture->setEnabled(target_&&hasSelection_);
        page.maskStatus->setText(adjustment.mask
            ? (adjustment.mask->coverage->bounds().empty()?QStringLiteral("Captured empty mask · affects no pixels"):QStringLiteral("Owned mask · follows this layer"))
            :QStringLiteral("All source pixels · selection is not implicit"));
        page.maskStatus->setVisible(adjustment.mask.has_value());
        page.showRegion->setVisible(adjustment.mask.has_value());
        page.showRegion->setChecked(showCapturedRegion_);
        page.showRegion->setEnabled(adjustment.mask && !adjustment.mask->coverage->bounds().empty());
        page.scope->setToolTip(adjustment.mask?QStringLiteral("This owned selection mask follows the layer, independent of later selection changes.")
            :QStringLiteral("Whole layer: all source pixels. Capture explicitly to use the current selection."));
    }
    if(curve_){
        std::vector<core::Vec2d> points;for(const auto& p:std::get<core::CurvesParameters>(item(Type::Curves).parameters).channels[std::size_t(curvesChannel_->currentIndex())].points)points.push_back({p.input,p.output});
        curve_->setPoints(points);
    }
    for(auto& page:pages_)if(page.refresh)page.refresh();
    if(colorize_)colorize_->setChecked(std::get<core::HueSaturationParameters>(item(Type::HueSaturation).parameters).colorize);
    if(preserveLuminosity_)preserveLuminosity_->setChecked(std::get<core::ColorBalanceParameters>(item(Type::ColorBalance).parameters).preserveLuminosity);
    if(tint_){
        const auto color=std::get<core::BlackWhiteParameters>(item(Type::BlackWhite).parameters).tintColor;
        const QColor qt(color.red,color.green,color.blue);
        tint_->setText(QStringLiteral("Tint color  %1").arg(qt.name(QColor::HexRgb).toUpper()));
        const auto dpr=tint_->devicePixelRatioF();QPixmap swatch(QSize(24,16)*dpr);swatch.setDevicePixelRatio(dpr);swatch.fill(qt);tint_->setIcon(QIcon(swatch));
    }
    refreshCurvePoint();
    if(onCapturedRegionChanged)onCapturedRegionChanged();
}
void AdjustmentsPanel::refreshCurvePoint()
{
    if(!curveInput_||!curveOutput_)return;
    const QScopedValueRollback guard(updating_,true);
    const auto selected=curve_->selectedIndex();
    const auto p=curve_->points()[std::size_t(selected)];
    if(!curveInput_->interactionActive())curveInput_->setValue(p.x*255);
    if(!curveOutput_->interactionActive())curveOutput_->setValue(p.y*255);
    const bool interior=selected>0&&selected+1<int(curve_->points().size());
    curveInput_->setEnabled(interior);removePoint_->setEnabled(interior);
}
void AdjustmentsPanel::navigate()
{
    resetAll_->setText(effectsCategoryActive()?tr("Reset Effects"):tr("Reset All"));
    if(effectsCategoryActive()) {
        compare_->setToolTip(tr("Temporarily bypass only this layer's effects on canvas. Adjustments and filters remain applied."));
        resetAll_->setToolTip(tr("Disable and reset all seven effects. Adjustments and filters are unchanged."));
        refresh();return;
    }
    compare_->setToolTip(filtersCategoryActive()
        ? tr("Temporarily bypass this layer's spatial filters on canvas only. Color adjustments remain applied. Release to return to After.")
        : tr("Temporarily bypass this layer's color adjustments on canvas only. Release to return to After."));
    resetAll_->setToolTip(filtersCategoryActive()
        ? tr("Reset all spatial filters and their captured masks. Color adjustments are unchanged.")
        : tr("Reset all Tone, Color and Monochrome adjustments. Spatial filters are unchanged."));
    refresh();if(onHistogramRequested)onHistogramRequested();
}
void AdjustmentsPanel::chooseTint()
{
    finishEditing();if(!target_)return;
    const auto target=target_;
    const auto color=std::get<core::BlackWhiteParameters>(item(Type::BlackWhite).parameters).tintColor;
    auto* owner=window();while(owner->parentWidget())owner=owner->parentWidget()->window();
    auto* dialog=new QColorDialog(QColor(color.red,color.green,color.blue),owner);
    dialog->setObjectName(QStringLiteral("AdjustmentTintDialog"));dialog->setWindowTitle(QStringLiteral("Black & White tint"));
    dialog->setOptions(QColorDialog::DontUseNativeDialog);dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->setWindowModality(Qt::ApplicationModal);
    (void)owner->winId();(void)dialog->winId();dialog->windowHandle()->setTransientParent(owner->windowHandle());
    connect(dialog,&QColorDialog::colorSelected,this,[this,target](QColor color){
        if(target_!=target)return;
        change(Type::BlackWhite,[color](auto& a){std::get<core::BlackWhiteParameters>(a.parameters).tintColor={std::uint8_t(color.red()),std::uint8_t(color.green()),std::uint8_t(color.blue()),255};});
    });dialog->show();
}
bool AdjustmentsPanel::eventFilter(QObject* watched,QEvent* event)
{
    if(watched==compare_&&(event->type()==QEvent::TouchCancel||event->type()==QEvent::Hide||event->type()==QEvent::WindowDeactivate)){
        compare_->setDown(false);if(onComparison)onComparison(false);
        if(filtersPanel_&&filtersPanel_->onComparison)filtersPanel_->onComparison(false);
        if(effectsPanel_&&effectsPanel_->onComparison)effectsPanel_->onComparison(false);
    }
    if(editing_&&event->type()==QEvent::KeyPress&&static_cast<QKeyEvent*>(event)->key()==Qt::Key_Escape){finishEditing(false);event->accept();return true;}
    if(editing_&&event->type()==QEvent::TouchCancel)finishEditing(false);
    return QWidget::eventFilter(watched,event);
}
void AdjustmentsPanel::hideEvent(QHideEvent* event)
{
    finishEditing();if(onComparison)onComparison(false);histogramTimer_->stop();histogramJob_.reset();QWidget::hideEvent(event);
    if(onCapturedRegionChanged)onCapturedRegionChanged();
}
void AdjustmentsPanel::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    if(onCapturedRegionChanged)onCapturedRegionChanged();
    QTimer::singleShot(0,this,[this]{if(onHistogramRequested)onHistogramRequested();});
}
} // namespace imageeditor::ui
