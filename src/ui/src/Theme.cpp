#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/ui/PopupOwnership.hpp"

#include <QApplication>
#include <QColor>
#include <QComboBox>
#include <QFont>
#include <QFontDatabase>
#include <QPalette>
#include <QStringList>
#include <QStyleFactory>
#include <QRegularExpression>
#include <QHash>
#include <QKeyEvent>
#include <QKeySequence>
#include <QAbstractItemView>
#include <QPointer>
#include <QProxyStyle>
#include <QPainter>
#include <QStyleOption>
#include <QTextEdit>
#include <QPlainTextEdit>
#include <algorithm>

// Documented Qt global API; the installed header exposes its declaration only
// to qdoc, so Qt clients declare it explicitly.
QT_BEGIN_NAMESPACE
Q_GUI_EXPORT void qt_set_sequence_auto_mnemonic(bool);
QT_END_NAMESPACE

// Pull shared theme assets into every consumer of the static UI library,
// including widget tests, instead of relying on executable-specific resources.
static void initializeThemeResources()
{
    Q_INIT_RESOURCE(editor_theme);
}

namespace imageeditor::ui {
namespace {
ThemeSettings activeTheme;
ThemeColors activeColors = darkThemeColors();
// Wheel motion scrolls the workspace/popup, never changes a closed dropdown.
// Install once for the whole application, including Qt-owned dialog controls.
class WidgetInputPolicy final : public QObject {
public:
    explicit WidgetInputPolicy(QObject* parent) : QObject(parent) {}
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        // The shortcut dispatcher/capture filters run first. Unbound Tab must
        // not change control focus; leave modified OS chords and actual text
        // tabs alone. Mouse focus and arrows/Enter keep their normal behavior.
        if (qobject_cast<QWidget*>(watched)
            && (event->type() == QEvent::KeyPress || event->type() == QEvent::ShortcutOverride)) {
            const auto* key = static_cast<QKeyEvent*>(event);
            if ((key->key() == Qt::Key_Tab || key->key() == Qt::Key_Backtab)
                && !(key->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) {
                if (const auto* text = qobject_cast<QTextEdit*>(watched); text && !text->tabChangesFocus()) return false;
                if (const auto* text = qobject_cast<QPlainTextEdit*>(watched); text && !text->tabChangesFocus()) return false;
                event->accept(); return true;
            }
        }
        // Uniform keyboard activation for our choice lists, including styles
        // whose popup otherwise recognizes Space but not Return. Editable
        // font-search fields keep their own commit/focus behavior.
        if (event->type() == QEvent::KeyPress) {
            const auto* key = static_cast<QKeyEvent*>(event);
            auto* popup = QApplication::activePopupWidget();
            auto* widget = qobject_cast<QWidget*>(watched);
            QPointer<QComboBox> combo = popup ? qobject_cast<QComboBox*>(popupLogicalParent(popup)) : nullptr;
            if (combo && !combo->isEditable() && widget
                && (widget == popup || popup->isAncestorOf(widget)) && key->key() == Qt::Key_Escape) {
                combo->hidePopup();
                event->accept(); return true;
            }
            if (combo && !combo->isEditable() && widget
                && (widget == popup || popup->isAncestorOf(widget))
                && (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter)) {
                const auto index = combo->view()->currentIndex();
                if (!key->isAutoRepeat() && index.isValid()
                    && (index.flags() & Qt::ItemIsEnabled) && (index.flags() & Qt::ItemIsSelectable)) {
                    combo->hidePopup();
                    combo->setCurrentIndex(index.row());
                    if (combo) Q_EMIT combo->activated(index.row());
                    if (combo) Q_EMIT combo->textActivated(combo->currentText());
                }
                event->accept(); return true;
            }
        }
        if (event->type() == QEvent::KeyPress && !QApplication::activePopupWidget()) {
            auto* combo = qobject_cast<QComboBox*>(watched);
            const auto* key = static_cast<QKeyEvent*>(event);
            if (combo && !combo->isEditable() && combo->isEnabled()
                && (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter)) {
                if (!key->isAutoRepeat()) combo->showPopup();
                event->accept(); return true;
            }
        }
        if (event->type() != QEvent::Wheel) return false;
        for (auto* widget = qobject_cast<QWidget*>(watched); widget; widget = widget->parentWidget()) {
            if (qobject_cast<QComboBox*>(widget)) {
                event->ignore(); // Qt can propagate to an enclosing scroll area.
                return true;
            }
            // A popup list owns its scrolling; don't mistake it for the field.
            if (widget->isWindow()) break;
        }
        return false;
    }
};
class ThemeStyle final : public QProxyStyle {
public:
    ThemeStyle() : QProxyStyle(QStyleFactory::create(QStringLiteral("Fusion"))) {}
    int styleHint(StyleHint hint, const QStyleOption* option = nullptr, const QWidget* widget = nullptr,
        QStyleHintReturn* data = nullptr) const override
    {
        if (hint == SH_MenuBar_AltKeyNavigation || hint == SH_UnderlineShortcut) return 0;
        return QProxyStyle::styleHint(hint, option, widget, data);
    }
    void drawPrimitive(PrimitiveElement element, const QStyleOption* option, QPainter* painter,
        const QWidget* widget = nullptr) const override
    {
        if (element == PE_IndicatorArrowDown || element == PE_IndicatorArrowUp
            || element == PE_IndicatorSpinUp || element == PE_IndicatorSpinDown
            || element == PE_IndicatorArrowLeft || element == PE_IndicatorArrowRight) {
            painter->save(); painter->setRenderHint(QPainter::Antialiasing);
            const auto group = option->state.testFlag(State_Enabled) ? QPalette::Active : QPalette::Disabled;
            painter->setPen(QPen(option->palette.color(group, QPalette::ButtonText), 1.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter->translate(QRectF(option->rect).center());
            if (element == PE_IndicatorArrowUp || element == PE_IndicatorSpinUp) painter->rotate(180);
            else if (element == PE_IndicatorArrowLeft) painter->rotate(90);
            else if (element == PE_IndicatorArrowRight) painter->rotate(-90);
            const double radius = std::min(4.0, std::min(option->rect.width(), option->rect.height()) / 3.0);
            painter->drawPolyline(QPolygonF({{-radius,-radius*.4},{0,radius*.6},{radius,-radius*.4}}));
            painter->restore(); return;
        }
        if (element == PE_IndicatorItemViewItemCheck) {
            painter->save(); painter->setRenderHint(QPainter::Antialiasing);
            const auto area = QRectF(option->rect).adjusted(2, 4, -2, -4);
            const bool on = option->state.testFlag(State_On);
            painter->setPen(QPen(option->palette.color(on ? QPalette::Text : QPalette::PlaceholderText), 1.2));
            painter->setBrush(Qt::NoBrush); painter->drawEllipse(area);
            if (on) { painter->setBrush(option->palette.text()); painter->drawEllipse(area.center(), 1.8, 1.8); }
            else painter->drawLine(area.topLeft() - QPointF(0, 1), area.bottomRight() + QPointF(0, 1));
            painter->restore(); return;
        }
        QProxyStyle::drawPrimitive(element, option, painter, widget);
    }
};
}
const ThemeSettings& currentThemeSettings() { return activeTheme; }
QColor themeColor(ThemeColor role) { return activeColors.at(static_cast<std::size_t>(role)); }
QColor themeTone(const char* value)
{
    // Each former Dark shade has one semantic owner. Recolor in one pass, so
    // a chosen color equal to another old token cannot cascade replacements.
    static const QHash<QString, ThemeColor> roles = [] {
        QHash<QString, ThemeColor> result;
        const auto add = [&](ThemeColor role, const char* values) {
            for (const auto& color : QString::fromLatin1(values).split(' ', Qt::SkipEmptyParts)) result.insert(color, role);
        };
        add(ThemeColor::Background, "#151820 #101219 #14171E");
        add(ThemeColor::Surface, "#171A22 #191D27 #1C2029 #1B1E26");
        add(ThemeColor::Control, "#222630 #20242F #202530 #222733 #242936 #242A36 #252A36 #272C38 #282D39 #2B3241 #343B4A #252B3A");
        add(ThemeColor::Border, "#343A49 #292D37 #292E3B #303542 #303645 #343947 #353B4B #3A4151 #41495B #4A5369 #515A70 #59637A #66708B");
        add(ThemeColor::Selection, "#34405E #3A4665 #30384D #1B2030");
        add(ThemeColor::Accent, "#6577F3 #7C8BFA #8D9AF8 #C3C9FF #AAB4FF");
        add(ThemeColor::Text, "#E8EAF0 #C2C7D4 #C8CDD8 #D7DAE3 #D8DBE4 #E7E9EF #EEF0F6 #EEF0F7 #F0F2F7 #F3F4F7 #F4F5F8");
        add(ThemeColor::SecondaryText, "#9299AA #5F6573 #626978 #666C79 #686E7C #777E90 #858C9D #858DA1 #9DA4B4 #AAB0BE");
        add(ThemeColor::SelectedText, "#FFFFFF");
        add(ThemeColor::Canvas, "#0E1013");
        return result;
    }();
    const auto key = QString::fromLatin1(value).toUpper();
    const QColor original(key);
    const auto found = roles.constFind(key);
    if (found == roles.cend()) { // Status-only success/error colors, not accent.
        return themeColor(ThemeColor::Background).lightnessF() > .5 ? original.darker(180) : original;
    }
    const auto role = *found;
    const auto target = themeColor(role);
    if (role == ThemeColor::Canvas) return target;
    // These are the source stylesheet's immutable anchors, not the current
    // Dark preset. Changing a preset must not reinterpret the original tokens.
    static const ThemeColors sourceAnchors {QColor("#151820"), QColor("#171A22"), QColor("#222630"), QColor("#E8EAF0"),
        QColor("#9299AA"), QColor("#6577F3"), QColor("#34405E"), QColor("#34405E"), QColor("#FFFFFF"),
        QColor("#343A49"), QColor("#42464D"), QColor("#888A90"), QColor("#777A80"), QColor("#B6BDCC")};
    const auto anchor = sourceAnchors.at(static_cast<std::size_t>(role));
    if (target == anchor) return original;
    const bool reverse = (anchor.lightnessF() > .5) != (target.lightnessF() > .5);
    const int delta = (qGray(original.rgb()) - qGray(anchor.rgb())) * (reverse ? -1 : 1);
    return QColor(std::clamp(target.red() + delta, 0, 255), std::clamp(target.green() + delta, 0, 255),
        std::clamp(target.blue() + delta, 0, 255));
}

core::Rgba8 editorAccent()
{
    const auto color = QApplication::palette().color(QPalette::Highlight);
    return {static_cast<std::uint8_t>(color.red()), static_cast<std::uint8_t>(color.green()),
        static_cast<std::uint8_t>(color.blue()), static_cast<std::uint8_t>(color.alpha())};
}

void applyEditorTheme(QApplication& application)
{
    qt_set_sequence_auto_mnemonic(false);
    initializeThemeResources();
    if (!application.findChild<QObject*>(QStringLiteral("WidgetInputPolicy"), Qt::FindDirectChildrenOnly)) {
        auto* policy = new WidgetInputPolicy(&application);
        policy->setObjectName(QStringLiteral("WidgetInputPolicy"));
        application.installEventFilter(policy);
    }
    application.setStyle(new ThemeStyle);

    const auto families = QFontDatabase::families();
    const QString fontFamily = families.contains(QStringLiteral("Inter"))
        ? QStringLiteral("Inter") : QStringLiteral("Noto Sans");
    QFont font(fontFamily);
    font.setPointSizeF(9.5);
    application.setFont(font);
    applyEditorTheme(application, ThemeSettings{});
}

void applyEditorTheme(QApplication& application, const ThemeSettings& settings)
{
    activeTheme = settings;
    activeColors = resolvedThemeColors(settings);

    QPalette palette;
    palette.setColor(QPalette::Window, themeTone("#151820"));
    palette.setColor(QPalette::WindowText, themeTone("#E8EAF0"));
    palette.setColor(QPalette::Base, themeTone("#101219"));
    // Alternating file-browser rows are not selections. Keep them neutral;
    // only genuinely selected items use Highlight/HighlightedText.
    palette.setColor(QPalette::AlternateBase, themeColor(ThemeColor::Surface));
    palette.setColor(QPalette::ToolTipBase, themeTone("#242936"));
    palette.setColor(QPalette::ToolTipText, themeTone("#F4F5F8"));
    palette.setColor(QPalette::Text, themeTone("#E8EAF0"));
    palette.setColor(QPalette::Button, themeTone("#20242F"));
    palette.setColor(QPalette::ButtonText, themeTone("#E8EAF0"));
    palette.setColor(QPalette::BrightText, themeTone("#FFFFFF"));
    palette.setColor(QPalette::Highlight, themeTone("#6577F3"));
    palette.setColor(QPalette::HighlightedText, themeTone("#FFFFFF"));
    palette.setColor(QPalette::PlaceholderText, themeTone("#777E90"));
    palette.setColor(QPalette::Disabled, QPalette::Text, themeTone("#686E7C"));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, themeTone("#686E7C"));
    palette.setColor(QPalette::Mid, themeColor(ThemeColor::Border));
    if (application.palette() != palette) application.setPalette(palette);

