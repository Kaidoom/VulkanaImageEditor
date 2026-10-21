#include "imageeditor/ui/BackgroundRemovalPanel.hpp"
#include "imageeditor/ui/Localization.hpp"
#include "imageeditor/ui/Theme.hpp"

#include <QApplication>
#include <QComboBox>
#include <QEvent>
#include <QFormLayout>
#include <QHideEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLocale>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTextLayout>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace imageeditor::ui {
namespace {

i18n::Message seconds(double milliseconds)
{
    return i18n::text("background.removal.duration")
        .arg(i18n::ltrToken(QLocale::c().toString(milliseconds / 1000.0, 'f', 2)));
}

QLabel* plainLabel(i18n::Message text, QWidget* parent)
{
    auto* label = i18n::make<QLabel>(std::move(text), parent);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setMinimumWidth(0);
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    return label;
}

} // namespace

BackgroundRemovalPanel::BackgroundRemovalPanel(QWidget* parent)
    : QFrame(parent)
{
    setObjectName(QStringLiteral("BackgroundRemovalPanel"));
    i18n::setAccessibleName(this, i18n::text("background.removal.title"));
    setAttribute(Qt::WA_StyledBackground);
    setMinimumWidth(260);
    setStyleSheet(
        QStringLiteral("QFrame#BackgroundRemovalPanel { background: %1; "
                       "border: 1px solid %2; border-radius: 6px; }")
            .arg(themeColor(ThemeColor::Surface).name(),
                 themeColor(ThemeColor::Border).name()));

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(10, 10, 10, 10);
    outer->setSpacing(7);

    auto* title = plainLabel(i18n::text("background.removal.title"), this);
    auto titleFont = title->font();
    titleFont.setBold(true);
    title->setFont(titleFont);
    outer->addWidget(title);
    target_ = plainLabel({}, this);
    target_->setObjectName(QStringLiteral("BackgroundRemovalTarget"));
    outer->addWidget(target_);

    auto* form = new QFormLayout;
    form->setContentsMargins(0, 0, 0, 0);
    form->setSpacing(6);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    model_ = new QComboBox(this);
    model_->setObjectName(QStringLiteral("BackgroundRemovalModel"));
    i18n::setAccessibleName(model_, i18n::text("background.removal.model"));
    model_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    model_->setMinimumContentsLength(8);
    model_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    i18n::addRow(form, i18n::text("background.removal.model"), model_);

    view_ = new QComboBox(this);
    view_->setObjectName(QStringLiteral("BackgroundRemovalView"));
    i18n::setAccessibleName(view_, i18n::text("main.window.refinement.view"));
    i18n::addItem(view_, i18n::text("main.window.refinement.transparency"), int(View::Transparency));
    i18n::addItem(view_, i18n::text("main.window.refinement.on.black"), int(View::Black));
    i18n::addItem(view_, i18n::text("main.window.refinement.on.white"), int(View::White));
    i18n::addItem(view_, i18n::text("background.removal.mask.only"), int(View::Mask));
    const auto refreshViewHints = [this] {
        for (int i = 0; i < view_->count(); ++i)
            view_->setItemData(i, i18n::text(i == 3
                    ? "background.removal.mask.raw.tooltip"
                    : "background.removal.preview.tooltip"), Qt::ToolTipRole);
        view_->setToolTip(view_->currentData(Qt::ToolTipRole).toString());
    };
    refreshViewHints();
    connect(&i18n::Localization::instance(), &i18n::Localization::languageChanged,
            this, refreshViewHints);
    view_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    view_->setMinimumContentsLength(8);
    view_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    i18n::addRow(form, i18n::text("main.window.refinement.view"), view_);
    outer->addLayout(form);

    original_ = i18n::make<QPushButton>(i18n::text("main.window.refinement.hold.for.original"), this);
    original_->setObjectName(QStringLiteral("BackgroundRemovalOriginal"));
    i18n::setAccessibleName(original_, i18n::text("main.window.refinement.hold.for.original"));
    i18n::setToolTip(original_, i18n::text("background.removal.original.hint"));
    original_->setAutoDefault(false);
    original_->installEventFilter(this);
    outer->addWidget(original_);
    connect(original_, &QPushButton::pressed, this, [this] {
        setOriginalHeld(true);
    });
    connect(original_, &QPushButton::released, this, [this] {
        setOriginalHeld(false);
    });

    maskHint_ = plainLabel({}, this);
    maskHint_->setObjectName(QStringLiteral("BackgroundRemovalMaskHint"));
    maskHint_->setStyleSheet(QStringLiteral("color: %1;")
        .arg(themeColor(ThemeColor::SecondaryText).name()));
    outer->addWidget(maskHint_);
    setTarget({}, false);

    status_ = plainLabel({}, this);
    status_->setObjectName(QStringLiteral("BackgroundRemovalStatus"));
    i18n::setAccessibleName(status_, i18n::text("background.removal.title"));
    status_->setMinimumHeight(status_->fontMetrics().height() * 2);
    status_->setMaximumHeight(status_->fontMetrics().lineSpacing() * 3);
    setStatus(i18n::text("background.removal.preparing"));
    status_->installEventFilter(this);
    outer->addWidget(status_);

    progress_ = new QProgressBar(this);
    progress_->setObjectName(QStringLiteral("BackgroundRemovalProgress"));
    i18n::setAccessibleName(progress_, i18n::text("background.removal.processing"));
    progress_->setRange(0, 0);
    progress_->setTextVisible(false);
    progress_->setFixedHeight(6);
    progress_->hide();
    outer->addWidget(progress_);

    timing_ = plainLabel({}, this);
    timing_->setObjectName(QStringLiteral("BackgroundRemovalTiming"));
    timing_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    timing_->setStyleSheet(QStringLiteral("color: %1;")
        .arg(themeColor(ThemeColor::SecondaryText).name()));
    outer->addWidget(timing_);

    auto* buttons = new QHBoxLayout;
    regenerate_ = i18n::make<QPushButton>(i18n::text("background.removal.retry"), this);
    regenerate_->setObjectName(QStringLiteral("BackgroundRemovalRegenerate"));
    i18n::setAccessibleName(regenerate_, i18n::text("background.removal.retry"));
    regenerate_->setAutoDefault(false);
    auto* cancel = i18n::make<QPushButton>(i18n::text("dialog.button.cancel"), this);
    cancel->setObjectName(QStringLiteral("BackgroundRemovalCancel"));
    i18n::setAccessibleName(cancel, i18n::text("dialog.button.cancel"));
    cancel->setAutoDefault(false);
    apply_ = i18n::make<QPushButton>(i18n::text("dialog.button.apply"), this);
    apply_->setObjectName(QStringLiteral("BackgroundRemovalApply"));
    i18n::setAccessibleName(apply_, i18n::text("dialog.button.apply"));
    apply_->setAutoDefault(false);
    buttons->addWidget(regenerate_);
    buttons->addStretch();
    buttons->addWidget(cancel);
    buttons->addWidget(apply_);
    outer->addLayout(buttons);

    connect(model_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        setOriginalHeld(false);
        clearResult();
        model_->setToolTip(model_->currentText());
        if (onModelChanged && !selectedModel().isEmpty())
            onModelChanged(selectedModel());
    });
    connect(view_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        view_->setToolTip(view_->currentData(Qt::ToolTipRole).toString());
        if (onViewChanged)
            onViewChanged(view());
    });
    connect(regenerate_, &QPushButton::clicked, this, [this] {
        if (onRegenerate)
            onRegenerate();
    });
    connect(apply_, &QPushButton::clicked, this, [this] {
        setOriginalHeld(false);
        if (!busy_ && validResult_ && onApply)
            onApply();
    });
    connect(cancel, &QPushButton::clicked, this, [this] {
        setOriginalHeld(false);
        if (onCancel)
            onCancel();
    });
    elapsedTimer_ = new QTimer(this);
    elapsedTimer_->setInterval(100);
    connect(elapsedTimer_, &QTimer::timeout, this, [this] {
        updateElapsed();
    });
    if (qApp)
        qApp->installEventFilter(this);
    connect(&i18n::Localization::instance(), &i18n::Localization::languageChanged,
            this, [this] { updateStatusText(); });
    i18n::Localization::instance().direction(this);
    updateControls();
}

