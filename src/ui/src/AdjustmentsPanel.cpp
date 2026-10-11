#include "imageeditor/ui/LocalizedNames.hpp"
#include "imageeditor/ui/Localization.hpp"
#include "imageeditor/ui/AdjustmentsPanel.hpp"
#include "imageeditor/ui/EffectsPanel.hpp"
#include "imageeditor/ui/CompactValueControl.hpp"
#include "imageeditor/ui/FiltersPanel.hpp"
#include "imageeditor/ui/Theme.hpp"
#include <QApplication>
#include <QCheckBox>
#include "imageeditor/ui/ColorDialog.hpp"
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
#include "imageeditor/ui/CurrentPageStack.hpp"
#include <algorithm>

namespace imageeditor::ui {
namespace {
using Type = core::AdjustmentType;
constexpr std::size_t index(Type type) { return static_cast<std::size_t>(type); }
i18n::Message displayName(Type type) { return i18n::adjustmentName(type); }
QLabel* explanation(QVBoxLayout* layout, const i18n::Message& text)
{
    auto* label = i18n::make<QLabel>(text);
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
    compare_ = i18n::make<QPushButton>(i18n::text("adjustments.panel.hold.for.before"));
    compare_->setObjectName(QStringLiteral("AdjustmentCompare"));
    i18n::setToolTip(compare_, i18n::text("adjustments.panel.temporarily.bypass.this.layer.s.adjustments.on.the.canvas"));
    resetAll_ = i18n::make<QPushButton>(i18n::text("adjustments.panel.reset.all"));
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
    const std::array<i18n::Message,3> groupNames {i18n::text("adjustments.panel.tone"),i18n::text("adjustments.panel.color"),i18n::text("adjustments.panel.monochrome")};
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
        for(const auto type:groups[i])i18n::addItem(navigation_[i], displayName(type),int(type));
        navigation_[i]->setMaxVisibleItems(int(groups[i].size()));
        modeRow->addWidget(navigation_[i],1);
        auto* enabledStack=new QStackedWidget;
        enabledStack->setObjectName(QStringLiteral("AdjustmentEnabledStack%1").arg(i));
        enabledStack->setSizePolicy(QSizePolicy::Fixed,QSizePolicy::Fixed);
        modeRow->addWidget(enabledStack,0,Qt::AlignVCenter);layout->addLayout(modeRow);
        auto* scroll=new QScrollArea;
        scroll->setFrameShape(QFrame::NoFrame);scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        stacks_[i]=new CurrentPageStack;
        stacks_[i]->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);
        scroll->setWidget(stacks_[i]);layout->addWidget(scroll,1);
        for(const auto type:groups[i])enabledStack->addWidget(makePage(type,stacks_[i]).enabled);
        i18n::addTab(tabs_, tab,groupNames[i]);
        connect(navigation_[i],&QComboBox::currentIndexChanged,this,[this,i,enabledStack](int row){
            finishEditing();stacks_[i]->setCurrentIndex(row);enabledStack->setCurrentIndex(row);navigate();
        });
    }
    auto& exposure=pages_[index(Type::Exposure)];
    number(exposure, "AdjustmentExposure", i18n::text("adjustments.panel.exposure"), -20, 20, 2, [this]{return std::get<core::ExposureParameters>(item(Type::Exposure).parameters).stops;}, [this](double v){std::get<core::ExposureParameters>(item(Type::Exposure).parameters).stops=v;}, QStringLiteral(" EV"));
    explanation(exposure.layout, i18n::text("adjustments.panel.linear.light.exposure.in.stops.1.ev.doubles.light"));

    auto& brightness=pages_[index(Type::BrightnessContrast)];
    number(brightness, "AdjustmentBrightness", i18n::text("adjustments.panel.brightness"), -100, 100, 0, [this]{return std::get<core::BrightnessContrastParameters>(item(Type::BrightnessContrast).parameters).brightness*100;}, [this](double v){std::get<core::BrightnessContrastParameters>(item(Type::BrightnessContrast).parameters).brightness=v/100;});
    number(brightness, "AdjustmentContrast", i18n::text("adjustments.panel.contrast"), -100, 100, 0, [this]{return std::get<core::BrightnessContrastParameters>(item(Type::BrightnessContrast).parameters).contrast*100;}, [this](double v){std::get<core::BrightnessContrastParameters>(item(Type::BrightnessContrast).parameters).contrast=v/100;});
    explanation(brightness.layout, i18n::text("adjustments.panel.brightness.and.contrast.are.independent.srgb.tone.controls.contrast"));