    auto sheet = QStringLiteral(R"QSS(
        QMainWindow, QDialog {
            background: #151820;
        }
        QDialog#NewDocumentDialog, QDialog#PreferencesDialog, QDialog#ExportDialog, QDialog#AboutVulkanaDialog {
            border: 1px solid #343A49;
            border-radius: 10px;
        }
        QWidget#VulkanCanvasContainer {
            background: #0E1013;
        }
        QWidget#CanvasWorkspace {
            background: #0E1013;
        }
        QWidget#PanelOverlaySurface {
            background: transparent;
        }
        QWidget#PixelPreviewBadge {
            background: #151820;
            border: 1px solid #343A49;
            border-radius: 6px;
        }
        QWidget#OverlayPanelResizeHandle,
        QWidget#OverlayLeftPanelResizeHandle {
            background: transparent;
        }
        QWidget#OverlayPanelCard,
        QWidget#OverlayLeftPanelCard {
            background: #151820;
            border: 1px solid #343A49;
            border-radius: 9px;
        }
        QWidget#OverlayPanelCard[dropTarget="true"],
        QWidget#OverlayLeftPanelCard[dropTarget="true"] {
            border: 2px solid #8D9AF8;
            background: #1B2030;
        }
        QWidget#PanelDockColumnContent {
            background: #151820;
            border-radius: 8px;
        }
        QWidget#WorkspacePanel,
        QWidget#ColorPanelShell,
        QWidget#LayersPanel,
        QWidget#PropertiesPanelShell {
            background: #151820;
            border: 1px solid #292E3B;
            border-radius: 8px;
        }
        QWidget#WorkspacePanel[floatingPanel="true"],
        QWidget#ColorPanelShell[floatingPanel="true"],
        QWidget#LayersPanel[floatingPanel="true"],
        QWidget#PropertiesPanelShell[floatingPanel="true"] {
            border-color: #4A5369;
        }
        QMainWindow::separator {
            background: #101219;
            width: 8px;
            height: 8px;
            border-left: 1px solid #292E3B;
        }
        QMainWindow::separator:hover {
            background: #20242F;
            border-left-color: #6577F3;
        }

        QMenuBar {
            background: #171A22;
            color: #D8DBE4;
            border-bottom: 1px solid #292E3B;
            padding: 3px 6px;
            spacing: 4px;
        }
        QMenuBar::item {
            border-radius: 5px;
            padding: 5px 9px;
        }
        QMenuBar::item:selected {
            background: #292E3B;
        }
        QMenu {
            background: #20242F;
            border: 1px solid #353B4B;
            border-radius: 7px;
            padding: 6px;
        }
        QMenu::item {
            border-radius: 5px;
            padding: 7px 28px 7px 10px;
        }
        QMenu::item:selected {
            background: #34405E;
        }
        QMenu::separator {
            height: 1px;
            background: #343947;
            margin: 5px 8px;
        }

        QToolBar {
            background: #171A22;
            border: none;
            border-right: 1px solid #292E3B;
            spacing: 4px;
            padding: 7px 6px;
        }
        QToolBar#ToolRail {
            /* Keep the complete tool set and quick colors accessible without
               shrinking the established icon or pointer-target dimensions. */
            spacing: 2px;
        }
        QToolBar#ToolOptionsBar {
            background: #171A22;
            border: none;
            border-bottom: 1px solid #292E3B;
            padding: 0;
            margin: 0;
            spacing: 0;
        }
        QWidget#ToolOptionsRoot,
        QScrollArea#ToolOptionsViewport,
        QScrollArea#ToolOptionsViewport > QWidget > QWidget,
        QWidget#BrushOptionsPage,
        QStackedWidget#ToolOptionsPageStack {
            background: transparent;
        }
        QLabel#ToolOptionsContext {
            color: #8D9AF8;
            font-size: 8pt;
            font-weight: 750;
            padding-right: 2px;
        }
        QLabel#ToolOptionLabel {
            color: #AAB0BE;
            font-size: 8.5pt;
        }
        QToolBar#ToolRail[railDock="top"] {
            border: none;
            border-bottom: 1px solid #292E3B;
        }
        QToolBar#ToolRail[railDock="bottom"] {
            border: none;
            border-top: 1px solid #292E3B;
        }
        QToolBar#ToolRail[railDock="right-panel"] {
            background: #171A22;
            border: none;
            border-right: 1px solid #292E3B;
        }
        QToolButton {
            background: transparent;
            color: #C2C7D4;
            border: 1px solid transparent;
            border-radius: 7px;
            padding: 7px;
        }
        QToolButton:hover {
            background: #252A36;
            border-color: #343A49;
        }
        QToolButton:pressed {
            background: #30384D;
        }
        QToolButton:checked {
            background: #34405E;
            border-color: #6577F3;
        }
        QToolButton#PreviousToolOptions, QToolButton#NextToolOptions {
            padding: 4px;
            border-radius: 6px;
        }
        QToolButton:focus {
            border-color: #8D9AF8;
        }
        QToolButton:disabled {
            color: #5F6573;
        }
        QToolButton[toolOptionsButton="true"] {
            background: #222630;
            border: 1px solid #343A49;
            border-radius: 6px;
            padding: 4px 8px;
        }
        QToolButton[toolOptionsButton="true"]:hover {
            background: #2B3241;
            border-color: #66708B;
        }
        QToolButton[toolOptionsButton="true"]:checked,
        QToolButton[toolOptionsButton="true"]:pressed,
        QToolButton[toolOptionsPrimary="true"] {
            background: #34405E;
            border-color: palette(highlight);
        }
        QToolButton[toolOptionsButton="true"]:focus {
            border-color: #8D9AF8;
        }
        QToolButton[toolOptionsButton="true"]:disabled {
            background: #1B1E26;
            border-color: #292E3B;
            color: #5F6573;
        }
        QToolButton[toolOptionsIcon="true"] {
            padding: 4px;
        }
        QToolButton[toolOptionsStatus="true"] {
            padding: 0px 6px;
            border: none;
            border-radius: 3px;
        }

        QListView#LayerList {
            background: #14171E;
            border: none;
            padding: 6px;
            selection-background-color: #34405E;
            selection-color: #FFFFFF;
        }
        QListView#LayerList::item {
            min-height: 46px;
            border-radius: 7px;
            padding: 5px 7px;
            margin: 2px 0;
        }
        /* LayerRowDelegate draws the subtle theme-derived tint and left strip. */
        QListView#LayerList::item:hover, QListView#LayerList::item:selected {
            background: transparent;
        }
        QListView#LayerList:focus {
            border: 1px solid #6577F3;
        }
        QListView#LayerList::indicator {
            width: 17px;
            height: 17px;
        }
        QListView#LayerList::drop-indicator {
            background: #8D9AF8;
            border: 1px solid #C3C9FF;
            height: 2px;
        }

        QToolButton#BrushTipPickerButton,
        QToolButton#BrushGrainPickerButton {
            background: #20242F;
            border: 1px solid #343A49;
            border-radius: 7px;
            padding: 5px 8px;
            text-align: left;
        }
        QToolButton#BrushTipPickerButton:hover,
        QToolButton#BrushGrainPickerButton:hover {
            background: #282D39;
            border-color: #6577F3;
        }
        QToolButton#BrushTipSectionHeader,
        QToolButton#BrushGrainSectionHeader,
        QToolButton#BrushTipSectionHeader:checked,
        QToolButton#BrushGrainSectionHeader:checked {
            background: #1C2029;
            border: 1px solid #303645;
            border-radius: 6px;
            color: #D7DAE3;
            font-weight: 600;
            padding: 6px 8px;
            text-align: left;
        }
        QListWidget#BrushTipPickerGrid,
        QListWidget#BrushGrainPickerGrid,
        QListWidget#BrushPresetGrid {
            background: #171A22;
            border: 1px solid #353B4B;
            border-radius: 7px;
            padding: 5px;
            outline: none;
        }
        QListWidget#BrushTipPickerGrid::item,
        QListWidget#BrushGrainPickerGrid::item,
        QListWidget#BrushPresetGrid::item {
            border: 1px solid transparent;
            border-radius: 7px;
            color: #C8CDD8;
            padding: 4px;
        }
        QListWidget#BrushTipPickerGrid::item:hover,
        QListWidget#BrushGrainPickerGrid::item:hover,
        QListWidget#BrushPresetGrid::item:hover {
            background: #252A36;
            border-color: #41495B;
        }
        QListWidget#BrushTipPickerGrid::item:selected,
        QListWidget#BrushGrainPickerGrid::item:selected,
        QListWidget#BrushPresetGrid::item:selected {
            background: #34405E;
            border-color: #6577F3;
            color: #FFFFFF;
        }
        QListWidget#BrushTipPickerGrid::item:disabled,
        QListWidget#BrushGrainPickerGrid::item:disabled,
        QListWidget#BrushPresetGrid::item:disabled {
            color: #626978;
        }

        QLabel#SectionLabel {
            color: #9299AA;
            font-size: 8pt;
            font-weight: 700;
            padding-top: 5px;
        }
        QLabel#LayerControlLabel {
            color: #AAB0BE;
            font-size: 8.5pt;
        }
        QLabel#MutedLabel {
            color: #858C9D;
        }
        QLabel#ErrorLabel {
            color: #F08A96;
        }
        QLabel#CanvasFpsStatus {
            color: #91D6A8;
            font-family: monospace;
            font-size: 8pt;
            padding: 0 6px;
        }
        QLabel#ToolTitle {
            color: #F0F2F7;
            font-size: 13pt;
            font-weight: 650;
        }

        QPushButton, QComboBox, QSpinBox, QDoubleSpinBox, QFontComboBox, QLineEdit {
            background: #222630;
            color: #E7E9EF;
            border: 1px solid #343A49;
            border-radius: 6px;
            padding: 6px 8px;
            min-height: 20px;
        }
        QPushButton:hover, QComboBox:hover, QSpinBox:hover, QDoubleSpinBox:hover,
        QFontComboBox:hover, QLineEdit:hover {
            border-color: #515A70;
            background: #282D39;
        }
        QPushButton:pressed {
            background: #30384D;
        }
        QPushButton:checked {
            background: #3A4665;
            border-color: #6577F3;
        }
        QPushButton:focus, QComboBox:focus, QSpinBox:focus, QDoubleSpinBox:focus,
        QFontComboBox:focus, QLineEdit:focus {
            border-color: #6577F3;
        }
        QPushButton:disabled, QComboBox:disabled, QSpinBox:disabled,
        QDoubleSpinBox:disabled, QLineEdit:disabled {
            background: #1B1E26;
            color: #666C79;
            border-color: #292D37;
        }

        /* One rounded field; the chevron is not a separate raised button. */
        QComboBox {
            padding: 6px 30px 6px 9px;
            /* Use compact selected rows like the font completion list, not
               Fusion's non-editable menu delegate/checkmark gutter. */
            combobox-popup: 0;
        }
        QComboBox:on {
            background: #282D39;
            border-color: #6577F3;
        }
        QComboBox::drop-down {
            subcontrol-origin: border;
            subcontrol-position: center right;
            width: 26px;
            margin: 1px;
            background: transparent;
            border: none;
            border-radius: 5px;
        }
        QComboBox::down-arrow {
            image: url(:/theme/chevron-down.svg);
            width: 14px;
            height: 14px;
        }
        QComboBox::down-arrow:disabled { image: url(:/theme/chevron-down-disabled.svg); }
        QComboBox QLineEdit, QComboBox QLineEdit:hover,
        QComboBox QLineEdit:focus, QComboBox QLineEdit:disabled {
            background: transparent;
            border: none;
            border-radius: 0;
            padding: 0;
            min-height: 0;
            selection-background-color: #6577F3;
        }

        /* Shared stepper chrome, also used by Qt's QColorDialog spin boxes. */
        QSpinBox, QDoubleSpinBox { padding-right: 23px; }
        QSpinBox::up-button, QSpinBox::down-button,
        QDoubleSpinBox::up-button, QDoubleSpinBox::down-button {
            subcontrol-origin: border;
            width: 17px;
            background: #272C38;
            border: none;
            border-left: 1px solid #3A4151;
        }
        QSpinBox::up-button, QDoubleSpinBox::up-button {
            subcontrol-position: top right;
            border-top-right-radius: 5px;
            border-bottom: 1px solid #3A4151;
        }
        QSpinBox::down-button, QDoubleSpinBox::down-button {
            subcontrol-position: bottom right;
            border-bottom-right-radius: 5px;
        }
        QSpinBox::up-button:hover, QSpinBox::down-button:hover,
        QDoubleSpinBox::up-button:hover, QDoubleSpinBox::down-button:hover { background: #343B4A; }
        QSpinBox::up-arrow, QDoubleSpinBox::up-arrow {
            image: url(:/theme/chevron-up.svg); width: 12px; height: 12px;
        }
        QSpinBox::down-arrow, QDoubleSpinBox::down-arrow {
            image: url(:/theme/chevron-down.svg); width: 12px; height: 12px;
        }
        QSpinBox::up-arrow:disabled, QDoubleSpinBox::up-arrow:disabled { image: url(:/theme/chevron-up-disabled.svg); }
        QSpinBox::down-arrow:disabled, QDoubleSpinBox::down-arrow:disabled { image: url(:/theme/chevron-down-disabled.svg); }
        /* These controls paint palette-aware chevrons themselves. */
        QDoubleSpinBox[compactValueControl="true"]::up-arrow,
        QDoubleSpinBox[compactValueControl="true"]::down-arrow { image: none; width: 0; height: 0; }

        QDoubleSpinBox[compactValueControl="true"] {
            background: #202530;
            border: 1px solid #3A4151;
            border-radius: 6px;
            padding: 0 0 0 4px;
            min-height: 28px;
            max-height: 28px;
        }
        QDoubleSpinBox[compactValueControl="true"]:hover {
            background: #242A36;
            border-color: #59637A;
        }
        QDoubleSpinBox[compactValueControl="true"]:focus {
            border-color: #7C8BFA;
        }
        QDoubleSpinBox[compactValueControl="true"] QLineEdit#CompactValueEditor,
        QDoubleSpinBox[compactValueControl="true"] QLineEdit#CompactValueEditor:hover,
        QDoubleSpinBox[compactValueControl="true"] QLineEdit#CompactValueEditor:focus {
            background: transparent;
            border: none;
            border-radius: 0;
            padding: 0 2px;
            min-height: 0;
            color: #EEF0F6;
            selection-background-color: #6577F3;
        }
        QDoubleSpinBox[compactValueControl="true"]::up-button,
        QDoubleSpinBox[compactValueControl="true"]::down-button {
            subcontrol-origin: border;
            width: 17px;
            background: #272C38;
            border: none;
            border-left: 1px solid #3A4151;
        }
        QDoubleSpinBox[compactValueControl="true"]::up-button {
            subcontrol-position: top right;
            border-top-right-radius: 5px;
            border-bottom: 1px solid #3A4151;
        }
        QDoubleSpinBox[compactValueControl="true"]::down-button {
            subcontrol-position: bottom right;
            border-bottom-right-radius: 5px;
        }
        QDoubleSpinBox[compactValueControl="true"]::up-button:hover,
        QDoubleSpinBox[compactValueControl="true"]::down-button:hover {
            background: #343B4A;
        }

        QSlider::groove:horizontal {
            background: #303542;
            height: 4px;
            border-radius: 2px;
        }
        QSlider::sub-page:horizontal {
            background: #6577F3;
            border-radius: 2px;
        }
        QSlider::handle:horizontal {
            background: #E8EAF0;
            border: 2px solid #6577F3;
            width: 13px;
            height: 13px;
            margin: -6px 0;
            border-radius: 7px;
        }
        QSlider:focus::handle:horizontal {
            border-color: #FFFFFF;
        }

        QCheckBox:focus {
            color: #FFFFFF;
        }

        QScrollArea, QScrollArea > QWidget > QWidget {
            background: #171A22;
            border: none;
        }
        /* Default for every panel, popup and future scroll area: no opt-in
           property or per-panel copy of the styling is needed. */
        QScrollBar:vertical {
            background: transparent;
            width: 12px;
            margin: 0 3px;
            border: none;
        }
        QScrollBar::handle:vertical {
            background: #515A70;
            min-height: 28px;
            border-radius: 3px;
        }
        QScrollBar::handle:vertical:hover {
            background: #9299AA;
        }
        QScrollBar::handle:vertical:pressed {
            background: #6577F3;
        }
        QScrollBar::add-line:vertical,
        QScrollBar::sub-line:vertical {
            height: 0;
            background: transparent;
            border: none;
        }
        QScrollBar::up-arrow:vertical,
        QScrollBar::down-arrow:vertical {
            width: 0;
            height: 0;
            image: none;
        }
        QScrollBar::add-page:vertical,
        QScrollBar::sub-page:vertical {
            background: transparent;
        }
        QScrollBar:horizontal {
            background: transparent;
            height: 12px;
            margin: 3px 0;
            border: none;
        }
        QScrollBar::handle:horizontal {
            background: #515A70;
            min-width: 28px;
            border-radius: 3px;
        }
        QScrollBar::handle:horizontal:hover { background: #9299AA; }
        QScrollBar::handle:horizontal:pressed { background: #6577F3; }
        QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal {
            width: 0;
            background: transparent;
            border: none;
        }
        QScrollBar::left-arrow:horizontal, QScrollBar::right-arrow:horizontal {
            width: 0;
            height: 0;
            image: none;
        }
        QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal {
            background: transparent;
        }
        QLabel#StatusNotification {
            background: #222630;
            color: #E8EAF0;
            border: 1px solid #343A49;
            border-radius: 4px;
            padding: 6px 10px;
        }
        QStatusBar {
            background: #171A22;
            color: #9DA4B4;
            border-top: 1px solid #292E3B;
        }
        QStatusBar::item {
            border: none;
        }
        QToolTip {
            background: #272C38;
            color: #F3F4F7;
            border: 1px solid #41495B;
            padding: 5px 7px;
        }
        QSplitter::handle {
            background: #292E3B;
        }
        QTabWidget::pane { border: 1px solid #343A49; border-radius: 7px; background: #171A22; }
        QTabBar::tab { padding: 9px 18px; margin-right: 4px; color: #9299AA;
            background: #151820; border-bottom: 2px solid transparent; }
        QTabBar::tab:selected { color: #E8EAF0; border-bottom-color: #6577F3; background: #171A22; }
        QTabBar::tab:hover { color: #E8EAF0; background: #222733; }
    )QSS");
    static const QRegularExpression colorExpression(QStringLiteral("#[0-9A-Fa-f]{6}"));
    QString resolved;
    qsizetype offset = 0;
    auto matches = colorExpression.globalMatch(sheet);
    while (matches.hasNext()) {
        const auto match = matches.next();
        resolved += QStringView(sheet).mid(offset, match.capturedStart() - offset);
        resolved += themeTone(match.captured().toLatin1().constData()).name();
        offset = match.capturedEnd();
    }
    resolved += QStringView(sheet).mid(offset);
    // Dedicated document chrome, independent of ordinary panel-category tabs.
    // Append after legacy tone mapping so explicit custom tab colors are exact.
    resolved += QStringLiteral(R"QSS(
        QWidget#DocumentTabsPanel { background: %1; border-bottom: 1px solid %2; }
        QTabBar#DocumentTabs { background: transparent; }
        QTabBar#DocumentTabs::tab { padding: 5px 16px 5px 14px; margin-right: 2px;
            color: %5; background: %3; border: none; border-bottom: 2px solid transparent; }
        QTabBar#DocumentTabs::tab:selected { color: %6; background: %4; border-bottom-color: %7; }
        QTabBar#DocumentTabs::tab:hover { color: %6; }
        QTabBar#DocumentTabs::close-button { subcontrol-position: right; right: 8px; }
    )QSS").arg(themeColor(ThemeColor::Surface).name(), themeColor(ThemeColor::Border).name(),
        themeColor(ThemeColor::DocumentTabInactive).name(), themeColor(ThemeColor::DocumentTabActive).name(),
        themeColor(ThemeColor::SecondaryText).name(), themeColor(ThemeColor::Text).name(),
        themeColor(ThemeColor::Accent).name());
    if (themeColor(ThemeColor::Text).lightnessF() < .5) {
        resolved.replace(QStringLiteral(":/theme/chevron-up.svg"), QStringLiteral(":/theme/chevron-up-light.svg"));
        resolved.replace(QStringLiteral(":/theme/chevron-up-disabled.svg"), QStringLiteral(":/theme/chevron-up-disabled-light.svg"));
        resolved.replace(QStringLiteral(":/theme/chevron-down.svg"), QStringLiteral(":/theme/chevron-down-light.svg"));
        resolved.replace(QStringLiteral(":/theme/chevron-down-disabled.svg"), QStringLiteral(":/theme/chevron-down-disabled-light.svg"));
    }
    // Reassigning identical QSS still sends StyleChange to every widget and
    // repolishes the workspace. A matching Custom/preset needs no visual work.
    if (application.styleSheet() != resolved) application.setStyleSheet(resolved);
}

} // namespace imageeditor::ui
