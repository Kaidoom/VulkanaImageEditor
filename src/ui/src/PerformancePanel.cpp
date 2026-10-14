#include "imageeditor/ui/PerformancePanel.hpp"
#include "imageeditor/ui/Localization.hpp"
#include "imageeditor/ui/ToolIcons.hpp"
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QScrollArea>
#include <QStandardItemModel>
#include <QVBoxLayout>
#include <QShowEvent>
#include <QHideEvent>
namespace imageeditor::ui {
PerformancePanel::PerformancePanel(QWidget* parent):QWidget(parent)
{
    setObjectName("PreferencesPerformancePage");
    auto* layout=new QVBoxLayout(this);layout->setContentsMargins(0,0,0,0);
    auto* scroll=new QScrollArea(this);scroll->setWidgetResizable(true);scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);layout->addWidget(scroll);
    auto* body=new QWidget(scroll);auto* content=new QVBoxLayout(body);content->setContentsMargins(16,10,16,10);content->setSpacing(8);
    auto* form=new QFormLayout;form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setVerticalSpacing(4);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);content->addLayout(form);
    const auto row=[&](const char* name,const char* label){
        auto* box=new QComboBox(body);box->setObjectName(QString::fromLatin1(name));
        box->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);box->setMinimumContentsLength(15);
        box->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        auto* text=i18n::make<QLabel>(i18n::text(label),body);text->setBuddy(box);form->addRow(text,box);
        i18n::setAccessibleName(box,i18n::text(label));connect(box,&QComboBox::currentIndexChanged,this,[this]{changed();});return box;
    };
    gpu_=row("PerformanceGpu","performance.gpu");i18n::addItem(gpu_,i18n::text("performance.auto"),QString{});
    i18n::setToolTip(gpu_,i18n::text("performance.gpu.check.hint"));
    const auto memory=[&](const char* name,const char* label,int automatic){
        auto* box=row(name,label);i18n::addItem(box,i18n::text("performance.auto.memory").arg(automatic),0);
        for(int mib:{64,128,256,512,1024,2048,4096,8192})box->addItem(QString::number(mib)+QStringLiteral(" MiB"),mib);
        return box;
    };
    undo_=memory("PerformanceUndo","performance.undo",256);
    cpuCache_=memory("PerformanceCpuCache","performance.cpu.cache",512);
    gpuCache_=memory("PerformanceGpuCache","performance.gpu.cache",512);
    threads_=row("PerformanceThreads","performance.threads");
    objectThreads_=row("PerformanceObjectThreads","performance.object.threads");
    i18n::addItem(threads_,i18n::text("performance.auto.threads").arg(platform::automaticProcessingThreads()),0);
    i18n::addItem(objectThreads_,i18n::text("performance.auto.threads").arg(platform::resolvedObjectSelectionThreads({})),0);
    for(int i=1;i<=8;++i)threads_->addItem(QString::number(i),i);
    for(int i=1;i<=6;++i)objectThreads_->addItem(QString::number(i),i);
    for(const auto* id:{"performance.restart.note","performance.memory.note"}) {
        auto* note=i18n::make<QLabel>(i18n::text(id),body);note->setWordWrap(true);note->setObjectName("MutedLabel");content->addWidget(note);
    }
    usage_=new QLabel(body);usage_->setObjectName("PerformanceUsage");usage_->setTextFormat(Qt::PlainText);usage_->setWordWrap(true);content->addWidget(usage_);
    auto* buttons=new QHBoxLayout;
    auto* clear=i18n::make<QPushButton>(toolGlyph(ToolGlyph::Trash),i18n::text("performance.clear"),body);
    clear->setObjectName("PerformanceClearCaches");clear->setAutoDefault(false);
    i18n::setToolTip(clear,i18n::text("performance.clear.hint"));buttons->addWidget(clear);
    auto* defaults=i18n::make<QPushButton>(toolGlyph(ToolGlyph::ResetCrop),i18n::text("performance.defaults"),body);
    defaults->setObjectName("PerformanceDefaults");defaults->setAutoDefault(false);buttons->addWidget(defaults);buttons->addStretch();content->addLayout(buttons);
    content->addStretch();scroll->setWidget(body);
    connect(clear,&QPushButton::clicked,this,[this]{if(onClearCaches)onClearCaches();refreshUsage();});
    connect(defaults,&QPushButton::clicked,this,[this]{setSettings({});changed();});
    timer_.setInterval(1000);connect(&timer_,&QTimer::timeout,this,[this]{refreshUsage();});
}
platform::PerformanceSettings PerformancePanel::settings() const
{
    return platform::sanitizePerformanceSettings({undo_->currentData().toInt(),cpuCache_->currentData().toInt(),gpuCache_->currentData().toInt(),
        threads_->currentData().toInt(),objectThreads_->currentData().toInt(),gpu_->currentData().toString()});
}
void PerformancePanel::setSettings(platform::PerformanceSettings s)
{
    s=platform::sanitizePerformanceSettings(std::move(s));const QScopedValueRollback guard(updating_,true);
    const auto select=[](QComboBox* box,int value){
        auto index=box->findData(value);if(index<0){box->addItem(QString::number(value)+QStringLiteral(" MiB"),value);index=box->count()-1;}box->setCurrentIndex(index);
    };
    select(undo_,s.undoMiB);select(cpuCache_,s.documentCacheMiB);select(gpuCache_,s.textureCacheMiB);
    select(threads_,s.processingThreads);select(objectThreads_,s.objectSelectionThreads);
    auto index=gpu_->findData(s.graphicsDevice);
    if(index<0){i18n::addItem(gpu_,i18n::text("performance.gpu.unavailable"),s.graphicsDevice);index=gpu_->count()-1;
        if(auto* model=qobject_cast<QStandardItemModel*>(gpu_->model()))model->item(index)->setEnabled(false);}
    gpu_->setCurrentIndex(index);
}
void PerformancePanel::setGraphicsDevices(const std::vector<render::GraphicsDeviceInfo>& devices)
{
    const auto s=settings();const QScopedValueRollback guard(updating_,true);gpu_->clear();
    i18n::addItem(gpu_,i18n::text("performance.auto"),QString{});
    for(const auto& d:devices)if(!d.id.empty()) {
        const auto name=QString::fromStdString(d.name),id=QString::fromStdString(d.id);
        if(d.availability==render::GraphicsDeviceAvailability::Available)gpu_->addItem(name,id);
        else {
            i18n::addItem(gpu_,i18n::text(d.availability==render::GraphicsDeviceAvailability::StartupCheckFailed
                ?"performance.gpu.failed.item":"performance.gpu.unsupported.item").arg(name),id);
            if(auto* model=qobject_cast<QStandardItemModel*>(gpu_->model()))model->item(gpu_->count()-1)->setEnabled(false);
        }
    }
    setSettings(s);
}
void PerformancePanel::changed(){if(!updating_&&onChanged)onChanged();}
void PerformancePanel::refreshUsage()
{
    if(!isVisible()||window()->isMinimized()||!readUsage)return;
    const auto u=readUsage();const auto mib=[](std::uint64_t value){return QString::number(double(value)/(1024*1024),'f',1)+QStringLiteral(" MiB");};
    const auto ram=u.residentBytes?i18n::ltrToken(mib(*u.residentBytes)):i18n::text("performance.unknown");
    i18n::setText(usage_,i18n::text("performance.usage").arg(ram).arg(i18n::ltrToken(mib(u.activeUndoBytes))).arg(i18n::ltrToken(mib(u.allUndoBytes)))
        .arg(i18n::ltrToken(mib(u.documentCacheBytes))).arg(i18n::ltrToken(mib(u.textureCacheBytes)))
        .arg(u.gpuName.isEmpty()?i18n::text("performance.unknown"):i18n::ltrToken(u.gpuName)));
}
void PerformancePanel::showEvent(QShowEvent* e){QWidget::showEvent(e);refreshUsage();timer_.start();}
void PerformancePanel::hideEvent(QHideEvent* e){timer_.stop();QWidget::hideEvent(e);}
}