    auto& levels=pages_[index(Type::Levels)];
    const QList<i18n::Message> channels {i18n::text("adjustments.panel.composite.rgb"),i18n::text("adjustments.panel.red"),i18n::text("adjustments.panel.green"),i18n::text("adjustments.panel.blue")};
    levelsChannel_=choices(levels,"AdjustmentLevelsChannel",channels,[this](int){refresh();updateHistogramDisplay();});
    levelsHistogram_=new AdjustmentCurveEditor;
    levelsHistogram_->setObjectName(QStringLiteral("AdjustmentLevelsHistogram"));levelsHistogram_->setHistogramOnly(true);
    levels.layout->addWidget(levelsHistogram_);
    histogramLabels_[0]=explanation(levels.layout, i18n::text("adjustments.panel.input.histogram.alpha.weighted.srgb"));
    using LevelField=double core::LevelsChannel::*;
    const auto levelNumber=[this,&levels](const char* name,const i18n::Message& label,LevelField field,bool gamma=false){
        number(levels,name,label,gamma?.1:0,gamma?10:255,gamma?2:1,
            [this,field,gamma]{const auto& c=std::get<core::LevelsParameters>(item(Type::Levels).parameters).channels[std::size_t(levelsChannel_->currentIndex())];return (c.*field)*(gamma?1:255);},
            [this,field,gamma](double v){auto& c=std::get<core::LevelsParameters>(item(Type::Levels).parameters).channels[std::size_t(levelsChannel_->currentIndex())];
                auto value=v/(gamma?1:255);
                if(field==&core::LevelsChannel::inputBlack)value=std::min(value,c.inputWhite);
                if(field==&core::LevelsChannel::inputWhite)value=std::max(value,c.inputBlack);
                c.*field=value;});
    };
    levelNumber("AdjustmentLevelsInputBlack", i18n::text("adjustments.panel.input.black"), &core::LevelsChannel::inputBlack);
    levelNumber("AdjustmentLevelsGamma", i18n::text("adjustments.panel.gamma"), &core::LevelsChannel::gamma, true);
    levelNumber("AdjustmentLevelsInputWhite", i18n::text("adjustments.panel.input.white"), &core::LevelsChannel::inputWhite);
    levelNumber("AdjustmentLevelsOutputBlack", i18n::text("adjustments.panel.output.black"), &core::LevelsChannel::outputBlack);
    levelNumber("AdjustmentLevelsOutputWhite", i18n::text("adjustments.panel.output.white"), &core::LevelsChannel::outputWhite);
    explanation(levels.layout, i18n::text("adjustments.panel.composite.is.evaluated.first.then.r.g.b.input"));

    auto& curves=pages_[index(Type::Curves)];
    curvesChannel_=choices(curves,"AdjustmentCurvesChannel",channels,[this](int){refresh();updateHistogramDisplay();});
    curve_=new AdjustmentCurveEditor;
    curves.layout->addWidget(curve_);
    explanation(curves.layout, i18n::text("adjustments.panel.left.click.add.drag.move.right.click.removepointaction.delete"));
    histogramLabels_[1]=explanation(curves.layout, i18n::text("adjustments.panel.input.histogram.alpha.weighted.srgb"));
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
    const auto pointNumber=[this,&curves](const char* name,const i18n::Message& label,bool input){
        return number(curves,name,label,0,255,1,
            [this,input]{const auto& p=curve_->points()[std::size_t(curve_->selectedIndex())];return (input?p.x:p.y)*255;},
            [this,input](double value){auto p=curve_->points()[std::size_t(curve_->selectedIndex())];(input?p.x:p.y)=value/255;curve_->setSelectedPoint(p);});
    };
    curveInput_=pointNumber("AdjustmentCurveInput", i18n::text("adjustments.panel.input"), true);
    curveOutput_=pointNumber("AdjustmentCurveOutput", i18n::text("adjustments.panel.output"), false);
    removePoint_=i18n::make<QPushButton>(i18n::text("adjustments.panel.remove.point"));
    removePoint_->setObjectName(QStringLiteral("AdjustmentCurveRemove"));curves.layout->addWidget(removePoint_);
    connect(removePoint_,&QPushButton::clicked,curve_,&AdjustmentCurveEditor::removeSelectedPoint);

