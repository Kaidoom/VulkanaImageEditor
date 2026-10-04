#include "imageeditor/ui/ColorPanel.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/ui/UiLayoutConfig.hpp"

#include <QApplication>
#include <QColorDialog>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
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
    ui::ColorPanel panel;
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
    QColorDialog* picker() { return panel.findChild<QColorDialog*>("CustomSwatchColorDialog"); }
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
        CHECK(dialog->windowHandle()->transientParent() == panel.windowHandle());
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
    CHECK(p.panel.findChild<QWidget*>("ColorPanelColors")->height()==46);
    CHECK(p.panel.findChild<QScrollArea*>()->verticalScrollBar()->maximum()>0);
    CHECK(ui::UiLayoutConfig{}.color.minimum==120);
    CHECK(ui::UiLayoutConfig{}.color.maximum==QWIDGETSIZE_MAX);
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
    defaultsAndWorkingColors();customLifecycle();gridsReflowAndShortPanelsScroll();
    if(!failures)std::cout<<"Color panel: responsive grids, working colors, custom picker/persistence passed\n";
    return failures?1:0;
}
