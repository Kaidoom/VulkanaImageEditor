#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/PerformancePanel.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/platform/AvailableMemory.hpp"
#include "imageeditor/ui/WorkspaceDialog.hpp"
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QProgressBar>
#include <QScopedValueRollback>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>
namespace imageeditor::ui {
namespace {
class GraphicsInitializationCard final:public QDialog {
public:
    explicit GraphicsInitializationCard(QWidget* parent):QDialog(parent,Qt::SubWindow){}
    // This is a status card, not an interruptible operation. The main window's
    // close control remains available while the driver finishes compilation.
    void reject() override {}
};
}
void MainWindow::graphicsInitializationChanged(bool pending)
{
    if(pending) {
        if(!graphicsStartupPending_&&!canvasWindow_->graphicsInitializing())return;
        if(graphicsStartupPresenter_) {
            if(graphicsClosePending_) {
                auto* title=graphicsStartupPresenter_->findChild<QLabel*>("ToolTitle");
                i18n::setText(title,i18n::text("graphics.startup.aborting"));
                auto* detail=graphicsStartupPresenter_->findChild<QLabel*>("GraphicsInitializationDetail");
                i18n::setText(detail,i18n::text("graphics.startup.closing"));
            }
            return;
        }
        auto* presenter=new WorkspaceDialog(*workspace_,*this);
        graphicsStartupPresenter_=presenter;graphicsPreviousDialog_=workspaceDialog_;workspaceDialog_=presenter;
        auto* card=new GraphicsInitializationCard(presenter);card->setObjectName("GraphicsInitializationCard");
        card->setFixedWidth(390);
        auto* layout=new QVBoxLayout(card);layout->setContentsMargins(20,18,20,18);layout->setSpacing(12);
        auto* title=i18n::make<QLabel>(i18n::text(graphicsClosePending_?"graphics.startup.aborting":"graphics.startup.title"));
        title->setObjectName("ToolTitle");layout->addWidget(title);
        auto* detail=i18n::make<QLabel>(i18n::text(graphicsClosePending_?"graphics.startup.closing":"graphics.startup.detail"));
        detail->setObjectName("GraphicsInitializationDetail");detail->setWordWrap(true);layout->addWidget(detail);
        auto* progress=new QProgressBar(card);progress->setObjectName("GraphicsInitializationProgress");
        progress->setRange(0,0);progress->setTextVisible(false);layout->addWidget(progress);
        presenter->open(*card);
        return;
    }
    // Compiler completion precedes the first frame. Keep the very same card
    // through device, pipeline and swapchain setup; never expose welcome UI in
    // that gap. Closing is the exception: it must not require rendering a frame.
    if(graphicsStartupPending_&&!graphicsClosePending_&&!canvasWindow_->rendererStats().presentQueuedFrames)return;
    graphicsStartupPending_=false;
    if(graphicsStartupPresenter_) {
        if(workspaceDialog_==graphicsStartupPresenter_)workspaceDialog_=graphicsPreviousDialog_;
        graphicsStartupPresenter_->finish();graphicsStartupPresenter_->deleteLater();graphicsStartupPresenter_=nullptr;graphicsPreviousDialog_=nullptr;
    }
    if(graphicsClosePending_) {graphicsClosePending_=false;close();}
    else {
        refreshDocumentTabs();
        if(startupUiTimer_)startupUiTimer_->start(0);
    }
}
void MainWindow::handleGraphicsDeviceFallback(const render::GraphicsDeviceFallback& recovery)
{
    const auto selected=QString::fromStdString(recovery.selectedId);
    if(!platform::acceptGraphicsDeviceFallback(selected)){gpuFallbackPending_=false;return;}
    QSettings settings;
    auto saved=platform::loadPerformanceSettings(settings);
    // Do not overwrite a newer explicit choice made while startup was pending.
    if(saved.graphicsDevice==QString::fromStdString(recovery.requestedId)) {
        saved.graphicsDevice=selected;
        if(!platform::savePerformanceSettings(settings,saved))qWarning("Could not save replacement graphics device");
        for(auto* widget:QApplication::topLevelWidgets())if(auto* editor=dynamic_cast<MainWindow*>(widget))
            editor->performance_.graphicsDevice=selected;
    }
    gpuFallbackRequested_=QString::fromStdString(recovery.requestedName);
    gpuFallbackSelected_=QString::fromStdString(recovery.selectedName);
    gpuFallbackPending_=true;
    presentGraphicsDeviceFallback();
}
void MainWindow::presentGraphicsDeviceFallback()
{
    if(!gpuFallbackPending_)return;
    if(!isVisible()||isMinimized()||fileBusy_||workspaceDialog_||QApplication::activeModalWidget()
        ||QApplication::activePopupWidget()||!shortcutGestureIdle()||editorTextInputActive()) {
        QTimer::singleShot(200,this,&MainWindow::presentGraphicsDeviceFallback);return;
    }
    gpuFallbackPending_=false;
    WorkspaceDialog presenter(*workspace_,*this);
    QDialog notification(&presenter,Qt::SubWindow);
    notification.setObjectName("GpuFallbackNotification");notification.setFixedWidth(430);
    i18n::setWindowTitle(&notification,i18n::text("performance.gpu.changed.title"));
    auto* layout=new QVBoxLayout(&notification);layout->setContentsMargins(20,18,20,18);layout->setSpacing(14);
    auto* title=i18n::make<QLabel>(i18n::text("performance.gpu.changed.title"));title->setObjectName("ToolTitle");layout->addWidget(title);
    auto* message=new QLabel(&notification);message->setObjectName("GpuFallbackMessage");message->setTextFormat(Qt::PlainText);message->setWordWrap(true);
    if(gpuFallbackRequested_.isEmpty())i18n::setText(message,i18n::text("performance.gpu.changed.unknown").arg(i18n::ltrToken(gpuFallbackSelected_)));
    else i18n::setText(message,i18n::text("performance.gpu.changed.message").arg(i18n::ltrToken(gpuFallbackRequested_)).arg(i18n::ltrToken(gpuFallbackSelected_)));
    layout->addWidget(message);
    auto* buttons=new i18n::ButtonBox(QDialogButtonBox::Ok,&notification);layout->addWidget(buttons);
    connect(buttons,&QDialogButtonBox::accepted,&notification,&QDialog::accept);
    const QScopedValueRollback active(workspaceDialog_,&presenter);
    presenter.exec(notification);
}
void MainWindow::applyPerformanceMemory(const platform::PerformanceSettings& value)
{
    const auto next=platform::sanitizePerformanceSettings(value);
    const bool unchanged=next.undoBytes()==performance_.undoBytes()&&next.documentCacheBytes()==performance_.documentCacheBytes()
        &&next.textureCacheBytes()==performance_.textureCacheBytes();
    performance_=next;if(unchanged)return;
    // No trimming during an in-flight/retractable edit, and no changes to the
    // redo branch or saved checkpoint. History trims at its normal next commit.
    for(const auto& context:documents_)context->session.history().setMemoryBudget(std::size_t(performance_.undoBytes()));
    emptyDocument_.session.history().setMemoryBudget(std::size_t(performance_.undoBytes()));
    canvasWindow_->setTextureCacheBudget(performance_.textureCacheBytes());
    updateDocumentResources();
}
void MainWindow::clearInactiveCaches()
{
    updateDocumentResources(true);
    // Existing renderer deferral keeps in-flight Vulkan allocations alive
    // until their fences complete. Required active textures are never evicted.
    canvasWindow_->clearInactiveTextures();
}
PerformanceUsage MainWindow::performanceUsage() const
{
    const auto memory=documentMemory();const auto renderer=canvasWindow_->rendererStats();
    return {platform::processResidentMemoryBytes(),session().history().memoryUsed(),memory.historyBytes,
        memory.derivedBytes,renderer.textureCacheBytes,QString::fromStdString(renderer.deviceName)};
}
}
