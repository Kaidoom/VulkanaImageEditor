#include "imageeditor/diffusion/ModelProfiles.h"
#include "imageeditor/ui/DiffusionPanel.hpp"
#include "imageeditor/ui/DiffusionPreview.hpp"
#include "imageeditor/ui/Theme.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QToolButton>
#include <QWheelEvent>
#include <QtTest>

using namespace imageeditor::ui;
class DiffusionPanelTests : public QObject {
  Q_OBJECT
private slots:
  void initTestCase() {
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName("VulkanaTests");
    QCoreApplication::setApplicationName("DiffusionPanelTests");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       settings_.path());
    applyEditorTheme(
        *qobject_cast<QApplication *>(QCoreApplication::instance()));
  }
  void init() { QSettings().clear(); }
  void loadedModelsReflectWorkerNotConfiguration() {
    DiffusionPanel panel;
    QVERIFY(panel.findChild<QLabel *>("DiffusionLoadedModelsEmpty"));
    QVERIFY(!panel.findChild<QPushButton *>("DiffusionUnloadModel"));
    const QJsonArray loaded{QJsonObject{
        {"id", "worker:1"}, {"backend", "cuda"},
        {"profile", QJsonObject{{"name", "Qwen Image 2.1 Turbo"}}},
        {"files", QJsonObject{{"diffusion", "/models/qwen-q4.gguf"},
                              {"textEncoder", "/models/text-q4.gguf"},
                              {"vae", "/models/vae.safetensors"},
                              {"vision", "/models/vision.gguf"}}}}};
    panel.setLoadedModels(loaded, true);
    auto *unload = panel.findChild<QPushButton *>("DiffusionUnloadModel");
    QVERIFY(unload && unload->isEnabled());
    QString unloaded;
    panel.onUnloadModel = [&](const QString &id) { unloaded = id; };
    panel.setConfiguration({{"profile", "flux3-image-klein-distilled"},
                            {"files", QJsonObject{{"diffusion", "/models/flux.gguf"}}}});
    QCOMPARE(panel.findChild<QLabel *>("DiffusionLoadedFile_diffusion")->text(),
             QString("Diffusion · qwen-q4.gguf"));
    QVERIFY(panel.findChild<QLabel *>("DiffusionLoadedFile_vision"));
    if (const auto capture = qEnvironmentVariable("VULKANA_DIFFUSION_PANEL_CAPTURE");
        !capture.isEmpty()) {
      panel.resize(480, 800);
      panel.show();
      panel.findChild<QTabWidget *>("DiffusionTabs")->setCurrentIndex(0);
      QCoreApplication::processEvents();
      QVERIFY(panel.grab().save(capture + "-loaded-models.png"));
    }
    panel.setBusy(true);
    QVERIFY(!unload->isEnabled());
    unload->click();
    QVERIFY(unloaded.isEmpty());
    panel.setBusy(false);
    panel.setLoadedModels(loaded, false); // stopping, not yet unloaded
    QVERIFY(!unload->isEnabled());
    panel.setLoadedModels(loaded, true);
    unload->click();
    QCOMPARE(unloaded, QString("worker:1"));
    panel.setLoadedModels({}, false);
    QVERIFY(panel.findChild<QLabel *>("DiffusionLoadedModelsEmpty"));
    QVERIFY(!panel.findChild<QPushButton *>("DiffusionUnloadModel"));
  }
  void capabilityDefaultsAndUserOverrides() {
    DiffusionPanel panel;
    panel.setConfiguration({{"profile", "flux3-image-klein-distilled"},
                            {"files", QJsonObject{}},
                            {"settings", QJsonObject{}}});
    QCOMPARE(panel.request()["steps"].toInt(), 16);
    QCOMPARE(panel.request()["guidance"].toDouble(), 1.0);
    QVERIFY(!panel.request().contains("negativePrompt"));
    QVERIFY(!panel.request().contains("strength"));
    QVERIFY(
        !panel.configuration()["settings"].toObject().contains("prefixCache"));
    auto *steps = panel.findChild<QDoubleSpinBox *>("Diffusion_steps");
    QVERIFY(steps);
    steps->setValue(23);
    panel.setProfiles(imageeditor::diffusion::modelProfiles());
    QCOMPARE(panel.request()["steps"].toInt(), 23);
    panel.setMode(DiffusionPanel::Mode::Fill);
    QVERIFY(panel.imageToImageEnabled());
    QVERIFY(panel.request().contains("strength"));
    panel.setConfiguration({{"profile", "qwen-image-2.1-turbo"},
                            {"files", QJsonObject{}},
                            {"settings", QJsonObject{}}});
    QCOMPARE(panel.request()["steps"].toInt(), 8);
    QCOMPARE(panel.request()["schedule"].toString(), QString("turbo"));
  }
  void modeSpecificControls_data() {
    QTest::addColumn<QString>("profile");
    QTest::newRow("flux3") << QString("flux3-image-klein-distilled");
    QTest::newRow("qwen") << QString("qwen-image-2.1-turbo");
  }
  void newFillStrengthDefaultsRemainAdjustable_data() {
    QTest::addColumn<QString>("profile");
    QTest::newRow("qwen-turbo") << QString("qwen-image-2.1-turbo");
    QTest::newRow("qwen") << QString("qwen-image-2.1");
    QTest::newRow("flux3") << QString("flux3-image-klein-distilled");
  }
  void newFillStrengthDefaultsRemainAdjustable() {
    QFETCH(QString, profile);
    DiffusionPanel panel;
    panel.setConfiguration({{"profile", profile}});
    auto *strength = panel.findChild<QDoubleSpinBox *>("Diffusion_strength");
    QVERIFY(strength);
    QCOMPARE(strength->value(), .75); // other modes keep their default
    panel.setMode(DiffusionPanel::Mode::Fill);
    QCOMPARE(strength->value(), 1.0);
    QCOMPARE(panel.prepareGeneration()["strength"].toDouble(), 1.0);
    QVERIFY(strength->isEnabled());
    QVERIFY(panel.findChild<QTabWidget *>("DiffusionTabs")
                ->widget(1)->isAncestorOf(strength));
    strength->setValue(.6);
    panel.setMode(DiffusionPanel::Mode::Edit);
    panel.setMode(DiffusionPanel::Mode::Fill);
    QCOMPARE(panel.request()["strength"].toDouble(), .6);
    panel.saveSettings();
    DiffusionPanel restored;
    QCOMPARE(restored.request()["strength"].toDouble(), .6);
  }
  void savedFillStrengthIsPreserved_data() {
    QTest::addColumn<double>("value");
    for (double value : {0.0, .75, 1.0})
      QTest::newRow(qPrintable(QString::number(value))) << value;
  }
  void savedFillStrengthIsPreserved() {
    QFETCH(double, value);
    const QJsonObject generation{{"strength", value}};
    QSettings().setValue("diffusion/generation", QJsonDocument(generation).toJson());
    QSettings().setValue("diffusion/modelConfigurations", QJsonDocument(QJsonArray{
        QJsonObject{{"id", "saved"}, {"name", "Saved configuration"},
                    {"configuration", QJsonObject{{"profile", "flux3-image-klein-distilled"}}},
                    {"generation", generation}}}).toJson());
    DiffusionPanel panel;
    panel.setMode(DiffusionPanel::Mode::Fill);
    QCOMPARE(panel.request()["strength"].toDouble(), value);
    auto *models = panel.findChild<QComboBox *>("DiffusionModels");
    models->setCurrentIndex(models->findData("saved"));
    QCOMPARE(panel.request()["strength"].toDouble(), value);
    models->setCurrentIndex(0); // genuinely new configuration
    QCOMPARE(panel.request()["strength"].toDouble(), 1.0);
    models->setCurrentIndex(models->findData("saved"));
    QCOMPARE(panel.request()["strength"].toDouble(), value);
  }
  void modeSpecificControls() {
    QFETCH(QString, profile);
    DiffusionPanel panel;
    panel.setConfiguration({{"profile", profile}});
    auto *tabs = panel.findChild<QTabWidget *>("DiffusionTabs");
    auto *denoise = panel.findChild<QCheckBox *>("DiffusionImageToImage");
    auto *strength = panel.findChild<QDoubleSpinBox *>("Diffusion_strength");
    auto *negative = panel.findChild<QPlainTextEdit *>("DiffusionNegativePrompt");
    QVERIFY(denoise && strength && negative);
    QVERIFY(tabs->widget(1)->isAncestorOf(denoise));
    QVERIFY(tabs->widget(1)->isAncestorOf(strength));
    QVERIFY(!tabs->widget(0)->isAncestorOf(strength));
    QVERIFY(!denoise->isHidden() && !strength->isHidden());
    QVERIFY(!negative->isHidden() && !negative->isEnabled());
    QVERIFY(negative->toolTip().contains("CFG"));
    panel.setMode(DiffusionPanel::Mode::Generate);
    strength->setValue(1);
    QVERIFY(!denoise->isChecked() && !denoise->isEnabled());
    QVERIFY(!strength->isEnabled());
    QVERIFY(!panel.request().contains("strength"));
    panel.setMode(DiffusionPanel::Mode::Edit);
    QVERIFY(denoise->isEnabled() && !denoise->isChecked());
    QVERIFY(!strength->isEnabled());
    QVERIFY(strength->toolTip().contains("Reference-only"));
    denoise->setChecked(true);
    QVERIFY(strength->isEnabled());
    QCOMPARE(panel.request()["strength"].toDouble(), 1.0);
    panel.setBusy(true);
    QVERIFY(!denoise->isEnabled() && !strength->isEnabled());
    QCOMPARE(panel.request()["strength"].toDouble(), 1.0); // mode, not widget state
    panel.setBusy(false);
    QVERIFY(denoise->isEnabled() && strength->isEnabled());
    panel.setMode(DiffusionPanel::Mode::Fill);
    QVERIFY(denoise->isChecked() && !denoise->isEnabled());
    QVERIFY(strength->isEnabled());
    QVERIFY(denoise->toolTip().contains("Always used"));
    strength->setValue(.75);
    panel.setMode(DiffusionPanel::Mode::Generate);
    QVERIFY(!denoise->isChecked() && !strength->isEnabled());
    panel.setMode(DiffusionPanel::Mode::Edit);
    QVERIFY(denoise->isChecked());
    QCOMPARE(panel.request()["strength"].toDouble(), .75);
    denoise->setChecked(false);
    panel.setMode(DiffusionPanel::Mode::Fill);
    QVERIFY(denoise->isChecked());
    panel.setMode(DiffusionPanel::Mode::Edit);
    QVERIFY(!denoise->isChecked());
    QVERIFY(!panel.request().contains("strength"));
    QCOMPARE(strength->value(), .75); // unavailable values are retained
    if (const auto capture = qEnvironmentVariable("VULKANA_DIFFUSION_PANEL_CAPTURE");
        !capture.isEmpty() && profile.startsWith("flux3")) {
      panel.setMode(DiffusionPanel::Mode::Fill);
      panel.resize(480, 900);
      panel.showGeneration();
      panel.show();
      QCoreApplication::processEvents();
      QVERIFY(panel.grab().save(capture + "-mode-settings.png"));
    }
  }
  void modeSettingsSurviveRestartAndLegacyConfigurations() {
    const QJsonObject configuration{{"profile", "flux3-image-klein-distilled"}};
    QSettings().setValue("diffusion/modelConfigurations", QJsonDocument(QJsonArray{
        QJsonObject{{"id", "legacy"}, {"name", "Legacy"},
                    {"configuration", configuration},
                    {"generation", QJsonObject{{"strength", 1.0}}}},
        QJsonObject{{"id", "reference"}, {"name", "Reference editing"},
                    {"configuration", configuration}, {"editImageToImage", false},
                    {"generation", QJsonObject{{"strength", .8}}}}}).toJson());
    {
      DiffusionPanel panel;
      panel.setMode(DiffusionPanel::Mode::Edit);
      auto *denoise = panel.findChild<QCheckBox *>("DiffusionImageToImage");
      denoise->setChecked(true);
      auto *configs = panel.findChild<QComboBox *>("DiffusionModels");
      configs->setCurrentIndex(configs->findData("legacy"));
      QCOMPARE(panel.request()["strength"].toDouble(), 1.0);
      QVERIFY(denoise->isChecked()); // absent legacy preference keeps current choice
      configs->setCurrentIndex(configs->findData("reference"));
      QVERIFY(!denoise->isChecked());
      QCOMPARE(panel.findChild<QDoubleSpinBox *>("Diffusion_strength")->value(), .8);
      denoise->setChecked(true);
      panel.setMode(DiffusionPanel::Mode::Generate);
      panel.saveSettings();
    }
    DiffusionPanel restored;
    QVERIFY(!restored.findChild<QCheckBox *>("DiffusionImageToImage")->isChecked());
    restored.setMode(DiffusionPanel::Mode::Edit);
    QVERIFY(restored.imageToImageEnabled());
    QCOMPARE(restored.request()["strength"].toDouble(), .8);
  }
  void compactTabsAndSafeScrolling() {
    DiffusionPanel panel;
    panel.resize(500, 760);
    panel.show();
    panel.showGeneration();
    auto *tabs = panel.findChild<QTabWidget *>("DiffusionTabs");
    QVERIFY(tabs);
    QCOMPARE(tabs->count(), 3);
    auto *generate = panel.findChild<QPushButton *>("DiffusionGenerate");
    auto *model = panel.findChild<QComboBox *>("DiffusionModels");
    QVERIFY(!generate->isEnabled());
    QVERIFY(model->currentText().contains("Missing configuration"));
    auto *width = panel.findChild<QSpinBox *>("DiffusionWidth");
    auto *height = panel.findChild<QSpinBox *>("DiffusionHeight");
    QVERIFY(!panel.findChild<QComboBox *>("DiffusionDestination"));
    auto *output = panel.findChild<QComboBox *>("DiffusionMode");
    QCoreApplication::processEvents();
    QCOMPARE(width->mapTo(&panel, QPoint{}).y(),
             height->mapTo(&panel, QPoint{}).y());
    QVERIFY(panel.findChild<QToolButton *>("DiffusionResetSize"));
    QVERIFY(!panel.findChild<QLineEdit *>("DiffusionFile_worker"));
    const int original = width->value();
    auto wheel = [](QWidget *widget) {
      QWheelEvent event(QPointF(10, 10), widget->mapToGlobal(QPoint(10, 10)),
                        {}, QPoint(0, -120), Qt::NoButton, Qt::NoModifier,
                        Qt::NoScrollPhase, false);
      QCoreApplication::sendEvent(widget, &event);
    };
    width->setFocus();
    wheel(width);
    QCOMPARE(width->value(), original);
    const int destination = output->currentIndex();
    wheel(output);
    QCOMPARE(output->currentIndex(), destination);
    QTest::keyClick(width, Qt::Key_Up);
    QCOMPARE(width->value(), original + 32);
    auto *seed = panel.findChild<QLineEdit *>("DiffusionSeed");
    auto *policy = panel.findChild<QComboBox *>("DiffusionSeedMode");
    QVERIFY(policy);
    policy->setCurrentIndex(policy->findData("random"));
    const auto randomA = panel.prepareGeneration()["seed"].toDouble();
    const auto randomB = panel.prepareGeneration()["seed"].toDouble();
    QVERIFY(randomA != randomB);
    QVERIFY(!seed->isEnabled());
    policy->setCurrentIndex(policy->findData("fixed"));
    seed->setText("42");
    QCOMPARE(panel.prepareGeneration()["seed"].toInt(), 42);
    panel.generationAccepted();
    QCOMPARE(panel.prepareGeneration()["seed"].toInt(), 42);
    auto *randomSeed = panel.findChild<QToolButton *>("DiffusionRandomSeed");
    QVERIFY(randomSeed);
    randomSeed->click();
    QVERIFY(seed->text() != "42");
    QCOMPARE(policy->currentData().toString(), QString("fixed"));
    QCOMPARE(panel.prepareGeneration()["seed"].toDouble(), seed->text().toDouble());
    seed->setText("42");
    policy->setCurrentIndex(policy->findData("increment"));
    panel.findChild<QDoubleSpinBox *>("Diffusion_batch")->setValue(2);
    panel.generationAccepted();
    QCOMPARE(panel.prepareGeneration()["seed"].toInt(), 44);
    auto *inputs = panel.findChild<QListWidget *>("DiffusionInputsList");
    QCOMPARE(inputs->count(), 1); // Add row is always accessible.
    bool requestedInput = false;
    panel.onAddInput = [&](auto source) {
      requestedInput = source == DiffusionPanel::InputSource::File;
    };
    QTest::mouseClick(inputs->viewport(), Qt::LeftButton, {},
                      inputs->visualItemRect(inputs->item(0)).center());
    QTRY_VERIFY(qobject_cast<QMenu *>(QApplication::activePopupWidget()));
    auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
    QVERIFY(!menu->actions().isEmpty());
    menu->actions().last()->trigger();
    menu->close();
    QVERIFY(requestedInput);
    QImage image(64, 64, QImage::Format_RGBA8888);
    image.fill(Qt::red);
    panel.addInput({image, "Reference", "Test", false, {}});
    QCOMPARE(inputs->count(), 2);
    QVERIFY(inputs->height() < 90);
    QCoreApplication::processEvents();
    const QRect item = inputs->visualItemRect(inputs->item(0));
    QTest::mouseClick(inputs->viewport(), Qt::LeftButton, {},
                      QPoint(item.right() - 12, item.center().y()));
    QTRY_COMPARE(panel.inputs().size(), 0);
    tabs->setCurrentIndex(0);
    auto *advanced =
        panel.findChild<QToolButton *>("DiffusionGenerationSectionHeader");
    advanced->setChecked(true);
    auto *steps = panel.findChild<QDoubleSpinBox *>("Diffusion_steps");
    panel.findChild<QScrollArea *>("DiffusionConfigurationPage")
        ->ensureWidgetVisible(steps);
    QCoreApplication::processEvents();
    const auto before = steps->value();
    auto *scroll = panel.findChild<QScrollArea *>("DiffusionConfigurationPage")
                       ->verticalScrollBar();
    const auto scrollBefore = scroll->value();
    wheel(steps);
    QCOMPARE(steps->value(), before);
    QVERIFY2(
        scroll->value() > scrollBefore,
        qPrintable(
            QString(
                "scroll before %1 after %2 max %3 visible %4 current tab %5")
                .arg(scrollBefore)
                .arg(scroll->value())
                .arg(scroll->maximum())
                .arg(steps->isVisible())
                .arg(tabs->currentIndex())));
    for (const auto &key : {"diffusion", "textEncoder", "vae"})
      panel.findChild<QLineEdit *>(QString("DiffusionFile_") + key)
          ->setText("/models/example.gguf");
    QVERIFY(generate->isEnabled());
    QVERIFY(!tabs->widget(1)->isAncestorOf(steps));
    QVERIFY(tabs->widget(0)->isAncestorOf(steps));
  }
  void inputSectionGrowsOnlyWithImages() {
    DiffusionPanel panel;
    panel.resize(650, 1400);
    panel.show();
    panel.showGeneration();
    auto *list = panel.findChild<QListWidget *>("DiffusionInputsList");
    auto *section = panel.findChild<QWidget *>("DiffusionInputsSection");
    auto *header =
        panel.findChild<QToolButton *>("DiffusionInputsSectionHeader");
    QImage input(1024, 1536, QImage::Format_RGBA8888);
    input.fill(Qt::red);
    int capped = 0;
    for (int count = 0; count <= 7; ++count) {
      if (count)
        QVERIFY(panel.addInput({input, "Portrait", "Test", false, {}}));
      QCoreApplication::processEvents();
      QVERIFY2(section->height() <= list->height() + header->height() + 20,
               qPrintable(QString("Section %1, list %2")
                              .arg(section->height())
                              .arg(list->height())));
      if (count == 1)
        QVERIFY(list->height() < 100);
      if (count == 5)
        capped = list->height();
      if (count > 5) {
        QCOMPARE(list->height(), capped);
        QVERIFY(list->verticalScrollBar()->maximum() > 0);
      }
    }
  }
  void comparisonAlignmentAndDivider() {
    DiffusionPreview preview;
    preview.resize(800, 600);
    QImage result(100, 80, QImage::Format_RGBA8888);
    result.fill(Qt::red);
    QImage reference(40, 20, QImage::Format_RGBA8888);
    reference.fill(Qt::blue);
    preview.setImage(result);
    preview.setComparison(reference, QTransform::fromTranslate(110, 60),
                          QTransform::fromTranslate(100, 50));
    preview.setComparisonEnabled(true);
    preview.show();
    preview.actualSize();
    QCoreApplication::processEvents();
    QCOMPARE(preview.referenceRect().topLeft() - preview.imageRect().topLeft(),
             QPointF(10, 10));
    QCOMPARE(preview.referenceRect().size(), QSizeF(40, 20));
    const auto origin = preview.imageRect().topLeft();
    const auto painted = preview.grab().toImage();
    QCOMPARE(painted.pixelColor((origin + QPointF(20, 20)).toPoint()),
             QColor(Qt::blue));
    QCOMPARE(painted.pixelColor((origin + QPointF(75, 20)).toPoint()),
             QColor(Qt::red));
    QVERIFY(painted.pixelColor((origin + QPointF(5, 20)).toPoint()) !=
            QColor(Qt::red));
    const auto frame = preview.comparisonBounds();
    QTest::mousePress(&preview, Qt::LeftButton, {}, frame.center().toPoint());
    QTest::mouseMove(&preview, (frame.topLeft() + QPointF(frame.width() * .25,
                                                          frame.height() * .5))
                                   .toPoint());
    QTest::mouseRelease(
        &preview, Qt::LeftButton, {},
        (frame.topLeft() + QPointF(frame.width() * .25, frame.height() * .5))
            .toPoint());
    QVERIFY(std::abs(preview.comparisonSplit() - .25) < .02);
    preview.setComparisonAlignment(DiffusionPreview::Alignment::Center);
    preview.actualSize();
    QCOMPARE(preview.referenceRect().center(), preview.imageRect().center());
    QCOMPARE(preview.referenceRect().size(), QSizeF(40, 20));
    preview.setComparisonAlignment(DiffusionPreview::Alignment::Fit);
    preview.actualSize();
    QCOMPARE(preview.referenceRect().size(), QSizeF(100, 50));
    QCOMPARE(preview.referenceRect().center(), preview.imageRect().center());
    preview.setComparisonAlignment(DiffusionPreview::Alignment::Canvas);
    preview.setComparison(reference, QTransform::fromTranslate(-40, -20),
                          QTransform{});
    preview.actualSize();
    QCOMPARE(preview.referenceRect().topLeft() - preview.imageRect().topLeft(),
             QPointF(-40, -20));
    // Identical portrait inputs remain on exactly the same pixel grid.
    QImage portrait(1024, 1536, QImage::Format_RGBA8888);
    portrait.fill(Qt::red);
    preview.setImage(portrait);
    preview.setComparison(portrait);
    preview.fitView();
    preview.setComparisonSplit(0);
    const auto left = preview.grab().toImage();
    preview.setComparisonSplit(1);
    const auto right = preview.grab().toImage();
    QCOMPARE(left.pixelColor(400, 300), right.pixelColor(400, 300));
  }
  void pendingPreviewAndOverlayNavigation() {
    QWidget host;
    host.resize(1000, 800);
    DiffusionPanel panel(&host);
    panel.resize(500, 740);
    host.show();
    QImage first(90, 60, QImage::Format_RGBA8888);
    first.fill(Qt::red);
    QImage second(120, 80, QImage::Format_RGBA8888);
    second.fill(Qt::blue);
    panel.appendResult({first, {}, "First", {}});
    panel.appendResult({second, {}, "Second", {}});
    auto *list = panel.findChild<QListWidget *>("DiffusionResultsList");
    list->setCurrentRow(0);
    panel.setBusy(true);
    QCOMPARE(list->count(), 3);
    QCOMPARE(panel.results().size(), 2);
    QImage draft(24, 16, QImage::Format_RGBA8888);
    draft.fill(Qt::green);
    panel.setProgressPreview(draft);
    QCOMPARE(panel.selectedResult()->image, first);
    panel.showResultPreview();
    auto *dialog = host.findChild<QDialog *>("DiffusionPreviewDialog");
    QVERIFY(dialog);
    auto *previous =
        dialog->findChild<QToolButton *>("DiffusionPreviousPreview");
    auto *next = dialog->findChild<QToolButton *>("DiffusionNextPreview");
    auto *preview = dynamic_cast<DiffusionPreview *>(
        dialog->findChild<QWidget *>("DiffusionPreview"));
    QVERIFY(previous && next && preview);
    QCOMPARE(preview->image(), first);
    next->click();
    QCOMPARE(preview->image(), second);
    next->click();
    QCOMPARE(preview->image(), draft);
    QVERIFY(!panel.selectedResult());
    QCOMPARE(panel.selectedResultIndex(), -1);
    QVERIFY(!panel.findChild<QPushButton *>("DiffusionApply")->isEnabled());
    QVERIFY(!dialog->findChild<QPushButton *>("DiffusionCompareSelected")
                 ->isEnabled());
    draft.fill(Qt::yellow);
    panel.setProgressPreview(draft);
    QCOMPARE(preview->image(), draft);
    QCOMPARE(list->count(), 3);
    const auto captures =
        qEnvironmentVariable("VULKANA_DIFFUSION_PANEL_CAPTURE");
    if (!captures.isEmpty())
      QVERIFY(dialog->grab().save(captures + "-pending.png"));
    next->click();
    QCOMPARE(preview->image(), first); // wrap
    previous->click();
    QCOMPARE(preview->image(), draft);
    panel.setBusy(false); // cancellation keeps completed results intact
    QCOMPARE(list->count(), 2);
    QCOMPARE(panel.results().size(), 2);
    QCOMPARE(preview->image(), second);
    QCOMPARE(panel.results()[0].image, first);
    panel.setBusy(true);
    list->setCurrentRow(2);
    QVERIFY(preview->image().isNull());
    panel.setProgressPreview(draft);
    panel.setBusy(false);
    QImage final(180, 120, QImage::Format_RGBA8888);
    final.fill(Qt::magenta);
    panel.appendResult({final, {}, "Third", {}});
    QCOMPARE(list->count(), 3);
    QCOMPARE(panel.results().size(), 3);
    QCOMPARE(preview->image(), final);
    dialog->close();
  }
  void inputAndResultIndependence() {
    DiffusionPanel panel;
    QImage source(30, 20, QImage::Format_RGBA8888);
    source.fill(Qt::red);
    QVERIFY(panel.addInput({source, "Input", "Pixels", false, {}}));
    source.fill(Qt::blue);
    QCOMPARE(panel.inputs().first().image.pixelColor(0, 0), QColor(Qt::red));
    panel.appendResult({source, {}, "First", {{"id", 1}}});
    panel.setBusy(true);
    panel.setProgressPreview(source);
    QCOMPARE(panel.results().size(), 1);
    QCOMPARE(panel.selectedResult()->metadata["id"].toInt(), 1);
    QImage second(40, 10, QImage::Format_RGBA8888);
    second.fill(Qt::green);
    panel.appendResult({second, {}, "Second", {{"id", 2}}});
    QCOMPARE(panel.results().size(), 2);
    QCOMPARE(panel.selectedResultIndex(), 1);
    auto *list = panel.findChild<QListWidget *>("DiffusionResultsList");
    list->setCurrentRow(0);
    QCOMPARE(panel.selectedResult()->image.size(), QSize(30, 20));
    panel.clearResults();
    QVERIFY(panel.selectedResult() == nullptr);
  }
  void referenceBudgetAndDestinationPreview() {
    DiffusionPanel panel;
    QImage image(20, 20, QImage::Format_RGBA8888);
    image.fill(Qt::red);
    for (int i = 0; i < 8; ++i)
      QVERIFY(
          panel.addInput({image, QString::number(i), "Reference", false, {}}));
    QVERIFY(!panel.addInput({image, "Excess", "Reference", false, {}}));
    panel.setRegions({{QRectF(.1, .1, .4, .4), "Object"}});
    QCOMPARE(panel.inputs().size(),
             8); // Never drop existing references implicitly.
    QVERIFY(!panel.addInput({image, "Excess", "Reference", false, {}}));
    panel.appendResult({image, {}, "First", {{"rawResult", 7}}});
    int changed = 0;
    panel.onDestinationChanged = [&] { ++changed; };
    panel.setDestination(DiffusionPanel::Destination::NewDocument);
    QCOMPARE(changed, 1);
    QImage preview = image;
    preview.fill(Qt::green);
    panel.setResultPreview(0, preview);
    QCOMPARE(panel.selectedResult()->image.pixelColor(0, 0), QColor(Qt::green));
    QCOMPARE(panel.selectedResult()->metadata["rawResult"].toInt(), 7);
  }
  void fullResolutionPreviewAndRegionEditing() {
    QWidget host;
    host.resize(900, 700);
    DiffusionPanel panel(&host);
    panel.resize(350, 600);
    host.show();
    QImage image(1600, 1000, QImage::Format_RGBA8888);
    image.fill(Qt::red);
    const auto sourcePath =
        qEnvironmentVariable("VULKANA_DIFFUSION_PANEL_IMAGE");
    if (!sourcePath.isEmpty()) {
      QImage supplied(sourcePath);
      if (!supplied.isNull())
        image = supplied.scaled(image.size(), Qt::IgnoreAspectRatio,
                                Qt::SmoothTransformation);
    }
    panel.appendResult({image, {}, "Large", {}});
    QVERIFY(!panel.findChild<QPushButton *>("DiffusionInspectResult"));
    auto *thumbnail = panel.findChild<QLabel *>("DiffusionResultThumbnail");
    QVERIFY(thumbnail);
    QTest::mouseMove(thumbnail, thumbnail->rect().center());
    QCoreApplication::processEvents();
    if (const auto captures =
            qEnvironmentVariable("VULKANA_DIFFUSION_PANEL_CAPTURE");
        !captures.isEmpty())
      QVERIFY(thumbnail->grab().save(captures + "-hover.png"));
    QTest::mouseClick(thumbnail, Qt::LeftButton, {},
                      thumbnail->rect().center());
    QTRY_VERIFY(host.findChild<QDialog *>("DiffusionPreviewDialog"));
    auto *dialog = host.findChild<QDialog *>("DiffusionPreviewDialog");
    auto *preview = static_cast<DiffusionPreview *>(
        dialog->findChild<QWidget *>("DiffusionPreview"));
    QVERIFY(preview);
    QCOMPARE(preview->image().size(), QSize(1600, 1000));
    preview->actualSize();
    QCOMPARE(preview->zoom(), 1.0);
    preview->setRegionEditing(true);
    preview->setRegions({{QRectF(.2, .3, .4, .2), "Subject"}});
    preview->setSelectedRegion(0);
    preview->setSelectedRegionLabel("Flower");
    QCOMPARE(preview->regions()[0].label, QString("Flower"));
    const auto captures =
        qEnvironmentVariable("VULKANA_DIFFUSION_PANEL_CAPTURE");
    if (!captures.isEmpty()) {
      preview->fitView();
      QTest::qWait(60);
      QVERIFY(dialog->grab().save(captures + "-regions.png"));
      panel.resize(500, 760);
      host.resize(900, 820);
      QTest::qWait(60);
      QVERIFY(panel.grab().save(captures + "-panel-setup.png"));
      if (auto *header =
              panel.findChild<QToolButton *>("DiffusionModelSectionHeader"))
        header->setChecked(false);
      QTest::qWait(60);
      auto *tabs = panel.findChild<QTabWidget *>("DiffusionTabs");
      tabs->setCurrentIndex(1);
      panel.addInput({image, "Example input", "Raw layer pixels", false, {}});
      QTest::qWait(60);
      QVERIFY(panel.grab().save(captures + "-panel.png"));
      tabs->setCurrentIndex(0);
      QTest::qWait(60);
      QVERIFY(panel.grab().save(captures + "-config.png"));
      tabs->setCurrentIndex(2);
      if (auto *area = panel.findChild<QScrollArea *>())
        area->verticalScrollBar()->setValue(
            area->verticalScrollBar()->maximum());
      QTest::qWait(60);
      QVERIFY(panel.grab().save(captures + "-panel-results.png"));
      preview->setRegionEditing(false);
      preview->actualSize();
      QTest::qWait(60);
      QVERIFY(dialog->grab().save(captures + "-actual.png"));
    }
    preview->removeSelectedRegion();
    QVERIFY(preview->regions().isEmpty());
    dialog->close();
  }
  void persistentModelSettings() {
    {
      DiffusionPanel panel;
      panel.setWorkerExecutable("/example/worker");
      panel.setConfiguration(
          {{"profile", "qwen-image-2.1-turbo"},
           {"files", QJsonObject{{"diffusion", "/models/a.gguf"},
                                 {"textEncoder", "/models/b.gguf"},
                                 {"vae", "/models/c.safetensors"}}},
           {"settings",
            QJsonObject{{"device", "CUDA7"},
                        {"maxVramGiB", 7.5},
                        {"threads", 4},
                        {"weights", "manual"},
                        {"placements", QJsonObject{{"diffusion", "gpu"},
                                                   {"textEncoder", "ram"},
                                                   {"vae", "disk"}}}}}});
      auto *sampler = panel.findChild<QComboBox *>("Diffusion_sampler");
      sampler->addItem("Experimental sampler", "euler_ge");
      sampler->setCurrentIndex(sampler->findData("euler_ge"));
      auto *schedule = panel.findChild<QComboBox *>("Diffusion_schedule");
      schedule->setCurrentIndex(schedule->findData("custom"));
      panel.findChild<QPlainTextEdit *>("DiffusionSigmas")
          ->setPlainText("1.0, 0.5, 0.0");
      panel.saveSettings();
    }
    DiffusionPanel restored;
    QVERIFY(restored.workerExecutable().isEmpty());
    QVERIFY(!QSettings().contains("diffusion/workerExecutable"));
    QCOMPARE(restored.request()["sampler"].toString(), QString("euler_ge"));
    QCOMPARE(restored.request()["schedule"].toString(), QString("custom"));
    QCOMPARE(restored.request()["sigmas"].toArray().size(), 3);
    restored.setSamplers({"euler", "euler_ge"}, {"discrete"});
    QCOMPARE(restored.request()["sampler"].toString(), QString("euler_ge"));
    QCOMPARE(
        restored.configuration()["files"].toObject()["diffusion"].toString(),
        QString("/models/a.gguf"));
    QCOMPARE(restored.configuration()["settings"]
                 .toObject()["maxVramGiB"]
                 .toDouble(),
             7.5);
    restored.setDevices(
        {QJsonObject{{"id", "CUDA0"}, {"name", "Available GPU"}}});
    QCOMPARE(
        restored.configuration()["settings"].toObject()["device"].toString(),
        QString("CUDA7"));
    QCOMPARE(restored.configuration()["settings"]
                 .toObject()["placements"]
                 .toObject()["vae"]
                 .toString(),
             QString("disk"));
  }

private:
  QTemporaryDir settings_;
};
QTEST_MAIN(DiffusionPanelTests)
#include "DiffusionPanelTests.moc"
