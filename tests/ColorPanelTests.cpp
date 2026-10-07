#include "imageeditor/ui/ColorPanel.hpp"
#include "imageeditor/ui/SwatchesPanel.hpp"
#include "imageeditor/ui/ColorPicker.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/ui/UiLayoutConfig.hpp"

#include <QApplication>
#include "imageeditor/ui/ColorDialog.hpp"
#include <QListWidget>
#include <QLineEdit>
#include <QSpinBox>
#include <QToolButton>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QScreen>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QWindow>

#include <iostream>

namespace ui = imageeditor::ui;
namespace core = imageeditor::core;
namespace {
int failures = 0;
#define CHECK(condition) do { if (!(condition)) { ++failures; std::cerr << "Line " << __LINE__ << ": " #condition "\n"; } } while (false)
const auto key = QStringLiteral("editor/custom-swatches-v1");
void settle()
{
    for (int i = 0; i < 4; ++i) {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
    }
}
struct Panel {
    ui::SwatchesPanel panel;
    QListWidget* defaults = panel.findChild<QListWidget*>("DefaultColorSwatches");
    QListWidget* custom = panel.findChild<QListWidget*>("CustomColorSwatches");
    QPushButton* add = panel.findChild<QPushButton*>("AddCustomSwatch");
    QPushButton* remove = panel.findChild<QPushButton*>("DeleteCustomSwatch");
    Panel()
    {
        panel.resize(340, 500);
        panel.show();
        settle();
        CHECK(defaults && custom && add && remove);
    }
    imageeditor::ui::ColorDialog* picker() { return panel.findChild<imageeditor::ui::ColorDialog*>("CustomSwatchColorDialog"); }
    void click(QListWidget* grid, int row)
    {
        auto* scroll = panel.findChild<QScrollArea*>();
        scroll->ensureWidgetVisible(grid);
        settle();
        QTest::mouseClick(grid->viewport(), Qt::LeftButton, {}, grid->visualItemRect(grid->item(row)).center());
        settle();
    }
    void doubleClickCustom(int row)
    {
        click(custom, row);
        QTest::mouseDClick(custom->viewport(), Qt::LeftButton, {}, custom->visualItemRect(custom->item(row)).center());
        settle();
    }
    void accept(const QColor& color)
    {
        auto* dialog = picker();
        CHECK(dialog);
        if (!dialog) return;
        CHECK(!dialog->isWindow() && !dialog->windowHandle());
        CHECK(dialog->minimumSize() == dialog->maximumSize());
        dialog->setCurrentColor(color);
        dialog->accept();
        settle();
    }
};
QColor color(QListWidget* grid, int row)
{
    return grid->item(row)->data(Qt::UserRole).value<QColor>();
}
void defaultsAndWorkingColors()
{
    QSettings().remove(key);
    Panel p;
    CHECK(p.defaults->count() == 50);
    CHECK(color(p.defaults, 0) == QColor(Qt::black));
    CHECK(color(p.defaults, 9) == QColor(Qt::white));
    for (int i=1; i<10; ++i) {
        const auto c = color(p.defaults,i);
        CHECK(c.red() == c.green() && c.green() == c.blue());
        CHECK(c.red() > color(p.defaults,i-1).red());
    }
    CHECK(p.custom->count() == 0 && !p.remove->isEnabled());
    CHECK(!p.add->icon().isNull() && !p.remove->icon().isNull());
    int notifications = 0;
    p.panel.onColorsChanged = [&](auto) { ++notifications; };
    const core::EditorColors initial {{8,9,10,255}, {20,30,40,200}, core::ColorSlot::Secondary};
    p.panel.setColors(initial);
    CHECK(notifications == 0);
    p.click(p.defaults, 0);
    CHECK(p.panel.colors().primary == initial.primary);
    CHECK(p.panel.colors().secondary == core::Rgba8(0,0,0,255));
    CHECK(p.panel.colors().active == core::ColorSlot::Secondary);
    CHECK(notifications == 1 && !p.remove->isEnabled());
    QTest::mouseDClick(p.defaults->viewport(), Qt::LeftButton, {}, p.defaults->visualItemRect(p.defaults->item(0)).center());
    settle();
    CHECK(!p.picker() && color(p.defaults,0) == QColor(Qt::black));
    p.defaults->setFocus();
    QTest::keyClick(p.defaults, Qt::Key_Right);
    settle();
    CHECK(p.panel.colors().secondary == core::Rgba8(28,28,28,255));
}
void customLifecycle()
{
    QSettings().remove(key);
    const QColor first(26,64,155,91), edited(219,120,44,167), second(80,210,90);
    {
        Panel p;
        p.add->click();settle();
        CHECK(p.picker());
        if (p.picker()) p.picker()->reject();
        settle();
        CHECK(p.custom->count()==0 && !p.remove->isEnabled());
        p.add->click();settle();p.accept(first);
        CHECK(p.custom->count()==1 && color(p.custom,0)==first);
        CHECK(p.custom->item(0)->toolTip()=="#1A409B5B");
        CHECK(p.remove->isEnabled());
        CHECK(p.panel.foregroundColor()==core::Rgba8(26,64,155,91));
        p.doubleClickCustom(0);
        CHECK(p.picker() && p.picker()->currentColor()==first);
        if(p.picker()) { p.picker()->setCurrentColor(Qt::yellow);p.picker()->reject(); }
        settle();
        CHECK(color(p.custom,0)==first && p.panel.foregroundColor()==core::Rgba8(26,64,155,91));
        p.doubleClickCustom(0);p.accept(edited);
        CHECK(p.custom->count()==1 && color(p.custom,0)==edited);
        CHECK(p.panel.foregroundColor()==core::Rgba8(219,120,44,167));
        p.add->click();settle();p.accept(second);
        CHECK(p.custom->count()==2 && color(p.custom,1)==second);
        p.click(p.defaults,9);
        CHECK(!p.remove->isEnabled() && p.custom->selectedItems().isEmpty());
        p.click(p.custom,0);
        CHECK(p.remove->isEnabled() && p.defaults->selectedItems().isEmpty());
        p.custom->clearSelection();settle();
        CHECK(!p.remove->isEnabled());
    }
    {
        Panel reopened;
        CHECK(reopened.custom->count()==2);
        CHECK(color(reopened.custom,0)==edited && color(reopened.custom,1)==second);
        CHECK(!reopened.remove->isEnabled());
        reopened.click(reopened.custom,0);
        const auto active=reopened.panel.foregroundColor();
        reopened.remove->click();settle();
        CHECK(reopened.custom->count()==1 && color(reopened.custom,0)==second);
        CHECK(!reopened.remove->isEnabled());
        CHECK(reopened.panel.foregroundColor()==active);
    }
    Panel final;
    CHECK(final.custom->count()==1 && color(final.custom,0)==second);
    final.click(final.custom,0);
    final.panel.setForegroundColor({1,2,3,255});
    CHECK(!final.remove->isEnabled());
    final.click(final.custom,0);final.remove->click();settle();
    CHECK(final.custom->count()==0 && QSettings().value(key).toStringList().isEmpty());
}
void gridsReflowAndShortPanelsScroll()
{
    QStringList stored;
    for(int i=0;i<50;++i)stored.push_back(QColor::fromHsv(i*7,190,210,130+i).name(QColor::HexArgb));
    stored.push_back("invalid color");
    QSettings().setValue(key,stored);
    Panel p;
    CHECK(p.custom->count()==50);
    int smallHeight=0;
    for(const int width:{280,560,340}) {
        p.panel.resize(width,640);settle();
        for(auto* grid:{p.defaults,p.custom}) {
            grid->doItemsLayout();settle();
            CHECK(grid->count()==50 && grid->gridSize()==QSize(28,28));
            CHECK(grid->horizontalScrollBar()->maximum()==0);
            CHECK(grid->verticalScrollBar()->maximum()==0);
            for(int i=0;i<grid->count();++i)
                CHECK(grid->viewport()->rect().contains(grid->visualItemRect(grid->item(i))));
        }
        CHECK(p.defaults->height()==p.custom->height());
        if(width==280)smallHeight=p.defaults->height();
        if(width==560)CHECK(p.defaults->height()<smallHeight);
    }
    if(const auto path=qEnvironmentVariable("IMAGEEDITOR_SWATCH_REVIEW");!path.isEmpty())
        CHECK(p.panel.grab().save(path));
    p.panel.resize(340,40);settle();
    CHECK(p.panel.height()==40);
    CHECK(!p.panel.findChild<QWidget*>("ColorPanelColors"));
    CHECK(p.panel.findChild<QScrollArea*>()->verticalScrollBar()->maximum()>0);
    CHECK(ui::UiLayoutConfig{}.color.minimum==120);
    CHECK(ui::UiLayoutConfig{}.color.maximum==QWIDGETSIZE_MAX);
    CHECK(ui::UiLayoutConfig{}.swatches.minimum==120);
}
void colorPickerAndPanel()
{
    ui::ColorPanel panel;
    panel.resize(360, 490); panel.show(); settle();
    CHECK(!panel.findChild<QListWidget*>());
    auto* picker = dynamic_cast<ui::ColorPicker*>(panel.findChild<QWidget*>("ColorPicker"));
    CHECK(picker);
    if (!picker) return;
    int changes = 0;
    panel.onColorsChanged = [&](auto) { ++changes; };
    const core::EditorColors colors {{10, 20, 30, 255}, {30, 140, 200, 91}, core::ColorSlot::Secondary};
    panel.setColors(colors);
    CHECK(changes == 0 && picker->color().rgba() == QColor(30, 140, 200, 91).rgba());
    CHECK(picker->findChild<QWidget*>("ColorManualInputs")->isHidden());
    for (auto* spin : picker->findChildren<QSpinBox*>()) CHECK(!spin->isVisible());
    CHECK(!picker->findChild<QLineEdit*>("ColorHex")->isVisible());
    auto* field = picker->findChild<QWidget*>("ColorSaturationValue");
    QTest::mouseClick(field, Qt::LeftButton, {}, {5, 5});
    CHECK(changes == 1 && panel.colors().primary == colors.primary);
    CHECK(panel.colors().secondary == core::Rgba8(255, 255, 255, 91));
    panel.findChild<QToolButton*>("PanelSwapColors")->click();
    CHECK(panel.colors().active == core::ColorSlot::Primary);
    CHECK(picker->color().rgba() == QColor(10, 20, 30).rgba());
    auto* plane = picker->findChild<QWidget*>("ColorSaturationValue");
    auto* hue = picker->findChild<QWidget*>("ColorHueStrip");
    auto* scroll = panel.findChild<QScrollArea*>(); scroll->ensureWidgetVisible(plane); settle();
    panel.setForegroundColor({0, 0, 255, 91});
    // Full saturation/brightness preserves the chosen hue and independent alpha.
    QTest::mouseClick(plane, Qt::LeftButton, {}, {plane->width() - 5, 5});
    CHECK(panel.foregroundColor() == core::Rgba8(0, 0, 255, 91));
    QTest::mouseClick(plane, Qt::LeftButton, {}, {5, 5});
    CHECK(panel.foregroundColor() == core::Rgba8(255, 255, 255, 91));
    const auto white = panel.foregroundColor();
    QTest::mousePress(plane, Qt::LeftButton, {}, plane->rect().center());
    CHECK(panel.foregroundColor() != white);
    QTest::keyClick(plane, Qt::Key_Escape);
    CHECK(panel.foregroundColor() == white);
    QTest::mouseRelease(plane, Qt::LeftButton, {}, plane->rect().center());
    CHECK(panel.foregroundColor() == white);
    QTest::mousePress(plane, Qt::LeftButton, {}, {plane->width() - 5, plane->height() - 5});
    QEvent cancel(QEvent::TouchCancel); QApplication::sendEvent(plane, &cancel);
    QTest::mouseRelease(plane, Qt::LeftButton);
    CHECK(panel.foregroundColor() == white);
    panel.setForegroundColor({0, 0, 0, 91});
    QTest::mouseClick(hue, Qt::LeftButton, {}, {hue->width() / 2, 5 + (hue->height() - 10) / 3});
    CHECK(panel.foregroundColor() == core::Rgba8(0, 0, 0, 91));
    QTest::mouseClick(plane, Qt::LeftButton, {}, {plane->width() - 5, 5});
    CHECK(panel.foregroundColor().blue == 255 && panel.foregroundColor().red < 10 && panel.foregroundColor().green < 10);
    panel.setForegroundColor({38, 137, 180, 255});
    scroll->verticalScrollBar()->setValue(0); settle();
    if (const auto path = qEnvironmentVariable("IMAGEEDITOR_COLOR_PANEL_REVIEW"); !path.isEmpty()) CHECK(panel.grab().save(path));
    panel.resize(228, 120); settle();
    CHECK(panel.width() == 228 && panel.height() == 120);
    CHECK(scroll->verticalScrollBar()->maximum() > 0);
}
void modernDialogContract()
{
    ui::ColorDialog dialog(QColor(10, 20, 30, 91));
    dialog.setOptions(ui::ColorDialog::ShowAlphaChannel | ui::ColorDialog::DontUseNativeDialog);
    dialog.show(); settle();
    CHECK(dialog.currentColor() == QColor(10, 20, 30, 91));
    CHECK(!dialog.findChild<QListWidget*>());
    auto* button = dialog.findChild<QPushButton*>("PickScreenColor");
    CHECK(button);
    CHECK(button->text().isEmpty());
    CHECK(!button->icon().isNull());
    CHECK(button->width() == 30 && button->height() == 28);
    CHECK(dialog.findChild<QWidget*>("ColorSaturationValue") && dialog.findChild<QWidget*>("ColorHueStrip"));
    CHECK(dialog.minimumSize() == dialog.maximumSize());
    CHECK(dialog.findChild<QWidget*>("ColorManualInputs")->isVisible());
    const auto fixed = dialog.size(); dialog.resize(900, 900); CHECK(dialog.size() == fixed);
    auto* hex = dialog.findChild<QLineEdit*>("ColorHex");
    hex->setFocus(); hex->setText("#1234567B");
    QMetaObject::invokeMethod(hex, "editingFinished");
    CHECK(dialog.currentColor() == QColor(0x12, 0x34, 0x56, 0x7B));
    hex->setText("#badvalue"); QMetaObject::invokeMethod(hex, "editingFinished");
    CHECK(hex->text() == "#1234567B");
    int changed = 0, selected = 0;
    QObject::connect(&dialog, &ui::ColorDialog::currentColorChanged, [&] { ++changed; });
    QObject::connect(&dialog, &ui::ColorDialog::colorSelected, [&](QColor c) { ++selected; CHECK(c == QColor(90, 80, 70, 60)); });
    dialog.setCurrentColor(QColor(90, 80, 70, 60));
    CHECK(changed == 1 && selected == 0);
    dialog.reject(); CHECK(!dialog.selectedColor().isValid() && selected == 0);
    dialog.show(); settle(); dialog.accept();
    CHECK(selected == 1 && dialog.selectedColor() == QColor(90, 80, 70, 60));
    ui::ColorDialog opaque(QColor(10, 20, 30, 91));
    CHECK(opaque.currentColor().alpha() == 255);
    opaque.setCurrentColor(QColor(90, 80, 70, 60)); CHECK(opaque.currentColor().alpha() == 255);
}
void nativeScreenPick()
{
    CHECK(QGuiApplication::platformName() == "xcb");
    if (QGuiApplication::platformName() != "xcb") return;
    if (qEnvironmentVariable("XDG_SESSION_TYPE") == "wayland") {
        std::cout << "Direct screen capture requires an X11 session; XWayland uses the desktop portal.\n";
        return;
    }
    QWidget sample;
    sample.setStyleSheet("background-color: #3478C1;");
    sample.setGeometry(50, 50, 140, 140); sample.show();
    ui::ColorDialog dialog(QColor(10, 20, 30, 91));
    dialog.setOption(ui::ColorDialog::ShowAlphaChannel);
    dialog.move(400, 80); dialog.show();
    QTest::qWait(250);
    auto* button = dialog.findChild<QPushButton*>("PickScreenColor");
    CHECK(button && button->isEnabled());
    button->click(); settle();
    CHECK(!button->isEnabled() && QWidget::mouseGrabber() == &dialog);
    const QPointF global = sample.mapToGlobal(sample.rect().center());
    const auto local = dialog.mapFromGlobal(global);
    QMouseEvent release(QEvent::MouseButtonRelease, local, local, global, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&dialog, &release); settle();
    if (dialog.currentColor() != QColor(0x34, 0x78, 0xC1, 91))
        std::cerr << "Screen sample=" << dialog.currentColor().name(QColor::HexArgb).toStdString()
                  << "; expected widget=" << sample.grab().toImage().pixelColor(70, 70).name().toStdString()
                  << "; global=" << global.x() << ',' << global.y()
                  << "; dialog=" << dialog.x() << ',' << dialog.y() << '\n';
    CHECK(dialog.currentColor() == QColor(0x34, 0x78, 0xC1, 91));
    CHECK(button->isEnabled() && !QWidget::mouseGrabber() && !QWidget::keyboardGrabber());
    button->click(); QTest::keyClick(&dialog, Qt::Key_Escape); settle();
    CHECK(dialog.isVisible() && button->isEnabled());
    CHECK(dialog.currentColor() == QColor(0x34, 0x78, 0xC1, 91));
    button->click(); dialog.reject(); settle();
    CHECK(!QWidget::mouseGrabber() && !QWidget::keyboardGrabber());
    dialog.show(); settle(); CHECK(button->isEnabled()); dialog.close();
}
} // namespace
int main(int argc,char** argv)
{
    QApplication app(argc,argv);
    QCoreApplication::setOrganizationName("VulkanaTests");
    QCoreApplication::setApplicationName("ColorPanelTests");
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings.path());
    ui::applyEditorTheme(app);
    if (app.arguments().contains("--screen-pick-native")) {
        nativeScreenPick();
        if (!failures) std::cout << "Native XCB screen picker: sample, alpha, Escape and close/reopen passed\n";
        return failures ? 1 : 0;
    }
    defaultsAndWorkingColors();customLifecycle();gridsReflowAndShortPanelsScroll();
    colorPickerAndPanel();modernDialogContract();
    if(!failures)std::cout<<"Color panel: responsive grids, working colors, custom picker/persistence passed\n";
    return failures?1:0;
}
