#include "imageeditor/ui/OverlayDockWorkspace.hpp"

#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/WorkspacePanel.hpp"
#include "imageeditor/ui/RulerStrip.hpp"
#include "imageeditor/ui/PopupOwnership.hpp"

#include <QColor>
#include <QCursor>
#include <QDebug>
#include <QEnterEvent>
#include <QEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPalette>
#include <QRegion>
#include <QResizeEvent>
#include <QShowEvent>
#include <QSizePolicy>
#include <QSplitter>
#include <QStackedLayout>
#include <QStyle>
#include <QTimer>
#include <QToolBar>
#include <QWindow>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace imageeditor::ui {
namespace {

using PanelDockSide = OverlayDockWorkspace::PanelDockSide;
using PanelPlacement = OverlayDockWorkspace::PanelPlacement;

QColor translucent(QColor color, int alpha)
{
    color.setAlpha(alpha);
    return color;
}

class PanelOverlaySurface final : public QWidget {
public:
    explicit PanelOverlaySurface(QWidget* parent)
        : QWidget(parent, Qt::Window | Qt::FramelessWindowHint
                | Qt::NoDropShadowWindowHint)
    {
        setProperty("vulkanaNativeChildSurface", true);
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_NoSystemBackground);
        setAutoFillBackground(false);
    }

protected:
    void paintEvent(QPaintEvent* event) override
    {
        // Replace old backing-store pixels with alpha-zero before children are
        // composited. The native surface never moves; old/new child footprints
        // can therefore be cleared and redrawn in one Wayland commit.
        QPainter painter(this);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        for (const QRect& rect : event->region()) {
            painter.fillRect(rect, Qt::transparent);
        }
    }
};

class EmptyDockShelf final : public QWidget {
public:
    explicit EmptyDockShelf(PanelDockSide side, QWidget* parent)
        : QWidget(parent)
        , side_(side)
    {
        setObjectName(side == PanelDockSide::Left
                ? QStringLiteral("LeftPanelDockShelf")
                : QStringLiteral("RightPanelDockShelf"));
        setToolTip(QStringLiteral("Drag a panel here to dock it"));
        setAttribute(Qt::WA_StyledBackground);
    }

protected:
    void paintEvent(QPaintEvent* event) override
    {
        QPainter painter(this);
        painter.setClipRegion(event->region());
        painter.setRenderHint(QPainter::Antialiasing);
        painter.fillRect(rect(), palette().color(QPalette::Window));

        const bool highlighted = parentWidget()
            && parentWidget()->property("dropTarget").toBool();
        painter.setPen(QPen(palette().color(highlighted
                ? QPalette::Highlight : QPalette::Mid),
            highlighted ? 2.0 : 1.0));
        painter.drawRoundedRect(rect().adjusted(1, 1, -2, -2), 8, 8);

        painter.save();
        painter.translate(width() / 2.0, height() / 2.0);
        painter.rotate(side_ == PanelDockSide::Left ? -90.0 : 90.0);
        QFont shelfFont = font();
        shelfFont.setPointSizeF(7.5);
        shelfFont.setWeight(QFont::DemiBold);
        painter.setFont(shelfFont);
        painter.setPen(palette().color(highlighted
                ? QPalette::Text : QPalette::PlaceholderText));
        painter.drawText(QRectF(-55.0, -12.0, 110.0, 24.0),
            Qt::AlignCenter, QStringLiteral("DOCK PANEL"));
        painter.restore();
    }

private:
    PanelDockSide side_;
};

class PanelDragProxy final : public QWidget {
public:
    explicit PanelDragProxy(QWidget* parent)
        : QWidget(parent)
    {
        setObjectName(QStringLiteral("WorkspacePanelDragProxy"));
        setAttribute(Qt::WA_TransparentForMouseEvents);
        hide();
    }

    void setTitle(const QString& title)
    {
        title_ = title;
        update();
    }

    QSize sizeHint() const override
    {
        QFont titleFont = font();
        titleFont.setWeight(QFont::DemiBold);
        const QFontMetrics metrics(titleFont);
        return {std::clamp(metrics.horizontalAdvance(title_) + 52, 96, 200),
            std::max(32, metrics.height() + 12)};
    }

protected:
    void paintEvent(QPaintEvent* event) override
    {
        QPainter painter(this);
        painter.setClipRegion(event->region());
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(palette().color(QPalette::Highlight), 1.5));
        painter.setBrush(translucent(palette().color(QPalette::AlternateBase), 238));
        painter.drawRoundedRect(rect().adjusted(1, 1, -2, -2), 9, 9);
        QFont proxyFont = font();
        proxyFont.setWeight(QFont::DemiBold);
        painter.setFont(proxyFont);
        painter.setPen(palette().color(QPalette::Text));
        const auto textRect = rect().adjusted(13, 0, -36, 0);
        painter.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter,
            painter.fontMetrics().elidedText(title_, Qt::ElideRight, textRect.width()));
        painter.setPen(Qt::NoPen);
        painter.setBrush(palette().color(QPalette::Highlight));
        const QPoint center(width() - 20, height() / 2);
        for (int column = -1; column <= 1; ++column) {
            for (int row = -1; row <= 1; row += 2) {
                painter.drawEllipse(
                    QPointF(center + QPoint {column * 4, row * 3}), 1.4, 1.4);
            }
        }
    }

private:
    QString title_;
};

class PanelDockDropIndicator final : public QWidget {
public:
    explicit PanelDockDropIndicator(QWidget* parent)
        : QWidget(parent)
    {
        setObjectName(QStringLiteral("PanelDockDropIndicator"));
        setAttribute(Qt::WA_TransparentForMouseEvents);
        hide();
    }

    void setTarget(PanelDockSide side, bool hasDockedPeers,
        std::vector<int> spacerYs, int insertionLineY,
        bool currentPosition)
    {
        tabTarget_ = false;
        setProperty("dropKind", QStringLiteral("dock"));
        side_ = side;
        hasDockedPeers_ = hasDockedPeers;
        spacerYs_ = std::move(spacerYs);
        insertionLineY_ = insertionLineY;
        currentPosition_ = currentPosition;
        update();
    }
    void setTabTarget(int lineX) {
        tabTarget_ = true;
        tabLineX_ = lineX;
        setProperty("dropKind", lineX < 0 ? QStringLiteral("new-tab") : QStringLiteral("tab-insertion"));
        update();
    }

protected:
    void paintEvent(QPaintEvent* event) override
    {
        QPainter painter(this);
        painter.setClipRegion(event->region());
        painter.setRenderHint(QPainter::Antialiasing);
        if (tabTarget_) {
            const auto accent = palette().color(QPalette::Highlight);
            painter.setPen(QPen(accent, 3));
            if (tabLineX_ >= 0) {
                painter.drawLine(tabLineX_, 2, tabLineX_, height() - 3);
            } else {
                painter.setBrush(translucent(accent, 30));
                painter.drawRoundedRect(rect().adjusted(3, 3, -4, -4), 8, 8);
                const QRect label(width() / 2 - 48, height() / 2 - 15, 96, 30);
                painter.setBrush(accent); painter.drawRoundedRect(label, 6, 6);
                painter.setPen(palette().color(QPalette::HighlightedText));
                painter.drawText(label, Qt::AlignCenter, tr("New tab"));
            }
            return;
        }
        const QColor accent = palette().color(currentPosition_
            ? QPalette::PlaceholderText : QPalette::Highlight);
        painter.setPen(QPen(accent, currentPosition_ ? 2.0 : 3.0));
        painter.setBrush(translucent(accent, currentPosition_ ? 5 : hasDockedPeers_ ? 8 : 14));
        painter.drawRoundedRect(rect().adjusted(2, 2, -3, -3), 9, 9);

        QFont labelFont = font();
        labelFont.setPointSizeF(8.0);
        labelFont.setWeight(QFont::DemiBold);
        painter.setFont(labelFont);

        if (hasDockedPeers_ && insertionLineY_ >= 0) {
            for (const int spacerY : spacerYs_) {
                if (spacerY == insertionLineY_) {
                    continue;
                }
                const int lineY = std::clamp(spacerY, 5, height() - 6);
                painter.setPen(QPen(translucent(palette().color(QPalette::PlaceholderText), 105), 2.0,
                    Qt::SolidLine, Qt::RoundCap));
                painter.drawLine(13, lineY, width() - 14, lineY);
            }

            const int centerY = std::clamp(
                insertionLineY_, 12, std::max(12, height() - 13));
            const QRect spacerRect(7, centerY - 11,
                std::max(1, width() - 14), 22);
            painter.setPen(QPen(palette().color(currentPosition_
                    ? QPalette::Mid : QPalette::Highlight), 1.5));
            painter.setBrush(translucent(palette().color(currentPosition_
                    ? QPalette::Button : QPalette::Highlight), currentPosition_ ? 230 : 255));
            painter.drawRoundedRect(spacerRect, 7, 7);
            painter.setPen(palette().color(currentPosition_
                    ? QPalette::Text : QPalette::HighlightedText));
            painter.drawText(spacerRect, Qt::AlignCenter,
                currentPosition_ ? QStringLiteral("CURRENT POSITION")
                                 : QStringLiteral("DROP HERE"));
            return;
        }

        const QString label = currentPosition_
            ? QStringLiteral("CURRENT POSITION")
            : (side_ == PanelDockSide::Left
                ? QStringLiteral("DOCK LEFT") : QStringLiteral("DOCK RIGHT"));
        const QSize labelSize {
            std::min(126, std::max(78, width() - 20)), 32,
        };
        const int labelY = (height() - labelSize.height()) / 2;
        const QRect labelRect(
            QPoint {(width() - labelSize.width()) / 2, labelY},
            labelSize);
        painter.setPen(QPen(palette().color(currentPosition_
                ? QPalette::Mid : QPalette::Highlight), 1.0));
        painter.setBrush(translucent(palette().color(QPalette::Button), 238));
        painter.drawRoundedRect(labelRect, 8, 8);
        painter.setPen(palette().color(QPalette::Text));
        painter.drawText(labelRect, Qt::AlignCenter, label);
    }

private:
    PanelDockSide side_ {PanelDockSide::Right};
    bool hasDockedPeers_ {false};
    std::vector<int> spacerYs_;
    int insertionLineY_ {-1};
    bool currentPosition_ {false};
    bool tabTarget_ {false};
    int tabLineX_ {-1};
};

