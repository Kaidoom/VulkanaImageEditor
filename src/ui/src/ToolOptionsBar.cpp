#include "imageeditor/ui/ToolOptionsBar.hpp"

#include <QHBoxLayout>
#include <QEvent>
#include <QLabel>
#include <QApplication>
#include <QScrollArea>
#include <QScrollBar>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QToolButton>
#include <algorithm>

namespace imageeditor::ui {
namespace {
// QStackedWidget normally reserves the largest hidden page. Only the active
// tool should own overflow width; the host's height never changes.
class ToolPageStack final : public QStackedWidget {
public:
    using QStackedWidget::QStackedWidget;
    QSize sizeHint() const override { return currentWidget() ? currentWidget()->sizeHint() : QSize {}; }
    QSize minimumSizeHint() const override { return currentWidget() ? currentWidget()->minimumSizeHint() : QSize {}; }
};
}

ToolOptionsBar::ToolOptionsBar(QWidget* parent)
    : QToolBar(QStringLiteral("Tool Options"), parent)
{
    setObjectName(QStringLiteral("ToolOptionsBar"));
    setAllowedAreas(Qt::TopToolBarArea);
    setMovable(false);
    setFloatable(false);
    setOrientation(Qt::Horizontal);
    setContextMenuPolicy(Qt::PreventContextMenu);
    // 30 px controls, 5 px above/below, and the theme's 1 px bottom border.
    setFixedHeight(41);
    QToolBar::layout()->setContentsMargins(0, 0, 0, 1);
    toggleViewAction()->setVisible(false);

    auto* root = new QWidget(this);
    root->setObjectName(QStringLiteral("ToolOptionsRoot"));
    // The toolbar owns the fixed outer height. Its containers must fill that
    // budget rather than clip children to an inferred, smaller size hint.
    root->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    auto* layout = new QHBoxLayout(root);
    // Own the padding here. QToolBar's style metrics interpret even horizontal
    // stylesheet padding as a frame width on all four sides.
    layout->setContentsMargins(14, 5, 14, 5);
    layout->setSpacing(0);

    auto* leftSlot = leftSlot_ = new QWidget(root);
    leftSlot->setObjectName(QStringLiteral("ToolOptionsLeftSlot"));
    // Leave room for longer tool labels and the overflow navigation button.
    // The symmetric right slot preserves centered controls.
    leftSlot->setFixedWidth(120);
    auto* leftLayout = new QHBoxLayout(leftSlot);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(0);
    contextLabel_ = new QLabel(leftSlot);
    contextLabel_->setObjectName(QStringLiteral("ToolOptionsContext"));
    leftLayout->addWidget(contextLabel_, 0, Qt::AlignLeft | Qt::AlignVCenter);
    auto* leadingGap = leadingGap_ = new QWidget(leftSlot);
    leadingGap->setObjectName(QStringLiteral("ToolOptionsLeadingGap"));
    leadingGap->setFixedWidth(16);
    leftLayout->addWidget(leadingGap);
    leadingStack_ = new ToolPageStack(leftSlot);
    leadingStack_->setObjectName(QStringLiteral("ToolOptionsLeadingStack"));
    leadingStack_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    emptyLeading_ = new QWidget(leadingStack_);
    leadingStack_->addWidget(emptyLeading_);
    leftLayout->addWidget(leadingStack_);
    leftLayout->addStretch(1);

    const auto overflowButton = [](Qt::ArrowType arrow, const QString& name,
                                   const QString& label, QWidget* parent) {
        auto* button = new QToolButton(parent);
        button->setObjectName(name);
        button->setArrowType(arrow);
        button->setFixedSize(24, 28);
        button->setToolTip(label);
        button->setAccessibleName(label);
        button->setAutoRepeat(true);
        button->hide();
        return button;
    };
    previousOptions_ = overflowButton(Qt::LeftArrow,
        QStringLiteral("PreviousToolOptions"), QStringLiteral("Previous tool settings"), leftSlot);
    leftLayout->addWidget(previousOptions_);

    pageViewport_ = new QScrollArea(root);
    pageViewport_->setObjectName(QStringLiteral("ToolOptionsViewport"));
    pageViewport_->setFrameShape(QFrame::NoFrame);
    pageViewport_->setWidgetResizable(true);
    pageViewport_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    pageViewport_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // QScrollArea's generic minimum size includes room for scrollbars. This
    // scrollbar-free strip instead receives its height from the fixed host.
    pageViewport_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    pageViewport_->setMinimumWidth(0);

    pageStack_ = new ToolPageStack(root);
    pageStack_->setObjectName(QStringLiteral("ToolOptionsPageStack"));
    pageStack_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    emptyPage_ = new QWidget(pageStack_);
    emptyPage_->setObjectName(QStringLiteral("EmptyToolOptionsPage"));
    pageStack_->addWidget(emptyPage_);
    pageViewport_->setWidget(pageStack_);
    connect(pageStack_, &QStackedWidget::currentChanged, this, [this] {
        pageStack_->updateGeometry();
        pageViewport_->horizontalScrollBar()->setValue(0);
    });

    auto* rightBalance = rightBalance_ = new QWidget(root);
    rightBalance->setObjectName(QStringLiteral("ToolOptionsRightBalance"));
    rightBalance->setFixedWidth(leftSlot->width());
    auto* rightLayout = new QHBoxLayout(rightBalance);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    nextOptions_ = overflowButton(Qt::RightArrow,
        QStringLiteral("NextToolOptions"), QStringLiteral("More tool settings"), rightBalance);
    rightLayout->addWidget(nextOptions_);
    rightLayout->addStretch(1);

    layout->addWidget(leftSlot);
    layout->addWidget(pageViewport_, 1);
    layout->addWidget(rightBalance);
    auto* scroll = pageViewport_->horizontalScrollBar();
    connect(scroll, &QScrollBar::rangeChanged, this,
        [this] { updateOverflowControls(); });
    connect(scroll, &QScrollBar::valueChanged, this,
        [this] { updateOverflowControls(); });
    connect(previousOptions_, &QToolButton::clicked, this, [this, scroll] {
        scroll->setValue(scroll->value() - pageViewport_->viewport()->width() / 2);
    });
    connect(nextOptions_, &QToolButton::clicked, this, [this, scroll] {
        scroll->setValue(scroll->value() + pageViewport_->viewport()->width() / 2);
    });
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget* next) {
        if (next && pageStack_->isAncestorOf(next)) {
            // Reveal the whole control, not just QScrollArea's input-method
            // caret rectangle (which can leave a spinbox label/steps clipped).
            auto* control = next;
            const auto available = pageViewport_->viewport()->size();
            while (auto* parent = control->parentWidget()) {
                if (parent == pageStack_->currentWidget()
                    || parent->width() > available.width()
                    || parent->height() > available.height()) {
                    break;
                }
                control = parent;
            }
            const QRect bounds(control->mapTo(pageStack_, QPoint {}), control->size());
            auto* scroll = pageViewport_->horizontalScrollBar();
            if (bounds.left() < scroll->value()) {
                scroll->setValue(bounds.left());
            } else if (bounds.right() >= scroll->value() + available.width()) {
                scroll->setValue(bounds.right() - available.width() + 1);
            }
        }
    });
    addWidget(root);
    setActiveTool(core::ToolId::Move);
}