void BackgroundRemovalPanel::setModels(
    const std::vector<BackgroundRemovalModel>& models)
{
    const QString previous = selectedModel();
    const QSignalBlocker blocker(model_);
    model_->clear();
    for (const auto& model : models) {
        model_->addItem(model.name, model.id);
        model_->setItemData(model_->count() - 1, model.name, Qt::ToolTipRole);
    }
    const int previousIndex = model_->findData(previous);
    if (previousIndex >= 0)
        model_->setCurrentIndex(previousIndex);
    model_->setToolTip(model_->currentText());
    clearResult();
    if (models.empty())
        setError(i18n::text("background.removal.unavailable"));
    updateControls();
}

void BackgroundRemovalPanel::setTarget(
    const QString& layerName, bool existingMask)
{
    target_->setText(layerName);
    target_->setVisible(!layerName.isEmpty());
    i18n::setText(maskHint_, i18n::text(existingMask
        ? "background.removal.mask.combine" : "background.removal.mask.add"));
    i18n::setToolTip(maskHint_, i18n::text("background.removal.preview.hint"));
}

void BackgroundRemovalPanel::setBusy(bool busy, i18n::Message status)
{
    busy_ = busy;
    if (busy_) {
        setOriginalHeld(false);
        clearResult();
        elapsed_.restart();
        busyStatus_ = status.isEmpty()
            ? i18n::text("background.removal.processing") : status;
        elapsedTimer_->start();
        updateElapsed();
    } else {
        elapsedTimer_->stop();
        if (!status.isEmpty())
            setStatus(status);
    }
    updateControls();
}

