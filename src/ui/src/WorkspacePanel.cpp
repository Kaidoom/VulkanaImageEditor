#include "imageeditor/ui/WorkspacePanel.hpp"

#include <QApplication>
#include <QColor>
#include <QCursor>
#include <QEnterEvent>
#include <QEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QResizeEvent>
#include <QSizePolicy>
#include <QStyle>
#include <QVBoxLayout>

#include <utility>

namespace imageeditor::ui {
namespace {

class PanelTitleBar final : public QWidget {
public:
    explicit PanelTitleBar(WorkspacePanel& panel)
        : QWidget(&panel)
        , panel_(panel)
    {
        setObjectName(QStringLiteral("WorkspacePanelTitleBar"));
        setFixedHeight(38);
        setCursor(Qt::OpenHandCursor);
        setToolTip(QStringLiteral("Drag to move or dock this panel"));
        setAttribute(Qt::WA_Hover);
    }

protected:
    bool event(QEvent* event) override
    {
        if (event && (event->type() == QEvent::ApplicationDeactivate
                || event->type() == QEvent::WindowDeactivate
                || event->type() == QEvent::FocusOut
                || event->type() == QEvent::Hide
                || event->type() == QEvent::Close
                || event->type() == QEvent::TouchCancel)) {
            cancelDrag();
        }
        // UngrabMouse is not terminal. A Wayland surface handoff can emit it
        // while the physical button remains down; CrossWindowPointerRouter
        // continues routing the logical gesture to this title bar.
        return QWidget::event(event);
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() != Qt::LeftButton) {
            QWidget::mousePressEvent(event);
            return;
        }
        pressed_ = true;
        dragging_ = false;
        pressGlobal_ = event->globalPosition().toPoint();
        pressOffset_ = panel_.mapFromGlobal(pressGlobal_);
        setCursor(Qt::ClosedHandCursor);
        update();
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (!pressed_) {
            QWidget::mouseMoveEvent(event);
            return;
        }
        const QPoint global = event->globalPosition().toPoint();
        if (!dragging_) {
            if ((global - pressGlobal_).manhattanLength()
                < QApplication::startDragDistance()) {
                event->accept();
                return;
            }
            dragging_ = true;
            if (panel_.onDragStarted) {
                panel_.onDragStarted(&panel_, pressGlobal_, pressOffset_);
            }
        }
        if (panel_.onDragMoved) {
            panel_.onDragMoved(&panel_, global);
        }
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (event->button() != Qt::LeftButton || !pressed_) {
            QWidget::mouseReleaseEvent(event);
            return;
        }
        const bool wasDragging = dragging_;
        pressed_ = false;
        dragging_ = false;
        if (QWidget::mouseGrabber() == this) {
            releaseMouse();
        }
        setCursor(Qt::OpenHandCursor);
        update();
        if (wasDragging && panel_.onDragFinished) {
            panel_.onDragFinished(
                &panel_, event->globalPosition().toPoint(), false);
        }
        event->accept();
    }

    void paintEvent(QPaintEvent* event) override
    {
        QPainter painter(this);
        painter.setClipRegion(event->region());
        painter.fillRect(rect(), palette().color(QPalette::Window));
        painter.setPen(palette().color(QPalette::Mid));
        painter.drawLine(rect().bottomLeft(), rect().bottomRight());

        QFont titleFont = font();
        titleFont.setWeight(QFont::DemiBold);
        painter.setFont(titleFont);
        painter.setPen(palette().color(QPalette::Text));
        painter.drawText(rect().adjusted(12, 0, -42, 0),
            Qt::AlignLeft | Qt::AlignVCenter, panel_.title());

        painter.setPen(Qt::NoPen);
        painter.setBrush(palette().color(dragging_
                ? QPalette::Highlight : QPalette::PlaceholderText));
        const QPoint center(width() - 20, height() / 2);
        for (int column = -1; column <= 1; ++column) {
            for (int row = -1; row <= 1; row += 2) {
                painter.drawEllipse(
                    QPointF(center + QPoint {column * 4, row * 3}), 1.4, 1.4);
            }
        }
    }

private:
    void cancelDrag()
    {
        if (!pressed_) {
            return;
        }
        const bool wasDragging = dragging_;
        pressed_ = false;
        dragging_ = false;
        setCursor(Qt::OpenHandCursor);
        update();
        if (wasDragging && panel_.onDragFinished) {
            panel_.onDragFinished(&panel_, QCursor::pos(), true);
        }
    }

