#include "imageeditor/core/LayerCommands.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/DiffusionPanel.hpp"
#include "imageeditor/ui/DiffusionPreview.hpp"
#include "imageeditor/ui/DiffusionTransaction.hpp"
#include "imageeditor/ui/FlattenedDocument.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/OverlayDockWorkspace.hpp"
#include "imageeditor/ui/Theme.hpp"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
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
#include <QScrollArea>
#include <QSettings>
#include <QSlider>
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
    window->setUnsavedPromptEnabled(false);
    window->resize(1280, 900);
    window->show();
    QCoreApplication::processEvents();
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
  void inputLimitAcceptsNativeKeyboard() {
    openMode("AIEditLayerAction");
    auto *limit = panel->findChild<QSpinBox *>("DiffusionMaxInputSize");
    QVERIFY(limit);
    panel->findChild<QScrollArea *>("DiffusionGenerationPage")->ensureWidgetVisible(limit);
    QCoreApplication::processEvents();
    QTest::mouseClick(limit, Qt::LeftButton, {}, QPoint(50, limit->height() / 2));
    QTRY_VERIFY(limit->hasFocus());
    limit->selectAll();
    for (const auto ch : QString("2048")) {
      QKeyEvent key(QEvent::KeyPress, ch.unicode(), Qt::NoModifier, QString(ch));
      QCoreApplication::sendEvent(window->windowHandle(), &key);
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
  void generationHistoryAndDestinations() {
    openMode("AIGenerateImageAction");
    const auto before = session().document()->revision();
    const auto count = session().document()->layers().size();
    generate();
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    QCOMPARE(panel->results().size(), 1);
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
  void cancellationFailureCloseAndRetarget() {
    openMode("AIEditLayerAction");
    generate();
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    QCOMPARE(panel->results().size(), 1);
    generate("slow");
    QVERIFY(panel->isBusy());
    panel->onCancel();
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    QCOMPARE(panel->results().size(), 1);
    generate("fail");
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    QCOMPARE(panel->results().size(), 1);
    generate();
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
    child<QToolButton>("DiffusionNoticeOpen")->click();
    QVERIFY(child<QWidget>("AIImageEditorWorkspace")->isVisible());
    auto *destinations =
        panel->findChild<QComboBox *>("DiffusionOutputDestination");
    auto *items = qobject_cast<QStandardItemModel *>(destinations->model());
    QCOMPARE(destinations->findData("replaceOriginal"), -1);
    QVERIFY(items->item(int(u::DiffusionPanel::Destination::ReplaceSelected))
                ->isEnabled());
    const auto before = native();
    panel->setDestination(u::DiffusionPanel::Destination::ReplaceSelected);
    panel->onApply();
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
    auto *compare = child<QPushButton>("DiffusionCompareSelected");
    QVERIFY(compare && compare->isEnabled());
    compare->click();
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
    QVERIFY(child<QWidget>("DiffusionProgressNotification")
                ->isVisible()); // completion remains visible
    QTRY_VERIFY(child<QPushButton>("DiffusionUnloadModel")->isEnabled());
    child<QPushButton>("DiffusionUnloadModel")->click();
    generate();
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    QCOMPARE(panel->results().size(), 2);
  }
  void comparisonFollowsLayerSelection() {
    openMode("AIGenerateImageAction");
    generate();
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isBusy(), 10000);
    panel->setDestination(u::DiffusionPanel::Destination::NewLayer);
    panel->onApply();
    const auto revision = session().document()->revision();
    panel->showResultPreview();
    child<QPushButton>("DiffusionCompareSelected")->click();
    auto *preview =
        dynamic_cast<u::DiffusionPreview *>(child<QWidget>("DiffusionPreview"));
    QVERIFY(preview);
    preview->setComparisonSplit(1);
    auto *list = child<QListView>("LayerList");
    QVERIFY(list);
    const auto original = list->model()->index(1, 0);
    QVERIFY(original.isValid());
    list->scrollTo(original);
    QCoreApplication::processEvents();
    QTest::mouseClick(list->viewport(), Qt::LeftButton, {},
                      list->visualRect(original).center());
    QCoreApplication::processEvents();
    QCOMPARE(preview->grab().toImage().pixelColor(
                 preview->imageRect().center().toPoint()),
             QColor(70, 90, 210));
    QCOMPARE(session().document()->revision(), revision);
    const auto captures =
        qEnvironmentVariable("VULKANA_DIFFUSION_INTEGRATION_CAPTURE");
    if (!captures.isEmpty()) {
      preview->setComparisonSplit(.5);
      QVERIFY(child<QDialog>("DiffusionPreviewDialog")
                  ->grab()
                  .save(captures + "-comparison.png"));
    }
    child<QDialog>("DiffusionPreviewDialog")->close();
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
