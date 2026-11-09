#include "imageeditor/core/LayerCommands.hpp"
#include "imageeditor/core/DocumentCommands.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/DiffusionPanel.hpp"
#include "imageeditor/ui/DiffusionPreview.hpp"
#include "imageeditor/ui/LayerListView.hpp"
#include "imageeditor/ui/LayerListModel.hpp"
#include "imageeditor/ui/DiffusionTransaction.hpp"
#include "imageeditor/ui/FlattenedDocument.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/NewDocumentDialog.hpp"
#include "imageeditor/ui/OverlayDockWorkspace.hpp"
#include "imageeditor/ui/WorkspacePanel.hpp"
#include "imageeditor/ui/Theme.hpp"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QInputDialog>
#include <QInputMethodEvent>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QScopeGuard>
#include <QListView>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QPointer>
#include <QScrollArea>
#include <QSettings>
#include <QSlider>
#include <QSplitter>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <memory>
#include <tuple>
namespace u = imageeditor::ui;
namespace c = imageeditor::core;
class DiffusionIntegrationTests : public QObject {
  Q_OBJECT
  QTemporaryDir temporary;
  std::unique_ptr<u::MainWindow> window;
  u::DiffusionPanel *panel{};
  QString fixturePath;
  QString modelPath;
  template <class T> T *child(const char *name) {
    return window->findChild<T *>(name);
  }
  void openMode(const char *action) {
    auto *a = child<QAction>(action);
    QVERIFY(a);
    a->trigger();
    QCoreApplication::processEvents();
  }
  void prompt(const QString &text) {
    auto *field = panel->findChild<QPlainTextEdit *>("DiffusionPrompt");
    QVERIFY(field);
    field->setPlainText(text);
  }
  c::EditorSession &session() {
    return const_cast<c::EditorSession &>(window->editorSession());
  }
  void generate(const QString &text = "A green studio object") {
    prompt(text);
    panel->onGenerate();
  }
  QImage native() {
    return u::diffusionRawLayer(
        *session().document()->layer(*session().activeLayer()));
  }
private slots:
  void initTestCase() {
    QCoreApplication::setOrganizationName("VulkanaDiffusionTests");
    QCoreApplication::setApplicationName("Integration");
    QStandardPaths::setTestModeEnabled(true);
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       temporary.path());
    u::applyEditorTheme(*qobject_cast<QApplication *>(qApp));
    fixturePath = temporary.filePath("source.png");
    QImage source(64, 64, QImage::Format_RGBA8888);
    source.fill(QColor(70, 90, 210));
    QVERIFY(source.save(fixturePath));
    modelPath = temporary.filePath("model.gguf");
    QFile model(modelPath);
    QVERIFY(model.open(QIODevice::WriteOnly));
    model.write("test");
    model.close();
  }
  void init() {
    QSettings().clear();
    window = std::make_unique<u::MainWindow>(nullptr, false, false);
    QVERIFY(child<QWidget>("AIImageEditorWorkspace")->isHidden());
    window->setUnsavedPromptEnabled(false);
    window->resize(1280, 900);
    window->show();
    QCoreApplication::processEvents();
    QVERIFY(child<QWidget>("AIImageEditorWorkspace")->isHidden());
    QVERIFY(window->openImageFromPath(fixturePath));
    auto *widget = child<QWidget>("AIImageEditorPanel");
    QVERIFY(widget);
    panel = dynamic_cast<u::DiffusionPanel *>(widget);
    QVERIFY(panel);
    panel->setWorkerExecutable(QString::fromUtf8(DIFFUSION_UI_WORKER));
    panel->setConfiguration({{"profile", "qwen-image-2.1-turbo"},
                             {"files", QJsonObject{{"diffusion", modelPath},
                                                   {"textEncoder", modelPath},
                                                   {"vae", modelPath},
                                                   {"vision", modelPath}}},
                             {"settings", QJsonObject{{"device", "CPU"}}}});
    panel->setResolution({64, 64});
  }
  void cleanup() {
    window.reset();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  }
  void aiStaysHiddenAcrossRestartAndStartupChooser() {
    openMode("AIGenerateImageAction");
    panel->saveSettings();
    window.reset();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    // Keep the saved model configuration, as on an ordinary subsequent launch.
    window = std::make_unique<u::MainWindow>(nullptr, false, false);
    window->setUnsavedPromptEnabled(false);
    window->resize(1280, 900);
    auto *shell = child<QWidget>("AIImageEditorWorkspace");
    QVERIFY(shell && shell->isHidden());
    window->show();
    QCoreApplication::processEvents();
    QVERIFY(shell->isHidden());
    bool inspected = false, hiddenDuringChooser = false;
    QTimer probe, watchdog;
    connect(&probe, &QTimer::timeout, window.get(), [&] {
      for (auto *dialog : window->findChildren<QDialog *>()) {
        if (dynamic_cast<u::NewDocumentDialog *>(dialog) && dialog->isVisible()) {
          inspected = true;
          hiddenDuringChooser = shell->isHidden();
          probe.stop();
          dialog->reject();
          return;
        }
      }
    });
    watchdog.setSingleShot(true);
    connect(&watchdog, &QTimer::timeout, window.get(), [&] { window->close(); });
    probe.start(10);
    watchdog.start(5000);
    window->showStartupDocument();
    probe.stop(); watchdog.stop();
    QVERIFY(inspected && hiddenDuringChooser && shell->isHidden());
    panel = dynamic_cast<u::DiffusionPanel *>(child<QWidget>("AIImageEditorPanel"));
    QVERIFY(panel);
    QCOMPARE(panel->configuration()["files"].toObject()["diffusion"].toString(), modelPath);
    openMode("AIGenerateImageAction");
    auto *workspace = dynamic_cast<u::OverlayDockWorkspace *>(child<QWidget>("CanvasWorkspace"));
    QVERIFY(workspace && workspace->rightPanelCard()->isAncestorOf(shell));
    QVERIFY(shell->isVisible());
    panel->onClose();
    QVERIFY(shell->isHidden());
  }
  void presetPromptAcceptsNativeKeyboardAndIme() {
    openMode("AIEditLayerAction");
    auto *presets = panel->findChild<QComboBox *>("DiffusionPresets");
    auto *field = panel->findChild<QPlainTextEdit *>("DiffusionPrompt");
    QVERIFY(presets && field);
    const int row = presets->findData("builtin.background-remove");
    QVERIFY(row > 0);
    presets->setCurrentIndex(row);
    presets->activated(row);
    const QString preset = field->toPlainText();
    QVERIFY(preset.contains("background"));
    QVERIFY(!field->isReadOnly());
    panel->findChild<QScrollArea *>("DiffusionGenerationPage")
        ->ensureWidgetVisible(field);
    QCoreApplication::processEvents();
    QTest::mouseClick(field->viewport(), Qt::LeftButton);
    QTRY_VERIFY(field->hasFocus());
    QCOMPARE(child<QWidget>("VulkanCanvasContainer")->focusProxy(),
             static_cast<QWidget *>(field));
    field->selectAll();
    const auto tool = session().activeTool();
    const auto revision = session().document()->revision();
    const auto key = [&](int value, const QString &text,
                         Qt::KeyboardModifiers modifiers = {}) {
      QKeyEvent press(QEvent::KeyPress, value, modifiers, text);
      QCoreApplication::sendEvent(window->windowHandle(), &press);
      QKeyEvent release(QEvent::KeyRelease, value, modifiers, text);
      QCoreApplication::sendEvent(window->windowHandle(), &release);
    };
    key(Qt::Key_D, "d");
    QCOMPARE(field->toPlainText(), QString("d"));
    key(Qt::Key_Z, {}, Qt::ControlModifier);
    QCOMPARE(field->toPlainText(), preset);
    field->selectAll();
    QInputMethodEvent ime;
    ime.setCommitString(QString::fromUtf8("retouche 日本語"));
    QCoreApplication::sendEvent(window->windowHandle(), &ime);
    QCOMPARE(field->toPlainText(), QString::fromUtf8("retouche 日本語"));
    QCOMPARE(panel->request().value("prompt").toString(), field->toPlainText());
    QCOMPARE(session().document()->revision(), revision);
    QCOMPARE(session().activeTool(), tool);
  }
  void presetSnippetsAreEditableWithoutPrompts() {
    openMode("AIEditLayerAction");
    auto *presets = panel->findChild<QComboBox *>("DiffusionPresets");
    auto *field = panel->findChild<QPlainTextEdit *>("DiffusionPrompt");
    field->setPlainText("Previous prompt");
    for (const auto &id :
         {"builtin.generate", "builtin.fill", "builtin.background-replace",
          "builtin.background-remove"}) {
      const auto row = presets->findData(id);
      QVERIFY(row > 0);
      presets->setCurrentIndex(row);
      presets->activated(row);
      QVERIFY(!field->toPlainText().isEmpty());
      QVERIFY(!field->toPlainText().contains("{instruction}"));
      QVERIFY(!child<QInputDialog>("DiffusionTextPrompt"));
      QVERIFY(!QApplication::activeModalWidget());
    }
    const auto snippet = field->toPlainText();
    panel->findChild<QScrollArea *>("DiffusionGenerationPage")
        ->ensureWidgetVisible(field);
    QCoreApplication::processEvents();
    QTest::mouseClick(field->viewport(), Qt::LeftButton);
    field->selectAll();
    QKeyEvent key(QEvent::KeyPress, Qt::Key_D, Qt::NoModifier, "d");
    QCoreApplication::sendEvent(window->windowHandle(), &key);
    QCOMPARE(field->toPlainText(), QString("d"));
    field->undo();
    QCOMPARE(field->toPlainText(), snippet);
    field->undo();
    QVERIFY(field->toPlainText() != snippet); // preset changes are undoable too
  }
  void inputLimitPresetAcceptsNativeKeyboard() {
    openMode("AIEditLayerAction");
    auto *limit = panel->findChild<QComboBox *>("DiffusionMaxInputSize");
    QVERIFY(limit && !limit->isEditable());
    auto *area = panel->findChild<QScrollArea *>("DiffusionGenerationPage");
    area->ensureWidgetVisible(limit);
    QCoreApplication::processEvents();
    if (const auto capture = qEnvironmentVariable("VULKANA_DIFFUSION_INTEGRATION_CAPTURE"); !capture.isEmpty())
      QVERIFY(panel->grab().save(capture + "-max-input.png"));
    const auto *list = panel->findChild<QListWidget *>("DiffusionInputsList");
    QVERIFY(list);
    QCOMPARE(limit->geometry().center().x(), list->geometry().center().x());
    QTest::mouseClick(limit, Qt::LeftButton);
    QKeyEvent home(QEvent::KeyPress, Qt::Key_Home, Qt::NoModifier);
    QCoreApplication::sendEvent(window->windowHandle(), &home);
    for (int i = 0; i < limit->findData(2048); ++i) {
      QKeyEvent down(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
      QCoreApplication::sendEvent(window->windowHandle(), &down);
    }
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QCoreApplication::sendEvent(window->windowHandle(), &enter);
    QCOMPARE(panel->maxInputSize(), 2048);
    QCOMPARE(panel->request()["referenceMaxSize"].toInt(), 2048);
  }
  void collapsedNegativePromptAcceptsNativeKeyboard() {
    openMode("AIGenerateImageAction");
    auto *header = panel->findChild<QToolButton *>("DiffusionNegativeSectionHeader");
    auto *negative = panel->findChild<QPlainTextEdit *>("DiffusionNegativePrompt");
    auto *positive = panel->findChild<QPlainTextEdit *>("DiffusionPrompt");
    QVERIFY(header && negative && !header->isChecked());
    QVERIFY(!negative->isVisible() && !negative->isEnabled());
    panel->findChild<QDoubleSpinBox *>("Diffusion_guidance")->setValue(2.0);
    QVERIFY(negative->isEnabled());
    header->click();
    panel->findChild<QScrollArea *>("DiffusionGenerationPage")->ensureWidgetVisible(negative);
    QCoreApplication::processEvents();
    QVERIFY(negative->isVisible());
    QVERIFY(header->mapTo(panel, QPoint()).y() >= positive->mapTo(panel, QPoint(0, positive->height())).y());
    QTest::mouseClick(negative->viewport(), Qt::LeftButton);
    QTRY_VERIFY(negative->hasFocus());
    QInputMethodEvent ime;
    ime.setCommitString("Avoid text");
    QCoreApplication::sendEvent(window->windowHandle(), &ime);
    QCOMPARE(negative->toPlainText(), QString("Avoid text"));
    header->click();
    QCOMPARE(panel->request()["negativePrompt"].toString(), QString("Avoid text"));
    panel->findChild<QDoubleSpinBox *>("Diffusion_guidance")->setValue(1.0);
    QVERIFY(!panel->request().contains("negativePrompt"));
    QCOMPARE(negative->toPlainText(), QString("Avoid text"));
  }
  void livePreviewAppearsBesidePreviousResults() {
    openMode("AIGenerateImageAction");
    generate();
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    const auto original = panel->selectedResult()->image;
    const auto revision = session().document()->revision();
    generate("slow");
    auto *list = panel->findChild<QListWidget *>("DiffusionResultsList");
    QCOMPARE(list->count(), 2);
    QTRY_COMPARE_WITH_TIMEOUT(list->item(1)->text(), QString("Pending"), 10000);
    QCOMPARE(panel->results().size(), 1);
    QCOMPARE(panel->selectedResult()->image, original);
    list->setCurrentRow(1);
    panel->showResultPreview();
    auto *preview =
        dynamic_cast<u::DiffusionPreview *>(child<QWidget>("DiffusionPreview"));
    QVERIFY(preview);
    QCOMPARE(preview->image().size(), QSize(16, 16));
    panel->onApply();
    QCOMPARE(session().document()->revision(), revision);
    child<QToolButton>("DiffusionPreviousPreview")->click();
    QCOMPARE(preview->image(), original);
    child<QToolButton>("DiffusionNextPreview")->click();
    QCOMPARE(preview->image().size(), QSize(16, 16));
    panel->onCancel();
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    QCOMPARE(list->count(), 1);
    QCOMPARE(preview->image(), original);
    generate();
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    QCOMPARE(list->count(), 2);
    QCOMPARE(panel->results().size(), 2);
    QVERIFY(panel->selectedResult());
  }
  void idleEditingRefreshesTargetBeforeFirstGeneration() {
    openMode("AIEditLayerAction");
    QVERIFY(session().execute(std::make_unique<c::SetLayerOpacityCommand>(
        *session().activeLayer(), .75F)));
    const auto revision = session().document()->revision();
    generate();
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    QCOMPARE(session().document()->revision(), revision);
    QCOMPARE(panel->results().size(), 1);
    // Idle canvas editing is allowed even while a preview is retained.
    QVERIFY(session().execute(std::make_unique<c::SetLayerOpacityCommand>(
        *session().activeLayer(), .5F)));
    const auto edited = session().document()->revision();
    panel->setDestination(u::DiffusionPanel::Destination::ReplaceSelected);
    panel->onApply();
    QVERIFY(session().document()->revision() != edited);
    QCOMPARE(panel->results().size(), 1);
  }
  void discardCompletedPreviewWhileNextJobStarts() {
    openMode("AIGenerateImageAction");
    generate();
    QTRY_COMPARE_WITH_TIMEOUT(panel->results().size(), 1, 10000);
    const auto previous = panel->selectedResult()->label;
    QTRY_VERIFY(child<QPushButton>("DiffusionUnloadModel") && child<QPushButton>("DiffusionUnloadModel")->isEnabled());
    child<QPushButton>("DiffusionUnloadModel")->click();
    QTRY_VERIFY(child<QLabel>("DiffusionLoadedModelsEmpty"));

    openMode("AIEditLayerAction");
    generate("identity"); // The new worker still needs the captured input file.
    panel->findChild<QTabWidget *>("DiffusionTabs")->setCurrentIndex(2);
    auto *list = child<QListWidget>("DiffusionResultsList");
    auto *discard = child<QPushButton>("DiffusionDiscard");
    QCOMPARE(list->movement(), QListView::Static);
    QCOMPARE(list->count(), 2);
    auto *pending = list->item(1);
    list->setCurrentItem(pending);
    QVERIFY(!discard->isEnabled());
    panel->onDiscard();
    QCOMPARE(panel->results().size(), 1);
    list->setCurrentRow(0);
    QVERIFY(discard->isVisible() && discard->isEnabled());
    discard->click();
    QVERIFY(panel->isBusy());
    QVERIFY(panel->results().isEmpty());
    QCOMPARE(list->count(), 1);
    QCOMPARE(list->currentItem(), pending);
    QCOMPARE(child<QLabel>("DiffusionResultThumbnail")->text(), QString("Generating…"));
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    QCOMPARE(list->count(), 1);
    QVERIFY(panel->selectedResult());
    QVERIFY(panel->selectedResult()->label != previous);
    QCOMPARE(panel->selectedResult()->image.pixelColor(0, 0), native().pixelColor(0, 0));
  }
  void generationHistoryAndDestinations() {
    openMode("AIGenerateImageAction");
    const auto before = session().document()->revision();
    const auto count = session().document()->layers().size();
    generate();
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    QCOMPARE(panel->results().size(), 1);
    const auto request = panel->selectedResult()->metadata.value("request").toObject();
    QVERIFY(!request.contains("referenceImages"));
    QVERIFY(!request.contains("spatial"));
    QCOMPARE(session().document()->revision(), before);
    const auto previous = panel->results().front().image;
    generate();
    QCOMPARE(panel->results().size(), 1);
    QVERIFY(session().execute(std::make_unique<c::SetLayerOpacityCommand>(
        *session().activeLayer(), .9F)));
    const auto during = session().document()->revision();
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    QCOMPARE(session().document()->revision(), during);
    QCOMPARE(panel->results().size(), 2);
    QCOMPARE(panel->results().front().image, previous);
    panel->setDestination(u::DiffusionPanel::Destination::NewLayer);
    panel->onApply();
    QCOMPARE(session().document()->layers().size(), count + 1);
    QVERIFY(session().undo());
    QCOMPARE(session().document()->layers().size(), count);
    QVERIFY(session().redo());
    openMode("AIGenerateImageAction");
    generate();
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    const auto docs = window->documentCount();
    panel->setDestination(u::DiffusionPanel::Destination::NewDocument);
    panel->onApply();
    QCOMPARE(window->documentCount(), docs + 1);
    QCOMPARE(session().document()->canvas().extent.width, 64u);
  }
  void generationOnlyModelOmitsRetainedEditingInputs() {
    openMode("AIEditLayerAction");
    const auto inputId = panel->inputs().front().id;
    panel->setRegions({{QRectF(.1, .1, .4, .4), "Region1"}});
    auto configuration = panel->configuration();
    configuration["profile"] = "krea-2-turbo";
    panel->setConfiguration(configuration);
    QCOMPARE(panel->mode(), u::DiffusionPanel::Mode::Generate);
    child<QMenu>("AIImageEditorMenu")->aboutToShow();
    QVERIFY(!child<QAction>("AIEditLayerAction")->isEnabled());
    QVERIFY(!child<QAction>("AIGenerativeFillAction")->isEnabled());
    panel->onAddInput(u::DiffusionPanel::InputSource::File);
    QVERIFY(!QApplication::activeModalWidget());
    const auto revision = session().document()->revision();
    generate();
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    QCOMPARE(panel->results().size(), 1);
    const auto request = panel->selectedResult()->metadata["request"].toObject();
    for (const auto *field : {"referenceImages", "referenceMaxSize", "initialImage", "mask", "strength", "spatial"})
      QVERIFY2(!request.contains(field), field);
    QCOMPARE(panel->inputs().front().id, inputId);
    QCOMPARE(session().document()->revision(), revision);
    configuration["profile"] = "flux3-image-klein-distilled";
    panel->setConfiguration(configuration);
    child<QMenu>("AIImageEditorMenu")->aboutToShow();
    QVERIFY(child<QAction>("AIEditLayerAction")->isEnabled());
    panel->setMode(u::DiffusionPanel::Mode::Edit);
    generate();
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    QCOMPARE(panel->results().size(), 2);
    const auto edit = panel->selectedResult()->metadata["request"].toObject();
    QCOMPARE(edit["referenceImages"].toArray().size(), 1);
    QVERIFY(edit.contains("spatial"));
  }
  void loraControlsUseNativeInputAndWarmGeneration() {
    openMode("AIGenerateImageAction");
    auto config = panel->configuration();
    const QJsonArray defaults{
        QJsonObject{{"id", "mythic"}, {"path", modelPath}, {"name", "Mythic"}, {"strength", .8}, {"enabled", true}},
        QJsonObject{{"id", "disabled"}, {"path", "/missing/optional.safetensors"}, {"name", "Optional"}, {"strength", 1}, {"enabled", false}}};
    config["loras"] = defaults;
    panel->setConfiguration(config);
    auto *tabs = panel->findChild<QTabWidget *>("DiffusionTabs");
    tabs->setCurrentIndex(0);
    auto *header = child<QToolButton>("DiffusionLoraDefaultsSectionHeader");
    QVERIFY(!header->isChecked());
    header->click();
    auto *name = panel->findChild<QLineEdit *>("DiffusionLoraFriendlyName");
    panel->findChild<QScrollArea *>("DiffusionConfigurationPage")->ensureWidgetVisible(name);
    QCoreApplication::processEvents();
    QTest::mouseClick(name, Qt::LeftButton);
    QTRY_VERIFY(name->hasFocus());
    name->selectAll();
    QKeyEvent typed(QEvent::KeyPress, Qt::Key_M, Qt::NoModifier, "Mythic portrait");
    QCoreApplication::sendEvent(window->windowHandle(), &typed);
    QInputMethodEvent ime;
    ime.setCommitString(QString::fromUtf8(" 日本語"));
    QCoreApplication::sendEvent(window->windowHandle(), &ime);
    QCOMPARE(panel->configuration()["loras"].toArray()[0].toObject()["name"].toString(), QString::fromUtf8("Mythic portrait 日本語"));
    if (const auto capture = qEnvironmentVariable("VULKANA_DIFFUSION_INTEGRATION_CAPTURE"); !capture.isEmpty())
      QVERIFY(panel->grab().save(capture + "-lora-config.png"));
    tabs->setCurrentIndex(1);
    child<QToolButton>("DiffusionLorasSectionHeader")->click();
    auto *strength = child<QDoubleSpinBox>("DiffusionLoraStrength");
    strength->setValue(.4);
    QCoreApplication::processEvents();
    if (const auto capture = qEnvironmentVariable("VULKANA_DIFFUSION_INTEGRATION_CAPTURE"); !capture.isEmpty())
      QVERIFY(panel->grab().save(capture + "-lora-generate.png"));
    QTimer::singleShot(0, window.get(), [this] {
      auto *dialog = child<QInputDialog>("DiffusionConfigurationNamePrompt");
      QVERIFY(dialog);
      dialog->setTextValue("LoRA configuration");
      dialog->accept();
    });
    child<QToolButton>("DiffusionDuplicateConfiguration")->click();
    const auto saved = QJsonDocument::fromJson(QSettings().value("diffusion/modelConfigurations").toByteArray()).array();
    QCOMPARE(saved.size(), 1);
    QCOMPARE(saved[0].toObject()["configuration"].toObject()["loras"].toArray()[0].toObject()["strength"].toDouble(), .8);
    QVERIFY(!saved[0].toObject()["generation"].toObject().contains("loras"));
    // Duplication copies configuration defaults, not session-only overrides.
    QCOMPARE(panel->request()["loras"].toArray()[0].toObject()["strength"].toDouble(), .8);
    panel->findChild<QDoubleSpinBox *>("DiffusionLoraStrength")->setValue(.4);
    generate();
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    QCOMPARE(panel->results().size(), 1);
    auto result = panel->selectedResult()->metadata;
    auto adapters = result["request"].toObject()["loras"].toArray();
    QCOMPARE(adapters.size(), 1);
    QCOMPARE(adapters[0].toObject()["strength"].toDouble(), .4);
    QCOMPARE(panel->configuration()["loras"].toArray()[0].toObject()["strength"].toDouble(), .8);
    const auto context = result["contextGeneration"];
    panel->findChild<QCheckBox *>("DiffusionLoraEnabled")->setChecked(false);
    generate();
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    QCOMPARE(panel->results().size(), 2);
    result = panel->selectedResult()->metadata;
    QVERIFY(!result["request"].toObject().contains("loras"));
    QCOMPARE(result["contextGeneration"], context);
  }
  void resolutionSelectorNativeEditsAndMatchActions() {
    openMode("AIGenerateImageAction");
    auto *ratio = panel->findChild<QComboBox *>("DiffusionAspectRatio");
    auto *width = panel->findChild<QSpinBox *>("DiffusionWidth");
    auto *height = panel->findChild<QSpinBox *>("DiffusionHeight");
    QCOMPARE(ratio->currentData().toSize(), QSize(1, 1));
    const auto revision = session().document()->revision();
    panel->findChild<QToolButton *>("DiffusionResolutionSectionHeader")->click();
    for (auto *field : {width, height}) {
      panel->findChild<QScrollArea *>("DiffusionGenerationPage")->ensureWidgetVisible(field);
      QCoreApplication::processEvents();
      QTest::mouseClick(field, Qt::LeftButton, {}, QPoint(25, field->height() / 2));
      QTRY_VERIFY(field->hasFocus());
      field->selectAll();
      for (auto digit : QString("1024")) {
        QKeyEvent key(QEvent::KeyPress, digit.unicode(), Qt::NoModifier, QString(digit));
        QCoreApplication::sendEvent(window->windowHandle(), &key);
      }
      QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
      QCoreApplication::sendEvent(window->windowHandle(), &enter);
      QCOMPARE(field->value(), 1024);
      QCOMPARE(ratio->currentText(), QString("Custom"));
    }
    child<QToolButton>("DiffusionResetSize")->click();
    QCOMPARE(width->value(), 64);
    QCOMPARE(height->value(), 64);
    QCOMPARE(ratio->currentData().toSize(), QSize(1, 1));
    QImage reference(128, 192, QImage::Format_RGBA8888);
    reference.fill(Qt::blue);
    QVERIFY(panel->addInput({reference, "Portrait", "Test"}));
    child<QListWidget>("DiffusionInputsList")->setCurrentRow(0);
    child<QToolButton>("DiffusionMatchSelectedSize")->click();
    QCOMPARE(width->value(), 128);
    QCOMPARE(height->value(), 192);
    QCOMPARE(ratio->currentData().toSize(), QSize(2, 3));
    QCOMPARE(panel->findChild<QComboBox *>("DiffusionMegapixels")->currentData().toDouble(), 1.0);
    panel->clearInputs();
    child<QToolButton>("DiffusionMatchSelectedSize")->click();
    QCOMPARE(width->value(), 64);
    QCOMPARE(height->value(), 64);
    QCOMPARE(ratio->currentData().toSize(), QSize(1, 1));
    QCOMPARE(session().document()->revision(), revision);
  }
  void checkpointOnlyGenerationAndNativeFileEntry() {
    openMode("AIEditLayerAction");
    panel->setConfiguration({{"profile", "stable-diffusion"}, {"files", QJsonObject{}}});
    QCOMPARE(panel->mode(), u::DiffusionPanel::Mode::Generate);
    auto *field = child<QLineEdit>("DiffusionFile_diffusion");
    auto *tabs = child<QTabWidget>("DiffusionTabs");
    tabs->setCurrentIndex(0);
    child<QScrollArea>("DiffusionConfigurationPage")->ensureWidgetVisible(field);
    QCoreApplication::processEvents();
    QTest::mouseClick(field, Qt::LeftButton);
    QTRY_VERIFY(field->hasFocus());
    QInputMethodEvent entry;
    entry.setCommitString(modelPath);
    QCoreApplication::sendEvent(window->windowHandle(), &entry);
    QCOMPARE(field->text(), modelPath);
    QVERIFY(child<QPushButton>("DiffusionGenerate")->isEnabled());
    QCOMPARE(panel->configuration()["files"].toObject().size(), 1);
    const auto before = session().document()->layers().size();
    const auto revision = session().document()->revision();
    generate();
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    QCOMPARE(panel->results().size(), 1);
    const auto request = panel->selectedResult()->metadata["request"].toObject();
    QVERIFY(!request.contains("referenceImages"));
    QVERIFY(!request.contains("initialImage"));
    QVERIFY(!request.contains("mask"));
    QVERIFY(!request.contains("spatial"));
    QCOMPARE(request["guidance"].toDouble(), 7.0);
    QCOMPARE(session().document()->revision(), revision);
    panel->setDestination(u::DiffusionPanel::Destination::NewLayer);
    panel->onApply();
    QCOMPARE(session().document()->layers().size(), before + 1);
    QVERIFY(session().undo());
    QCOMPARE(session().document()->layers().size(), before);
  }
  void incompatibleResolutionFailsBeforeWorkerStartupAndRecovers() {
    openMode("AIGenerateImageAction");
    auto config = panel->configuration();
    config["profile"] = "krea-2-turbo";
    panel->setConfiguration(config);
    auto *ratio = child<QComboBox>("DiffusionAspectRatio");
    auto *multiple = child<QComboBox>("DiffusionResolutionMultiple");
    auto *width = child<QSpinBox>("DiffusionWidth");
    auto *height = child<QSpinBox>("DiffusionHeight");
    ratio->setCurrentIndex(ratio->findData(QSize(2, 3)));
    ratio->activated(ratio->currentIndex());
    width->setValue(841);
    height->setValue(1257);
    const auto revision = session().document()->revision();
    const auto worker = panel->workerExecutable();
    // A missing worker would fail discovery if validation did not run first.
    panel->setWorkerExecutable(temporary.filePath("missing-worker"));
    generate();
    QVERIFY(!panel->isBusy());
    QVERIFY(panel->results().isEmpty());
    QVERIFY(child<QLabel>("DiffusionLoadedModelsEmpty"));
    bool sawSizeError = false;
    for (auto *label : panel->findChildren<QLabel *>())
      sawSizeError |= label->text() == "Invalid resolution: 841 × 1257. Krea 2 Turbo requires dimensions divisible by 8. Try 840 × 1256 or 848 × 1264.";
    QVERIFY(sawSizeError);
    QCOMPARE(QSize(width->value(), height->value()), QSize(841, 1257));
    QCOMPARE(session().document()->revision(), revision);

    panel->setWorkerExecutable(worker);
    ratio->setCurrentIndex(ratio->findData(QSize(2, 3)));
    ratio->activated(ratio->currentIndex());
    QCOMPARE(QSize(width->value(), height->value()), QSize(840, 1256));
    generate(); // first handshake must not change the accepted request
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    QCOMPARE(panel->results().size(), 1);
    QCOMPARE(panel->selectedResult()->image.size(), QSize(840, 1256));
    QCOMPARE(QSize(width->value(), height->value()), QSize(840, 1256));
    QCOMPARE(session().document()->revision(), revision);
    multiple->setCurrentIndex(multiple->findData(16));
    generate();
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    QCOMPARE(panel->results().size(), 2);
    QCOMPARE(panel->selectedResult()->image.size(), QSize(832, 1248));
    width->setValue(841);
    height->setValue(1257);
    generate(); // Same early rejection with an already-loaded worker.
    QVERIFY(!panel->isBusy());
    QCOMPARE(panel->results().size(), 2);
    QCOMPARE(QSize(width->value(), height->value()), QSize(841, 1257));
    QCOMPARE(session().document()->revision(), revision);
  }
  void editOtherDocumentAndReplacement() {
    const auto originalId = window->activeDocumentId();
    const auto original = native();
    const auto second = temporary.filePath("second.png");
    QImage other(64, 64, QImage::Format_RGBA8888);
    other.fill(Qt::red);
    QVERIFY(other.save(second));
    QVERIFY(window->openImageFromPath(second));
    const auto otherId = window->activeDocumentId();
    QVERIFY(window->activateDocument(originalId));
    openMode("AIEditLayerAction");
    generate("slow");
    const auto revision = session().document()->revision();
    QVERIFY(session().execute(std::make_unique<c::SetLayerOpacityCommand>(
        *session().activeLayer(), .7F)));
    QVERIFY(session().document()->revision() != revision);
    QVERIFY(session().undo());
    QVERIFY(window->activateDocument(otherId));
    auto *otherLayer = session().document()->layer(*session().activeLayer());
    QVERIFY(session().execute(
        std::make_unique<c::SetLayerOpacityCommand>(otherLayer->id, .8F)));
    QVERIFY(window->activateDocument(originalId));
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    QCOMPARE(native(), original);
    panel->setDestination(u::DiffusionPanel::Destination::ReplaceSelected);
    panel->onApply();
    QVERIFY(native() != original);
    QVERIFY(session().undo());
    QCOMPARE(native(), original);
    QVERIFY(session().redo());
  }
  void fillPreviewAndNewDocumentProtection() {
    const auto original = u::flattenDocument(*session().document()).image;
    std::vector<std::uint8_t> coverage(64 * 64, 0);
    for (int y = 0; y < 64; ++y)
      for (int x = 16; x < 32; ++x)
        coverage[std::size_t(y) * 64 + std::size_t(x)] = 128;
    session().document()->setSelection(
        c::SelectionMask::fromR8({64, 64}, coverage, 64));
    const auto selection = session().document()->selection();
    openMode("AIGenerativeFillAction");
    generate();
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    QCOMPARE(panel->results().size(), 1);
    const auto preview = panel->selectedResult()->image;
    QCOMPARE(preview.size(), original.size());
    QCOMPARE(preview.pixelColor(0, 0), original.pixelColor(0, 0));
    QVERIFY(preview.pixelColor(20, 20) != original.pixelColor(20, 20));
    const auto originalDoc = window->activeDocumentId();
    panel->setDestination(u::DiffusionPanel::Destination::NewDocument);
    const auto appliedPreview = panel->selectedResult()->image;
    panel->onApply();
    QVERIFY(window->activeDocumentId() != originalDoc);
    const auto output = native();
    QCOMPARE(output.pixelColor(0, 0), original.pixelColor(0, 0));
    QCOMPARE(output.pixelColor(20, 20), appliedPreview.pixelColor(20, 20));
    QVERIFY(window->activateDocument(originalDoc));
    QCOMPARE(session().document()->selection(), selection);
    QCOMPARE(u::flattenDocument(*session().document()).image, original);
  }
  void transparentFillPreviewMatchesDestination() {
    const auto before = u::flattenDocument(*session().document()).image;
    session().document()->setSelection(
        c::SelectionMask::rectangle({64, 64}, {16, 16, 16, 16}));
    openMode("AIGenerativeFillAction");
    generate("transparent");
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    QCOMPARE(panel->results().size(), 1);
    QCOMPARE(panel->selectedResult()->image, before);
    panel->setDestination(u::DiffusionPanel::Destination::NewDocument);
    QCOMPARE(panel->selectedResult()->image.pixelColor(20, 20).alpha(), 0);
    QCOMPARE(panel->selectedResult()->image.pixelColor(0, 0),
             before.pixelColor(0, 0));
    panel->onApply();
    QCOMPARE(native().pixelColor(20, 20).alpha(), 0);
    QCOMPARE(native().pixelColor(0, 0), before.pixelColor(0, 0));
  }
  void configurationPromptsStayCenteredOnAiPanel() {
    openMode("AIImageEditorAction");
    child<QTabWidget>("DiffusionTabs")->setCurrentIndex(0);
    auto *configs = child<QComboBox>("DiffusionConfigurationModels");
    auto *workspace = dynamic_cast<u::OverlayDockWorkspace *>(child<QWidget>("CanvasWorkspace"));
    QVERIFY(configs && workspace);
    QVERIFY(!configs->isEnabled());
    QCOMPARE(configs->count(), 0);
    QVERIFY(!child<QComboBox>("DiffusionModels")->isEnabled());
    auto *add = child<QToolButton>("DiffusionAddConfiguration");
    QVERIFY(add && add->isEnabled() && !add->icon().isNull());
    const auto stored = [] { return QJsonDocument::fromJson(QSettings().value("diffusion/modelConfigurations").toByteArray()).array(); };
    auto centered = [&](QDialog *dialog) {
      QVERIFY(dialog && !dialog->isWindow());
      QVERIFY(!(dialog->windowFlags() & (Qt::WindowMinimizeButtonHint | Qt::WindowMaximizeButtonHint)));
      const auto center = dialog->mapToGlobal(dialog->rect().center());
      QVERIFY((center - panel->mapToGlobal(panel->rect().center())).manhattanLength() <= 2);
    };
    auto namePrompt = [&](const std::function<void()> &trigger, const QString &name, bool accepted) {
      QTimer watchdog;
      bool opened = false;
      watchdog.setSingleShot(true);
      connect(&watchdog, &QTimer::timeout, window.get(), [this] {
        if (auto *dialog = child<QInputDialog>("DiffusionConfigurationNamePrompt")) dialog->reject();
      });
      watchdog.start(5000);
      QTimer::singleShot(0, &watchdog, [&, name, accepted] {
        opened = true;
        auto *dialog = child<QInputDialog>("DiffusionConfigurationNamePrompt");
        centered(dialog);
        if (!dialog) return;
        auto *field = dialog->findChild<QLineEdit *>();
        QVERIFY(field);
        auto *buttons = dialog->findChild<QDialogButtonBox *>();
        QVERIFY(buttons);
        if (dialog->textValue().isEmpty()) QVERIFY(!buttons->button(QDialogButtonBox::Ok)->isEnabled());
        if (name == "Second") {
          dialog->setTextValue(QString::fromUtf8("First 日本語"));
          QVERIFY(!buttons->button(QDialogButtonBox::Ok)->isEnabled());
        }
        const auto position = workspace->panelOverlay()->mapFromGlobal(field->mapToGlobal(field->rect().center()));
        QTest::mouseClick(workspace->panelOverlay()->windowHandle(), Qt::LeftButton, {}, position);
        field->selectAll();
        QInputMethodEvent input;
        input.setCommitString(name);
        QCoreApplication::sendEvent(window->windowHandle(), &input);
        QCOMPARE(field->text(), name);
        if (accepted) {
          QVERIFY(buttons->button(QDialogButtonBox::Ok)->isEnabled());
          if (const auto capture = qEnvironmentVariable("VULKANA_DIFFUSION_INTEGRATION_CAPTURE"); !capture.isEmpty())
            QVERIFY(workspace->panelOverlay()->grab().save(capture + "-configuration-name.png"));
          buttons->button(QDialogButtonBox::Ok)->click();
        } else buttons->button(QDialogButtonBox::Cancel)->click();
      });
      trigger();
      QVERIFY(opened);
      QVERIFY(watchdog.isActive());
    };
    const auto newConfig = [&] {
      const auto position = workspace->panelOverlay()->mapFromGlobal(add->mapToGlobal(add->rect().center()));
      QTest::mouseClick(workspace->panelOverlay()->windowHandle(), Qt::LeftButton, {}, position);
    };
    const auto initial = panel->configuration();
    namePrompt(newConfig, "Abandoned", false);
    QCOMPARE(panel->configuration(), initial);
    QCOMPARE(stored().size(), 0);
    namePrompt(newConfig, "First 日本語", true);
    QCOMPARE(panel->configurationName(), QString::fromUtf8("First 日本語"));
    QCOMPARE(stored().size(), 1);
    QVERIFY(configs->isEnabled());
    QCOMPARE(configs->count(), 1);
    QCOMPARE(configs->currentIndex(), 0);
    QVERIFY(child<QComboBox>("DiffusionModels")->isEnabled());
    QVERIFY(panel->configuration()["files"].toObject().isEmpty());
    auto *weights = child<QLineEdit>("DiffusionFile_diffusion");
    weights->setText(modelPath);
    QTRY_COMPARE(stored()[0].toObject()["configuration"].toObject()["files"].toObject()["diffusion"].toString(), modelPath);
    const auto firstId = configs->currentData();
    namePrompt([&] { child<QToolButton>("DiffusionDuplicateConfiguration")->click(); }, "Second", true);
    QCOMPARE(stored().size(), 2);
    QCOMPARE(panel->configurationName(), QString("Second"));
    QCOMPARE(weights->text(), modelPath);
    const auto secondId = configs->currentData();
    QVERIFY(firstId != secondId);
    weights->setText(modelPath + ".copy");
    configs->setCurrentIndex(configs->findData(firstId));
    QCOMPARE(weights->text(), modelPath);
    namePrompt(newConfig, "Cancelled again", false);
    QCOMPARE(configs->currentData(), firstId);
    QCOMPARE(stored().size(), 2);
    auto remove = [&](bool yes) {
      QTimer::singleShot(0, window.get(), [&, yes] {
        auto *dialog = child<QMessageBox>("DiffusionConfigurationDeletePrompt");
        centered(dialog);
        if (!dialog) return;
        QCOMPARE(dialog->defaultButton(), dialog->button(QMessageBox::No));
        dialog->button(yes ? QMessageBox::Yes : QMessageBox::No)->click();
      });
      child<QToolButton>("DiffusionDeleteConfiguration")->click();
    };
    remove(false);
    QCOMPARE(stored().size(), 2);
    QCOMPARE(configs->currentData(), firstId);
    remove(true);
    QCOMPARE(stored().size(), 1);
    QCOMPARE(configs->currentData(), secondId);
    QCOMPARE(weights->text(), modelPath + ".copy");
    remove(true);
    QVERIFY(stored().isEmpty());
    QVERIFY(!configs->isEnabled());
    QCOMPARE(configs->count(), 0);
    QVERIFY(!child<QComboBox>("DiffusionModels")->isEnabled());
    QVERIFY(add->isEnabled());
    QVERIFY(!child<QToolButton>("DiffusionDeleteConfiguration")->isEnabled());
    if (const auto capture = qEnvironmentVariable("VULKANA_DIFFUSION_INTEGRATION_CAPTURE"); !capture.isEmpty())
      QVERIFY(panel->grab().save(capture + "-configuration-empty.png"));
    QVERIFY(child<QWidget>("DiffusionSessionDivider"));
  }
  void cancellationFailureCloseAndRetarget() {
    openMode("AIEditLayerAction");
    auto *action = child<QPushButton>("DiffusionGenerate");
    QVERIFY(action && action->isEnabled());
    QCOMPARE(action->text(), QString("Generate"));
    prompt("A green studio object");
    action->click();
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    QCOMPARE(panel->results().size(), 1);
    generate("slow");
    QVERIFY(panel->isBusy());
    QCOMPARE(action->text(), QString("Cancel"));
    QVERIFY(action->isEnabled());
    action->click();
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    QCOMPARE(action->text(), QString("Generate"));
    QVERIFY(action->isEnabled());
    QCOMPARE(panel->results().size(), 1);
    generate("fail");
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    QCOMPARE(panel->results().size(), 1);
    QCOMPARE(action->text(), QString("Generate"));
    prompt("A green studio object");
    action->click();
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    QCOMPARE(panel->results().size(), 2);
    const auto first = window->activeDocumentId();
    const auto second = temporary.filePath("retarget.png");
    QImage image(96, 64, QImage::Format_RGBA8888);
    image.fill(Qt::yellow);
    QVERIFY(image.save(second));
    QVERIFY(window->openImageFromPath(second));
    const auto secondId = window->activeDocumentId();
    openMode("AIEditLayerAction");
    QCOMPARE(panel->inputs().front().image.size(), QSize(96, 64));
    QCOMPARE(panel->results().size(), 2);
    QVERIFY(window->activateDocument(first));
    QVERIFY(window->closeDocument(secondId));
    QVERIFY(!panel->isBusy());
    openMode("AIEditLayerAction");
    auto *shell = child<QWidget>("AIImageEditorWorkspace");
    QVERIFY(shell);
    generate("slow");
    shell->close();
  }
  void staleResultAndInputOrdering() {
    openMode("AIEditLayerAction");
    generate();
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    panel->onCancel();
    const auto id = *session().activeLayer();
    QVERIFY(session().execute(
        std::make_unique<c::SetLayerOpacityCommand>(id, .6F)));
    const auto revision = session().document()->revision();
    panel->setDestination(u::DiffusionPanel::Destination::ReplaceSelected);
    panel->onApply();
    QVERIFY(session().document()->revision() != revision);
    QCOMPARE(panel->results().size(), 1);
    const auto previous = panel->results().front().image;
    generate();
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    QCOMPARE(panel->results().size(), 2);
    QCOMPARE(panel->results().front().image, previous);
    panel->onDiscard();
    QCOMPARE(panel->results().size(), 1);
    openMode("AIGenerateImageAction");
    QImage a(32, 64, QImage::Format_RGBA8888),
        b(64, 32, QImage::Format_RGBA8888);
    a.fill(Qt::red);
    b.fill(Qt::blue);
    panel->setInputs(
        {{b, "B", "Test", false, {}}, {a, "A", "Test", false, {}}});
    generate();
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    const auto refs = panel->selectedResult()
                          ->metadata.value("request")
                          .toObject()
                          .value("referenceImages")
                          .toArray();
    QCOMPARE(refs.size(), 2);
    QCOMPARE(QImage(refs[0].toString()).size(), b.size());
    QCOMPARE(QImage(refs[1].toString()).pixelColor(0, 0), a.pixelColor(0, 0));
  }
  void changedTargetCanReplaceOrAddWithoutLosingHistory() {
    openMode("AIEditLayerAction");
    generate();
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    const auto generated = panel->selectedResult()->image;
    const auto original = native();
    const auto targetId = *session().activeLayer();
    QVERIFY(session().execute(
        std::make_unique<c::SetLayerOpacityCommand>(targetId, .5F)));
    panel->setDestination(u::DiffusionPanel::Destination::ReplaceSelected);
    panel->onApply();
    QCOMPARE(native(), generated);
    QCOMPARE(session().document()->layer(targetId)->opacity, .5F);
    QCOMPARE(panel->results().size(), 1);
    QVERIFY(session().undo());
    QCOMPARE(native(), original);
    QVERIFY(session().redo());
    QCOMPARE(native(), generated);
    const auto count = session().document()->layers().size();
    panel->setDestination(u::DiffusionPanel::Destination::NewLayer);
    panel->onApply();
    QCOMPARE(session().document()->layers().size(), count + 1);
    QCOMPARE(panel->results().size(), 1);
    QVERIFY(session().undo());
    QCOMPARE(session().document()->layers().size(), count);
    panel->onClose();
    openMode("AIGenerateImageAction");
    QCOMPARE(panel->results().size(), 1);
    panel->setDestination(u::DiffusionPanel::Destination::NewDocument);
    const auto docs = window->documentCount();
    panel->onApply();
    QCOMPARE(window->documentCount(), docs + 1);
    QCOMPARE(panel->results().size(), 1);
  }
  void confirmedFillPreservesCurrentPixelsOutsideOriginalSelection() {
    std::vector<std::uint8_t> mask(64 * 64, 0);
    for (int y = 0; y < 64; ++y)
      for (int x = 16; x < 32; ++x)
        mask[y * 64 + x] = 128;
    session().document()->setSelection(
        c::SelectionMask::fromR8({64, 64}, mask, 64));
    openMode("AIGenerativeFillAction");
    generate();
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    auto *doc = session().document();
    auto now = u::captureDiffusionTarget(*doc, *session().activeLayer(), false);
    QImage changed(64, 64, QImage::Format_RGBA8888);
    changed.fill(QColor(250, 180, 20));
    QVERIFY(session().execute(
        u::diffusionReplaceCommand(*doc, now, changed, false, false)));
    const auto currentPixels = native();
    const auto newSelection =
        c::SelectionMask::rectangle({64, 64}, {40, 40, 8, 8});
    doc->setSelection(newSelection);
    panel->setDestination(u::DiffusionPanel::Destination::ReplaceSelected);
    panel->onApply();
    const auto after = native();
    QCOMPARE(after.pixelColor(0, 0), changed.pixelColor(0, 0));
    QCOMPARE(after.pixelColor(44, 44), changed.pixelColor(44, 44));
    QVERIFY(after.pixelColor(20, 20) != changed.pixelColor(20, 20));
    QCOMPARE(doc->selection(), newSelection);
    QVERIFY(session().undo());
    QCOMPARE(native(), currentPixels);
    QVERIFY(session().redo());
    QCOMPARE(native(), after);
    QCOMPARE(panel->results().size(), 1);
  }
  void modeControlsChooseTheInferencePath() {
    openMode("AIEditLayerAction");
    auto *denoise = child<QCheckBox>("DiffusionImageToImage");
    auto *strength = child<QDoubleSpinBox>("Diffusion_strength");
    QVERIFY(denoise && strength);
    strength->setValue(.75);
    generate();
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    auto request = panel->selectedResult()->metadata["request"].toObject();
    QCOMPARE(request["referenceImages"].toArray().size(), 1);
    QVERIFY(!request.contains("initialImage"));
    QVERIFY(!request.contains("strength"));
    QVERIFY(!request.contains("mask"));
    denoise->setChecked(true);
    generate();
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    request = panel->selectedResult()->metadata["request"].toObject();
    QCOMPARE(QImage(request["initialImage"].toString()).size(), QSize(64, 64));
    QCOMPARE(request["strength"].toDouble(), .75);
    QVERIFY(!request.contains("mask"));
    session().document()->setSelection(
        c::SelectionMask::rectangle({64, 64}, {16, 16, 32, 32}));
    openMode("AIGenerativeFillAction");
    strength->setValue(1);
    generate();
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    request = panel->selectedResult()->metadata["request"].toObject();
    QVERIFY(request.contains("initialImage"));
    QVERIFY(request.contains("mask"));
    QCOMPARE(request["strength"].toDouble(), 1.0);
    openMode("AIGenerateImageAction");
    generate();
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    request = panel->selectedResult()->metadata["request"].toObject();
    QVERIFY(!request.contains("initialImage"));
    QVERIFY(!request.contains("strength"));
    QVERIFY(!request.contains("mask"));
  }
  void fillUsesCanvasSpaceBeyondLayerBounds() {
    panel->setMaxInputSize(8192); // This check inspects native full-canvas snapshots.
    const auto project = qEnvironmentVariable("VULKANA_DIFFUSION_TEST_PROJECT");
    if (!project.isEmpty())
      QVERIFY(window->openImageFromPath(project));
    else {
      auto canvas = session().document()->canvas();
      canvas.extent = {96, 64};
      QVERIFY(session().document()->setCanvas(canvas));
      QVERIFY(session().document()->setLayerTransform(*session().activeLayer(),
                                                      {1, 0, 16, 0, 1, 0}));
    }
    auto *doc = session().document();
    const auto size = doc->canvas().extent;
    for (const auto &layer : doc->layers()) {
      if (const auto *raster = std::get_if<c::RasterLayer>(&layer.payload);
          raster && raster->surface->extent().width < size.width) {
        session().setActiveLayer(layer.id);
        break;
      }
    }
    const auto originalLayer = *session().activeLayer();
    const auto original = native();
    const int margin = (int(size.width) - original.width()) / 2;
    QVERIFY(margin > 0);
    std::vector<std::uint8_t> coverage(std::size_t(size.width) * size.height, 0);
    for (unsigned y = 0; y < size.height; ++y)
      for (unsigned x = 0; x < size.width; ++x)
        if (x < unsigned(margin + 4) || x >= size.width - unsigned(margin + 4))
          coverage[std::size_t(y) * size.width + x] = 255;
    QVERIFY(doc->setSelection(c::SelectionMask::fromR8(size, coverage, size.width)));
    const auto selection = doc->selection();
    const auto revision = doc->revision();
    openMode("AIGenerativeFillAction");
    panel->clearInputs();
    panel->onAddInput(u::DiffusionPanel::InputSource::VisibleCanvas);
    const auto inputSnapshot = panel->inputs().front().image.copy();
    generate();
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    QCOMPARE(doc->revision(), revision);
    const auto request = panel->selectedResult()->metadata["request"].toObject();
    const QSize canvasSize(int(size.width), int(size.height));
    const QImage initial(request["initialImage"].toString());
    const QImage reference(request["referenceImages"].toArray().first().toString());
    const QImage mask(request["mask"].toString());
    QCOMPARE(initial.size(), canvasSize);
    QCOMPARE(reference.size(), canvasSize);
    QCOMPARE(mask.size(), canvasSize);
    QCOMPARE(initial.pixelColor(0, 0), QColor(Qt::white));
    QCOMPARE(reference.pixelColor(0, 0), QColor(Qt::white));
    QCOMPARE(panel->inputs().front().image, inputSnapshot);
    if (project.isEmpty())
      QCOMPARE(inputSnapshot.pixelColor(0, 0).alpha(), 0);
    QCOMPARE(mask.pixelColor(0, 0).red(), 255);
    QCOMPARE(mask.pixelColor(canvasSize.width() - 1, 0).red(), 255);
    QCOMPARE(mask.pixelColor(canvasSize.width() / 2, 0).red(), 0);
    const auto capture = qEnvironmentVariable("VULKANA_DIFFUSION_INTEGRATION_CAPTURE");
    if (!capture.isEmpty()) {
      QVERIFY(initial.save(capture + "-fill-initial.png"));
      QVERIFY(reference.save(capture + "-fill-reference.png"));
      QVERIFY(mask.save(capture + "-fill-mask.png"));
    }
    QCOMPARE(panel->selectedResult()->image.size(), canvasSize);
    QCOMPARE(panel->selectedResult()->image.pixelColor(0, 0), QColor(30, 200, 90));
    panel->setDestination(u::DiffusionPanel::Destination::NewLayer);
    panel->onApply();
    const auto result = *session().activeLayer();
    QVERIFY(result != originalLayer);
    const auto pixels = native();
    QCOMPARE(pixels.size(), canvasSize);
    QCOMPARE(pixels.pixelColor(0, 0), QColor(30, 200, 90));
    QCOMPARE(pixels.pixelColor(canvasSize.width() - 1, 0), QColor(30, 200, 90));
    QCOMPARE(pixels.pixelColor(canvasSize.width() / 2, 0).alpha(), 0);
    QCOMPARE(doc->layer(result)->localToDocument, c::AffineTransform{});
    QCOMPARE(u::diffusionRawLayer(*doc->layer(originalLayer)), original);
    QCOMPARE(doc->selection(), selection);
    QVERIFY(session().undo());
    QVERIFY(!doc->layer(result));
    QVERIFY(session().redo());
    QCOMPARE(u::diffusionRawLayer(*doc->layer(result)), pixels);
  }
  void modeControlsAcceptNativeKeyboard() {
    openMode("AIEditLayerAction");
    auto *denoise = child<QCheckBox>("DiffusionImageToImage");
    auto *strength = child<QDoubleSpinBox>("Diffusion_strength");
    auto *scroll = child<QScrollArea>("DiffusionGenerationPage");
    QVERIFY(denoise && strength && scroll);
    scroll->ensureWidgetVisible(denoise);
    QCoreApplication::processEvents();
    QTest::mouseClick(denoise, Qt::LeftButton, {}, QPoint(8, denoise->height() / 2));
    QVERIFY(denoise->isChecked());
    scroll->ensureWidgetVisible(strength);
    QCoreApplication::processEvents();
    QTest::mouseClick(strength, Qt::LeftButton);
    QTRY_VERIFY(strength->hasFocus());
    const auto tool = session().activeTool();
    const auto revision = session().document()->revision();
    for (const auto &[code, modifiers, text] :
         std::initializer_list<std::tuple<int, Qt::KeyboardModifiers, QString>>{
             {Qt::Key_A, Qt::ControlModifier, {}},
             {Qt::Key_1, Qt::NoModifier, "1"},
             {Qt::Key_Return, Qt::NoModifier, {}}}) {
      QKeyEvent press(QEvent::KeyPress, code, modifiers, text);
      QCoreApplication::sendEvent(window->windowHandle(), &press);
      QKeyEvent release(QEvent::KeyRelease, code, modifiers, text);
      QCoreApplication::sendEvent(window->windowHandle(), &release);
    }
    QCOMPARE(strength->value(), 1.0);
    QCOMPARE(session().activeTool(), tool);
    QCOMPARE(session().document()->revision(), revision);
  }
  void inputRegionsReachWorkerIndependently() {
    openMode("AIGenerateImageAction");
    QImage red(120, 60, QImage::Format_RGBA8888), blue(60, 120, QImage::Format_RGBA8888);
    red.fill(Qt::red); blue.fill(Qt::blue);
    panel->setInputs({{red, "First cats", "Test"}, {blue, "Second cats", "Test"}});
    const auto firstId = panel->inputs()[0].id, secondId = panel->inputs()[1].id;
    QVERIFY(panel->setInputRegions(firstId, {{QRectF(.1, .2, .3, .4), "Region1"}}));
    QVERIFY(panel->setInputRegions(secondId, {{QRectF(.2, .1, .5, .3), "Region1"}}));
    auto *list = child<QListWidget>("DiffusionInputsList");
    QVERIFY(list->model()->moveRows({}, 0, 1, {}, 2));
    const auto revision = session().document()->revision();
    generate("Give <Image1/Region1> the color of <Image2/Region1>.");
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    QCOMPARE(panel->results().size(), 1);
    QCOMPARE(session().document()->revision(), revision);
    auto request = panel->selectedResult()->metadata["request"].toObject();
    auto spatial = request["spatial"].toObject();
    QCOMPARE(spatial["version"].toInt(), 2);
    QVERIFY(spatial["regions"].toArray().isEmpty());
    QVERIFY(!spatial.contains("annotationImage"));
    QCOMPARE(spatial["inputRegions"].toArray().size(), 2);
    QCOMPARE(spatial["inputRegions"].toArray()[0].toObject()["image"].toInt(), 1);
    QCOMPARE(spatial["inputRegions"].toArray()[0].toObject()["regions"].toArray()[0].toObject()["x"].toDouble(), .2);
    QCOMPARE(QImage(request["referenceImages"].toArray()[0].toString()).pixelColor(0, 0), QColor(Qt::blue));
    QCOMPARE(QImage(request["referenceImages"].toArray()[1].toString()).pixelColor(0, 0), QColor(Qt::red));
    panel->setRegions({{QRectF(.1, .1, .6, .6), "Output subject"}});
    generate();
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    request = panel->selectedResult()->metadata["request"].toObject();
    spatial = request["spatial"].toObject();
    QCOMPARE(spatial["inputRegions"].toArray().size(), 2);
    QVERIFY(QFileInfo::exists(spatial["annotationImage"].toString()));
    QCOMPARE(session().document()->revision(), revision);
  }
  void fluxNativeRegionsAndOptionalCrops() {
    openMode("AIEditLayerAction");
    panel->setConfiguration({{"profile", "flux3-image-klein-distilled"},
      {"files", QJsonObject{{"diffusion", modelPath}, {"textEncoder", modelPath}, {"vae", modelPath}}}});
    QVERIFY(panel->setInputRegions(panel->inputs()[0].id, {{QRectF(.1, .2, .3, .4), "Region1"}}));
    const auto revision = session().document()->revision();
    generate("In <Image1>, change <Region1>.");
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    QVERIFY(panel->selectedResult());
    auto request = panel->selectedResult()->metadata["request"].toObject();
    QVERIFY(!request["spatial"].toObject()["cropGuidance"].toBool());
    QCOMPARE(request["referenceImages"].toArray().size(), 1);
    QCOMPARE(request["prompt"].toString(), QString("In <Image1>, change <Region1>."));
    panel->findChild<QCheckBox *>("DiffusionRegionCropGuidance")->click();
    generate("In <Image1>, change <Image1/Region1>.");
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    request = panel->selectedResult()->metadata["request"].toObject();
    QVERIFY(request["spatial"].toObject()["cropGuidance"].toBool());
    QCOMPARE(session().document()->revision(), revision);
  }
  void regionDescriptionAndDestinationThroughOwnedUi() {
    openMode("AIGenerateImageAction");
    panel->setConfiguration({{"profile", "flux3-image-klein-distilled"},
      {"files", QJsonObject{{"diffusion", modelPath}, {"textEncoder", modelPath}, {"vae", modelPath}}}});
    QImage room(128, 128, QImage::Format_RGBA8888), donor(128, 128, QImage::Format_RGBA8888);
    room.fill(Qt::red); donor.fill(Qt::blue);
    panel->setInputs({{room, "Room", "Test"}, {donor, "Cats", "Test"}});
    const auto sourceId = panel->inputs()[1].id;
    QVERIFY(panel->setOutputSceneId(panel->inputs()[0].id));
    panel->setResolution({128, 128});
    QVERIFY(panel->setInputRegions(sourceId, {{QRectF(.1, .1, .3, .5), "Region1", "cat-source"}}));
    const auto revision = session().document()->revision();
    panel->showInputRegions(sourceId);
    QPointer<QDialog> sourceDialog = child<QDialog>("DiffusionPreviewDialog");
    auto *preview = dynamic_cast<u::DiffusionPreview *>(sourceDialog->findChild<QWidget *>("DiffusionPreview"));
    preview->setSelectedRegion(0);
    auto *description = sourceDialog->findChild<QLineEdit *>("DiffusionRegionDescription");
    QVERIFY(description && description->isEnabled());
    QTest::mouseClick(description, Qt::LeftButton);
    QTRY_VERIFY(description->hasFocus());
    QKeyEvent typed(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, "a kitten");
    QCoreApplication::sendEvent(sourceDialog->windowHandle(), &typed);
    QCOMPARE(description->text(), QString("a kitten"));
    QVERIFY(description->hasFocus());
    // Exercise the native canvas-host IME bridge after focusing the owned popup.
    QInputMethodEvent ime; ime.setCommitString(QString::fromUtf8(" 日本語"));
    QCoreApplication::sendEvent(window->windowHandle(), &ime);
    QCOMPARE(panel->inputs()[1].regions[0].description, QString::fromUtf8("a kitten 日本語"));
    if (const auto path = qEnvironmentVariable("VULKANA_DIFFUSION_PANEL_CAPTURE"); !path.isEmpty())
      QVERIFY(sourceDialog->grab().save(path + "-source.png"));
    auto *dest = sourceDialog->findChild<QComboBox *>("DiffusionRegionDestination");
    QVERIFY(dest && dest->isVisible());
    dest->setCurrentIndex(dest->findData("__new_target__"));
    dest->activated(dest->currentIndex());
    QTRY_VERIFY(sourceDialog.isNull());
    auto *targetDialog = child<QDialog>("DiffusionPreviewDialog");
    QVERIFY(targetDialog && targetDialog->parentWidget());
    QVERIFY(targetDialog->windowFlags() & Qt::FramelessWindowHint);
    QVERIFY(!(targetDialog->windowFlags() & Qt::WindowMinMaxButtonsHint));
    preview = dynamic_cast<u::DiffusionPreview *>(targetDialog->findChild<QWidget *>("DiffusionPreview"));
    QCoreApplication::processEvents();
    const auto frame = preview->imageRect();
    const auto start = (frame.topLeft() + QPointF(frame.width() * .6, frame.height() * .6)).toPoint();
    const auto end = (frame.topLeft() + QPointF(frame.width() * .9, frame.height() * .9)).toPoint();
    QTest::mousePress(preview, Qt::LeftButton, {}, start);
    QTest::mouseMove(preview, end);
    QTest::keyClick(preview, Qt::Key_Escape);
    QVERIFY(panel->regions().isEmpty());
    QVERIFY(panel->inputs()[1].regions[0].destinationId.isEmpty());
    QTest::mousePress(preview, Qt::LeftButton, {}, start);
    QTest::mouseMove(preview, end);
    QTest::mouseRelease(preview, Qt::LeftButton, {}, end);
    QCOMPARE(panel->regions().size(), 1);
    QVERIFY(panel->regions()[0].targetOnly);
    QCOMPARE(panel->inputs()[1].regions[0].destinationId, panel->regions()[0].id);
    QVERIFY(!targetDialog->findChild<QLineEdit *>("DiffusionRegionDescription")->isEnabled());
    if (const auto path = qEnvironmentVariable("VULKANA_DIFFUSION_PANEL_CAPTURE"); !path.isEmpty())
      QVERIFY(targetDialog->grab().save(path + "-target.png"));
    targetDialog->close();
    generate("Use <Scene> as the output scene. Transfer <Image2/Region1> to its target position.");
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    QVERIFY(panel->selectedResult());
    const auto request = panel->selectedResult()->metadata["request"].toObject();
    const auto spatial = request["spatial"].toObject();
    QCOMPARE(spatial["version"].toInt(), 3);
    QCOMPARE(spatial["outputScene"].toInt(), 1);
    const auto region = spatial["inputRegions"].toArray()[0].toObject()["regions"].toArray()[0].toObject();
    QCOMPARE(region["description"].toString(), QString::fromUtf8("a kitten 日本語"));
    QCOMPARE(region["targetId"].toString(), spatial["regions"].toArray()[0].toObject()["id"].toString());
    QCOMPARE(session().document()->revision(), revision);
  }
  void fluxRejectsLegacyRegionWorker() {
    qputenv("VULKANA_TEST_DIFFUSION_SPATIAL_V1", "1");
    const auto restore = qScopeGuard([]{qunsetenv("VULKANA_TEST_DIFFUSION_SPATIAL_V1");});
    openMode("AIGenerateImageAction");
    panel->setConfiguration({{"profile", "flux3-image-klein-distilled"},
      {"files", QJsonObject{{"diffusion", modelPath}, {"textEncoder", modelPath}, {"vae", modelPath}}}});
    const auto revision = session().document()->revision();
    generate("A cat");
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    QVERIFY(panel->results().isEmpty());
    QCOMPARE(session().document()->revision(), revision);
  }
  void croppedLayerInputMatchesSizeAndKeepsOutsidePixels() {
    auto *doc = session().document();
    const auto id = *session().activeLayer();
    QVERIFY(doc->setLayerTransform(id, {1, 0, -32, 0, 1, 0}));
    const auto selection = c::SelectionMask::rectangle({64, 64}, {50, 50, 8, 8});
    QVERIFY(doc->setSelection(selection));
    const auto before = native();
    openMode("AIEditLayerAction");
    const auto inputId = panel->inputs().front().id;
    QCOMPARE(panel->inputs().front().sourceCanvas, QSize(64, 64));
    QVERIFY(panel->setInputCropToCanvas(inputId, true));
    auto *inputs = child<QListWidget>("DiffusionInputsList");
    inputs->setCurrentRow(0);
    auto *match = child<QToolButton>("DiffusionMatchSelectedSize");
    QVERIFY(match);
    match->click();
    QCOMPARE(panel->request()["width"].toInt(), 32);
    QCOMPARE(panel->request()["height"].toInt(), 64);
    generate();
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    const auto request = panel->selectedResult()->metadata["request"].toObject();
    QCOMPARE(QImage(request["referenceImages"].toArray()[0].toString()).size(), QSize(32, 64));
    QCOMPARE(native(), before);
    QCOMPARE(panel->selectedResult()->toDocument.map(QPointF{}), QPointF{});
    panel->setDestination(u::DiffusionPanel::Destination::ReplaceSelected);
    panel->onApply();
    const auto after = native();
    QCOMPARE(after.size(), before.size());
    QCOMPARE(after.pixelColor(0, 0), before.pixelColor(0, 0));
    QCOMPARE(after.pixelColor(31, 0), before.pixelColor(31, 0));
    QCOMPARE(after.pixelColor(32, 0), QColor(30, 200, 90));
    QCOMPARE(after.pixelColor(63, 63), QColor(30, 200, 90));
    QCOMPARE(doc->selection(), selection);
    QVERIFY(session().undo());
    QCOMPARE(native(), before);
    QVERIFY(session().redo());
    QCOMPARE(native(), after);
  }
  void limitedCanvasReferenceRetainsFillPreparation() {
    auto *doc = session().document();
    const auto id = *session().activeLayer();
    auto canvas = doc->canvas(); canvas.extent = {128, 128};
    QVERIFY(doc->setCanvas(canvas));
    QVERIFY(doc->setLayerTransform(id, {1, 0, 32, 0, 1, 32}));
    QVERIFY(doc->setSelection(c::SelectionMask::filled({128, 128}, 255)));
    openMode("AIGenerativeFillAction");
    panel->setMaxInputSize(64);
    generate();
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    const auto request = panel->selectedResult()->metadata["request"].toObject();
    const QImage reference(request["referenceImages"].toArray()[0].toString());
    QCOMPARE(reference.size(), QSize(64, 64));
    QCOMPARE(reference.pixelColor(0, 0), QColor(Qt::white));
    QCOMPARE(QImage(request["initialImage"].toString()).size(), QSize(128, 128));
    QCOMPARE(request["referenceMaxSize"].toInt(), 64);
    QCOMPARE(panel->inputs()[0].image.pixelColor(0, 0).alpha(), 0);
  }
  void inputRegionLabelsAcceptNativeKeyboard() {
    openMode("AIEditLayerAction");
    const auto id = panel->inputs().front().id;
    QVERIFY(panel->setInputRegions(id, {{QRectF(.1, .1, .5, .5), "Region1"}}));
    panel->showInputRegions(id);
    auto *dialog = child<QDialog>("DiffusionPreviewDialog");
    QVERIFY(dialog);
    auto *preview = dynamic_cast<u::DiffusionPreview *>(child<QWidget>("DiffusionPreview"));
    QVERIFY(preview);
    preview->setSelectedRegion(0);
    auto *label = child<QLineEdit>("DiffusionRegionLabel");
    QVERIFY(label);
    QTest::mouseClick(label, Qt::LeftButton);
    QTRY_VERIFY(label->hasFocus());
    label->selectAll();
    QKeyEvent press(QEvent::KeyPress, Qt::Key_C, Qt::NoModifier, "Cat");
    QCoreApplication::sendEvent(dialog->windowHandle(), &press);
    QKeyEvent release(QEvent::KeyRelease, Qt::Key_C, Qt::NoModifier, "Cat");
    QCoreApplication::sendEvent(dialog->windowHandle(), &release);
    QCOMPARE(panel->inputs().front().regions[0].label, QString("Cat"));
    QVERIFY(panel->regions().isEmpty());
    QCOMPARE(panel->inputs().front().id, id);
    dialog->close();
  }
  void inputRegionsRejectAnOlderWorkerCleanly() {
    const auto previous = qgetenv("VULKANA_TEST_DIFFUSION_SPATIAL_V1");
    const auto restore = qScopeGuard([previous] {
      if (previous.isNull()) qunsetenv("VULKANA_TEST_DIFFUSION_SPATIAL_V1");
      else qputenv("VULKANA_TEST_DIFFUSION_SPATIAL_V1", previous);
    });
    qputenv("VULKANA_TEST_DIFFUSION_SPATIAL_V1", "1");
    openMode("AIEditLayerAction");
    const auto id = panel->inputs().front().id;
    QVERIFY(panel->setInputRegions(id, {{QRectF(.1, .1, .4, .4), "Region1"}}));
    const auto revision = session().document()->revision();
    generate();
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    QVERIFY(panel->results().isEmpty());
    QCOMPARE(session().document()->revision(), revision);
    QVERIFY(panel->setInputRegions(id, {}));
    generate();
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    QCOMPARE(panel->results().size(), 1); // existing editing still works on schema 1
  }
  void historySurvivesDocumentClosureAndModeChanges() {
    const auto owner = window->activeDocumentId();
    openMode("AIEditLayerAction");
    generate();
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    const auto originalResult = panel->selectedResult()->image;
    const auto resultPath = panel->selectedResult()
                                ->metadata["image"]
                                .toObject()["path"]
                                .toString();
    const auto fresh = temporary.filePath("history-target.png");
    QImage other(96, 64, QImage::Format_RGBA8888);
    other.fill(Qt::red);
    QVERIFY(other.save(fresh));
    QVERIFY(window->openImageFromPath(fresh));
    QVERIFY(window->closeDocument(owner));
    QVERIFY(QFileInfo::exists(resultPath));
    QCOMPARE(panel->results().size(), 1);
    openMode("AIGenerateImageAction");
    generate();
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    QCOMPARE(panel->results().size(), 2);
    panel->findChild<QListWidget *>("DiffusionResultsList")->setCurrentRow(0);
    QCOMPARE(panel->selectedResult()->image, originalResult);
    panel->setDestination(u::DiffusionPanel::Destination::NewLayer);
    const auto count = session().document()->layers().size();
    panel->onApply();
    QCOMPARE(session().document()->layers().size(), count + 1);
    QCOMPARE(native(), originalResult);
    panel->onDiscard();
    QCOMPARE(panel->results().size(), 1);
    panel->onDiscard();
    QCOMPARE(panel->results().size(), 0);
    QVERIFY(!QFileInfo::exists(resultPath));
  }
  void panelIsFixedAndDoesNotBlockIdleCanvas() {
    openMode("AIGenerateImageAction");
    auto *shell = child<QWidget>("AIImageEditorWorkspace");
    QVERIFY(shell && !shell->isWindow());
    QVERIFY(shell->parentWidget() != window.get());
    QVERIFY(shell->height() > window->height() * .7);
    const auto before = session().document()->revision();
    QVERIFY(session().execute(std::make_unique<c::SetLayerOpacityCommand>(
        *session().activeLayer(), .7F)));
    QVERIFY(session().document()->revision() != before);
    if (const auto path =
            qEnvironmentVariable("VULKANA_DIFFUSION_INTEGRATION_CAPTURE");
        !path.isEmpty()) {
      QTest::qWait(50);
      QVERIFY(shell->grab().save(path + "-generation.png"));
      auto *tabs = panel->findChild<QTabWidget *>("DiffusionTabs");
      tabs->setCurrentIndex(0);
      QTest::qWait(50);
      QVERIFY(shell->grab().save(path + "-configuration.png"));
      tabs->setCurrentIndex(1);
    }
  }
  void expandedPreviewOwnsInputAndApplies() {
    auto *right = child<QSplitter>("RightPanelSplitter");
    auto *left = child<QSplitter>("LeftPanelSplitter");
    QVERIFY(right && left && right->isVisible() && left->isVisible());
    const auto sizes = right->sizes();
    openMode("AIGenerateImageAction");
    QVERIFY(!right->isVisible());
    QVERIFY(left->isVisible());
    generate();
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    auto *tabs = panel->findChild<QTabWidget *>("DiffusionTabs");
    QCOMPARE(tabs->currentIndex(), 1);
    generate();
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    QCOMPARE(panel->results().size(), 2);
    if (const auto path = qEnvironmentVariable("VULKANA_DIFFUSION_INTEGRATION_CAPTURE"); !path.isEmpty()) {
      panel->findChild<QScrollArea *>("DiffusionGenerationPage")
          ->ensureWidgetVisible(child<QLabel>("DiffusionGenerationThumbnail"));
      QCoreApplication::processEvents();
      QVERIFY(child<QWidget>("AIImageEditorWorkspace")->grab().save(path + "-latest-generation.png"));
      tabs->setCurrentIndex(2);
      QCoreApplication::processEvents();
      QVERIFY(child<QWidget>("AIImageEditorWorkspace")->grab().save(path + "-image-details.png"));
      tabs->setCurrentIndex(1);
    }
    const auto revision = session().document()->revision();
    auto *shell = child<QWidget>("AIImageEditorWorkspace");
    const auto aiGeometry = shell->geometry();
    const auto aiParent = shell->parentWidget();
    panel->showResultPreview();
    QVERIFY(shell->isVisible());
    QCOMPARE(shell->geometry(), aiGeometry);
    QCOMPARE(shell->parentWidget(), aiParent);
    QVERIFY(!right->isVisible());
    auto *dialog = child<QDialog>("DiffusionPreviewDialog");
    QVERIFY(dialog && !dialog->isWindow());
    QVERIFY(!(dialog->windowFlags() & (Qt::WindowMinimizeButtonHint | Qt::WindowMaximizeButtonHint)));
    auto *preview = child<QWidget>("DiffusionPreview");
    QVERIFY(preview);
    preview->setFocus();
    QTRY_VERIFY(preview->hasFocus());
    // Exercise the actual native host-window bridge, not just a widget key.
    QKeyEvent previous(QEvent::KeyPress, Qt::Key_Left, {});
    QCoreApplication::sendEvent(window->windowHandle(), &previous);
    QCOMPARE(panel->selectedResultIndex(), 0);
    child<QPushButton>("DiffusionCompareLayer")->setFocus();
    QKeyEvent next(QEvent::KeyPress, Qt::Key_Right, {});
    QCoreApplication::sendEvent(window->windowHandle(), &next);
    QCOMPARE(panel->selectedResultIndex(), 1);
    QKeyEvent down(QEvent::KeyPress, Qt::Key_Down, {});
    QCoreApplication::sendEvent(window->windowHandle(), &down);
    QCOMPARE(session().document()->revision(), revision);
    if (const auto path = qEnvironmentVariable("VULKANA_DIFFUSION_INTEGRATION_CAPTURE"); !path.isEmpty()) {
      QVERIFY(dialog->grab().save(path + "-result-overlay.png"));
    }
    const auto count = session().document()->layers().size();
    panel->setDestination(u::DiffusionPanel::Destination::NewLayer);
    child<QPushButton>("DiffusionApply")->click();
    QCOMPARE(session().document()->layers().size(), count + 1);
    QVERIFY(!dialog->isVisible());
    QVERIFY(child<QWidget>("AIImageEditorWorkspace")->isVisible());
    QVERIFY(!right->isVisible());
    panel->onClose();
    QVERIFY(right->isVisible() && left->isVisible());
    QCOMPARE(right->sizes(), sizes);
  }
  void aiContentSharesTheDockResizeHandle() {
    auto *workspace = dynamic_cast<u::OverlayDockWorkspace *>(child<QWidget>("CanvasWorkspace"));
    QVERIFY(workspace);
    const auto tabs = workspace->savePanelTabs();
    const int originalWidth = workspace->panelWidth();
    openMode("AIGenerateImageAction");
    auto *shell = child<QWidget>("AIImageEditorWorkspace");
    auto *handle = workspace->panelResizeHandle();
    QVERIFY(shell && handle->isVisible());
    QVERIFY(workspace->rightPanelCard()->isAncestorOf(shell));
    const int before = shell->width();
    const QPoint start = handle->mapToGlobal(handle->rect().center());
    QTest::mousePress(handle, Qt::LeftButton, {}, handle->rect().center());
    const QPoint target = start - QPoint(40, 0);
    QMouseEvent move(QEvent::MouseMove, handle->mapFromGlobal(target), target,
                     Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(handle, &move);
    QTest::mouseRelease(handle, Qt::LeftButton);
    QCoreApplication::processEvents();
    QCOMPARE(shell->width(), before + 40);
    QVERIFY(!child<QSplitter>("RightPanelSplitter")->isVisible());
    panel->onClose();
    QVERIFY(child<QSplitter>("RightPanelSplitter")->isVisible());
    QCOMPARE(workspace->savePanelTabs(), tabs);
    workspace->setPanelWidth(originalWidth);
  }
  void backgroundGenerationSurvivesPanelAndDocumentClosure() {
    const auto originalDoc = window->activeDocumentId();
    const auto otherPath = temporary.filePath("continuing.png");
    QImage second(80, 80, QImage::Format_RGBA8888);
    second.fill(Qt::red);
    QVERIFY(second.save(otherPath));
    QVERIFY(window->openImageFromPath(otherPath));
    const auto secondDoc = window->activeDocumentId();
    QVERIFY(window->activateDocument(originalDoc));
    openMode("AIEditLayerAction");
    generate("slow");
    QVERIFY(session().execute(std::make_unique<c::SetLayerOpacityCommand>(
        *session().activeLayer(), .6F)));
    panel->onClose();
    QVERIFY(panel->isBusy());
    QVERIFY(child<QWidget>("AIImageEditorWorkspace")->isHidden());
    QTRY_VERIFY(child<QWidget>("DiffusionProgressNotification")->isVisible());
    child<QToolButton>("DiffusionNoticeDismiss")->click();
    QVERIFY(child<QWidget>("DiffusionProgressNotification")->isHidden());
    QTimer::singleShot(0, window.get(), [this] {
      auto *dialog = child<QMessageBox>("DiffusionClosePrompt");
      QVERIFY(dialog && !dialog->isWindow());
      dialog->button(QMessageBox::Cancel)->click();
    });
    QVERIFY(!window->closeDocument(originalDoc));
    QVERIFY(panel->isBusy());
    QTimer::singleShot(0, window.get(), [this] {
      auto *dialog = child<QMessageBox>("DiffusionClosePrompt");
      QVERIFY(dialog);
      for (auto *button : dialog->buttons())
        if (button->text() == "Close document") {
          button->click();
          return;
        }
      dialog->reject();
      QFAIL("Missing close action");
    });
    QVERIFY(window->closeDocument(originalDoc));
    QVERIFY(panel->isBusy());
    QCOMPARE(window->activeDocumentId(), secondDoc);
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    QCOMPARE(panel->results().size(), 1);
    QVERIFY(child<QWidget>("AIImageEditorWorkspace")->isHidden());
    QVERIFY(child<QWidget>("DiffusionProgressNotification")->isVisible());
    const auto tab = child<QTabWidget>("DiffusionTabs")->currentIndex();
    child<QToolButton>("DiffusionNoticeOpen")->click();
    QVERIFY(child<QWidget>("AIImageEditorWorkspace")->isVisible());
    QVERIFY(child<QDialog>("DiffusionPreviewDialog")->isVisible());
    QCOMPARE(child<QTabWidget>("DiffusionTabs")->currentIndex(), tab);
    auto *destinations = child<QComboBox>("DiffusionOutputDestination");
    auto *items = qobject_cast<QStandardItemModel *>(destinations->model());
    QCOMPARE(destinations->findData("replaceOriginal"), -1);
    QVERIFY(items->item(int(u::DiffusionPanel::Destination::ReplaceSelected))
                ->isEnabled());
    const auto before = native();
    panel->setDestination(u::DiffusionPanel::Destination::ReplaceSelected);
    child<QPushButton>("DiffusionApply")->click();
    QVERIFY(native() != before);
    QVERIFY(session().undo());
    QCOMPARE(native(), before);
    QVERIFY(session().redo());
  }
  void selectedReplacementValidatesCurrentLayer() {
    openMode("AIGenerateImageAction");
    generate();
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    auto *doc = session().document();
    const auto rasterId = *session().activeLayer();
    auto text = c::Layer::text("Text", c::TextLayer{});
    const auto id = text.id;
    QVERIFY(doc->insertLayer(doc->layers().size(), std::move(text)));
    session().setActiveLayer(id);
    panel->onResultSelected(panel->selectedResultIndex());
    auto *items = qobject_cast<QStandardItemModel *>(
        panel->findChild<QComboBox *>("DiffusionOutputDestination")->model());
    QVERIFY(!items->item(int(u::DiffusionPanel::Destination::ReplaceSelected))
                 ->isEnabled());
    const auto revision = doc->revision();
    panel->setDestination(u::DiffusionPanel::Destination::ReplaceSelected);
    panel->onApply();
    QCOMPARE(doc->revision(), revision);
    session().setActiveLayer(rasterId);
    panel->onResultSelected(panel->selectedResultIndex());
    QVERIFY(items->item(int(u::DiffusionPanel::Destination::ReplaceSelected))
                ->isEnabled());
  }
  void alignedEditCanBeAppliedFittedOrAtGeneratedSize() {
    auto path = qEnvironmentVariable("VULKANA_DIFFUSION_SIZE_IMAGE");
    if (path.isEmpty()) {
      path = temporary.filePath("unaligned-portrait.png");
      QImage source(782, 1395, QImage::Format_RGBA8888); source.fill(Qt::blue);
      QVERIFY(source.save(path));
    }
    QVERIFY(window->openImageFromPath(path));
    panel->setMaxInputSize(1440);
    openMode("AIEditLayerAction");
    child<QToolButton>("DiffusionResetSize")->click();
    QCOMPARE(panel->request()["width"].toInt(), 800);
    QCOMPARE(panel->request()["height"].toInt(), 1408);
    child<QToolButton>("DiffusionMatchSelectedSize")->click();
    QCOMPARE(panel->request()["width"].toInt(), 800);
    QCOMPARE(panel->request()["height"].toInt(), 1408);
    generate("identity");
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 15000);
    QCOMPARE(panel->selectedResult()->image.size(), QSize(800, 1408));
    auto canvas = session().document()->canvas(); canvas.extent = {1600, 2000};
    QVERIFY(session().execute(std::make_unique<c::ChangeCanvasSpecCommand>(canvas)));
    panel->showResultPreview();
    auto *fit = child<QCheckBox>("DiffusionFitOutput");
    QVERIFY(fit && fit->isChecked() && fit->isEnabled());
    QCOMPARE(panel->resultPlacement().mapRect(QRectF(0, 0, 800, 1408)).size(), QSizeF(782, 1395));
    child<QPushButton>("DiffusionApply")->click();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCOMPARE(native().size(), QSize(800, 1408));
    auto transform = session().document()->layer(*session().activeLayer())->localToDocument;
    QVERIFY(std::abs(transform.m00 * 800 - 782) < 1e-6);
    QVERIFY(std::abs(transform.m11 * 1408 - 1395) < 1e-6);
    QVERIFY(session().undo());
    panel->showResultPreview();
    fit = child<QCheckBox>("DiffusionFitOutput");
    fit->click();
    QCOMPARE(panel->resultPlacement(), QTransform{});
    child<QPushButton>("DiffusionCompareLayer")->click();
    child<QListWidget>("DiffusionComparisonLayers")->setCurrentRow(0);
    auto *preview = dynamic_cast<u::DiffusionPreview *>(child<QWidget>("DiffusionPreview"));
    QVERIFY(preview);
    preview->actualSize();
    QCOMPARE(preview->imageRect().size(), QSizeF(800, 1408));
    QCOMPARE(preview->referenceRect().size(), QSizeF(782, 1395));
    fit->click();
    preview->actualSize();
    QCOMPARE(preview->imageRect().size(), QSizeF(782, 1395));
    fit->click();
    if (const auto capture = qEnvironmentVariable("VULKANA_DIFFUSION_INTEGRATION_CAPTURE"); !capture.isEmpty())
      QVERIFY(child<QDialog>("DiffusionPreviewDialog")->grab().save(capture + "-true-size.png"));
    child<QPushButton>("DiffusionApply")->click();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    const auto id = *session().activeLayer();
    QCOMPARE(native().size(), QSize(800, 1408));
    QCOMPARE(session().document()->layer(id)->localToDocument, c::AffineTransform{});
    QVERIFY(session().undo());
    QVERIFY(!session().document()->layer(id));
    QVERIFY(session().redo());
    QCOMPARE(session().document()->layer(id)->localToDocument, c::AffineTransform{});
    panel->showResultPreview();
    QVERIFY(!child<QCheckBox>("DiffusionFitOutput")->isChecked());
  }
  void portraitInputAndOutputKeepDimensions() {
    const auto path = temporary.filePath("portrait.png");
    QImage image(1024, 1536, QImage::Format_RGBA8888);
    image.fill(Qt::blue);
    for (int y = 300; y < 700; ++y)
      for (int x = 210; x < 260; ++x)
        image.setPixelColor(x, y, Qt::red);
    QVERIFY(image.save(path));
    QVERIFY(window->openImageFromPath(path));
    const auto original = native();
    openMode("AIEditLayerAction");
    generate("identity");
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    const auto *result = panel->selectedResult();
    QVERIFY(result);
    QCOMPARE(result->image.size(), QSize(1024, 1536));
    QCOMPARE(result->before.size(), QSize(682, 1024));
    QCOMPARE(panel->inputs().front().toDocument.map(QPointF(682, 1024)), QPointF(1024, 1536));
    const auto expected = result->before.scaled(original.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    QCOMPARE(result->image, expected);
    QCOMPARE(result->metadata["request"].toObject()["width"].toInt(), 1024);
    QCOMPARE(result->metadata["request"].toObject()["height"].toInt(), 1536);
    panel->setDestination(u::DiffusionPanel::Destination::NewDocument);
    panel->onApply();
    QCOMPARE(native(), expected);
  }
  void removableEditInputResetAndComparison() {
    openMode("AIEditLayerAction");
    auto *list = panel->findChild<QListWidget *>("DiffusionInputsList");
    auto *gen = panel->findChild<QPushButton *>("DiffusionGenerate");
    QVERIFY(!panel->inputs().isEmpty());
    QCoreApplication::processEvents();
    const auto rect = list->visualItemRect(list->item(0));
    QTest::mouseClick(list->viewport(), Qt::LeftButton, {},
                      QPoint(rect.right() - 12, rect.center().y()));
    QTRY_VERIFY(panel->inputs().isEmpty());
    QVERIFY(!gen->isEnabled());
    generate();
    QVERIFY(!panel->isBusy());
    QVERIFY(panel->inputs().isEmpty());
    QImage image(96, 64, QImage::Format_RGBA8888);
    image.fill(Qt::yellow);
    QVERIFY(panel->addInput({image,
                             "New edit image",
                             "File",
                             false,
                             {},
                             QTransform::fromTranslate(20, 10)}));
    QVERIFY(gen->isEnabled());
    panel->findChild<QToolButton *>("DiffusionResetSize")->click();
    QCOMPARE(panel->request()["width"].toInt(), 96);
    QCOMPARE(panel->request()["height"].toInt(), 64);
    generate("identity");
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    QCOMPARE(panel->selectedResult()->image.pixelColor(0, 0),
             QColor(Qt::yellow));
    QCOMPARE(panel->selectedResult()->toDocument.map(QPointF{}),
             QPointF(20, 10));
    panel->showResultPreview();
    auto *compare = child<QPushButton>("DiffusionCompareLayer");
    QVERIFY(compare && compare->isEnabled());
    compare->click();
    auto *choices = child<QListWidget>("DiffusionComparisonLayers");
    QVERIFY(choices && choices->count() > 0);
    choices->setCurrentRow(0);
    QVERIFY(panel->comparisonActive());
    auto *view =
        dynamic_cast<u::DiffusionPreview *>(child<QWidget>("DiffusionPreview"));
    QVERIFY(view);
    view->actualSize();
    QCOMPARE(view->referenceRect().topLeft() - view->imageRect().topLeft(),
             QPointF(-20, -10));
    auto *align = child<QComboBox>("DiffusionCompareAlignment");
    align->setCurrentIndex(1);
    view->actualSize();
    QCOMPARE(view->referenceRect().center(), view->imageRect().center());
    align->setCurrentIndex(2);
    view->actualSize();
    QCOMPARE(view->referenceRect().width() / view->referenceRect().height(),
             1.0);
    auto *dialog = child<QDialog>("DiffusionPreviewDialog");
    dialog->close();
    panel->setDestination(u::DiffusionPanel::Destination::NewLayer);
    panel->onApply();
    const auto transform =
        session().document()->layer(*session().activeLayer())->localToDocument;
    QCOMPARE(transform.m02, 20.0);
    QCOMPARE(transform.m12, 10.0);
    QCOMPARE(native().size(), QSize(96, 64));
  }
  void progressIsHiddenWithPanelAndBackendIsVerified() {
    openMode("AIGenerateImageAction");
    generate("slow");
    QTRY_VERIFY(child<QWidget>("DiffusionProgressNotification"));
    QVERIFY(child<QWidget>("DiffusionProgressNotification")->isHidden());
    QTRY_VERIFY(child<QPushButton>("DiffusionUnloadModel"));
    QVERIFY(!child<QPushButton>("DiffusionUnloadModel")->isEnabled());
    panel->onClose();
    QVERIFY(child<QWidget>("DiffusionProgressNotification")->isVisible());
    openMode("AIImageEditorAction");
    QVERIFY(child<QWidget>("DiffusionProgressNotification")->isHidden());
    panel->onCancel();
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    QTRY_VERIFY(child<QPushButton>("DiffusionUnloadModel")->isEnabled());
    panel->findChild<QComboBox *>("DiffusionBackend")->setCurrentIndex(1);
    generate();
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    QVERIFY(panel->results()
                .isEmpty()); // CPU test worker cannot masquerade as Vulkan.
    panel->findChild<QComboBox *>("DiffusionBackend")->setCurrentIndex(0);
    generate();
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    QCOMPARE(panel->results().size(), 1);
    QVERIFY(child<QWidget>("DiffusionProgressNotification")->isHidden());
    panel->onClose();
    QVERIFY(child<QWidget>("DiffusionProgressNotification")->isHidden()); // Already observed.
    openMode("AIImageEditorAction");
    QTRY_VERIFY(child<QPushButton>("DiffusionUnloadModel")->isEnabled());
    child<QPushButton>("DiffusionUnloadModel")->click();
    generate();
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    QCOMPARE(panel->results().size(), 2);
  }
  void notificationOpensLatestPreviewWithoutChangingTabs() {
    openMode("AIGenerateImageAction");
    generate();
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    QCOMPARE(panel->results().size(), 1);
    auto *tabs = child<QTabWidget>("DiffusionTabs");
    tabs->setCurrentIndex(0);
    generate("slow");
    panel->onClose();
    QVERIFY(child<QWidget>("DiffusionProgressNotification")->isVisible());
    child<QToolButton>("DiffusionNoticeOpen")->click();
    auto *dialog = child<QDialog>("DiffusionPreviewDialog");
    QVERIFY(dialog && dialog->isVisible() && !dialog->isWindow());
    QCOMPARE(tabs->currentIndex(), 0);
    QVERIFY(!panel->selectedResult()); // Inspect the running job, not an older result.
    QVERIFY(child<QWidget>("DiffusionProgressNotification")->isHidden());
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    QCOMPARE(panel->results().size(), 2);
    QVERIFY(child<QWidget>("DiffusionProgressNotification")->isHidden());
    dialog->close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    panel->onClose();
    QVERIFY(child<QWidget>("DiffusionProgressNotification")->isHidden());
    generate("slow");
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    QVERIFY(child<QWidget>("DiffusionProgressNotification")->isVisible());
    child<QToolButton>("DiffusionNoticeOpen")->click();
    dialog = child<QDialog>("DiffusionPreviewDialog");
    QVERIFY(dialog && dialog->isVisible());
    QCOMPARE(tabs->currentIndex(), 0);
    QCOMPARE(panel->selectedResultIndex(), 2);
    QCOMPARE(dynamic_cast<u::DiffusionPreview *>(child<QWidget>("DiffusionPreview"))->image(),
             panel->results().back().image);
    QVERIFY(child<QWidget>("DiffusionProgressNotification")->isHidden());
    dialog->close();
    panel->onClose();
    QVERIFY(child<QWidget>("DiffusionProgressNotification")->isHidden());
  }
  void comparisonChooserIsIndependentOfDockLayout_data() {
    QTest::addColumn<int>("placement");
    QTest::newRow("left") << 0;
    QTest::newRow("right") << 1;
    QTest::newRow("floating") << 2;
  }
  void comparisonChooserIsIndependentOfDockLayout() {
    QFETCH(int, placement);
    auto *workspace = dynamic_cast<u::OverlayDockWorkspace *>(child<QWidget>("CanvasWorkspace"));
    auto *layerFrame = dynamic_cast<u::WorkspacePanel *>(child<QWidget>("LayersPanel"));
    QVERIFY(workspace && layerFrame);
    auto *sourceList = child<QListView>("LayerList");
    auto *layerModel = dynamic_cast<u::LayerListModel *>(sourceList->model());
    QVERIFY(layerModel);
    const auto sourceId = session().activeLayer().value();
    (void)layerModel->data(layerModel->index(layerModel->rowForLayer(sourceId)), Qt::DecorationRole);
    const auto sourceThumbnail = layerModel->cachedContentThumbnail(sourceId);
    QVERIFY(!sourceThumbnail.isNull());
    if (placement == 1) workspace->dockPanel(layerFrame, u::OverlayDockWorkspace::PanelDockSide::Right, 0);
    if (placement == 2) workspace->floatPanel(layerFrame, QRect(20, 40, 300, 500));
    openMode("AIGenerateImageAction");
    generate();
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    panel->setDestination(u::DiffusionPanel::Destination::NewLayer);
    panel->onApply();
    // The chooser includes editable text/shapes and hidden raster layers,
    // while adjustment layers and raster layers without pixels are excluded.
    std::vector<c::LayerId> uncachedLayers;
    for (auto payload : {c::LayerPayload(c::TextLayer{"Comparison text"}),
                         c::LayerPayload(c::ShapeLayer{}),
                         c::LayerPayload(c::AdjustmentLayer{}),
                         c::LayerPayload(c::RasterLayer{})}) {
      c::Layer layer;
      layer.id = c::makeLayerId();
      uncachedLayers.push_back(layer.id);
      layer.name = "Extra layer";
      layer.payload = std::move(payload);
      QVERIFY(session().document()->insertLayer(0, std::move(layer)));
    }
    openMode("AIImageEditorAction");
    const auto revision = session().document()->revision();
    const auto active = session().activeLayer();
    auto *editorList = child<QListView>("LayerList");
    QVERIFY(editorList);
    panel->showResultPreview();
    auto *dialog = child<QDialog>("DiffusionPreviewDialog");
    auto *shield = dialog->parentWidget();
    QVERIFY(shield->mask().isEmpty());
    QVERIFY((dialog->geometry().center() - shield->rect().center()).manhattanLength() <= 2);
    QVERIFY(child<QWidget>("AIImageEditorWorkspace")->isVisible());
    // Even a direct event to the underlying list is rejected by the shield.
    QTest::mouseClick(editorList->viewport(), Qt::LeftButton, {}, QPoint(30, 70));
    QCOMPARE(session().activeLayer(), active);
    QCOMPARE(session().document()->revision(), revision);
    child<QPushButton>("DiffusionCompareLayer")->click();
    auto *choices = child<QListWidget>("DiffusionComparisonLayers");
    auto *chooser = child<QWidget>("DiffusionComparisonPanel");
    auto *main = child<QWidget>("DiffusionPreviewContent");
    auto *search = child<QLineEdit>("DiffusionComparisonSearch");
    QVERIFY(choices && chooser && main && search);
    QCoreApplication::processEvents();
    QCOMPARE(choices->count(), 4);
    for (const auto id : uncachedLayers) QVERIFY(layerModel->cachedContentThumbnail(id).isNull());
    QVERIFY(chooser->isVisible());
    QVERIFY(chooser->geometry().right() < main->geometry().left());
    QVERIFY((dialog->geometry().center() - shield->rect().center()).manhattanLength() <= 2);
    // Use the real host keyboard/IME route for search, including caret keys.
    const auto searchPoint = workspace->panelOverlay()->mapFromGlobal(search->mapToGlobal(search->rect().center()));
    QTest::mouseClick(workspace->panelOverlay()->windowHandle(), Qt::LeftButton, {}, searchPoint);
    QTRY_VERIFY(search->hasFocus());
    QInputMethodEvent input;
    input.setCommitString("SOURCE");
    QCoreApplication::sendEvent(window->windowHandle(), &input);
    QCOMPARE(search->text(), QString("SOURCE"));
    const auto resultIndex = panel->selectedResultIndex();
    QKeyEvent left(QEvent::KeyPress, Qt::Key_Left, {});
    QCoreApplication::sendEvent(window->windowHandle(), &left);
    QCOMPARE(search->cursorPosition(), 5);
    QCOMPARE(panel->selectedResultIndex(), resultIndex);
    QListWidgetItem *match = nullptr;
    for (int row = 0; row < choices->count(); ++row) if (!choices->item(row)->isHidden()) {
      QVERIFY(!match);
      match = choices->item(row);
    }
    QVERIFY(match);
    QCOMPARE(match->icon().cacheKey(), sourceThumbnail.cacheKey());
    QCOMPARE(layerModel->cachedContentThumbnail(sourceId).cacheKey(), sourceThumbnail.cacheKey());
    const auto point = workspace->panelOverlay()->mapFromGlobal(
        choices->viewport()->mapToGlobal(choices->visualItemRect(match).center()));
    QTest::mouseClick(workspace->panelOverlay()->windowHandle(), Qt::LeftButton, {}, point);
    QVERIFY(panel->comparisonActive());
    QCOMPARE(session().activeLayer(), active);
    QCOMPARE(session().document()->revision(), revision);
    auto *preview = dynamic_cast<u::DiffusionPreview *>(child<QWidget>("DiffusionPreview"));
    QVERIFY(preview);
    preview->setComparisonSplit(1);
    QCOMPARE(preview->grab().toImage().pixelColor(preview->imageRect().center().toPoint()), QColor(70, 90, 210));
    const auto captures = qEnvironmentVariable("VULKANA_DIFFUSION_INTEGRATION_CAPTURE");
    if (!captures.isEmpty()) {
      search->clear();
      preview->setComparisonSplit(.5);
      QVERIFY(workspace->panelOverlay()->grab().save(captures + "-comparison-workspace-" + QTest::currentDataTag() + ".png"));
    }
    child<QPushButton>("DiffusionCompareLayer")->click();
    QVERIFY(!panel->comparisonActive());
    QVERIFY(!chooser->isVisible());
    QVERIFY((dialog->geometry().center() - shield->rect().center()).manhattanLength() <= 2);
    dialog->close();
    QCOMPARE(session().activeLayer(), active);
  }
  void loadedModelsTrackReplacementFailureAndUnload() {
    openMode("AIGenerateImageAction");
    QVERIFY(child<QLabel>("DiffusionLoadedModelsEmpty"));
    generate();
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    QTRY_VERIFY(child<QPushButton>("DiffusionUnloadModel"));
    QTRY_VERIFY(child<QPushButton>("DiffusionUnloadModel")->isEnabled());
    const auto oldId = child<QPushButton>("DiffusionUnloadModel")
                           ->property("modelId").toString();
    const auto oldText = child<QLabel>("DiffusionLoadedFile_diffusion")->text();
    auto config = panel->configuration();
    const auto originalConfig = config;
    const auto otherModel = temporary.filePath("another-model.gguf");
    if (!QFile::exists(otherModel))
      QVERIFY(QFile::copy(modelPath, otherModel));
    auto files = config["files"].toObject();
    files["diffusion"] = otherModel;
    config["files"] = files;
    auto settings = config["settings"].toObject();
    settings["device"] = "invalid-test-device";
    config["settings"] = settings;
    panel->setConfiguration(config);
    QCOMPARE(child<QLabel>("DiffusionLoadedFile_diffusion")->text(), oldText);
    generate(); // definite load validation failure preserves the old context
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    QCOMPARE(panel->results().size(), 1);
    QTRY_VERIFY(child<QPushButton>("DiffusionUnloadModel")->isEnabled());
    QCOMPARE(child<QPushButton>("DiffusionUnloadModel")->property("modelId").toString(), oldId);
    QCOMPARE(child<QLabel>("DiffusionLoadedFile_diffusion")->text(), oldText);
    config["settings"] = originalConfig["settings"];
    panel->setConfiguration(config);
    generate();
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    QTRY_VERIFY(child<QPushButton>("DiffusionUnloadModel")->isEnabled());
    const auto newId = child<QPushButton>("DiffusionUnloadModel")
                           ->property("modelId").toString();
    QVERIFY(newId != oldId);
    QCOMPARE(child<QLabel>("DiffusionLoadedFile_diffusion")->text(),
             QString("Diffusion · another-model.gguf"));
    panel->onUnloadModel(oldId); // stale actions must not unload a replacement
    QCOMPARE(child<QPushButton>("DiffusionUnloadModel")->property("modelId").toString(), newId);
    generate("slow");
    QVERIFY(!child<QPushButton>("DiffusionUnloadModel")->isEnabled());
    panel->onUnloadModel(newId); // cannot interrupt a running generation
    QTest::qWait(60);
    QVERIFY(panel->isBusy());
    panel->onClose();
    panel->onCancel();
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    QTRY_VERIFY(child<QPushButton>("DiffusionUnloadModel")->isEnabled());
    const auto resultCount = panel->results().size();
    child<QPushButton>("DiffusionUnloadModel")->click();
    QVERIFY(!child<QPushButton>("DiffusionUnloadModel")->isEnabled());
    QTRY_VERIFY_WITH_TIMEOUT(child<QLabel>("DiffusionLoadedModelsEmpty"), 10000);
    QVERIFY(!child<QPushButton>("DiffusionUnloadModel"));
    QCOMPARE(panel->results().size(), resultCount);
    openMode("AIImageEditorAction");
    generate();
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    QTRY_VERIFY(child<QPushButton>("DiffusionUnloadModel"));
    QVERIFY(child<QPushButton>("DiffusionUnloadModel")->property("modelId").toString() != newId);
    QCOMPARE(panel->results().size(), resultCount + 1);
  }
  void workerCrashRecoveryAndUnloadQueue() {
    openMode("AIGenerateImageAction");
    generate("crash");
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    QCOMPARE(panel->results().size(), 0);
    QTest::qWait(50);
    QVERIFY(child<QLabel>("DiffusionLoadedModelsEmpty"));
    generate();
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    QCOMPARE(panel->results().size(), 1);
    QTRY_VERIFY(child<QPushButton>("DiffusionUnloadModel"));
    QTRY_VERIFY(child<QPushButton>("DiffusionUnloadModel")->isEnabled());
    child<QPushButton>("DiffusionUnloadModel")->click();
    generate();
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    QCOMPARE(panel->results().size(), 2);
  }
  void realWorkerRoundTrip() {
    const auto worker = qEnvironmentVariable("VULKANA_DIFFUSION_TEST_WORKER"),
               loadPath = qEnvironmentVariable("VULKANA_DIFFUSION_TEST_LOAD");
    if (worker.isEmpty() || loadPath.isEmpty())
      QSKIP("Set optional local worker/model configuration paths to run real "
            "inference.");
    QFile file(loadPath);
    QVERIFY(file.open(QIODevice::ReadOnly));
    panel->setWorkerExecutable(worker);
    panel->setConfiguration(QJsonDocument::fromJson(file.readAll()).object());
    panel->setResolution({512, 512});
    openMode("AIGenerateImageAction");
    const auto original = session().document()->revision();
    generate("A realistic orange ceramic teapot on a blue cloth");
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 180000);
    QCOMPARE(panel->results().size(), 1);
    QCOMPARE(session().document()->revision(), original);
    QCOMPARE(panel->selectedResult()->image.size(), QSize(512, 512));
    const auto context =
        panel->selectedResult()->metadata.value("contextGeneration");
    generate("A realistic orange ceramic teapot on a blue cloth");
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 180000);
    QCOMPARE(panel->results().size(), 2);
    QCOMPARE(panel->selectedResult()->metadata.value("contextGeneration"),
             context);
    panel->setDestination(u::DiffusionPanel::Destination::NewDocument);
    panel->onApply();
    QCOMPARE(session().document()->canvas().extent.width, 512u);
    QCOMPARE(session().document()->canvas().extent.height, 512u);
  }
};
QTEST_MAIN(DiffusionIntegrationTests)
#include "DiffusionIntegrationTests.moc"
