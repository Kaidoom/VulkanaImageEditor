#include "imageeditor/ui/LayerInfoPanel.hpp"
#include "imageeditor/core/LayerGeometry.hpp"
#include "imageeditor/core/LayerTransform.hpp"
#include "imageeditor/ui/FontFamilyPicker.hpp"
#include "imageeditor/ui/Localization.hpp"
#include "imageeditor/ui/ToolOptionsNumber.hpp"
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <numbers>

namespace imageeditor::ui {
LayerInfoPanel::LayerInfoPanel(QWidget* parent)
    : QWidget(parent)
{
    setObjectName("LayerInfoPanel");
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    transform_ = new QWidget(this);
    auto* form = new QFormLayout(transform_);
    form->setContentsMargins(0, 0, 0, 0);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    auto field = [&](const char* id, const char* object) {
        auto* value = new QLabel;
        value->setObjectName(object);
        value->setTextFormat(Qt::PlainText);
        value->setWordWrap(true);
        form->addRow(i18n::make<QLabel>(i18n::text(id)), value);
        return value;
    };
    size_ = field("properties.info.size", "LayerInfoSize");
    angle_ = field("properties.info.angle", "LayerInfoAngle");
    position_ = field("properties.info.position", "LayerInfoPosition");
    layout->addWidget(transform_);
    name_ = new QLabel;
    name_->setObjectName("PropertiesTargetName");
    name_->setTextFormat(Qt::PlainText);
    name_->setWordWrap(true);
    layout->addWidget(name_);
    text_ = new QWidget(this);
    auto* textForm = new QFormLayout(text_);
    textForm->setContentsMargins(0, 0, 0, 0);
    textForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    textForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
    family_ = new FontFamilyPicker(text_);
    family_->setObjectName("LayerInfoFont");
    family_->setMinimumContentsLength(8);
    family_->setMinimumWidth(80);
    textForm->addRow(i18n::make<QLabel>(i18n::text("properties.info.font")), family_);
    fontSize_ = new ToolOptionsNumber(text_);
    fontSize_->setObjectName("LayerInfoFontSize");
    fontSize_->setRange(.25, 2048);
    fontSize_->setDecimals(2);
    fontSize_->setSuffix(" px");
    i18n::setAccessibleName(fontSize_, i18n::text("properties.info.size"));
    i18n::setAccessibleName(family_, i18n::text("properties.info.font"));
    textForm->addRow(i18n::make<QLabel>(i18n::text("properties.info.size")), fontSize_);
    color_ = new QPushButton(text_);
    color_->setObjectName("LayerInfoTextColor");
    color_->setMinimumHeight(28);
    i18n::setAccessibleName(color_, i18n::text("properties.info.text.color"));
    textForm->addRow(i18n::make<QLabel>(i18n::text("properties.info.color")), color_);
    layout->addWidget(text_);
    for (auto* w : std::array<QWidget*, 3> { family_, fontSize_, color_ })
        i18n::setToolTip(w, i18n::text("properties.info.whole.text"));
    family_->onFamilyChosen = [this](const QString& family) {
        if (!updating_ && target_ && onTextFormat) {
            core::TextStylePatch patch;
            patch.family = family.toStdString();
            patch.style = std::string { };
            onTextFormat(target_, std::move(patch));
        }
    };
    // Commit at the numeric interaction boundary, not once per key/stamp.
    const auto publish = [this] {
        if (updating_) return;
        if (!updating_ && numericDirty_ && target_ && onTextFormat) {
            numericDirty_ = false;
            core::TextStylePatch patch;
            patch.sizePixels = fontSize_->value();
            onTextFormat(target_, std::move(patch));
        }
        // Focus alone reveals the representative value for editing. Restore
        // Mixed on leaving an untouched field rather than implying uniformity.
        const QSignalBlocker block(fontSize_);
        fontSize_->setValue(displayedFontSize_);
        fontSize_->setIndeterminate(displayedSizeMixed_);
    };
    connect(fontSize_, &QDoubleSpinBox::valueChanged, this, [this] {
        if (!updating_)
            numericDirty_ = true;
    });
    connect(fontSize_->findChild<QLineEdit*>(), &QLineEdit::textEdited, this, [this] {
        if (!updating_)
            numericDirty_ = true;
    });
    fontSize_->onInteractionFinished = publish;
    connect(fontSize_, &QDoubleSpinBox::editingFinished, this, publish);
    connect(color_, &QPushButton::clicked, this, [this] {
        if (target_ && onTextColor)
            onTextColor(target_);
    });
    setLayer(nullptr);
}
void LayerInfoPanel::setLayer(const core::Layer* layer, const core::LayerContainer* container,
    std::optional<core::DocumentBounds> groupBounds)
{
    const QScopedValueRollback guard(updating_, true);
    const auto id = layer ? layer->id : 0;
    if (id != target_) {
        family_->finishSearch(false);
        fontSize_->clearFocus();
        numericDirty_ = false;
    }
    target_ = id;
    text_->setVisible(layer && std::holds_alternative<core::TextLayer>(layer->payload));
    text_->setEnabled(layer && std::holds_alternative<core::TextLayer>(layer->payload));
    transform_->setVisible(bool(layer) || groupBounds.has_value());
    if (!layer) {
        if (container) {
            i18n::setText(name_, i18n::Message("%1 (%2)").arg(QString::fromStdString(container->name),
                i18n::text(container->kind == core::ContainerKind::Folder ? "properties.info.folder"
                                                                          : "properties.info.group")));
        } else {
            i18n::setText(name_, i18n::text("properties.panel.no.layer.selected"));
        }
        if (groupBounds) {
            const auto e = groupBounds->extent();
            size_->setText(QString("%1 × %2 px").arg(e.width, 0, 'f', 1).arg(e.height, 0, 'f', 1));
            angle_->setText("—");
            position_->setText("—");
        }
        return;
    }
    // Report the same geometry frame used by transform handles, excluding effects.
    const auto frame = core::layerInteractionBounds(*layer);
    const auto& t = layer->localToDocument;
    const auto a = t.map({ frame.x, frame.y }), b = t.map({ frame.right(), frame.y }),
               c = t.map({ frame.x, frame.bottom() });
    size_->setText(QString("%1 × %2 px")
            .arg(std::hypot(b.x - a.x, b.y - a.y), 0, 'f', 1)
            .arg(std::hypot(c.x - a.x, c.y - a.y), 0, 'f', 1));
    angle_->setText(QString("%1°").arg(std::atan2(b.y - a.y, b.x - a.x) * 180 / std::numbers::pi, 0, 'f', 1));
    position_->setText(QString("%1, %2 px").arg(a.x, 0, 'f', 1).arg(a.y, 0, 'f', 1));
    i18n::setText(name_, i18n::Message("%1 (%2)").arg(QString::fromStdString(layer->name),
        i18n::text(std::holds_alternative<core::RasterLayer>(layer->payload) ? "properties.panel.raster.layer"
                : std::holds_alternative<core::TextLayer>(layer->payload)
                ? "properties.panel.editable.text.layer"
                : std::holds_alternative<core::ShapeLayer>(layer->payload)
                ? "properties.panel.editable.shape.layer"
                : "properties.info.adjustment")));
    const auto* text = std::get_if<core::TextLayer>(&layer->payload);
    if (!text)
        return;
    const auto normalized = core::normalizedText(*text);
    const auto style = normalized.runs.empty() ? normalized.defaultStyle : normalized.runs.front().style;
    bool familyMixed = false, sizeMixed = false, colorMixed = false;
    for (const auto& run : normalized.runs) {
        familyMixed |= run.style.font.family != style.font.family;
        sizeMixed |= run.style.sizePixels != style.sizePixels;
        colorMixed |= run.style.color != style.color;
    }
    displayedFontSize_ = style.sizePixels;
    displayedSizeMixed_ = sizeMixed;
    if (!family_->searching())
        family_->setDisplayedFamily(QString::fromStdString(style.font.family), familyMixed);
    if (!fontSize_->hasFocus() && !fontSize_->interactionActive()) {
        const QSignalBlocker block(fontSize_);
        fontSize_->setValue(style.sizePixels);
        fontSize_->setIndeterminate(sizeMixed);
    }
    const auto color = QColor(style.color.red, style.color.green, style.color.blue, style.color.alpha);
    i18n::setText(color_,
        colorMixed ? i18n::text("properties.info.mixed")
                   : i18n::Message(color.name(QColor::HexArgb).toUpper()));
    QPixmap swatch(24, 16);
    swatch.fill(colorMixed ? Qt::transparent : color);
    color_->setIcon(QIcon(swatch));
}
}
