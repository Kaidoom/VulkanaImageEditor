#include "imageeditor/ui/Localization.hpp"
#include "imageeditor/ui/PreferencesDialog.hpp"
#include "imageeditor/ui/ShortcutsPanel.hpp"
#include "imageeditor/ui/ToolOptionsNumber.hpp"
#include "imageeditor/ui/ColorDialog.hpp"
#include "imageeditor/platform/ProjectIntegration.hpp"
#include <QCoreApplication>
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
#include <algorithm>

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
    i18n::setWindowTitle(this, i18n::text("preferences.dialog.preferences"));
    if (parent) setAttribute(Qt::WA_ShowWithoutActivating);
    setSizeGripEnabled(false);
    setFixedSize(720, 590);
    setProperty("workspacePreferredSize", QSize(720, 590));
    auto* layout = new QVBoxLayout(this); layout->setContentsMargins(22, 18, 22, 18); layout->setSpacing(12);
    auto* heading = i18n::make<QLabel>(i18n::text("preferences.dialog.preferences"), this); heading->setObjectName(QStringLiteral("ToolTitle"));
    layout->addWidget(heading);
    auto* tabs = new QTabWidget(this); tabs->setObjectName(QStringLiteral("PreferencesTabs"));
    layout->addWidget(tabs, 1);
    auto* general = new QWidget(tabs);
    auto* generalLayout = new QVBoxLayout(general); generalLayout->setContentsMargins(16, 14, 16, 12); generalLayout->setSpacing(10);
    general->setObjectName(QStringLiteral("PreferencesGeneralPage"));
    i18n::Localization::instance().direction(general);
    auto* languageRow = new QHBoxLayout;
    languageRow->addWidget(i18n::make<QLabel>(i18n::text("language.label"), general));
    language_ = new QComboBox(general);
    language_->setObjectName(QStringLiteral("LanguageCombo"));
    i18n::addItem(language_, i18n::text("language.system"), QStringLiteral("system"));
    const auto languages = i18n::Localization::instance().languages();
    for (const auto& language : languages)
        language_->addItem(language.nativeName, language.id);
    languageRow->addWidget(language_, 1); generalLayout->addLayout(languageRow);
    auto* languageNote = i18n::make<QLabel>(i18n::text("language.review.note"), general);
    languageNote->setWordWrap(true); languageNote->setObjectName(QStringLiteral("MutedLabel"));
    languageNote->setVisible(std::any_of(languages.begin(), languages.end(), [](const auto& language) { return language.reviewPending; }));
    generalLayout->addWidget(languageNote);
    auto* themes = new QWidget(tabs);
    themes->setObjectName(QStringLiteral("PreferencesThemesPage"));
    auto* themeLayout = new QVBoxLayout(themes);
    themeLayout->setContentsMargins(16, 14, 16, 12); themeLayout->setSpacing(10);
    auto* presetRow = new QHBoxLayout;
    presetRow->addWidget(i18n::make<QLabel>(i18n::text("preferences.dialog.preset"), themes));
    preset_ = new QComboBox(themes); preset_->setObjectName(QStringLiteral("ThemePresetCombo"));
    preset_->setItemDelegate(new QStyledItemDelegate(preset_));
    for (const auto& info : themePresets()) i18n::addItem(preset_, i18n::text((QByteArray("theme.preset.") + info.key).constData()), int(info.preset));
    presetRow->addWidget(preset_, 1);
    customize_ = i18n::make<QPushButton>(i18n::text("preferences.dialog.customize.preset"), themes);
    i18n::setToolTip(customize_, i18n::text("preferences.dialog.copy.this.preset.s.colors.into.custom"));
    presetRow->addWidget(customize_);
    themeLayout->addLayout(presetRow);
    auto* hint = i18n::make<QLabel>(i18n::text("preferences.dialog.preview.a.preset.or.customize.its.colors.apply.saves"), themes);
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
        cellLayout->addWidget(i18n::make<QLabel>(i18n::text((QByteArray("theme.color.") + QByteArray(themeColorKey(role)).toLower()).constData()), cell));
        auto* swatch = new ThemeSwatch(cell); swatch->setFixedHeight(36);
        swatch->setObjectName(QStringLiteral("ThemeColor_") + QString::fromLatin1(themeColorKey(role)));
        i18n::setAccessibleName(swatch, i18n::text((QByteArray("theme.color.") + QByteArray(themeColorKey(role)).toLower()).constData()));
        i18n::setToolTip(swatch, i18n::text("preferences.dialog.choose.1").arg(i18n::text((QByteArray("theme.color.") + QByteArray(themeColorKey(role)).toLower()).constData()).toLower()));
        swatches_[i] = swatch; cellLayout->addWidget(swatch);
        grid->addWidget(cell, static_cast<int>(i / 3), static_cast<int>(i % 3));
        connect(swatch, &QPushButton::clicked, this, [this, role] { pickColor(role); });
    }
    for (int column=0; column<3; ++column) grid->setColumnStretch(column, 1);
    grid->setRowStretch(static_cast<int>((swatches_.size() + 2) / 3), 1);
    scroll->setWidget(body); themeLayout->addWidget(scroll, 1);
    auto* measurementTitle = i18n::make<QLabel>(i18n::text("preferences.dialog.canvas.measurement"), general);
    measurementTitle->setObjectName(QStringLiteral("SectionLabel"));
    generalLayout->addWidget(measurementTitle);
    advancedMeasurementReadout_ = i18n::make<QCheckBox>(i18n::text("preferences.dialog.advanced.measurement.readout"), general);
    advancedMeasurementReadout_->setObjectName(QStringLiteral("AdvancedMeasurementReadout"));
    i18n::setToolTip(advancedMeasurementReadout_, i18n::text("preferences.dialog.show.signed.x.and.y.in.addition.to.distance"));
    generalLayout->addWidget(advancedMeasurementReadout_);
    layerOutlinesVisible_ = i18n::make<QCheckBox>(i18n::text("preferences.dialog.selected.layer.outlines.in.move.mode"), general);
    layerOutlinesVisible_->setObjectName(QStringLiteral("SelectedLayerOutlines"));
    i18n::setToolTip(layerOutlinesVisible_, i18n::text("preferences.dialog.show.each.selected.layer.s.transformed.bounds.togglelayeroutlinesaction.toggles"));
    generalLayout->addWidget(layerOutlinesVisible_);
    collapseLayerSelectionOnEmptyClick_ = i18n::make<QCheckBox>(i18n::text("preferences.dialog.keep.only.the.last.selected.layer.when.clicking.empty"), general);
    collapseLayerSelectionOnEmptyClick_->setObjectName(QStringLiteral("CollapseLayerSelectionOnEmptyClick"));
    i18n::setToolTip(collapseLayerSelectionOnEmptyClick_, i18n::text("preferences.dialog.collapse.multiple.selected.layers.when.clicking.outside.the.canvas"));
    generalLayout->addWidget(collapseLayerSelectionOnEmptyClick_);
    auto* nudgeRow = new QHBoxLayout;
    nudgeRow->addWidget(i18n::make<QLabel>(i18n::text("preferences.dialog.shift.nudge.distance"), general));
    shiftNudgePixels_ = new ToolOptionsNumber(general);
    shiftNudgePixels_->setObjectName(QStringLiteral("ShiftNudgePixels"));
    shiftNudgePixels_->setDecimals(0);
    shiftNudgePixels_->setRange(1, 1000);
    i18n::setSuffix(shiftNudgePixels_, i18n::text("preferences.dialog.px"));
    i18n::setToolTip(shiftNudgePixels_, i18n::text("preferences.dialog.move.layers.or.selection.masks.in.document.pixels.nudge"));
    nudgeRow->addWidget(shiftNudgePixels_);
    generalLayout->addLayout(nudgeRow);
    auto* hintRow = new QHBoxLayout;
    hintRow->addWidget(i18n::make<QLabel>(i18n::text("preferences.dialog.bottom.tool.notifications"), general));
    toolHintPosition_ = new QComboBox(general);
    toolHintPosition_->setObjectName(QStringLiteral("ToolHintPosition"));
    i18n::addItems(toolHintPosition_, {i18n::text("preferences.dialog.left"), i18n::text("preferences.dialog.center"), i18n::text("preferences.dialog.disabled")});
    hintRow->addWidget(toolHintPosition_);
    generalLayout->addLayout(hintRow);
    if (platform::projectIntegrationAvailable(QCoreApplication::applicationFilePath())) {
        auto* integration = i18n::make<QPushButton>(i18n::text("project.integration.preferences"), general);
        integration->setObjectName(QStringLiteral("ProjectIntegrationSettings"));
        connect(integration, &QPushButton::clicked, this, [this] { done(ProjectFilesRequested); });
        generalLayout->addWidget(integration);
    }
    generalLayout->addStretch(1);
    i18n::addTab(tabs, general, i18n::text("preferences.dialog.general"));
    i18n::addTab(tabs, themes, i18n::text("preferences.dialog.themes"));
    shortcuts_ = new ShortcutsPanel(draft_.shortcuts, tabs);
    i18n::addTab(tabs, shortcuts_, i18n::text("preferences.dialog.shortcuts"));
    shortcuts_->onChanged = [this](const ShortcutBindings& bindings) {
        draft_.shortcuts = bindings; preview();
    };
    status_ = new QLabel(this); status_->setObjectName(QStringLiteral("ErrorLabel")); status_->setWordWrap(true);
    status_->setVisible(false); layout->addWidget(status_);
    auto* buttons = new i18n::ButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Apply, this);
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
    connect(language_, &QComboBox::currentIndexChanged, this, [this] {
        draft_.language = language_->currentData().toString(); preview();
    });
}
void PreferencesDialog::refresh()
{
    const QSignalBlocker languageBlocker(language_);
    language_->setCurrentIndex(std::max(0, language_->findData(draft_.language)));
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
        i18n::setText(status_, i18n::text("preferences.dialog.finish.or.cancel.the.pending.key.binding.first")); status_->show(); return false;
    }
    if (onApply && !onApply(draft_)) {
        i18n::setText(status_, i18n::text("preferences.dialog.preferences.could.not.be.saved.check.permissions.or.available"));
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
    ColorDialog picker(draft_.theme.custom[static_cast<std::size_t>(role)], owner);
    picker.setObjectName(QStringLiteral("ThemeColorDialog"));
    i18n::setWindowTitle(&picker, i18n::text((QByteArray("theme.color.") + QByteArray(themeColorKey(role)).toLower()).constData()));
    picker.setOptions(ColorDialog::DontUseNativeDialog);
    if (picker.exec() != QDialog::Accepted) return;
    auto color = picker.selectedColor(); if (!color.isValid()) return;
    color.setAlpha(255); draft_.theme.custom[static_cast<std::size_t>(role)] = color; refresh(); preview();
}
}
