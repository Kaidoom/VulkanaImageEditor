#include "imageeditor/ui/CompactValueControl.hpp"

#include <QEvent>
#include <QFocusEvent>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLineEdit>
#include <QApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QStyle>
#include <QStyleOptionSpinBox>
#include <QTabletEvent>
#include <QTimer>
#include <QTouchEvent>
#include <QWheelEvent>

#include <algorithm>
#include <optional>
#include <utility>

namespace imageeditor::ui {
namespace {

std::optional<QPointF> pointerPressGlobalPosition(const QEvent& event)
{
    switch (event.type()) {
    case QEvent::MouseButtonPress:
        return static_cast<const QMouseEvent&>(event).globalPosition();
    case QEvent::TabletPress:
        return static_cast<const QTabletEvent&>(event).globalPosition();
    case QEvent::TouchBegin: {
        const auto& points = static_cast<const QTouchEvent&>(event).points();
        if (!points.isEmpty()) {
            return points.front().globalPosition();
        }
        break;
    }
    default:
        break;
    }
    return std::nullopt;
}

} // namespace

CompactValueControl::CompactValueControl(QWidget* parent)
    : QDoubleSpinBox(parent)
{
    setProperty("compactValueControl", true);
    setAccelerated(true);
    setKeyboardTracking(true);
    setButtonSymbols(QAbstractSpinBox::UpDownArrows);
    setAlignment(Qt::AlignCenter);
    setFocusPolicy(Qt::StrongFocus);
    lineEdit()->setObjectName(QStringLiteral("CompactValueEditor"));
    lineEdit()->setAlignment(Qt::AlignCenter);
    // All pointer input is deliberately received by this one widget. The
    // editor remains fully keyboard-editable after we focus/select it.
    lineEdit()->setAttribute(Qt::WA_TransparentForMouseEvents);
    lineEdit()->setReadOnly(true);
    lineEdit()->installEventFilter(this);
    connect(lineEdit(), &QLineEdit::selectionChanged, this,
        [this] { clampTextSelectionToNumber(); });
    connect(lineEdit(), &QLineEdit::cursorPositionChanged, this,
        [this](int, int current) { clampTextCursorToNumber(current); });
    connect(this, &QAbstractSpinBox::editingFinished, this,
        [this] { enterSlideMode(); });
}

QRect CompactValueControl::valueFieldRect() const
{
    QStyleOptionSpinBox option;
    initStyleOption(&option);
    return style()->subControlRect(QStyle::CC_SpinBox, &option,
        QStyle::SC_SpinBoxEditField, this);
}

QRect CompactValueControl::progressTrackRect() const
{
    QStyleOptionSpinBox option;
    initStyleOption(&option);
    auto track = option.rect.adjusted(1, 1, -1, -1);
    const auto up = style()->subControlRect(QStyle::CC_SpinBox, &option,
        QStyle::SC_SpinBoxUp, this);
    const auto down = style()->subControlRect(QStyle::CC_SpinBox, &option,
        QStyle::SC_SpinBoxDown, this);
    const auto buttons = up.united(down);
    if (buttons.isValid() && track.intersects(buttons)) {
        if (buttons.center().x() >= option.rect.center().x()) {
            track.setRight(buttons.left() - 1);
        } else {
            track.setLeft(buttons.right() + 1);
        }
    }
    return track;
}

bool CompactValueControl::isManualEntryActive() const noexcept
{
    return entryMode_ == EntryMode::Manual;
}

double CompactValueControl::valueFromPosition(qreal localX) const noexcept
{
    const auto field = valueFieldRect();
    if (field.width() <= 1 || maximum() <= minimum()) {
        return minimum();
    }
    const auto fraction = std::clamp(
        (localX - static_cast<qreal>(field.left()))
            / static_cast<qreal>(field.width() - 1),
        0.0, 1.0);
    return minimum() + fraction * (maximum() - minimum());
}

bool CompactValueControl::event(QEvent* event)
{
    if (!event) {
        return QDoubleSpinBox::event(event);
    }
    const auto type = event->type();
    if (type == QEvent::ShortcutOverride) {
        const auto* key = static_cast<const QKeyEvent*>(event);
        if (shouldClaimEntryShortcut(*key)) {
            event->accept();
            return true;
        }
    }
    const auto handled = QDoubleSpinBox::event(event);
    switch (type) {
    case QEvent::TouchCancel:
        finishSlide();
        finishInteraction();
        break;
    case QEvent::FocusOut:
    case QEvent::WindowDeactivate:
    case QEvent::Hide:
    case QEvent::Close:
        finishSlide();
        commitAndExitManualEntry();
        finishInteraction();
        break;
    default:
        break;
    }
    return handled;
}

bool CompactValueControl::eventFilter(QObject* watched, QEvent* event)
{
    if (event && isManualEntryActive()) {
        if (const auto global = pointerPressGlobalPosition(*event)) {
            const QRect globalBounds(mapToGlobal(QPoint {}), size());
            if (!globalBounds.contains(global->toPoint())) {
                // A native canvas press does not necessarily move QWidget
                // focus. Commit before the outside event continues so the
                // canvas uses the final value and the editor loses its caret.
                commitAndExitManualEntry();
                clearFocus();
            }
        }
    }

    if (watched == lineEdit() && event) {
        if (event->type() == QEvent::ShortcutOverride) {
            const auto* key = static_cast<const QKeyEvent*>(event);
            if (shouldClaimEntryShortcut(*key)) {
                event->accept();
                return true;
            }
        } else if (event->type() == QEvent::KeyPress) {
            auto* key = static_cast<QKeyEvent*>(event);
            if (key->matches(QKeySequence::SelectAll)) {
                if (isManualEntryActive()) {
                    selectNumericText();
                } else {
                    clearTextSelection();
                }
                event->accept();
                return true;
            }
            if (key->key() == Qt::Key_Escape && isManualEntryActive()) {
                commitAndExitManualEntry();
                event->accept();
                return true;
            } else {
                prepareManualEntryForKey(*key);
            }
        } else if (event->type() == QEvent::InputMethod) {
            const auto* input = static_cast<const QInputMethodEvent*>(event);
            if (!isManualEntryActive() && !input->commitString().isEmpty()
                && canBeginManualEntry(input->commitString())) {
                enterManualEntry();
            }
        } else if (event->type() == QEvent::FocusOut
            && isManualEntryActive()) {
            // Let QAbstractSpinBox interpret the pending editor text first.
            QTimer::singleShot(0, this,
                [this] { commitAndExitManualEntry(); });
        }
    }
    return QDoubleSpinBox::eventFilter(watched, event);
}

void CompactValueControl::focusInEvent(QFocusEvent* event)
{
    QDoubleSpinBox::focusInEvent(event);
    if (!isManualEntryActive() && !sliding_) {
        clearTextSelection();
    }
}

void CompactValueControl::keyPressEvent(QKeyEvent* event)
{
    if (event && event->key() == Qt::Key_Escape
        && isManualEntryActive()) {
        commitAndExitManualEntry();
        event->accept();
        return;
    }
    if (event) {
        prepareManualEntryForKey(*event);
        if (event->key() == Qt::Key_Up || event->key() == Qt::Key_Down
            || event->key() == Qt::Key_PageUp || event->key() == Qt::Key_PageDown)
            beginInteraction();
    }
    QDoubleSpinBox::keyPressEvent(event);
}

void CompactValueControl::keyReleaseEvent(QKeyEvent* event)
{
    QDoubleSpinBox::keyReleaseEvent(event);
    if (!event->isAutoRepeat() && !isManualEntryActive()
        && (event->key() == Qt::Key_Up || event->key() == Qt::Key_Down
            || event->key() == Qt::Key_PageUp || event->key() == Qt::Key_PageDown))
        finishInteraction();
}

void CompactValueControl::stepBy(int steps)
{
    QDoubleSpinBox::stepBy(steps);
    if (!isManualEntryActive()) clearTextSelection();
}

void CompactValueControl::beginInteraction()
{
    if (interactionActive_) return;
    interactionActive_ = true;
    if (onInteractionStarted) onInteractionStarted();
}

void CompactValueControl::finishInteraction()
{
    if (!interactionActive_) return;
    interactionActive_ = false;
    if (onInteractionFinished) onInteractionFinished();
}

void CompactValueControl::finishEditing(bool commit, bool keepFocus)
{
    if (commit) commitAndExitManualEntry();
    else enterSlideMode();
    finishSlide();
    finishInteraction();
    if (!keepFocus) {
        clearFocus();
        if (lineEdit()) lineEdit()->clearFocus();
    }
}

void CompactValueControl::paintEvent(QPaintEvent* event)
{
    QDoubleSpinBox::paintEvent(event);

    const auto track = progressTrackRect();
    if (!track.isValid() || maximum() <= minimum()) {
        return;
    }

    const auto fraction = std::clamp(
        (value() - minimum()) / (maximum() - minimum()), 0.0, 1.0);
    QRectF fill(track);
    const auto fillWidth = static_cast<qreal>(track.width()) * fraction;
    if (layoutDirection() == Qt::RightToLeft) {
        fill.setLeft(fill.right() - fillWidth);
    } else {
        fill.setWidth(fillWidth);
    }

    auto color = palette().color(QPalette::Highlight);
    color.setAlpha(isEnabled() ? (underMouse() ? 94 : 72) : 34);
    // Keep numeric selection prominent without dimming the editor or steppers.
    if (isManualEntryActive()) color.setAlphaF(color.alphaF() * 0.35F);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    QPainterPath clip;
    clip.addRoundedRect(QRectF(rect().adjusted(1, 1, -1, -1)), 4.0, 4.0);
    painter.setClipPath(clip);
    painter.fillRect(fill, color);

    painter.setClipping(false);
    QStyleOptionSpinBox option;
    initStyleOption(&option);
    const auto up = style()->subControlRect(QStyle::CC_SpinBox, &option,
        QStyle::SC_SpinBoxUp, this);
    const auto down = style()->subControlRect(QStyle::CC_SpinBox, &option,
        QStyle::SC_SpinBoxDown, this);
    auto arrowColor = palette().color(QPalette::Text);
    arrowColor.setAlpha(isEnabled() ? 190 : 80);
    painter.setPen(QPen(arrowColor, 1.2, Qt::SolidLine,
        Qt::RoundCap, Qt::RoundJoin));
    const auto drawChevron = [&painter](const QRect& rect, bool pointsUp) {
        const QPointF center = QRectF(rect).center();
        const auto direction = pointsUp ? -1.0 : 1.0;
        const QPointF tip {center.x(), center.y() + direction * 2.0};
        painter.drawLine(QPointF {center.x() - 3.0,
                             center.y() - direction * 1.0},
            tip);
        painter.drawLine(tip, QPointF {center.x() + 3.0,
                                  center.y() - direction * 1.0});
    };
    drawChevron(up, true);
    drawChevron(down, false);
}

bool CompactValueControl::isValueFieldPosition(const QPointF& position) const
{
    return valueFieldRect().contains(position.toPoint());
}

bool CompactValueControl::isManualEntryKey(const QKeyEvent& event) const
{
    constexpr auto commandModifiers = Qt::ControlModifier
        | Qt::AltModifier | Qt::MetaModifier;
    return !event.text().isEmpty()
        && !(event.modifiers() & commandModifiers)
        && canBeginManualEntry(event.text());
}

bool CompactValueControl::shouldClaimEntryShortcut(
    const QKeyEvent& event) const
{
    return !isManualEntryActive() && isManualEntryKey(event);
}

bool CompactValueControl::canBeginManualEntry(
    const QString& replacement) const
{
    if (replacement.isEmpty()) {
        return false;
    }
    auto candidate = lineEdit()->text();
    const auto [start, length] = numericTextRange();
    candidate.replace(start, length, replacement);
    auto cursor = start + static_cast<int>(replacement.size());
    return validate(candidate, cursor) != QValidator::Invalid;
}

std::pair<int, int> CompactValueControl::numericTextRange() const
{
    const auto fullText = lineEdit()->text();
    const auto numericText = cleanText();
    const auto fullLength = static_cast<int>(fullText.size());
    auto start = numericText.isEmpty()
        ? -1 : static_cast<int>(fullText.indexOf(numericText));
    if (start < 0) {
        start = std::clamp(static_cast<int>(prefix().size()), 0, fullLength);
    }
    auto length = static_cast<int>(numericText.size());
    if (length == 0) {
        length = std::max(0,
            fullLength - start - static_cast<int>(suffix().size()));
    }
    length = std::clamp(length, 0, fullLength - start);
    return {start, length};
}

void CompactValueControl::clampTextSelectionToNumber()
{
    if (clampingSelection_ || !lineEdit() || !lineEdit()->hasSelectedText()) {
        return;
    }
    const auto [numberStart, numberLength] = numericTextRange();
    if (!isManualEntryActive()) {
        clampingSelection_ = true;
        lineEdit()->deselect();
        clampingSelection_ = false;
        return;
    }
    const auto numberEnd = numberStart + numberLength;
    const auto selectionStart = lineEdit()->selectionStart();
    const auto selectionEnd = selectionStart
        + static_cast<int>(lineEdit()->selectedText().size());
    if (selectionStart >= numberStart && selectionEnd <= numberEnd) {
        return;
    }

    const auto clippedStart = std::max(selectionStart, numberStart);
    const auto clippedEnd = std::min(selectionEnd, numberEnd);
    clampingSelection_ = true;
    if (clippedEnd > clippedStart) {
        lineEdit()->setSelection(clippedStart, clippedEnd - clippedStart);
    } else {
        lineEdit()->deselect();
        lineEdit()->setCursorPosition(std::clamp(
            lineEdit()->cursorPosition(), numberStart, numberEnd));
    }
    clampingSelection_ = false;
}

void CompactValueControl::clampTextCursorToNumber(int position)
{
    if (clampingCursor_ || !isManualEntryActive() || !lineEdit()) {
        return;
    }
    const auto [numberStart, numberLength] = numericTextRange();
    const auto clamped = std::clamp(
        position, numberStart, numberStart + numberLength);
    if (clamped == position) {
        return;
    }
    clampingCursor_ = true;
    lineEdit()->setCursorPosition(clamped);
    clampingCursor_ = false;
}

void CompactValueControl::selectNumericText()
{
    if (!lineEdit()) {
        return;
    }
    const auto [start, length] = numericTextRange();
    lineEdit()->setSelection(start, length);
}

void CompactValueControl::clearTextSelection()
{
    if (lineEdit()) {
        lineEdit()->deselect();
    }
}

void CompactValueControl::prepareManualEntryForKey(const QKeyEvent& event)
{
    if (!isManualEntryActive() && isManualEntryKey(event)) {
        enterManualEntry();
    }
}

void CompactValueControl::commitAndExitManualEntry()
{
    if (!isManualEntryActive()) {
        return;
    }
    interpretText();
    enterSlideMode();
}

void CompactValueControl::enterManualEntry()
{
    finishSlide();
    entryMode_ = EntryMode::Manual;
    beginInteraction();
    lineEdit()->setReadOnly(false);
    lineEdit()->setAttribute(Qt::WA_TransparentForMouseEvents, false);
    lineEdit()->setFocus(Qt::OtherFocusReason);
    selectNumericText();
    if (qApp) {
        qApp->installEventFilter(this);
    }
    update(progressTrackRect());
}

void CompactValueControl::enterSlideMode()
{
    const bool wasManual = isManualEntryActive();
    if (!lineEdit()) {
        entryMode_ = EntryMode::Slide;
        return;
    }
    entryMode_ = EntryMode::Slide;
    if (qApp) {
        qApp->removeEventFilter(this);
    }
    lineEdit()->setReadOnly(true);
    lineEdit()->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    clearTextSelection();
    if (wasManual) {
        update(progressTrackRect());
        finishInteraction();
    }
}

void CompactValueControl::beginSlide(QMouseEvent* event)
{
    if (isManualEntryActive()) {
        event->ignore();
        return;
    }
    sliding_ = true;
    beginInteraction();
    lineEdit()->setFocus(Qt::MouseFocusReason);
    updateSlide(event->position());
    clearTextSelection();
    event->accept();
}

void CompactValueControl::updateSlide(const QPointF& position)
{
    setValue(valueFromPosition(position.x()));
}

void CompactValueControl::finishSlide()
{
    const bool wasSliding = sliding_;
    sliding_ = false;
    if (!isManualEntryActive()) {
        clearTextSelection();
    }
    if (wasSliding) finishInteraction();
}

void CompactValueControl::mousePressEvent(QMouseEvent* event)
{
    if (event && event->button() == Qt::LeftButton
        && isValueFieldPosition(event->position())) {
        beginSlide(event);
        return;
    }
    if (event && event->button() == Qt::LeftButton) {
        commitAndExitManualEntry();
        beginInteraction();
    }
    QDoubleSpinBox::mousePressEvent(event);
}

void CompactValueControl::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event && event->button() == Qt::LeftButton
        && isValueFieldPosition(event->position())) {
        // Reuse the same numeric editor and transaction as keyboard entry.
        // Do not start another slide on the second press: its release must
        // leave the selected number ready to replace, not change the value.
        enterManualEntry();
        event->accept();
        return;
    }
    if (event && event->button() == Qt::LeftButton) mousePressEvent(event);
    else QDoubleSpinBox::mouseDoubleClickEvent(event);
}

void CompactValueControl::mouseMoveEvent(QMouseEvent* event)
{
    if (event && sliding_) {
        updateSlide(event->position());
        event->accept();
        return;
    }
    QDoubleSpinBox::mouseMoveEvent(event);
}

void CompactValueControl::mouseReleaseEvent(QMouseEvent* event)
{
    if (event && sliding_ && event->button() == Qt::LeftButton) {
        updateSlide(event->position());
        finishSlide();
        event->accept();
        return;
    }
    QDoubleSpinBox::mouseReleaseEvent(event);
    if (event && event->button() == Qt::LeftButton && !isManualEntryActive()) finishInteraction();
}

void CompactValueControl::wheelEvent(QWheelEvent* event)
{
    // Scrolling over editor controls should continue scrolling their parent
    // panel instead of silently changing a brush parameter.
    event->ignore();
}

} // namespace imageeditor::ui