class PanelDockColumn final : public QWidget {
public:
    PanelDockColumn(PanelDockSide side, QWidget* parent)
        : QWidget(parent)
        , side_(side)
    {
        setObjectName(side == PanelDockSide::Left
                ? QStringLiteral("OverlayLeftPanelCard")
                : QStringLiteral("OverlayPanelCard"));
        setAttribute(Qt::WA_StyledBackground);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);

        layout_ = new QHBoxLayout(this);
        layout_->setContentsMargins(0, 0, 0, 0);
        layout_->setSpacing(0);

        contentHost_ = new QWidget(this);
        contentHost_->setObjectName(QStringLiteral("PanelDockColumnContent"));
        stackedLayout_ = new QStackedLayout(contentHost_);
        stackedLayout_->setContentsMargins(0, 0, 0, 0);
        stackedLayout_->setStackingMode(QStackedLayout::StackOne);

        splitter_ = new QSplitter(Qt::Vertical, contentHost_);
        splitter_->setObjectName(side == PanelDockSide::Left
                ? QStringLiteral("LeftPanelSplitter")
                : QStringLiteral("RightPanelSplitter"));
        splitter_->setChildrenCollapsible(false);
        splitter_->setHandleWidth(8);
        shelf_ = new EmptyDockShelf(side, contentHost_);
        stackedLayout_->addWidget(splitter_);
        stackedLayout_->addWidget(shelf_);
        layout_->addWidget(contentHost_, 1);
        setHasPanels(false);
    }

    [[nodiscard]] QSplitter* splitter() const noexcept { return splitter_; }
    [[nodiscard]] QRect contentGeometryInParent() const
    {
        return {contentHost_->mapTo(parentWidget(), QPoint {}),
            contentHost_->size()};
    }

    void setHasPanels(bool hasPanels)
    {
        hasPanels_ = hasPanels;
        stackedLayout_->setCurrentWidget(hasPanels
                ? static_cast<QWidget*>(splitter_)
                : static_cast<QWidget*>(shelf_));
        setProperty("emptyShelf", !hasPanels);
        update();
    }

    void setDockContentVisible(bool visible)
    {
        contentHost_->setVisible(visible);
    }

    void setToolRail(QToolBar* toolRail)
    {
        if (toolRail_ == toolRail) {
            return;
        }
        if (toolRail_) {
            layout_->removeWidget(toolRail_);
        }
        toolRail_ = toolRail;
        if (toolRail_) {
            toolRail_->setParent(this);
            layout_->insertWidget(0, toolRail_);
            toolRail_->show();
        }
    }

    void removeToolRail(QToolBar* toolRail)
    {
        if (!toolRail || toolRail_ != toolRail) {
            return;
        }
        layout_->removeWidget(toolRail_);
        toolRail_->hide();
        toolRail_ = nullptr;
    }

    void setDropHighlighted(bool highlighted)
    {
        if (property("dropTarget").toBool() == highlighted) {
            return;
        }
        setProperty("dropTarget", highlighted);
        style()->unpolish(this);
        style()->polish(this);
        shelf_->update();
        update();
    }

private:
    PanelDockSide side_;
    QHBoxLayout* layout_ {nullptr};
    QWidget* contentHost_ {nullptr};
    QStackedLayout* stackedLayout_ {nullptr};
    QSplitter* splitter_ {nullptr};
    EmptyDockShelf* shelf_ {nullptr};
    QToolBar* toolRail_ {nullptr};
    bool hasPanels_ {false};
};

class SideResizeHandle final : public QWidget {
public:
    SideResizeHandle(OverlayDockWorkspace& workspace,
        PanelDockSide side, QWidget* parent)
        : QWidget(parent)
        , workspace_(workspace)
        , side_(side)
    {
        setObjectName(side == PanelDockSide::Left
                ? QStringLiteral("OverlayLeftPanelResizeHandle")
                : QStringLiteral("OverlayPanelResizeHandle"));
        setCursor(Qt::SizeHorCursor);
        setToolTip(QStringLiteral("Drag to resize panels"));
        setMouseTracking(true);
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
        pressGlobalX_ = event->globalPosition().x();
        initialWidth_ = side_ == PanelDockSide::Left
            ? workspace_.leftPanelWidth() : workspace_.panelWidth();
        update();
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (!resizing_) {
            QWidget::mouseMoveEvent(event);
            return;
        }
        const int movement = static_cast<int>(std::lround(
            event->globalPosition().x() - pressGlobalX_));
        if (side_ == PanelDockSide::Left) {
            workspace_.setLeftPanelWidth(initialWidth_ + movement);
        } else {
            workspace_.setPanelWidth(initialWidth_ - movement);
        }
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (!resizing_ || event->button() != Qt::LeftButton) {
            QWidget::mouseReleaseEvent(event);
            return;
        }
        cancelResize();
        event->accept();
    }

    void paintEvent(QPaintEvent* event) override
    {
        QPainter painter(this);
        painter.setClipRegion(event->region());
        if (hovered_ || resizing_) {
            painter.fillRect(rect(), translucent(palette().color(QPalette::Highlight),
                resizing_ ? 92 : 58));
        }
        const QColor divider = palette().color(hovered_ || resizing_
            ? QPalette::Highlight : QPalette::Mid);
        painter.fillRect(QRect(width() / 2, 0,
                             resizing_ || hovered_ ? 2 : 1, height()), divider);
    }

private:
    void cancelResize()
    {
        if (!resizing_) {
            return;
        }
        resizing_ = false;
        if (QWidget::mouseGrabber() == this) {
            releaseMouse();
        }
        update();
    }

    OverlayDockWorkspace& workspace_;
    PanelDockSide side_;
    qreal pressGlobalX_ {0.0};
    int initialWidth_ {0};
    bool hovered_ {false};
    bool resizing_ {false};
};

template <typename T>
bool eraseOne(std::vector<T*>& values, const T* value)
{
    const auto found = std::find(values.begin(), values.end(), value);
    if (found == values.end()) {
        return false;
    }
    values.erase(found);
    return true;
}

} // namespace

OverlayDockWorkspace::OverlayDockWorkspace(
    render::CanvasWindow* canvasWindow, QWidget* parent)
    : QWidget(parent)
    , canvasWindow_(canvasWindow)
{
    Q_ASSERT(canvasWindow_);
    installPopupOwnershipPolicy();
    canvasWindow_->setHostPresentationAvailable(false);
    setObjectName(QStringLiteral("CanvasWorkspace"));
    setAttribute(Qt::WA_StyledBackground);
    setMinimumSize(kMinimumPanelWidth + kMinimumCanvasExposure
            + kResizeHandleHalfWidth + kPanelInset,
        240);

    canvasContainer_ = QWidget::createWindowContainer(canvasWindow_, this);
    canvasContainer_->setObjectName(QStringLiteral("VulkanCanvasContainer"));
    canvasContainer_->setAttribute(Qt::WA_NativeWindow);
    Q_ASSERT(canvasContainer_->testAttribute(Qt::WA_NativeWindow));
    // The shared theme's VulkanCanvasContainer rule owns the fallback surface.
    // A local palette here would retain an old color after a runtime theme change.
    canvasContainer_->setAutoFillBackground(true);
    canvasContainer_->setAttribute(Qt::WA_StyledBackground);
    canvasContainer_->setFocusPolicy(Qt::StrongFocus);
    canvasContainer_->setMinimumSize(320, 240);
    canvasContainer_->installEventFilter(this);

    // This one stable ARGB child surface is the complete panel world. Child
    // geometry may change freely without creating/moving Wayland top-levels or
    // resizing the Vulkan swapchain below it.
    panelOverlay_ = new PanelOverlaySurface(this);
    panelOverlay_->setObjectName(QStringLiteral("PanelOverlaySurface"));
    panelOverlay_->setAttribute(Qt::WA_NativeWindow);
    panelOverlay_->installEventFilter(this);

    auto* leftColumn = new PanelDockColumn(PanelDockSide::Left, panelOverlay_);
    auto* rightColumn = new PanelDockColumn(PanelDockSide::Right, panelOverlay_);
    leftColumn_ = leftColumn;
    rightColumn_ = rightColumn;
    leftSplitter_ = leftColumn->splitter();
    rightSplitter_ = rightColumn->splitter();
    leftColumn_->installEventFilter(this);
    rightColumn_->installEventFilter(this);

    leftResizeHandle_ = new SideResizeHandle(
        *this, PanelDockSide::Left, panelOverlay_);
    rightResizeHandle_ = new SideResizeHandle(
        *this, PanelDockSide::Right, panelOverlay_);
    leftResizeHandle_->setFixedWidth(kResizeHandleWidth);
    rightResizeHandle_->setFixedWidth(kResizeHandleWidth);
    leftResizeHandle_->installEventFilter(this);
    rightResizeHandle_->installEventFilter(this);

    panelDragProxy_ = new PanelDragProxy(panelOverlay_);
    panelDropIndicator_ = new PanelDockDropIndicator(panelOverlay_);
    for (int i = 0; i < 2; ++i) {
        rulers_[i] = new RulerStrip(i == 0 ? RulerStrip::Axis::Horizontal : RulerStrip::Axis::Vertical, panelOverlay_);
        rulers_[i]->setObjectName(i == 0 ? QStringLiteral("HorizontalRuler") : QStringLiteral("VerticalRuler"));
        rulers_[i]->hide();
        rulerDockHints_[i] = new PanelDragProxy(panelOverlay_);
        rulerDockHints_[i]->setObjectName(QStringLiteral("RulerDockHint%1").arg(i));
        rulers_[i]->onDragStarted = [this, i](QPoint global) { draggingRuler_ = i; updateRulerDrag(global); };
        rulers_[i]->onDragMoved = [this](QPoint global) { updateRulerDrag(global); };
        rulers_[i]->onDragFinished = [this](QPoint global, bool cancel) { finishRulerDrag(global, cancel); };
    }

    leftColumn_->hide();
    rightColumn_->hide();
    leftResizeHandle_->hide();
    rightResizeHandle_->hide();
}

