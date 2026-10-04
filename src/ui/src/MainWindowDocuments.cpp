#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/OverlayDockWorkspace.hpp"
#include "imageeditor/ui/LayerListModel.hpp"
#include "imageeditor/ui/LayerTransferMimeData.hpp"
#include "imageeditor/ui/CompactValueControl.hpp"
#include "imageeditor/ui/TextController.hpp"
#include "imageeditor/ui/AdjustmentsPanel.hpp"
#include "imageeditor/ui/FiltersPanel.hpp"
#include "imageeditor/ui/EffectsPanel.hpp"
#include "imageeditor/ui/TransformOptionsPage.hpp"
#include "imageeditor/ui/ShapeOptionsPage.hpp"
#include "imageeditor/ui/CropOptionsPage.hpp"
#include "imageeditor/ui/CrossWindowPointerRouter.hpp"
#include "imageeditor/ui/PixelPreview.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/core/LayerGeometry.hpp"
#include "imageeditor/core/SpatialFilterCache.hpp"
#include <QAction>
#include <QApplication>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QListView>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QTabBar>
#include <QTimer>
#include <QDrag>
#include <QDropEvent>
#include <QMouseEvent>
#include <QWindow>
#include <QStatusBar>
#include <QVBoxLayout>
#include <algorithm>
#include <unordered_set>