    WorkspacePanel& panel_;
    QPoint pressGlobal_;
    QPoint pressOffset_;
    bool pressed_ {false};
    bool dragging_ {false};
};

class PanelResizeGrip final : public QWidget {
public:
    explicit PanelResizeGrip(WorkspacePanel& panel)
        : QWidget(&panel)
        , panel_(panel)
    {
        setObjectName(QStringLiteral("WorkspacePanelResizeGrip"));
        setFixedSize(24, 24);
        setCursor(Qt::SizeFDiagCursor);
        setToolTip(QStringLiteral("Drag to resize panel"));
        setAttribute(Qt::WA_Hover);
    }

protected:
    bool event(QEvent* event) override
    {
        if (event && (event->type() == QEvent::ApplicationDeactivate
                || event->type() == QEvent::WindowDeactivate
                || event->type() == QEvent::FocusOut
                || event->type() == QEvent::Hide
                || event->type() == QEvent::Close
                || event->type() == QEvent::TouchCancel)) {
            cancelResize();
        }
        return QWidget::event(event);
    }

    void enterEvent(QEnterEvent* event) override
    {
        hovered_ = true;
        update();
        QWidget::enterEvent(event);
    }

    void leaveEvent(QEvent* event) override
    {
        hovered_ = false;
        update();
        QWidget::leaveEvent(event);
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() != Qt::LeftButton) {
            QWidget::mousePressEvent(event);
            return;
        }
        resizing_ = true;
        if (panel_.onResizeStarted) {
            panel_.onResizeStarted(&panel_, event->globalPosition().toPoint());
        }
        update();
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (!resizing_) {
            QWidget::mouseMoveEvent(event);
            return;
        }
        if (panel_.onResizeMoved) {
            panel_.onResizeMoved(&panel_, event->globalPosition().toPoint());
        }
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (!resizing_ || event->button() != Qt::LeftButton) {
            QWidget::mouseReleaseEvent(event);
            return;
        }
        resizing_ = false;
        if (QWidget::mouseGrabber() == this) {
            releaseMouse();
        }
        update();
        if (panel_.onResizeFinished) {
            panel_.onResizeFinished(&panel_, false);
        }
        event->accept();
    }

    void paintEvent(QPaintEvent* event) override
    {
        QPainter painter(this);
        painter.setClipRegion(event->region());
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(palette().color(hovered_ || resizing_
                    ? QPalette::Highlight : QPalette::PlaceholderText),
            1.7, Qt::SolidLine, Qt::RoundCap));
        for (int inset = 5; inset <= 15; inset += 5) {
            painter.drawLine(width() - inset, height() - 3,
                width() - 3, height() - inset);
        }
    }

private:
    void cancelResize()
    {
        if (!resizing_) {
            return;
        }
        resizing_ = false;
        update();
        if (panel_.onResizeFinished) {
            panel_.onResizeFinished(&panel_, true);
        }
    }

    WorkspacePanel& panel_;
    bool hovered_ {false};
    bool resizing_ {false};
};

} // namespace

WorkspacePanel::WorkspacePanel(
    QString title, QWidget* content, QWidget* parent)
    : QWidget(parent)
    , title_(std::move(title))
    , content_(content)
{
    Q_ASSERT(content_);
    setObjectName(QStringLiteral("WorkspacePanel"));
    setAttribute(Qt::WA_StyledBackground);
    setMinimumWidth(228);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    applyHeightRange();

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(1, 1, 1, 1);
    layout->setSpacing(0);
    titleBar_ = new PanelTitleBar(*this);
    layout->addWidget(titleBar_);
    content_->setParent(this);
    layout->addWidget(content_, 1);

    resizeGrip_ = new PanelResizeGrip(*this);
    resizeGrip_->hide();
}

void WorkspacePanel::setFloatingPresentation(bool floating)
{
    if (floatingPresentation_ == floating) {
        return;
    }
    floatingPresentation_ = floating;
    setProperty("floatingPanel", floating);
    resizeGrip_->setVisible(floating);
    resizeGrip_->raise();
    style()->unpolish(this);
    style()->polish(this);
    update();
}

void WorkspacePanel::setHeightRange(int minimum, int maximum)
{
    Q_ASSERT(minimum >= 0);
    Q_ASSERT(maximum > minimum);
    configuredMinimumHeight_ = minimum;
    configuredMaximumHeight_ = maximum;
    applyHeightRange();
}

void WorkspacePanel::applyHeightRange()
{
    // Reset both bounds before applying a new pair. QWidget otherwise adjusts
    // the opposite bound when one crosses it. The same developer-configured
    // range deliberately follows a panel between docked and floating modes.
    setMinimumHeight(0);
    setMaximumHeight(QWIDGETSIZE_MAX);
    setMinimumHeight(configuredMinimumHeight_);
    setMaximumHeight(configuredMaximumHeight_);
    updateGeometry();
}

void WorkspacePanel::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (resizeGrip_) {
        resizeGrip_->move(width() - resizeGrip_->width(),
            height() - resizeGrip_->height());
        resizeGrip_->raise();
    }
}

} // namespace imageeditor::ui