    auto& hue=pages_[index(Type::HueSaturation)];
    const QList<i18n::Message> hues{i18n::text("adjustments.panel.master"),i18n::text("adjustments.panel.reds"),i18n::text("adjustments.panel.yellows"),i18n::text("adjustments.panel.greens"),i18n::text("adjustments.panel.cyans"),i18n::text("adjustments.panel.blues"),i18n::text("adjustments.panel.magentas")};
    hueRange_=choices(hue,"AdjustmentHueRange",hues,[this](int){refresh();});
    using HueField=double core::HueRangeParameters::*;
    const auto hueNumber=[this,&hue](const char* name,const i18n::Message& label,HueField field,bool degrees){
        number(hue,name,label,degrees?-180:-100,degrees?180:100,0,
            [this,field,degrees]{return (std::get<core::HueSaturationParameters>(item(Type::HueSaturation).parameters).ranges[std::size_t(hueRange_->currentIndex())].*field)*(degrees?1:100);},
            [this,field,degrees](double v){std::get<core::HueSaturationParameters>(item(Type::HueSaturation).parameters).ranges[std::size_t(hueRange_->currentIndex())].*field=v/(degrees?1:100);},degrees?QStringLiteral("°"):QString());
    };
    hueNumber("AdjustmentHue", i18n::text("adjustments.panel.hue"), &core::HueRangeParameters::hue, true);
    hueNumber("AdjustmentSaturation", i18n::text("adjustments.panel.saturation"), &core::HueRangeParameters::saturation, false);
    hueNumber("AdjustmentLightness", i18n::text("adjustments.panel.lightness"), &core::HueRangeParameters::lightness, false);
    colorize_=i18n::make<QCheckBox>(i18n::text("adjustments.panel.colorize"));colorize_->setObjectName(QStringLiteral("AdjustmentColorize"));hue.layout->addWidget(colorize_);
    connect(colorize_,&QCheckBox::toggled,this,[this](bool value){if(!updating_)change(Type::HueSaturation,[value](auto& a){std::get<core::HueSaturationParameters>(a.parameters).colorize=value;});});
    number(hue, "AdjustmentColorizeHue", i18n::text("adjustments.panel.colorize.hue"), 0, 360, 0, [this]{return std::get<core::HueSaturationParameters>(item(Type::HueSaturation).parameters).colorizeHue;}, [this](double v){std::get<core::HueSaturationParameters>(item(Type::HueSaturation).parameters).colorizeHue=v;}, QStringLiteral("°"));
    number(hue, "AdjustmentColorizeSaturation", i18n::text("adjustments.panel.colorize.saturation"), 0, 100, 0, [this]{return std::get<core::HueSaturationParameters>(item(Type::HueSaturation).parameters).colorizeSaturation*100;}, [this](double v){std::get<core::HueSaturationParameters>(item(Type::HueSaturation).parameters).colorizeSaturation=v/100;}, QStringLiteral("%"));
    explanation(hue.layout, i18n::text("adjustments.panel.master.and.six.overlapping.smooth.hue.ranges.remain.active"));

    auto& vibrance=pages_[index(Type::Vibrance)];
    number(vibrance, "AdjustmentVibrance", i18n::text("adjustments.panel.vibrance"), -100, 100, 0, [this]{return std::get<core::VibranceParameters>(item(Type::Vibrance).parameters).amount*100;}, [this](double v){std::get<core::VibranceParameters>(item(Type::Vibrance).parameters).amount=v/100;});
    explanation(vibrance.layout, i18n::text("adjustments.panel.saturation.aware.enhancement.muted.colors.respond.more.than.saturated"));

