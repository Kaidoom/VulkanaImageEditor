#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/core/LayerPixelCleanup.hpp"
#include "imageeditor/ui/Localization.hpp"
#include "imageeditor/ui/ToolIcons.hpp"
#include "imageeditor/ui/WorkspaceDialog.hpp"

#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QProgressBar>
#include <QPushButton>
#include <QScopeGuard>
#include <QScopedValueRollback>
#include <QSpinBox>
#include <QStatusBar>
#include <QVBoxLayout>

namespace imageeditor::ui {
namespace {
std::optional<int> chooseAlphaThreshold(WorkspaceDialog& presenter, const i18n::Message& title)
{
    QDialog card(&presenter, Qt::SubWindow | Qt::FramelessWindowHint);
    card.setObjectName("CleanTransparencyDialog");
    i18n::setWindowTitle(&card, title);
    auto* layout = new QVBoxLayout(&card);
    const auto prompt = i18n::text("layers.cleanup.threshold");
    auto* label = i18n::make<QLabel>(prompt, &card);
    layout->addWidget(label);
    auto* row = new QHBoxLayout;
    row->setSpacing(4);
    layout->addLayout(row);

    auto* value = new QSpinBox(&card);
    value->setRange(0, 255);
    value->setValue(core::defaultCleanupAlpha);
    value->setAlignment(Qt::AlignCenter);
    value->setButtonSymbols(QAbstractSpinBox::NoButtons);
    value->setStyleSheet(QStringLiteral("QSpinBox { padding: 0 6px; }"));
    value->setFixedWidth(std::max(56, value->sizeHint().width()));
    label->setBuddy(value);
    row->addWidget(value);

    const auto addStep = [&](const QString& symbol, const i18n::Message& name, auto step) {
        auto* button = new QPushButton(symbol, &card);
        button->setAutoDefault(false);
        button->setAutoRepeat(true);
        button->setStyleSheet(QStringLiteral("QPushButton { padding: 0; }"));
        i18n::setAccessibleName(button, name);
        i18n::setToolTip(button, name);
        QObject::connect(button, &QPushButton::clicked, value, step);
        row->addWidget(button);
        return button;
    };
    auto* decrease = addStep(QStringLiteral("−"), i18n::text("layers.cleanup.decrease.threshold"), &QSpinBox::stepDown);
    auto* increase = addStep(QStringLiteral("+"), i18n::text("layers.cleanup.increase.threshold"), &QSpinBox::stepUp);
    decrease->setObjectName("DecreaseCleanupThreshold");
    increase->setObjectName("IncreaseCleanupThreshold");
    const auto updateSteps = [=](int number) {
        decrease->setEnabled(number > value->minimum());
        increase->setEnabled(number < value->maximum());
    };
    QObject::connect(value, &QSpinBox::valueChanged, &card, updateSteps);
    updateSteps(value->value());
    auto* actions = new i18n::ButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &card);
    const int height = actions->sizeHint().height();
    value->setFixedHeight(height);
    decrease->setFixedSize(height, height);
    increase->setFixedSize(height, height);
    row->addSpacing(8);
    row->addStretch();
    row->addWidget(actions);
    QObject::connect(actions, &QDialogButtonBox::accepted, &card, &QDialog::accept);
    QObject::connect(actions, &QDialogButtonBox::rejected, &card, &QDialog::reject);
    if (presenter.exec(card) != QDialog::Accepted) return std::nullopt;
    value->interpretText();
    return value->value();
}
}

void MainWindow::createLayerPixelActions(QMenu* menu)
{
    menu->addSeparator();
    cleanTransparencyAction_ = i18n::addAction(menu, toolGlyph(ToolGlyph::OpacityMode),
        i18n::text("layers.cleanup.transparency") + QStringLiteral("…"), this,
        [this] { cleanLayerPixels(false); });
    cleanTransparencyAction_->setObjectName("CleanTransparencyAction");
    deleteInvertedAction_
        = i18n::addAction(menu, toolGlyph(ToolGlyph::Invert), i18n::text("layers.cleanup.delete.inverted"), this,
            [this] { startFill(false, true, { }, PixelErase::Inverted); });
    deleteInvertedAction_->setObjectName("DeleteInvertedAction");
    trimToCanvasAction_ = i18n::addAction(menu, toolGlyph(ToolGlyph::Crop), i18n::text("layers.cleanup.trim"),
        this, [this] { cleanLayerPixels(true); });
    trimToCanvasAction_->setObjectName("TrimLayersToCanvasAction");
    menu->addSeparator();
    connect(menu, &QMenu::aboutToShow, this, &MainWindow::refreshLayerPixelActions);
    refreshLayerPixelActions();
}