OverlayDockWorkspace::~OverlayDockWorkspace()
{
    clearTabCallbacks();
    for (auto* ruler : rulers_) {
        ruler->onDragStarted = {}; ruler->onDragMoved = {}; ruler->onDragFinished = {};
        ruler->cancelDrag();
    }
    const auto clearCallbacks = [](WorkspacePanel* panel) {
        if (!panel) {
            return;
        }
        panel->onDragStarted = {};
        panel->onDragMoved = {};
        panel->onDragFinished = {};
        panel->onResizeStarted = {};
        panel->onResizeMoved = {};
        panel->onResizeFinished = {};
    };
    for (auto* panel : leftPanels_) {
        clearCallbacks(panel);
    }
    for (auto* panel : rightPanels_) {
        clearCallbacks(panel);
    }
    for (auto* panel : floatingPanels_) {
        clearCallbacks(panel);
    }
    for (auto* panel : registeredPanels_) clearCallbacks(panel);

    draggedPanel_ = nullptr;
    draggedTab_ = nullptr;
    resizedPanel_ = nullptr;
    if (auto* grabber = QWidget::mouseGrabber(); grabber && panelOverlay_
        && (grabber == panelOverlay_ || panelOverlay_->isAncestorOf(grabber))) {
        grabber->releaseMouse();
    }
    if (hostWindow_) {
        hostWindow_->removeEventFilter(this);
    }
    if (canvasWindow_) {
        canvasWindow_->setHostPresentationAvailable(false);
    }
}

void OverlayDockWorkspace::addPanel(WorkspacePanel* panel, PanelDockSide side)
{
    if (!panel) {
        return;
    }
    configurePanel(panel);
    registeredPanels_.push_back(panel);
    hiddenPanels_.remove(panel);
    insertDockedPanel(panel, side, -1);
}

void OverlayDockWorkspace::configurePanel(WorkspacePanel* panel)
{
    Q_ASSERT(panel);
    panel->onDragStarted = [this](WorkspacePanel* target,
                                  QPoint global, QPoint offset) {
        beginPanelDrag(target, global, offset);
    };
    panel->onDragMoved = [this](WorkspacePanel* target, QPoint global) {
        movePanelDrag(target, global);
    };
    panel->onDragFinished = [this](WorkspacePanel* target,
                                   QPoint global, bool canceled) {
        finishPanelDrag(target, global, canceled);
    };
    panel->onResizeStarted = [this](WorkspacePanel* target, QPoint global) {
        beginPanelResize(target, global);
    };
    panel->onResizeMoved = [this](WorkspacePanel* target, QPoint global) {
        movePanelResize(target, global);
    };
    panel->onResizeFinished = [this](WorkspacePanel* target, bool canceled) {
        finishPanelResize(target, canceled);
    };
    panel->installEventFilter(this);
    for (auto* child : panel->findChildren<QWidget*>()) {
        child->installEventFilter(this);
    }
}

void OverlayDockWorkspace::removePanelFromPlacements(WorkspacePanel* panel)
{
    eraseOne(leftPanels_, panel);
    eraseOne(rightPanels_, panel);
    eraseOne(floatingPanels_, panel);
}

void OverlayDockWorkspace::insertDockedPanel(
    WorkspacePanel* panel, PanelDockSide side, int index)
{
    if (!panel) {
        return;
    }
    const bool requestedVisible = panelRequestedVisible(panel);
    removePanelFromPlacements(panel);
    auto& panels = side == PanelDockSide::Left ? leftPanels_ : rightPanels_;
    auto* splitter = side == PanelDockSide::Left ? leftSplitter_ : rightSplitter_;
    const int insertion = index < 0
        ? static_cast<int>(panels.size())
        : std::clamp(index, 0, static_cast<int>(panels.size()));
    panels.insert(panels.begin() + insertion, panel);
    panel->setFloatingPresentation(false);
    panel->setParent(splitter);
    splitter->insertWidget(insertion, panel);
    splitter->setCollapsible(insertion, false);
    panel->setVisible(requestedVisible);
    if (panels.size() > 1) {
        splitter->setSizes(QList<int>(static_cast<qsizetype>(panels.size()), 1));
    }
    updatePanelGeometry();
}

void OverlayDockWorkspace::dockPanel(
    WorkspacePanel* panel, PanelDockSide side, int index)
{
    insertDockedPanel(panelFrame(panel), side, index);
}

void OverlayDockWorkspace::floatPanel(
    WorkspacePanel* panel, const QRect& requestedGeometry)
{
    panel = panelFrame(panel);
    if (!panel) {
        return;
    }
    const bool requestedVisible = panelRequestedVisible(panel);
    QRect geometry = requestedGeometry;
    if (!geometry.isValid() || geometry.isEmpty()) {
        QSize floatingSize = panel->size().expandedTo(QSize {310, 240});
        const QSize available = panelOverlay_->size().isValid()
            ? panelOverlay_->size() : size();
        geometry = QRect(QPoint {
                std::max(kPanelInset, (available.width() - floatingSize.width()) / 2),
                std::max(kPanelInset, (available.height() - floatingSize.height()) / 2)},
            floatingSize);
    }

    removePanelFromPlacements(panel);
    floatingPanels_.push_back(panel);
    panel->setParent(panelOverlay_);
    panel->setFloatingPresentation(true);
    panel->setGeometry(clampFloatingGeometry(panel, geometry));
    panel->setVisible(requestedVisible);
    if (requestedVisible) {
        panel->raise();
    }
    updatePanelGeometry();
}

void OverlayDockWorkspace::setPanelVisible(WorkspacePanel* panel, bool visible)
{
    if (!panel) {
        return;
    }
    if (auto* group = tabGroup(panel); group && group->frame != panel) {
        const bool wasHidden = hiddenPanels_.contains(panel);
        if (visible == !wasHidden) return;
        if (visible) hiddenPanels_.remove(panel);
        else hiddenPanels_.insert(panel);
        refreshTabGroup(*group, visible && wasHidden ? panel : nullptr);
        updatePanelGeometry();
        return;
    }
    if (visible) {
        hiddenPanels_.remove(panel);
        panel->show();
        if (panelPlacement(panel) == PanelPlacement::Floating) {
            panel->raise();
        }
    } else {
        hiddenPanels_.insert(panel);
        panel->hide();
    }
    updatePanelGeometry();
}

bool OverlayDockWorkspace::panelVisible(const WorkspacePanel* panel) const
{
    return panel && panelRequestedVisible(panel);
}

void OverlayDockWorkspace::setPanelsSuppressed(bool suppressed)
{
    if (panelsSuppressed_ == suppressed) return;
    if (suppressed) {
        if (draggedPanel_) finishPanelDrag(draggedPanel_, {}, true);
        if (resizedPanel_) finishPanelResize(resizedPanel_, true);
    }
    panelsSuppressed_ = suppressed;
    updateOverlayLayoutAndStacking();
}

OverlayDockWorkspace::PanelPlacement OverlayDockWorkspace::panelPlacement(
    const WorkspacePanel* panel) const
{
    panel = panelFrame(panel);
    if (std::find(leftPanels_.begin(), leftPanels_.end(), panel)
        != leftPanels_.end()) {
        return PanelPlacement::DockedLeft;
    }
    if (std::find(rightPanels_.begin(), rightPanels_.end(), panel)
        != rightPanels_.end()) {
        return PanelPlacement::DockedRight;
    }
    return PanelPlacement::Floating;
}

int OverlayDockWorkspace::dockedPanelIndex(
    const WorkspacePanel* panel) const
{
    const auto placement = panelPlacement(panel);
    if (placement == PanelPlacement::Floating) {
        return -1;
    }
    return panelIndex(panel, placement);
}

QRect OverlayDockWorkspace::floatingPanelGeometry(
    const WorkspacePanel* panel) const
{
    return panel && panelPlacement(panel) == PanelPlacement::Floating
        ? panelFrame(panel)->geometry() : QRect {};
}

