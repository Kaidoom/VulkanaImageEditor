#include "imageeditor/ui/Localization.hpp"
#include "imageeditor/ui/SwatchesPanel.hpp"
#include "imageeditor/ui/CollapsibleSection.hpp"
#include "imageeditor/ui/PopupOwnership.hpp"
#include "imageeditor/ui/ToolIcons.hpp"

#include "imageeditor/ui/ColorDialog.hpp"
#include <QImage>
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
constexpr QSize gradientTileSize {64, 30};
constexpr int gradientPreviewRole = Qt::UserRole + 1;
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

std::vector<core::GradientStop> gradient(std::initializer_list<QRgb> colors)
{
    std::vector<core::GradientStop> result;
    result.reserve(colors.size());
    for (const auto rgb : colors) {
        result.push_back({double(result.size()) / double(colors.size() - 1),
            {std::uint8_t(qRed(rgb)), std::uint8_t(qGreen(rgb)),
                std::uint8_t(qBlue(rgb)), 255}});
    }
    return result;
}

const std::vector<std::vector<core::GradientStop>>& gradientPresets()
{
    // Each family flows from subdued to vivid. These are editable color ramps,
    // not serialized preset identifiers or changes to the editor's paint colors.
    static const std::vector<std::vector<core::GradientStop>> presets {
        // Grays.
        gradient({0x000000, 0xffffff}),
        gradient({0x171b21, 0x78818b, 0xf0f3f5}),
        gradient({0x302a28, 0xaba299, 0xf5eee4}),
        gradient({0x303944, 0xb6c0cb, 0xffffff, 0x828d9a}),
        // Blues.
        gradient({0x071b3d, 0x1661b5, 0x91dcff}),
        gradient({0x173c71, 0x00a4c7, 0xb6f4ed}),
        gradient({0x162354, 0x4767df, 0xc2c9ff}),
        gradient({0x003b69, 0x23a9d1, 0xdaf8ff}),
        // Purples.
        gradient({0x25123d, 0x714aaa, 0xddd0ff}),
        gradient({0x39195b, 0xa044c3, 0xf6b8ef}),
        gradient({0x1f2364, 0x7355cd, 0xd0a5ef}),
        gradient({0x482a60, 0x997aa8, 0xeadde9}),
        // Pinks.
        gradient({0x6d204b, 0xde5f94, 0xffc9df}),
        gradient({0xa03062, 0xf27f9e, 0xffdabd}),
        gradient({0x773a67, 0xbd7ba0, 0xf9dce7}),
        gradient({0xbd315c, 0xff81a6, 0xffe3eb}),
        // Oranges.
        gradient({0x713018, 0xe87826, 0xffd886}),
        gradient({0xcd5124, 0xffa345, 0xffedb0}),
        gradient({0x9b4027, 0xea9270, 0xffddbc}),
        gradient({0x64273c, 0xe46b45, 0xffcb62}),
        // Reds.
        gradient({0x4b1321, 0xc13b45, 0xff9890}),
        gradient({0x8b1829, 0xed414e, 0xffb8a8}),
        gradient({0x5e1d34, 0xba5669, 0xf4b2aa}),
        gradient({0xb52432, 0xf36a45, 0xffbc70}),
        // Greens.
        gradient({0x123c32, 0x2b8b68, 0xb8e4aa}),
        gradient({0x235344, 0x65b28b, 0xd5f5d4}),
        gradient({0x354b25, 0x93ac50, 0xf1edac}),
        gradient({0x07535a, 0x26a596, 0xc5f5d2}),
        // Clouds.
        gradient({0x8ca6c5, 0xd8e8f5, 0xffffff}),
        gradient({0xa9a5c4, 0xe1d8ed, 0xfff4eb}),
        gradient({0xaccdcf, 0xe2eee7, 0xffffff}),
        gradient({0xc9a9a9, 0xf4e1d5, 0xfffaf3}),
        // Iridescence.
        gradient({0x8edee0, 0xc3b3f1, 0xf9bbdb, 0xffe9b5}),
        gradient({0x8bbbe4, 0xc4f0e6, 0xf2dcfc, 0xe9acc8}),
        gradient({0xb7a8e8, 0xf8d2db, 0xf8edc5, 0xa7e3db, 0x8fb3e5}),
        gradient({0x547bb6, 0x8bcbbd, 0xe4dcae, 0xd8a0c1, 0x8c6ca9}),
        // Pastels.
        gradient({0xf0b9cc, 0xd6c4ec, 0xb9d8f4}),
        gradient({0xffd5b7, 0xffedc7, 0xd5e6bc}),
        gradient({0xb9e3d9, 0xc6d8ef, 0xe4cff0}),
        gradient({0xf5c4bc, 0xf9e2c7, 0xdde9d2, 0xc8e3eb}),
    };
    return presets;
}