void MainWindow::refreshLayerPixelActions()
{
    if (!trimToCanvasAction_)
        return;
    const auto* doc = session().document();
    const bool ready = doc && !fileBusy_ && !workspaceDialog_ && !backgroundRemoval_ && !layerTransform_
        && !selectionTransform_ && !canvasResize_ && shortcutGestureIdle(true);
    const bool writableRasters
        = ready && !session().editingLayerMask() && core::canCleanLayerPixels(*doc, session().selectedLayers());
    cleanTransparencyAction_->setEnabled(writableRasters);
    trimToCanvasAction_->setEnabled(writableRasters);
    const auto* layer = ready && session().activeLayer() ? doc->layer(*session().activeLayer()) : nullptr;
    const auto* raster = layer ? std::get_if<core::RasterLayer>(&layer->payload) : nullptr;
    deleteInvertedAction_->setEnabled(layer && !layer->mirror && doc->selection()
        && !doc->selection()->bounds().empty()
        && (session().editingLayerMask()
            || (raster && raster->surface && raster->surface->dataKind() != core::RasterDataKind::PackedData)));
}

void MainWindow::cleanLayerPixels(bool trimToCanvas)
try {
    if (fileBusy_ || workspaceDialog_ || !activeDocument_ || !settleForFileOperation())
        return;
    const auto owner = activeDocument_;
    auto& doc = *session().document();
    const auto selected = session().layerSelectionState();
    if (session().editingLayerMask() || !core::canCleanLayerPixels(doc, selected.ids))
        return;
    int threshold = core::defaultCleanupAlpha;
    const auto title
        = trimToCanvas ? i18n::text("layers.cleanup.trim") : i18n::text("layers.cleanup.transparency");
    if (!trimToCanvas) {
        WorkspaceDialog presenter(*workspace_, *this);
        QScopedValueRollback modal(workspaceDialog_, &presenter);
        const auto chosen = chooseAlphaThreshold(presenter, title);
        if (!chosen) return;
        threshold = *chosen;
    }
    if (owner != activeDocument_ || owner->closed)
        return;
    std::unique_ptr<core::Command> command;
    {
        WorkspaceDialog presenter(*workspace_, *this);
        QDialog card(&presenter, Qt::SubWindow | Qt::FramelessWindowHint);
        auto* layout = new QVBoxLayout(&card);
        layout->addWidget(i18n::make<QLabel>(title, &card));
        auto* bar = new QProgressBar(&card);
        bar->setRange(0, 0);
        layout->addWidget(bar);
        auto* buttons = new i18n::ButtonBox(QDialogButtonBox::Cancel, &card);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::rejected, &card, &QDialog::reject);
        bool cancelled = false;
        connect(&card, &QDialog::rejected, &card, [&] { cancelled = true; });
        QScopedValueRollback busy(fileBusy_, true);
        QScopedValueRollback modal(workspaceDialog_, &presenter);
        QScopedValueRollback<QWidget*> progress(fileProgress_, &card);
        QElapsedTimer elapsed, cadence;
        elapsed.start();
        cadence.start();
        const auto finish = qScopeGuard([&] { presenter.finish(); });
        command = core::makeLayerPixelCleanupCommand(doc, selected,
            trimToCanvas ? core::LayerPixelCleanup::TrimToCanvas : core::LayerPixelCleanup::RemoveLowAlpha,
            std::uint8_t(threshold), [&] {
                if (elapsed.elapsed() >= 250 && cadence.elapsed() >= 16) {
                    if (!presenter.active())
                        presenter.open(card);
                    QApplication::processEvents(QEventLoop::AllEvents, 5);
                    cadence.restart();
                }
                return !cancelled;
            });
        if (cancelled)
            return;
    }
    if (!command) {
        statusBar()->showMessage(i18n::text("layers.cleanup.no.pixels"), 2500);
        return;
    }
    if (!session().execute(std::move(command)))
        throw std::runtime_error("Could not apply layer cleanup");
    fileState().untouched = false;
    synchronizeUi(false, false);
} catch (const std::exception& error) {
    statusBar()->showMessage(QString::fromUtf8(error.what()), 5000);
}
} // namespace imageeditor::ui