QByteArray OverlayDockWorkspace::saveDockedPanelSizes(
    PanelDockSide side) const
{
    const auto* splitter = side == PanelDockSide::Left
        ? leftSplitter_ : rightSplitter_;
    return splitter ? splitter->saveState() : QByteArray {};
}

bool OverlayDockWorkspace::restoreDockedPanelSizes(
    PanelDockSide side, const QByteArray& state)
{
    auto* splitter = side == PanelDockSide::Left
        ? leftSplitter_ : rightSplitter_;
    if (!splitter || state.isEmpty()) {
        return false;
    }
    return splitter->restoreState(state);
}

void OverlayDockWorkspace::setToolRail(
    QToolBar* toolRail, ToolRailPlacement placement)
{
    if (!toolRail) {
        return;
    }
    if (toolRail_ == toolRail && toolRailPlacement_ == placement) {
        updatePanelGeometry();
        return;
    }
    if (toolRail_) {
        takeToolRail(toolRail_);
    }

    toolRail_ = toolRail;
    toolRailPlacement_ = placement;
    toolRail_->removeEventFilter(this);
    toolRail_->installEventFilter(this);
    switch (placement) {
    case ToolRailPlacement::Left:
        static_cast<PanelDockColumn*>(leftColumn_)->setToolRail(toolRail_);
        break;
    case ToolRailPlacement::Right:
        static_cast<PanelDockColumn*>(rightColumn_)->setToolRail(toolRail_);
        break;
    case ToolRailPlacement::Top:
    case ToolRailPlacement::Bottom:
        toolRail_->setParent(panelOverlay_);
        toolRail_->show();
        break;
    }
    updatePanelGeometry();
}

void OverlayDockWorkspace::takeToolRail(QToolBar* toolRail)
{
    if (!toolRail || toolRail_ != toolRail) {
        return;
    }
    static_cast<PanelDockColumn*>(leftColumn_)->removeToolRail(toolRail);
    static_cast<PanelDockColumn*>(rightColumn_)->removeToolRail(toolRail);
    toolRail->hide();
    toolRail->setParent(panelOverlay_);
    toolRail->removeEventFilter(this);
    toolRail_ = nullptr;
    updatePanelGeometry();
}

bool OverlayDockWorkspace::hasToolRail(
    const QToolBar* toolRail) const noexcept
{
    return toolRail && toolRail_ == toolRail;
}

int OverlayDockWorkspace::panelWidth() const noexcept
{
    return effectiveRightWidth_;
}

void OverlayDockWorkspace::setPanelWidth(int width)
{
    requestedRightWidth_ = std::clamp(
        width, kMinimumPanelWidth, kMaximumPanelWidth);
    updatePanelGeometry();
}

int OverlayDockWorkspace::leftPanelWidth() const noexcept
{
    return effectiveLeftWidth_;
}

void OverlayDockWorkspace::setLeftPanelWidth(int width)
{
    requestedLeftWidth_ = std::clamp(
        width, kMinimumPanelWidth, kMaximumPanelWidth);
    updatePanelGeometry();
}

void OverlayDockWorkspace::setTransientOverlayInteractionRegion(
    const QRegion& region)
{
    if (transientInteractionRegion_ == region) {
        return;
    }
    transientInteractionRegion_ = region;
    updatePanelGeometry();
}

void OverlayDockWorkspace::setContextOverlayInteractionRegion(const QRegion& region)
{
    // A queued text-cache/viewport update can reposition and raise its context
    // toolbar even while a dialog is open. Modality owns the top of this plane,
    // including when the context footprint itself has not changed.
    if (modalOverlay_) modalOverlay_->raise();
    if(contextInteractionRegion_==region)return;
    contextInteractionRegion_=region;updatePanelGeometry();
}

void OverlayDockWorkspace::setModalOverlay(QWidget* overlay)
{
    if (modalOverlay_ && modalOverlay_ != overlay) modalOverlay_->hide();
    modalOverlay_ = overlay;
    if (overlay) {
        overlay->setGeometry(rect());
        overlay->show();
    }
    updateOverlayLayoutAndStacking();
}

void OverlayDockWorkspace::setPassiveOverlay(QWidget* overlay, bool visible)
{
    Q_ASSERT(!overlay || overlay->parentWidget() == panelOverlay_);
    if (passiveOverlay_ == overlay && passiveOverlayRequestedVisible_ == visible)
        return;
    if (passiveOverlay_ && passiveOverlay_ != overlay) passiveOverlay_->hide();
    passiveOverlay_ = overlay;
    passiveOverlayRequestedVisible_ = overlay && visible;
    if (overlay) {
        overlay->setAttribute(Qt::WA_TransparentForMouseEvents);
        overlay->setFocusPolicy(Qt::NoFocus);
    }
    updateOverlayLayoutAndStacking();
}

void OverlayDockWorkspace::setViewModeOverlay(QWidget* overlay, bool visible)
{
    Q_ASSERT(!overlay || overlay->parentWidget() == panelOverlay_);
    if (viewModeOverlay_ && viewModeOverlay_ != overlay) viewModeOverlay_->hide();
    viewModeOverlay_ = overlay;
    viewModeOverlayRequestedVisible_ = overlay && visible;
    updateOverlayLayoutAndStacking();
}

int OverlayDockWorkspace::panelIndex(
    const WorkspacePanel* panel, PanelPlacement placement) const
{
    panel = panelFrame(panel);
    const std::vector<WorkspacePanel*>* panels = nullptr;
    switch (placement) {
    case PanelPlacement::DockedLeft: panels = &leftPanels_; break;
    case PanelPlacement::DockedRight: panels = &rightPanels_; break;
    case PanelPlacement::Floating: panels = &floatingPanels_; break;
    }
    const auto found = std::find(panels->begin(), panels->end(), panel);
    return found == panels->end()
        ? -1 : static_cast<int>(std::distance(panels->begin(), found));
}

bool OverlayDockWorkspace::hasVisibleDockedPanel(PanelDockSide side) const
{
    if (panelsSuppressed_) return false;
    const auto& panels = side == PanelDockSide::Left
        ? leftPanels_ : rightPanels_;
    return std::any_of(panels.begin(), panels.end(),
        [this](const WorkspacePanel* panel) {
            return panelRequestedVisible(panel);
        });
}

bool OverlayDockWorkspace::panelRequestedVisible(
    const WorkspacePanel* panel) const
{
    if (const auto* group = tabGroup(panel); group && group->frame == panel)
        return std::any_of(group->panels.begin(), group->panels.end(),
            [this](const WorkspacePanel* member) { return !hiddenPanels_.contains(member); });
    return panel && !hiddenPanels_.contains(panel);
}

QRect OverlayDockWorkspace::clampFloatingGeometry(
    const WorkspacePanel* panel, const QRect& requested) const
{
    const QSize available = panelOverlay_->size().isValid()
        ? panelOverlay_->size() : size();
    const int availableWidth = std::max(1, available.width() - 2 * kPanelInset);
    const int availableHeight = std::max(1, available.height() - 2 * kPanelInset);
    const int minimumWidth = std::min(kMinimumPanelWidth, availableWidth);
    const int configuredMinimumHeight = panel
        ? panel->configuredMinimumHeight() : 160;
    const int configuredMaximumHeight = panel
        ? panel->configuredMaximumHeight() : QWIDGETSIZE_MAX;
    const int minimumHeight = std::min(
        configuredMinimumHeight, availableHeight);
    const int maximumHeight = std::max(minimumHeight,
        std::min(configuredMaximumHeight, availableHeight));
    const int panelWidth = std::clamp(
        requested.width(), minimumWidth, availableWidth);
    const int panelHeight = std::clamp(
        requested.height(), minimumHeight, maximumHeight);
    const int maximumX = std::max(kPanelInset,
        available.width() - kPanelInset - panelWidth);
    // The body may extend below the workspace, but the complete header remains
    // available to pull it back up or dock it below another panel.
    const int visibleHeader = std::min(panelHeight, panel ? panel->headerExtent() : 42);
    const int maximumY = std::max(kPanelInset,
        available.height() - kPanelInset - visibleHeader);
    return {
        std::clamp(requested.x(), kPanelInset, maximumX),
        std::clamp(requested.y(), kPanelInset, maximumY),
        panelWidth,
        panelHeight,
    };
}

void OverlayDockWorkspace::clampFloatingPanels()
{
    for (auto* panel : floatingPanels_) {
        const QRect clamped = clampFloatingGeometry(panel, panel->geometry());
        if (panel->geometry() != clamped) {
            panel->setGeometry(clamped);
        }
    }
}

void OverlayDockWorkspace::beginPanelDrag(
    WorkspacePanel* panel, QPoint, QPoint pressOffset)
{
    if (!panel || draggedPanel_ || panelsSuppressed_) {
        return;
    }
    draggedPanel_ = panel;
    dragOrigin_ = panelPlacement(panel);
    dragOriginIndex_ = panelIndex(panel, dragOrigin_);
    dragOriginGeometry_ = panel->geometry();
    dragOffset_ = pressOffset;

    if (dragOrigin_ != PanelPlacement::Floating || draggedTab_) {
        auto* proxy = static_cast<PanelDragProxy*>(panelDragProxy_);
        proxy->setTitle(draggedTab_ ? draggedTab_->title() : panel->title());
        proxy->setGeometry(QRect(
            panelOverlay_->mapFromGlobal(panel->mapToGlobal(QPoint {})),
            proxy->sizeHint()));
        proxy->show();
        proxy->raise();
    } else {
        panel->raise();
    }
    updatePanelGeometry();
}

