#include "imageeditor/ui/BackgroundRemoval.hpp"
#include "imageeditor/ui/BackgroundRemovalMask.hpp"
#include "imageeditor/ui/BackgroundRemovalPanel.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/OverlayDockWorkspace.hpp"
#include "imageeditor/ui/PixelPreview.hpp"
#include "imageeditor/ui/PopupOwnership.hpp"
#include "imageeditor/ui/CrossWindowPointerRouter.hpp"
#include "imageeditor/ui/ToolOptionsBar.hpp"
#include "imageeditor/ui/Localization.hpp"
#include "imageeditor/core/LayerMaskEdit.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QMenu>
#include <QMenuBar>
#include <QMouseEvent>
#include <QScopedValueRollback>
#include <QStatusBar>
#include <QTabBar>
#include <QTimer>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <utility>

namespace imageeditor::ui {
namespace {
class BackgroundMaskCommand final : public core::Command {
public:
    BackgroundMaskCommand(core::LayerId id, core::LayerMaskState before, core::LayerMaskState after,
        core::LayerSelectionState selection, bool editing)
        : command_(id, std::move(before), std::move(after), "Remove Background")
        , selection_(std::move(selection)), editing_(editing) {}
    bool apply(core::Document& doc) override { return command_.apply(doc); }
    bool undo(core::Document& doc) override { return command_.undo(doc); }
    std::string_view label() const noexcept override { return "Remove Background"; }
    std::size_t memoryCost() const noexcept override {
        return sizeof(*this) + command_.memoryCost() + selection_.ids.capacity() * sizeof(core::LayerId);
    }
    const core::LayerSelectionState* layerSelectionAfter(bool) const noexcept override { return &selection_; }
    std::optional<bool> maskEditingAfter(bool undo) const noexcept override { return undo ? editing_ : true; }
private:
    core::LayerMaskCommand command_;
    core::LayerSelectionState selection_;
    bool editing_;
};
}

struct BackgroundRemovalWorkspace {
    std::shared_ptr<DocumentContext> context;
    core::Document* owner{};
    core::Revision revision{}, selectionRevision{};
    core::LayerId target{};
    core::LayerSnapshot originalLayer;
    core::LayerSelectionState layerSelection;
    bool oldMaskEditing{}, oldPanelsSuppressed{}, oldOutlines{}, held{}, dirty{}, ready{}, spacePan{};
    core::Rgba8 background, light, dark;
    std::vector<std::pair<QPointer<QWidget>, bool>> disabledWidgets;
    std::vector<std::pair<QPointer<QAction>, bool>> disabledActions;
    BackgroundRemovalPanel* panel{};
    QTimer* timer{};
    std::unique_ptr<BackgroundRemovalSourceBuilder> builder;
    BackgroundRemovalSource source;
    std::shared_ptr<BackgroundRemovalEngine> engine;
    QString requestedModel;
    std::uint64_t generation{}, runningGeneration{};
    std::shared_ptr<std::atomic_bool> cancel;
    QElapsedTimer preparation;
    double preparationMs{};
    struct Job {
        BackgroundRemovalResult result;
        core::LayerMaskState mask;
        std::shared_ptr<const core::RasterSurface> grayscale;
    };
    Job result;
    std::future<Job> worker;
    ~BackgroundRemovalWorkspace() {
        if (cancel) *cancel = true;
        if (worker.valid()) worker.wait();
    }
    bool current(const MainWindow& window) const {
        const auto* doc = window.editorSession().document();
        if (window.activeDocumentId() != context->id || doc != owner || !doc
            || doc->revision() != revision || doc->selectionRevision() != selectionRevision)
            return false;
        const auto* layer = doc->layer(target);
        const auto* raster = layer ? std::get_if<core::RasterLayer>(&layer->payload) : nullptr;
        return raster && raster->surface && (source.rgba.isNull()
            || (raster->surface->id() == source.sourceId && raster->surface->revision() == source.sourceRevision));
    }
};

void MainWindow::createBackgroundRemovalAction(QMenu* menu)
{
    removeBackgroundAction_ = i18n::addAction(menu, i18n::text("background.removal.action"));
    removeBackgroundAction_->setIcon(refineMaskAction_->icon());
    removeBackgroundAction_->setObjectName(QStringLiteral("RemoveBackgroundAction"));
    connect(removeBackgroundAction_, &QAction::triggered, this, &MainWindow::beginBackgroundRemoval);
    connect(menu, &QMenu::aboutToShow, this, &MainWindow::refreshBackgroundRemovalAction);
    refreshBackgroundRemovalAction();
}

void MainWindow::refreshBackgroundRemovalAction()
{
    if (!removeBackgroundAction_) return;
    const auto* doc = session().document();
    const auto* layer = doc && session().activeLayer() ? doc->layer(*session().activeLayer()) : nullptr;
    const auto* raster = layer ? std::get_if<core::RasterLayer>(&layer->payload) : nullptr;
    const bool compatible = raster && raster->surface && (!layer->mask || layer->mask->enabled);
    removeBackgroundAction_->setEnabled(compatible && session().selectedLayers().size() == 1
        && !fileBusy_ && !workspaceDialog_ && !refinement_ && !backgroundRemoval_
        && retiredBackgroundRemoval_.empty() && !canvasResize_ && !layerTransform_ && !selectionTransform_
        && shortcutGestureIdle(true));
    i18n::setToolTip(removeBackgroundAction_, i18n::text(layer && layer->mask && !layer->mask->enabled
        ? "background.removal.mask.disabled" : "background.removal.help"));
}

void MainWindow::beginBackgroundRemoval()
{
    refreshBackgroundRemovalAction();
    if (!removeBackgroundAction_ || !removeBackgroundAction_->isEnabled() || !settleForFileOperation()) return;
    try {
        const auto* layer = session().document()->layer(*session().activeLayer());
        auto state = std::make_shared<BackgroundRemovalWorkspace>();
        state->context = activeDocument_;
        state->owner = session().document();
        state->revision = state->owner->revision();
        state->selectionRevision = state->owner->selectionRevision();
        state->target = layer->id;
        for (const auto& snapshot : state->owner->snapshot().layersBottomToTop)
            if (snapshot.id == layer->id) state->originalLayer = snapshot;
        state->layerSelection = session().layerSelectionState();
        state->oldMaskEditing = session().editingLayerMask();
        state->oldPanelsSuppressed = workspace_->panelsSuppressed();
        state->oldOutlines = canvasWindow_->layerOutlinesVisible();
        state->background = canvasWindow_->scene().canvasBackground;
        state->light = canvasWindow_->scene().checkerLight;
        state->dark = canvasWindow_->scene().checkerDark;
        state->preparation.start();
        state->builder = std::make_unique<BackgroundRemovalSourceBuilder>(*layer);
        state->engine = std::make_shared<BackgroundRemovalEngine>();
        state->panel = new BackgroundRemovalPanel(workspace_->panelOverlay());
        state->panel->setTarget(QString::fromStdString(layer->name), bool(layer->mask));
        state->panel->setModels(BackgroundRemovalEngine::models());
        state->timer = new QTimer(state->panel);
        state->timer->setSingleShot(true);
        backgroundRemoval_ = state;
        // This is an edit barrier, not a blocking operation. Painting, native
        // window management, panel input, zoom and middle-button pan stay live.
        fileBusy_ = true;
        pointerRouter_->cancelCapture();
        const auto disable = [&](QWidget* widget) {
            if (!widget || std::any_of(state->disabledWidgets.begin(), state->disabledWidgets.end(),
                    [widget](const auto& item) { return item.first == widget; })) return;
            state->disabledWidgets.push_back({widget, widget->isEnabled()});
            widget->setEnabled(false);
        };
        disable(documentTabs_);
        disable(toolOptionsBar_);
        for (auto* child : workspace_->panelOverlay()->findChildren<QWidget*>(QString{}, Qt::FindDirectChildrenOnly)) {
            if (child == state->panel) continue;
            disable(child);
        }
        for (auto* action : findChildren<QAction*>()) {
            state->disabledActions.push_back({action, action->isEnabled()});
            action->setEnabled(false);
        }
        workspace_->setPanelsSuppressed(true);
        canvasWindow_->setLayerOutlinesVisible(false);
        canvasWindow_->setPixelPreview(false);
        pixelPreview_->setEnabled(false);
        canvasWindow_->setActiveTool(core::ToolId::Move);
        const auto active = [this, weak = std::weak_ptr(state)] {
            const auto s = weak.lock();
            return s && backgroundRemoval_ == s;
        };
        state->panel->onModelChanged = [this, active](const QString&) { if (active()) requestBackgroundRemoval(); };
        state->panel->onRegenerate = [this, active] { if (active()) requestBackgroundRemoval(); };
        state->panel->onViewChanged = [this, active](auto) { if (active()) publishBackgroundRemovalPreview(); };
        state->panel->onOriginalChanged = [this, active](bool held) {
            if (active()) { backgroundRemoval_->held = held; publishBackgroundRemovalPreview(); }
        };
        state->panel->onApply = [this, active] { if (active()) finishBackgroundRemoval(true); };
        state->panel->onCancel = [this, active] { if (active()) finishBackgroundRemoval(false); };
        connect(state->timer, &QTimer::timeout, this, [this, active] { if (active()) advanceBackgroundRemoval(); });
        placeBackgroundRemovalPanel();
        state->panel->show();
        state->panel->raise();
        requestBackgroundRemoval();
        publishBackgroundRemovalPreview();
    } catch (const std::exception& error) {
        finishBackgroundRemoval(false);
        statusBar()->showMessage(i18n::text("background.removal.failed").arg(QString::fromUtf8(error.what())), 7000);
    }
}

void MainWindow::placeBackgroundRemovalPanel()
{
    if (!backgroundRemoval_) return;
    auto* panel = backgroundRemoval_->panel;
    auto area = workspace_->rulerContentRect().adjusted(8, 8, -8, -8);
    if (workspace_->rulerVisible(Qt::Horizontal)) {
        const auto ruler = workspace_->rulerStrip(Qt::Horizontal)->geometry();
        if (workspace_->rulerFarEdge(Qt::Horizontal)) area.setBottom(ruler.top() - 8);
        else area.setTop(ruler.bottom() + 8);
    }
    if (workspace_->rulerVisible(Qt::Vertical)) {
        const auto ruler = workspace_->rulerStrip(Qt::Vertical)->geometry();
        if (workspace_->rulerFarEdge(Qt::Vertical)) area.setRight(ruler.left() - 8);
        else area.setLeft(ruler.right() + 8);
    }
    const int width = std::clamp(area.width(), 1, 316);
    const int height = std::clamp(panel->sizeHint().height(), 1, std::max(1, area.height()));
    panel->setGeometry(area.right() - width + 1, area.top(), width, height);
    workspace_->setContextOverlayInteractionRegion(panel->geometry());
}

void MainWindow::requestBackgroundRemoval()
{
    if (!backgroundRemoval_) return;
    auto& state = *backgroundRemoval_;
    ++state.generation;
    state.requestedModel = state.panel->selectedModel();
    state.dirty = !state.requestedModel.isEmpty();
    state.ready = false;
    state.result = {};
    if (state.cancel) *state.cancel = true;
    if (state.dirty) {
        state.panel->setBusy(true, state.worker.valid()
            ? i18n::text("background.removal.cancelling.previous") : i18n::text("background.removal.preparing"));
        state.timer->start(0);
    }
    placeBackgroundRemovalPanel();
    publishBackgroundRemovalPreview();
}

void MainWindow::advanceBackgroundRemoval()
{
    if (!backgroundRemoval_) return;
    auto& state = *backgroundRemoval_;
    if (!state.current(*this)) { finishBackgroundRemoval(false); return; }
    if (state.worker.valid()) {
        if (state.worker.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
            state.timer->start(16);
            return;
        }
        auto result = state.worker.get();
        if (state.runningGeneration == state.generation && !result.result.cancelled) {
            state.result = std::move(result);
            state.ready = state.result.mask && state.result.result.error.isEmpty();
            if (state.ready) state.panel->setResult(state.result.result);
            else state.panel->setError(state.result.result.error.isEmpty()
                ? i18n::text("background.removal.empty") : state.result.result.error);
            placeBackgroundRemovalPanel();
            publishBackgroundRemovalPreview();
        }
    }
    if (!state.dirty) return;
    try {
        if (state.builder) {
            QElapsedTimer slice;
            slice.start();
            while (!state.builder->step(32)) {
                if (slice.elapsed() >= 4) { state.timer->start(0); return; }
            }
            state.source = state.builder->source();
            state.builder.reset();
            state.preparationMs = double(state.preparation.elapsed());
        }
        state.dirty = false;
        state.runningGeneration = state.generation;
        state.cancel = std::make_shared<std::atomic_bool>(false);
        state.panel->setBusy(true, i18n::text("background.removal.processing"));
        const auto source = state.source;
        const auto engine = state.engine;
        const auto model = state.requestedModel;
        const auto cancel = state.cancel;
        const auto preparationMs = std::exchange(state.preparationMs, 0.);
        state.worker = std::async(std::launch::async, [source, engine, model, cancel, preparationMs] {
            BackgroundRemovalWorkspace::Job job;
            QElapsedTimer total;
            total.start();
            try {
                job.result = engine->evaluate(model, source.rgba, *cancel);
                if (*cancel || job.result.cancelled || !job.result.error.isEmpty()) return job;
                job.mask = backgroundRemovalMask(source, job.result.mask, [cancel] { return bool(*cancel); });
                const auto image = job.result.mask;
                const core::Extent2u extent{std::uint32_t(image.width()), std::uint32_t(image.height())};
                std::vector<std::byte> pixels(std::size_t(extent.width) * extent.height * 4);
                for (int y = 0; y < image.height(); ++y) {
                    if (*cancel) { job.result.cancelled = true; return job; }
                    const auto* row = image.constScanLine(y);
                    auto* output = pixels.data() + std::size_t(y) * extent.width * 4;
                    for (int x = 0; x < image.width(); ++x) {
                        output[x * 4] = output[x * 4 + 1] = output[x * 4 + 2] = std::byte(row[x]);
                        output[x * 4 + 3] = std::byte{255};
                    }
                }
                job.grayscale = std::make_shared<core::ContiguousRasterSurface>(extent, std::move(pixels));
                job.result.details += QStringLiteral("\nLayer copy: %1 ms; preview total includes mask composition.")
                    .arg(preparationMs, 0, 'f', 1);
                job.result.timings.totalMs = preparationMs + double(total.elapsed());
            } catch (const std::exception& error) {
                job.result.cancelled = bool(*cancel);
                if (!*cancel) job.result.error = QString::fromUtf8(error.what());
            }
            return job;
        });
        state.timer->start(16);
    } catch (const std::exception& error) {
        state.dirty = false;
        state.panel->setError(QString::fromUtf8(error.what()));
        placeBackgroundRemovalPanel();
    }
}

void MainWindow::prepareBackgroundRemovalSnapshot(core::DocumentSnapshot& snapshot)
{
    if (!backgroundRemoval_ || !backgroundRemoval_->current(*this)) return;
    const auto& state = *backgroundRemoval_;
    // Isolate the raw raster so other layers/styles cannot conceal the mask.
    auto layer = state.originalLayer;
    layer.visible = true;
    layer.opacity = layer.fillOpacity = 1;
    layer.blendMode = core::BlendMode::Normal;
    layer.adjustments.reset();
    layer.filters.reset(); layer.filterCache.reset();
    layer.effects.reset(); layer.effectCache.reset();
    layer.renderCache.reset(); layer.rasterEffectFrame.reset();
    if (!state.held && state.ready) {
        if (state.panel->view() == BackgroundRemovalPanel::View::Mask) {
            layer.payload = core::RasterLayerSnapshot{state.result.grayscale};
            layer.mask.reset(); layer.crop.reset();
        } else layer.mask = state.result.mask;
    }
    snapshot.layersBottomToTop = {std::move(layer)};
    snapshot.tree = {};
    snapshot.tree.roots = {state.target};
    snapshot.selection.reset();
    snapshot.selectionRevision = 0;
}

void MainWindow::publishBackgroundRemovalPreview()
{
    if (!backgroundRemoval_) return;
    const auto& state = *backgroundRemoval_;
    const auto view = state.panel->view();
    const bool black = view == BackgroundRemovalPanel::View::Black || view == BackgroundRemovalPanel::View::Mask;
    const bool solid = black || view == BackgroundRemovalPanel::View::White;
    const core::Rgba8 color = black ? core::Rgba8{0, 0, 0, 255} : core::Rgba8{255, 255, 255, 255};
    canvasWindow_->setCanvasColors(state.background, solid ? color : state.light, solid ? color : state.dark);
    canvasWindow_->setDocument(session().document()->snapshot(), false);
}

void MainWindow::finishBackgroundRemoval(bool apply)
{
    if (!backgroundRemoval_) return;
    auto state = backgroundRemoval_;
    if (apply) {
        if (!state->current(*this) || !state->ready || state->dirty || state->worker.valid()) return;
        try {
            if (session().execute(std::make_unique<BackgroundMaskCommand>(state->target,
                    state->source.originalMask, state->result.mask, state->layerSelection, state->oldMaskEditing)))
                fileState().untouched = false;
        } catch (const std::exception& error) {
            state->panel->setError(QString::fromUtf8(error.what()));
            return;
        }
    }
    if (state->cancel) *state->cancel = true;
    state->timer->stop();
    backgroundRemoval_.reset(); // Hide/delete cannot publish Original callbacks.
    state->panel->hide();
    state->panel->deleteLater();
    fileBusy_ = false;
    keyboardPanelTarget_.clear();
    canvasContainer_->setFocusProxy(nullptr);
    pointerRouter_->cancelCapture();
    canvasWindow_->cancelPanInput();
    canvasWindow_->setSpacePanHeld(false);
    canvasWindow_->setCanvasColors(state->background, state->light, state->dark);
    canvasWindow_->setLayerOutlinesVisible(state->oldOutlines);
    canvasWindow_->setActiveTool(session().activeTool());
    workspace_->setContextOverlayInteractionRegion({});
    workspace_->setPanelsSuppressed(state->oldPanelsSuppressed);
    for (auto& [widget, enabled] : state->disabledWidgets) if (widget) widget->setEnabled(enabled);
    for (auto& [action, enabled] : state->disabledActions) if (action) action->setEnabled(enabled);
    pixelPreview_->setEnabled(pixelPreviewAction_->isChecked());
    canvasWindow_->setPixelPreview(pixelPreviewAction_->isChecked());
    retiredBackgroundRemoval_.push_back(std::move(state));
    drainBackgroundRemoval();
    synchronizeUi(true, false);
    if (apply) statusBar()->showMessage(i18n::text("background.removal.applied"), 4000);
}

void MainWindow::drainBackgroundRemoval()
{
    std::erase_if(retiredBackgroundRemoval_, [](const auto& state) {
        return !state->worker.valid() || state->worker.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
    });
    refreshBackgroundRemovalAction();
    if (retiredBackgroundRemoval_.empty()) return;
    if (!backgroundRemovalCleanupTimer_) {
        backgroundRemovalCleanupTimer_ = new QTimer(this);
        backgroundRemovalCleanupTimer_->setSingleShot(true);
        connect(backgroundRemovalCleanupTimer_, &QTimer::timeout, this, &MainWindow::drainBackgroundRemoval);
    }
    backgroundRemovalCleanupTimer_->start(30);
}

void MainWindow::shutdownBackgroundRemoval()
{
    finishBackgroundRemoval(false);
    if (backgroundRemovalCleanupTimer_) backgroundRemovalCleanupTimer_->stop();
    retiredBackgroundRemoval_.clear();
}

bool MainWindow::backgroundRemovalEvent(QObject* watched, QEvent* event)
{
    if (!backgroundRemoval_) return false;
    auto& state = *backgroundRemoval_;
    const auto* overlay = workspace_->panelOverlay();
    if (watched == overlay && event->type() == QEvent::Resize) {
        placeBackgroundRemovalPanel();
        QTimer::singleShot(0, this, &MainWindow::placeBackgroundRemovalPanel);
    }
    const bool releasePan = state.spacePan && event->type() == QEvent::KeyRelease
        && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Space
        && !static_cast<QKeyEvent*>(event)->isAutoRepeat();
    if (releasePan || event->type() == QEvent::ApplicationDeactivate
        || ((watched == this || watched == windowHandle())
            && (event->type() == QEvent::WindowDeactivate || event->type() == QEvent::Hide))) {
        state.spacePan = false;
        canvasWindow_->setSpacePanHeld(false);
        if (releasePan) { event->accept(); return true; }
    }
    const auto* widget = qobject_cast<QWidget*>(watched);
    const bool ours = watched == canvasWindow_ || watched == windowHandle() || watched == overlay->windowHandle()
        || (widget && (widget == this || isAncestorOf(widget) || overlay->isAncestorOf(widget)))
        || popupBelongsTo(watched, state.panel);
    if (!ours) return false;
    const auto type = event->type();
    const bool key = type == QEvent::KeyPress || type == QEvent::KeyRelease || type == QEvent::InputMethod;
    if (auto* popup = QApplication::activePopupWidget(); popup && popupBelongsTo(popup, state.panel)) {
        if (key && qobject_cast<QWindow*>(watched)) {
            auto* combo = qobject_cast<QComboBox*>(popupLogicalParent(popup));
            QCoreApplication::sendEvent(combo ? static_cast<QWidget*>(combo->view()) : popup, event);
            event->accept();
            return true;
        }
        if (popupBelongsTo(watched, state.panel)) return false;
    }
    if (routePanelKeyboard(watched, event)) return true;
    if (type == QEvent::ShortcutOverride) { event->accept(); return true; }
    if (popupBelongsTo(watched, state.panel)) {
        if (type == QEvent::KeyPress && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
            finishBackgroundRemoval(false); return true;
        }
        return false;
    }
    if (type == QEvent::KeyPress || type == QEvent::KeyRelease) {
        const auto* press = static_cast<QKeyEvent*>(event);
        if (press->key() == Qt::Key_Escape && type == QEvent::KeyPress) {
            finishBackgroundRemoval(false); event->accept(); return true;
        }
        if (press->key() == Qt::Key_Space && !press->isAutoRepeat()) {
            state.spacePan = type == QEvent::KeyPress;
            canvasWindow_->setSpacePanHeld(state.spacePan);
        }
        event->accept(); return true;
    }
    // Native QWidget carriers must dispatch to the actual panel child first.
    if (watched == windowHandle() || watched == overlay->windowHandle()
        || watched == canvasContainer_->windowHandle()) return false;
    if (watched == canvasWindow_) {
        if (type == QEvent::Wheel) return false;
        if (type == QEvent::MouseMove && canvasWindow_->panDragging()) return false;
        if (type == QEvent::MouseButtonPress || type == QEvent::MouseButtonRelease) {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::MiddleButton || (mouse->button() == Qt::LeftButton
                    && (state.spacePan || canvasWindow_->panDragging()))) return false;
        }
    }
    switch (type) {
    case QEvent::Shortcut: case QEvent::MouseButtonPress: case QEvent::MouseButtonRelease:
    case QEvent::MouseButtonDblClick: case QEvent::MouseMove: case QEvent::Wheel:
    case QEvent::TabletPress: case QEvent::TabletMove: case QEvent::TabletRelease:
    case QEvent::InputMethod: case QEvent::DragEnter: case QEvent::DragMove: case QEvent::Drop:
    case QEvent::ContextMenu: case QEvent::TouchBegin: case QEvent::TouchUpdate: case QEvent::TouchEnd:
        event->accept(); return true;
    default: return false;
    }
}
} // namespace imageeditor::ui
