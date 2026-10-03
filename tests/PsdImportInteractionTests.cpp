#include "PsdFixture.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/PsdImportDialog.hpp"
#include "imageeditor/ui/Theme.hpp"
#include <QApplication>
#include <QAction>
#include <QComboBox>
#include <QFile>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QStatusBar>
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
bool open(u::MainWindow &window, const QString &path, bool current = false,
          bool cancel = false, std::function<void()> mutation = {},
          bool skipUnsupported = false) {
  QTimer driver;
  driver.setInterval(20);
  int ticks = 0;
  bool validating = false;
  QObject::connect(&driver, &QTimer::timeout, &window, [&] {
    auto *card = window.findChild<QDialog *>("PsdImportDialog");
    if (!card)
      return;
    if (++ticks > (skipUnsupported ? 6000 : 600)) {
      CHECK(false);
      card->reject();
      return;
    }
    auto *button = card->findChild<QPushButton *>("PsdImport");
    if (skipUnsupported && !validating)
      for (auto *route : card->findChildren<QComboBox *>())
        if (route->objectName().startsWith("PsdRoute") &&
            route->currentData().toInt() < 0)
          route->setCurrentIndex(route->findData(int(u::PsdRoute::Skip)));
    if (!validating && button->isEnabled()) {
      CHECK(button->text() == "Validate Import");
      button->click();
      validating = true;
    }
    auto *overlay = card->findChild<QWidget *>("PsdConfirmationOverlay");
    auto *confirm = card->findChild<QPushButton *>("PsdConfirmImport");
    if (!overlay || !overlay->isVisible() || !confirm || !confirm->isEnabled())
      return;
    driver.stop();
    CHECK(!card->isWindow());
    CHECK(!overlay->isWindow() && overlay->parentWidget() == card);
    CHECK(!button->isEnabled());
    auto checkLayout = [&] {
      QCoreApplication::processEvents();
      auto *tree = card->findChild<QTreeWidget *>("PsdLayers");
      auto *preview = card->findChild<QLabel *>("PsdPreview");
      auto *fonts = card->findChild<QComboBox *>("PsdFontRequest");
      if (fonts->isVisible()) {
        CHECK(tree->geometry().bottom() < fonts->geometry().top());
        CHECK(preview->geometry().bottom() < fonts->geometry().top());
      }
      CHECK(preview->pixmap().height() <= preview->height());
      CHECK(preview->pixmap().width() <= preview->width());
    };
    checkLayout();
    auto *report = card->findChild<QPushButton *>("PsdReport");
    report->setChecked(true);
    checkLayout();
    report->setChecked(false);
    checkLayout();
    CHECK(card->findChild<QComboBox *>("PsdDestination")
              ->currentData()
              .toBool() == current);
    if (mutation)
      mutation();
    if (cancel) {
      card->reject(); // First Escape/Cancel dismisses only the confirmation.
      CHECK(card->isVisible() && !overlay->isVisible());
      card->reject();
    } else
      confirm->click();
  });
  driver.start();
  return current ? window.importImageAsLayerFromPath(path)
                 : window.openImageFromPath(path);
}
int main(int argc, char **argv) {
  QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
  QApplication app(argc, argv);
  QStandardPaths::setTestModeEnabled(true);
  QTemporaryDir settings;
  QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope,
                     settings.path());
  QCoreApplication::setOrganizationName("Vulkana-Psd-Test");
  QCoreApplication::setApplicationName("PsdImport");
  u::applyEditorTheme(app, u::ThemeSettings{});
  QVulkanInstance vulkan;
  QVulkanInstance *instance = nullptr;
  int validationMessages = 0;
  if (app.arguments().contains("--native")) {
    vulkan.setApiVersion(QVersionNumber(1, 2));
    vulkan.setLayers({"VK_LAYER_KHRONOS_validation"});
    CHECK(vulkan.create());
    if (!vulkan.isValid())
      return 1;
    instance = &vulkan;
    vulkan.installDebugOutputFilter(
        [&](QVulkanInstance::DebugMessageSeverityFlags flags,
            QVulkanInstance::DebugMessageTypeFlags, const void *message) {
          if (flags & (QVulkanInstance::WarningSeverity |
                       QVulkanInstance::ErrorSeverity)) {
            ++validationMessages;
            std::cerr
                << static_cast<const VkDebugUtilsMessengerCallbackDataEXT *>(
                       message)
                       ->pMessage
                << '\n';
          }
          return false;
        });
    std::cout << "Qt " << qVersion() << " platform "
              << QGuiApplication::platformName().toStdString()
              << " validation layer enabled "
              << vulkan.layers().contains("VK_LAYER_KHRONOS_validation")
              << '\n';
  }
  {
    QTemporaryDir files;
    QString path = files.filePath("independent.psd");
    QFile file(path);
    CHECK(file.open(QIODevice::WriteOnly));
    psdfixture::Layer text;
    text.name = "Unicode text";
    text.tags["TySh"] = psdfixture::textTag(QString::fromUtf8("Hello 😀\r"));
    psdfixture::Layer masked;
    masked.name = "Mask";
    masked.maskRect = {1, 0, 1, 1};
    masked.mask = QByteArray(1, 0);
    auto bytes = psdfixture::file({text, masked});
    CHECK(file.write(bytes) == bytes.size());
    file.close();
    u::MainWindow window(instance, false);
    window.setUnsavedPromptEnabled(false);
    window.resize(1250, 850);
    window.show();
    QString error;
    u::MainWindow::FileInteractions interactions;
    interactions.reportError = [&](const QString &e) { error = e; };
    window.setFileInteractions(std::move(interactions));
    const auto initialCount = window.documentCount();
    const auto initialDocument = window.activeDocumentId();
    CHECK(!open(window, path, false, true));
    CHECK(window.documentCount() == initialCount &&
          window.activeDocumentId() == initialDocument);
    CHECK(open(window, path));
    CHECK(window.documentCount() == 1);
    const auto first = window.activeDocumentId();
    auto *context = window.documentContext(first);
    CHECK(context && context->projectPath.isEmpty() &&
          context->sourcePath == path);
    auto &session = const_cast<c::EditorSession &>(window.editorSession());
    auto *document = session.document();
    CHECK(document->isModified());
    CHECK(document->layers().size() == 2);
    CHECK(std::holds_alternative<c::TextLayer>(document->layers()[0].payload));
    CHECK(document->layers()[1].mask);
    const auto selection = session.layerSelectionState();
    const auto revision = document->revision();
    CHECK(!open(window, path, true, true));
    CHECK(document->revision() == revision &&
          session.history().undoDepth() == 0 &&
          session.layerSelectionState() == selection);
    // A history retention target is not an insertion limit. The core retains
    // its newest complete command even when that one command is over budget.
    session.history() = c::History(1);
    CHECK(open(window, path, true));
    CHECK(document->layers().size() == 4 && session.history().undoDepth() == 1);
    CHECK(session.history().memoryUsed() > session.history().memoryBudget());
    const auto added = session.layerSelectionState();
    auto pixels =
        std::get<c::RasterLayer>(document->layers().back().payload).surface;
    CHECK(session.undo());
    CHECK(document->layers().size() == 2 &&
          session.layerSelectionState() == selection);
    CHECK(session.redo());
    CHECK(document->layers().size() == 4 &&
          session.layerSelectionState() == added);
    CHECK(std::get<c::RasterLayer>(document->layers().back().payload).surface ==
          pixels);
    CHECK(open(window, path));
    CHECK(window.documentCount() == 2);
    const auto second = window.activeDocumentId();
    CHECK(first != second);
    CHECK(window.documentContext(first)->session.document()->layers().size() ==
          4);
    CHECK(open(window, path, true, false, [&] {
      CHECK(!window.activateDocument(first));
      CHECK(!window.closeDocument(second));
    }));
    CHECK(window.activeDocumentId() == second);
    CHECK(window.editorSession().document()->layers().size() == 4);
    CHECK(!open(window, path, true, false, [&] {
      auto *d = const_cast<c::Document *>(window.editorSession().document());
      CHECK(d->setLayerOpacity(d->layers().back().id, .7F));
    }));
    CHECK(error.contains("destination changed"));
    CHECK(window.editorSession().document()->layers().size() == 4);
    if(instance) {
      auto& editing=const_cast<c::EditorSession&>(window.editorSession());
      editing.history()=c::History();
      const auto target=editing.document()->layers().back().id;
      editing.setActiveLayer(target);
      window.findChild<QAction*>("ToolAction_brush")->trigger();
      for(auto* native:QGuiApplication::allWindows())
        if(auto* canvas=dynamic_cast<imageeditor::render::CanvasWindow*>(native)) {
          const auto original=std::get<c::RasterLayer>(editing.document()->layer(target)->payload).surface;
          c::NormalizedPointerSample pointer;pointer.documentPosition={50,50};pointer.pressure=1;pointer.buttons=c::PointerButtonPrimary;
          CHECK(canvas->onBrushStrokeBegan(pointer));QTest::qWait(100);
          pointer.documentPosition={75,75};pointer.timestampMicroseconds=100000;
          CHECK(canvas->onBrushStrokeMoved(pointer));QTest::qWait(100);
          CHECK(canvas->onBrushStrokeEnded(pointer));QTest::qWait(100);
          CHECK(std::get<c::RasterLayer>(editing.document()->layer(target)->payload).surface!=original);
          CHECK(canvas->rendererStats().compositionError.empty());
          CHECK(editing.undo());CHECK(std::get<c::RasterLayer>(editing.document()->layer(target)->payload).surface==original);
          CHECK(editing.redo());
          canvas->setDocument(editing.document()->snapshot(),false);QTest::qWait(100);
          CHECK(canvas->rendererStats().compositionError.empty());
        }
    }
    if (auto sample = qEnvironmentVariable("VULKANA_PSD_REFERENCE");
        !sample.isEmpty()) {
      CHECK(open(window, sample, false, false, [&] {
        auto *dialog = window.findChild<QDialog *>("PsdImportDialog");
        dialog->grab().save(qEnvironmentVariable(
            "VULKANA_PSD_UI_SCREENSHOT", "/tmp/vulkana-psd-review.png"));
      }));
      CHECK(window.editorSession().document()->layers().size() == 9);
    }
    if (auto heavy = qEnvironmentVariable("VULKANA_PSD_HEAVY_REFERENCE");
        !heavy.isEmpty()) {
      // Four clipped members now convert inside two native clipping groups.
      CHECK(open(window, heavy, false, false, {}, true));
      const auto *imported = window.editorSession().document();
      CHECK(imported->layers().size() == 41);
      CHECK(imported->tree().containers.size() == 9);
      CHECK(imported->canvas().extent == c::Extent2u{2979, 4000});
    }
    if (instance) {
      QTest::qWait(400);
      bool found = false;
      for (auto *native : QGuiApplication::allWindows())
        if (auto *canvas =
                dynamic_cast<imageeditor::render::CanvasWindow *>(native)) {
          auto stats = canvas->rendererStats();
          CHECK(stats.framesSubmitted > 0 && stats.compositionError.empty());
          std::cout << "GPU " << stats.deviceName << " frames "
                    << stats.framesSubmitted << " staging_capacity_bytes "
                    << stats.stagingCapacityBytes << '\n';
          // Status sampling runs across this interval without touching the
          // native canvas or requesting another upload/render of idle content.
          QTest::qWait(1200);
          const auto after = canvas->rendererStats();
          CHECK(after.framesSubmitted == stats.framesSubmitted);
          CHECK(after.uploadedBytes == stats.uploadedBytes);
          auto *memory = window.findChild<QLabel *>("MemoryStatus");
          CHECK(memory && memory->isVisible() && memory->text().contains("GPU cache"));
          if (const auto screenshot = qEnvironmentVariable("VULKANA_MEMORY_STATUS_PREVIEW"); !screenshot.isEmpty())
            CHECK(window.statusBar()->grab().save(screenshot));
          std::cout << "Memory status idle frame delta "
                    << after.framesSubmitted - stats.framesSubmitted << '\n';
          found = true;
        }
      CHECK(found);
    }
  }
  CHECK(validationMessages == 0);
  std::cout << "PSD interaction failures " << failures
            << " validation messages " << validationMessages << '\n';
  return failures ? 1 : 0;
}
