#include "imageeditor/ui/BrushPresetGrid.hpp"

#include <QAbstractItemView>
#include <QIcon>
#include <QPixmap>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QStyle>

#include <algorithm>
#include <utility>

namespace imageeditor::ui {
namespace {

constexpr int kTileWidth = 66;
constexpr int kTileHeight = 66;
constexpr int kIconExtent = 56;
constexpr int kGridPaddingAllowance = 12;

} // namespace

BrushPresetGrid::BrushPresetGrid(QWidget* parent)
    : QListWidget(parent)
{
    setObjectName(QStringLiteral("BrushPresetGrid"));
    setViewMode(QListView::IconMode);
    setFlow(QListView::LeftToRight);
    setMovement(QListView::Static);
    setResizeMode(QListView::Adjust);
    setWrapping(true);
    setUniformItemSizes(true);
    setWordWrap(false);
    setTextElideMode(Qt::ElideRight);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setIconSize(QSize(kIconExtent, kIconExtent));
    setGridSize(QSize(kTileWidth, kTileHeight));
    setSpacing(0);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setFocusPolicy(Qt::StrongFocus);

    connect(this, &QListWidget::currentItemChanged, this,
        [this](QListWidgetItem* current, QListWidgetItem*) {
            if (updatingSelection_ || !current
                || !current->flags().testFlag(Qt::ItemIsEnabled)) {
                return;
            }
            const auto id = current->data(Qt::UserRole).toString().toStdString();
            if (id == currentPresetId_) {
                return;
            }
            if (onPresetSelected) {
                onPresetSelected(id);
            }
        });
}

void BrushPresetGrid::setItems(std::vector<BrushPresetGridItem> items)
{
    items_ = std::move(items);
    rebuild();
}

void BrushPresetGrid::setCurrentPresetId(std::string_view presetId)
{
    currentPresetId_ = presetId;
    updateCurrentItem();
}

void BrushPresetGrid::setCurrentPresetModified(bool modified)
{
    if (currentPresetModified_ == modified) {
        return;
    }
    currentPresetModified_ = modified;
    updateCurrentItem();
}

void BrushPresetGrid::resizeEvent(QResizeEvent* event)
{
    QListWidget::resizeEvent(event);
    updateGridHeight();
}

void BrushPresetGrid::rebuild()
{
    updatingSelection_ = true;
    const QSignalBlocker blocker(this);
    clear();
    for (const auto& preset : items_) {
        auto icon = preset.thumbnail.isNull()
            ? style()->standardIcon(QStyle::SP_MessageBoxWarning)
            : QIcon(QPixmap::fromImage(preset.thumbnail));
        auto* item = new QListWidgetItem(std::move(icon),
            QString{},
            this);
        item->setData(Qt::UserRole, QString::fromStdString(preset.id));
        item->setData(Qt::UserRole + 1, preset.displayName);
        item->setData(Qt::UserRole + 2, preset.available);
        item->setSizeHint(QSize(kTileWidth, kTileHeight));
        item->setTextAlignment(Qt::AlignHCenter | Qt::AlignTop);
        item->setToolTip(preset.available
                ? preset.displayName
                : QStringLiteral("%1\nUnavailable: %2")
                      .arg(preset.displayName, preset.unavailableReason));
        item->setData(Qt::AccessibleTextRole, preset.available
                ? preset.displayName
                : preset.displayName + QStringLiteral(", unavailable"));
        if (!preset.available) {
            item->setFlags(item->flags() & ~Qt::ItemIsEnabled
                & ~Qt::ItemIsSelectable);
        }
    }
    updatingSelection_ = false;
    updateCurrentItem();
    updateGridHeight();
}

void BrushPresetGrid::updateCurrentItem()
{
    updatingSelection_ = true;
    const QSignalBlocker blocker(this);
    QListWidgetItem* selected = nullptr;
    for (int row = 0; row < count(); ++row) {
        auto* item = this->item(row);
        const auto id = item->data(Qt::UserRole).toString().toStdString();
        const auto baseName = item->data(Qt::UserRole + 1).toString();
        const auto available = item->data(Qt::UserRole + 2).toBool();
        const auto isCurrent = id == currentPresetId_;
        if (available)
            item->setToolTip(baseName + (isCurrent && currentPresetModified_
                ? QStringLiteral("\nModified") : QString{}));
        item->setData(Qt::AccessibleTextRole, !available
                ? baseName + QStringLiteral(", unavailable")
                : isCurrent && currentPresetModified_
                    ? baseName + QStringLiteral(", modified")
                    : baseName);
        if (isCurrent && item->flags().testFlag(Qt::ItemIsEnabled)) {
            selected = item;
        }
    }
    setCurrentItem(selected);
    if (!selected) {
        clearSelection();
    }
    updatingSelection_ = false;
}

void BrushPresetGrid::updateGridHeight()
{
    const auto availableWidth = std::max(1,
        viewport()->width() - 2 * frameWidth() - kGridPaddingAllowance);
    const auto columns = std::max(1, availableWidth / kTileWidth);
    const auto rows = std::max(1, (count() + columns - 1) / columns);
    const auto required = rows * kTileHeight + 2 * frameWidth() + 2;
    if (height() != required) {
        setFixedHeight(required);
    }
}

} // namespace imageeditor::ui