    auto& balance=pages_[index(Type::ColorBalance)];
    balanceRange_=choices(balance,"AdjustmentBalanceRange",{QStringLiteral("Shadows"),QStringLiteral("Midtones"),QStringLiteral("Highlights")},[this](int){refresh();});
    const std::array<QString,3> balanceNames{QStringLiteral("Cyan / Red"),QStringLiteral("Magenta / Green"),QStringLiteral("Yellow / Blue")};
    for(std::size_t c=0;c<3;++c) {
        const auto name=QStringLiteral("AdjustmentBalance%1").arg(c).toLatin1();
        number(balance,name.constData(),balanceNames[c],-100,100,0,
            [this,c]{return std::get<core::ColorBalanceParameters>(item(Type::ColorBalance).parameters).tones[std::size_t(balanceRange_->currentIndex())][c]*100;},
            [this,c](double v){std::get<core::ColorBalanceParameters>(item(Type::ColorBalance).parameters).tones[std::size_t(balanceRange_->currentIndex())][c]=v/100;});
    }
    preserveLuminosity_=i18n::make<QCheckBox>(i18n::text("adjustments.panel.preserve.luminosity"));preserveLuminosity_->setObjectName(QStringLiteral("AdjustmentPreserveLuminosity"));balance.layout->addWidget(preserveLuminosity_);
    connect(preserveLuminosity_,&QCheckBox::toggled,this,[this](bool value){if(!updating_)change(Type::ColorBalance,[value](auto& a){std::get<core::ColorBalanceParameters>(a.parameters).preserveLuminosity=value;});});
    explanation(balance.layout, i18n::text("adjustments.panel.smooth.tonal.weighting.combines.all.three.ranges.preserve.luminosity"));

    auto& warmth=pages_[index(Type::WarmthTint)];
    number(warmth, "AdjustmentWarmth", i18n::text("adjustments.panel.warmth"), -100, 100, 0, [this]{return std::get<core::WarmthTintParameters>(item(Type::WarmthTint).parameters).warmth*100;}, [this](double v){std::get<core::WarmthTintParameters>(item(Type::WarmthTint).parameters).warmth=v/100;});
    number(warmth, "AdjustmentTint", i18n::text("adjustments.panel.tint"), -100, 100, 0, [this]{return std::get<core::WarmthTintParameters>(item(Type::WarmthTint).parameters).tint*100;}, [this](double v){std::get<core::WarmthTintParameters>(item(Type::WarmthTint).parameters).tint=v/100;});
    explanation(warmth.layout, i18n::text("adjustments.panel.relative.warm.cool.and.green.magenta.correction.for.rendered"));

    auto& mono=pages_[index(Type::BlackWhite)];
    for(std::size_t c=0;c<6;++c){
        const auto name=QStringLiteral("AdjustmentMono%1").arg(c).toLatin1();
        number(mono,name.constData(),hues[qsizetype(c+1)],-100,100,0,
            [this,c]{return std::get<core::BlackWhiteParameters>(item(Type::BlackWhite).parameters).contributions[c]*100;},
            [this,c](double v){std::get<core::BlackWhiteParameters>(item(Type::BlackWhite).parameters).contributions[c]=v/100;});
    }
    tint_=new QPushButton; tint_->setObjectName(QStringLiteral("AdjustmentMonoTintColor"));mono.layout->addWidget(tint_);
    connect(tint_,&QPushButton::clicked,this,&AdjustmentsPanel::chooseTint);
    number(mono, "AdjustmentMonoTintStrength", i18n::text("adjustments.panel.tint.strength"), 0, 100, 0, [this]{return std::get<core::BlackWhiteParameters>(item(Type::BlackWhite).parameters).tintStrength*100;}, [this](double v){std::get<core::BlackWhiteParameters>(item(Type::BlackWhite).parameters).tintStrength=v/100;}, QStringLiteral("%"));
    explanation(mono.layout, i18n::text("adjustments.panel.hue.weighted.luminance.offsets.preserve.neutral.gray.stability.tint"));
    explanation(pages_[index(Type::Invert)].layout, i18n::text("adjustments.panel.invert.unassociated.srgb.color.channels.while.keeping.alpha.unchanged"));

