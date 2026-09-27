#include "imageeditor/ui/NewDocumentDialog.hpp"
#include "imageeditor/ui/RecentFiles.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/ui/ToolOptionsNumber.hpp"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <cstdlib>
#include <iostream>

namespace {

int failures = 0;

void check(bool condition, const char* expression, int line)
{
    if (!condition) {
        std::cerr << "FAIL line " << line << ": " << expression << '\n';
        ++failures;
    }
}

#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

void changeModeStartsFromTheCurrentCanvas()
{
    using imageeditor::core::CanvasSpec;
    imageeditor::ui::NewDocumentDialog dialog(
        imageeditor::ui::NewDocumentDialog::Mode::ChangeCanvasSize,
        CanvasSpec {.extent = {731, 419}, .dotsPerInch = 144.0});

    auto* width = dialog.findChild<QDoubleSpinBox*>(QStringLiteral("CanvasWidthSpinBox"));
    auto* height = dialog.findChild<QDoubleSpinBox*>(QStringLiteral("CanvasHeightSpinBox"));
    auto* dpi = dialog.findChild<QDoubleSpinBox*>(QStringLiteral("CanvasDpiSpinBox"));
    const auto* buttons = dialog.findChild<QDialogButtonBox*>(
        QStringLiteral("CanvasDialogButtons"));
    CHECK(width && width->value() == 731);
    CHECK(height && height->value() == 419);
    CHECK(dpi && dpi->value() == 144.0);
    CHECK(buttons && buttons->button(QDialogButtonBox::Ok)->text() == QStringLiteral("Apply"));
    CHECK(dialog.windowTitle() == QStringLiteral("Change canvas size"));
    const auto* presets = dialog.findChild<QComboBox*>(QStringLiteral("CanvasPresetCombo"));
    CHECK(presets && presets->currentText() == QStringLiteral("Custom"));
    CHECK(!dialog.findChild<QComboBox*>(QStringLiteral("CanvasBackgroundCombo")));
    CHECK(!dialog.findChild<QPushButton*>(QStringLiteral("OpenDocumentButton")));
    CHECK(!dialog.findChild<QAction*>(QStringLiteral("OpenImageFromNewAction")));
    CHECK(!dialog.findChild<QAction*>(QStringLiteral("OpenProjectFromNewAction")));
    CHECK(!dialog.findChild<QListWidget*>(QStringLiteral("RecentFilesList")));
    if (!width || !height || !dpi) {
        return;
    }

    width->setValue(1024);
    height->setValue(768);
    dpi->setValue(110.0);
    CHECK(dialog.canvasSpec()
        == CanvasSpec({.extent = {1024, 768}, .dotsPerInch = 110.0}));
}

void createModeKeepsTheExistingDefaults()
{
    imageeditor::ui::NewDocumentDialog dialog;
    CHECK(dialog.canvasSpec()
        == imageeditor::core::CanvasSpec({.extent = {1600, 900}, .dotsPerInch = 96.0}));
    const auto* buttons = dialog.findChild<QDialogButtonBox*>(
        QStringLiteral("CanvasDialogButtons"));
    CHECK(buttons && buttons->button(QDialogButtonBox::Ok)->text() == QStringLiteral("Create"));
    CHECK(dialog.backgroundColor() == imageeditor::core::Rgba8({0, 0, 0, 0}));
    auto* presets = dialog.findChild<QComboBox*>(QStringLiteral("CanvasPresetCombo"));
    CHECK(presets && presets->currentText() == QStringLiteral("Custom"));
}

void presetsFillDimensionsAndManualEditsReturnToCustom()
{
    using imageeditor::core::CanvasSpec;
    using imageeditor::ui::NewDocumentDialog;
    for (const auto mode : {NewDocumentDialog::Mode::CreateDocument,
                           NewDocumentDialog::Mode::ChangeCanvasSize}) {
        NewDocumentDialog dialog(mode, CanvasSpec {{731, 419}, 144});
        auto* presets = dialog.findChild<QComboBox*>(QStringLiteral("CanvasPresetCombo"));
        auto* width = dialog.findChild<QDoubleSpinBox*>(QStringLiteral("CanvasWidthSpinBox"));
        auto* height = dialog.findChild<QDoubleSpinBox*>(QStringLiteral("CanvasHeightSpinBox"));
        auto* dpi = dialog.findChild<QDoubleSpinBox*>(QStringLiteral("CanvasDpiSpinBox"));
        CHECK(presets && width && height && dpi);
        if (!presets || !width || !height || !dpi) return;
        const auto select = [presets](const QString& prefix) {
            const auto index = presets->findText(prefix, Qt::MatchStartsWith);
            CHECK(index > 0);
            if (index > 0) presets->setCurrentIndex(index);
        };
        select(QStringLiteral("Icon"));
        CHECK(dialog.canvasSpec() == CanvasSpec({{16, 16}, 96}));
        select(QStringLiteral("Full HD"));
        CHECK(dialog.canvasSpec().extent == imageeditor::core::Extent2u({1920, 1080}));
        select(QStringLiteral("4K"));
        CHECK(dialog.canvasSpec().extent == imageeditor::core::Extent2u({3840, 2160}));
        select(QStringLiteral("A4"));
        CHECK(dialog.canvasSpec() == CanvasSpec({.extent = {2480, 3508}, .dotsPerInch = 300.0}));
        CHECK(presets->currentIndex() != 0);
        width->setValue(2500);
        CHECK(presets->currentText() == QStringLiteral("Custom"));
        CHECK(height->value() == 3508 && dpi->value() == 300);
        select(QStringLiteral("Icon"));
        height->setValue(257);
        CHECK(presets->currentText() == QStringLiteral("Custom"));
        select(QStringLiteral("A4"));
        dpi->setValue(240);
        CHECK(presets->currentText() == QStringLiteral("Custom"));
        CHECK(width->value() == 2480 && height->value() == 3508);
    }
}

void allRequestedPresetFamiliesHaveTheirExactPixelDimensions()
{
    using imageeditor::core::CanvasSpec;
    imageeditor::ui::NewDocumentDialog dialog;
    auto* presets = dialog.findChild<QComboBox*>(QStringLiteral("CanvasPresetCombo"));
    CHECK(presets);
    if (!presets) return;
    struct Expected { const char* family; unsigned width; unsigned height; double ppi; };
    const Expected expected[] {
        {"Icon", 16, 16, 96}, {"Icon", 32, 32, 96}, {"Icon", 48, 48, 96},
        {"Icon", 64, 64, 96}, {"Icon", 128, 128, 96}, {"Icon", 256, 256, 96},
        {"Texture", 512, 512, 96}, {"Texture", 1024, 1024, 96},
        {"Texture", 2048, 2048, 96}, {"Texture", 4096, 4096, 96},
        {"HD", 1280, 720, 96}, {"Full HD", 1920, 1080, 96}, {"4K", 3840, 2160, 96},
        {"Social", 1080, 1080, 96}, {"Social", 1080, 1920, 96},
        {"A4", 2480, 3508, 300}, {"US Letter", 2550, 3300, 300},
        {"Portrait", 600, 900, 300}, {"Portrait", 1200, 1800, 300},
        {"Portrait", 1500, 2100, 300}, {"Portrait", 2400, 3000, 300},
        {"Landscape", 900, 600, 300}, {"Landscape", 1800, 1200, 300},
        {"Landscape", 2100, 1500, 300}, {"Landscape", 3000, 2400, 300},
    };
    // Check semantic labels and values rather than freezing punctuation or
    // decorative group separators. Pixels remain the editable representation.
    for (const auto& target : expected) {
        bool found = false;
        for (int i = 1; i < presets->count(); ++i) {
            if (!presets->itemText(i).startsWith(QString::fromLatin1(target.family))) continue;
            presets->setCurrentIndex(i);
            if (dialog.canvasSpec() == CanvasSpec({{target.width, target.height}, target.ppi})) {
                using imageeditor::ui::NewDocumentDialog;
                NewDocumentDialog resize(NewDocumentDialog::Mode::ChangeCanvasSize, dialog.canvasSpec());
                const auto* matched = resize.findChild<QComboBox*>(QStringLiteral("CanvasPresetCombo"));
                CHECK(matched && matched->currentText() == presets->itemText(i));
                CHECK(resize.canvasSpec() == dialog.canvasSpec());
                NewDocumentDialog create(NewDocumentDialog::Mode::CreateDocument, dialog.canvasSpec());
                const auto* custom = create.findChild<QComboBox*>(QStringLiteral("CanvasPresetCombo"));
                CHECK(custom && custom->currentText() == QStringLiteral("Custom"));
                found = true;
                break;
            }
        }
        if (!found)
            std::cerr << "Missing preset " << target.family << ' ' << target.width << 'x'
                      << target.height << " at " << target.ppi << " ppi\n";
        CHECK(found);
    }
    // The compact icon family must be immediately discoverable below Custom.
    CHECK(presets->itemText(0) == QStringLiteral("Custom"));
    CHECK(presets->itemText(1).startsWith(QStringLiteral("Icon")));
    presets->setCurrentIndex(1);
    CHECK(dialog.canvasSpec() == CanvasSpec({{16, 16}, 96}));
}

void resizePresetMatchingRequiresBothDimensionsAndResolution()
{
    using imageeditor::core::CanvasSpec;
    using imageeditor::ui::NewDocumentDialog;
    for (const auto spec : {CanvasSpec {{1921, 1080}, 96}, CanvasSpec {{1920, 1081}, 96},
                            CanvasSpec {{1080, 1920}, 300}, CanvasSpec {{1920, 1080}, 300},
                            CanvasSpec {{2480, 3508}, 96}, CanvasSpec {{2480, 3508}, 299.99}}) {
        NewDocumentDialog resize(NewDocumentDialog::Mode::ChangeCanvasSize, spec);
        const auto* presets = resize.findChild<QComboBox*>(QStringLiteral("CanvasPresetCombo"));
        CHECK(presets && presets->currentText() == QStringLiteral("Custom"));
        CHECK(resize.canvasSpec() == spec);
    }
}

void backgroundChoicesHaveExplicitRgbaAndOpenImageHasASeparateResult()
{
    imageeditor::ui::NewDocumentDialog dialog;
    auto* background = dialog.findChild<QComboBox*>(QStringLiteral("CanvasBackgroundCombo"));
    auto* open = dialog.findChild<QPushButton*>(QStringLiteral("OpenDocumentButton"));
    auto* image = dialog.findChild<QAction*>(QStringLiteral("OpenImageFromNewAction"));
    auto* project = dialog.findChild<QAction*>(QStringLiteral("OpenProjectFromNewAction"));
    CHECK(background && open && image && project);
    if (!background || !open || !image || !project) return;
    CHECK(background->count() == 3);
    // Button activation uses an explicit owned popup, not Qt's setMenu path
    // which derives the transient owner from the child overlay plane.
    auto* menu = qobject_cast<QMenu*>(image->parent());
    CHECK(menu && menu->actions().contains(image) && menu->actions().contains(project));
    CHECK(image->isEnabled());
    CHECK(project->isEnabled());
    const auto setBackground = [&dialog, background](const QString& name, imageeditor::core::Rgba8 expected) {
        const auto index = background->findText(name);
        CHECK(index >= 0);
        if (index >= 0) background->setCurrentIndex(index);
        CHECK(dialog.backgroundColor() == expected);
    };
    setBackground(QStringLiteral("White"), {255, 255, 255, 255});
    setBackground(QStringLiteral("Black"), {0, 0, 0, 255});
    setBackground(QStringLiteral("Transparent"), {0, 0, 0, 0});
    image->trigger();
    CHECK(dialog.result() == imageeditor::ui::NewDocumentDialog::OpenImage);
    CHECK(dialog.result() != QDialog::Accepted);
    project->trigger();
    CHECK(dialog.result() == imageeditor::ui::NewDocumentDialog::OpenProject);
    CHECK(dialog.result() != QDialog::Accepted);
}

void recentFilesDisplayRefreshAndActivation()
{
    using namespace imageeditor::ui;
    QTemporaryDir temporary;
    CHECK(temporary.isValid());
    if (!temporary.isValid()) return;
    QSettings settings(temporary.filePath(QStringLiteral("prefs.ini")), QSettings::IniFormat);
    RecentFiles recent(settings);
    NewDocumentDialog dialog;
    dialog.setRecentFiles(&recent);
    auto* list = dialog.findChild<QListWidget*>(QStringLiteral("RecentFilesList"));
    auto* clear = dialog.findChild<QPushButton*>(QStringLiteral("ClearRecentFilesButton"));
    auto* section = dialog.findChild<QWidget*>(QStringLiteral("RecentFilesSection"));
    CHECK(list && clear && section);
    if (!list || !clear || !section) return;
    CHECK(list->count() == 0 && list->isHidden());
    CHECK(!clear->isEnabled());
    CHECK(!section->isHidden());
    CHECK(list->minimumHeight() == list->maximumHeight());
    const auto image = temporary.filePath(QStringLiteral("images/画面.png"));
    const auto project = temporary.filePath(QStringLiteral("projects/画面.ieproj"));
    recent.recordSuccess(image, RecentFileKind::Image);
    recent.recordSuccess(project, RecentFileKind::Project);
    dialog.refreshRecentFiles();
    CHECK(list->count() == 2 && !list->isHidden());
    CHECK(clear->isEnabled());
    CHECK(list->item(0)->text() == QStringLiteral("画面.ieproj"));
    CHECK(list->item(1)->text() == QStringLiteral("画面.png"));
    CHECK(!list->item(0)->text().contains(QFileInfo(project).absolutePath()));
    CHECK(list->item(0)->toolTip().contains(project));
    CHECK(list->item(0)->toolTip().contains(QStringLiteral("Project")));
    CHECK(list->item(1)->toolTip().contains(image));
    CHECK(list->item(1)->toolTip().contains(QStringLiteral("Image")));
    CHECK(list->item(0)->data(Qt::UserRole).toString() == project);
    CHECK(dialog.selectedRecentPath().isEmpty());

    list->setCurrentRow(1);
    recent.recordSuccess(temporary.filePath(QStringLiteral("third.png")), RecentFileKind::Image);
    dialog.refreshRecentFiles();
    CHECK(list->currentItem() && list->currentItem()->data(Qt::UserRole).toString() == image);
    CHECK(list->count() == 3);
    const auto before = recent.entries();
    int finishCount = 0;
    QObject::connect(&dialog, &QDialog::finished, &dialog, [&finishCount] { ++finishCount; });
    dialog.show();
    list->setFocus();
    QTest::keyClick(list, Qt::Key_Return);
    CHECK(dialog.result() == NewDocumentDialog::OpenRecent);
    CHECK(dialog.selectedRecentPath() == image);
    CHECK(finishCount == 1);
    // Platform styles can emit both activated and doubleClicked. Resolve once.
    list->itemDoubleClicked(list->currentItem());
    CHECK(finishCount == 1);
    CHECK(recent.entries() == before); // The caller records only successful opens.
}

void recentFilesRemovalClearAndResizeIsolation()
{
    using namespace imageeditor::ui;
    QTemporaryDir temporary;
    CHECK(temporary.isValid());
    if (!temporary.isValid()) return;
    QSettings settings(temporary.filePath(QStringLiteral("prefs.ini")), QSettings::IniFormat);
    RecentFiles recent(settings);
    const auto image = temporary.filePath(QStringLiteral("still-present.png"));
    QFile imageFile(image);
    CHECK(imageFile.open(QIODevice::WriteOnly));
    imageFile.close();
    recent.recordSuccess(image, RecentFileKind::Image);
    recent.recordSuccess(temporary.filePath(QStringLiteral("missing.ieproj")), RecentFileKind::Project);
    NewDocumentDialog dialog;
    dialog.setRecentFiles(&recent);
    dialog.show();
    QApplication::processEvents();
    auto* list = dialog.findChild<QListWidget*>(QStringLiteral("RecentFilesList"));
    auto* clear = dialog.findChild<QPushButton*>(QStringLiteral("ClearRecentFilesButton"));
    auto* remove = dialog.findChild<QAction*>(QStringLiteral("RemoveRecentFileAction"));
    auto* menu = dialog.findChild<QMenu*>(QStringLiteral("RecentFileContextMenu"));
    CHECK(list && clear && remove && menu);
    if (!list || !clear || !remove || !menu) return;
    list->customContextMenuRequested(list->visualItemRect(list->item(1)).center());
    CHECK(remove->data().toString() == image);
    remove->trigger();
    menu->hide();
    CHECK(recent.entries().size() == 1 && list->count() == 1);
    CHECK(QFileInfo::exists(image));
    CHECK(dialog.result() != NewDocumentDialog::OpenRecent);
    CHECK(dialog.selectedRecentPath().isEmpty());
    clear->click();
    CHECK(recent.entries().isEmpty() && list->count() == 0);
    CHECK(!clear->isEnabled());
    CHECK(list->isHidden());
    CHECK(QFileInfo::exists(image));
    RecentFiles reloaded(settings);
    CHECK(reloaded.entries().isEmpty());
    dialog.setRecentFiles(nullptr);
    CHECK(dialog.findChild<QWidget*>(QStringLiteral("RecentFilesSection"))->isHidden());
    dialog.hide();

    NewDocumentDialog resize(NewDocumentDialog::Mode::ChangeCanvasSize,
        imageeditor::core::CanvasSpec {{320, 240}, 96});
    resize.setRecentFiles(&recent);
    resize.refreshRecentFiles();
    CHECK(!resize.findChild<QWidget*>(QStringLiteral("RecentFilesSection")));
    CHECK(resize.selectedRecentPath().isEmpty());
}

void recentViewportShowsFiveRowsAndCapsTheListAtTen()
{
    using namespace imageeditor::ui;
    QTemporaryDir temporary;
    CHECK(temporary.isValid());
    if (!temporary.isValid()) return;
    QSettings settings(temporary.filePath(QStringLiteral("prefs.ini")), QSettings::IniFormat);
    RecentFiles recent(settings);
    NewDocumentDialog dialog;
    dialog.setRecentFiles(&recent);
    dialog.show();
    QApplication::processEvents();
    auto* list = dialog.findChild<QListWidget*>(QStringLiteral("RecentFilesList"));
    CHECK(list);
    if (!list) return;
    CHECK(list->isHidden());
    CHECK(!list->verticalScrollBar()->isVisible());

    int fiveRowViewportHeight = 0;
    for (int recorded = 1; recorded <= 12; ++recorded) {
        recent.recordSuccess(
            temporary.filePath(QStringLiteral("image-%1.png").arg(recorded)), RecentFileKind::Image);
        dialog.refreshRecentFiles();
        list->doItemsLayout();
        list->scrollToTop();
        QApplication::processEvents();

        const int expectedCount = std::min(recorded, 10);
        const int expectedVisible = std::min(expectedCount, 5);
        CHECK(list->count() == expectedCount);
        CHECK(list->verticalScrollBar()->isVisible() == (expectedCount > 5));
        CHECK((list->verticalScrollBar()->maximum() > 0) == (expectedCount > 5));
        // Measure actual themed item geometry rather than relying on the
        // implementation's font-based row-height calculation.
        int fullyVisible = 0;
        int partlyVisible = 0;
        const auto viewport = list->viewport()->rect();
        for (int row = 0; row < list->count(); ++row) {
            const auto rect = list->visualItemRect(list->item(row));
            if (viewport.contains(rect)) ++fullyVisible;
            else if (viewport.intersects(rect)) ++partlyVisible;
        }
        CHECK(fullyVisible == expectedVisible);
        CHECK(partlyVisible == 0);
        if (expectedCount == 5)
            fiveRowViewportHeight = list->viewport()->height();
        if (expectedCount > 5)
            CHECK(list->viewport()->height() == fiveRowViewportHeight);
        CHECK(dialog.minimumSize() == dialog.maximumSize());
        CHECK(dialog.width() <= 980 && dialog.height() <= 640);
    }
    CHECK(recent.entries().size() == 10);
    CHECK(list->item(0)->data(Qt::UserRole).toString()
        == temporary.filePath(QStringLiteral("image-12.png")));
    CHECK(list->item(9)->data(Qt::UserRole).toString()
        == temporary.filePath(QStringLiteral("image-3.png")));
    list->scrollToBottom();
    QApplication::processEvents();
    CHECK(list->viewport()->rect().contains(list->visualItemRect(list->item(9))));

    const auto fixedSize = dialog.size();
    dialog.resize(fixedSize + QSize(100, 100));
    CHECK(dialog.size() == fixedSize);
    dialog.hide();

    NewDocumentDialog resize(NewDocumentDialog::Mode::ChangeCanvasSize,
        imageeditor::core::CanvasSpec {{320, 240}, 96});
    resize.show();
    QApplication::processEvents();
    CHECK(resize.minimumSize() == resize.maximumSize());
    CHECK(resize.width() <= 980 && resize.height() <= 640);
    const auto fixedResizeSize = resize.size();
    resize.resize(fixedResizeSize + QSize(100, 100));
    CHECK(resize.size() == fixedResizeSize);
}

void dimensionsUseTheSharedNumericControlAndKeepMemoryLimits()
{
    imageeditor::ui::NewDocumentDialog dialog;
    auto* width = dialog.findChild<QDoubleSpinBox*>(QStringLiteral("CanvasWidthSpinBox"));
    auto* height = dialog.findChild<QDoubleSpinBox*>(QStringLiteral("CanvasHeightSpinBox"));
    auto* dpi = dialog.findChild<QDoubleSpinBox*>(QStringLiteral("CanvasDpiSpinBox"));
    auto* buttons = dialog.findChild<QDialogButtonBox*>(QStringLiteral("CanvasDialogButtons"));
    CHECK(width && height && dpi && buttons);
    if (!width || !height || !dpi || !buttons) return;
    for (auto* number : {width, height, dpi}) {
        CHECK(dynamic_cast<imageeditor::ui::ToolOptionsNumber*>(number));
        CHECK(number->property("compactValueControl").toBool());
        CHECK(number->singleStep() == 1);
    }
    CHECK(width->decimals() == 0 && height->decimals() == 0);
    CHECK(width->minimum() == 1 && height->minimum() == 1);
    width->setValue(16384);
    height->setValue(16384);
    CHECK(!buttons->button(QDialogButtonBox::Ok)->isEnabled());
    height->setValue(100);
    CHECK(buttons->button(QDialogButtonBox::Ok)->isEnabled());
}

} // namespace

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    imageeditor::ui::applyEditorTheme(application);
    const auto preview = qEnvironmentVariable("IMAGEEDITOR_TEST_NEW_DOCUMENT_PREVIEW");
    if (!preview.isEmpty()) {
        imageeditor::ui::NewDocumentDialog dialog;
        dialog.show();
        QTest::qWait(120);
        return dialog.grab().save(preview) ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    changeModeStartsFromTheCurrentCanvas();
    createModeKeepsTheExistingDefaults();
    presetsFillDimensionsAndManualEditsReturnToCustom();
    allRequestedPresetFamiliesHaveTheirExactPixelDimensions();
    resizePresetMatchingRequiresBothDimensionsAndResolution();
    backgroundChoicesHaveExplicitRgbaAndOpenImageHasASeparateResult();
    recentFilesDisplayRefreshAndActivation();
    recentFilesRemovalClearAndResizeIsolation();
    recentViewportShowsFiveRowsAndCapsTheListAtTen();
    dimensionsUseTheSharedNumericControlAndKeepMemoryLimits();

    if (failures != 0) {
        std::cerr << failures << " canvas-size dialog assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All canvas-size dialog tests passed\n";
    return EXIT_SUCCESS;
}