ToolOptionsBar::~ToolOptionsBar()
{
    // QToolBar releases/reparents its QWidgetAction contents in its base
    // destructor. That can change scrollbar ranges and focus after our maps
    // have already been destroyed, but before QObject disconnects receivers.
    disconnect(qApp, nullptr, this, nullptr);
    for (auto* child : findChildren<QObject*>())
        disconnect(child, nullptr, this, nullptr);
}

bool ToolOptionsBar::event(QEvent* event)
{
    const bool handled = QToolBar::event(event);
    // QToolBar creates its internal layout before our object name is set and
    // caches generic toolbar frame margins. Theme changes reset them too.
    // Reserve only the separator here; ToolOptionsRoot owns the actual padding.
    if (event->type() == QEvent::StyleChange || event->type() == QEvent::Polish)
        QToolBar::layout()->setContentsMargins(0, 0, 0, 1);
    return handled;
}

void ToolOptionsBar::registerToolPage(
    core::ToolId tool, const QString& label, QWidget* page)
{
    if (!page) {
        return;
    }
    if (const auto existing = pages_.find(tool); existing != pages_.end() && existing->second.page != page) {
        auto* old = existing->second.page;
        pages_.erase(existing);
        if (std::none_of(pages_.begin(), pages_.end(), [old](const auto& p) { return p.second.page == old; })) {
            pageStack_->removeWidget(old);
            old->deleteLater();
        }
    }
    if (pageStack_->indexOf(page) < 0) {
        page->setParent(pageStack_);
        pageStack_->addWidget(page);
    }
    pages_[tool] = {label, page};
    if (tool == activeTool_) {
        setActiveTool(tool);
    }
}

