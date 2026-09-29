#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/ExportDialog.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/ProjectFile.hpp"
#include "imageeditor/ui/Theme.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPdfDocument>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QVulkanInstance>
#include <iostream>
#include <vulkan/vulkan.h>
namespace u = imageeditor::ui;
namespace c = imageeditor::core;
int failures = 0;
#define CHECK(...)                                                             \
  do {                                                                         \
    if (!(__VA_ARGS__)) {                                                      \
      ++failures;                                                              \
      std::cerr << "FAIL " << __LINE__ << ": " #__VA_ARGS__ "\n";              \
    }                                                                          \
  } while (false)
u::ExportDialog *dialog(u::MainWindow &window) {
  return dynamic_cast<u::ExportDialog *>(
      window.findChild<QDialog *>("ExportDialog"));
}
void run(QVulkanInstance *instance, const QDir &output) {
  u::MainWindow window(instance, false);
  window.setUnsavedPromptEnabled(false);
  window.resize(1250, 850);
  window.show();
  QImage starter(32, 32, QImage::Format_RGBA8888);
  starter.fill(Qt::white);
  CHECK(starter.save(output.filePath("other.png")));
  CHECK(window.openImageFromPath(output.filePath("other.png")));
  const auto other = window.activeDocumentId();
  c::Document doc({{240, 160}, 144});
  for (int i = 0; i < 15; ++i) {
    c::Layer layer;
    layer.id = c::makeLayerId();
    layer.name = "Entry " + std::to_string(i + 1);
    layer.payload = c::RasterLayer{std::make_shared<c::ContiguousRasterSurface>(
        c::Extent2u{80, 60}, c::Rgba8{std::uint8_t(i * 15), 80, 180, 128})};
    layer.localToDocument.m02 = i * 5;
    layer.localToDocument.m12 = i * 3;
    CHECK(doc.insertLayer(doc.layers().size(), layer));
  }
  auto project = output.filePath("export-source.vulkana");
  CHECK(u::saveProject(project, doc));
  CHECK(window.openImageFromPath(project));
  const auto owner = window.activeDocumentId();
  const auto *source = window.editorSession().document();
  auto before = source->revision();
  auto content = source->contentState();
  auto selection = window.editorSession().selectedLayers();
  const auto depth = window.editorSession().history().undoDepth();
  if (instance) {
    QElapsedTimer wait;
    wait.start();
    bool rendered = false;
    while (wait.elapsed() < 10000 && !rendered) {
      QTest::qWait(20);
      for (auto *native : QGuiApplication::allWindows())
        if (auto *canvas =
                dynamic_cast<imageeditor::render::CanvasWindow *>(native))
          rendered |= canvas->rendererStats().framesSubmitted > 0;
    }
    CHECK(rendered);
  }
  int stage = 0, ticks = 0;
  QTimer timer;
  timer.setInterval(20);
  QObject::connect(&timer, &QTimer::timeout, &window, [&] {
    auto *card = dialog(window);
    if (!card)
      return;
    if (++ticks > 1000) {
      CHECK(false);
      std::cerr
          << "status "
          << card->findChild<QLabel *>("ExportStatus")->text().toStdString()
          << '\n';
      card->reject();
      return;
    }
    auto *write = card->findChild<QPushButton *>("ExportWrite");
    if (stage == 0) {
      ++stage;
      card->findChild<QComboBox *>("ExportFormat")->setCurrentIndex(3);
      card->setDestination(output.filePath("dialog.pdf"));
      card->findChild<QComboBox *>("PdfExportMode")->setCurrentIndex(1);
      return;
    }
    if (stage == 1 && write->isEnabled()) {
      ++stage;
      CHECK(card->settings().pdf.ppi == 144);
      CHECK(card->settings().pdf.instanceId == owner);
      CHECK(!window.activateDocument(other));
      CHECK(!window.closeDocument(owner));
      CHECK(window.activeDocumentId() == owner);
      auto *list = card->findChild<QListWidget *>("PdfExportItemList");
      CHECK(list->count() == 15);
      CHECK(list->item(0)->text().startsWith("1 · Entry 15"));
      CHECK(!card->findChild<QDoubleSpinBox *>("ExportWidth")->isVisible());
      auto *range = card->findChild<QLineEdit *>("PdfExportRange");
      range->setFocus();
      range->selectAll();
      QTest::keyClicks(range, "1-3, 2");
      CHECK(card->settings().pdf.chosen.size() == 3);
      return;
    }
    if (stage == 2 && write->isEnabled()) {
      ++stage;
      CHECK(card->findChild<QComboBox *>("PdfExportPreviewPage")->count() == 3);
      auto *range = card->findChild<QLineEdit *>("PdfExportRange");
      range->selectAll();
      QTest::keyClicks(range, "0, 99");
      CHECK(!write->isEnabled());
      CHECK(!card->pdfInputError().isEmpty());
      range->selectAll();
      QTest::keyClicks(range, "1-3");
      card->findChild<QCheckBox *>("PdfExportReverse")->setChecked(true);
      return;
    }
    if (stage == 3 && write->isEnabled()) {
      ++stage;
      CHECK(card->findChild<QComboBox *>("PdfExportPreviewPage")
                ->itemText(0)
                .contains("Entry 13"));
      card->findChild<QComboBox *>("PdfExportPreviewPage")->setCurrentIndex(2);
      return;
    }
    if (stage == 4 && write->isEnabled() &&
        !card->findChild<QListWidget *>("PdfExportItemList")->item(0)->icon().isNull() &&
        card->findChild<QLabel *>("ExportStatus")
            ->text()
            .startsWith("Page 3")) {
      ++stage;
      CHECK(card->grab().save(output.filePath("pdf-export-dialog.png")));
      write->click();
      return;
    }
    if (stage == 5 && card->findChild<QWidget *>("ExportSuccessOverlay")) {
      ++stage;
      CHECK(!card->findChild<QWidget *>("ExportSuccessOverlay")->isWindow());
      card->findChild<QPushButton *>("ExportSuccessClose")->click();
      card->reject();
      return;
    }
  });
  timer.start();
  window.exportImage();
  timer.stop();
  CHECK(stage == 6);
  if(stage!=6){window.close();return;} // Do not dereference unset success state after a failed interaction.
  CHECK(window.activeDocumentId() == owner);
  CHECK(source == window.editorSession().document());
  CHECK(source->revision() == before && source->contentState() == content);
  CHECK(window.editorSession().history().undoDepth() == depth);
  CHECK(window.editorSession().selectedLayers() == selection);
  CHECK(window.projectPath() == QFileInfo(project).absoluteFilePath());
  CHECK(window.documentContext(owner)->exportSettings &&
        window.documentContext(owner)->exportSettings->format ==
            u::ExportFormat::Pdf);
  CHECK(!window.documentContext(other)->exportSettings);
  QPdfDocument pdf;
  CHECK(pdf.load(output.filePath("dialog.pdf")) == QPdfDocument::Error::None);
  CHECK(pdf.pageCount() == 3);
  CHECK(pdf.pagePointSize(0) == QSizeF(120, 80));
  // Abort a multipage export, keep the panel/settings usable, then retry from
  // that same panel. Cancellation must not be mistaken for a successful save.
  const auto priorSettings = *window.documentContext(owner)->exportSettings;
  QTimer retry;
  retry.setInterval(20);
  int retryStage = 0, retryTicks = 0;
  QObject::connect(&retry, &QTimer::timeout, &window, [&] {
    auto* card = dialog(window);
    if (!card) return;
    if (++retryTicks > 1000) { CHECK(false); card->reject(); return; }
    auto* cancel = card->findChild<QPushButton*>("ExportCancel");
    auto* write = card->findChild<QPushButton*>("ExportWrite");
    if (retryStage == 0) {
      ++retryStage;
      card->setDestination(output.filePath("cancel-write.pdf"));
      card->findChild<QDoubleSpinBox*>("PdfExportPpi")->setValue(600);
    } else if (retryStage == 1 && write->isEnabled()) {
      ++retryStage;
      write->click();
      CHECK(cancel->isVisible());
      cancel->click();
    } else if (retryStage == 2 && cancel->isHidden()) {
      ++retryStage;
      CHECK(card->isVisible());
      CHECK(!card->findChild<QWidget*>("ExportSuccessOverlay"));
      CHECK(!QFileInfo::exists(output.filePath("cancel-write.pdf")));
      CHECK(*window.documentContext(owner)->exportSettings == priorSettings);
      CHECK(card->findChild<QLineEdit*>("ExportDestination")->isEnabled());
      card->setDestination(output.filePath("retry.pdf"));
      card->findChild<QDoubleSpinBox*>("PdfExportPpi")->setValue(144);
    } else if (retryStage == 3 && write->isEnabled()) {
      ++retryStage;
      write->click();
    } else if (retryStage == 4 && card->findChild<QWidget*>("ExportSuccessOverlay")) {
      ++retryStage;
      CHECK(cancel->isHidden());
      card->findChild<QPushButton*>("ExportSuccessClose")->click();
      card->findChild<QPushButton*>("ExportHeaderClose")->click();
    }
  });
  retry.start();
  window.exportImage();
  retry.stop();
  CHECK(retryStage == 5);
  CHECK(QFileInfo::exists(output.filePath("retry.pdf")));
  CHECK(source->revision() == before && source->contentState() == content);
  CHECK(window.editorSession().history().undoDepth() == depth);
  CHECK(window.editorSession().selectedLayers() == selection);
  // A cancelled PDF draft must not replace successful per-document settings.
  const auto successful = *window.documentContext(owner)->exportSettings;
  QTimer stop;
  stop.setInterval(20);
  int cancelled = 0;
  QObject::connect(&stop, &QTimer::timeout, &window, [&] {
    if (auto *card = dialog(window)) {
      if (cancelled == 0) {
        ++cancelled;
        card->findChild<QDoubleSpinBox *>("PdfExportPpi")->setValue(600);
        card->setDestination(output.filePath("cancelled.pdf"));
        card->reject();
      }
    }
  });
  stop.start();
  window.exportImage();
  stop.stop();
  CHECK(cancelled == 1);
  CHECK(!QFileInfo::exists(output.filePath("cancelled.pdf")));
  CHECK(*window.documentContext(owner)->exportSettings == successful);
  CHECK(source->revision() == before && source->contentState() == content);
  window.close();
  QTest::qWait(100);
}
int main(int argc, char **argv) {
  QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName("ImageEditorTests");
  QCoreApplication::setApplicationName("PdfExport");
  QStandardPaths::setTestModeEnabled(true);
  QTemporaryDir settings, files;
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                     settings.path());
  u::applyEditorTheme(app);
  QDir output(qEnvironmentVariable("VULKANA_PDF_EXPORT_OUTPUT", files.path()));
  CHECK(output.mkpath("."));
  std::atomic_int warnings{0}, errors{0};
  if (app.arguments().contains("--validation")) {
    QVulkanInstance instance;
    instance.setApiVersion(QVersionNumber(1, 2));
    instance.setLayers({"VK_LAYER_KHRONOS_validation"});
    instance.installDebugOutputFilter([&](auto severity, auto type,
                                          const void *message) {
      if (!type.testFlag(QVulkanInstance::ValidationMessage))
        return false;
      if (severity.testFlag(QVulkanInstance::ErrorSeverity))
        ++errors;
      else if (severity.testFlag(QVulkanInstance::WarningSeverity))
        ++warnings;
      else
        return false;
      std::cerr << static_cast<const VkDebugUtilsMessengerCallbackDataEXT *>(
                       message)
                       ->pMessage
                << '\n';
      return true;
    });
    CHECK(instance.create());
    if (instance.isValid())
      run(&instance, output);
    instance.destroy();
    CHECK(warnings == 0 && errors == 0);
    std::cout << "Vulkan validation enabled: " << warnings << " warnings, "
              << errors << " errors\n";
  } else
    run(nullptr, output);
  std::cout << "Platform " << QGuiApplication::platformName().toStdString()
            << "; PDF export interaction " << (failures ? "FAILED" : "passed")
            << '\n';
  return failures ? 1 : 0;
}