void OverlayDockWorkspace::movePanelDrag(
    WorkspacePanel* panel, QPoint globalPosition)
{
    if (!panel || draggedPanel_ != panel) {
        return;
    }
    const QPoint overlayPosition = panelOverlay_->mapFromGlobal(globalPosition);
    QRect previous;
    if (dragOrigin_ == PanelPlacement::Floating && !draggedTab_) {
        const QRect moved(overlayPosition - dragOffset_, panel->size());
        previous = panel->geometry();
        panel->setGeometry(clampFloatingGeometry(panel, moved));
        panel->raise();
    } else {
        // Keep the drag chip beside the pointer so it does not cover the exact
        // insertion gap or middle-drop target the user is aiming at.
        previous = panelDragProxy_->geometry();
        QRect moved(overlayPosition + QPoint(12, 12), panelDragProxy_->size());
        const int maximumX = std::max(kPanelInset,
            panelOverlay_->width() - kPanelInset - moved.width());
        const int maximumY = std::max(kPanelInset,
            panelOverlay_->height() - kPanelInset - moved.height());
        moved.moveLeft(std::clamp(moved.left(), kPanelInset, maximumX));
        moved.moveTop(std::clamp(moved.top(), kPanelInset, maximumY));
        panelDragProxy_->setGeometry(moved);
        panelDragProxy_->raise();
    }
    updateDropHighlights(overlayPosition);
    const QRect current = dragOrigin_ == PanelPlacement::Floating && !draggedTab_
        ? panel->geometry() : panelDragProxy_->geometry();
    if (previous != current) {
        updatePanelGeometry();
    }
}

void OverlayDockWorkspace::finishPanelDrag(
    WorkspacePanel* panel, QPoint globalPosition, bool canceled)
{
    if (!panel || draggedPanel_ != panel) {
        return;
    }
    const auto origin = dragOrigin_;
    const QRect originGeometry = dragOriginGeometry_;
    const QPoint overlayPosition = panelOverlay_->mapFromGlobal(globalPosition);
    const auto tabDrop = canceled ? TabDrop{} : tabDropAt(overlayPosition);
    QPointer<WorkspacePanel> tabTarget(tabDrop.target);
    QPointer<WorkspacePanel> movedPanel(draggedTab_ ? draggedTab_ : panel);
    const bool individualTab = draggedTab_ != nullptr;
    bool dropLeft = !canceled && panelDragTouchesDropZone(
        PanelDockSide::Left, overlayPosition);
    bool dropRight = !canceled && panelDragTouchesDropZone(
        PanelDockSide::Right, overlayPosition);
    if (dropLeft && dropRight) {
        if (overlayPosition.x() < panelOverlay_->width() / 2) {
            dropRight = false;
        } else {
            dropLeft = false;
        }
    }
    const auto dropSide = dropLeft
        ? PanelDockSide::Left : PanelDockSide::Right;
    const int insertionIndex = dropLeft || dropRight
        ? dockInsertionIndex(dropSide, overlayPosition) : -1;
    const bool currentPosition = (dropLeft || dropRight)
        && isCurrentDockPosition(dropSide, insertionIndex);

    const bool originWasDocked = origin != PanelPlacement::Floating;
    QRect requestedFloatingGeometry;
    if (originWasDocked || individualTab) {
        const QSize floatingSize {
            std::clamp(panel->width(), 310, 460),
            std::clamp(panel->height(), 220, 560),
        };
        requestedFloatingGeometry = clampFloatingGeometry(panel, QRect(
            overlayPosition - dragOffset_, floatingSize));
    }

    draggedPanel_ = nullptr;
    draggedTab_ = nullptr;
    panelDragProxy_->hide();
    panelDropIndicator_->hide();
    static_cast<PanelDockColumn*>(leftColumn_)->setDropHighlighted(false);
    static_cast<PanelDockColumn*>(rightColumn_)->setDropHighlighted(false);

    if (canceled) {
        if (!originWasDocked && !individualTab) {
            panel->setGeometry(clampFloatingGeometry(panel, originGeometry));
        }
    }
    updatePanelGeometry();

    if (canceled) {
        return;
    }
    if (tabTarget) {
        QTimer::singleShot(0, this, [this, movedPanel, tabTarget, index = tabDrop.index] {
            if (movedPanel && tabTarget) tabifyPanel(movedPanel, tabTarget, index);
        });
        return;
    }
    if (currentPosition) {
        return;
    }
    QPointer<WorkspacePanel> guardedPanel(movedPanel);
    if (dropLeft || dropRight) {
        QTimer::singleShot(0, this,
            [this, guardedPanel, dropSide, insertionIndex, individualTab] {
            if (guardedPanel) {
                if (individualTab) detachPanel(guardedPanel);
                dockPanel(guardedPanel, dropSide, insertionIndex);
            }
        });
    } else if (originWasDocked || individualTab) {
        QTimer::singleShot(0, this,
            [this, guardedPanel, requestedFloatingGeometry, individualTab] {
                if (guardedPanel) {
                    if (individualTab) detachPanel(guardedPanel);
                    floatPanel(guardedPanel, requestedFloatingGeometry);
                }
            });
    }
}

void OverlayDockWorkspace::beginPanelResize(
    WorkspacePanel* panel, QPoint pressGlobal)
{
    if (!panel || panelPlacement(panel) != PanelPlacement::Floating
        || resizedPanel_ || panelsSuppressed_) {
        return;
    }
    resizedPanel_ = panel;
    resizePressGlobal_ = pressGlobal;
    resizeInitialSize_ = panel->size();
    panel->raise();
}

void OverlayDockWorkspace::movePanelResize(
    WorkspacePanel* panel, QPoint globalPosition)
{
    if (!panel || resizedPanel_ != panel) {
        return;
    }
    const QPoint movement = globalPosition - resizePressGlobal_;
    const QRect requested(panel->pos(), QSize {
        resizeInitialSize_.width() + movement.x(),
        resizeInitialSize_.height() + movement.y(),
    });
    const QRect clamped = clampFloatingGeometry(panel, requested);
    if (panel->geometry() != clamped) {
        panel->setGeometry(clamped);
        panel->raise();
        updatePanelGeometry();
    }
}

void OverlayDockWorkspace::finishPanelResize(
    WorkspacePanel* panel, bool canceled)
{
    if (!panel || resizedPanel_ != panel) {
        return;
    }
    if (canceled) {
        panel->resize(resizeInitialSize_);
    }
    resizedPanel_ = nullptr;
    updatePanelGeometry();
}

void OverlayDockWorkspace::updateColumnContents()
{
    // Hide floating frames, not their individual tabs. Dock frames stay in
    // their unchanged splitter; hiding its host preserves all saved sizes.
    for (auto* panel : floatingPanels_)
        panel->setVisible(!panelsSuppressed_ && panelRequestedVisible(panel));
    const bool leftHasPanels = hasVisibleDockedPanel(PanelDockSide::Left);
    const bool rightHasPanels = hasVisibleDockedPanel(PanelDockSide::Right);
    // Empty adoption shelves are drag affordances, not permanent chrome.
    // Internally floating panels remain directly movable; both shelves appear
    // as soon as one of them is actively dragged.
    const bool adoptionNeeded = draggedPanel_ != nullptr;
    const bool toolRailOnLeft = toolRail_
        && toolRailPlacement_ == ToolRailPlacement::Left;
    const bool toolRailOnRight = toolRail_
        && toolRailPlacement_ == ToolRailPlacement::Right;
    const bool showLeft = leftHasPanels || toolRailOnLeft || adoptionNeeded;
    const bool showRight = rightHasPanels || toolRailOnRight || adoptionNeeded;

    auto* leftColumn = static_cast<PanelDockColumn*>(leftColumn_);
    auto* rightColumn = static_cast<PanelDockColumn*>(rightColumn_);
    leftColumn->setHasPanels(leftHasPanels);
    rightColumn->setHasPanels(rightHasPanels);
    leftColumn->setDockContentVisible(leftHasPanels || adoptionNeeded);
    rightColumn->setDockContentVisible(rightHasPanels || adoptionNeeded);
    leftColumn_->setVisible(showLeft);
    rightColumn_->setVisible(showRight);
    leftResizeHandle_->setVisible(leftHasPanels);
    rightResizeHandle_->setVisible(rightHasPanels);
}

void OverlayDockWorkspace::updateDropHighlights(QPoint overlayPosition)
{
    if (const auto drop = tabDropAt(overlayPosition); drop.target) {
        static_cast<PanelDockColumn*>(leftColumn_)->setDropHighlighted(false);
        static_cast<PanelDockColumn*>(rightColumn_)->setDropHighlighted(false);
        auto* indicator = static_cast<PanelDockDropIndicator*>(panelDropIndicator_);
        indicator->setTabTarget(drop.lineX);
        indicator->setGeometry(drop.area);
        indicator->show(); indicator->raise();
        return;
    }
    const QRect leftDropZone = dockDropZone(PanelDockSide::Left);
    const QRect rightDropZone = dockDropZone(PanelDockSide::Right);
    bool overLeft = panelDragTouchesDropZone(
        PanelDockSide::Left, overlayPosition);
    bool overRight = panelDragTouchesDropZone(
        PanelDockSide::Right, overlayPosition);
    if (overLeft && overRight) {
        if (overlayPosition.x() < panelOverlay_->width() / 2) {
            overRight = false;
        } else {
            overLeft = false;
        }
    }
    static_cast<PanelDockColumn*>(leftColumn_)->setDropHighlighted(overLeft);
    static_cast<PanelDockColumn*>(rightColumn_)->setDropHighlighted(overRight);

    if (!overLeft && !overRight) {
        panelDropIndicator_->hide();
        return;
    }
    const auto side = overLeft ? PanelDockSide::Left : PanelDockSide::Right;
    const QRect target = overLeft ? leftDropZone : rightDropZone;
    const int insertionIndex = dockInsertionIndex(side, overlayPosition);
    static_cast<PanelDockDropIndicator*>(panelDropIndicator_)->setTarget(
        side, hasDockedDropPeers(side),
        dockInsertionGapYs(side, target),
        dockInsertionLineY(side, overlayPosition, target),
        isCurrentDockPosition(side, insertionIndex));
    panelDropIndicator_->setGeometry(target);
    panelDropIndicator_->show();
    panelDropIndicator_->raise();
}