QImage gradientPreview(const std::vector<core::GradientStop>& stops)
{
    auto effect = core::defaultLayerEffect(core::LayerEffectType::GradientOverlay);
    effect.gradientStops = stops;
    QImage image(256, 1, QImage::Format_ARGB32_Premultiplied);
    for (int x = 0; x < image.width(); ++x) {
        const auto color = core::encodeColor(
            core::sampleLayerEffectGradient(effect, double(x) / (image.width() - 1)));
        image.setPixelColor(x, 0, QColor(color.red, color.green, color.blue, color.alpha));
    }
    return image;
}

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
    explicit SwatchDelegate(QSize tileSize, QObject* parent)
        : QStyledItemDelegate(parent), tileSize_(tileSize) {}
    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override
    {
        return tileSize_;
    }
    void paint(QPainter* painter, const QStyleOptionViewItem& option,
        const QModelIndex& index) const override
    {
        painter->save();
        if (!(option.state & QStyle::State_Enabled)) painter->setOpacity(.45);
        const auto area = option.rect.adjusted(3, 3, -3, -3);
        painter->setClipRect(area);
        for (int y = area.top(); y <= area.bottom(); y += 5)
            for (int x = area.left(); x <= area.right(); x += 5)
                painter->fillRect(x, y, 5, 5,
                    ((x - area.left()) / 5 + (y - area.top()) / 5) % 2
                        ? QColor(94, 98, 111) : QColor(160, 164, 175));
        const auto preview = index.data(gradientPreviewRole).value<QImage>();
        if (preview.isNull()) {
            painter->fillRect(area, index.data(Qt::UserRole).value<QColor>());
        } else {
            painter->setRenderHint(QPainter::SmoothPixmapTransform);
            painter->drawImage(area, preview);
        }
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
private:
    QSize tileSize_;
};

// Like the brush shelf, fixed-size tiles reflow while the containing panel is
// the only scroll area. No nested scrolling or per-swatch child widgets.
class SwatchGrid final : public QListWidget {
public:
    explicit SwatchGrid(QWidget* parent, QSize tileSize = {swatchExtent, swatchExtent})
        : QListWidget(parent)
    {
        setViewMode(QListView::IconMode);
        setFlow(QListView::LeftToRight);
        setMovement(QListView::Static);
        setResizeMode(QListView::Adjust);
        setWrapping(true);
        setUniformItemSizes(true);
        setGridSize(tileSize);
        setSpacing(0);
        setFrameShape(QFrame::NoFrame);
        setSelectionMode(QAbstractItemView::SingleSelection);
        setEditTriggers(QAbstractItemView::NoEditTriggers);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setMouseTracking(true);
        setStyleSheet(QStringLiteral("QListWidget { background: transparent; border: none; padding: 0; }"));
        setItemDelegate(new SwatchDelegate(tileSize, this));
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
        const int columns = std::max(1, viewport()->width() / gridSize().width());
        const int rows = std::max(1, (count() + columns - 1) / columns);
        setFixedHeight(rows * gridSize().height());
    }
};
} // namespace

