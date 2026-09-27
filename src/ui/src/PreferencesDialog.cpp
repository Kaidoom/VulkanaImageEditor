#include "imageeditor/ui/PreferencesDialog.hpp"
#include "imageeditor/ui/ShortcutsPanel.hpp"
#include "imageeditor/ui/ToolOptionsNumber.hpp"
#include <QColorDialog>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QLabel>
#include <QComboBox>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStyleOptionButton>
#include <QStyledItemDelegate>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QWindow>

namespace imageeditor::ui {
namespace {
class ThemeSwatch final : public QPushButton {
public:
    using QPushButton::QPushButton;
    QColor color;
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        QStyleOptionButton option; initStyleOption(&option); option.text.clear();
        style()->drawControl(QStyle::CE_PushButton, &option, &painter, this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setBrush(color); painter.setPen(palette().color(QPalette::Mid));
        painter.drawRoundedRect(QRectF(8, 7, 27, height() - 14), 4, 4);
        painter.setPen(palette().color(isEnabled() ? QPalette::Active : QPalette::Disabled, QPalette::ButtonText));
        painter.drawText(rect().adjusted(45, 0, -8, 0), Qt::AlignVCenter | Qt::AlignLeft, color.name().toUpper());
    }
};
}
PreferencesDialog::PreferencesDialog(PreferencesState initial, QWidget* parent)
    : QDialog(parent, parent ? Qt::SubWindow : Qt::Dialog), draft_(initial), applied_(initial)
{
    setObjectName(QStringLiteral("PreferencesDialog"));
    setWindowTitle(tr("Preferences"));
    if (parent) setAttribute(Qt::WA_ShowWithoutActivating);
    setSizeGripEnabled(false);
    setFixedSize(720, 590);
    setProperty("workspacePreferredSize", QSize(720, 590));
    auto* layout = new QVBoxLayout(this); layout->setContentsMargins(22, 18, 22, 18); layout->setSpacing(12);
    auto* heading = new QLabel(tr("Preferences"), this); heading->setObjectName(QStringLiteral("ToolTitle"));
    layout->addWidget(heading);
    auto* tabs = new QTabWidget(this); tabs->setObjectName(QStringLiteral("PreferencesTabs"));
    layout->addWidget(tabs, 1);
    auto* general = new QWidget(tabs);
    auto* generalLayout = new QVBoxLayout(general); generalLayout->setContentsMargins(16, 14, 16, 12); generalLayout->setSpacing(10);
    general->setObjectName(QStringLiteral("PreferencesGeneralPage"));
    auto* themes = new QWidget(tabs);
    themes->setObjectName(QStringLiteral("PreferencesThemesPage"));
    auto* themeLayout = new QVBoxLayout(themes);
    themeLayout->setContentsMargins(16, 14, 16, 12); themeLayout->setSpacing(10);
    auto* presetRow = new QHBoxLayout;
    presetRow->addWidget(new QLabel(tr("Preset"), themes));
    preset_ = new QComboBox(themes); preset_->setObjectName(QStringLiteral("ThemePresetCombo"));
    preset_->setItemDelegate(new QStyledItemDelegate(preset_));
    for (const auto& info : themePresets()) preset_->addItem(tr(info.label), int(info.preset));
    presetRow->addWidget(preset_, 1);
    customize_ = new QPushButton(tr("Customize preset"), themes);
    customize_->setToolTip(tr("Copy this preset's colors into Custom."));
    presetRow->addWidget(customize_);
    themeLayout->addLayout(presetRow);
    auto* hint = new QLabel(tr("Preview a preset or customize its colors. Apply saves your changes. Workspace colors never affect your artwork."), themes);
    hint->setObjectName(QStringLiteral("MutedLabel")); hint->setWordWrap(true); themeLayout->addWidget(hint);
    auto* scroll = new QScrollArea(themes); scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setObjectName(QStringLiteral("PreferencesThemeScroll"));
    scroll->verticalScrollBar()->setObjectName(QStringLiteral("PreferencesScrollBar"));
    scroll->verticalScrollBar()->setProperty("editorScrollBar", true);
    auto* body = new QWidget(scroll); auto* grid = new QGridLayout(body);
    grid->setContentsMargins(0, 4, 8, 4); grid->setHorizontalSpacing(18); grid->setVerticalSpacing(10);
    for (std::size_t i = 0; i < swatches_.size(); ++i) {
        const auto role = static_cast<ThemeColor>(i);
        auto* cell = new QWidget(body); auto* cellLayout = new QVBoxLayout(cell);
        cellLayout->setContentsMargins(0, 0, 0, 0); cellLayout->setSpacing(4);
        cellLayout->addWidget(new QLabel(tr(themeColorLabel(role)), cell));
        auto* swatch = new ThemeSwatch(cell); swatch->setFixedHeight(36);
        swatch->setObjectName(QStringLiteral("ThemeColor_") + QString::fromLatin1(themeColorKey(role)));
        swatch->setAccessibleName(tr(themeColorLabel(role)));
        swatch->setToolTip(tr("Choose %1").arg(tr(themeColorLabel(role)).toLower()));
        swatches_[i] = swatch; cellLayout->addWidget(swatch);
        grid->addWidget(cell, static_cast<int>(i / 3), static_cast<int>(i % 3));
        connect(swatch, &QPushButton::clicked, this, [this, role] { pickColor(role); });
    }
    for (int column=0; column<3; ++column) grid->setColumnStretch(column, 1);
    grid->setRowStretch(static_cast<int>((swatches_.size() + 2) / 3), 1);
    scroll->setWidget(body); themeLayout->addWidget(scroll, 1);
    auto* measurementTitle = new QLabel(tr("Canvas & measurement"), general);
    measurementTitle->setObjectName(QStringLiteral("SectionLabel"));
    generalLayout->addWidget(measurementTitle);
    advancedMeasurementReadout_ = new QCheckBox(tr("Advanced measurement readout"), general);
    advancedMeasurementReadout_->setObjectName(QStringLiteral("AdvancedMeasurementReadout"));
    advancedMeasurementReadout_->setToolTip(tr("Show signed ΔX and ΔY in addition to distance and angle."));
    generalLayout->addWidget(advancedMeasurementReadout_);
    layerOutlinesVisible_ = new QCheckBox(tr("Selected layer outlines in Move mode"), general);
    layerOutlinesVisible_->setObjectName(QStringLiteral("SelectedLayerOutlines"));
    layerOutlinesVisible_->setToolTip(tr("Show each selected layer's transformed bounds · {{ToggleLayerOutlinesAction}} toggles. Darker outside the canvas."));
    generalLayout->addWidget(layerOutlinesVisible_);
    collapseLayerSelectionOnEmptyClick_ = new QCheckBox(tr("Keep only the last-selected layer when clicking empty space"), general);
    collapseLayerSelectionOnEmptyClick_->setObjectName(QStringLiteral("CollapseLayerSelectionOnEmptyClick"));
    collapseLayerSelectionOnEmptyClick_->setToolTip(tr("Collapse multiple selected layers when clicking outside the canvas or in empty Layers-panel space. The primary layer remains selected."));
    generalLayout->addWidget(collapseLayerSelectionOnEmptyClick_);
    auto* nudgeRow = new QHBoxLayout;
    nudgeRow->addWidget(new QLabel(tr("Shift + nudge distance"), general));
    shiftNudgePixels_ = new ToolOptionsNumber(general);
    shiftNudgePixels_->setObjectName(QStringLiteral("ShiftNudgePixels"));
    shiftNudgePixels_->setDecimals(0);
    shiftNudgePixels_->setRange(1, 1000);
    shiftNudgePixels_->setSuffix(tr(" px"));
    shiftNudgePixels_->setToolTip(tr("Move layers or selection masks in document pixels. Nudge keys without Shift always move 1 px."));
    nudgeRow->addWidget(shiftNudgePixels_);
    generalLayout->addLayout(nudgeRow);
    auto* hintRow = new QHBoxLayout;
    hintRow->addWidget(new QLabel(tr("Bottom tool notifications"), general));
    toolHintPosition_ = new QComboBox(general);
    toolHintPosition_->setObjectName(QStringLiteral("ToolHintPosition"));
    toolHintPosition_->addItems({tr("Left"), tr("Center"), tr("Disabled")});
    hintRow->addWidget(toolHintPosition_);
    generalLayout->addLayout(hintRow);
    generalLayout->addStretch(1);
    tabs->addTab(general, tr("General"));
    tabs->addTab(themes, tr("Themes"));
    shortcuts_ = new ShortcutsPanel(draft_.shortcuts, tabs);
    tabs->addTab(shortcuts_, tr("Shortcuts"));
    shortcuts_->onChanged = [this](const ShortcutBindings& bindings) {
        draft_.shortcuts = bindings; preview();
    };
    status_ = new QLabel(this); status_->setObjectName(QStringLiteral("ErrorLabel")); status_->setWordWrap(true);
    status_->setVisible(false); layout->addWidget(status_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Apply, this);
    buttons->button(QDialogButtonBox::Ok)->setObjectName(QStringLiteral("PreferencesOk"));
    buttons->button(QDialogButtonBox::Cancel)->setObjectName(QStringLiteral("PreferencesCancel"));
    buttons->button(QDialogButtonBox::Apply)->setObjectName(QStringLiteral("PreferencesApply"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] { if (apply()) accept(); });
    connect(buttons, &QDialogButtonBox::rejected, this, &PreferencesDialog::reject);
    connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, [this] { apply(); });
    connect(preset_, &QComboBox::currentIndexChanged, this, [this](int index) {
        draft_.theme.preset = static_cast<ThemePreset>(preset_->itemData(index).toInt()); refresh(); preview();
    });
    connect(customize_, &QPushButton::clicked, this, [this] {
        draft_.theme.custom = resolvedThemeColors(draft_.theme); draft_.theme.preset = ThemePreset::Custom; refresh(); preview();
    });
    connect(advancedMeasurementReadout_, &QCheckBox::toggled, this, [this](bool enabled) {
        draft_.advancedMeasurementReadout = enabled; preview();
    });
    connect(layerOutlinesVisible_, &QCheckBox::toggled, this, [this](bool enabled) {
        draft_.layerOutlinesVisible = enabled; preview();
    });
    connect(collapseLayerSelectionOnEmptyClick_, &QCheckBox::toggled, this, [this](bool enabled) {
        draft_.collapseLayerSelectionOnEmptyClick = enabled; preview();
    });
    connect(shiftNudgePixels_, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        draft_.shiftNudgePixels = int(value); preview();
    });
    connect(toolHintPosition_, &QComboBox::currentIndexChanged, this, [this](int index) {
        draft_.toolHintPosition = index; preview();
    });
    refresh();
}
void PreferencesDialog::refresh()
{
    updateShortcutHints(this, draft_.shortcuts);
    const QSignalBlocker blocker(preset_); preset_->setCurrentIndex(preset_->findData(int(draft_.theme.preset)));
    const QSignalBlocker measurementBlocker(advancedMeasurementReadout_);
    advancedMeasurementReadout_->setChecked(draft_.advancedMeasurementReadout);
    const QSignalBlocker outlineBlocker(layerOutlinesVisible_);
    layerOutlinesVisible_->setChecked(draft_.layerOutlinesVisible);
    const QSignalBlocker collapseBlocker(collapseLayerSelectionOnEmptyClick_);
    collapseLayerSelectionOnEmptyClick_->setChecked(draft_.collapseLayerSelectionOnEmptyClick);
    const QSignalBlocker nudgeBlocker(shiftNudgePixels_);
    shiftNudgePixels_->setValue(draft_.shiftNudgePixels);
    const QSignalBlocker hintBlocker(toolHintPosition_);
    toolHintPosition_->setCurrentIndex(draft_.toolHintPosition);
    const auto colors = resolvedThemeColors(draft_.theme);
    const bool custom = draft_.theme.preset == ThemePreset::Custom;
    customize_->setEnabled(!custom);
    for (std::size_t i = 0; i < swatches_.size(); ++i) {
        static_cast<ThemeSwatch*>(swatches_[i])->color = colors[i];
        swatches_[i]->setEnabled(custom); swatches_[i]->update();
    }
}
void PreferencesDialog::preview() { status_->hide(); updateShortcutHints(this, draft_.shortcuts); if (onPreview) onPreview(draft_); }
bool PreferencesDialog::apply()
{
    if (shortcuts_->pending()) {
        status_->setText(tr("Finish or cancel the pending key binding first.")); status_->show(); return false;
    }
    if (onApply && !onApply(draft_)) {
        status_->setText(tr("Preferences could not be saved. Check permissions or available disk space, then retry."));
        status_->show(); return false;
    }
    applied_ = draft_; status_->hide(); return true;
}
void PreferencesDialog::reject()
{
    if (shortcuts_->pending()) { shortcuts_->cancelPending(); return; }
    if (draft_ != applied_ && onPreview) onPreview(applied_);
    QDialog::reject();
}
void PreferencesDialog::pickColor(ThemeColor role)
{
    QWidget* owner = this; while (owner->parentWidget()) owner = owner->parentWidget();
    QColorDialog picker(draft_.theme.custom[static_cast<std::size_t>(role)], owner);
    picker.setObjectName(QStringLiteral("ThemeColorDialog"));
    picker.setWindowTitle(tr(themeColorLabel(role)));
    picker.setOptions(QColorDialog::DontUseNativeDialog);
    picker.setWindowFlag(Qt::Tool);
    picker.setWindowModality(Qt::ApplicationModal);
    (void)owner->winId(); (void)picker.winId();
    picker.windowHandle()->setTransientParent(owner->windowHandle());
    if (picker.exec() != QDialog::Accepted) return;
    auto color = picker.selectedColor(); if (!color.isValid()) return;
    color.setAlpha(255); draft_.theme.custom[static_cast<std::size_t>(role)] = color; refresh(); preview();
}
}
