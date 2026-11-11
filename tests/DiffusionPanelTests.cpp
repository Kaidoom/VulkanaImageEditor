#include "imageeditor/diffusion/ModelProfiles.h"
#include "imageeditor/diffusion/Resolution.h"
#include "imageeditor/ui/DiffusionPanel.hpp"
#include "imageeditor/ui/DiffusionPreview.hpp"
#include "imageeditor/ui/Theme.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
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
#include <QSignalSpy>
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
  void familyAndVariantConfigurationRoundTrip() {
    DiffusionPanel panel;
    auto *family = panel.findChild<QComboBox *>("DiffusionFamily");
    auto *variant = panel.findChild<QComboBox *>("DiffusionProfile");
    QCOMPARE(family->count(), 4);
    QCOMPARE(family->currentText(), QString("Qwen 2.1 Image"));
    QCOMPARE(variant->count(), 2);
    QCOMPARE(variant->currentText(), QString("Turbo"));
    family->setCurrentIndex(family->findData("flux-klein"));
    QCOMPARE(variant->count(), 4);
    QCOMPARE(variant->itemText(0), QString("Flux 2 base"));
    QCOMPARE(variant->itemText(1), QString("Flux 2 distilled"));
    QCOMPARE(variant->itemText(2), QString("Flux 3 base"));
    QCOMPARE(variant->itemText(3), QString("Flux 3 distilled"));
    variant->setCurrentIndex(variant->findData("flux2-klein-distilled"));
    QCOMPARE(panel.request()["steps"].toInt(), 4);
    QCOMPARE(panel.request()["scheduler"].toString(), QString("flux2"));
    QVERIFY(!panel.supportsRegions());
    variant->setCurrentIndex(variant->findData("flux3-image-klein-base"));
    QCOMPARE(panel.request()["guidance"].toDouble(), 3.5);
    QCOMPARE(panel.request()["steps"].toInt(), 20);
    QVERIFY(panel.supportsRegions());
    // Existing profile IDs remain the persisted identity, independent of grouping.
    for (const auto &value : imageeditor::diffusion::modelProfiles()) {
      const auto p = value.toObject();
      panel.setConfiguration({{"profile", p["id"]}});
      QCOMPARE(panel.configuration()["profile"], p["id"]);
      QCOMPARE(variant->currentData().toString(), p["id"].toString());
      const auto familyKey = p.value("familyGroup").toString(p.value("family").toString());
      QCOMPARE(family->currentData().toString(), familyKey);
      QCOMPARE(variant->isHidden(), variant->count() == 1);
      auto *steps = panel.findChild<QDoubleSpinBox *>("Diffusion_steps");
      steps->setValue(31);
      panel.setProfiles(imageeditor::diffusion::modelProfiles());
      QCOMPARE(steps->value(), 31.0); // Worker discovery preserves edited defaults.
      panel.saveSettings();
      DiffusionPanel restored;
      QCOMPARE(restored.configuration()["profile"], p["id"]);
      QCOMPARE(restored.request()["steps"].toInt(), 31);
    }
  }
  void checkpointConfigurationUsesEmbeddedComponents() {
    DiffusionPanel panel;
    QImage input(64, 64, QImage::Format_RGBA8888); input.fill(Qt::red);
    panel.setInputs({{input, "Reference", "Test"}});
    panel.setMode(DiffusionPanel::Mode::Fill);
    const QJsonArray loras{QJsonObject{{"id", "one"}, {"path", "/models/style.safetensors"},
      {"name", "Style"}, {"strength", .7}, {"enabled", true}},
      QJsonObject{{"id", "two"}, {"path", "/models/lighting.safetensors"},
      {"name", "Lighting"}, {"strength", .5}, {"enabled", false}}};
    panel.setConfiguration({{"profile", "stable-diffusion"},
      {"files", QJsonObject{{"diffusion", "/models/checkpoint.safetensors"}}}, {"loras", loras}});
    QVERIFY(panel.findChild<QPushButton *>("DiffusionGenerate")->isEnabled());
    QCOMPARE(panel.mode(), DiffusionPanel::Mode::Generate);
    QVERIFY(!panel.supportsMode(DiffusionPanel::Mode::Edit));
    QVERIFY(!panel.supportsMode(DiffusionPanel::Mode::Fill));
    QVERIFY(!panel.supportsImageInputs());
    QVERIFY(!panel.supportsRegions());
    QVERIFY(panel.findChild<QLineEdit *>("DiffusionFile_textEncoder")->parentWidget()->isHidden());
    QVERIFY(panel.findChild<QLineEdit *>("DiffusionFile_vision")->parentWidget()->isHidden());
    QVERIFY(!panel.findChild<QLineEdit *>("DiffusionFile_vae")->parentWidget()->isHidden());
    QVERIFY(panel.findChild<QComboBox *>("DiffusionProfile")->isHidden());
    QCOMPARE(panel.configuration()["files"].toObject().size(), 1);
    panel.findChild<QPlainTextEdit *>("DiffusionPrompt")->setPlainText("A teapot");
    panel.findChild<QPlainTextEdit *>("DiffusionNegativePrompt")->setPlainText("blurry");
    const auto request = panel.prepareGeneration();
    QCOMPARE(request["steps"].toInt(), 25);
    QCOMPARE(request["guidance"].toDouble(), 7.0);
    QCOMPARE(request["sampler"].toString(), QString("dpm++2m"));
    QCOMPARE(request["scheduler"].toString(), QString("karras"));
    QCOMPARE(request["negativePrompt"].toString(), QString("blurry"));
    QVERIFY(!request.contains("strength"));
    QVERIFY(!request.contains("flowShift"));
    QVERIFY(!request.contains("referenceMaxSize"));
    QVERIFY(panel.spatialParameters().isEmpty());
    QCOMPARE(request["loras"].toArray().size(), 1);
    QCOMPARE(panel.configuration()["loras"].toArray(), loras);
    panel.findChild<QLineEdit *>("DiffusionFile_vae")->setText("/models/vae.safetensors");
    panel.saveSettings();
    DiffusionPanel restored;
    QCOMPARE(restored.configuration(), panel.configuration());
    QCOMPARE(restored.request()["negativePrompt"].toString(), QString("blurry"));
    if (const auto capture = qEnvironmentVariable("VULKANA_DIFFUSION_PANEL_CAPTURE"); !capture.isEmpty()) {
      panel.resize(520, 900); panel.show(); QCoreApplication::processEvents();
      QVERIFY(panel.grab().save(capture + "-checkpoint.png"));
    }
  }
  void flux2IgnoresStoredRegionGuides() {
    DiffusionPanel panel;
    panel.setConfiguration({{"profile", "flux3-image-klein-distilled"}});
    QImage image(64, 64, QImage::Format_RGBA8888); image.fill(Qt::blue);
    panel.setInputs({{image, "Source", "Test"}});
    QVector<imageeditor::ui::DiffusionRegion> regions;
    for (int i = 0; i < 10; ++i)
      regions.append({QRectF(.1, .1, .2, .2), QString("Region%1").arg(i + 1)});
    QVERIFY(panel.setInputRegions(panel.inputs().front().id, regions));
    panel.setConfiguration({{"profile", "flux2-klein-distilled"}});
    QVERIFY(panel.addInput({image, "Reference", "Test"}));
    QVERIFY(panel.spatialParameters().isEmpty());
    QCOMPARE(panel.inputs().front().regions.size(), 10);
    panel.findChild<QPlainTextEdit *>("DiffusionPrompt")->setPlainText("A test image");
    panel.prepareGeneration();
  }
  void loraDefaultsAndConfigurationScopedOverrides() {
    const QJsonArray defaultsA{
        QJsonObject{{"id", "first"}, {"path", "/models/a.safetensors"}, {"name", "Mythic"}, {"strength", .8}, {"enabled", true}},
        QJsonObject{{"id", "second"}, {"path", "/models/b.safetensors"}, {"name", "Lighting"}, {"strength", .5}, {"enabled", false}}};
    const QJsonArray defaultsB{
        QJsonObject{{"id", "first"}, {"path", "/models/c.safetensors"}, {"name", "Different model"}, {"strength", .3}, {"enabled", true}}};
    const QJsonArray configurations{
        QJsonObject{{"id", "A"}, {"name", "A"}, {"configuration", QJsonObject{{"profile", "flux3-image-klein-distilled"}, {"loras", defaultsA}}}},
        QJsonObject{{"id", "B"}, {"name", "B"}, {"configuration", QJsonObject{{"profile", "qwen-image-2.1-turbo"}, {"loras", defaultsB}}}}};
    QSettings().setValue("diffusion/modelConfigurations", QJsonDocument(configurations).toJson());
    QSettings().setValue("diffusion/modelId", "A");
    DiffusionPanel panel;
    auto *models = panel.findChild<QComboBox *>("DiffusionModels");
    auto *header = panel.findChild<QToolButton *>("DiffusionLorasSectionHeader");
    QVERIFY(!header->isChecked());
    QCOMPARE(header->text(), QString("LoRAs (1/2 enabled)"));
    QCOMPARE(panel.configuration()["loras"].toArray(), defaultsA);
    auto checkboxes = panel.findChildren<QCheckBox *>("DiffusionLoraEnabled");
    auto strengths = panel.findChildren<QDoubleSpinBox *>("DiffusionLoraStrength");
    checkboxes[0]->setChecked(false);
    checkboxes[1]->setChecked(true);
    strengths[1]->setValue(-.4);
    QCOMPARE(panel.configuration()["loras"].toArray(), defaultsA);
    for (auto mode : {DiffusionPanel::Mode::Edit, DiffusionPanel::Mode::Fill, DiffusionPanel::Mode::Generate}) {
      panel.setMode(mode);
      const auto enabled = panel.request()["loras"].toArray();
      QCOMPARE(enabled.size(), 1);
      QCOMPARE(enabled[0].toObject()["name"].toString(), QString("Lighting"));
      QCOMPARE(enabled[0].toObject()["strength"].toDouble(), -.4);
    }
    models->setCurrentIndex(models->findData("B"));
    QCOMPARE(panel.request()["loras"].toArray()[0].toObject()["strength"].toDouble(), .3);
    panel.findChild<QDoubleSpinBox *>("DiffusionLoraStrength")->setValue(1.4);
    models->setCurrentIndex(models->findData("A"));
    QCOMPARE(panel.request()["loras"].toArray()[0].toObject()["strength"].toDouble(), -.4);
    panel.findChild<QToolButton *>("DiffusionLorasReset")->click();
    QTRY_COMPARE(panel.request()["loras"].toArray()[0].toObject()["strength"].toDouble(), .8);
    models->setCurrentIndex(models->findData("B"));
    QCOMPARE(panel.request()["loras"].toArray()[0].toObject()["strength"].toDouble(), 1.4);
    panel.saveSettings();
    const auto saved = QJsonDocument::fromJson(QSettings().value("diffusion/modelConfigurations").toByteArray()).array();
    QCOMPARE(saved.size(), 2);
    QCOMPARE(saved[0].toObject()["configuration"].toObject()["loras"].toArray(), defaultsA);
    QCOMPARE(saved[1].toObject()["configuration"].toObject()["loras"].toArray(), defaultsB);
    QVERIFY(!saved[1].toObject()["generation"].toObject().contains("loras"));
    DiffusionPanel restored;
    QCOMPARE(restored.configuration()["loras"].toArray(), defaultsB);
    QCOMPARE(restored.request()["loras"].toArray()[0].toObject()["strength"].toDouble(), .3);
    models->setCurrentIndex(0);
    QVERIFY(panel.findChild<QWidget *>("DiffusionLorasSection")->isHidden());
    QVERIFY(!panel.request().contains("loras"));
    panel.findChild<QToolButton *>("DiffusionLoraAdd")->click();
    QTRY_COMPARE(panel.findChildren<QLineEdit *>("DiffusionLoraPath").size(), 1);
    panel.findChild<QLineEdit *>("DiffusionLoraPath")->setText("/models/new-style.safetensors");
    QCOMPARE(panel.configuration()["loras"].toArray()[0].toObject()["name"].toString(), QString("new-style"));
    panel.findChild<QCheckBox *>("DiffusionLoraDefaultEnabled")->setChecked(false);
    QVERIFY(!panel.request().contains("loras"));
    panel.findChild<QCheckBox *>("DiffusionLoraEnabled")->setChecked(true);
    QVERIFY(panel.request().contains("loras"));
    auto *generation = panel.findChild<QWidget *>("DiffusionLorasSection");
    QVERIFY(!generation->findChild<QLineEdit *>("DiffusionLoraPath"));
    QVERIFY(!generation->findChild<QLineEdit *>("DiffusionLoraFriendlyName"));
    QVERIFY(!generation->findChild<QToolButton *>("DiffusionLoraAdd"));
    panel.findChild<QToolButton *>("DiffusionLoraRemove")->click();
    QTRY_VERIFY(generation->isHidden());
    QVERIFY(!panel.request().contains("loras"));
  }
  void modeSpecificControls_data() {
    QTest::addColumn<QString>("profile");
    QTest::newRow("flux3") << QString("flux3-image-klein-distilled");
    QTest::newRow("qwen") << QString("qwen-image-2.1-turbo");
  }
  void generationOverridesStaySeparateAndFollowConfiguration() {
    const QJsonArray exposed{"steps", "guidance", "schedule", "batch"};
    QSettings().setValue("diffusion/modelConfigurations", QJsonDocument(QJsonArray{
      QJsonObject{{"id", "A"}, {"name", "Flux"},
        {"configuration", QJsonObject{{"profile", "flux3-image-klein-distilled"}}},
        {"generation", QJsonObject{{"steps", 16}}}, {"exposedGeneration", exposed}},
      QJsonObject{{"id", "B"}, {"name", "Qwen"},
        {"configuration", QJsonObject{{"profile", "qwen-image-2.1-turbo"}}},
        {"generation", QJsonObject{{"steps", 8}}}, {"exposedGeneration", exposed}}
    }).toJson());
    QSettings().setValue("diffusion/modelId", "A");
    DiffusionPanel panel;
    auto *models = panel.findChild<QComboBox *>("DiffusionModels");
    auto *steps = panel.findChild<QDoubleSpinBox *>("DiffusionOverride_steps");
    auto *baseline = panel.findChild<QDoubleSpinBox *>("Diffusion_steps");
    auto *reset = panel.findChild<QToolButton *>("DiffusionOverrideReset_steps");
    auto *expose = panel.findChild<QToolButton *>("DiffusionExpose_steps");
    QVERIFY(!panel.findChild<QToolButton *>("DiffusionOverridesSectionHeader")->isChecked());
    QVERIFY(expose->isChecked());
    QCOMPARE(steps->value(), 16.);
    QVERIFY(!reset->isEnabled());
    steps->setValue(9);
    QCOMPARE(panel.request()["steps"].toInt(), 9);
    QCOMPARE(baseline->value(), 16.);
    QVERIFY(reset->isEnabled());
    for (auto mode : {DiffusionPanel::Mode::Edit, DiffusionPanel::Mode::Fill, DiffusionPanel::Mode::Generate}) {
      panel.setMode(mode);
      QCOMPARE(panel.request()["steps"].toInt(), 9);
    }
    models->setCurrentIndex(models->findData("B"));
    QCOMPARE(steps->value(), 8.);
    steps->setValue(12);
    auto *schedule = panel.findChild<QComboBox *>("DiffusionOverride_schedule");
    schedule->setCurrentIndex(schedule->findData("auto"));
    QCOMPARE(panel.request()["schedule"].toString(), QString("auto"));
    QCOMPARE(panel.findChild<QComboBox *>("Diffusion_schedule")->currentData().toString(), QString("turbo"));
    models->setCurrentIndex(models->findData("A"));
    QCOMPARE(steps->value(), 9.);
    baseline->setValue(20);
    QCOMPARE(steps->value(), 9.);
    reset->click();
    QCOMPARE(steps->value(), 20.);
    QVERIFY(!reset->isEnabled());
    steps->setValue(10);
    panel.saveSettings();
    const auto entries = QJsonDocument::fromJson(QSettings().value("diffusion/modelConfigurations").toByteArray()).array();
    QCOMPARE(entries[0].toObject()["generation"].toObject()["steps"].toInt(), 20);
    QCOMPARE(entries[1].toObject()["generation"].toObject()["steps"].toInt(), 8);
    QCOMPARE(entries[1].toObject()["generation"].toObject()["schedule"].toString(), QString("turbo"));
    QVERIFY(entries[0].toObject()["exposedGeneration"].toArray().contains("steps"));
    {
      DiffusionPanel restored;
      QCOMPARE(restored.request()["steps"].toInt(), 20); // Session overrides are not saved defaults.
      QVERIFY(restored.findChild<QToolButton *>("DiffusionExpose_steps")->isChecked());
    }
    models->setCurrentIndex(models->findData("B"));
    QCOMPARE(steps->value(), 12.);
    QCOMPARE(panel.request()["schedule"].toString(), QString("auto"));
    models->setCurrentIndex(models->findData("A"));
    QCOMPARE(steps->value(), 10.);
    expose->setChecked(false);
    QCOMPARE(panel.request()["steps"].toInt(), 20);
    expose->setChecked(true);
    QCOMPARE(steps->value(), 20.);
  }
  void generationOverridesRespectDependenciesAndResetIndividually() {
    DiffusionPanel panel;
    panel.setConfiguration({{"profile", "flux3-image-klein-distilled"}});
    auto *section = panel.findChild<QWidget *>("DiffusionOverridesSection");
    QVERIFY(section->isHidden());
    for (const auto *key : {"steps", "guidance", "sampler", "eta", "schedule", "scheduler", "sigmas",
                           "flowShift", "cacheMode", "cacheReuseThreshold", "vaeTiling", "vaeTileSize", "preview", "previewInterval"})
      panel.findChild<QToolButton *>(QString("DiffusionExpose_") + key)->setChecked(true);
    QVERIFY(!section->isHidden());
    auto *cfg = panel.findChild<QDoubleSpinBox *>("DiffusionOverride_guidance");
    auto *negative = panel.findChild<QPlainTextEdit *>("DiffusionNegativePrompt");
    QVERIFY(negative);
    QVERIFY(!negative->isEnabled());
    cfg->setValue(4);
    QVERIFY(negative->isEnabled());
    negative->setPlainText("blur");
    QCOMPARE(panel.request()["negativePrompt"].toString(), QString("blur"));
    QCOMPARE(panel.findChild<QDoubleSpinBox *>("Diffusion_guidance")->value(), 1.);
    auto *sampler = panel.findChild<QComboBox *>("DiffusionOverride_sampler");
    sampler->setCurrentIndex(sampler->findData("euler_a"));
    QVERIFY(panel.findChild<QDoubleSpinBox *>("DiffusionOverride_eta")->isEnabled());
    QVERIFY(!panel.findChild<QDoubleSpinBox *>("Diffusion_eta")->isEnabled());
    panel.findChild<QDoubleSpinBox *>("DiffusionOverride_eta")->setValue(.5);
    QCOMPARE(panel.request()["eta"].toDouble(), .5);
    panel.setSamplers({"euler", "euler_a"}, {"discrete", "karras"});
    QCOMPARE(sampler->currentData().toString(), QString("euler_a"));
    auto *schedule = panel.findChild<QComboBox *>("DiffusionOverride_schedule");
    schedule->setCurrentIndex(schedule->findData("custom"));
    auto *sigmas = panel.findChild<QPlainTextEdit *>("DiffusionOverride_sigmas");
    QVERIFY(sigmas->isEnabled());
    QVERIFY(!panel.findChild<QPlainTextEdit *>("DiffusionSigmas")->isEnabled());
    sigmas->setPlainText("1.0, 0.5, 0.0");
    QCOMPARE(panel.request()["sigmas"].toArray(), QJsonArray({1., .5, 0.}));
    QVERIFY(!panel.findChild<QComboBox *>("DiffusionOverride_scheduler")->isEnabled());
    auto *cache = panel.findChild<QComboBox *>("DiffusionOverride_cacheMode");
    cache->setCurrentIndex(cache->findData("easycache"));
    QVERIFY(panel.findChild<QDoubleSpinBox *>("DiffusionOverride_cacheReuseThreshold")->isEnabled());
    QVERIFY(!panel.findChild<QDoubleSpinBox *>("Diffusion_cacheReuseThreshold")->isEnabled());
    panel.findChild<QCheckBox *>("DiffusionOverride_vaeTiling")->setChecked(true);
    QVERIFY(panel.findChild<QDoubleSpinBox *>("DiffusionOverride_vaeTileSize")->isEnabled());
    panel.findChild<QCheckBox *>("DiffusionOverride_preview")->setChecked(false);
    QVERIFY(!panel.findChild<QDoubleSpinBox *>("DiffusionOverride_previewInterval")->isEnabled());
    panel.findChild<QToolButton *>("DiffusionOverrideReset_guidance")->click();
    QVERIFY(!negative->isEnabled());
    QVERIFY(!panel.request().contains("negativePrompt"));
    QCOMPARE(panel.request()["eta"].toDouble(), .5);
    panel.setBusy(true);
    QVERIFY(!section->isEnabled());
    panel.setBusy(false);
    QVERIFY(section->isEnabled());
    panel.setConfiguration({{"profile", "stable-diffusion"}});
    QVERIFY(!panel.findChild<QDoubleSpinBox *>("DiffusionOverride_flowShift")->isEnabled());
  }
  void generationOnlyCapabilitiesKeepExistingInputs() {
    DiffusionPanel panel;
    panel.setConfiguration({{"profile", "flux3-image-klein-distilled"}});
    QImage image(80, 64, QImage::Format_RGBA8888);
    image.fill(Qt::blue);
    QVERIFY(panel.addInput({image, "Reference", "Test"}));
    const auto inputId = panel.inputs().front().id;
    panel.setRegions({{QRectF(.1, .1, .4, .4), "Region1"}});
    panel.setMode(DiffusionPanel::Mode::Fill);
    panel.setConfiguration({{"profile", "krea-2-turbo"},
      {"files", QJsonObject{{"diffusion", "/models/d.gguf"}, {"textEncoder", "/models/t.gguf"}, {"vae", "/models/v.safetensors"}}}});
    QCOMPARE(panel.mode(), DiffusionPanel::Mode::Generate);
    QVERIFY(panel.supportsMode(DiffusionPanel::Mode::Generate));
    QVERIFY(!panel.supportsMode(DiffusionPanel::Mode::Edit));
    QVERIFY(!panel.supportsMode(DiffusionPanel::Mode::Fill));
    QVERIFY(!panel.supportsImageInputs());
    QVERIFY(!panel.supportsRegions());
    QVERIFY(!panel.addInput({image, "Ignored", "Test"}));
    QVERIFY(!panel.findChild<QListWidget *>("DiffusionInputsList")->isEnabled());
    QVERIFY(!panel.findChild<QPushButton *>("DiffusionRegionsButton")->isEnabled());
    QVERIFY(!panel.findChild<QDoubleSpinBox *>("Diffusion_strength")->isEnabled());
    auto *modes = panel.findChild<QComboBox *>("DiffusionMode");
    QVERIFY(!(modes->model()->flags(modes->model()->index(1, 0)) & Qt::ItemIsEnabled));
    QVERIFY(!(modes->model()->flags(modes->model()->index(2, 0)) & Qt::ItemIsEnabled));
    panel.setMode(DiffusionPanel::Mode::Edit);
    QCOMPARE(panel.mode(), DiffusionPanel::Mode::Generate);
    QCOMPARE(panel.inputs().front().id, inputId);
    QVERIFY(panel.spatialParameters().isEmpty());
    const auto request = panel.prepareGeneration();
    QCOMPARE(request["steps"].toInt(), 8);
    QCOMPARE(request["guidance"].toDouble(), 1.0);
    QCOMPARE(request["sampler"].toString(), QString("euler"));
    QCOMPARE(request["scheduler"].toString(), QString("simple"));
    QCOMPARE(request["schedule"].toString(), QString("auto"));
    QVERIFY(!request.contains("strength"));
    QVERIFY(!request.contains("referenceMaxSize"));
    QVERIFY(panel.findChild<QPushButton *>("DiffusionGenerate")->isEnabled());
    if (const auto capture = qEnvironmentVariable("VULKANA_DIFFUSION_PANEL_CAPTURE"); !capture.isEmpty()) {
      panel.resize(520, 900);
      panel.showGeneration();
      panel.show();
      QCoreApplication::processEvents();
      QVERIFY(panel.grab().save(capture + "-generation-only.png"));
    }
    panel.findChild<QDoubleSpinBox *>("Diffusion_steps")->setValue(12);
    panel.saveSettings();
    DiffusionPanel restored;
    QCOMPARE(restored.configuration()["profile"].toString(), QString("krea-2-turbo"));
    QCOMPARE(restored.request()["steps"].toInt(), 12);
    QVERIFY(!restored.supportsImageInputs());
    panel.setConfiguration({{"profile", "flux3-image-klein-distilled"}});
    QVERIFY(panel.supportsImageInputs());
    QCOMPARE(panel.inputs().front().id, inputId);
    QCOMPARE(panel.inputs().front().image, image);
    QCOMPARE(panel.regions().size(), 1);
    panel.setMode(DiffusionPanel::Mode::Edit);
    QCOMPARE(panel.mode(), DiffusionPanel::Mode::Edit);
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
    auto *negativeHeader = panel.findChild<QToolButton *>("DiffusionNegativeSectionHeader");
    QVERIFY(denoise && strength && negative);
    QVERIFY(negativeHeader && !negativeHeader->isChecked());
    QVERIFY(tabs->widget(1)->isAncestorOf(negativeHeader));
    QVERIFY(negativeHeader->toolTip().contains("CFG"));
    negativeHeader->click();
    QVERIFY(negativeHeader->isChecked());
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
    auto *seed = panel.findChild<QDoubleSpinBox *>("DiffusionSeed");
    auto *policy = panel.findChild<QComboBox *>("DiffusionSeedMode");
    QVERIFY(policy);
    policy->setCurrentIndex(policy->findData("random"));
    const auto randomA = panel.prepareGeneration()["seed"].toDouble();
    const auto randomB = panel.prepareGeneration()["seed"].toDouble();
    QVERIFY(randomA != randomB);
    QVERIFY(!seed->isEnabled());
    policy->setCurrentIndex(policy->findData("fixed"));
    seed->setValue(42);
    QCOMPARE(panel.prepareGeneration()["seed"].toInt(), 42);
    panel.generationAccepted();
    QCOMPARE(panel.prepareGeneration()["seed"].toInt(), 42);
    auto *randomSeed = panel.findChild<QToolButton *>("DiffusionRandomSeed");
    QVERIFY(randomSeed);
    randomSeed->click();
    QVERIFY(seed->text() != "42");
    QCOMPARE(policy->currentData().toString(), QString("fixed"));
    QCOMPARE(panel.prepareGeneration()["seed"].toDouble(), seed->text().toDouble());
    seed->setValue(42);
    policy->setCurrentIndex(policy->findData("increment"));
    panel.findChild<QDoubleSpinBox *>("Diffusion_batch")->setValue(2);
    panel.generationAccepted();
    QCOMPARE(panel.prepareGeneration()["seed"].toInt(), 44);
    policy->setCurrentIndex(policy->findData("fixed"));
    seed->setValue(4294967295.0);
    QTest::keyClick(seed, Qt::Key_Up);
    QCOMPARE(panel.prepareGeneration()["seed"].toVariant().toLongLong(), 4294967296LL);
    wheel(seed);
    QCOMPARE(seed->value(), 4294967296.0); // Scrolling the panel never edits the seed.
    seed->setValue(9007199254740990.0);
    QTest::keyClick(seed, Qt::Key_Up);
    QCOMPARE(seed->text(), QString("9007199254740991"));
    QTest::keyClick(seed, Qt::Key_Up);
    QCOMPARE(panel.prepareGeneration()["seed"].toVariant().toLongLong(), 9007199254740991LL);
    panel.saveSettings();
    {
      DiffusionPanel restored;
      QCOMPARE(restored.generationParameters()["seed"].toVariant().toLongLong(), 9007199254740991LL);
    }
    policy->setCurrentIndex(policy->findData("increment"));
    panel.generationAccepted(); // Batch of two wraps within the supported range.
    QCOMPARE(seed->value(), 1.0);
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
  void resolutionSelectorSynchronization() {
    DiffusionPanel panel;
    auto *width = panel.findChild<QSpinBox *>("DiffusionWidth");
    auto *height = panel.findChild<QSpinBox *>("DiffusionHeight");
    auto *ratio = panel.findChild<QComboBox *>("DiffusionAspectRatio");
    auto *mp = panel.findChild<QComboBox *>("DiffusionMegapixels");
    auto *multiple = panel.findChild<QComboBox *>("DiffusionResolutionMultiple");
    auto *header = panel.findChild<QToolButton *>("DiffusionResolutionSectionHeader");
    const auto size = [&] { return QSize(width->value(), height->value()); };
    const auto choose = [&](QSize preset) {
      ratio->setCurrentIndex(ratio->findData(preset));
      ratio->activated(ratio->currentIndex());
    };
    QVERIFY(!header->isChecked());
    QCOMPARE(size(), QSize(1024, 1024));
    QCOMPARE(ratio->currentData().toSize(), QSize(1, 1));
    QCOMPARE(mp->currentData().toDouble(), 1.0);
    QCOMPARE(multiple->currentData().toInt(), 0);
    QVERIFY(multiple->currentText().startsWith("Auto"));
    multiple->setCurrentIndex(multiple->findData(8));
    header->click();
    QCOMPARE(size(), QSize(1024, 1024));
    QSignalSpy widthChanges(width, &QSpinBox::valueChanged);
    QSignalSpy heightChanges(height, &QSpinBox::valueChanged);
    choose({16, 9});
    QCOMPARE(size(), QSize(1368, 768));
    QCOMPARE(widthChanges.count(), 1);
    QCOMPARE(heightChanges.count(), 1);
    QCOMPARE(panel.request()["width"].toInt(), 1368);
    choose({1, 1});
    mp->setCurrentIndex(mp->findData(2.0));
    QCOMPARE(size(), QSize(1448, 1448));
    multiple->setCurrentIndex(multiple->findData(64));
    QCOMPARE(size(), QSize(1472, 1472));
    width->setValue(1504);
    width->setValue(1472);
    QCOMPARE(size(), QSize(1472, 1472));
    QCOMPARE(ratio->currentText(), QString("Custom"));
    mp->setCurrentIndex(mp->findData(1.0));
    multiple->setCurrentIndex(multiple->findData(8));
    QCOMPARE(size(), QSize(1472, 1472));
    choose({1, 1});
    QCOMPARE(size(), QSize(1024, 1024));
    choose({});
    QCOMPARE(size(), QSize(1024, 1024));
    panel.setMode(DiffusionPanel::Mode::Edit);
    QCOMPARE(ratio->currentText(), QString("Custom"));
    panel.setResolution({1024, 1536});
    QCOMPARE(size(), QSize(1024, 1536));
    QCOMPARE(ratio->currentData().toSize(), QSize(2, 3));
    header->click();
    header->click();
    QCOMPARE(size(), QSize(1024, 1536));
    panel.setResolution({1376, 768}); // Within 1% of 16:9 after rounding.
    QCOMPARE(ratio->currentData().toSize(), QSize(16, 9));
    panel.setResolution({2016, 1312});
    QCOMPARE(ratio->currentText(), QString("Custom"));
    panel.saveSettings();
    DiffusionPanel restored;
    QCOMPARE(restored.request()["width"].toInt(), 2016);
    QCOMPARE(restored.request()["height"].toInt(), 1312);
    QCOMPARE(restored.findChild<QComboBox *>("DiffusionAspectRatio")->currentText(), QString("Custom"));
  }
  void resolutionSelectorDetectsStoredSizeWithoutRecalculating() {
    QSettings().setValue("diffusion/width", 1536);
    QSettings().setValue("diffusion/height", 1024);
    DiffusionPanel panel;
    QCOMPARE(panel.request()["width"].toInt(), 1536);
    QCOMPARE(panel.request()["height"].toInt(), 1024);
    QCOMPARE(panel.findChild<QComboBox *>("DiffusionAspectRatio")->currentData().toSize(), QSize(3, 2));
    panel.findChild<QToolButton *>("DiffusionResolutionSectionHeader")->click();
    QCOMPARE(panel.request()["width"].toInt(), 1536);
    QCOMPARE(panel.request()["height"].toInt(), 1024);
  }
  void automaticResolutionUsesSelectedModel() {
    QJsonArray configs;
    for (const auto &profile : imageeditor::diffusion::modelProfiles()) {
      const auto id = profile.toObject().value("id");
      configs.append(QJsonObject{{"id", id}, {"name", id},
          {"configuration", QJsonObject{{"profile", id}}}});
    }
    QSettings().setValue("diffusion/modelConfigurations", QJsonDocument(configs).toJson());
    QSettings().setValue("diffusion/modelId", "krea-2-turbo");
    DiffusionPanel panel;
    auto *models = panel.findChild<QComboBox *>("DiffusionModels");
    auto *profiles = panel.findChild<QComboBox *>("DiffusionProfile");
    auto *families = panel.findChild<QComboBox *>("DiffusionFamily");
    auto *multiple = panel.findChild<QComboBox *>("DiffusionResolutionMultiple");
    auto *ratio = panel.findChild<QComboBox *>("DiffusionAspectRatio");
    auto *width = panel.findChild<QSpinBox *>("DiffusionWidth");
    auto *height = panel.findChild<QSpinBox *>("DiffusionHeight");
    const auto size = [&] { return QSize(width->value(), height->value()); };
    const auto portrait = [&] {
      ratio->setCurrentIndex(ratio->findData(QSize(2, 3)));
      ratio->activated(ratio->currentIndex());
    };
    QCOMPARE(multiple->currentData().toInt(), 0);
    QCOMPARE(multiple->currentText(), QString("Auto (8)"));
    portrait();
    QCOMPARE(size(), QSize(840, 1256));
    models->setCurrentIndex(models->findData("flux3-image-klein-distilled"));
    QCOMPARE(multiple->currentText(), QString("Auto (16)"));
    QCOMPARE(size(), QSize(832, 1248));
    models->setCurrentIndex(models->findData("qwen-image-2.1-turbo"));
    QCOMPARE(multiple->currentText(), QString("Auto (32)"));
    QCOMPARE(size(), QSize(832, 1248));
    multiple->setCurrentIndex(multiple->findData(8));
    models->setCurrentIndex(models->findData("flux3-image-klein-distilled"));
    QCOMPARE(multiple->currentData().toInt(), 8);
    QCOMPARE(size(), QSize(840, 1256));
    multiple->setCurrentIndex(multiple->findData(0));
    QCOMPARE(size(), QSize(832, 1248));
    width->setValue(841);
    models->setCurrentIndex(models->findData("krea-2-turbo"));
    QCOMPARE(size(), QSize(841, 1248));
    QCOMPARE(ratio->currentText(), QString("Custom"));
    families->setCurrentIndex(families->findData("qwen-image-2.1"));
    profiles->setCurrentIndex(profiles->findData("qwen-image-2.1-turbo"));
    QCOMPARE(size(), QSize(841, 1248));
    portrait();
    QCOMPARE(size(), QSize(832, 1248));
    families->setCurrentIndex(families->findData("krea2"));
    QCOMPARE(size(), QSize(840, 1256));
    panel.setResolution({512, 768});
    panel.setProfiles(imageeditor::diffusion::modelProfiles());
    panel.setMode(DiffusionPanel::Mode::Generate);
    QCOMPARE(size(), QSize(512, 768)); // No recalculation on handshake or mode refresh.
  }
  void resolutionErrorsSuggestNearbySizes() {
    using imageeditor::diffusion::resolutionError;
    QCOMPARE(QString::fromStdString(resolutionError(840, 1260, 8, "Krea 2 Turbo")),
        QString("Invalid resolution: 840 × 1260. Krea 2 Turbo requires dimensions divisible by 8. Try 840 × 1256 or 840 × 1264."));
    QVERIFY(resolutionError(840, 1256, 8, "Krea 2 Turbo").empty());
    QCOMPARE(QString::fromStdString(resolutionError(33, 8191, 32, "Qwen Image 2.1")),
        QString("Invalid resolution: 33 × 8191. Qwen Image 2.1 requires dimensions divisible by 32. Try 32 × 8160 or 64 × 8192."));
  }
  void resolutionRemainsAuthoritativeAcrossCapabilitiesAndModes() {
    for (const auto &value : imageeditor::diffusion::modelProfiles()) {
      const auto profile = value.toObject();
      DiffusionPanel panel;
      panel.setConfiguration({{"profile", profile["id"]}});
      auto *width = panel.findChild<QSpinBox *>("DiffusionWidth");
      auto *height = panel.findChild<QSpinBox *>("DiffusionHeight");
      auto *ratio = panel.findChild<QComboBox *>("DiffusionAspectRatio");
      auto *multiple = panel.findChild<QComboBox *>("DiffusionResolutionMultiple");
      auto *seed = panel.findChild<QDoubleSpinBox *>("DiffusionSeed");
      multiple->setCurrentIndex(multiple->findData(8));
      ratio->setCurrentIndex(ratio->findData(QSize(2, 3)));
      ratio->activated(ratio->currentIndex());
      QCOMPARE(QSize(width->value(), height->value()), QSize(840, 1256));
      if (profile["family"] == "krea2") {
        QCOMPARE(panel.prepareGeneration()["width"].toInt(), 840);
        QCOMPARE(panel.prepareGeneration()["height"].toInt(), 1256);
      }
      width->setValue(841);
      height->setValue(1257);
      const auto previousSeed = seed->text();
      QSignalSpy widthChanges(width, &QSpinBox::valueChanged);
      QSignalSpy heightChanges(height, &QSpinBox::valueChanged);
      // First worker handshake, reconnect, configuration refresh and mode
      // switches all use this path. None may align the user's dimensions.
      panel.setProfiles(imageeditor::diffusion::modelProfiles());
      panel.setConfiguration(panel.configuration());
      for (auto mode : {DiffusionPanel::Mode::Generate, DiffusionPanel::Mode::Edit, DiffusionPanel::Mode::Fill}) {
        panel.setMode(mode);
        try {
          panel.prepareGeneration();
          QFAIL("An incompatible resolution must be rejected before generation");
        } catch (const std::invalid_argument &e) {
          const auto message = QString::fromUtf8(e.what());
          QVERIFY(message.contains(profile["name"].toString()));
          QVERIFY(message.contains(QString("divisible by %1").arg(profile["alignment"].toInt())));
          QVERIFY(message.contains("841 × 1257"));
          QVERIFY(message.contains("Try "));
        }
        QCOMPARE(seed->text(), previousSeed);
        QCOMPARE(QSize(width->value(), height->value()), QSize(841, 1257));
        QCOMPARE(ratio->currentText(), QString("Custom"));
      }
      QCOMPARE(widthChanges.count(), 0);
      QCOMPARE(heightChanges.count(), 0);
      panel.saveSettings();
      DiffusionPanel restored;
      QCOMPARE(restored.request()["width"].toInt(), 841);
      QCOMPARE(restored.request()["height"].toInt(), 1257);
      QCOMPARE(restored.findChild<QComboBox *>("DiffusionAspectRatio")->currentData().toSize(), QSize(2, 3));
      multiple->setCurrentIndex(multiple->findData(profile["alignment"].toInt()));
      ratio->setCurrentIndex(ratio->findData(QSize(2, 3)));
      ratio->activated(ratio->currentIndex());
      const QSize expected = profile["family"] == "krea2" ? QSize(840, 1256)
          : profile["family"] == "stable-diffusion" ? QSize(832, 1280) : QSize(832, 1248);
      QCOMPARE(QSize(width->value(), height->value()), expected);
      const auto request = panel.prepareGeneration();
      QCOMPARE(request["width"].toInt(), expected.width());
      QCOMPARE(request["height"].toInt(), expected.height());
      QVERIFY(panel.findChild<QLabel *>("DiffusionResolutionRequirement")->text().contains(profile["name"].toString()));
      QSettings().clear();
    }
  }
  void centeredSizeAndNarrowResolutionLayout() {
    DiffusionPanel panel;
    panel.showGeneration();
    panel.show();
    panel.findChild<QToolButton *>("DiffusionResolutionSectionHeader")->click();
    for (int panelWidth : {520, panel.minimumWidth()}) {
      panel.resize(panelWidth, 900);
      QCoreApplication::processEvents();
      QCOMPARE(panel.width(), panelWidth);
      auto *row = panel.findChild<QHBoxLayout *>("DiffusionSizeRow");
      auto *first = row->itemAt(1)->widget();
      auto *last = panel.findChild<QToolButton *>("DiffusionMatchSelectedSize");
      const auto left = first->mapTo(&panel, QPoint{}).x();
      const auto right = last->mapTo(&panel, QPoint(last->width(), 0)).x();
      const auto origin = row->parentWidget()->mapTo(&panel, row->geometry().topLeft()).x();
      QVERIFY(std::abs((left + right) - (origin * 2 + row->geometry().width())) <= 2);
      auto *area = panel.findChild<QScrollArea *>("DiffusionGenerationPage");
      QCOMPARE(area->horizontalScrollBar()->maximum(), 0);
      for (const auto *name : {"DiffusionAspectRatio", "DiffusionMegapixels", "DiffusionResolutionMultiple"}) {
        auto *box = panel.findChild<QComboBox *>(name);
        QVERIFY(box->width() >= 70);
        QVERIFY(box->geometry().right() < box->parentWidget()->width());
      }
      if (const auto capture = qEnvironmentVariable("VULKANA_DIFFUSION_PANEL_CAPTURE"); !capture.isEmpty())
        QVERIFY(panel.grab().save(capture + QString("-resolution-%1.png").arg(panelWidth)));
    }
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
    auto *limit = panel.findChild<QComboBox *>("DiffusionMaxInputSize");
    QVERIFY(limit);
    QImage input(1024, 1536, QImage::Format_RGBA8888);
    input.fill(Qt::red);
    int capped = 0;
    for (int count = 0; count <= 7; ++count) {
      if (count)
        QVERIFY(panel.addInput({input, "Portrait", "Test", false, {}}));
      QCoreApplication::processEvents();
      QVERIFY2(section->height() <= list->height() + header->height() + limit->height() + 30,
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
  void inputRegionsFollowTheirImages() {
    DiffusionPanel panel;
    panel.resize(600, 900);
    panel.showGeneration();
    panel.show();
    QImage red(200, 100, QImage::Format_RGBA8888), blue(100, 200, QImage::Format_RGBA8888);
    red.fill(Qt::red); blue.fill(Qt::blue);
    QVERIFY(panel.addInput({red, "Cats", "Test"}));
    QVERIFY(panel.addInput({blue, "Other cats", "Test"}));
    const auto redId = panel.inputs()[0].id, blueId = panel.inputs()[1].id;
    QVERIFY(!redId.isEmpty() && redId != blueId);
    QVERIFY(panel.setInputRegions(redId, {{QRectF(.1, .2, .3, .4), "Region1"}}));
    QVERIFY(panel.setInputRegions(blueId, {{QRectF(.2, .1, .5, .3), "Region1"}}));
    panel.setRegions({{QRectF(.1, .1, .8, .8), "Output object"}});
    panel.showInputRegions(redId);
    auto *dialog = panel.findChild<QDialog *>("DiffusionPreviewDialog");
    QVERIFY(dialog && dialog->parentWidget());
    QVERIFY(!(dialog->windowFlags() & Qt::WindowMinMaxButtonsHint));
    auto *preview = dynamic_cast<DiffusionPreview *>(dialog->findChild<QWidget *>("DiffusionPreview"));
    QVERIFY(preview);
    QCOMPARE(preview->image(), red);
    QCOMPARE(preview->regions()[0].bounds, QRectF(.1, .2, .3, .4));
    preview->setSelectedRegion(0);
    preview->setSelectedRegionLabel("Cat A");
    QCOMPARE(panel.inputs()[0].regions[0].label, QString("Cat A"));
    QCOMPARE(panel.inputs()[1].regions[0].label, QString("Region1"));
    QCOMPARE(panel.regions()[0].label, QString("Output object"));
    auto *list = panel.findChild<QListWidget *>("DiffusionInputsList");
    QVERIFY(list->model()->moveRows({}, 0, 1, {}, 2));
    QCOMPARE(panel.inputs()[0].id, blueId);
    QCOMPARE(panel.inputs()[1].id, redId);
    QCOMPARE(panel.inputs()[1].regions[0].label, QString("Cat A"));
    const auto groups = panel.inputRegionsJson();
    QCOMPARE(groups.size(), 2);
    QCOMPARE(groups[1].toObject()["image"].toInt(), 2);
    QCOMPARE(groups[1].toObject()["regions"].toArray()[0].toObject()["label"].toString(), QString("Cat A"));
    QCOMPARE(preview->image(), red);
    if (const auto capture = qEnvironmentVariable("VULKANA_DIFFUSION_PANEL_CAPTURE"); !capture.isEmpty())
      QVERIFY(dialog->grab().save(capture + "-input-regions.png"));
    dialog->close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    const auto remove = [&](int row) {
      const auto box = list->visualItemRect(list->item(row));
      QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, QPoint(box.right() - 12, box.center().y()));
      QCoreApplication::processEvents();
    };
    remove(0);
    QCOMPARE(panel.inputs().size(), 1);
    QCOMPARE(panel.inputs()[0].id, redId);
    QCOMPARE(panel.inputs()[0].regions[0].label, QString("Cat A"));
    QVERIFY(!panel.setInputRegions(blueId, {}));
    panel.clearInputs();
    QVERIFY(panel.inputRegionsJson().isEmpty());
    QCOMPARE(panel.regions().size(), 1); // output layout is independent
  }
  void inputRegionsRespectReferenceBudget() {
    DiffusionPanel panel;
    const QJsonObject files{{"diffusion", "/models/diffusion.gguf"},
                           {"textEncoder", "/models/text.gguf"}, {"vae", "/models/vae.safetensors"}};
    panel.setConfiguration({{"profile", "qwen-image-2.1-turbo"}, {"files", files}});
    QImage image(100, 100, QImage::Format_RGBA8888); image.fill(Qt::red);
    QVERIFY(panel.addInput({image, "First", "Test"}));
    QVERIFY(panel.addInput({image, "Second", "Test"}));
    QVector<DiffusionRegion> regions;
    for (int i = 0; i < 6; ++i)
      regions.push_back({QRectF(.1, .1, .2, .2), QString("Region%1").arg(i + 1)});
    QVERIFY(panel.setInputRegions(panel.inputs()[0].id, regions));
    auto *generate = panel.findChild<QPushButton *>("DiffusionGenerate");
    QVERIFY(generate->isEnabled()); // two full references + six crops = eight
    panel.setRegions({{QRectF(.1, .1, .5, .5), "Placement"}});
    QVERIFY(!generate->isEnabled()); // Qwen output annotation also uses a slot
    QVERIFY_EXCEPTION_THROWN(panel.prepareGeneration(), std::invalid_argument);
    panel.setConfiguration({{"profile", "flux3-image-klein-distilled"}, {"files", files}});
    QVERIFY(generate->isEnabled()); // FLUX uses a caption for output layout
    auto *crop = panel.findChild<QCheckBox *>("DiffusionRegionCropGuidance");
    QVERIFY(crop && !crop->isChecked());
    QVERIFY(!panel.regionCropGuidance());
    for (int i = 6; i < 12; ++i)
      regions.push_back({QRectF(.1, .1, .2, .2), QString("Region%1").arg(i + 1)});
    QVERIFY(panel.setInputRegions(panel.inputs()[0].id, regions));
    QVERIFY(generate->isEnabled()); // native boxes do not consume image slots
    crop->click();
    QVERIFY(panel.regionCropGuidance());
    QVERIFY(!generate->isEnabled());
    crop->click();
    QVERIFY(generate->isEnabled());
    panel.setBusy(true);
    QVERIFY(!crop->isEnabled());
    QVERIFY(!panel.setInputRegions(panel.inputs()[0].id, {}));
    panel.setBusy(false);
    QVERIFY(panel.setInputRegions(panel.inputs()[0].id, {}));
    QVERIFY(generate->isEnabled());
  }
  void fluxCropGuidancePersistsOnlyWhenEnabled() {
    DiffusionPanel panel;
    panel.setConfiguration({{"profile", "flux3-image-klein-distilled"}});
    auto *crop = panel.findChild<QCheckBox *>("DiffusionRegionCropGuidance");
    QVERIFY(!crop->isChecked());
    crop->click(); panel.saveSettings();
    DiffusionPanel restored;
    QVERIFY(restored.regionCropGuidance());
    restored.setConfiguration({{"profile", "qwen-image-2.1-turbo"}});
    QVERIFY(!restored.regionCropGuidance());
    QVERIFY(restored.findChild<QCheckBox *>("DiffusionRegionCropGuidance")->isHidden());
  }
  void transferLinksSurviveNamesOrderAndCropping() {
    DiffusionPanel panel;
    panel.setConfiguration({{"profile", "flux3-image-klein-distilled"},
      {"files", QJsonObject{{"diffusion", "/models/d.gguf"}, {"textEncoder", "/models/t.gguf"}, {"vae", "/models/v.safetensors"}}}});
    QImage room(200, 100, QImage::Format_RGBA8888), cats(100, 200, QImage::Format_RGBA8888);
    room.fill(Qt::red); cats.fill(Qt::blue);
    DiffusionPanel::Input donor{cats, "Cats", "Test"}; donor.sourceCanvas = {100, 120};
    panel.setInputs({{room, "Room", "Test"}, donor});
    const auto roomId = panel.inputs()[0].id, donorId = panel.inputs()[1].id;
    QVERIFY(panel.setOutputSceneId(roomId));
    QVERIFY(panel.setInputRegions(donorId, {{QRectF(.1, .1, .3, .4), "Region1", "source-id", "a kitten"}}));
    panel.setRegions({{QRectF(.6, .6, .3, .3), "Floor", "target-id"}});
    QVERIFY(panel.setRegionDestination(donorId, "source-id", "target-id"));
    QVERIFY(panel.regions()[0].targetOnly);
    auto spatial = panel.spatialParameters();
    QCOMPARE(spatial["version"].toInt(), 3);
    QCOMPARE(spatial["outputScene"].toInt(), 1);
    QCOMPARE(spatial["inputRegions"].toArray()[0].toObject()["regions"].toArray()[0].toObject()["description"].toString(), QString("a kitten"));
    QVERIFY(panel.findChild<QListWidget *>("DiffusionInputsList")->model()->moveRows({}, 1, 1, {}, 0));
    QCOMPARE(panel.outputSceneId(), roomId);
    QCOMPARE(panel.outputSceneIndex(), 2);
    QCOMPARE(panel.inputs()[0].regions[0].destinationId, QString("target-id"));
    QVERIFY(panel.setInputCropToCanvas(donorId, true));
    auto regions = panel.inputs()[0].regions;
    regions[0].label = "Chosen cat"; regions[0].description = "a kitten with a collar";
    QVERIFY(panel.setInputRegions(donorId, regions));
    QVERIFY(panel.setInputCropToCanvas(donorId, false));
    QCOMPARE(panel.inputs()[0].regions[0].description, QString("a kitten with a collar"));
    QCOMPARE(panel.inputs()[0].regions[0].destinationId, QString("target-id"));
    const auto target = panel.regions()[0];
    panel.setRegions({});
    QVERIFY(!panel.findChild<QPushButton *>("DiffusionGenerate")->isEnabled());
    QVERIFY_EXCEPTION_THROWN(panel.prepareGeneration(), std::invalid_argument);
    panel.setRegions({target});
    QVERIFY(panel.setRegionDestination(donorId, "source-id", {}));
    QVERIFY(panel.regions()[0].targetOnly); // unlinking never inserts a new model object
    QVERIFY(panel.findChild<QPushButton *>("DiffusionGenerate")->isEnabled());
    QVERIFY(panel.setRegionDestination(donorId, "source-id", "target-id"));
    panel.setInputs({panel.inputs()[0]}); // remove the room
    QVERIFY_EXCEPTION_THROWN(panel.prepareGeneration(), std::invalid_argument);
    panel.showInputRegions(donorId);
    auto *dialog = panel.findChild<QDialog *>("DiffusionPreviewDialog");
    auto *view = dynamic_cast<DiffusionPreview *>(dialog->findChild<QWidget *>("DiffusionPreview"));
    view->setSelectedRegion(0);
    auto *destination = dialog->findChild<QComboBox *>("DiffusionRegionDestination");
    QVERIFY(destination->isVisible()); // a removed scene must not hide the way to unlink
    QVERIFY(destination->findData("__new_target__") < 0);
    destination->setCurrentIndex(0);
    destination->activated(0);
    QVERIFY(panel.inputs()[0].regions[0].destinationId.isEmpty());
    QCOMPARE(panel.outputSceneId(), donorId);
    QVERIFY(panel.findChild<QPushButton *>("DiffusionGenerate")->isEnabled());
    dialog->close();
    QVERIFY(panel.addInput({room, "Room", "Test"}));
    QVERIFY(panel.setOutputSceneId(panel.inputs()[1].id));
    QVERIFY(panel.setRegionDestination(donorId, "source-id", "target-id"));
    panel.setInputs({panel.inputs()[0]});
    QVERIFY(panel.setInputRegions(donorId, {})); // deleting the linked source also repairs the scene choice
    QCOMPARE(panel.outputSceneId(), donorId);
    QVERIFY(panel.findChild<QPushButton *>("DiffusionGenerate")->isEnabled());
  }
  void outputFrameUsesSelectedSceneAndOutputAspect() {
    DiffusionPanel panel;
    panel.setConfiguration({{"profile", "flux3-image-klein-distilled"}});
    QImage room(128, 64, QImage::Format_RGBA8888); room.fill(Qt::red);
    QImage other(64, 64, QImage::Format_RGBA8888); other.fill(Qt::blue);
    panel.setInputs({{other, "Other", "Test"}, {room, "Room", "Test"}});
    panel.setResolution({128, 256});
    QVERIFY(panel.setOutputSceneId(panel.inputs()[1].id));
    panel.showOutputRegions();
    auto *view = dynamic_cast<DiffusionPreview *>(panel.findChild<QWidget *>("DiffusionPreview"));
    QVERIFY(view);
    QCOMPARE(view->image().size(), QSize(128, 256));
    QCOMPARE(view->image().pixelColor(64, 128), QColor(Qt::red));
    QCOMPARE(view->image().pixelColor(64, 0).alpha(), 0); // proportionate fit, no stretching
    panel.findChild<QDialog *>("DiffusionPreviewDialog")->close();
  }
  void canvasCropTogglePreservesSnapshotAndRegions() {
    DiffusionPanel panel;
    panel.resize(560, 800); panel.showGeneration(); panel.show();
    QImage image(120, 80, QImage::Format_RGBA8888); image.fill(Qt::green);
    DiffusionPanel::Input input{image, "Oversized", "Layer"};
    input.sourceCanvas = QSize(80, 64);
    input.toDocument = QTransform::fromTranslate(-20, -10);
    QVERIFY(panel.addInput(input));
    const auto id = panel.inputs().front().id;
    QVERIFY(panel.setInputRegions(id, {{QRectF(.5, .25, .25, .25), "Region1"},
                                      {QRectF(0, 0, .1, .1), "Outside"}}));
    const auto originalRegions = panel.inputs().front().regions;
    auto *list = panel.findChild<QListWidget *>("DiffusionInputsList");
    QCoreApplication::processEvents();
    const auto row = list->visualItemRect(list->item(0));
    QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, QPoint(row.right() - 44, row.center().y()));
    QTRY_VERIFY(panel.inputs().front().cropToCanvas);
    const auto &cropped = panel.inputs().front();
    QCOMPARE(cropped.image.size(), QSize(80, 64));
    QCOMPARE(cropped.toDocument, QTransform{});
    QCOMPARE(cropped.regions.size(), 1);
    QCOMPARE(cropped.regions[0].id, originalRegions[0].id);
    QCOMPARE(cropped.regions[0].bounds, QRectF(.5, 10.0 / 64, 30.0 / 80, 20.0 / 64));
    auto edited = cropped.regions; edited[0].label = "Hair";
    QVERIFY(panel.setInputRegions(id, edited));
    QVERIFY(panel.setInputCropToCanvas(id, false));
    QCOMPARE(panel.inputs().front().image, image);
    QCOMPARE(panel.inputs().front().toDocument, input.toDocument);
    QCOMPARE(panel.inputs().front().regions.size(), 2);
    QCOMPARE(panel.inputs().front().regions[0].bounds, originalRegions[0].bounds);
    QCOMPARE(panel.inputs().front().regions[0].label, QString("Hair"));
    QCOMPARE(panel.inputs().front().regions[1].label, QString("Outside"));
    QVERIFY(panel.addInput({image, "External file", "File"}));
    QVERIFY(!panel.setInputCropToCanvas(panel.inputs()[1].id, true));
    auto copy = panel.inputs(); copy[0].fullToDocument = QTransform::fromTranslate(1000, 0);
    panel.setInputs(copy);
    QVERIFY(panel.setInputCropToCanvas(id, true));
    QVERIFY(panel.inputs()[0].image.isNull());
    QVERIFY_EXCEPTION_THROWN(panel.prepareGeneration(), std::invalid_argument);
    QVERIFY(panel.setInputCropToCanvas(id, false));
    QCOMPARE(panel.inputs()[0].image, image);
  }
  void inputSizeLimitPreservesProportionsAndRegions() {
    DiffusionPanel panel;
    QImage image(1600, 800, QImage::Format_RGBA8888); image.fill(Qt::red);
    DiffusionPanel::Input input{image, "Large layer", "Test"};
    input.sourceCanvas = QSize(600, 400);
    input.toDocument = QTransform::fromScale(.5, .5);
    QVERIFY(panel.addInput(input));
    const auto id = panel.inputs().front().id;
    QVERIFY(panel.setInputRegions(id, {{QRectF(.25, .25, .5, .5), "Region1"}}));
    QCOMPARE(panel.inputs()[0].image.size(), QSize(1024, 512));
    QCOMPARE(panel.inputs()[0].toDocument.map(QPointF(1024, 512)), QPointF(800, 400));
    const auto region = panel.inputs()[0].regions[0];
    panel.setMaxInputSize(256);
    QCOMPARE(panel.inputs()[0].image.size(), QSize(256, 128));
    QCOMPARE(panel.inputs()[0].regions[0].bounds, region.bounds);
    QCOMPARE(panel.inputs()[0].regions[0].id, region.id);
    if (const auto capture = qEnvironmentVariable("VULKANA_DIFFUSION_PANEL_CAPTURE"); !capture.isEmpty()) {
      panel.resize(560, 950); panel.showGeneration(); panel.show();
      QCoreApplication::processEvents();
      QVERIFY(panel.grab().save(capture + "-input-size.png"));
    }
    QVERIFY(panel.setInputCropToCanvas(id, true));
    QCOMPARE(panel.inputs()[0].image.width(), 256);
    QVERIFY(std::abs(double(panel.inputs()[0].image.width()) / panel.inputs()[0].image.height() - 1.5) < .01);
    QCOMPARE(panel.inputs()[0].outputSize, QSize(600, 400));
    QCOMPARE(panel.inputs()[0].regions[0].id, region.id);
    panel.setMaxInputSize(2048);
    QCOMPARE(panel.inputs()[0].image.size(), QSize(600, 400));
    QVERIFY(panel.setInputCropToCanvas(id, false));
    QCOMPARE(panel.inputs()[0].image, image);
    QCOMPARE(panel.inputs()[0].regions[0].bounds, region.bounds);
    QCOMPARE(panel.request()["referenceMaxSize"].toInt(), 2048);
    panel.saveSettings();
    DiffusionPanel restored;
    QCOMPARE(restored.maxInputSize(), 2048);
    panel.setMaxInputSize(1500); // Existing non-preset values are kept exactly.
    panel.saveSettings();
    DiffusionPanel custom;
    QCOMPARE(custom.maxInputSize(), 1500);
    auto *limit = custom.findChild<QComboBox *>("DiffusionMaxInputSize");
    QVERIFY(limit && !limit->isEditable());
    QVERIFY(limit->findData(1440) >= 0);
    QCOMPARE(limit->currentData().toInt(), 1500);
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
    QVERIFY(!dialog->findChild<QPushButton *>("DiffusionCompareLayer")
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
  void generatedLayerNamesUseFrozenModelAndActualSeed() {
    DiffusionPanel::Result result;
    result.metadata = {{"configuration", QJsonObject{{"profile", "qwen-image-2.1-turbo"}}},
        {"request", QJsonObject{{"seed", 42}}},
        {"image", QJsonObject{{"generationInfo", QJsonObject{{"seed", "43"}}}}}};
    QCOMPARE(result.layerName(), QString("Qwen_43"));
    result.metadata["profile"] = "flux3-image-klein-distilled";
    QCOMPARE(result.layerName(), QString("Flux3_43"));
    result.metadata["profile"] = QJsonObject{{"id", "krea-2-turbo"}};
    QCOMPARE(result.layerName(), QString("Krea2_43"));
    result.metadata.remove("image");
    QCOMPARE(result.layerName(), QString("Krea2_42"));
    result.metadata["profile"] = "stable-diffusion";
    result.metadata["configuration"] = QJsonObject{{"files", QJsonObject{{"diffusion", "/models/Illustrious-v1.safetensors"}}}};
    QCOMPARE(result.layerName(), QString("Illustrious-v1_42"));
  }
  void generationThumbnailAndFrozenResultDetails() {
    QWidget host;
    host.resize(1100, 900);
    DiffusionPanel panel(&host);
    panel.resize(600, 820);
    host.show();
    panel.showGeneration();
    auto *tabs = panel.findChild<QTabWidget *>("DiffusionTabs");
    QImage first(100, 60, QImage::Format_RGBA8888);
    first.fill(Qt::red);
    panel.appendResult({first, {}, "First", {
      {"configurationName", "My model"},
      {"configuration", QJsonObject{{"files", QJsonObject{{"diffusion", "/models/original.gguf"}}}}},
      {"request", QJsonObject{{"seed", 42}}},
      {"image", QJsonObject{{"generationInfo", QJsonObject{{"seed", "43"}}}}},
      {"createdAt", "2026-11-08T10:20:30.000Z"}}});
    QCOMPARE(tabs->currentIndex(), 1);
    auto *live = panel.findChild<QLabel *>("DiffusionGenerationThumbnail");
    QVERIFY(live && !live->isHidden() && !live->pixmap().isNull());
    QVERIFY(live->toolTip().isEmpty());
    QVERIFY(panel.findChild<QLabel *>("DiffusionResultThumbnail")->toolTip().isEmpty());
    QCOMPARE(live->pixmap().toImage().pixelColor(0, 0), QColor(Qt::red));
    QCOMPARE(panel.findChild<QLabel *>("DiffusionResultConfig")->text(), QString("My model"));
    QCOMPARE(panel.findChild<QLabel *>("DiffusionResultModel")->text(), QString("original.gguf"));
    QCOMPARE(panel.findChild<QLineEdit *>("DiffusionResultSeed")->text(), QString("43"));
    QVERIFY(!panel.findChild<QLabel *>("DiffusionResultCreated")->text().isEmpty());
    panel.findChild<QToolButton *>("DiffusionCopyResultSeed")->click();
    QCOMPARE(QApplication::clipboard()->text(), QString("43"));
    auto *apply = panel.findChild<QPushButton *>("DiffusionApply");
    QVERIFY(apply->isHidden());
    panel.setBusy(true);
    QImage draft(16, 16, QImage::Format_RGBA8888);
    draft.fill(Qt::green);
    panel.setProgressPreview(draft);
    QCOMPARE(panel.selectedResultIndex(), 0); // Browsing history needn't follow the live image.
    QCOMPARE(live->pixmap().toImage().pixelColor(0, 0), QColor(Qt::green));
    QTest::mouseClick(live, Qt::LeftButton);
    auto *dialog = host.findChild<QDialog *>("DiffusionPreviewDialog");
    QVERIFY(dialog && dialog->isVisible());
    QCOMPARE(panel.selectedResultIndex(), -1);
    QVERIFY(!apply->isEnabled());
    panel.setBusy(false);
    QImage final(80, 90, QImage::Format_RGBA8888);
    final.fill(Qt::blue);
    panel.appendResult({final, {}, "Second", {}});
    QCOMPARE(tabs->currentIndex(), 1);
    QCOMPARE(live->pixmap().toImage().pixelColor(0, 0), QColor(Qt::blue));
    QVERIFY(dialog->isAncestorOf(apply));
    QVERIFY(apply->isEnabled());
    auto *preview = dialog->findChild<QWidget *>("DiffusionPreview");
    QTest::keyClick(preview, Qt::Key_Left);
    QCOMPARE(panel.selectedResultIndex(), 0);
    QCOMPARE(panel.findChild<QLineEdit *>("DiffusionResultSeed")->text(), QString("43"));
    QTest::keyClick(preview, Qt::Key_Right);
    QCOMPARE(panel.selectedResultIndex(), 1);
    int applied = 0;
    panel.onApply = [&] { ++applied; QVERIFY(!dialog->isVisible()); };
    apply->click();
    QCOMPARE(applied, 1);
    QVERIFY(apply->isHidden() && apply->parentWidget() == &panel);
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
  void outputFittingUsesCapturedResultModeAndKeepsChoices() {
    DiffusionPanel panel;
    panel.setOutputCanvasSize({600, 400});
    QImage image(800, 1408, QImage::Format_RGBA8888); image.fill(Qt::blue);
    QTransform placement; placement.translate(20, 40); placement.rotate(30); placement.scale(.5, .8);
    panel.appendResult({image, {}, "Edit", {}, placement, DiffusionPanel::Mode::Edit});
    panel.showResultPreview();
    auto *fit = panel.findChild<QCheckBox *>("DiffusionFitOutput");
    QVERIFY(fit && fit->isChecked() && fit->isEnabled());
    QCOMPARE(panel.resultPlacement(), placement);
    fit->click();
    auto native = panel.resultPlacement();
    QVERIFY(std::abs(std::hypot(native.m11(), native.m12()) - 1.) < 1e-8);
    QVERIFY(std::abs(std::hypot(native.m21(), native.m22()) - 1.) < 1e-8);
    QCOMPARE(native.map(QPointF{}), placement.map(QPointF{}));
    QVERIFY(std::abs(std::atan2(native.m12(), native.m11()) - std::atan2(placement.m12(), placement.m11())) < 1e-8);
    panel.setTargetCompatible(true);
    panel.setDestination(DiffusionPanel::Destination::ReplaceSelected);
    QVERIFY(fit->isChecked() && !fit->isEnabled());
    panel.setDestination(DiffusionPanel::Destination::NewLayer);
    QVERIFY(!fit->isChecked() && fit->isEnabled());
    QCOMPARE(panel.resultPlacement(), native);
    panel.appendResult({image, {}, "Generate", {}, {}, DiffusionPanel::Mode::Generate});
    QVERIFY(!fit->isChecked() && fit->isEnabled());
    fit->click();
    QCOMPARE(panel.resultPlacement().mapRect(QRectF(QPointF{}, image.size())).size(), QSizeF(600, 400));
    panel.setDestination(DiffusionPanel::Destination::NewDocument);
    QVERIFY(!fit->isChecked() && !fit->isEnabled());
    QCOMPARE(panel.resultPlacement(), QTransform{});
    panel.setDestination(DiffusionPanel::Destination::NewLayer);
    panel.appendResult({image, {}, "Fill", {}, {}, DiffusionPanel::Mode::Fill});
    QVERIFY(fit->isChecked() && !fit->isEnabled());
    panel.findChild<QListWidget *>("DiffusionResultsList")->setCurrentRow(0);
    QVERIFY(!fit->isChecked() && fit->isEnabled());
    QCOMPARE(panel.resultPlacement(), native);
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
  void configurationsAutosaveWithoutMixingTheirSettings() {
    const QJsonObject config{{"profile", "flux3-image-klein-distilled"},
                             {"files", QJsonObject{{"diffusion", "/models/first.gguf"}}}};
    const QJsonArray configs{
        QJsonObject{{"id", "A"}, {"name", "First"}, {"configuration", config}, {"backend", "auto"}},
        QJsonObject{{"id", "B"}, {"name", "Second"}, {"configuration", config}, {"backend", "vulkan"}}};
    QSettings().setValue("diffusion/modelConfigurations", QJsonDocument(configs).toJson());
    QSettings().setValue("diffusion/modelId", "A");
    const auto stored = [](const QString &id) {
      const auto array = QJsonDocument::fromJson(QSettings().value("diffusion/modelConfigurations").toByteArray()).array();
      for (const auto &entry : array) if (entry.toObject()["id"] == id) return entry.toObject();
      return QJsonObject{};
    };
    DiffusionPanel panel;
    QVERIFY(!panel.findChild<QPushButton *>("DiffusionSaveConfiguration"));
    auto *models = panel.findChild<QComboBox *>("DiffusionConfigurationModels");
    auto *weights = panel.findChild<QLineEdit *>("DiffusionFile_diffusion");
    weights->setText("/models/updated-a.gguf");
    // Switching immediately flushes the old configuration before loading the next.
    models->setCurrentIndex(models->findData("B"));
    QCOMPARE(stored("A")["configuration"].toObject()["files"].toObject()["diffusion"].toString(), QString("/models/updated-a.gguf"));
    QCOMPARE(weights->text(), QString("/models/first.gguf"));
    QCOMPARE(panel.backend(), QString("vulkan"));
    weights->setText("/models/updated-b.gguf");
    auto *threads = panel.findChild<QDoubleSpinBox *>("Diffusion_threads");
    QVERIFY(threads);
    threads->setValue(6);
    panel.findChild<QDoubleSpinBox *>("Diffusion_steps")->setValue(23);
    panel.findChild<QComboBox *>("DiffusionBackend")->setCurrentIndex(2);
    QTRY_COMPARE(stored("B")["configuration"].toObject()["settings"].toObject()["threads"].toInt(), 6);
    QTRY_COMPARE(stored("B")["generation"].toObject()["steps"].toInt(), 23);
    QCOMPARE(stored("B")["backend"].toString(), QString("cuda"));
    panel.findChild<QToolButton *>("DiffusionLoraAdd")->click();
    QTRY_VERIFY(panel.findChild<QLineEdit *>("DiffusionLoraPath"));
    panel.findChild<QLineEdit *>("DiffusionLoraPath")->setText("/models/style.safetensors");
    QTRY_COMPARE(stored("B")["configuration"].toObject()["loras"].toArray().size(), 1);
    QTRY_COMPARE(stored("B")["configuration"].toObject()["loras"].toArray()[0].toObject()["path"].toString(), QString("/models/style.safetensors"));
    models->setCurrentIndex(models->findData("A"));
    QCOMPARE(weights->text(), QString("/models/updated-a.gguf"));
    QCOMPARE(panel.backend(), QString("auto"));
    QVERIFY(panel.configuration()["loras"].toArray().isEmpty());
    models->setCurrentIndex(models->findData("B"));
    QCOMPARE(weights->text(), QString("/models/updated-b.gguf"));
    QCOMPARE(panel.request()["steps"].toInt(), 23);
    DiffusionPanel restored;
    QCOMPARE(restored.configurationName(), QString("Second"));
    QCOMPARE(restored.configuration(), panel.configuration());
    QCOMPARE(restored.backend(), QString("cuda"));
    QCOMPARE(restored.request()["steps"].toInt(), 23);
    auto *families = panel.findChild<QComboBox *>("DiffusionFamily");
    families->setCurrentIndex(families->findText("Stable Diffusion"));
    QTRY_COMPARE(stored("B")["configuration"].toObject()["profile"].toString(), QString("stable-diffusion"));
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
