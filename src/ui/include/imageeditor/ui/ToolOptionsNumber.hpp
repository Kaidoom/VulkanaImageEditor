#pragma once
#include "imageeditor/ui/CrossWindowPointerRouter.hpp"
#include <QDoubleSpinBox>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QStyleOptionSpinBox>
#include <QWheelEvent>
#include <functional>

namespace imageeditor::ui {
// Shared signed numeric control: pointer holds are one action, arrows never
// retain text focus, and wheel input belongs to the enclosing scroll area.
class ToolOptionsNumber final : public QDoubleSpinBox, public PointerPressPreparation {
public:
    explicit ToolOptionsNumber(QWidget* parent)
        : QDoubleSpinBox(parent)
    {
        setProperty("compactValueControl", true);
        lineEdit()->setObjectName(QStringLiteral("CompactValueEditor"));
        setSingleStep(1);
        setKeyboardTracking(false);
        setFixedHeight(30);
    }

    bool interactionActive() const { return mouseStepping_ || keyStepping_; }
    // Keep the underlying representative number for the first increment, but
    // never present it as a uniform value for a mixed text-format range.
    void setIndeterminate(bool value)
    {
        indeterminate_ = value;
        lineEdit()->setText(prefix() + textFromValue(this->value()) + suffix());
    }
    std::function<void()> onInteractionFinished;
    std::function<void()> onInteractionCancelled;
    // Overlay focus bridges must distinguish pointer stepping from text entry
    // before capture begins, using the same style hit test as the control.
    bool isStepButtonPress(const QMouseEvent& event) const
    {
        if (event.button() != Qt::LeftButton)
            return false;
        QStyleOptionSpinBox option;
        initStyleOption(&option);
        const auto hit
            = style()->hitTestComplexControl(QStyle::CC_SpinBox, &option, event.position().toPoint(), this);
        return hit == QStyle::SC_SpinBoxUp || hit == QStyle::SC_SpinBoxDown;
    }
    void preparePointerPress(const QMouseEvent& event) override
    {
        if (!isStepButtonPress(event))
            return;
        // Qt assigns click focus before dispatch. Finish pending typing as
        // its own action and remove focus before the router starts capture.
        interpretText();
        finishInteraction();
        clearFocus();
        lineEdit()->deselect();
    }
    void finishInteraction()
    {
        const bool wasActive = interactionActive();
        mouseStepping_ = keyStepping_ = false;
        if (wasActive && onInteractionFinished)
            onInteractionFinished();
    }

protected:
    QString textFromValue(double value) const override
    {
        return indeterminate_ ? QStringLiteral("Mixed") : QDoubleSpinBox::textFromValue(value);
    }
    void focusInEvent(QFocusEvent* event) override
    {
        if(indeterminate_)setIndeterminate(false);
        QDoubleSpinBox::focusInEvent(event);
    }
    bool event(QEvent* event) override
    {
        // The shared pointer router sends this before releasing Qt's implicit
        // press owner on focus/deactivation/cancellation. Transactional controls
        // can roll back instead of mistaking that synthetic release for Apply.
        if (event->type() == QEvent::TouchCancel && interactionActive() && onInteractionCancelled) {
            mouseStepping_ = keyStepping_ = false;
            onInteractionCancelled();
        }
        return QDoubleSpinBox::event(event);
    }
    void mousePressEvent(QMouseEvent* event) override
    {
        if(indeterminate_)setIndeterminate(false);
        if (isStepButtonPress(*event)) {
            // Also support this shared control outside a routed editor.
            // In the app the preparation has already run before capture.
            preparePointerPress(*event);
            mouseStepping_ = true;
        }
        QDoubleSpinBox::mousePressEvent(event);
    }
    void mouseDoubleClickEvent(QMouseEvent* event) override
    {
        if (isStepButtonPress(*event))
            mousePressEvent(event);
        else
            QDoubleSpinBox::mouseDoubleClickEvent(event);
    }
    void mouseReleaseEvent(QMouseEvent* event) override
    {
        QDoubleSpinBox::mouseReleaseEvent(event);
        if (event->button() == Qt::LeftButton)
            finishInteraction();
    }
    void focusOutEvent(QFocusEvent* event) override
    {
        QDoubleSpinBox::focusOutEvent(event);
        finishInteraction();
    }
    void keyReleaseEvent(QKeyEvent* event) override
    {
        QDoubleSpinBox::keyReleaseEvent(event);
        if (!event->isAutoRepeat()
            && (event->key() == Qt::Key_Up || event->key() == Qt::Key_Down || event->key() == Qt::Key_PageUp
                || event->key() == Qt::Key_PageDown))
            finishInteraction();
    }
    void stepBy(int steps) override
    {
        QDoubleSpinBox::stepBy(steps);
        // Native stepping selects the number after every step, including
        // timer repeats. Only keyboard/text edits should keep a selection.
        if (mouseStepping_)
            lineEdit()->deselect();
    }
    void paintEvent(QPaintEvent* event) override
    {
        QDoubleSpinBox::paintEvent(event);
        QStyleOptionSpinBox option;
        initStyleOption(&option);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(palette().color(QPalette::Text), 1.2, Qt::SolidLine, Qt::RoundCap));
        for (const auto sub : { QStyle::SC_SpinBoxUp, QStyle::SC_SpinBoxDown }) {
            const auto rect = style()->subControlRect(QStyle::CC_SpinBox, &option, sub, this);
            const auto c = QRectF(rect).center();
            const double direction = sub == QStyle::SC_SpinBoxUp ? -1.0 : 1.0;
            const auto tip = c + QPointF(0, direction * 2);
            painter.drawLine(c + QPointF(-3, -direction), tip);
            painter.drawLine(tip, c + QPointF(3, -direction));
        }
    }
    void wheelEvent(QWheelEvent* event) override { event->ignore(); }
    void keyPressEvent(QKeyEvent* event) override
    {
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter
            || event->key() == Qt::Key_Escape) {
            interpretText();
            finishInteraction();
            clearFocus();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Up || event->key() == Qt::Key_Down || event->key() == Qt::Key_PageUp
            || event->key() == Qt::Key_PageDown)
            keyStepping_ = true;
        QDoubleSpinBox::keyPressEvent(event);
    }

private:
    bool mouseStepping_ { false }, keyStepping_ { false };
    bool indeterminate_ {false};
};
}