SwatchesPanel::SwatchesPanel(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("SwatchesPanelContent"));
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
    content->setSpacing(9);
    QVBoxLayout* defaultsContent = nullptr;
    content->addWidget(collapsibleSection(i18n::text("swatches.panel.swatches"),
        QStringLiteral("ColorSwatches"), defaultsContent));
    defaults_ = new SwatchGrid(body);
    defaults_->setObjectName(QStringLiteral("DefaultColorSwatches"));
    i18n::setAccessibleName(defaults_, i18n::text("swatches.panel.default.color.swatches"));
    for (const auto rgb : defaultSwatches)
        setSwatchColor(new QListWidgetItem(defaults_), QColor::fromRgba(rgb));
    defaultsContent->addWidget(defaults_);

    QVBoxLayout* gradientContent = nullptr;
    content->addWidget(collapsibleSection(i18n::text("swatches.panel.gradient"),
        QStringLiteral("GradientSwatches"), gradientContent));
    gradients_ = new SwatchGrid(body, gradientTileSize);
    gradients_->setObjectName(QStringLiteral("GradientSwatches"));
    i18n::setAccessibleName(gradients_, i18n::text("swatches.panel.gradient.swatches"));
    for (const auto& preset : gradientPresets()) {
        auto* item = new QListWidgetItem(gradients_);
        item->setData(gradientPreviewRole, gradientPreview(preset));
    }
    const auto updateGradientLabels = [this] {
        for (int i = 0; i < gradients_->count(); ++i) {
            const auto label = i18n::text("swatches.panel.gradient.preset").arg(i + 1);
            auto* item = gradients_->item(i);
            item->setData(Qt::AccessibleTextRole, QString(label));
            item->setToolTip(label);
        }
    };
    updateGradientLabels();
    connect(&i18n::Localization::instance(), &i18n::Localization::languageChanged,
        this, updateGradientLabels);
    gradientContent->addWidget(gradients_);
    // Mouse clicks and keyboard activation apply a preset. Merely navigating
    // the grid with arrows does not create a series of undoable layer edits.
    connect(gradients_, &QListWidget::itemClicked, this,
        [this](QListWidgetItem* item) { chooseGradient(gradients_->row(item)); });
    connect(gradients_, &QListWidget::itemActivated, this,
        [this](QListWidgetItem* item) { chooseGradient(gradients_->row(item)); });
    setGradientEnabled(false);

    QVBoxLayout* customContent = nullptr;
    content->addWidget(collapsibleSection(i18n::text("swatches.panel.custom"),
        QStringLiteral("CustomSwatches"), customContent));
    auto* customRow = new QHBoxLayout;
    customRow->setSpacing(6);
    customRow->addStretch();
    auto* add = new QPushButton(toolGlyph(ToolGlyph::Plus), QString(), body);
    add->setObjectName(QStringLiteral("AddCustomSwatch"));
    i18n::setToolTip(add, i18n::text("swatches.panel.add.custom.color"));
    delete_ = new QPushButton(toolGlyph(ToolGlyph::Trash), QString(), body);
    delete_->setObjectName(QStringLiteral("DeleteCustomSwatch"));
    i18n::setToolTip(delete_, i18n::text("swatches.panel.delete.selected.custom.color"));
    for (auto* button : {add, delete_}) {
        // QSS minimum sizes are reapplied on polish, overriding setFixedSize's
        // minimum. Keep the 26x24 content plus the theme's one-pixel border.
        button->setStyleSheet(QStringLiteral("QPushButton { padding: 0; min-width: 26px; min-height: 24px; }"));
        button->setFixedSize(28, 26);
        button->setIconSize({18, 18});
        button->setAccessibleName(button->toolTip());
        customRow->addWidget(button);
    }
    customContent->addLayout(customRow);
    custom_ = new SwatchGrid(body);
    custom_->setObjectName(QStringLiteral("CustomColorSwatches"));
    i18n::setAccessibleName(custom_, i18n::text("swatches.panel.custom.color.swatches"));
    QSettings settings;
    for (const auto& stored : settings.value(customSwatchesKey).toStringList()) {
        const QColor color(stored);
        if (color.isValid())
            setSwatchColor(new QListWidgetItem(custom_), color);
    }
    customContent->addWidget(custom_);
    content->addStretch(1);
    scroll->setWidget(body);
    layout->addWidget(scroll);
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

void SwatchesPanel::setColors(core::EditorColors colors)
{
    colors_ = colors;
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

void SwatchesPanel::setForegroundColor(core::Rgba8 color)
{
    auto colors = colors_;
    colors.setColor(colors.active, color);
    setColors(colors);
}

void SwatchesPanel::setGradientEnabled(bool enabled)
{
    gradients_->setEnabled(enabled);
    const auto hint = i18n::text(enabled ? "swatches.panel.gradient.apply.hint"
        : "swatches.panel.gradient.disabled.hint");
    i18n::setToolTip(gradients_, hint);
    // The enabled header also exposes the explanation when the grid is disabled.
    if (auto* header = findChild<QToolButton*>("GradientSwatchesSectionHeader"))
        i18n::setToolTip(header, hint);
}

void SwatchesPanel::chooseGradient(int row)
{
    const auto& presets = gradientPresets();
    if (!gradients_->isEnabled() || row < 0 || std::size_t(row) >= presets.size()) return;
    if (onGradientSelected) onGradientSelected(presets[std::size_t(row)]);
}

void SwatchesPanel::chooseSwatch(QListWidget* grid)
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

void SwatchesPanel::editCustomSwatch(int row)
{
    if (colorDialog_) {
        colorDialog_->raise();
        colorDialog_->setFocus(Qt::OtherFocusReason);
        return;
    }
    const auto active = colors_.foreground();
    const auto initial = row >= 0 ? swatchColor(custom_->item(row))
        : QColor(active.red, active.green, active.blue, active.alpha);
    auto* owner = popupTopLevelOwner(this);
    auto* dialog = new ColorDialog(initial, owner);
    colorDialog_ = dialog;
    dialog->setObjectName(QStringLiteral("CustomSwatchColorDialog"));
    i18n::setWindowTitle(dialog, row >= 0 ? i18n::text("swatches.panel.edit.custom.color") : i18n::text("swatches.panel.add.custom.color"));
    dialog->setOptions(ColorDialog::ShowAlphaChannel | ColorDialog::DontUseNativeDialog);
    dialog->setCurrentColor(initial);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(this, &QObject::destroyed, dialog, &QDialog::reject);
    connect(dialog, &ColorDialog::colorSelected, this, [this, row](const QColor& color) {
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

void SwatchesPanel::saveCustomSwatches()
{
    QStringList stored;
    for (int i = 0; i < custom_->count(); ++i)
        stored.push_back(swatchColor(custom_->item(i)).name(QColor::HexArgb));
    QSettings settings;
    settings.setValue(customSwatchesKey, stored);
}

void SwatchesPanel::updateDeleteButton()
{
    delete_->setEnabled(!custom_->selectedItems().isEmpty());
}

} // namespace imageeditor::ui