    for(auto& page:pages_)page.layout->addStretch(1);
    filtersPanel_=new FiltersPanel;
    filtersPanel_->setEmbedded();
    i18n::addTab(tabs_, filtersPanel_, i18n::text("adjustments.panel.filters"));
    effectsPanel_=new EffectsPanel;i18n::addTab(tabs_, effectsPanel_, i18n::text("adjustments.panel.effects"));
    connect(&i18n::Localization::instance(), &i18n::Localization::languageChanged, this, &AdjustmentsPanel::refreshCategoryHints);
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
void AdjustmentsPanel::showEffect(core::LayerEffectType type)
{
    if (!tabs_->isTabEnabled(tabs_->indexOf(effectsPanel_))) return;
    tabs_->setCurrentWidget(effectsPanel_);
    effectsPanel_->selectType(type);
    tabs_->tabBar()->setFocus(Qt::OtherFocusReason);
}
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
    page.enabled=i18n::make<QCheckBox>(i18n::text("adjustments.panel.enabled"));page.enabled->setObjectName(QStringLiteral("AdjustmentEnabled%1").arg(int(type)));
    auto* reset=i18n::make<QPushButton>(i18n::text("adjustments.panel.reset"));reset->setObjectName(QStringLiteral("AdjustmentReset%1").arg(int(type)));
    connect(page.enabled,&QCheckBox::toggled,this,[this,type](bool enabled){if(!updating_)change(type,[enabled](auto& a){a.enabled=enabled;},false);});
    connect(reset,&QPushButton::clicked,this,[this,type]{change(type,[type](auto& a){a=core::defaultAdjustment(type);},false);});
    page.scope=new QComboBox;page.scope->setObjectName(QStringLiteral("AdjustmentScope%1").arg(int(type)));
    i18n::addItems(page.scope, {i18n::text("adjustments.panel.whole.layer"),i18n::text("adjustments.panel.captured.selection")});
    auto* scopeRow=new QHBoxLayout;scopeRow->setSpacing(6);
    page.scope->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);
    scopeRow->addWidget(page.scope,1);
    page.capture=i18n::make<QPushButton>(i18n::text("adjustments.panel.capture"));
    page.capture->setObjectName(QStringLiteral("AdjustmentCapture%1").arg(int(type)));
    i18n::setToolTip(page.capture, i18n::text("adjustments.panel.own.a.snapshot.of.the.current.selection.in.this"));
    scopeRow->addWidget(page.capture);scopeRow->addWidget(reset);page.layout->addLayout(scopeRow);
    page.maskStatus=explanation(page.layout,QString());
    page.showRegion=i18n::make<QCheckBox>(i18n::text("adjustments.panel.show.captured.region"));
    page.showRegion->setObjectName(QStringLiteral("AdjustmentShowRegion%1").arg(int(type)));
    i18n::setToolTip(page.showRegion, i18n::text("adjustments.panel.show.a.static.accent.outline.of.this.adjustment.s"));
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
CompactValueControl* AdjustmentsPanel::number(Page& page,const char* name,const i18n::Message& label,
    double min,double max,int decimals,std::function<double()> read,std::function<void(double)> write,const QString& suffix)
{
    auto* control=new CompactValueControl;
    control->setObjectName(QString::fromLatin1(name));i18n::setPrefix(control, label+QStringLiteral(": "));control->setSuffix(suffix);
    control->setDecimals(decimals);control->setRange(min,max);control->setSingleStep(decimals>0?.1:1);
    control->setFixedHeight(30);control->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);
    i18n::setAccessibleName(control, label);page.layout->addWidget(control);page.numbers.push_back(control);
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
QComboBox* AdjustmentsPanel::choices(Page& page,const char* name,const QList<i18n::Message>& labels,std::function<void(int)> callback)
{
    auto* combo=new QComboBox;combo->setObjectName(QString::fromLatin1(name));i18n::addItems(combo, labels);
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
    i18n::setText(targetLabel_, layer?i18n::text("adjustments.panel.1.primary.layer").arg(QString::fromStdString(layer->name)):i18n::text("adjustments.panel.select.a.raster.text.or.shape.layer"));
    targetLabel_->setToolTip(layer?QString::fromStdString(layer->name):QString());
    const auto* adjustment=layer?std::get_if<core::AdjustmentLayer>(&layer->payload):nullptr;
    const auto placement=document&&layer?document->tree().placement(layer->id):std::nullopt;
    scopeLabel_->setVisible(adjustment);
    if(adjustment)i18n::setText(scopeLabel_, adjustment->scope==core::AdjustmentScope::ThisGroup&&placement&&placement->parent
        ?i18n::text("adjustments.panel.this.group.lower.content.including.nested.groups.this.group")
        :i18n::text("adjustments.panel.all.below.accumulated.content.in.the.current.compositing.domain"));
    filtersPanel_->setTarget(adjustment?nullptr:layer,hasSelection);
    effectsPanel_->setTarget(adjustment?nullptr:layer);
    tabs_->setTabEnabled(3,!adjustment);tabs_->setTabEnabled(4,!adjustment);
    if(adjustment && tabs_->currentIndex()>2)tabs_->setCurrentIndex(0);
    tabs_->setEnabled(layer);resetAll_->setEnabled(layer);compare_->setEnabled(layer);refreshCategoryHints();refresh();
}
void AdjustmentsPanel::refreshCategoryHints()
{
    const auto hint = !tabs_->isEnabled() ? i18n::text("adjustments.panel.select.a.raster.text.or.shape.layer")
        : !tabs_->isTabEnabled(3) ? i18n::text("adjustments.panel.operator.no.filters.effects") : i18n::Message{};
    for (int tab : {3, 4}) tabs_->setTabToolTip(tab, hint);
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
        i18n::setText(page.maskStatus, adjustment.mask
            ? (adjustment.mask->coverage->bounds().empty()?i18n::text("adjustments.panel.captured.empty.mask.affects.no.pixels"):i18n::text("adjustments.panel.owned.mask.follows.this.layer"))
            :i18n::text("adjustments.panel.all.source.pixels.selection.is.not.implicit"));
        page.maskStatus->setVisible(adjustment.mask.has_value());
        page.showRegion->setVisible(adjustment.mask.has_value());
        page.showRegion->setChecked(showCapturedRegion_);
        page.showRegion->setEnabled(adjustment.mask && !adjustment.mask->coverage->bounds().empty());
        i18n::setToolTip(page.scope, adjustment.mask?i18n::text("adjustments.panel.this.owned.selection.mask.follows.the.layer.independent.of")
            :i18n::text("adjustments.panel.whole.layer.all.source.pixels.capture.explicitly.to.use"));
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
        i18n::setText(tint_, i18n::text("adjustments.panel.tint.color.1").arg(qt.name(QColor::HexRgb).toUpper()));
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
    i18n::setText(resetAll_, effectsCategoryActive()?i18n::text("adjustments.panel.reset.effects"):i18n::text("adjustments.panel.reset.all"));
    if(effectsCategoryActive()) {
        i18n::setToolTip(compare_, i18n::text("adjustments.panel.temporarily.bypass.only.this.layer.s.effects.on.canvas"));
        i18n::setToolTip(resetAll_, i18n::text("adjustments.panel.disable.and.reset.all.seven.effects.adjustments.and.filters"));
        refresh();return;
    }
    i18n::setToolTip(compare_, filtersCategoryActive()
        ? i18n::text("adjustments.panel.temporarily.bypass.this.layer.s.spatial.filters.on.canvas")
        : i18n::text("adjustments.panel.temporarily.bypass.this.layer.s.color.adjustments.on.canvas"));
    i18n::setToolTip(resetAll_, filtersCategoryActive()
        ? i18n::text("adjustments.panel.reset.all.spatial.filters.and.their.captured.masks.color")
        : i18n::text("adjustments.panel.reset.all.tone.color.and.monochrome.adjustments.spatial.filters"));
    refresh();if(onHistogramRequested)onHistogramRequested();
}
void AdjustmentsPanel::chooseTint()
{
    finishEditing();if(!target_)return;
    const auto target=target_;
    const auto color=std::get<core::BlackWhiteParameters>(item(Type::BlackWhite).parameters).tintColor;
    auto* owner=window();while(owner->parentWidget())owner=owner->parentWidget()->window();
    auto* dialog=new ColorDialog(QColor(color.red,color.green,color.blue),owner);
    dialog->setObjectName(QStringLiteral("AdjustmentTintDialog"));i18n::setWindowTitle(dialog, i18n::text("adjustments.panel.black.white.tint"));
    dialog->setOptions(ColorDialog::DontUseNativeDialog);dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog,&ColorDialog::colorSelected,this,[this,target](QColor color){
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
