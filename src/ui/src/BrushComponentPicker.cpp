#include "imageeditor/ui/Localization.hpp"
#include "imageeditor/ui/BrushComponentPicker.hpp"
#include "imageeditor/ui/PopupOwnership.hpp"

#include <QAbstractItemView>
#include <QEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QListWidget>
#include <QMenu>
#include <QPixmap>
#include <QStyle>
#include <QStyleOptionToolButton>
#include <QToolButton>
#include <QTimer>
#include <QWidgetAction>

#include <algorithm>
#include <utility>

namespace imageeditor::ui {
namespace {
class PickerMenuButton final : public QToolButton {
public:
    using QToolButton::QToolButton;
protected:
    void initStyleOption(QStyleOptionToolButton* option) const override
    {
        QToolButton::initStyleOption(option);
        option->features |= QStyleOptionToolButton::HasMenu;
    }
    void keyPressEvent(QKeyEvent* event) override
    {
        if (event->key() == Qt::Key_Down) { click(); event->accept(); }
        else QToolButton::keyPressEvent(event);
    }
};
}

BrushComponentPicker::BrushComponentPicker(
    core::BrushAssetType type, QWidget* parent)
    : QWidget(parent)
    , type_(type)
    , button_(new PickerMenuButton(this))
    , menu_(new QMenu(button_))
    , grid_(new QListWidget(menu_))
{
    setObjectName(type == core::BrushAssetType::Tip
            ? QStringLiteral("BrushTipPicker")
            : QStringLiteral("BrushGrainPicker"));
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(button_);

    button_->setObjectName(type == core::BrushAssetType::Tip
            ? QStringLiteral("BrushTipPickerButton")
            : QStringLiteral("BrushGrainPickerButton"));
    button_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    connect(button_, &QToolButton::clicked, this, &BrushComponentPicker::openPopup);
    connect(menu_, &QMenu::aboutToShow, button_, [this] { button_->setDown(true); });
    connect(menu_, &QMenu::aboutToHide, button_, [this] { button_->setDown(false); });
    button_->setIconSize(QSize(40, 40));
    button_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    grid_->setObjectName(type == core::BrushAssetType::Tip
            ? QStringLiteral("BrushTipPickerGrid")
            : QStringLiteral("BrushGrainPickerGrid"));
    grid_->setViewMode(QListView::IconMode);
    grid_->setMovement(QListView::Static);
    grid_->setResizeMode(QListView::Adjust);
    grid_->setWrapping(true);
    grid_->setUniformItemSizes(true);
    grid_->setSelectionMode(QAbstractItemView::SingleSelection);
    grid_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    grid_->setIconSize(QSize(54, 54));
    grid_->setGridSize(QSize(92, 88));
    grid_->setMinimumHeight(196);
    grid_->setMaximumHeight(284);
    grid_->installEventFilter(this);

    auto* action = new QWidgetAction(menu_);
    action->setDefaultWidget(grid_);
    menu_->addAction(action);
    connect(menu_, &QMenu::aboutToShow, this, [this] {
        // A narrow Properties dock gets two columns; the normal dock width
        // gets three. The popup remains compositor-constrained at screen
        // edges because it is a real transient QMenu.
        const auto columns = width() < 270 ? 2 : 3;
        grid_->setFixedWidth(columns * grid_->gridSize().width() + 10);
        QTimer::singleShot(0, grid_, [this] {
            if (!menu_->isVisible()) {
                return;
            }
            auto* current = grid_->currentItem();
            if (!current || !current->flags().testFlag(Qt::ItemIsEnabled)) {
                current = nullptr;
                for (int row = 0; row < grid_->count(); ++row) {
                    auto* candidate = grid_->item(row);
                    if (candidate->flags().testFlag(Qt::ItemIsEnabled)) {
                        current = candidate;
                        grid_->setCurrentItem(current);
                        break;
                    }
                }
            }
            if (current) {
                grid_->scrollToItem(current,
                    QAbstractItemView::PositionAtCenter);
            }
            grid_->setFocus(Qt::PopupFocusReason);
        });
    });
    connect(grid_, &QListWidget::itemActivated, this,
        [this](QListWidgetItem* item) {
            if (!item || item->flags().testFlag(Qt::ItemIsEnabled) == false) {
                return;
            }
            const auto id = item->data(Qt::UserRole).toString().toStdString();
            if (onAssetSelected) {
                onAssetSelected(id);
            }
            menu_->close();
        });
    connect(grid_, &QListWidget::itemClicked, this,
        [this](QListWidgetItem* item) {
            if (!item || !item->flags().testFlag(Qt::ItemIsEnabled)) {
                return;
            }
            const auto id = item->data(Qt::UserRole).toString().toStdString();
            if (onAssetSelected) {
                onAssetSelected(id);
            }
            menu_->close();
        });
}

bool BrushComponentPicker::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == grid_ && event && event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape) {
            menu_->close();
            button_->setFocus(Qt::PopupFocusReason);
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void BrushComponentPicker::setItems(std::vector<BrushComponentItem> items)
{
    items_ = std::move(items);
    rebuildGrid();
    updateButton();
}

void BrushComponentPicker::setCurrentAssetId(std::string_view assetId)
{
    currentAssetId_ = assetId;
    updateButton();
}

void BrushComponentPicker::openPopup()
{
    // Keep this seam non-blocking so automated interaction can exercise the
    // real transient menu without entering QMenu's nested exec loop.
    showOwnedPopupMenu(menu_, button_);
}

void BrushComponentPicker::rebuildGrid()
{
    grid_->clear();
    for (const auto& component : items_) {
        auto* item = new QListWidgetItem(
            QIcon(QPixmap::fromImage(component.thumbnail)),
            component.displayName, grid_);
        item->setData(Qt::UserRole, QString::fromStdString(component.id));
        item->setTextAlignment(Qt::AlignHCenter | Qt::AlignTop);
        item->setToolTip(component.available
                ? component.displayName
                : i18n::text("brush.component.picker.1.unavailable.2")
                      .arg(component.displayName, component.unavailableReason));
        if (!component.available) {
            item->setFlags(item->flags() & ~Qt::ItemIsEnabled
                & ~Qt::ItemIsSelectable);
        }
    }
}

void BrushComponentPicker::updateButton()
{
    const auto found = std::find_if(items_.begin(), items_.end(),
        [this](const BrushComponentItem& item) {
            return item.id == currentAssetId_;
        });
    if (found == items_.end()) {
        i18n::setText(button_, i18n::text("brush.component.picker.unavailable.1")
            .arg(QString::fromStdString(currentAssetId_)));
        button_->setIcon(style()->standardIcon(QStyle::SP_MessageBoxWarning));
        i18n::setToolTip(button_, i18n::text("brush.component.picker.missing.brush.asset.id.1")
            .arg(QString::fromStdString(currentAssetId_)));
        button_->setAccessibleName(button_->text());
        grid_->clearSelection();
        return;
    }
    i18n::setText(button_, found->available ? found->displayName
        : i18n::text("brush.component.picker.1.unavailable").arg(found->displayName));
    button_->setIcon(QIcon(QPixmap::fromImage(found->thumbnail)));
    button_->setToolTip(found->available
            ? found->displayName : found->unavailableReason);
    i18n::setAccessibleName(button_, i18n::text("brush.component.picker.current.1.2")
        .arg(type_ == core::BrushAssetType::Tip
                ? i18n::text("brush.component.picker.brush.tip") : i18n::text("brush.component.picker.brush.grain"),
            button_->text()));
    const auto id = QString::fromStdString(currentAssetId_);
    for (int row = 0; row < grid_->count(); ++row) {
        auto* item = grid_->item(row);
        if (item->data(Qt::UserRole).toString() == id) {
            grid_->setCurrentItem(item);
            return;
        }
    }
    grid_->clearSelection();
}

} // namespace imageeditor::ui