void BackgroundRemovalPanel::setStatus(i18n::Message status)
{
    fullStatus_ = std::move(status);
    updateStatusText();
}

void BackgroundRemovalPanel::setError(const QString& error)
{
    setBusy(false);
    setOriginalHeld(false);
    clearResult();
    failed_ = true;
    setStatus(error);
    updateControls();
}

void BackgroundRemovalPanel::setResult(
    const BackgroundRemovalResult& result, bool canApply)
{
    setBusy(false);
    if (result.cancelled) {
        setError(i18n::text("background.removal.cancelled"));
        return;
    }
    if (!result.error.isEmpty()) {
        setError(result.error);
        return;
    }
    validResult_ = canApply && !result.mask.isNull();
    failed_ = !validResult_;
    setStatus(validResult_ ? i18n::text("background.removal.ready")
                          : i18n::text("background.removal.empty"));
    const auto& t = result.timings;
    i18n::setText(timing_, i18n::text("background.removal.time").arg(seconds(t.totalMs)));
    // Runtime operator and graph diagnostics retain their original wording.
    i18n::setToolTip(timing_, i18n::text("background.removal.timings")
            .arg(seconds(t.initializationMs), seconds(t.inferenceMs),
                 seconds(t.preprocessingMs), seconds(t.postprocessingMs),
                 seconds(t.totalMs)) + QStringLiteral("\n") + result.details);
    updateControls();
}

QString BackgroundRemovalPanel::selectedModel() const
{
    return model_->currentData().toString();
}

BackgroundRemovalPanel::View BackgroundRemovalPanel::view() const
{
    return View(view_->currentData().toInt());
}

QSize BackgroundRemovalPanel::sizeHint() const
{
    const auto hint = QFrame::sizeHint();
    return {316, hint.height()};
}