void ToolOptionsBar::setContextLabel(const QString& label)
{
    contextLabel_->setText(label.toUpper());
    updateSideSlots();
}

void ToolOptionsBar::registerToolLeadingWidget(core::ToolId tool, QWidget* widget)
{
    if (!widget) return;
    if (const auto found = leadingWidgets_.find(tool);
        found != leadingWidgets_.end() && found->second != widget) {
        auto* old = found->second;
        leadingWidgets_.erase(found);
        if (std::none_of(leadingWidgets_.begin(), leadingWidgets_.end(),
                [old](const auto& entry) { return entry.second == old; })) {
            leadingStack_->removeWidget(old);
            old->deleteLater();
        }
    }
    if (leadingStack_->indexOf(widget) < 0) leadingStack_->addWidget(widget);
    leadingWidgets_[tool] = widget;
    if (activeTool_ == tool) setActiveTool(tool);
}

void ToolOptionsBar::setActiveTool(core::ToolId tool)
{
    activeTool_ = tool;
    const auto leading = leadingWidgets_.find(tool);
    leadingStack_->setCurrentWidget(leading == leadingWidgets_.end() ? emptyLeading_ : leading->second);
    leadingStack_->setVisible(leading != leadingWidgets_.end());
    const auto found = pages_.find(tool);
    if (found == pages_.end()) {
        contextLabel_->clear();
        pageStack_->setCurrentWidget(emptyPage_);
        updateOverflowControls();
        return;
    }
    contextLabel_->setText(found->second.label.toUpper());
    pageStack_->setCurrentWidget(found->second.page);
    updateOverflowControls();
}

void ToolOptionsBar::updateOverflowControls()
{
    const auto* scroll = pageViewport_->horizontalScrollBar();
    const bool overflow = pageStack_->currentWidget() != emptyPage_
        && scroll->maximum() > scroll->minimum();
    previousOptions_->setVisible(overflow);
    nextOptions_->setVisible(overflow);
    previousOptions_->setEnabled(scroll->value() > scroll->minimum());
    nextOptions_->setEnabled(scroll->value() < scroll->maximum());
    updateSideSlots();
}

void ToolOptionsBar::updateSideSlots()
{
    // Reserve navigation room even when the arrows are hidden: changing the
    // overflow range must never cause an oscillating width/layout feedback loop.
    const auto* leading = leadingWidgetForTool(activeTool_);
    const int modeWidth = leading ? std::max(leading->sizeHint().width(), leading->minimumSizeHint().width()) : 0;
    leadingGap_->setVisible(leading != nullptr);
    const int width = std::max(120, contextLabel_->sizeHint().width()
        + (leading ? 16 + modeWidth : 0) + 8 + 24);
    leadingStack_->setFixedWidth(std::max(0, modeWidth));
    leftSlot_->setFixedWidth(width);
    rightBalance_->setFixedWidth(width);
}

QWidget* ToolOptionsBar::leadingWidgetForTool(core::ToolId tool) const noexcept
{
    const auto found = leadingWidgets_.find(tool);
    return found == leadingWidgets_.end() ? nullptr : found->second;
}

QWidget* ToolOptionsBar::pageForTool(core::ToolId tool) const noexcept
{
    const auto found = pages_.find(tool);
    return found == pages_.end() ? nullptr : found->second.page;
}

} // namespace imageeditor::ui