QRect OverlayDockWorkspace::dockDropZone(PanelDockSide side) const
{
    const auto* column = side == PanelDockSide::Left
        ? leftColumn_ : rightColumn_;
    if (!column || column->isHidden()) {
        return {};
    }
    QRect zone = static_cast<const PanelDockColumn*>(column)
        ->contentGeometryInParent();
    if (hasDockedDropPeers(side)) {
        return zone;
    }

    const int expandedWidth = std::min(kEmptyDockDropWidth,
        std::max(1, panelOverlay_->width() - 2 * kPanelInset));
    if (side == PanelDockSide::Left) {
        zone.setRight(std::min(panelOverlay_->width() - kPanelInset - 1,
            zone.left() + expandedWidth - 1));
    } else {
        zone.setLeft(std::max(kPanelInset,
            zone.right() - expandedWidth + 1));
    }
    return zone;
}

bool OverlayDockWorkspace::panelDragTouchesDropZone(
    PanelDockSide side, QPoint overlayPosition) const
{
    const QRect zone = dockDropZone(side);
    if (zone.isEmpty() || !draggedPanel_) {
        return false;
    }
    if (zone.contains(overlayPosition)) {
        return true;
    }

    const QRect dragVisual = dragOrigin_ == PanelPlacement::Floating && !draggedTab_
        ? draggedPanel_->geometry() : panelDragProxy_->geometry();
    if (!dragVisual.intersects(
            QRect(zone.left(), zone.top(), zone.width(), zone.height()))) {
        return false;
    }
    return side == PanelDockSide::Left
        ? dragVisual.left() <= zone.right()
        : dragVisual.right() >= zone.left();
}

bool OverlayDockWorkspace::hasDockedDropPeers(PanelDockSide side) const
{
    const auto& panels = side == PanelDockSide::Left
        ? leftPanels_ : rightPanels_;
    return std::any_of(panels.begin(), panels.end(),
        [this](const WorkspacePanel* panel) {
            return (panel != draggedPanel_ || draggedTab_) && panelRequestedVisible(panel);
        });
}

bool OverlayDockWorkspace::isCurrentDockPosition(
    PanelDockSide side, int insertionIndex) const
{
    const PanelPlacement sidePlacement = side == PanelDockSide::Left
        ? PanelPlacement::DockedLeft : PanelPlacement::DockedRight;
    return !draggedTab_ && dragOrigin_ == sidePlacement
        && insertionIndex == dragOriginIndex_;
}

int OverlayDockWorkspace::dockInsertionIndex(
    PanelDockSide side, QPoint overlayPosition) const
{
    const auto& panels = side == PanelDockSide::Left
        ? leftPanels_ : rightPanels_;
    const QRect dropZone = dockDropZone(side);
    const int targetSlot = closestDockInsertionSlot(
        side, overlayPosition, dropZone);
    int insertionIndex = 0;
    int visibleSlot = 0;
    for (const auto* panel : panels) {
        if (panel == draggedPanel_ && !draggedTab_) {
            continue;
        }
        if (panelRequestedVisible(panel)) {
            if (visibleSlot == targetSlot) {
                return insertionIndex;
            }
            ++visibleSlot;
        }
        ++insertionIndex;
    }
    return insertionIndex;
}

int OverlayDockWorkspace::dockInsertionLineY(PanelDockSide side,
    QPoint overlayPosition, const QRect& dropZone) const
{
    const auto gapYs = dockInsertionGapYs(side, dropZone);
    if (gapYs.empty()) {
        return -1;
    }
    const int slot = closestDockInsertionSlot(
        side, overlayPosition, dropZone);
    return gapYs[static_cast<std::size_t>(std::clamp(
        slot, 0, static_cast<int>(gapYs.size()) - 1))];
}

std::vector<int> OverlayDockWorkspace::dockInsertionGapYs(
    PanelDockSide side, const QRect& dropZone) const
{
    if (!hasDockedDropPeers(side) || dropZone.isEmpty()) {
        return {};
    }
    const auto& panels = side == PanelDockSide::Left
        ? leftPanels_ : rightPanels_;
    std::vector<QRect> peerRects;
    peerRects.reserve(panels.size());
    for (const auto* panel : panels) {
        if ((panel == draggedPanel_ && !draggedTab_) || !panelRequestedVisible(panel)) {
            continue;
        }
        peerRects.emplace_back(
            panel->mapTo(panelOverlay_, QPoint {}), panel->size());
    }
    std::sort(peerRects.begin(), peerRects.end(),
        [](const QRect& first, const QRect& second) {
            return first.top() < second.top();
        });
    if (peerRects.empty()) {
        return {};
    }

    std::vector<int> gapYs;
    gapYs.reserve(peerRects.size() + 1);
    gapYs.push_back(8);
    for (std::size_t index = 1; index < peerRects.size(); ++index) {
        const int globalGapY = (peerRects[index - 1].bottom()
            + peerRects[index].top()) / 2;
        gapYs.push_back(std::clamp(globalGapY - dropZone.top(),
            8, std::max(8, dropZone.height() - 9)));
    }
    gapYs.push_back(std::max(8, dropZone.height() - 9));
    return gapYs;
}

int OverlayDockWorkspace::closestDockInsertionSlot(PanelDockSide side,
    QPoint overlayPosition, const QRect& dropZone) const
{
    const auto gapYs = dockInsertionGapYs(side, dropZone);
    if (gapYs.empty()) {
        return 0;
    }
    const int localY = overlayPosition.y() - dropZone.top();
    int closest = 0;
    int closestDistance = std::abs(localY - gapYs.front());
    for (std::size_t index = 1; index < gapYs.size(); ++index) {
        const int distance = std::abs(localY - gapYs[index]);
        if (distance < closestDistance) {
            closest = static_cast<int>(index);
            closestDistance = distance;
        }
    }
    return closest;
}

bool OverlayDockWorkspace::eventFilter(QObject* watched, QEvent* event)
{
    if (draggedPanel_ && event && event->type() == QEvent::KeyPress
        && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
        finishPanelDrag(draggedPanel_, QCursor::pos(), true);
        return true;
    }
    if (event && event->type() == QEvent::MouseButtonPress) {
        auto* receiver = qobject_cast<QWidget*>(watched);
        const auto found = std::find_if(floatingPanels_.begin(),
            floatingPanels_.end(), [receiver](const WorkspacePanel* panel) {
                return receiver && panel
                    && (receiver == panel || panel->isAncestorOf(receiver));
            });
        if (found != floatingPanels_.end()) {
            auto* panel = *found;
            floatingPanels_.erase(found);
            floatingPanels_.push_back(panel);
            panel->raise();
        }
    }
    if (watched == canvasContainer_ && event
        && event->type() == QEvent::Resize) {
        canvasWindow_->prepareForHostResize();
        canvasContainer_->update();
    }
    if (watched == hostWindow_ && event
        && (event->type() == QEvent::Expose
            || event->type() == QEvent::Show
            || event->type() == QEvent::Hide
            || event->type() == QEvent::WindowStateChange)) {
        const auto* hostWindow = qobject_cast<const QWindow*>(watched);
        canvasWindow_->setHostPresentationAvailable(
            workspaceShown_ && hostWindow && hostWindow->isVisible()
            && !hostWindow->windowStates().testFlag(Qt::WindowMinimized));
    }
    if (!panelGeometryUpdateInProgress_
        && event
        && (event->type() == QEvent::Resize
            || event->type() == QEvent::Show
            || event->type() == QEvent::Hide)) {
        if (watched == panelOverlay_ || watched == leftColumn_
            || watched == rightColumn_
            || watched == toolRail_
            || dynamic_cast<WorkspacePanel*>(watched)) {
            scheduleOverlayUpdate();
        }
    }
    return QWidget::eventFilter(watched, event);
}

void OverlayDockWorkspace::hideEvent(QHideEvent* event)
{
    workspaceShown_ = false;
    canvasWindow_->setHostPresentationAvailable(false);
    QWidget::hideEvent(event);
}

void OverlayDockWorkspace::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    canvasContainer_->setGeometry(rect());
    ensurePanelSurfaceParent();
    clampFloatingPanels();
    updatePanelGeometry();
    scheduleOverlayUpdate();
}

