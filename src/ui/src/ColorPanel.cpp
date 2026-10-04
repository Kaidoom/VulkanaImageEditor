#include "imageeditor/ui/ColorPanel.hpp"
#include "imageeditor/ui/ColorSelector.hpp"
#include "imageeditor/ui/PopupOwnership.hpp"
#include "imageeditor/ui/ToolIcons.hpp"

#include <QColorDialog>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QVBoxLayout>
#include <QWindow>

#include <algorithm>
#include <array>

namespace imageeditor::ui {
namespace {
constexpr int swatchExtent = 28;
const auto customSwatchesKey = QStringLiteral("editor/custom-swatches-v1");

// Black-to-white first, followed by hue families, each dark to light.
constexpr std::array<QRgb, 50> defaultSwatches {
    0xff000000, 0xff1c1c1c, 0xff383838, 0xff555555, 0xff717171,
    0xff8e8e8e, 0xffaaaaaa, 0xffc6c6c6, 0xffe3e3e3, 0xffffffff,
    0xff800000, 0xffff0000, 0xffff6666, 0xffffcccc,
    0xff803300, 0xffff8000, 0xffffb366, 0xffffddbb,
    0xff806000, 0xffffcc00, 0xffffff66, 0xffffffcc,
    0xff466600, 0xff99cc00, 0xffc2e666, 0xffe8f5bb,
    0xff006633, 0xff00aa55, 0xff66cc88, 0xffc2e6cc,
    0xff005555, 0xff009999, 0xff66cccc, 0xffbbeedd,
    0xff005580, 0xff00aaff, 0xff66d5ff, 0xffcceeff,
    0xff000080, 0xff3366ff, 0xff8899ff, 0xffdddfff,
    0xff550080, 0xff9933cc, 0xffbb88ee, 0xffe6ccff,
    0xff800055, 0xffee3399, 0xffff88bb, 0xffffcce6,
};

QColor swatchColor(const QListWidgetItem* item)
{
    return item ? item->data(Qt::UserRole).value<QColor>() : QColor{};
}

void setSwatchColor(QListWidgetItem* item, const QColor& color)
{
    item->setData(Qt::UserRole, color);
    auto label = color.name(QColor::HexRgb);
    if (color.alpha() != 255)
        label += QStringLiteral("%1").arg(color.alpha(), 2, 16, QLatin1Char('0'));
    label = label.toUpper();
    item->setToolTip(label);
    item->setData(Qt::AccessibleTextRole, label);
}

class SwatchDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override
    {
        return {swatchExtent, swatchExtent};
    }
    void paint(QPainter* painter, const QStyleOptionViewItem& option,
        const QModelIndex& index) const override
    {
        painter->save();
        const auto area = option.rect.adjusted(3, 3, -3, -3);
        painter->setClipRect(area);
        for (int y = area.top(); y <= area.bottom(); y += 5)
            for (int x = area.left(); x <= area.right(); x += 5)
                painter->fillRect(x, y, 5, 5,
                    ((x - area.left()) / 5 + (y - area.top()) / 5) % 2
                        ? QColor(94, 98, 111) : QColor(160, 164, 175));
        painter->fillRect(area, index.data(Qt::UserRole).value<QColor>());
        painter->setClipping(false);
        painter->setBrush(Qt::NoBrush);
        painter->setPen(option.palette.color(QPalette::Mid));
        painter->drawRect(area.adjusted(0, 0, -1, -1));
        if (option.state & (QStyle::State_Selected | QStyle::State_MouseOver)) {
            painter->setPen(QPen(option.palette.color(
                option.state & QStyle::State_Selected ? QPalette::Highlight : QPalette::Text), 2));
            painter->drawRect(option.rect.adjusted(1, 1, -2, -2));
        }
        painter->restore();
    }
};

// Like the brush shelf, fixed-size tiles reflow while the containing panel is
// the only scroll area. No nested scrolling or per-swatch child widgets.
class SwatchGrid final : public QListWidget {
public:
    explicit SwatchGrid(QWidget* parent) : QListWidget(parent)
    {
        setViewMode(QListView::IconMode);
        setFlow(QListView::LeftToRight);
        setMovement(QListView::Static);
        setResizeMode(QListView::Adjust);
        setWrapping(true);
        setUniformItemSizes(true);
        setGridSize({swatchExtent, swatchExtent});
        setSpacing(0);
        setFrameShape(QFrame::NoFrame);
        setSelectionMode(QAbstractItemView::SingleSelection);
        setEditTriggers(QAbstractItemView::NoEditTriggers);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setMouseTracking(true);
        setStyleSheet(QStringLiteral("QListWidget { background: transparent; border: none; padding: 0; }"));
        setItemDelegate(new SwatchDelegate(this));
        connect(model(), &QAbstractItemModel::rowsInserted, this, [this] { updateHeight(); });
        connect(model(), &QAbstractItemModel::rowsRemoved, this, [this] { updateHeight(); });
        updateHeight();
    }
protected:
    void resizeEvent(QResizeEvent* event) override
    {
        QListWidget::resizeEvent(event);
        updateHeight();
    }
private:
    void updateHeight()
    {
        const int columns = std::max(1, viewport()->width() / swatchExtent);
        const int rows = std::max(1, (count() + columns - 1) / columns);
        setFixedHeight(rows * swatchExtent);
    }
};
} // namespace

