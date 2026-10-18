#include "imageeditor/ui/Localization.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/OverlayDockWorkspace.hpp"
#include "imageeditor/ui/WorkspacePanel.hpp"
#include "imageeditor/ui/PreferencesDialog.hpp"
#include "imageeditor/ui/PerformancePanel.hpp"
#include "imageeditor/ui/ScreenshotDialog.hpp"
#include "imageeditor/ui/ScreenshotPreview.hpp"
#include "imageeditor/ui/ProjectIntegrationDialog.hpp"
#include "imageeditor/ui/CompactValueControl.hpp"
#include "imageeditor/ui/EditorShortcuts.hpp"
#include "imageeditor/ui/BlendModeCombo.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/ui/AboutDialog.hpp"
#include "imageeditor/ui/NewDocumentDialog.hpp"
#include "imageeditor/ui/ColorDialog.hpp"
#include "imageeditor/ui/EffectsPanel.hpp"
#include "imageeditor/ui/GradientStopEditor.hpp"
#include "imageeditor/ui/SwatchesPanel.hpp"
#include "imageeditor/ui/AdjustmentsPanel.hpp"
#include "imageeditor/ui/PsdReviewText.hpp"
#include "imageeditor/ui/PsdExportPanel.hpp"
#include "imageeditor/ui/PsdImportDialog.hpp"
#include "PsdFixture.hpp"
#include <QAbstractItemView>
#include <QAbstractItemDelegate>
#include <QDialogButtonBox>
#include <QDir>
#include <QPushButton>
#include <QStyleOptionViewItem>
#include <QTextLayout>
#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QInputMethodEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenuBar>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QTabWidget>
#include <QTabBar>
#include <QTreeWidget>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QCheckBox>
#include <QElapsedTimer>
#include <QVBoxLayout>
#include <iostream>
namespace u = imageeditor::ui;
namespace i = imageeditor::ui::i18n;
int failures = 0;
#define CHECK(x) do { if (!(x)) { ++failures; std::cerr << __LINE__ << ": " #x "\n"; } } while (false)
void settle() { for (int n = 0; n < 4; ++n) QCoreApplication::processEvents(); }
void localeMatching()
{
    const QList<i::Language> available{{"en", "English"}, {"fr", "Français"}, {"ar", "العربية", true},
        {"pt-BR", "Português (Brasil)"}, {"zh-Hans", "简体中文"}, {"zh-Hant", "繁體中文"}};
    for (const auto& entry : std::initializer_list<std::pair<const char*, const char*>>{
        {"en-GB", "en"}, {"fr_CA", "fr"}, {"ar-EG", "ar"}, {"pt_BR", "pt-BR"}, {"pt", "pt-BR"},
        {"zh-CN", "zh-Hans"}, {"zh-SG", "zh-Hans"}, {"zh-TW", "zh-Hant"}, {"zh-HK", "zh-Hant"},
        {"zh_Hant_TW", "zh-Hant"}, {"zh-Hans-TW", "zh-Hans"}, {"zh-Hant-CN", "zh-Hant"}})
        CHECK(i::matchLanguage({entry.first}, available) == entry.second);
    CHECK(i::matchLanguage({"pt-PT"}, available).isEmpty());
    CHECK(i::matchLanguage({"de-DE", "fr-CA", "en-GB"}, available) == "fr");
    CHECK(i::matchLanguage({"does-not-exist"}, available).isEmpty());
    auto& service = i::Localization::instance();
    CHECK(service.languages().size() == 16);
    CHECK(service.resolveLanguage("zh-TW") == "zh-Hant");
    CHECK(service.resolveLanguage("pt-BR") == "pt-BR");
    CHECK(service.resolveLanguage("hi-IN") == "hi");
    CHECK(service.resolveLanguage("ko-KR") == "ko");
}
void recentUi(const QString& output)
{
    auto& service=i::Localization::instance();service.setLanguage("en");
    u::PreferencesDialog preferences({});auto* panel=preferences.performancePanel();
    const imageeditor::platform::PerformanceSettings settings{128,256,512,2,3,{}};
    panel->setSettings(settings);int changed{};panel->onChanged=[&]{++changed;};
    panel->readUsage=[] {return u::PerformanceUsage{256*1024*1024,1024,2048,4096,8192,"NVIDIA GeForce RTX 4080"};};
    preferences.findChild<QTabWidget*>()->setCurrentWidget(panel);service.direction(&preferences);preferences.show();
    QWidget host;host.resize(780,620);host.show();
    u::ScreenshotDialog screenshot(host,&host);service.direction(&screenshot);screenshot.show();
    auto* preview=dynamic_cast<u::ScreenshotPreview*>(screenshot.findChild<QWidget*>("ScreenshotPreview"));
    QImage image(160,120,QImage::Format_RGBA8888);image.fill(QColor(32,64,96));
    preview->setImage(image);preview->setCrop(QRect(20,10,80,65));screenshot.setCanAddLayer(true);
    const auto crop=preview->crop();auto* target=screenshot.findChild<QComboBox*>("ScreenshotTarget");const auto targetValue=target->currentData();
    u::ProjectIntegrationDialog registration(false,nullptr);service.direction(&registration);registration.show();
    const auto usableButtons=[](QWidget& owner) {
        for(auto* button:owner.findChildren<QPushButton*>())if(button->isVisible()&&!button->text().isEmpty()) {
            if(button->width()<button->minimumSizeHint().width())
                std::cerr<<"Clipped button: "<<button->objectName().toStdString()<<" / "<<button->text().toStdString()<<'\n';
            CHECK(button->width()>=button->minimumSizeHint().width());
        }
    };
    for(const auto* language:{"en","fr","es","de","it","pt-BR","pl","tr","ru","uk","ar","hi","zh-Hans","zh-Hant","ja","ko","qps","en"}) {
        service.setLanguage(language);settle();
        CHECK(panel->settings()==settings&&changed==0);
        CHECK(panel->findChild<QPushButton*>("PerformanceClearCaches")->text()==i::text("performance.clear"));
        if(QString(language)=="ar") {
            CHECK(panel->findChild<QLabel*>("PerformanceUsage")->text().contains(QStringLiteral("\u2066256.0 MiB\u2069")));
            CHECK(panel->findChild<QLabel*>("PerformanceUsage")->text().contains(QStringLiteral("\u2066NVIDIA GeForce RTX 4080\u2069")));
        }
        CHECK(screenshot.findChild<QPushButton*>("ScreenshotOpen")->text()==i::text("screenshot.open"));
        CHECK(screenshot.findChild<QPushButton*>("ScreenshotClose")->accessibleName()==i::text("screenshot.close"));
        CHECK(registration.findChild<QPushButton*>("ProjectIntegrationRegister")->text()==i::text("project.integration.register"));
        CHECK(preview->crop()==crop&&preview->croppedImage()==image.copy(crop));
        CHECK(preview->layoutDirection()==Qt::LeftToRight&&target->currentData()==targetValue);
        CHECK(screenshot.findChild<QWidget*>("ScreenshotDelay")->locale().decimalPoint()==QStringLiteral("."));
        if(QString(language)!="en") {
            CHECK(i::text("performance.clear.hint")!=service.source("performance.clear.hint"));
            CHECK(i::text("screenshot.crop.hint")!=service.source("screenshot.crop.hint"));
            CHECK(i::text("project.integration.description")!=service.source("project.integration.description"));
        }
        usableButtons(preferences);usableButtons(screenshot);usableButtons(registration);
        for(auto* button:screenshot.findChildren<QPushButton*>())if(button->isVisible())
            CHECK(screenshot.rect().contains(QRect(button->mapTo(&screenshot,QPoint{}),button->size())));
        if(!output.isEmpty()) {
            QDir().mkpath(output);
            preferences.grab().save(output+"/performance-"+language+".png");
            host.grab().save(output+"/screenshot-"+language+".png");
            registration.grab().save(output+"/registration-"+language+".png");
        }
    }
}
void adjustmentCategories(const QString& output)
{
    auto& service = i::Localization::instance(); service.setLanguage("en");
    u::AdjustmentsPanel panel;
    auto adjustment = imageeditor::core::Layer::adjustment("User layer اسم");
    auto raster = imageeditor::core::Layer::raster("Pixels", std::make_shared<imageeditor::core::ContiguousRasterSurface>(
        imageeditor::core::Extent2u{2, 2}, imageeditor::core::Rgba8{12, 34, 56, 255}));
    auto* tabs = panel.findChild<QTabWidget*>("AdjustmentTabs"); CHECK(tabs);
    if (!tabs) return;
    panel.resize(440, 650); panel.show();
    panel.setTarget(&raster, false); tabs->setCurrentIndex(4);
    panel.setTarget(&adjustment, false);
    const auto current = tabs->currentIndex();
    CHECK(current >= 0 && current < 3); // Qt may choose the nearest enabled page when disabling the current tab.
    for (const auto* language : {"fr", "ar", "en"}) {
        service.setLanguage(language); settle();
        CHECK(tabs->isTabEnabled(0) && tabs->isTabEnabled(1) && tabs->isTabEnabled(2));
        for (int tab : {3, 4}) {
            CHECK(!tabs->isTabEnabled(tab));
            CHECK(tabs->tabToolTip(tab) == i::text("adjustments.panel.operator.no.filters.effects"));
            QTest::mouseClick(tabs->tabBar(), Qt::LeftButton, {}, tabs->tabBar()->tabRect(tab).center());
            CHECK(tabs->currentIndex() == current);
        }
        if (!output.isEmpty()) panel.grab().save(output + "/adjustment-tabs-" + language + ".png");
    }
    panel.setTarget(&raster, false);
    CHECK(tabs->isTabEnabled(3) && tabs->isTabEnabled(4));
    CHECK(tabs->tabToolTip(3).isEmpty() && tabs->tabToolTip(4).isEmpty());
    QTest::mouseClick(tabs->tabBar(), Qt::LeftButton, {}, tabs->tabBar()->tabRect(4).center());
    CHECK(tabs->currentIndex() == 4);
    panel.setTarget(nullptr, false); CHECK(!tabs->isEnabled());
}
void psdReview(const QString& output)
{
    auto& service = i::Localization::instance(); service.setLanguage("en");
    const QString name = QString::fromUtf8("User اسم text 123");
    u::PsdLayerInfo layer; layer.name = name; layer.editable = true;
    CHECK(i::psdImportStatus(layer) == "Editable");
    layer.issues.append("Technical blend diagnostic");
    CHECK(i::psdImportStatus(layer) == "Editable with differences");
    layer.clippingBase = 2;
    CHECK(i::psdImportStatus(layer).endsWith(" · Clipped"));
    layer.editable = false; layer.raster = true;
    CHECK(i::psdImportStatus(layer).startsWith("Saved raster available"));
    layer.raster = false;
    CHECK(i::psdImportStatus(layer).startsWith("Review required"));
    u::PsdInspection inspection; inspection.size = {80, 60}; inspection.ppi = 144;
    inspection.sourceProfile = "Display P3"; inspection.colorHandling = u::PsdColorHandling::ConvertedToSRgb;
    QLabel summary; i::setText(&summary, i::psdSourceSummary(inspection));
    u::PsdExportOptions options;
    u::PsdExportPlan plan; plan.instanceId = 987; plan.options = options;
    u::PsdExportEntry folder; folder.id = 1; folder.name = "My folder"; folder.container = true; folder.kind = u::PsdContentKind::Group;
    u::PsdExportEntry text; text.id = 2; text.parent = 1; text.name = name; text.kind = u::PsdContentKind::Text;
    text.attention = true;
    plan.entries = {folder, text};
    QWidget host; auto* layout = new QVBoxLayout(&host);
    u::PsdExportPanel panel(options, &host, &host); layout->addWidget(panel.itemsWidget()); layout->addWidget(&panel);
    panel.setPlan(plan);
    auto* tree = host.findChild<QTreeWidget*>("PsdExportItems");
    auto* action = host.findChild<QComboBox*>("PsdExportAction");
    auto* description = host.findChild<QLabel*>("PsdExportActionDescription");
    host.findChild<QCheckBox*>("PsdExportAttention")->setChecked(false);
    host.findChild<QCheckBox*>("PsdExportHideGroups")->setChecked(false);
    tree->setCurrentItem(tree->topLevelItem(0)->child(0));
    action->setCurrentIndex(action->findData(int(u::PsdExportAction::Pixels)));
    const auto choices = options;
    int changed = 0; panel.changed = [&] { ++changed; };
    host.resize(850, 500); host.show(); panel.setPsdVisible(true);
    for (const auto* language : {"fr", "ar", "en", "ar", "en"}) {
        auto* selected = tree->currentItem();
        tree->topLevelItem(0)->setExpanded(false);
        service.setLanguage(language); settle();
        CHECK(!tree->topLevelItem(0)->isExpanded() && tree->currentItem() == selected);
        tree->topLevelItem(0)->setExpanded(true);
        CHECK(summary.text().contains("80 × 60") && summary.text().contains("144") && summary.text().contains("Display P3"));
        CHECK(tree->topLevelItem(0)->text(1) == i::text("psd.review.type.folder"));
        CHECK(tree->currentItem()->text(0) == name);
        CHECK(tree->currentItem()->text(1) == i::text("psd.review.type.text"));
        CHECK(action->currentData().toInt() == int(u::PsdExportAction::Pixels));
        CHECK(description->text() == i::text("psd.export.panel.export.this.layer.s.appearance.as.pixels"));
        CHECK(changed == 0 && options == choices);
        CHECK(layer.issues.front() == "Technical blend diagnostic");
        for (auto problem : {u::PsdExportProblem::EditableUnavailable, u::PsdExportProblem::NeedsGroup,
                u::PsdExportProblem::MissingBase, u::PsdExportProblem::BaseStyles}) {
            plan.problem = problem; plan.problemLayer = name; plan.error = "English low-level diagnostic";
            CHECK(i::psdExportProblem(plan).contains(name));
            CHECK(i::psdExportProblem(plan) != plan.error);
            CHECK(plan.error == "English low-level diagnostic");
        }
        plan.problem = u::PsdExportProblem::None; CHECK(i::psdExportProblem(plan) == plan.error);
        if (!output.isEmpty()) host.grab().save(output + "/psd-export-" + language + ".png");
    }
    // Exercise a real async import and its prepared confirmation, not just helpers.
    QTemporaryDir dir;
    QFile file(dir.filePath("review.psd")); CHECK(file.open(QIODevice::WriteOnly));
    psdfixture::Layer pixels; pixels.name = name;
    file.write(psdfixture::file({pixels})); file.close();
    u::PsdImportDialog dialog(file.fileName(), false, false, {});
    dialog.show();
    auto* button = dialog.findChild<QPushButton*>("PsdImport");
    QElapsedTimer timer; timer.start();
    while (!button->isEnabled() && timer.elapsed() < 10000) QTest::qWait(10);
    CHECK(button->isEnabled());
    dialog.findChild<QCheckBox*>("PsdAttention")->setChecked(false);
    auto* importTree = dialog.findChild<QTreeWidget*>("PsdLayers");
    auto* report = dialog.findChild<QPlainTextEdit*>();
    const auto initialReport = report->toPlainText();
    for (const auto* language : {"ar", "fr", "en"}) {
        service.setLanguage(language); settle();
        CHECK(importTree->topLevelItemCount() == 1);
        if (importTree->topLevelItemCount())
            CHECK(importTree->topLevelItem(0)->text(0) == name + " · " + i::psdContentName(u::PsdContentKind::Raster));
        CHECK(report->toPlainText() == initialReport);
        CHECK(button->isEnabled());
        auto* importAction = dialog.findChild<QComboBox*>("PsdImportAction");
        CHECK(importAction->findData(int(u::PsdRoute::Raster)) == -1); // Never compare translated type names.
        CHECK(importAction->currentData().toInt() == int(u::PsdRoute::Editable));
        if (!output.isEmpty()) dialog.grab().save(output + "/psd-import-" + language + ".png");
    }
    dialog.findChild<QCheckBox*>("PsdImportPreview")->setChecked(false);
    button->click(); timer.restart();
    while (!dialog.findChild<QLabel*>("PsdConfirmationStats") && timer.elapsed() < 10000) QTest::qWait(10);
    auto* stats = dialog.findChild<QLabel*>("PsdConfirmationStats"); CHECK(stats);
    const auto preparedReport = report->toPlainText();
    for (const auto* language : {"ar", "fr", "en"}) {
        service.setLanguage(language); settle();
        if (stats) CHECK(stats->text().endsWith(i::text("psd.import.dialog.new.document.tab")));
        CHECK(report->toPlainText() == preparedReport);
    }
    dialog.reject(); dialog.reject();
}
void bindings()
{
    auto& service = i::Localization::instance();
    CHECK(service.setLanguage("en"));
    CHECK(service.resolveLanguage("fr-CA") == "fr");
    CHECK(service.resolveLanguage("ar-EG") == "ar");
    auto* label = i::make<QLabel>(i::text("language.label"));
    CHECK(label->text() == "Language");
    QComboBox combo;
    i::addItem(&combo, i::text("language.system"), "system");
    i::addItem(&combo, i::text("language.label"), 91);
    combo.setCurrentIndex(1);
    QAction action;
    i::setText(&action, i::text("language.label"));
    i::setToolTip(&action, i::text("language.system"));
    action.setCheckable(true); action.setChecked(true);
    QToolButton actionButton; actionButton.setDefaultAction(&action);
    int actionEdits = 0;
    QObject::connect(&action, &QAction::triggered, [&] { ++actionEdits; });
    QObject::connect(&action, &QAction::toggled, [&] { ++actionEdits; });
    int changeNotifications = 0;
    int modelNotifications = 0;
    QObject::connect(combo.model(), &QAbstractItemModel::dataChanged, [&] { ++modelNotifications; });
    QObject::connect(&combo, &QComboBox::currentIndexChanged, [&] { ++changeNotifications; });
    QObject::connect(&combo, &QComboBox::currentTextChanged, [&] { ++changeNotifications; });
    QWidget form;
    QDoubleSpinBox number(&form); number.setRange(-1000, 1000); number.setDecimals(2); number.setValue(-12.5);
    service.direction(&form);
    const auto code = QString::fromUtf8("ملف-report 123.psd");
    QLabel filename(code); // A document string is not a translation key.
    for (const auto* language : {"fr", "es", "de", "it", "pt-BR", "pl", "tr", "ru", "uk",
             "ar", "hi", "zh-Hans", "zh-Hant", "ja", "ko", "qps", "en", "ar", "fr", "en"}) {
        const auto previousModelNotifications = modelNotifications;
        CHECK(service.setLanguage(language));
        CHECK(i::text("language.label") != "language.label");
        CHECK(QString(language) == "en" || i::text("language.label") != "Language");
        CHECK(label->text() == i::text("language.label").resolved());
        CHECK(combo.currentData().toInt() == 91 && combo.currentIndex() == 1);
        CHECK(combo.itemData(0).toString() == "system");
        CHECK(changeNotifications == 0);
        CHECK(modelNotifications > previousModelNotifications);
        CHECK(actionButton.text() == action.text() && actionButton.toolTip() == action.toolTip());
        CHECK(actionEdits == 0 && action.isChecked());
        CHECK(filename.text() == code);
        CHECK(number.text() == "-12.50" && number.value() == -12.5);
        CHECK(number.layoutDirection() == Qt::LeftToRight);
        CHECK(qApp->layoutDirection() == Qt::LeftToRight);
        CHECK(form.layoutDirection() == (service.isRtl() ? Qt::RightToLeft : Qt::LeftToRight));
        for (int n : {0, 1, 2, 3, 11, 102}) CHECK(!i::text("language.layers.count", n).contains("%n"));
    }
    CHECK(!service.setLanguage("does-not-exist"));
    CHECK(service.language() == "en");
    i::setText(label, i::text("main.window.files.replace.1").arg(code));
    service.setLanguage("fr"); CHECK(label->text().contains(code));
    label->setText("newer unbound status");
    service.setLanguage("ar"); CHECK(label->text() == "newer unbound status");
    delete label;
    service.setLanguage("en"); // Dead bindings cannot retain widgets or be dereferenced.
    combo.clear(); i::addItem(&combo, i::text("language.system"), 123);
    service.setLanguage("ar"); CHECK(combo.count() == 1 && combo.currentData().toInt() == 123);
}
void composition()
{
    auto& service = i::Localization::instance();
    for (const auto& committed : {QString::fromUtf8("中文"), QString::fromUtf8("日本語"), QString::fromUtf8("한국어")}) {
        service.setLanguage("en");
        QLineEdit edit; edit.setText("A😀B"); edit.setCursorPosition(3);
        QInputMethodEvent preedit(committed, {{QInputMethodEvent::Cursor, 1, 1, {}}});
        QCoreApplication::sendEvent(&edit, &preedit);
        service.requestLanguage("ar"); CHECK(service.language() == "en");
        CHECK(edit.text() == "A😀B");
        QInputMethodEvent commit; commit.setCommitString(committed, -2, 2);
        QCoreApplication::sendEvent(&edit, &commit); settle();
        CHECK(service.language() == "ar");
        CHECK(edit.text() == "A" + committed + "B");
        edit.undo(); CHECK(edit.text() == "A😀B");
    }
    service.setLanguage("en");
    bool safe = false; service.canSwitch = [&] { return safe; };
    service.requestLanguage("fr"); CHECK(service.language() == "en");
    service.requestLanguage("en"); // Cancel pending preference preview.
    safe = true; QKeyEvent release(QEvent::KeyRelease, Qt::Key_A, {});
    QCoreApplication::sendEvent(qApp, &release); settle(); CHECK(service.language() == "en");
    service.canSwitch = {};
}
void dropdown(const QString& output)
{
    auto& service = i::Localization::instance(); service.setLanguage("en");
    QComboBox combo; u::populateBlendModeCombo(combo); combo.resize(480, 40);
    combo.setCurrentIndex(combo.count() - 1); combo.show(); settle();
    // Warm the popup in English, then switch without reconstructing its model.
    combo.showPopup(); settle(); combo.hidePopup();
    for (const auto* language : {"ar", "fr", "en", "ar"}) {
        service.setLanguage(language); settle(); combo.showPopup(); settle();
        auto* view = combo.view();
        if (!output.isEmpty()) view->window()->grab().save(output + "/dropdown-before-" + language + ".png");
        for (int row = 0; row < combo.count(); ++row) {
            const auto index = combo.model()->index(row, 0);
            view->scrollTo(index, QAbstractItemView::PositionAtCenter); settle();
            const auto before = view->visualRect(index);
            const int originBefore = view->visualRect(combo.model()->index(0, 0)).top();
            if (row) CHECK(view->visualRect(combo.model()->index(row - 1, 0)).bottom() < before.top());
            QTextLayout layout(index.data().toString(), view->font());
            layout.beginLayout(); const auto line = layout.createLine(); layout.endLayout();
            CHECK(before.height() >= qCeil(line.height()));
            const int scrollBefore = view->verticalScrollBar()->value();
            QTest::mouseMove(view->viewport(), before.center()); settle();
            // At fractional scale the popup can legitimately auto-scroll near
            // its lower edge. Compare content geometry, not viewport offsets.
            // Hover must not change row height or its position within the list.
            const auto after = view->visualRect(index).translated(0,
                originBefore - view->visualRect(combo.model()->index(0, 0)).top());
            if (after != before) {
                std::cerr << "Popup " << language << " row " << row << " rect "
                    << before.x() << ',' << before.y() << ',' << before.width() << ',' << before.height()
                    << " -> " << after.x() << ',' << after.y() << ',' << after.width() << ',' << after.height()
                    << " scroll " << scrollBefore << " -> " << view->verticalScrollBar()->value()
                    << " viewport " << view->viewport()->width() << 'x' << view->viewport()->height() << '\n';
            }
            CHECK(after == before);
        }
        if (!output.isEmpty()) view->window()->grab().save(output + "/dropdown-after-" + language + ".png");
        combo.hidePopup();
    }
}
void dialogButtons()
{
    auto& service = i::Localization::instance(); service.setLanguage("en");
    u::PreferencesDialog preferences(u::PreferencesState{});
    u::AboutDialog about(nullptr);
    u::NewDocumentDialog document;
    u::NewDocumentDialog canvas(u::NewDocumentDialog::Mode::ChangeCanvasSize, {});
    u::ColorDialog color(QColor(12, 34, 56, 78));
    color.setOption(u::ColorDialog::ShowAlphaChannel);
    color.setCurrentColor(QColor(12, 34, 56, 78));
    i::ButtonBox custom(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    i::setText(custom.button(QDialogButtonBox::Ok), i::text("dialog.button.create"));
    for (const auto* language : {"ar", "en", "fr", "es", "de", "it", "pt-BR", "pl", "tr",
             "ru", "uk", "hi", "zh-Hans", "zh-Hant", "ja", "ko", "ar", "en"}) {
        service.setLanguage(language); settle();
        for (const auto& entry : std::initializer_list<std::pair<const char*, const char*>>{
                {"PreferencesOk", "dialog.button.ok"}, {"PreferencesApply", "dialog.button.apply"},
                {"PreferencesCancel", "dialog.button.cancel"}}) {
            auto* button = preferences.findChild<QPushButton*>(entry.first);
            CHECK(button && button->text() == i::text(entry.second));
        }
        CHECK(about.findChild<QPushButton*>("AboutClose")->text() == i::text("dialog.button.close"));
        CHECK(custom.button(QDialogButtonBox::Ok)->text() == i::text("dialog.button.create"));
        CHECK(custom.standardButton(custom.button(QDialogButtonBox::Ok)) == QDialogButtonBox::Ok);
        CHECK(document.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->text() == i::text("dialog.button.create"));
        CHECK(canvas.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->text() == i::text("dialog.button.apply"));
        for (auto* dialog : std::initializer_list<QDialog*>{&document, &canvas, &color}) {
            const auto* buttons = dialog->findChild<QDialogButtonBox*>();
            CHECK(buttons->button(QDialogButtonBox::Cancel)->text() == i::text("dialog.button.cancel"));
        }
        CHECK(color.currentColor() == QColor(12, 34, 56, 78));
    }
}
void workspace(const QString& output, const char* initialLanguage = "en")
{
    auto& service = i::Localization::instance(); service.setLanguage(initialLanguage);
    u::MainWindow window(nullptr, false, false);
    window.setUnsavedPromptEnabled(false); window.resize(1300, 850); window.show(); settle();
    auto* preferences = window.findChild<QAction*>("PreferencesAction");
    auto* adjustment = window.findChild<QAction*>("NewAdjustmentLayerAction");
    CHECK(preferences && !preferences->icon().isNull());
    int adjustmentMenus = 0;
    for (auto* action : window.menuBar()->actions()) {
        if (const auto* menu = action->menu()) {
            if (menu->actions().contains(adjustment)) ++adjustmentMenus;
            if (menu->actions().contains(preferences))
                CHECK(!menu->actions().contains(adjustment));
        }
    }
    CHECK(adjustmentMenus == 1); // Layers only; its other creation entry points remain intact.
    if (!output.isEmpty() && preferences) preferences->icon().pixmap(QSize(48, 48)).save(output + "/preferences-icon.png");
    QTemporaryDir dir;
    QImage image(60, 40, QImage::Format_RGBA8888); image.fill(QColor(40, 80, 120, 128));
    CHECK(image.save(dir.filePath("test.png")));
    CHECK(window.openImageFromPath(dir.filePath("test.png")));
    CHECK(window.openImageFromPath(dir.filePath("test.png")));
    const auto ids = window.documentIds(); const auto active = window.activeDocumentId();
    const auto* doc = window.editorSession().document();
    const auto revision = doc->revision(); const auto undo = window.editorSession().history().undoDepth();
    const auto* workspace = dynamic_cast<u::OverlayDockWorkspace*>(window.findChild<QWidget*>("CanvasWorkspace"));
    CHECK(workspace);
    if(QGuiApplication::platformName()=="xcb"||QGuiApplication::platformName().startsWith("wayland")) {
        CHECK(QTest::qWaitForWindowExposed(window.windowHandle()));
        QGuiApplication::sync();
        // Native placement/configure can arrive after QWidget::show returns.
        // Capture a settled baseline before changing language; keep the strict
        // geometry equality assertion throughout the actual switching test.
        QElapsedTimer deadline,stable;deadline.start();stable.start();auto previous=window.geometry();
        while(stable.elapsed()<200&&deadline.elapsed()<2000) {
            QTest::qWait(20);
            if(window.geometry()!=previous){previous=window.geometry();stable.restart();}
        }
        CHECK(stable.elapsed()>=200);
    }
    const auto tabs = workspace ? workspace->savePanelTabs() : QByteArray{};
    const auto state = window.saveState(); const auto geometry = window.geometry();
    auto combos = window.findChildren<QComboBox*>();
    auto* effects = dynamic_cast<u::EffectsPanel*>(window.findChild<QWidget*>("EffectsPanelContent"));
    CHECK(effects);
    auto* gradient=dynamic_cast<u::GradientStopEditor*>(window.findChild<QWidget*>("EffectGradientStops"));
    auto* swatches=dynamic_cast<u::SwatchesPanel*>(window.findChild<QWidget*>("SwatchesPanelContent"));
    CHECK(gradient && swatches);
    const auto gradientStops=gradient?gradient->stops():std::vector<imageeditor::core::GradientStop>{};
    const auto paintColors=window.editorSession().colors();
    int effectEdits = 0;
    if (effects) effects->onInteractionStarted = [&] { ++effectEdits; return false; };
    std::vector<int> indexes; for (auto* c : combos) indexes.push_back(c->currentIndex());
    for (const auto* language : {"fr", "es", "de", "it", "pt-BR", "pl", "tr", "ru", "uk",
             "ar", "hi", "zh-Hans", "zh-Hant", "ja", "ko", "qps", "fr", "en"}) {
        service.setLanguage(language); settle();
        CHECK(effectEdits == 0);
        for(const auto& entry:std::initializer_list<std::pair<const char*,const char*>>{
                {"ColorSwatchesSectionHeader","swatches.panel.swatches"},
                {"GradientSwatchesSectionHeader","swatches.panel.gradient"},
                {"CustomSwatchesSectionHeader","swatches.panel.custom"},
                {"LayerInfoSectionHeader","properties.info.title"},
                {"ToolShortcutsSectionHeader","properties.panel.shortcuts"}}) {
            const auto headers=window.findChildren<QToolButton*>(entry.first);
            CHECK(!headers.empty());
            for(const auto* header:headers)CHECK(header->text()==i::text(entry.second));
        }
        if(gradient) {
            CHECK(gradient->accessibleName()==i::text("effects.gradient.editor"));
            CHECK(gradient->toolTip()==i::text("effects.gradient.editor.hint"));
            CHECK(gradient->stops()==gradientStops && !gradient->interactionActive());
        }
        if(swatches) {
            const auto* grid=swatches->findChild<QListWidget*>("GradientSwatches");
            CHECK(grid && grid->count()==40);
            if(grid)for(int n=0;n<grid->count();++n)
                CHECK(grid->item(n)->toolTip()==i::text("swatches.panel.gradient.preset").arg(n+1));
        }
        CHECK(window.editorSession().colors()==paintColors);
        for(const auto& [objectName,id]:{
                std::pair{"EffectPixelAlphaShapesEffects","effects.panel.pixel.alpha.shapes.effects"},
                std::pair{"EffectMaskEffectsAfterRendering","effects.panel.mask.effects.after.rendering"}}) {
            const auto* option=window.findChild<QCheckBox*>(objectName);
            const auto* label=option?option->findChild<QLabel*>():nullptr;
            CHECK(option&&label&&option->accessibleName()==i::text(id)&&label->text()==i::text(id));
        }
        for (const auto& entry : std::initializer_list<std::pair<const char*, const char*>>{
                {"HorizontalRulerPositionMenu", "main.window.horizontal.position"},
                {"VerticalRulerPositionMenu", "main.window.vertical.position"},
                {"LayerMaskMenu", "main.window.layer.mask"}}) {
            const auto* menu = window.findChild<QMenu*>(entry.first);
            CHECK(menu && menu->title() == i::text(entry.second));
        }
        const auto* horizontal = window.findChild<QMenu*>("HorizontalRulerPositionMenu");
        const auto* vertical = window.findChild<QMenu*>("VerticalRulerPositionMenu");
        if (horizontal) {
            CHECK(horizontal->actions().at(0)->text() == i::text("main.window.top"));
            CHECK(horizontal->actions().at(1)->text() == i::text("main.window.bottom"));
        }
        if (vertical) {
            CHECK(vertical->actions().at(0)->text() == i::text("main.window.left"));
            CHECK(vertical->actions().at(1)->text() == i::text("main.window.right"));
        }
        for (const auto& entry : std::initializer_list<std::pair<const char*, const char*>>{
                {"FillBackgroundAction", "main.window.fill.fill.with.background"},
                {"FillForegroundAction", "main.window.fill.fill.with.foreground"},
                {"NewAdjustmentLayerAction", "main.window.new.adjustment.layer"},
                {"QuickCanvasResizeAction", "canvas.resize.action"},
                {"NewLayerFolderAction", "main.window.layer.organization.new.folder"},
                {"GroupLayersAction", "main.window.layer.organization.group.selected.items"},
                {"UngroupLayersAction", "main.window.layer.organization.ungroup"},
                {"MergeLayersAction", "main.window.layer.organization.merge.layers"},
                {"RasterizeLayersAction", "main.window.layer.organization.rasterize.layer"},
                {"DuplicateLayersAction", "main.window.layer.organization.duplicate.selected.layers"},
                {"RenameLayerItemAction", "main.window.layer.organization.rename"},
                {"HideSelectedLayersAction", "main.window.layer.organization.hide.selected"},
                {"ShowSelectedLayersAction", "main.window.layer.organization.reveal.selected"},
                {"IsolateSelectedLayersAction", "main.window.layer.organization.isolate.selected"},
                {"ShowAllLayersAction", "main.window.layer.organization.show.all.layers"},
                {"AddLayerMaskAction", "main.window.layer.masks.add.layer.mask"},
                {"MaskFromSelectionAction", "main.window.layer.masks.add.mask.from.selection"},
                {"DeleteLayerMaskAction", "main.window.layer.masks.delete.layer.mask"},
                {"EnableLayerMaskAction", "main.window.layer.masks.disable.layer.mask"},
                {"ApplyLayerMaskAction", "main.window.layer.masks.apply.layer.mask"},
                {"MaskToSelectionAction", "main.window.layer.masks.add.mask.to.selection"},
                {"InvertLayerMaskAction", "main.window.layer.masks.invert.layer.mask"}}) {
            const auto* action = window.findChild<QAction*>(entry.first);
            if (!action || action->text() != i::text(entry.second)) {
                std::cerr << language << ": stale " << entry.first << '\n'; ++failures;
            }
        }
        for (const auto& entry : std::initializer_list<std::pair<const char*, const char*>>{
                {"TextBold", "text.controller.bold"},
                {"TextAlignLeft", "text.controller.align.current.paragraphs.left"},
                {"FillModeSelection", "main.window.fill.selection.layer.click.to.fill.all.selected.coverage.or"},
                {"TransformFlipHorizontal", "transform.options.page.flip.horizontally.in.target.local.space"}}) {
            const auto* control = window.findChild<QWidget*>(entry.first);
            CHECK(control && control->toolTip() == i::text(entry.second));
        }
        CHECK(window.documentIds() == ids && window.activeDocumentId() == active);
        CHECK(window.editorSession().document() == doc && doc->revision() == revision);
        CHECK(window.editorSession().history().undoDepth() == undo);
        if (window.geometry() != geometry || window.saveState() != state)
            std::cerr << language << " geometry " << geometry.x() << ',' << geometry.y() << ' ' << geometry.width() << 'x' << geometry.height()
                << " -> " << window.x() << ',' << window.y() << ' ' << window.width() << 'x' << window.height()
                << " state=" << (window.saveState() == state) << " ltr=" << (qApp->layoutDirection() == Qt::LeftToRight)
                << " actualRect=" << window.geometry().x() << ',' << window.geometry().y()
                << " before=" << state.toHex().constData() << " after=" << window.saveState().toHex().constData() << '\n';
        CHECK(window.geometry() == geometry && window.saveState() == state);
        CHECK(!workspace || workspace->savePanelTabs() == tabs);
        for (qsizetype n = 0; n < combos.size(); ++n) CHECK(combos[n]->currentIndex() == indexes[size_t(n)]);
        if (!output.isEmpty()) {
            window.grab().save(output + "/workspace-" + language + ".png");
            if (workspace) workspace->panelOverlay()->grab().save(output + "/panels-" + language + ".png");
            u::PreferencesDialog preferences(u::PreferencesState{});
            preferences.show(); settle();
            preferences.grab().save(output + "/preferences-" + language + ".png");
            preferences.close();
        }
    }
    CHECK(window.activateDocument(ids.front())); CHECK(window.activateDocument(active));
    window.close(); settle(); service.canSwitch = {};
}
int main(int argc, char** argv)
{
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication app(argc, argv); app.setOrganizationName("VulkanaLocalizationTests"); app.setApplicationName("Tests");
    QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QStandardPaths::setTestModeEnabled(true);
    if (argc == 4 && QString::fromLocal8Bit(argv[1]) == "--probe-pack") {
        auto& service = i::Localization::instance(); service.initialize(QString::fromLocal8Bit(argv[2]));
        service.setLanguage("en");
        CHECK(i::text("language.label") == QString::fromUtf8(argv[3]));
        // These packs belong to the temporary workflow test. Reselecting the
        // language must reload English too, not just translated packs.
        const auto english = QString::fromLocal8Bit(argv[2]) + "/en.qm";
        if (QFile::exists(english)) {
            CHECK(QFile::rename(english, english + ".held"));
            service.setLanguage("en"); CHECK(i::text("language.label") == "Language");
            CHECK(QFile::rename(english + ".held", english));
            service.setLanguage("en"); CHECK(i::text("language.label") == QString::fromUtf8(argv[3]));
        }
        service.setLanguage("ar");
        CHECK(!i::text("language.label").isEmpty());
        return failures ? 1 : 0;
    }
#ifdef VULKANA_TEST_TRANSLATIONS
    // Relocatable builds deliberately omit development resource paths. Test the
    // generated packs explicitly; installed resource discovery is a staging check.
    i::Localization::instance().initialize(QStringLiteral(VULKANA_TEST_TRANSLATIONS));
#else
    i::Localization::instance().initialize();
#endif
    u::applyEditorTheme(app);
    const auto output = qEnvironmentVariable("VULKANA_LOCALIZATION_SCREENSHOTS");
    localeMatching(); bindings(); composition(); dropdown(output); dialogButtons();recentUi(output);
    adjustmentCategories(output); psdReview(output); workspace(output); workspace({}, "ar");
    std::cout << "Localization failures: " << failures << '\n'; return failures ? 1 : 0;
}
