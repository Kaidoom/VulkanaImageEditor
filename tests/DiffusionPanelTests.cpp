#include "imageeditor/diffusion/ModelProfiles.h"
#include "imageeditor/ui/DiffusionPanel.hpp"
#include "imageeditor/ui/DiffusionPreview.hpp"
#include "imageeditor/ui/Theme.hpp"
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
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