void OverlayDockWorkspace::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    workspaceShown_ = true;
    canvasContainer_->show();
    if (auto* topLevel = window()) {
        auto* nextHostWindow = topLevel->windowHandle();
        if (nextHostWindow != hostWindow_) {
            if (hostWindow_) {
                hostWindow_->removeEventFilter(this);
            }
            hostWindow_ = nextHostWindow;
            if (hostWindow_) {
                hostWindow_->installEventFilter(this);
            }
        }
    }
    canvasWindow_->setHostPresentationAvailable(
        hostWindow_ && hostWindow_->isVisible()
        && !hostWindow_->windowStates().testFlag(Qt::WindowMinimized));
    ensurePanelSurfaceParent();
    updatePanelGeometry();
    QTimer::singleShot(0, this, [this] {
        canvasWindow_->setHostPresentationAvailable(
            workspaceShown_ && hostWindow_ && hostWindow_->isVisible()
            && !hostWindow_->windowStates().testFlag(Qt::WindowMinimized));
        updateOverlayLayoutAndStacking();
        if (!qEnvironmentVariableIsSet("IMAGEEDITOR_OVERLAY_DIAGNOSTICS")) {
            return;
        }
        qInfo() << "Overlay workspace geometry"
                << "workspace" << geometry()
                << "canvas" << canvasContainer_->geometry()
                << "overlay" << panelOverlay_->geometry()
                << "left column" << leftColumn_->geometry()
                << "right column" << rightColumn_->geometry()
                << "right resize handle" << rightResizeHandle_->geometry()
                << "input region" << (panelOverlay_->windowHandle()
                        ? panelOverlay_->windowHandle()->mask() : QRegion {});
    });
}

void OverlayDockWorkspace::ensurePanelSurfaceParent()
{
    if (!panelOverlay_ || !canvasContainer_) {
        return;
    }
    if (!canvasContainer_->windowHandle()) {
        canvasContainer_->winId();
    }
    if (!panelOverlay_->windowHandle()) {
        panelOverlay_->winId();
    }

    auto* canvasSurface = canvasContainer_->windowHandle();
    auto* panelSurface = panelOverlay_->windowHandle();
    auto* siblingParent = canvasSurface ? canvasSurface->parent() : nullptr;
    if (!panelSurface || !siblingParent) {
        return;
    }
    if (panelSurface->parent() != siblingParent) {
        panelSurface->setParent(siblingParent);
    }
    panelOverlay_->resize(size());
    panelSurface->setGeometry(rect());
}

void OverlayDockWorkspace::scheduleOverlayUpdate()
{
    if (overlayUpdatePending_) {
        return;
    }
    overlayUpdatePending_ = true;
    QTimer::singleShot(0, this, [this] {
        overlayUpdatePending_ = false;
        updateOverlayLayoutAndStacking();
    });
}

void OverlayDockWorkspace::updateOverlayLayoutAndStacking()
{
    if (!panelOverlay_ || panelOverlay_->size().isEmpty()) {
        return;
    }
    ensurePanelSurfaceParent();
    updatePanelGeometry();
    if (!panelOverlay_->isVisible()) {
        return;
    }
    panelOverlay_->raise();
    if (viewModeOverlay_ && !viewModeOverlay_->isHidden()) viewModeOverlay_->raise();
    if (toolRail_ && !toolRail_->isHidden()
        && toolRail_->parentWidget() == panelOverlay_) {
        toolRail_->raise();
    }
    for (auto* panel : floatingPanels_) {
        if (panelRequestedVisible(panel)) {
            panel->raise();
        }
    }
    if (draggedPanel_) {
        if (dragOrigin_ == PanelPlacement::Floating && !draggedTab_) {
            draggedPanel_->raise();
        } else {
            panelDragProxy_->raise();
        }
    }
    leftResizeHandle_->raise();
    rightResizeHandle_->raise();
    if (panelDropIndicator_ && !panelDropIndicator_->isHidden()) {
        panelDropIndicator_->raise();
    }
    if (passiveOverlay_ && !passiveOverlay_->isHidden()) passiveOverlay_->raise();
    if (modalOverlay_) {
        modalOverlay_->setGeometry(rect());
        modalOverlay_->raise();
    }
}

QRegion OverlayDockWorkspace::calculateInteractionFootprint() const
{
    if (modalOverlay_) return rect();
    QRegion footprint = transientInteractionRegion_ | contextInteractionRegion_;
    if (welcomeOverlay_ && !welcomeOverlay_->isHidden()) footprint |= welcomeOverlay_->geometry();
    if (viewModeOverlay_ && !viewModeOverlay_->isHidden()) footprint |= viewModeOverlay_->geometry();
    for (int i = 0; i < 2; ++i) {
        if (rulerVisible_[i]) footprint |= rulers_[i]->geometry();
        if (!rulerDockHints_[i]->isHidden()) footprint |= rulerDockHints_[i]->geometry();
    }
    const std::array<QWidget*, 4> edgeWidgets {
        leftColumn_, rightColumn_, leftResizeHandle_, rightResizeHandle_,
    };
    for (const auto* widget : edgeWidgets) {
        if (widget && !widget->isHidden()) {
            footprint |= widget->geometry();
        }
    }
    for (const auto* panel : floatingPanels_) {
        if (panelRequestedVisible(panel) && !panel->isHidden()) {
            footprint |= panel->geometry();
        }
    }
    if (panelDragProxy_ && !panelDragProxy_->isHidden()) {
        footprint |= panelDragProxy_->geometry();
    }
    if (toolRail_ && !toolRail_->isHidden()) {
        footprint |= QRect(toolRail_->mapTo(panelOverlay_, QPoint {}),
            toolRail_->size());
    }
    return footprint.intersected(rect());
}

void OverlayDockWorkspace::updatePanelGeometry()
{
    if (!panelOverlay_ || width() <= 0 || height() <= 0) {
        return;
    }

    updateColumnContents();
    const bool leftHasPanels = hasVisibleDockedPanel(PanelDockSide::Left);
    const bool rightHasPanels = hasVisibleDockedPanel(PanelDockSide::Right);
    const bool adoptionNeeded = draggedPanel_ != nullptr;
    const int availableHeight = std::max(0,
        panelOverlay_->height() - 2 * kPanelInset);

    const int leftToolRailWidth = toolRail_
            && toolRailPlacement_ == ToolRailPlacement::Left
        ? kToolRailWidth : 0;
    const int rightToolRailWidth = toolRail_
            && toolRailPlacement_ == ToolRailPlacement::Right
        ? kToolRailWidth : 0;
    int leftWidth = leftColumn_->isHidden() ? 0
        : (leftHasPanels ? requestedLeftWidth_
                         : (adoptionNeeded ? kEmptyShelfWidth : 0))
            + leftToolRailWidth;
    int rightWidth = rightColumn_->isHidden() ? 0
        : (rightHasPanels ? requestedRightWidth_
                          : (adoptionNeeded ? kEmptyShelfWidth : 0))
            + rightToolRailWidth;
    const int maximumCombined = std::max(0,
        panelOverlay_->width() - 2 * kPanelInset - kMinimumCanvasExposure);
    int overflow = std::max(0, leftWidth + rightWidth - maximumCombined);
    if (overflow > 0 && rightHasPanels) {
        const int reduction = std::min(overflow,
            rightWidth - rightToolRailWidth - kMinimumPanelWidth);
        rightWidth -= reduction;
        overflow -= reduction;
    }
    if (overflow > 0 && leftHasPanels) {
        const int reduction = std::min(overflow,
            leftWidth - leftToolRailWidth - kMinimumPanelWidth);
        leftWidth -= reduction;
        overflow -= reduction;
    }
    if (overflow > 0) {
        rightWidth = std::max(0, rightWidth - overflow);
    }
    effectiveLeftWidth_ = leftHasPanels
        ? std::max(kMinimumPanelWidth, leftWidth - leftToolRailWidth)
        : requestedLeftWidth_;
    effectiveRightWidth_ = rightHasPanels
        ? std::max(kMinimumPanelWidth, rightWidth - rightToolRailWidth)
        : requestedRightWidth_;

    const QRect nextLeftGeometry(kPanelInset, kPanelInset,
        leftWidth, availableHeight);
    const QRect nextRightGeometry(
        std::max(kPanelInset,
            panelOverlay_->width() - kPanelInset - rightWidth),
        kPanelInset, rightWidth, availableHeight);
    const QRect nextLeftHandleGeometry(
        nextLeftGeometry.right() + 1 - kResizeHandleHalfWidth,
        kPanelInset, kResizeHandleWidth, availableHeight);
    const QRect nextRightHandleGeometry(
        nextRightGeometry.left() - kResizeHandleHalfWidth,
        kPanelInset, kResizeHandleWidth, availableHeight);

    panelGeometryUpdateInProgress_ = true;
    if (!leftColumn_->isHidden()) {
        leftColumn_->setGeometry(nextLeftGeometry);
    }
    if (!rightColumn_->isHidden()) {
        rightColumn_->setGeometry(nextRightGeometry);
    }
    if (!leftResizeHandle_->isHidden()) {
        leftResizeHandle_->setGeometry(nextLeftHandleGeometry);
    }
    if (!rightResizeHandle_->isHidden()) {
        rightResizeHandle_->setGeometry(nextRightHandleGeometry);
    }
    if (toolRail_ && (toolRailPlacement_ == ToolRailPlacement::Top
            || toolRailPlacement_ == ToolRailPlacement::Bottom)) {
        int railLeft = kPanelInset;
        int railRight = panelOverlay_->width() - kPanelInset;
        if (!leftColumn_->isHidden()) {
            railLeft = leftColumn_->geometry().right() + 1 + kPanelInset;
        }
        if (!rightColumn_->isHidden()) {
            railRight = rightColumn_->geometry().left() - kPanelInset;
        }
        const int railWidth = std::max(0, railRight - railLeft);
        const int railTop = toolRailPlacement_ == ToolRailPlacement::Top
            ? kPanelInset
            : std::max(kPanelInset,
                panelOverlay_->height() - kPanelInset - kToolRailWidth);
        toolRail_->setGeometry(
            railLeft, railTop, railWidth, kToolRailWidth);
    }
    panelGeometryUpdateInProgress_ = false;
    clampFloatingPanels();

    updateRulerGeometry();

    if (viewModeOverlay_) {
        auto area = rulerContentRect();
        if (rulerVisible_[0] && !rulerFarEdge_[0]) area.setTop(rulers_[0]->geometry().bottom() + 1);
        if (rulerVisible_[1]) {
            if (rulerFarEdge_[1]) area.setRight(rulers_[1]->geometry().left() - 1);
            else area.setLeft(rulers_[1]->geometry().right() + 1);
        }
        area.adjust(8, 8, -8, -8);
        const auto size = viewModeOverlay_->sizeHint().boundedTo(area.size().expandedTo(QSize(1, 1)));
        viewModeOverlay_->setGeometry(area.center().x() - size.width() / 2, area.top(), size.width(), size.height());
        viewModeOverlay_->setVisible(viewModeOverlayRequestedVisible_ && !modalOverlay_);
    }

    const QRegion nextFootprint = calculateInteractionFootprint();
    const QRegion dirtyRegion = lastInteractionFootprint_ | nextFootprint;
    lastInteractionFootprint_ = nextFootprint;
    const bool showPassive = passiveOverlay_ && passiveOverlayRequestedVisible_ && !modalOverlay_;
    if (passiveOverlay_) passiveOverlay_->setVisible(showPassive);
    const bool shouldShowOverlay = workspaceShown_ && (!nextFootprint.isEmpty() || showPassive);
    if (shouldShowOverlay) {
        ensurePanelSurfaceParent();
    }
    if (auto* panelSurface = panelOverlay_->windowHandle()) {
        // An empty QWindow mask means unrestricted input on Qt/Wayland, not
        // an empty input region. A passive-only plane must instead opt out of
        // input. Change the existing QWindow flag, not QWidget window flags
        // (which would recreate/hide the native surface). Interactive content
        // immediately restores ordinary masked routing, including modal cards.
        panelSurface->setFlag(Qt::WindowTransparentForInput, nextFootprint.isEmpty());
        if (panelSurface->mask() != nextFootprint) {
            panelSurface->setMask(nextFootprint);
        }
    }
    if (shouldShowOverlay) panelOverlay_->show();
    if (shouldShowOverlay && !dirtyRegion.isEmpty()) {
        panelOverlay_->update(dirtyRegion);
    } else if (!shouldShowOverlay) {
        panelOverlay_->hide();
    }
}