namespace imageeditor::ui {
namespace {
class DocumentTabs final : public QTabBar, public PointerPressPreparation {
public:
    using QTabBar::QTabBar;
    void preparePointerPress(const QMouseEvent& event) override {
        if (event.button() != Qt::LeftButton) return;
        const int index = tabAt(event.position().toPoint());
        // Activation settles the old document's gestures, including capture.
        // Do that before this press acquires capture or QTabBar's drag state.
        if (index >= 0 && isTabEnabled(index) && index != currentIndex()) setCurrentIndex(index);
    }
    QSize tabSizeHint(int index) const override {
        auto size = QTabBar::tabSizeHint(index);
        size.setWidth(std::clamp(size.width(), 120, 250));
        size.setHeight(32);
        return size;
    }
    QSize sizeHint() const override { auto size=QTabBar::sizeHint();size.setHeight(32);return size; }
protected:
    void mousePressEvent(QMouseEvent* event) override {
        preparePointerPress(*event); // Also correct without the workspace router.
        QTabBar::mousePressEvent(event);
    }
    void tabLayoutChange() override {
        QTabBar::tabLayoutChange();
        // Some styles position the close control against a bounded tab's edge
        // despite stylesheet padding. Keep the standard Qt button and inset it.
        for(int i=0;i<count();++i) if(auto* button=tabButton(i,QTabBar::RightSide)) {
            const auto tab=tabRect(i);
            button->move(std::min(button->x(),tab.right()-8-button->width()+1),button->y());
        }
    }
};
}
std::vector<DocumentInstanceId> MainWindow::documentIds() const
{
    std::vector<DocumentInstanceId> ids;
    for (const auto& context : documents_) ids.push_back(context->id);
    return ids;
}
const DocumentContext* MainWindow::documentContext(DocumentInstanceId id) const
{
    for (const auto& context : documents_) if (context->id == id) return context.get();
    return nullptr;
}
void MainWindow::createDocumentTabs()
{
    // A real layout row outside the native canvas, not another workspace
    // overlay. Its height is stable even with zero tabs; switching/reordering
    // documents never changes the workspace or swapchain geometry.
    auto* panel = new QWidget(centralWidget());
    panel->setObjectName("DocumentTabsPanel");
    panel->setAttribute(Qt::WA_StyledBackground);
    panel->setFixedHeight(33);
    auto* tabLayout = new QHBoxLayout(panel);
    tabLayout->setContentsMargins(4, 0, 4, 0); // Panel's 1 px border completes the 33 px row.
    tabLayout->setSpacing(0);
    documentTabs_ = new DocumentTabs(panel);
    documentTabs_->setObjectName("DocumentTabs");
    documentTabs_->setExpanding(false);
    documentTabs_->setMovable(true);
    documentTabs_->setTabsClosable(true);
    documentTabs_->setUsesScrollButtons(true);
    documentTabs_->setElideMode(Qt::ElideMiddle);
    documentTabs_->setDrawBase(false);
    tabLayout->addWidget(documentTabs_);
    static_cast<QVBoxLayout*>(centralWidget()->layout())->insertWidget(0, panel);
    documentTabs_->setFocusPolicy(Qt::NoFocus);
    documentTabs_->setAcceptDrops(true);
    tabHoverTimer_ = new QTimer(this);
    tabHoverTimer_->setSingleShot(true);
    tabHoverTimer_->setInterval(450);
    connect(tabHoverTimer_, &QTimer::timeout, this, [this] {
        const auto* transfer = dynamic_cast<const LayerTransferMimeData*>(tabHoverPayload_.data());
        if (transfer && transfer->source && !transfer->source->closed && !transfer->consumed
            && documentContext(tabHoverTarget_) && activateDocument(tabHoverTarget_)) transfer->destination = tabHoverTarget_;
    });
    layerModel_->onCaptureTransfer = [this](std::span<const core::LayerId> ids) { return captureLayerTransfer(ids); };
    layerModel_->onCanTransfer = [this](const QMimeData* data) { return canReceiveLayerTransfer(data); };
    layerModel_->onTransfer = [this](const QMimeData* data, core::ItemPlacement placement) {
        return receiveLayerTransfer(data, {}, placement);
    };
    connect(documentTabs_, &QTabBar::currentChanged, this, [this](int index) {
        if (switchingDocument_ || index < 0) return;
        if (!activateDocument(documentTabs_->tabData(index).toULongLong())) refreshDocumentTabs();
    });
    connect(documentTabs_, &QTabBar::tabCloseRequested, this, [this](int index) {
        closeDocument(documentTabs_->tabData(index).toULongLong());
    });
    connect(documentTabs_, &QTabBar::tabMoved, this, [this](int from, int to) {
        if (switchingDocument_) return;
        auto context = documents_.at(std::size_t(from));
        documents_.erase(documents_.begin() + from);
        documents_.insert(documents_.begin() + to, std::move(context));
    });
    welcome_ = new QWidget(workspace_->panelOverlay());
    welcome_->setObjectName("DocumentWelcome");
    welcome_->setAcceptDrops(true);
    auto* layout = new QVBoxLayout(welcome_);
    auto* title = new QLabel(tr("Open an image or start a new canvas"), welcome_);
    title->setAlignment(Qt::AlignCenter);
    layout->addWidget(title);
    auto* row = new QHBoxLayout;
    auto* create = new QPushButton(tr("New Canvas"), welcome_);
    auto* open = new QPushButton(tr("Open Document…"), welcome_);
    connect(create, &QPushButton::clicked, this, &MainWindow::createNewDocument);
    connect(open, &QPushButton::clicked, this, &MainWindow::openImage);
    row->addWidget(create); row->addWidget(open); layout->addLayout(row);
    workspace_->setWelcomeOverlay(welcome_, false);
}
QMimeData* MainWindow::captureLayerTransfer(std::span<const core::LayerId> ids, std::optional<core::Vec2d> grab)
{
    if (!activeDocument_ || ids.empty()) return nullptr;
    try {
        auto transfer = std::make_unique<LayerTransferMimeData>();
        transfer->source = activeDocument_;
        transfer->content = core::captureLayerTransfer(*session().document(), ids);
        const auto bounds = core::selectedLayerBounds(*session().document(), ids);
        transfer->anchor = grab.value_or(bounds ? (bounds->minimum + bounds->maximum) * 0.5 : core::Vec2d{});
        transfer->setData("application/x-vulkana-layer-transfer", "1");
        return transfer.release();
    } catch (const std::exception& e) {
        statusBar()->showMessage(tr("Unable to copy layers: %1").arg(QString::fromUtf8(e.what())), 6000);
        return nullptr;
    }
}
bool MainWindow::canReceiveLayerTransfer(const QMimeData* data) const
{
    const auto* transfer = dynamic_cast<const LayerTransferMimeData*>(data);
    return transfer && transfer->source && !transfer->source->closed && !transfer->consumed
        && activeDocument_ && transfer->source != activeDocument_ && !fileBusy_
        && (!transfer->destination || transfer->destination == activeDocumentId());
}
bool MainWindow::receiveLayerTransfer(const QMimeData* data, std::optional<core::Vec2d> point,
    std::optional<core::ItemPlacement> placement)
{
    if (!canReceiveLayerTransfer(data) || !settleForDocumentSwitch()) return false;
    const auto* transfer = static_cast<const LayerTransferMimeData*>(data);
    auto& document = *session().document();
    if (!placement) {
        placement = core::ItemPlacement{0, document.tree().roots.size()};
        if (const auto active = session().activeLayer()) {
            placement=document.tree().insertionAbove(*active);
        }
    }
    try {
        transfer->consumed = true;
        auto command = core::insertLayerTransfer(document, std::move(transfer->content), *placement,
            session().layerSelectionState(), point ? *point - transfer->anchor : core::Vec2d{});
        if (!command || !executeDocumentCommand(std::move(command))) return false;
        fileState().untouched = false;
        synchronizeUi(true, false);
        return true;
    } catch (const std::exception& e) {
        statusBar()->showMessage(tr("Unable to copy layers: %1").arg(QString::fromUtf8(e.what())), 6000);
        return false;
    }
}
bool MainWindow::routeDocumentDrag(QObject* watched, QEvent* event)
{
    const auto clearHover = [this] { tabHoverTimer_->stop(); tabHoverTarget_ = 0; tabHoverPayload_.clear(); };
    const QRect stripRect(documentTabs_->mapToGlobal(QPoint{}), documentTabs_->size());
    if (event->type() == QEvent::MouseMove && activeLayerMove_ && activeLayerMove_->dragging()) {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->buttons().testFlag(Qt::LeftButton) && stripRect.contains(mouse->globalPosition().toPoint())) {
            canvasWindow_->cancelTransformInput();
            finishLayerMove(false);
            pointerRouter_->cancelCapture();
            auto* payload = captureLayerTransfer(session().selectedLayers(), moveTransferGrab_);
            if (payload) {
                auto* drag = new QDrag(canvasContainer_);
                drag->setMimeData(payload);
                drag->exec(Qt::CopyAction, Qt::CopyAction);
                drag->deleteLater();
            }
            clearHover(); event->accept(); return true;
        }
    }
    if (event->type() == QEvent::DragLeave) { clearHover(); return false; }
    if (event->type() != QEvent::DragEnter && event->type() != QEvent::DragMove && event->type() != QEvent::Drop) return false;
    auto* drop = static_cast<QDropEvent*>(event);
    QPoint global;
    if (auto* widget = qobject_cast<QWidget*>(watched)) global = widget->mapToGlobal(drop->position().toPoint());
    else if (auto* window = qobject_cast<QWindow*>(watched)) global = window->mapToGlobal(drop->position().toPoint());
    else return false;
    const auto* transfer = dynamic_cast<const LayerTransferMimeData*>(drop->mimeData());
    const bool dropping = event->type() == QEvent::Drop;
    if (transfer && (!transfer->source || transfer->source->closed || transfer->consumed
        || (transfer->destination && !documentContext(transfer->destination)))) { clearHover(); drop->ignore(); return true; }
    if (stripRect.contains(global) || (!activeDocument_ && drop->mimeData()->hasUrls())) {
        if (transfer) {
            const auto index = documentTabs_->tabAt(documentTabs_->mapFromGlobal(global));
            const auto target = index < 0 ? 0 : documentTabs_->tabData(index).toULongLong();
            if (!target) { clearHover(); drop->ignore(); return true; }
            if (dropping) {
                clearHover();
                if (!activateDocument(target)) { drop->ignore(); return true; }
                transfer->destination = target;
                if (!receiveLayerTransfer(transfer)) { drop->ignore(); return true; }
            } else if (tabHoverTarget_ != target || tabHoverPayload_ != transfer) {
                tabHoverTarget_ = target; tabHoverPayload_ = transfer; tabHoverTimer_->start();
            }
            drop->setDropAction(Qt::CopyAction); drop->accept(); return true;
        }
        if (drop->mimeData()->hasUrls()) {
            clearHover();
            if (dropping) for (const auto& url : drop->mimeData()->urls()) if (url.isLocalFile()) openImageFromPath(url.toLocalFile());
            drop->acceptProposedAction(); return true;
        }
    }
    clearHover();
    if (transfer && (watched == canvasWindow_ || watched == canvasContainer_)) {
        if (!canReceiveLayerTransfer(transfer)) { drop->ignore(); return true; }
        if (dropping) {
            const auto local = canvasWindow_->mapFromGlobal(global);
            const auto point = canvasWindow_->scene().viewport.viewportToDocument({double(local.x()), double(local.y())},
                {double(session().document()->canvas().extent.width), double(session().document()->canvas().extent.height)},
                {double(canvasWindow_->width()), double(canvasWindow_->height())});
            if (!receiveLayerTransfer(transfer, point)) { drop->ignore(); return true; }
        }
        drop->setDropAction(Qt::CopyAction); drop->accept(); return true;
    }
    return false;
}
void MainWindow::refreshDocumentTabs()
{
    if (!documentTabs_) return;
    const QScopedValueRollback updating(switchingDocument_, true);
    const QSignalBlocker blocked(documentTabs_);
    while (documentTabs_->count() > int(documents_.size())) documentTabs_->removeTab(documentTabs_->count()-1);
    for (int i=0; i<int(documents_.size()); ++i) {
        const auto& context = documents_[std::size_t(i)];
        QString label = context->displayName;
        const auto path = context->projectPath.isEmpty() ? context->sourcePath : context->projectPath;
        const auto duplicates = std::ranges::count_if(documents_, [&](const auto& other) { return other->displayName == context->displayName; });
        if (duplicates > 1) {
            if (!path.isEmpty()) label += " · " + QFileInfo(QFileInfo(path).absolutePath()).fileName();
            else label += QStringLiteral(" · %1").arg(i+1);
            if (!path.isEmpty() && std::ranges::count_if(documents_, [&](const auto& other) {
                return other->displayName == context->displayName && QFileInfo(other->projectPath.isEmpty()?other->sourcePath:other->projectPath).absolutePath()==QFileInfo(path).absolutePath();
            }) > 1) label += QStringLiteral(" (%1)").arg(i+1);
        }
        if (context->session.document()->isModified()) label += " *";
        if (i == documentTabs_->count()) documentTabs_->addTab(label);
        else documentTabs_->setTabText(i, label);
        documentTabs_->setTabData(i, qulonglong(context->id));
        documentTabs_->setTabToolTip(i, path.isEmpty() ? tr("Unsaved document") : path);
        if (context == activeDocument_) documentTabs_->setCurrentIndex(i);
    }
    for (const auto* name : {"CloseDocumentAction", "NextDocumentAction", "PreviousDocumentAction"})
        if (auto* action = findChild<QAction*>(name)) action->setEnabled(!documents_.empty());
    for (const auto* name : {"SaveDocumentAction", "SaveDocumentAsAction", "ExportImageAction", "ImportImageAction", "ChangeCanvasSizeAction"})
        if (auto* action = findChild<QAction*>(name)) action->setEnabled(bool(activeDocument_));
    if (auto* action = findChild<QAction*>("ExportAgainAction")) action->setEnabled(activeDocument_ && activeDocument_->exportSettings.has_value());
    workspace_->setWelcomeOverlay(welcome_, !activeDocument_);
}
void MainWindow::captureDocumentView()
{
    if (!activeDocument_) return;
    activeDocument_->view = canvasWindow_->scene().viewport;
    activeDocument_->viewInitialized = true;
    activeDocument_->measurement = canvasWindow_->scene().measurement;
    activeDocument_->collapsedFolders = layerModel_->collapsedFolderIds();
    activeDocument_->cloneAnchor = cloneAnchor_;
}
bool MainWindow::settleForDocumentSwitch()
{
    if (fileBusy_ || workspaceDialog_ || cloneProcessing_ || localBlurProcessing_) return false;
    finishLayerRename();
    opacitySlider_->finishEditing();
    activeOpacityMergeKey_ = 0;
    adjustmentsPanel_->finishEditing(); finishAdjustmentEdit(true);
    filtersPanel_->finishEditing(); finishFilterEdit(true); finishEffectEdit(true);
    if (adjustmentEdit_ || filterEdit_ || effectEdit_) return false;
    if (textController_) textController_->finish();
    if (textController_ && textController_->active()) return false;
    moveOptionsPage_->finishNumericInput(); shapeOptionsPage_->finishNumericInput();
    transformOptionsPage_->finishNumericInput(); cropOptionsPage_->finishNumericInput();
    finishShapeCreation(true); finishShapeResize(false); finishShapeEdit(true);
    cancelActiveBrushStroke(true); cancelCloneStroke(); cancelLocalBlurStroke(); cancelFill();
    if (canvasWindow_->transformDragging()) canvasWindow_->cancelTransformInput();
    if (activeLayerMove_ && activeLayerMove_->dragging()) finishLayerMove(false);
    else finishLayerMove(true);
    finishLayerCrop(true); finishLayerTransform(true);
    if (activeLayerMove_ || layerTransform_ || layerCrop_) return false;
    finishSelectionNumericInput(); finishSelectionTransform(true);
    if(selectionTransform_)return false;
    cancelColorSelection(); cancelSmartSelection();
    canvasWindow_->cancelSelectionInput(); finishSelectionGesture(true); finishSelectionRotation(false);
    pointerRouter_->cancelCapture(); canvasWindow_->cancelPanInput(); canvasWindow_->cancelColorSampling();
    cancelFilterPreparation(true);
    if (shapeDensityTimer_) shapeDensityTimer_->stop();
    canvasWindow_->setAdjustmentBypassLayer({}); canvasWindow_->setFilterBypassLayer({}); canvasWindow_->setEffectBypassLayer({});
    adjustmentsPanel_->setTarget(nullptr, false); // Clear ID-only histogram/page caches before rebinding.
    return true;
}
bool MainWindow::activateDocument(DocumentInstanceId id)
{
    if (activeDocumentId() == id) return true;
    auto found = std::ranges::find_if(documents_, [id](const auto& context) { return context->id == id; });
    if (found == documents_.end() || switchingDocument_ || !settleForDocumentSwitch()) return false;
    const QScopedValueRollback switching(switchingDocument_, true);
    captureDocumentView();
    auto colors = session().colors();
    auto tool = session().activeTool();
    auto source = session().colorSampleSource();
    activeDocument_ = *found;
    activeDocument_->lastActivated = ++documentActivationClock_;
    session().setColors(colors); session().setActiveTool(tool); session().setColorSampleSource(source);
    cloneAnchor_ = activeDocument_->cloneAnchor;
    measureBoundsRevision_ = 0; measureBounds_.reset();
    shapeHitCache_.clear(); shapeHitDocument_ = nullptr; shapeViewScale_ = 0;
    textController_->setSession(session());
    {
        const QSignalBlocker guard(layerList_->selectionModel());
        layerModel_->setSession(&session());
        layerModel_->restoreCollapsedFolderIds(activeDocument_->collapsedFolders);
    }
    if (pixelPreview_) pixelPreview_->setDocumentInstance(activeDocumentId());
    // Restore the target view before preparing density-dependent text/shape
    // caches. The old tab's zoom must not force a cold rerasterization.
    canvasWindow_->setDocument(session().document()->snapshot(), !activeDocument_->viewInitialized);
    if (activeDocument_->viewInitialized)
        canvasWindow_->restoreDocumentView(activeDocument_->view, activeDocument_->measurement);
    synchronizeUi(true, false);
    refreshSpotHealOwnership();
    canvasWindow_->dismissLayerOutlines();
    refreshDocumentTabs(); updateDocumentResources();
    return true;
}
bool MainWindow::initializeDocument(std::unique_ptr<core::Document> document, QString name, QString path, QJsonObject metadata, QString source)
{
    if (!document || !settleForDocumentSwitch()) return false;
    auto next = std::make_shared<DocumentContext>();
    next->session.replaceDocument(std::move(document));
    next->displayName = std::move(name); next->projectPath = std::move(path);
    next->sourcePath = std::move(source); next->metadata = std::move(metadata);
    const auto collapsed = next->metadata["ui"].toObject()["layers"].toObject()["collapsedFolders"].toArray();
    for (const auto& value : collapsed) {
        bool valid = false; const auto id = value.toString().toULongLong(&valid);
        if (valid && id) next->collapsedFolders.push_back(id);
    }
    return publishDocuments({std::move(next)});
}
bool MainWindow::publishDocuments(std::vector<std::shared_ptr<DocumentContext>> staged)
{
    if (staged.empty() || !settleForDocumentSwitch()) return false;
    for (const auto& context : staged)
        if (!context || context->closed || !context->session.document()) return false;
    // Allocate the complete tab set before publishing any of it. Inactive tabs
    // retain source pixels only; activation alone prepares display resources.
    documents_.reserve(documents_.size() + staged.size());
    // Only the untouched launch placeholder may be retired without a close
    // decision. Explicit New canvases and every opened document remain tabs.
    auto placeholder = activeDocument_ && activeDocument_->untouched && !session().document()->isModified() ? activeDocument_ : nullptr;
    const auto oldCount = documents_.size();
    documents_.insert(documents_.end(), staged.begin(), staged.end());
    if (!activateDocument(staged.front()->id)) { documents_.resize(oldCount); return false; }
    if (placeholder) {
        placeholder->closed = true;
        textController_->forgetSession(placeholder->session);
        if (pixelPreview_) pixelPreview_->forgetDocument(placeholder->id);
        std::erase(documents_, placeholder);
    }
    refreshDocumentTabs(); updateDocumentResources();
    return true;
}
void MainWindow::showEmptyWorkspace()
{
    emptyDocument_.session.setColors(session().colors());
    emptyDocument_.session.setActiveTool(session().activeTool());
    emptyDocument_.session.setColorSampleSource(session().colorSampleSource());
    activeDocument_.reset(); cloneAnchor_.reset();
    if (pixelPreview_) pixelPreview_->setDocumentInstance(0);
    textController_->setSession(session()); layerModel_->setSession(&session());
    canvasWindow_->setDocument({}, false);
    synchronizeUi(true, false); refreshDocumentTabs(); updateDocumentResources();
}
bool MainWindow::closeDocument(DocumentInstanceId id)
{
    if (fileBusy_ || workspaceDialog_) return false;
    auto it = std::ranges::find_if(documents_, [id](const auto& context) { return context->id == id; });
    if (it == documents_.end()) return false;
    auto closing = *it;
    cancelDocumentRepair(id, true);
    const auto previous = activeDocumentId();
    const auto index = std::size_t(it - documents_.begin());
    if (closing == activeDocument_ || closing->session.document()->isModified()) {
        if (!activateDocument(id) || !settleForDocumentSwitch() || !guardUnsavedChanges()) {
            if (previous != id) activateDocument(previous);
            return false;
        }
    }
    if (closing == activeDocument_) {
        if (documents_.size() > 1) {
            auto next = documents_[index + 1 < documents_.size() ? index + 1 : index - 1];
            if (!activateDocument(next->id)) return false;
        } else showEmptyWorkspace();
    }
    closing->closed = true; ++closing->cancellationGeneration;
    cancelDocumentRepair(id);
    textController_->forgetSession(closing->session);
    if (pixelPreview_) pixelPreview_->forgetDocument(id);
    std::erase(documents_, closing);
    refreshDocumentTabs(); updateDocumentResources();
    canvasWindow_->scheduleFrame(); // Retire closed-document GPU caches on the existing frame-safe path.
    if (documents_.empty()) QTimer::singleShot(0, this, [this] {
        if (documents_.empty() && isVisible() && !workspaceDialog_) showStartupDocument();
    });
    else if (previous != id) activateDocument(previous);
    return true;
}
bool MainWindow::guardAllDocuments()
{
    if (!settleForDocumentSwitch()) return false;
    const auto previous = activeDocumentId();
    const auto owners = documents_; // Save dialogs may process nested UI events.
    for (const auto& context : owners) {
        if (!context->session.document()->isModified()) continue;
        if (!activateDocument(context->id) || !guardUnsavedChanges()) {
            activateDocument(previous); return false;
        }
    }
    activateDocument(previous);
    return true; // No document has been destroyed, including Discard choices.
}
MainWindow::DocumentMemory MainWindow::documentMemory() const
{
    DocumentMemory result;
    std::unordered_set<core::SurfaceId> sources, derived;
    std::unordered_set<const core::SelectionMask*> masks;
    const auto bytes=[](const auto& s){return s ? std::uint64_t(s->extent().width)*s->extent().height*4 : 0;};
    for(const auto& context:documents_) {
        result.historyBytes+=context->session.history().memoryUsed();
        for(const auto& layer:context->session.document()->layers()) {
            if(layer.mask&&masks.insert(layer.mask->coverage.get()).second)result.sourceBytes+=layer.mask->coverage->memoryCost();
            if(const auto* raster=std::get_if<core::RasterLayer>(&layer.payload);raster&&raster->surface&&sources.insert(raster->surface->id()).second)
                result.sourceBytes+=bytes(raster->surface);
            for(const auto& surface:{layer.renderCache?layer.renderCache->surface:nullptr,layer.filterCache?layer.filterCache->surface:nullptr})
                if(surface&&derived.insert(surface->id()).second)result.derivedBytes+=bytes(surface);
            if(layer.effectCache)result.derivedBytes+=layer.effectCache->bevelMemoryCost();
            if(layer.effectCache)for(const auto& mask:layer.effectCache->masks)
                if(mask&&mask->coverage&&masks.insert(mask->coverage.get()).second)result.derivedBytes+=mask->coverage->memoryCost();
        }
    }
    return result;
}
void MainWindow::updateDocumentResources()
{
    // Aggregate disposable CPU budget. Authoritative pixels and undo remain
    // owned by their sessions regardless of cache pressure.
    constexpr auto budget=512ULL*1024*1024;
    auto candidates=documents_;
    std::ranges::sort(candidates,{},&DocumentContext::lastActivated);
    auto memory=documentMemory();
    for(const auto& context:candidates) {
        if(memory.derivedBytes<=budget)break;
        if(context==activeDocument_)continue;
        for(const auto& item:context->session.document()->layers()) {
            auto* layer=context->session.document()->layer(item.id);
            layer->renderCache.reset();layer->filterCache.reset();layer->effectCache.reset();
        }
        textController_->forgetSession(context->session);
        memory=documentMemory();
    }
    std::vector<std::weak_ptr<const core::RasterSurface>> sources;
    for (const auto& context : documents_) {
        for (const auto& layer : context->session.document()->layers()) {
            if (const auto source = core::intrinsicSurface(layer)) sources.push_back(source);
            if (const auto source = core::renderedSurface(layer)) sources.push_back(source);
        }
    }
    canvasWindow_->setDocumentResources(activeDocumentId(), std::move(sources));
}
} // namespace imageeditor::ui