bool BackgroundRemovalPanel::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == status_ && event->type() == QEvent::Resize)
        updateStatusText();
    if (event->type() == QEvent::ApplicationDeactivate
        || event->type() == QEvent::WindowDeactivate
        || (watched == original_ && (event->type() == QEvent::FocusOut
            || event->type() == QEvent::Hide
            || event->type() == QEvent::EnabledChange
            || event->type() == QEvent::UngrabMouse))) {
        setOriginalHeld(false);
    }
    if (watched == original_ && (event->type() == QEvent::KeyPress
            || event->type() == QEvent::KeyRelease)) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Space || key->key() == Qt::Key_Return
            || key->key() == Qt::Key_Enter) {
            if (event->type() == QEvent::KeyPress) {
                if (key->isAutoRepeat())
                    return originalKey_ == key->key();
                if (!original_->isEnabled())
                    return false;
                originalKey_ = key->key();
                setOriginalHeld(true);
                original_->setDown(originalHeld_);
            } else {
                // A Space press can begin on the canvas before focus moves
                // here. Its release still belongs to canvas panning.
                if (originalKey_ != key->key())
                    return false;
                if (!key->isAutoRepeat())
                    setOriginalHeld(false);
            }
            return true;
        }
        if (key->key() == Qt::Key_Escape) {
            original_->setDown(false);
            setOriginalHeld(false);
            // The workspace still owns Escape and may cancel the operation.
        }
    }
    return QFrame::eventFilter(watched, event);
}

void BackgroundRemovalPanel::hideEvent(QHideEvent* event)
{
    setOriginalHeld(false);
    QFrame::hideEvent(event);
}

void BackgroundRemovalPanel::setOriginalHeld(bool held)
{
    if (held && !validResult_)
        held = false;
    if (!held)
        originalKey_ = 0;
    if (originalHeld_ == held)
        return;
    originalHeld_ = held;
    if (!held)
        original_->setDown(false);
    if (onOriginalChanged)
        onOriginalChanged(held);
}

void BackgroundRemovalPanel::updateElapsed()
{
    if (busy_)
        setStatus(i18n::text("background.removal.elapsed")
            .arg(busyStatus_, seconds(double(elapsed_.elapsed()))));
}

void BackgroundRemovalPanel::updateStatusText()
{
    // ONNX diagnostics may contain a full operator trace. Keep the controls
    // reachable; the complete diagnostic remains in the tooltip/accessibility
    // description rather than expanding the card beyond the canvas.
    QString text = fullStatus_.resolved();
    status_->setToolTip(text);
    status_->setAccessibleDescription(text);
    text.replace(QChar('\n'), QChar(' '));
    const int width = std::max(1, status_->contentsRect().width());
    QTextLayout layout(text, status_->font());
    QTextOption options;
    options.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout.setTextOption(options);
    layout.beginLayout();
    QString displayed;
    for (int index = 0; index < 3; ++index) {
        auto line = layout.createLine();
        if (!line.isValid())
            break;
        line.setLineWidth(width);
        if (index > 0)
            displayed += QChar('\n');
        if (index == 2) {
            displayed += status_->fontMetrics().elidedText(
                text.mid(line.textStart()), Qt::ElideRight, width);
        } else {
            displayed += text.mid(line.textStart(), line.textLength()).trimmed();
        }
    }
    layout.endLayout();
    status_->setText(displayed);
}

void BackgroundRemovalPanel::updateControls()
{
    // Model changes remain available during inference. The controller cancels
    // old generations; disabling this control would obstruct model comparison.
    model_->setEnabled(model_->count() > 0);
    regenerate_->setVisible(failed_ && !busy_ && model_->count() > 0);
    regenerate_->setEnabled(failed_ && !busy_ && model_->count() > 0);
    apply_->setEnabled(!busy_ && validResult_);
    original_->setEnabled(validResult_);
    progress_->setVisible(busy_);
}

void BackgroundRemovalPanel::clearResult()
{
    validResult_ = false;
    failed_ = false;
    i18n::setText(timing_, {});
    i18n::setToolTip(timing_, {});
    updateControls();
}

} // namespace imageeditor::ui
