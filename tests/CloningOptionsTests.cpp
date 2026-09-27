#include "imageeditor/ui/CloningOptionsPage.hpp"
#include "imageeditor/ui/BrushOptionsPage.hpp"
#include "imageeditor/ui/CompactValueControl.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/ui/ToolIcons.hpp"
#include "imageeditor/ui/ToolOptionsBar.hpp"

#include <QApplication>
#include <QComboBox>
#include <QImage>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QMainWindow>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QPointer>
#include <QScrollArea>
#include <QScrollBar>
#include <QTest>
#include <QToolButton>
#include <QWheelEvent>

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {
namespace c = imageeditor::core;
namespace u = imageeditor::ui;
int failures = 0;
#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "FAIL line " << __LINE__ << ": " #condition "\n"; ++failures; } } while (false)

void settle()
{
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
}

void toolbarDisconnectsBeforeBaseTeardown()
{
    class ReleaseObserver final : public QObject {
    public:
        QPointer<QScrollBar> scroll;
        bool sawRelease {};
        bool eventFilter(QObject* object, QEvent* event) override
        {
            if (event->type()==QEvent::ParentChange && !object->parent() && scroll) {
                sawRelease=true;
                // QWidgetAction reparenting can change style/scroll ranges in
                // QToolBar's base destructor, after the derived maps are gone.
                scroll->setRange(0,scroll->maximum()+17);
            }
            return false;
        }
    } observer;
    QMainWindow window;
    auto* bar=new u::ToolOptionsBar(&window);window.addToolBar(bar);
    auto* page=new QWidget;page->setMinimumWidth(2000);
    auto* leading=new QWidget;leading->setFixedSize(90,28);
    bar->registerToolPage(c::ToolId::Cloning,QStringLiteral("Cloning"),page);
    bar->registerToolLeadingWidget(c::ToolId::Cloning,leading);
    bar->setActiveTool(c::ToolId::Cloning);
    window.resize(1100,160);window.show();settle();
    auto* root=bar->findChild<QWidget*>("ToolOptionsRoot");
    auto* viewport=bar->findChild<QScrollArea*>("ToolOptionsViewport");
    CHECK(root&&viewport);if(!root||!viewport)return;
    observer.scroll=viewport->horizontalScrollBar();
    root->installEventFilter(&observer);
    delete bar;
    CHECK(observer.sawRelease);
    settle();
}

template<typename Widget>
Widget* find(QWidget& parent, const char* name)
{
    auto* result = dynamic_cast<Widget*>(parent.findChild<QWidget*>(QString::fromLatin1(name)));
    CHECK(result);
    return result;
}

void cachedModesAndProgrammaticSynchronization()
{
    QMainWindow window;
    auto* bar = new u::ToolOptionsBar(&window);
    window.addToolBar(bar);
    auto* page = new u::CloningOptionsPage(bar);
    // This exercises the generic cached page/leading host without requiring a
    // live document, renderer, or MainWindow's full cloning integration.
    bar->registerToolPage(c::ToolId::Brush, QStringLiteral("Cloning"), page);
    bar->registerToolLeadingWidget(c::ToolId::Brush, page->modeWidget());
    bar->setActiveTool(c::ToolId::Brush);
    window.resize(1900, 160);
    window.show();
    settle();
    auto* stamp = find<QToolButton>(*page->modeWidget(), "CloneModeStamp");
    auto* heal = find<QToolButton>(*page->modeWidget(), "CloneModeHeal");
    auto* adaptation = find<u::CompactValueControl>(*page, "CloneAdaptationControl");
    auto* source = find<QToolButton>(*page, "CloneSourceLayer");
    auto* below = find<QToolButton>(*page, "CloneSourceCurrentBelow");
    auto* all = find<QToolButton>(*page, "CloneSourceAllVisible");
    auto* sourceModes = find<QWidget>(*page, "CloneSourceModes");
    auto* size = find<u::CompactValueControl>(*page, "CloneSizeControl");
    auto* aligned = find<QToolButton>(*page, "CloneAlignedButton");
    if (!stamp || !heal || !adaptation || !source || !below || !all || !sourceModes || !size || !aligned) return;
    CHECK(stamp->isChecked() && !heal->isChecked());
    CHECK(adaptation->isVisible() && !adaptation->isEnabled());
    CHECK(sourceModes->accessibleDescription() == QStringLiteral("Alt+click to set source"));
    CHECK(source->isChecked() && !below->isChecked() && !all->isChecked());
    CHECK(source->accessibleName() == QStringLiteral("Source Layer (Raw)"));
    CHECK(below->accessibleName() == QStringLiteral("Current & Below"));
    CHECK(all->accessibleName() == QStringLiteral("All Visible"));
    CHECK(source->toolTip().contains(QStringLiteral("before its adjustments and opacity")));
    CHECK(below->toolTip().contains(QStringLiteral("content beneath it")));
    CHECK(all->toolTip().contains(QStringLiteral("Rendered visible document content")));
    CHECK(sourceModes->geometry().right() < size->geometry().left());
    CHECK(adaptation->geometry().right() < aligned->geometry().left());
    CHECK(aligned->parentWidget()->layout()->itemAt(aligned->parentWidget()->layout()->count() - 2)->widget() == aligned);
    CHECK(!page->findChild<QComboBox*>(QStringLiteral("CloneSourceCombo")));
    CHECK(!page->findChild<QLabel*>(QStringLiteral("CloneSourceLayerLabel")));
    for (auto* button : {source, below, all, aligned}) {
        CHECK(button->property("toolOptionsButton").toBool());
        CHECK(button->toolButtonStyle() == Qt::ToolButtonIconOnly);
        CHECK(button->size() == QSize(28, 28));
        CHECK(!button->icon().isNull());
        CHECK(button->focusPolicy() == Qt::NoFocus);
    }
    int callbacks = 0;
    page->onBrushSettingsChanged = [&](const auto&) { ++callbacks; };
    page->onModeChanged = [&](auto) { ++callbacks; };
    page->onSourceChanged = [&](auto) { ++callbacks; };
    page->onAlignedChanged = [&](auto) { ++callbacks; };
    page->onAdaptationChanged = [&](auto) { ++callbacks; };
    page->setSourceLayerLabel(QStringLiteral("Original photograph with a long stable layer name"));
    const auto identity = sourceModes->accessibleDescription();
    const auto originalSize = page->sizeHint();
    for (int i = 0; i < 6; ++i) {
        page->setMode(i % 2 ? c::CloneMode::Stamp : c::CloneMode::Heal);
        page->setSource(c::CloneSampleSource::AllVisible);
        page->setAligned(false);
        page->setAdaptation(.45);
        page->setBrushSettings(c::BrushSettings {});
        settle();
        CHECK(page->modeWidget() == bar->leadingWidgetForTool(c::ToolId::Brush));
        CHECK(bar->pageForTool(c::ToolId::Brush) == page);
        CHECK(page->sizeHint() == originalSize);
        CHECK(adaptation->isVisible() && adaptation->isEnabled() == (i % 2 == 0));
        CHECK(sourceModes->accessibleDescription() == identity);
        CHECK(all->isChecked() && !source->isChecked() && !below->isChecked());
    }
    CHECK(callbacks == 0);
    CHECK(!aligned->isChecked() && adaptation->value() == 45);
    CHECK(page->cloneSettings().source == c::CloneSampleSource::AllVisible);
    CHECK(page->cloneSettings().adaptation == .45);
    CHECK(source->toolTip().contains(QStringLiteral("long stable layer name")));
    CHECK(!page->isAncestorOf(page->modeWidget()));
    window.close();
}

void userEditsPublishOnceAndPreserveSharedBrushSettings()
{
    u::CloningOptionsPage page;
    // MainWindow normally reparents this into the bar's leading widget stack.
    page.modeWidget()->hide();
    page.resize(page.sizeHint());
    page.show();
    auto* size = find<u::CompactValueControl>(page, "CloneSizeControl");
    auto* hardness = find<u::CompactValueControl>(page, "CloneHardnessControl");
    auto* source = find<QToolButton>(page, "CloneSourceCurrentBelow");
    auto* all = find<QToolButton>(page, "CloneSourceAllVisible");
    auto* aligned = find<QToolButton>(page, "CloneAlignedButton");
    auto* adaptation = find<u::CompactValueControl>(page, "CloneAdaptationControl");
    auto* heal = find<QToolButton>(*page.modeWidget(), "CloneModeHeal");
    if (!size || !hardness || !source || !all || !aligned || !adaptation || !heal) return;
    c::BrushSettings settings;
    settings.sizePixels = 52.75;
    settings.hardness = .834;
    settings.opacity = .913;
    settings.flow = .567;
    settings.spacingPercent = 12.25;
    settings.tip.angleDegrees = 38;
    settings.tip.aspectRatio = .41;
    settings.pressureToFlow = false;
    settings.deterministicSeed = 981724;
    page.setBrushSettings(settings);
    int brushCalls = 0, modeCalls = 0, sourceCalls = 0, alignedCalls = 0, adaptationCalls = 0;
    page.onBrushSettingsChanged = [&](const c::BrushSettings& value) {
        ++brushCalls;
        page.setBrushSettings(value); // The application synchronizes shared settings immediately.
    };
    page.onModeChanged = [&](c::CloneMode value) { ++modeCalls; page.setMode(value); };
    page.onSourceChanged = [&](c::CloneSampleSource value) { ++sourceCalls; page.setSource(value); };
    page.onAlignedChanged = [&](bool value) { ++alignedCalls; page.setAligned(value); };
    page.onAdaptationChanged = [&](double value) { ++adaptationCalls; page.setAdaptation(value); };
    size->setValue(64.5);
    settings.sizePixels = 64.5;
    CHECK(brushCalls == 1 && page.brushSettings() == settings);
    hardness->setValue(60);
    settings.hardness = .6;
    CHECK(brushCalls == 2 && page.brushSettings() == settings);
    CHECK(page.cloneSettings().adaptation == .8);
    heal->click();
    heal->click();
    CHECK(modeCalls == 1 && page.cloneSettings().mode == c::CloneMode::Heal);
    CHECK(adaptation->isEnabled());
    aligned->click();
    CHECK(alignedCalls == 1 && !page.cloneSettings().aligned);
    source->click();
    source->click(); // Re-selecting the active source is not a settings change.
    CHECK(sourceCalls == 1 && page.cloneSettings().source == c::CloneSampleSource::CurrentAndBelow);
    adaptation->setValue(37);
    CHECK(adaptationCalls == 1 && page.cloneSettings().adaptation == .37);
    CHECK(page.brushSettings().hardness == .6);

    size->setFocus();
    settle();
    auto* editor = size->findChild<QLineEdit*>();
    CHECK(editor);
    QTest::keyClick(size, Qt::Key_6);
    CHECK(size->isManualEntryActive() && size->value() == 6);
    QTest::keyClick(size, Qt::Key_Period);
    if (editor) CHECK(editor->text() == QStringLiteral("Size: 6. px"));
    QTest::keyClick(size, Qt::Key_5);
    CHECK(size->value() == 6.5 && page.brushSettings().sizePixels == 6.5);
    if (editor) CHECK(editor->text() == QStringLiteral("Size: 6.5 px"));
    CHECK(brushCalls == 4);
    QTest::keyClick(size, Qt::Key_Return);
    CHECK(!size->isManualEntryActive());

    for (const auto angle : {QPoint(0, 120), QPoint(0, -120), QPoint(120, 0)}) {
        const auto center = source->rect().center();
        QWheelEvent wheel(center, source->mapToGlobal(center), {}, angle,
            Qt::NoButton, Qt::NoModifier, Qt::ScrollUpdate, false);
        QCoreApplication::sendEvent(source, &wheel);
    }
    CHECK(sourceCalls == 1 && source->isChecked() && !all->isChecked());
    all->click();
    CHECK(sourceCalls == 2 && !source->isChecked() && all->isChecked());
    CHECK(page.cloneSettings().source == c::CloneSampleSource::AllVisible);
    page.close();
}

void spotHealProcessingStaysCompactAndCentered()
{
    QMainWindow window;
    auto* bar = new u::ToolOptionsBar(&window);
    window.addToolBar(bar);
    auto* page = new u::CloningOptionsPage(bar);
    bar->registerToolPage(c::ToolId::Cloning, QStringLiteral("Cloning"), page);
    bar->registerToolLeadingWidget(c::ToolId::Cloning, page->modeWidget());
    bar->setActiveTool(c::ToolId::Cloning);
    page->setMode(c::CloneMode::SpotHeal);
    auto* row = find<QWidget>(*page, "SpotHealProgressRow");
    auto* label = find<QLabel>(*page, "SpotHealProgressLabel");
    auto* cancel = find<QPushButton>(*page, "SpotHealCancelProcessing");
    auto* size = find<u::CompactValueControl>(*page, "CloneSizeControl");
    auto* progress = page->findChild<QProgressBar*>();
    CHECK(progress);
    if (!row || !label || !cancel || !size || !progress) return;
    int cancelCalls = 0;
    page->onCancelProcessing = [&] { ++cancelCalls; };
    window.show();
    for (const int width : {1900, 2500}) {
        window.resize(width, 160);
        page->setProcessing(true, .5, QStringLiteral("Reconstructing Spot Heal…"));
        settle();
        CHECK(row->isVisible() && !size->isVisible());
        CHECK(!page->modeWidget()->isEnabled());
        CHECK(label->text() == QStringLiteral("Reconstructing Spot Heal…"));
        CHECK(progress->value() == 500 && progress->width() == 180);
        CHECK(label->width() == label->sizeHint().width());
        CHECK(cancel->width() == cancel->sizeHint().width());
        CHECK(progress->geometry().left() - label->geometry().right() - 1 == 8);
        CHECK(cancel->geometry().left() - progress->geometry().right() - 1 == 8);
        const QRect group(label->mapTo(page, QPoint {}),
            cancel->mapTo(page, cancel->rect().bottomRight()));
        CHECK(page->width() > group.width() + 400);
        CHECK(std::abs(group.center().x() - page->rect().center().x()) <= 1);
        const auto centerInBar = page->mapTo(bar, group.center());
        CHECK(std::abs(centerInBar.x() - bar->rect().center().x()) <= 2);

        const int previousCalls = cancelCalls;
        QTest::mouseClick(cancel, Qt::LeftButton);
        CHECK(cancelCalls == previousCalls + 1);
        page->setProcessing(false);
        settle();
        CHECK(!row->isVisible() && size->isVisible());
        CHECK(page->modeWidget()->isEnabled());
        CHECK(page->cloneSettings().mode == c::CloneMode::SpotHeal);
    }
    window.close();
}

void brushDirectionUsesTheSharedIconToggle()
{
    u::BrushOptionsPage page;
    auto* direction = find<QToolButton>(page, "BrushDirectionButton");
    auto* angle = find<u::CompactValueControl>(page, "BrushAngleControl");
    if (!direction || !angle) return;
    auto* controlsLayout = direction->parentWidget()->layout();
    CHECK(controlsLayout && controlsLayout->itemAt(controlsLayout->count() - 1)->widget() == direction);
    CHECK(controlsLayout && controlsLayout->itemAt(controlsLayout->count() - 2)->widget() == angle);
    CHECK(direction->property("toolOptionsButton").toBool());
    CHECK(direction->isCheckable() && direction->isChecked());
    CHECK(direction->toolButtonStyle() == Qt::ToolButtonIconOnly);
    CHECK(direction->size() == QSize(28, 28));
    CHECK(!direction->icon().isNull());
    CHECK(direction->accessibleName() == QStringLiteral("Follow stroke direction"));
    CHECK(direction->toolTip().contains(QStringLiteral("Off: keep a fixed Angle")));
    CHECK(direction->focusPolicy() == Qt::NoFocus);
    int callbacks = 0;
    page.onBrushSettingsChanged = [&](const c::BrushSettings& settings) {
        ++callbacks;
        CHECK(settings.tip.rotationMode == (direction->isChecked()
            ? c::BrushTipRotationMode::FollowStrokeDirection : c::BrushTipRotationMode::Fixed));
        page.setBrushSettings(settings);
    };
    direction->click();
    CHECK(!direction->isChecked() && callbacks == 1);
    direction->click();
    CHECK(direction->isChecked() && callbacks == 2);
}

bool containsInk(const QImage& image, const QColor& ink)
{
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x) {
            const auto pixel = image.pixelColor(x, y);
            if (pixel.alpha() >= 200 && std::abs(pixel.red() - ink.red()) <= 2
                && std::abs(pixel.green() - ink.green()) <= 2
                && std::abs(pixel.blue() - ink.blue()) <= 2) return true;
        }
    return false;
}

