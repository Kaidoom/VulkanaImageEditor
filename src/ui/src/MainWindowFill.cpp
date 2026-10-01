#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/CompactValueControl.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/ToolIcons.hpp"
#include "imageeditor/ui/ToolOptionsBar.hpp"
#include "imageeditor/ui/ToolOptionsButton.hpp"
#include <QAction>
#include <QButtonGroup>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QStatusBar>
#include <QTimer>

namespace imageeditor::ui {
void MainWindow::createFillActions()
{
    for (int i = 0; i < 2; ++i) {
        auto* action = new QAction(
            i ? QStringLiteral("Fill with Background") : QStringLiteral("Fill with Foreground"), this);
        action->setObjectName(
            i ? QStringLiteral("FillBackgroundAction") : QStringLiteral("FillForegroundAction"));
        action->setShortcut(
            QKeySequence(i ? QStringLiteral("Ctrl+Backspace") : QStringLiteral("Alt+Backspace")));
        action->setAutoRepeat(false);
        registerEditorWindowAction(action);
        fillActions_[std::size_t(i)] = action;
        connect(action, &QAction::triggered, this, [this, i] { startFill(i != 0, true); });
    }
}
void MainWindow::createFillControls()
{
    auto* page = new QWidget;
    page->setObjectName(QStringLiteral("FillOptionsPage"));
    auto* row = new QHBoxLayout(page);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);
    row->addStretch();
    row->addWidget(new QLabel(QStringLiteral("Fill mode"), page));
    const auto toggle = [page, row](const QString& name, const QString& text, const QString& hint,
                            ToolGlyph glyph) {
        auto* b = new ToolOptionsButton(name, text, hint, ToolOptionsButton::Kind::Toggle, page,
            ToolOptionsButton::Presentation::Icon);
        b->setIcon(toolGlyph(glyph));
        row->addWidget(b);
        return b;
    };
    auto* modes = new QButtonGroup(page);
    auto* selection = toggle(QStringLiteral("FillModeSelection"), QStringLiteral("Selection / Layer"),
        QStringLiteral(
            "Selection / Layer — click to fill all selected coverage, or the layer inside the canvas"),
        ToolGlyph::FillSelection);
    auto* contiguous = toggle(QStringLiteral("FillModeContiguous"), QStringLiteral("Contiguous"),
        QStringLiteral("Contiguous — click to fill a connected region on the active layer"),
        ToolGlyph::FillContiguous);
    modes->addButton(selection, 0);
    modes->addButton(contiguous, 1);
    selection->setChecked(true);
    const auto number
        = [page, row](const QString& name, const QString& label, double maximum, double value) {
              auto* n = new CompactValueControl(page);
              n->setObjectName(name);
              n->setRange(0, maximum);
              n->setDecimals(0);
              n->setSingleStep(1);
              n->setValue(value);
              n->setPrefix(label + QStringLiteral(": "));
              n->setAccessibleName(label);
              n->setFixedSize(144, 30);
              row->addWidget(n);
              return n;
          };
    auto* opacity = number(QStringLiteral("FillOpacityControl"), QStringLiteral("Opacity"), 100, 100);
    opacity->setSuffix(QStringLiteral("%"));
    opacity->setToolTip(
        QStringLiteral("Fill opacity, combined once with color alpha and selection coverage"));
    connect(opacity, &QDoubleSpinBox::valueChanged, this,
        [this](double value) { fillOptions_.opacity = value / 100; });
    auto* tolerance = number(QStringLiteral("FillToleranceControl"), QStringLiteral("Tolerance"), 255, 16);
    tolerance->setToolTip(
        QStringLiteral("Maximum seed-relative premultiplied RGBA difference · 0 exact · 255 all colors"));
    // Keep the cached control in its fixed slot in both modes. Disabling only
    // input preserves the centered group's geometry and the user's last value.
    tolerance->setEnabled(false);
    connect(tolerance, &QDoubleSpinBox::valueChanged, this,
        [this](double value) { fillOptions_.tolerance = std::uint8_t(value); });
    connect(modes, &QButtonGroup::idClicked, this, [this, tolerance](int id) {
        fillOptions_.mode = id ? core::FillMode::Contiguous : core::FillMode::SelectionOrLayer;
        tolerance->setEnabled(id == 1);
        updateActionState();
    });
    row->addStretch();
    toolOptionsBar_->registerToolPage(core::ToolId::Fill, QStringLiteral("Fill"), page);
    fillTimer_ = new QTimer(this);
    fillTimer_->setInterval(1);
    fillTimer_->setSingleShot(true);
    connect(fillTimer_, &QTimer::timeout, this, &MainWindow::advanceFill);
    canvasWindow_->onFillRequested
        = [this](core::Vec2d point) { startFill(false, false, point); };
}
void MainWindow::createEyedropperControls()
{
    auto* page = new QWidget;
    page->setObjectName(QStringLiteral("EyedropperOptionsPage"));
    auto* row = new QHBoxLayout(page);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);
    row->addStretch();
    auto* group = new QButtonGroup(page);
    for (int i = 0; i < 2; ++i) {
        auto* b = new ToolOptionsButton(
            i ? QStringLiteral("SampleActiveLayer") : QStringLiteral("SampleMergedVisible"),
            i ? QStringLiteral("Active Layer") : QStringLiteral("Merged Visible"),
            i ? QStringLiteral("Active Layer — sample its pixels without visibility or layer opacity")
              : QStringLiteral("Merged Visible — sample the visible composited document"),
            ToolOptionsButton::Kind::Toggle, page, ToolOptionsButton::Presentation::Icon);
        b->setIcon(toolGlyph(i ? ToolGlyph::ActiveLayer : ToolGlyph::MergedVisible));
        b->setChecked(i == 0);
        group->addButton(b, i);
        row->addWidget(b);
    }
    connect(group, &QButtonGroup::idClicked, this, [this](int id) {
        session().setColorSampleSource(
            id ? core::ColorSampleSource::ActiveLayer : core::ColorSampleSource::MergedVisible);
        canvasWindow_->refreshColorSample();
    });
    row->addStretch();
    toolOptionsBar_->registerToolPage(core::ToolId::Eyedropper, QStringLiteral("Eyedropper"), page);
}
void MainWindow::startFill(bool background, bool selectionOnly, core::Vec2d seed, bool eraseSelection)
{
    if (activeFill_ || !session().document() || !session().activeLayer() || layerTransform_
        || selectionTransform_)
        return;
    const auto* target = session().document()->layer(*session().activeLayer());
    if (!session().editingLayerMask()&&eraseSelection && (!target
        || !std::holds_alternative<core::RasterLayer>(target->payload))) return;
    cancelPendingEdits();
    auto options = fillOptions_;
    options.color = background ? session().colors().background() : session().colors().foreground();
    if (selectionOnly)
        options.mode = core::FillMode::SelectionOrLayer;
    options.seed = seed;
    options.eraseSelection = eraseSelection;
    const bool mask=session().editingLayerMask();options.coverageValues=mask;
    if(mask) {
        try {activeMaskEdit_=std::make_unique<core::LayerMaskEdit>(*session().document(),*session().activeLayer(),"Fill layer mask");}
        catch(const std::exception& e){statusBar()->showMessage(QString::fromUtf8(e.what()),4500);return;}
    }
    activeFill_
        = std::make_unique<core::FillOperation>(mask?activeMaskEdit_->proxy():*session().document(), *session().activeLayer(), options);
    if (!fillProgress_) {
        fillProgress_ = new QWidget(this);
        fillProgress_->setObjectName(QStringLiteral("FillProgress"));
        auto* row = new QHBoxLayout(fillProgress_);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(6);
        row->addWidget(new QLabel(QStringLiteral("Updating pixels…"), fillProgress_));
        auto* progress = new QProgressBar(fillProgress_);
        progress->setRange(0, 0);
        progress->setFixedSize(80, 10);
        row->addWidget(progress);
        auto* cancel = new ToolOptionsButton(QStringLiteral("FillCancel"), QStringLiteral("Cancel"),
            QStringLiteral("Cancel fill and restore original pixels · Escape"),
            ToolOptionsButton::Kind::Action, fillProgress_, ToolOptionsButton::Presentation::Status);
        row->addWidget(cancel);
        connect(cancel, &QToolButton::clicked, this, &MainWindow::cancelFill);
        statusBar()->addPermanentWidget(fillProgress_);
    }
    fillProgress_->show();
    updateActionState();
    fillTimer_->start();
}
void MainWindow::advanceFill()
{
    if (!activeFill_)
        return;
    const auto before = activeFill_->stats().writeBatches;
    // <=64K pixel chunks. Surface updates remain on its owning GUI thread;
    // between chunks the event loop can present frames, move panels or cancel.
    auto state = activeFill_->step();
    if (activeFill_->stats().writeBatches != before) {
        if(activeMaskEdit_)canvasWindow_->setDocument(session().document()->snapshot(),false);
        canvasWindow_->scheduleFrame();
    }
    if (state == core::FillState::Discovering || state == core::FillState::Applying) {
        fillTimer_->start();
        return;
    }
    auto result = core::RasterEditCommitResult::NoChanges;
    if (state == core::FillState::Ready)
        result = activeFill_->commit(activeMaskEdit_?activeMaskEdit_->provisionalHistory():session().history());
    const auto error = QString::fromUtf8(activeFill_->error());
    const auto label = QString::fromUtf8(activeFill_->label());
    activeFill_.reset();
    if(activeMaskEdit_) {
        if(result==core::RasterEditCommitResult::Committed||result==core::RasterEditCommitResult::NoChanges)
            result=activeMaskEdit_->commit(session().history());
        activeMaskEdit_.reset();
    }
    fillProgress_->hide();
    if (result == core::RasterEditCommitResult::Committed)
        fileState().untouched = false;
    synchronizeUi(false, false);
    statusBar()->showMessage(!error.isEmpty() ? QStringLiteral("%1 failed: %2").arg(label, error)
            : result == core::RasterEditCommitResult::Committed ? QStringLiteral("%1 applied").arg(label)
                                                                : QStringLiteral("%1 made no changes").arg(label),
        3500);
}
void MainWindow::cancelFill()
{
    if (!activeFill_)
        return;
    fillTimer_->stop();
    activeFill_->cancel();
    activeFill_.reset();
    if(activeMaskEdit_) {activeMaskEdit_.reset();canvasWindow_->setDocument(session().document()->snapshot(),false);}
    if (fillProgress_)
        fillProgress_->hide();
    canvasWindow_->scheduleFrame();
    updateActionState();
    statusBar()->showMessage(QStringLiteral("Pixel edit cancelled"), 2500);
}
}