void OverlayDockWorkspace::setRulerVisible(Qt::Orientation axis, bool visible)
{
    const int i = axis == Qt::Horizontal ? 0 : 1;
    if (rulerVisible_[i] == visible) return;
    if (!visible) rulers_[i]->cancelDrag();
    rulerVisible_[i] = visible;
    updatePanelGeometry();
    if (onRulerPlacementChanged) onRulerPlacementChanged();
}
bool OverlayDockWorkspace::rulerVisible(Qt::Orientation axis) const { return rulerVisible_[axis == Qt::Horizontal ? 0 : 1]; }
bool OverlayDockWorkspace::rulerFarEdge(Qt::Orientation axis) const { return rulerFarEdge_[axis == Qt::Horizontal ? 0 : 1]; }
QWidget* OverlayDockWorkspace::rulerStrip(Qt::Orientation axis) const { return rulers_[axis == Qt::Horizontal ? 0 : 1]; }
void OverlayDockWorkspace::setRulerFarEdge(Qt::Orientation axis, bool far)
{
    const int i = axis == Qt::Horizontal ? 0 : 1;
    if (rulerFarEdge_[i] == far) return;
    rulerFarEdge_[i] = far;
    updatePanelGeometry();
    if (onRulerPlacementChanged) onRulerPlacementChanged();
}
QRect OverlayDockWorkspace::rulerContentRect() const
{
    // Actual occupied workspace geometry, never panel names or transient
    // empty adoption shelves. The native canvas remains the full rect().
    auto area = panelOverlay_->rect();
    const bool leftOccupied = hasVisibleDockedPanel(PanelDockSide::Left)
        || (toolRail_ && !toolRail_->isHidden() && toolRailPlacement_ == ToolRailPlacement::Left);
    const bool rightOccupied = hasVisibleDockedPanel(PanelDockSide::Right)
        || (toolRail_ && !toolRail_->isHidden() && toolRailPlacement_ == ToolRailPlacement::Right);
    if (leftOccupied && !leftColumn_->isHidden()) area.setLeft(leftColumn_->geometry().right() + 1
        + (!leftResizeHandle_->isHidden() ? kResizeHandleHalfWidth : 0));
    if (rightOccupied && !rightColumn_->isHidden()) area.setRight(rightColumn_->geometry().left() - 1
        - (!rightResizeHandle_->isHidden() ? kResizeHandleHalfWidth : 0));
    if (toolRail_ && !toolRail_->isHidden() && toolRail_->parentWidget() == panelOverlay_) {
        if (toolRailPlacement_ == ToolRailPlacement::Top) area.setTop(toolRail_->geometry().bottom() + 1);
        if (toolRailPlacement_ == ToolRailPlacement::Bottom) area.setBottom(toolRail_->geometry().top() - 1);
    }
    return area.intersected(panelOverlay_->rect());
}
void OverlayDockWorkspace::updateRulerGeometry()
{
    const auto area = rulerContentRect();
    if (welcomeOverlay_ && !welcomeOverlay_->isHidden()) {
        const auto size = welcomeOverlay_->sizeHint();
        welcomeOverlay_->setGeometry(area.center().x() - size.width()/2,
            area.center().y() - size.height()/2, size.width(), size.height());
    }
    constexpr int t = RulerStrip::thickness;
    const int verticalInset = rulerVisible_[1] ? t : 0;
    const int horizontalInset = rulerVisible_[0] ? t : 0;
    const int hx = area.x() + (rulerFarEdge_[1] ? 0 : verticalInset);
    const int vy = area.y() + (rulerFarEdge_[0] ? 0 : horizontalInset);
    rulers_[0]->setGeometry(hx, rulerFarEdge_[0] ? area.bottom() - t + 1 : area.y(),
        std::max(0, area.width() - verticalInset), t);
    rulers_[1]->setGeometry(rulerFarEdge_[1] ? area.right() - t + 1 : area.x(), vy,
        t, std::max(0, area.height() - horizontalInset));
    for (int i = 0; i < 2; ++i) {
        rulers_[i]->setTicksTowardStart(rulerFarEdge_[i]);
        rulers_[i]->setVisible(rulerVisible_[i]);
        // Internal floating panels remain above ordinary workspace chrome.
        rulers_[i]->lower();
        if (!rulerDockHints_[i]->isHidden()) rulerDockHints_[i]->raise();
    }
}

void OverlayDockWorkspace::setWelcomeOverlay(QWidget* overlay, bool visible)
{
    welcomeOverlay_ = overlay;
    if (overlay) { overlay->setParent(panelOverlay_); overlay->setVisible(visible); }
    scheduleOverlayUpdate();
}
void OverlayDockWorkspace::setRulerView(core::ViewportState view, core::Extent2d document,
    core::Extent2d viewport, std::optional<core::DocumentBounds> bounds, std::optional<core::Vec2d> pointer)
{
    for (auto* ruler : rulers_) ruler->setView(view, document, viewport, bounds, pointer);
}
void OverlayDockWorkspace::updateRulerDrag(QPoint global)
{
    if (draggingRuler_ < 0) return;
    const auto area = rulerContentRect();
    const auto point = panelOverlay_->mapFromGlobal(global);
    const bool far = draggingRuler_ == 0 ? point.y() > area.center().y() : point.x() > area.center().x();
    for (int i = 0; i < 2; ++i) {
        const auto label = draggingRuler_ == 0 ? (i == 0 ? tr("Top ruler") : tr("Bottom ruler"))
            : (i == 0 ? tr("Left ruler") : tr("Right ruler"));
        static_cast<PanelDragProxy*>(rulerDockHints_[i])->setTitle(label + (bool(i) == far ? QStringLiteral(" ✓") : QString{}));
        const int x = draggingRuler_ == 0 ? area.center().x() - 65 : i == 0 ? area.left() : area.right() - 130;
        const int y = draggingRuler_ == 1 ? area.center().y() - 14 : i == 0 ? area.top() : area.bottom() - 28;
        rulerDockHints_[i]->setGeometry(x, y, 130, 28);
        rulerDockHints_[i]->show(); rulerDockHints_[i]->raise();
    }
    updatePanelGeometry();
}
void OverlayDockWorkspace::finishRulerDrag(QPoint global, bool cancel)
{
    if (draggingRuler_ < 0) return;
    const int i = std::exchange(draggingRuler_, -1);
    const auto point = panelOverlay_->mapFromGlobal(global);
    const auto area = rulerContentRect();
    for (auto* hint : rulerDockHints_) hint->hide();
    if (!cancel && rect().contains(point))
        rulerFarEdge_[i] = i == 0 ? point.y() > area.center().y() : point.x() > area.center().x();
    updatePanelGeometry();
    if (onRulerPlacementChanged) onRulerPlacementChanged();
}
bool OverlayDockWorkspace::cancelRulerDrag()
{
    if (draggingRuler_ < 0) return false;
    rulers_[draggingRuler_]->cancelDrag();
    return true;
}

} // namespace imageeditor::ui