void cloneIconsRenderDistinctlyAtToolbarSizes(QApplication& application)
{
    constexpr std::array glyphs {u::ToolGlyph::Cloning, u::ToolGlyph::CloneStamp,
        u::ToolGlyph::CloneHeal, u::ToolGlyph::CloneSource, u::ToolGlyph::ActiveLayer,
        u::ToolGlyph::CurrentAndBelow, u::ToolGlyph::MergedVisible,
        u::ToolGlyph::CloneAligned, u::ToolGlyph::FollowStrokeDirection};
    const auto originalPalette = application.palette();
    const QColor ink("#43B8CD");
    std::array<QImage, glyphs.size()> thumbnails;
    for (std::size_t i = 0; i < glyphs.size(); ++i) {
        const auto fixed = u::toolGlyph(glyphs[i], ink);
        const auto paletteIcon = u::toolGlyph(glyphs[i]);
        for (const int size : {18, 24, 32})
            for (const auto dpr : {1.0, 1.25, 1.5, 2.0, 3.0}) {
                const auto pixmap = fixed.pixmap(QSize(size, size), dpr);
                CHECK(pixmap.size() == QSize(qRound(size * dpr), qRound(size * dpr)));
                CHECK(std::abs(pixmap.devicePixelRatioF() - dpr) < 1e-8);
                CHECK(containsInk(pixmap.toImage(), ink));
                CHECK(fixed.pixmap(QSize(size, size), dpr, QIcon::Selected).toImage() == pixmap.toImage());
            }
        thumbnails[i] = fixed.pixmap(QSize(24, 24), 1).toImage();
        for (const QColor tone : {QColor("#CD8543"), QColor("#599BC8")}) {
            auto palette = originalPalette;
            palette.setColor(QPalette::Active, QPalette::Text, tone);
            application.setPalette(palette);
            CHECK(containsInk(paletteIcon.pixmap(QSize(32, 32), 1.25).toImage(), tone));
            CHECK(containsInk(fixed.pixmap(QSize(32, 32), 1.25).toImage(), ink));
        }
    }
    for (std::size_t i = 0; i < thumbnails.size(); ++i)
        for (std::size_t j = i + 1; j < thumbnails.size(); ++j)
            CHECK(thumbnails[i] != thumbnails[j]);
    application.setPalette(originalPalette);
    const auto aligned = u::toolGlyph(u::ToolGlyph::CloneAligned, ink);
    CHECK(aligned.pixmap(QSize(24, 24), 1, QIcon::Normal, QIcon::On).toImage()
        != aligned.pixmap(QSize(24, 24), 1, QIcon::Normal, QIcon::Off).toImage());

    const auto reviewPath = qEnvironmentVariable("IMAGEEDITOR_CLONE_ICON_REVIEW_PATH");
    if (!reviewPath.isEmpty()) {
        QImage sheet(static_cast<int>(glyphs.size()) * 140, 220, QImage::Format_ARGB32_Premultiplied);
        sheet.fill(application.palette().color(QPalette::Window));
        QPainter painter(&sheet);
        painter.setPen(application.palette().color(QPalette::Text));
        constexpr std::array names {"Cloning", "Stamp", "Heal", "Source", "Raw layer",
            "Current & Below", "All Visible", "Aligned", "Direction"};
        for (std::size_t i = 0; i < glyphs.size(); ++i) {
            const auto x = 10 + static_cast<int>(i) * 140;
            painter.drawText(QRect(x, 10, 130, 25), Qt::AlignCenter, QString::fromLatin1(names[i]));
            u::toolGlyph(glyphs[i]).paint(&painter, QRect(x + 28, 45, 74, 74));
            u::toolGlyph(glyphs[i]).paint(&painter, QRect(x + 31, 155, 18, 18));
            u::toolGlyph(glyphs[i]).paint(&painter, QRect(x + 75, 152, 24, 24));
        }
        painter.end();
        CHECK(sheet.save(reviewPath));
    }
}

} // namespace

int main(int argc, char** argv)
{
    QApplication application(argc, argv);
    u::applyEditorTheme(application);
    toolbarDisconnectsBeforeBaseTeardown();
    cachedModesAndProgrammaticSynchronization();
    userEditsPublishOnceAndPreserveSharedBrushSettings();
    spotHealProcessingStaysCompactAndCentered();
    brushDirectionUsesTheSharedIconToggle();
    cloneIconsRenderDistinctlyAtToolbarSizes(application);
    if (failures) return EXIT_FAILURE;
    std::cout << "Cloning options and icon tests passed\n";
    return EXIT_SUCCESS;
}
