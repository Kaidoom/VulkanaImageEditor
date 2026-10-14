#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/PerformancePanel.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/platform/AvailableMemory.hpp"
#include "imageeditor/ui/WorkspaceDialog.hpp"
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QScopedValueRollback>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>
namespace imageeditor::ui {
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