ColorPanel::ColorPanel(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("ColorPanelContent"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(this);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidgetResizable(true);
    // Scrollbar minimum-size hints must not prevent short docks from scrolling
    // their full-size swatches. The body retains its own content minimum.
    scroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    auto* body = new QWidget;
    auto* content = new QVBoxLayout(body);
    content->setContentsMargins(12, 12, 12, 12);
    selector_ = new ColorSelector(ColorSelector::Presentation::Detailed, body);
    content->addWidget(selector_, 0, Qt::AlignTop);
    content->addWidget(new QLabel(tr("Swatches"), body));
    defaults_ = new SwatchGrid(body);
    defaults_->setObjectName(QStringLiteral("DefaultColorSwatches"));
    defaults_->setAccessibleName(tr("Default color swatches"));
    for (const auto rgb : defaultSwatches)
        setSwatchColor(new QListWidgetItem(defaults_), QColor::fromRgba(rgb));
    content->addWidget(defaults_);

    auto* customRow = new QHBoxLayout;
    customRow->setSpacing(6);
    customRow->addWidget(new QLabel(tr("Custom"), body));
    customRow->addStretch();
    auto* add = new QPushButton(toolGlyph(ToolGlyph::Plus), QString(), body);
    add->setObjectName(QStringLiteral("AddCustomSwatch"));
    add->setToolTip(tr("Add custom color"));
    delete_ = new QPushButton(toolGlyph(ToolGlyph::Trash), QString(), body);
    delete_->setObjectName(QStringLiteral("DeleteCustomSwatch"));
    delete_->setToolTip(tr("Delete selected custom color"));
    for (auto* button : {add, delete_}) {
        button->setStyleSheet(QStringLiteral("QPushButton { padding: 0; min-width: 0; min-height: 0; }"));
        button->setFixedSize(28, 26);
        button->setIconSize({18, 18});
        button->setAccessibleName(button->toolTip());
        customRow->addWidget(button);
    }
    content->addLayout(customRow);
    custom_ = new SwatchGrid(body);
    custom_->setObjectName(QStringLiteral("CustomColorSwatches"));
    custom_->setAccessibleName(tr("Custom color swatches"));
    QSettings settings;
    for (const auto& stored : settings.value(customSwatchesKey).toStringList()) {
        const QColor color(stored);
        if (color.isValid())
            setSwatchColor(new QListWidgetItem(custom_), color);
    }
    content->addWidget(custom_);
    content->addStretch(1);
    scroll->setWidget(body);
    layout->addWidget(scroll);
    selector_->onColorsChanged = [this](core::EditorColors colors) {
        setColors(colors);
        if (onColorsChanged) {
            onColorsChanged(colors_);
        }
    };
    for (auto* grid : {defaults_, custom_}) {
        connect(grid, &QListWidget::itemSelectionChanged, this, [this, grid] {
            chooseSwatch(grid);
            updateDeleteButton();
        });
        // Clicking a previously selected entry reapplies it after using another
        // color control; keyboard navigation uses the selection signal above.
        connect(grid, &QListWidget::itemClicked, this, [this, grid] { chooseSwatch(grid); });
    }
    connect(custom_, &QListWidget::itemDoubleClicked, this,
        [this](QListWidgetItem* item) { editCustomSwatch(custom_->row(item)); });
    connect(add, &QPushButton::clicked, this, [this] { editCustomSwatch(); });
    connect(delete_, &QPushButton::clicked, this, [this] {
        const auto selected = custom_->selectedItems();
        if (selected.isEmpty()) return;
        // Removal must not implicitly select/apply a neighboring swatch.
        const QSignalBlocker blocker(custom_);
        delete custom_->takeItem(custom_->row(selected.front()));
        custom_->setCurrentRow(-1);
        custom_->clearSelection();
        saveCustomSwatches();
        updateDeleteButton();
    });
    updateDeleteButton();
}

void ColorPanel::setColors(core::EditorColors colors)
{
    colors_ = colors;
    selector_->setColors(colors_);
    const auto active = colors_.foreground();
    for (auto* grid : {defaults_, custom_}) {
        const auto selected = grid->selectedItems();
        if (!selected.isEmpty() && swatchColor(selected.front())
                != QColor(active.red, active.green, active.blue, active.alpha)) {
            const QSignalBlocker blocker(grid);
            grid->setCurrentRow(-1);
            grid->clearSelection();
        }
    }
    updateDeleteButton();
}

void ColorPanel::setForegroundColor(core::Rgba8 color)
{
    auto colors = colors_;
    colors.setColor(colors.active, color);
    setColors(colors);
}

void ColorPanel::chooseSwatch(QListWidget* grid)
{
    const auto selected = grid->selectedItems();
    if (selected.isEmpty()) return;
    auto* other = grid == defaults_ ? custom_ : defaults_;
    {
        const QSignalBlocker blocker(other);
        other->setCurrentRow(-1);
        other->clearSelection();
    }
    const auto color = swatchColor(selected.front());
    const core::Rgba8 rgba {static_cast<std::uint8_t>(color.red()),
        static_cast<std::uint8_t>(color.green()), static_cast<std::uint8_t>(color.blue()),
        static_cast<std::uint8_t>(color.alpha())};
    const bool changed = colors_.foreground() != rgba;
    setForegroundColor(rgba);
    if (changed && onColorsChanged) onColorsChanged(colors_);
}

void ColorPanel::editCustomSwatch(int row)
{
    if (colorDialog_) {
        colorDialog_->raise();
        colorDialog_->activateWindow();
        return;
    }
    const auto active = colors_.foreground();
    const auto initial = row >= 0 ? swatchColor(custom_->item(row))
        : QColor(active.red, active.green, active.blue, active.alpha);
    auto* owner = popupTopLevelOwner(this);
    auto* dialog = new QColorDialog(initial, owner);
    colorDialog_ = dialog;
    dialog->setObjectName(QStringLiteral("CustomSwatchColorDialog"));
    dialog->setWindowTitle(row >= 0 ? tr("Edit custom color") : tr("Add custom color"));
    dialog->setOptions(QColorDialog::ShowAlphaChannel | QColorDialog::DontUseNativeDialog);
    dialog->setCurrentColor(initial);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowModality(Qt::ApplicationModal);
    if (owner) {
        (void)owner->winId();
        (void)dialog->winId();
        dialog->windowHandle()->setTransientParent(owner->windowHandle());
        connect(owner->windowHandle(), &QWindow::visibleChanged, dialog,
            [dialog](bool visible) { if (!visible) dialog->reject(); });
    }
    connect(this, &QObject::destroyed, dialog, &QDialog::reject);
    connect(dialog, &QColorDialog::colorSelected, this, [this, row](const QColor& color) {
        if (!color.isValid()) return;
        auto* item = row >= 0 ? custom_->item(row) : new QListWidgetItem(custom_);
        if (!item) return;
        setSwatchColor(item, color);
        custom_->setCurrentItem(item, QItemSelectionModel::ClearAndSelect);
        chooseSwatch(custom_);
        saveCustomSwatches();
    });
    dialog->show();
}

void ColorPanel::saveCustomSwatches()
{
    QStringList stored;
    for (int i = 0; i < custom_->count(); ++i)
        stored.push_back(swatchColor(custom_->item(i)).name(QColor::HexArgb));
    QSettings settings;
    settings.setValue(customSwatchesKey, stored);
}

void ColorPanel::updateDeleteButton()
{
    delete_->setEnabled(!custom_->selectedItems().isEmpty());
}

} // namespace imageeditor::ui
