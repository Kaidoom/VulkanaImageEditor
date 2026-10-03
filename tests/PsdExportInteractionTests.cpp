#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/ExportDialog.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/ProjectFile.hpp"
#include "imageeditor/ui/PsdImport.hpp"
#include "imageeditor/ui/Theme.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTreeWidget>
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
      std::cerr << __LINE__ << ": " #__VA_ARGS__ "\n";                         \
    }                                                                          \
  } while (false)
void run(QVulkanInstance *vulkan, const QString &output) {
  u::MainWindow window(vulkan, false);
  window.setUnsavedPromptEnabled(false);
  window.resize(1250, 900);
  window.show();
  QImage image(32, 32, QImage::Format_RGBA8888);
  image.fill(Qt::white);
  CHECK(image.save(output + "/other.png"));
  CHECK(window.openImageFromPath(output + "/other.png"));
  const auto other = window.activeDocumentId();
  c::Document doc({{256, 180}, 144});
  auto l =
      c::Layer::raster("Editable source",
                       std::make_shared<c::ContiguousRasterSurface>(
                           c::Extent2u{200, 140}, c::Rgba8{210, 90, 40, 200}));
  auto fx = std::make_shared<c::LayerEffectStack>();
  fx->items[0].enabled = true;
  fx->items[0].size = 5;
  l.effects = fx;
  CHECK(doc.insertLayer(0, l));
  const auto project = output + "/source.vulkana";
  CHECK(u::saveProject(project, doc));
  CHECK(window.openImageFromPath(project));
  const auto owner = window.activeDocumentId();
  auto *source = window.editorSession().document();
  const auto revision = source->revision(), state = source->contentState();
  const auto depth = window.editorSession().history().undoDepth();
  const auto selected = window.editorSession().selectedLayers();
  if (vulkan) {
    QElapsedTimer wait;
    wait.start();
    bool rendered = false;
    while (wait.elapsed() < 10000 && !rendered) {
      QTest::qWait(20);
      for (auto *w : QGuiApplication::allWindows())
        if (auto *canvas = dynamic_cast<imageeditor::render::CanvasWindow *>(w))
          rendered |= canvas->rendererStats().framesSubmitted > 0;
    }
    CHECK(rendered);
  }
  int stage = 0, ticks = 0;
  QTimer driver;
  driver.setInterval(20);
  QObject::connect(&driver, &QTimer::timeout, &window, [&] {
    // Repeated retained-artifact runs exercise the normal overwrite guard.
    for (auto *w : QApplication::topLevelWidgets())
      if (auto *question = qobject_cast<QMessageBox *>(w);
          question && question->isVisible()) {
        if (auto *yes = question->button(QMessageBox::Yes))
          yes->click();
      }
    auto *card = dynamic_cast<u::ExportDialog *>(
        window.findChild<QDialog *>("ExportDialog"));
    if (!card)
      return;
    if (++ticks > 1000) {
      CHECK(false);
      std::cerr
          << "stage " << stage << " "
          << card->findChild<QLabel *>("ExportStatus")->text().toStdString()
          << '\n';
      driver.stop();
      card->reject();
      return;
    }
    auto *write = card->findChild<QPushButton *>("ExportWrite");
    auto *cancel = card->findChild<QPushButton *>("ExportCancel");
    if (stage == 0) {
      ++stage;
      card->findChild<QComboBox *>("ExportFormat")->setCurrentIndex(4);
      card->setDestination(output + "/cancelled.psd");
    } else if (stage == 1 && write->isEnabled()) {
      stage = 10;
      CHECK(!card->findChild<QCheckBox *>("PsdExportReviewed"));
      CHECK(!card->findChild<QPushButton *>("PsdExportDetails"));
      CHECK(!card->isWindow());
      CHECK(card->settings().psd.instanceId == owner);
      CHECK(card->findChild<QTreeWidget *>("PsdExportItems")
                ->topLevelItemCount() == 1);
      CHECK(!window.activateDocument(other));
      CHECK(!window.closeDocument(owner));
      auto *preview = card->findChild<QCheckBox *>("PsdExportPreview");
      CHECK(preview->isChecked());
      auto *action = card->findChild<QComboBox *>("PsdExportAction");
      action->setCurrentIndex(action->findData(int(u::PsdExportAction::Omit)));
    } else if (stage == 10 && write->isEnabled()) {
      stage = 11;
      CHECK(card->findChild<QTreeWidget *>("PsdExportItems")
                ->currentItem()
                ->text(1) == "Omit");
      card->findChild<QCheckBox *>("PsdExportPreview")->setChecked(false);
    } else if (stage == 11 && write->isEnabled()) {
      stage = 12;
      auto *action = card->findChild<QComboBox *>("PsdExportAction");
      action->setCurrentIndex(
          action->findData(int(u::PsdExportAction::Editable)));
    } else if (stage == 12 && write->isEnabled()) {
      stage = 13;
      CHECK(!card->psdPreviewEnabled());
      card->findChild<QCheckBox *>("PsdExportPreview")->setChecked(true);
    } else if (stage == 13 && write->isEnabled()) {
      stage = 2;
    } else if (stage == 2 && write->isEnabled()) {
      ++stage;
      CHECK(card->grab().save(output + "/psd-export-dialog.png"));
      write->click();
      cancel->click();
    } else if (stage == 3 && cancel->isHidden()) {
      ++stage;
      CHECK(card->isVisible());
      CHECK(!QFileInfo::exists(output + "/cancelled.psd"));
      CHECK(!window.documentContext(owner)->exportSettings);
      card->setDestination(output + "/dialog.psd");
    } else if (stage == 4 && write->isEnabled()) {
      ++stage;
      write->click();
    } else if (stage == 5 &&
               card->findChild<QWidget *>("ExportSuccessOverlay")) {
      ++stage;
      CHECK(!card->findChild<QWidget *>("ExportSuccessOverlay")->isWindow());
      card->findChild<QPushButton *>("ExportSuccessClose")->click();
      card->findChild<QPushButton *>("ExportHeaderClose")->click();
    }
  });
  driver.start();
  window.exportImage();
  driver.stop();
  CHECK(stage == 6);
  CHECK(window.activeDocumentId() == owner);
  CHECK(source->revision() == revision && source->contentState() == state);
  CHECK(window.editorSession().history().undoDepth() == depth);
  CHECK(window.editorSession().selectedLayers() == selected);
  CHECK(window.projectPath() == QFileInfo(project).absoluteFilePath());
  if (stage == 6) {
    CHECK(window.documentContext(owner)->exportSettings->format ==
          u::ExportFormat::Psd);
    CHECK(!window.documentContext(other)->exportSettings);
    auto inspection =
        u::inspectPsd(output + "/dialog.psd", std::make_shared<u::PsdJob>());
    CHECK(inspection.error.isEmpty());
    CHECK(inspection.layers.size() == 1);
  }
  window.close();
  QTest::qWait(100);
}
int main(int argc, char **argv) {
  QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
  QApplication app(argc, argv);
  QStandardPaths::setTestModeEnabled(true);
  QTemporaryDir settings, files;
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                     settings.path());
  QCoreApplication::setOrganizationName("ImageEditorTests");
  QCoreApplication::setApplicationName("PsdExport");
  u::applyEditorTheme(app);
  auto output = qEnvironmentVariable("VULKANA_PSD_EXPORT_OUTPUT", files.path());
  CHECK(QDir().mkpath(output));
  int messages = 0;
  if (app.arguments().contains("--validation")) {
    QVulkanInstance vulkan;
    vulkan.setApiVersion(QVersionNumber(1, 2));
    vulkan.setLayers({"VK_LAYER_KHRONOS_validation"});
    vulkan.installDebugOutputFilter([&](auto severity, auto,
                                        const void *message) {
      if (!(severity & (QVulkanInstance::WarningSeverity |
                        QVulkanInstance::ErrorSeverity)))
        return false;
      ++messages;
      std::cerr << static_cast<const VkDebugUtilsMessengerCallbackDataEXT *>(
                       message)
                       ->pMessage
                << '\n';
      return false;
    });
    CHECK(vulkan.create());
    if (vulkan.isValid())
      run(&vulkan, output);
    vulkan.destroy();
    CHECK(messages == 0);
    std::cout << "Vulkan validation enabled; messages " << messages << '\n';
  } else
    run(nullptr, output);
  std::cout << "Qt " << qVersion() << " platform "
            << QGuiApplication::platformName().toStdString()
            << "; PSD export failures " << failures << '\n';
  return failures ? 1 : 0;
}
