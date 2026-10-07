#include "imageeditor/core/LayerMaskEdit.hpp"
#include "imageeditor/core/SelectionCommands.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/CompactValueControl.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/OverlayDockWorkspace.hpp"
#include "imageeditor/ui/ProjectFile.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/ui/WorkspacePanel.hpp"
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QImage>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QVulkanInstance>
#include <iostream>
#include <sys/resource.h>
namespace c = imageeditor::core;
namespace u = imageeditor::ui;
namespace r = imageeditor::render;
int failures = 0;
#define CHECK(...)                                                             \
  do {                                                                         \
    if (!(__VA_ARGS__)) {                                                      \
      ++failures;                                                              \
      std::cerr << __LINE__ << ": " #__VA_ARGS__ "\n";                         \
    }                                                                          \
  } while (false)
void spin() {
  QCoreApplication::sendPostedEvents();
  QCoreApplication::processEvents();
}
bool wait(const std::function<bool()> &predicate) {
  for (int i = 0; i < 500; ++i) {
    spin();
    if (predicate())
      return true;
    QTest::qWait(10);
  }
  return false;
}
int main(int argc, char **argv) {
  QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
  QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
  QApplication app(argc, argv);
  QStandardPaths::setTestModeEnabled(true);
  QTemporaryDir temp;
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temp.path());
  u::applyEditorTheme(app);
  QVulkanInstance instance;
  QVulkanInstance *vulkan = nullptr;
  if (app.arguments().contains("--native")) {
    instance.setApiVersion(QVersionNumber(1, 2));
    instance.setLayers({"VK_LAYER_KHRONOS_validation"});
    CHECK(instance.create());
    if (!instance.isValid())
      return 1;
    vulkan = &instance;
    instance.installDebugOutputFilter([](auto severity, auto type,
                                         const void *data) {
      if (type.testFlag(QVulkanInstance::ValidationMessage) &&
          (severity.testFlag(QVulkanInstance::WarningSeverity) ||
           severity.testFlag(QVulkanInstance::ErrorSeverity))) {
        ++failures;
        std::cerr << static_cast<const VkDebugUtilsMessengerCallbackDataEXT *>(
                         data)
                         ->pMessage
                  << '\n';
      }
      return false;
    });
  }
  try {
    u::MainWindow w(vulkan, false, false);
    w.setUnsavedPromptEnabled(false);
    bool discardRefinement = false;
    int refinementPrompts = 0;
    u::MainWindow::FileInteractions files;
    files.confirmCancelRefinement = [&](const QString &) {
      ++refinementPrompts;
      return discardRefinement;
    };
    w.setFileInteractions(files);
    w.resize(1320, 900);
    w.show();
    spin();
    QImage image(128, 96, QImage::Format_RGBA8888);
    image.fill(QColor(90, 170, 230, 255));
    const auto path = temp.filePath("source.png");
    CHECK(image.save(path));
    CHECK(w.openImageFromPath(path));
    spin();
    auto &session = const_cast<c::EditorSession &>(w.editorSession());
    auto *doc = session.document();
    const auto id = *session.activeLayer();
    auto *workspace = dynamic_cast<u::OverlayDockWorkspace *>(
        w.findChild<QWidget *>("CanvasWorkspace"));
    CHECK(workspace);
    auto *colorShell = dynamic_cast<u::WorkspacePanel *>(
        workspace->panelOverlay()->findChild<QWidget *>("ColorPanelShell"));
    CHECK(colorShell);
    workspace->floatPanel(colorShell, {340, 230, 360, 300});
    workspace->setRulerVisible(Qt::Horizontal, true);
    workspace->setRulerVisible(Qt::Vertical, true);
    workspace->setRulerFarEdge(Qt::Vertical, true);
    spin();
    const auto savedTabs = workspace->savePanelTabs();
    const auto floatingGeometry = workspace->floatingPanelGeometry(colorShell);
    r::CanvasWindow *canvas = nullptr;
    for (auto *window : QGuiApplication::allWindows())
      if (window->objectName() == "VulkanCanvasWindow")
        canvas = dynamic_cast<r::CanvasWindow *>(window);
    CHECK(canvas);
    auto *selectAll = w.findChild<QAction *>("SelectAllAction");
    auto *refine = w.findChild<QAction *>("RefineSelectionAction");
    auto *refineMask = w.findChild<QAction *>("RefineMaskAction");
    CHECK(selectAll && refine && refineMask);
    CHECK(!refine->isEnabled());
    auto refresh = [&] {
      selectAll->trigger();
      spin();
    };
    refresh();
    auto panel = [&]() -> QWidget * {
      for (auto *p : workspace->panelOverlay()->findChildren<QWidget *>(
               "RefineSelectionWorkspace"))
        if (!p->isHidden())
          return p;
      return nullptr;
    };
    auto ready = [&] {
      return wait([&] {
        auto *p = panel();
        return p && p->findChild<QPushButton *>("RefineApply")->isEnabled();
      });
    };
    auto cancel = [&] {
      panel()->findChild<QPushButton *>("RefineCancel")->click();
      CHECK(wait([&] {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        return !panel();
      }));
      CHECK(!workspace->panelsSuppressed());
      CHECK(workspace->panelFrame(colorShell)->isVisible());
      CHECK(workspace->savePanelTabs() == savedTabs);
      CHECK(workspace->floatingPanelGeometry(colorShell) == floatingGeometry);
    };
    auto apply = [&] {
      panel()->findChild<QPushButton *>("RefineApply")->click();
      spin();
      QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
      spin();
      CHECK(!panel());
      CHECK(!workspace->panelsSuppressed());
      CHECK(workspace->panelFrame(colorShell)->isVisible());
    };
    auto set = [&](const char *name, double value) {
      panel()->findChild<QDoubleSpinBox *>(name)->setValue(value);
      CHECK(ready());
    };
    auto selection =
        c::SelectionMask::rectangle(doc->canvas().extent, {24, 18, 50, 48}, 91);
    session.execute(
        std::make_unique<c::SetSelectionCommand>(selection, "Fixture"));
    doc->markSaved();
    // Below marching-ants threshold is still an eligible explicit selection.
    CHECK(refine->isEnabled());
    for (auto *action : w.findChildren<QAction *>())
      if (action->shortcut() == QKeySequence("M")) {
        action->trigger();
        break;
      }
    auto *transformSelection =
        w.findChild<QAction *>("TransformSelectionAction");
    CHECK(transformSelection && transformSelection->isEnabled());
    transformSelection->trigger();
    CHECK(canvas->scene().transformOverlay.has_value());
    const QPointF click{50, 50};
    const QPointF global = canvas->mapToGlobal(click.toPoint());
    QMouseEvent rightPress(QEvent::MouseButtonPress, click, global,
                           Qt::RightButton, Qt::RightButton, Qt::NoModifier);
    QMouseEvent rightRelease(QEvent::MouseButtonRelease, click, global,
                             Qt::RightButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(canvas, &rightPress);
    QCoreApplication::sendEvent(canvas, &rightRelease);
    CHECK(canvas->finishContextMenuSuppressed());
    bool contextOpened = false;
    QTimer::singleShot(0, [&] {
      if (auto *popup =
              qobject_cast<QMenu *>(QApplication::activePopupWidget())) {
        contextOpened = true;
        popup->close();
      }
    });
    QContextMenuEvent finishedContext(QContextMenuEvent::Mouse, click.toPoint(),
                                      global.toPoint());
    QCoreApplication::sendEvent(canvas, &finishedContext);
    spin();
    CHECK(!contextOpened); // A right-click completion is not a menu request.
    const auto depth = session.history().undoDepth();
    const auto revision = doc->revision();
    const auto geometry = canvas->geometry();
    refine->trigger();
    CHECK(ready());
    CHECK(panel() && !panel()->isWindow());
    CHECK(canvas->geometry() == geometry);
    CHECK(doc->selection() == selection && doc->revision() == revision);
    CHECK(workspace->panelsSuppressed());
    CHECK(!workspace->panelFrame(colorShell)->isVisible());
    CHECK(!workspace->rightPanelCard()->isVisible());
    CHECK(!workspace->panelOverlay()->windowHandle()->mask().contains(
        floatingGeometry.center()));
    CHECK(workspace->rulerStrip(Qt::Horizontal)->isVisible());
    CHECK(workspace->rulerStrip(Qt::Vertical)->isVisible());
    CHECK(!panel()->geometry().intersects(
        workspace->rulerStrip(Qt::Horizontal)->geometry()));
    CHECK(!panel()->geometry().intersects(
        workspace->rulerStrip(Qt::Vertical)->geometry()));
    const auto windowSize = w.size();
    w.resize(windowSize - QSize(70, 50));
    spin();
    spin();
    CHECK(panel()->geometry().right() <
          workspace->rulerStrip(Qt::Vertical)->geometry().left());
    CHECK(panel()->geometry().top() >
          workspace->rulerStrip(Qt::Horizontal)->geometry().bottom());
    w.resize(windowSize);
    spin();
    spin();
    CHECK(panel()->findChild<QDoubleSpinBox *>("RefineRadius")->maximum() ==
          250);
    set("RefineRadius", 250);
    set("RefineRadius", 0);
    cancel();
    refine->trigger();
    CHECK(ready());
    auto *brushSize = panel()->findChild<QDoubleSpinBox *>("RefineBrushSize");
    const double originalBrushSize = brushSize->value();
    auto brushKey = [&](QEvent::Type type, int key, bool repeat = false) {
      QKeyEvent event(type, key, Qt::NoModifier, {}, repeat);
      QCoreApplication::sendEvent(canvas, &event);
    };
    if (auto *focus = QApplication::focusWidget())
      focus->clearFocus();
    brushKey(QEvent::ShortcutOverride, Qt::Key_BracketRight);
    CHECK(brushSize->value() == originalBrushSize);
    brushKey(QEvent::KeyPress, Qt::Key_BracketRight);
    const auto largerBrush = brushSize->value();
    CHECK(largerBrush > originalBrushSize);
    brushKey(QEvent::KeyPress, Qt::Key_BracketLeft);
    CHECK(brushSize->value() < largerBrush);
    brushSize->setValue(1);
    brushKey(QEvent::KeyPress, Qt::Key_BracketRight, true);
    CHECK(brushSize->value() > 1);
    brushSize->setValue(2000);
    brushKey(QEvent::KeyPress, Qt::Key_BracketRight);
    CHECK(brushSize->value() == 2000);
    brushSize->setValue(originalBrushSize);
    auto *sizeControl = dynamic_cast<u::CompactValueControl *>(brushSize);
    QTest::mouseDClick(sizeControl, Qt::LeftButton, Qt::NoModifier,
                       sizeControl->valueFieldRect().center());
    CHECK(sizeControl->isManualEntryActive());
    brushKey(QEvent::KeyPress, Qt::Key_BracketRight);
    CHECK(brushSize->value() ==
          originalBrushSize); // Numeric editing owns keys.
    sizeControl->finishEditing();
    brushSize->clearFocus();
    if (!qEnvironmentVariableIsEmpty("VULKANA_REFINE_CAPTURE"))
      workspace->panelOverlay()->grab().save(
          qEnvironmentVariable("VULKANA_REFINE_CAPTURE"));
    set("RefineFeather", 5);
    CHECK(doc->selection() == selection);
    auto *undo = w.findChild<QAction *>("UndoAction");
    auto *redo = w.findChild<QAction *>("RedoAction");
    undo->trigger();
    CHECK(ready());
    CHECK(panel()->findChild<QDoubleSpinBox *>("RefineFeather")->value() == 0);
    redo->trigger();
    CHECK(ready());
    CHECK(panel()->findChild<QDoubleSpinBox *>("RefineFeather")->value() == 5);
    // Cancelling a parameter gesture must not consume local Redo or insert a
    // phantom parameter state. Use the control's actual gesture callbacks.
    undo->trigger();
    CHECK(ready());
    auto *control = dynamic_cast<u::CompactValueControl *>(
        panel()->findChild<QDoubleSpinBox *>("RefineFeather"));
    QTest::mousePress(control, Qt::LeftButton, Qt::NoModifier,
                      control->valueFieldRect().center());
    CHECK(control->interactionActive());
    control->setValue(9);
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(canvas, &escape);
    QTest::mouseRelease(control, Qt::LeftButton, Qt::NoModifier,
                        control->valueFieldRect().center());
    CHECK(ready());
    CHECK(control->value() == 0 && redo->isEnabled() && !undo->isEnabled());
    redo->trigger();
    CHECK(ready());
    CHECK(control->value() == 5);
    panel()->findChild<QPushButton *>("RefineReset")->click();
    CHECK(ready() && control->value() == 0);
    undo->trigger();
    CHECK(ready() && control->value() == 5);
    cancel();
    CHECK(doc->selection() == selection);
    CHECK(session.history().undoDepth() == depth);
    CHECK(!doc->isModified());
    refine->trigger();
    CHECK(ready());
    CHECK(panel()->findChild<QDoubleSpinBox *>("RefineBrushSize")->value() ==
          originalBrushSize);
    set("RefineFeather", 5);
    apply();
    CHECK(doc->selection() != selection);
    CHECK(!doc->isModified());
    CHECK(session.history().undoDepth() == depth + 1);
    CHECK(session.undo());
    CHECK(doc->selection() == selection);
    refine->trigger();
    CHECK(ready());
    apply();
    CHECK(session.history().canRedo());
    // Layer output is atomic and restores the active thumbnail on Undo.
    refine->trigger();
    CHECK(ready());
    panel()->findChild<QComboBox *>("RefineOutput")->setCurrentIndex(1);
    CHECK(ready());
    apply();
    CHECK(doc->layer(id)->mask);
    CHECK(session.editingLayerMask());
    CHECK(session.undo());
    CHECK(!doc->layer(id)->mask);
    CHECK(!session.editingLayerMask());
    CHECK(session.redo());
    CHECK(session.editingLayerMask());
    // A flipped, offset native mask retains exact storage on neutral Apply.
    auto native = std::make_shared<c::LayerMask>();
    std::vector<std::uint8_t> nativeBytes(180 * 140, 31);
    for (int y = 20; y < 90; ++y)
      for (int x = 22; x < 142; ++x)
        nativeBytes[std::size_t(y * 180 + x)] = 180;
    native->coverage = c::SelectionMask::fromR8({180, 140}, nativeBytes, 180);
    native->outside = 31;
    native->localToMask = {1, 0, 17, 0, 1, 12};
    doc->setLayerMask(id, native);
    doc->setLayerTransform(id, {-1, 0, 128, 0, 1, 0});
    refresh();
    const auto temporary = doc->selection();
    refineMask->trigger();
    CHECK(ready());
    apply();
    CHECK(doc->layer(id)->mask == native);
    CHECK(doc->selection() == temporary);
    refineMask->trigger();
    CHECK(ready());
    set("RefineFeather", 3);
    apply();
    const auto changed = doc->layer(id)->mask;
    CHECK(changed != native);
    CHECK(changed->outside == 31);
    CHECK(changed->localToMask == native->localToMask);
    CHECK(doc->selection() == temporary);
    CHECK(session.undo());
    CHECK(doc->layer(id)->mask == native);
    CHECK(session.redo());
    CHECK(doc->layer(id)->mask == changed);
    const auto savedPath = temp.filePath("refined.vulkana");
    CHECK(u::saveProject(savedPath, *doc));
    const auto reopened = u::loadProject(savedPath);
    CHECK(reopened &&
          c::equivalentLayerMasks(reopened.document->layer(id)->mask, changed));
    CHECK(reopened && reopened.document->layer(id)->localToDocument ==
                          doc->layer(id)->localToDocument);
    const auto &source =
        std::get<c::RasterLayer>(doc->layer(id)->payload).surface;
    std::array<std::byte, 4> pixel;
    source->copyRgba8({12, 34, 1, 1}, pixel, 4);
    CHECK((pixel == std::array{std::byte{90}, std::byte{170}, std::byte{230},
                               std::byte{255}}));
    // Coverage at the native storage boundary needs real outside context and
    // extends its frame without changing the original document coordinates.
    auto edge = std::make_shared<c::LayerMask>();
    edge->coverage = c::SelectionMask::filled({40, 30}, 255);
    edge->outside = 0;
    edge->localToMask = {1, 0, -30, 0, 1, -20};
    doc->setLayerTransform(id, {});
    doc->setLayerMask(id, edge);
    refresh();
    refineMask->trigger();
    CHECK(ready());
    set("RefineFeather", 6);
    apply();
    const auto expanded = doc->layer(id)->mask;
    CHECK(expanded->coverage->extent().width > 40);
    CHECK(c::layerMaskCoverage(expanded, {29.5, 35.5}) > 0);
    CHECK(c::layerMaskCoverage(expanded, {50.5, 35.5}) > .99);
    CHECK(c::layerMaskCoverage(expanded, {90.5, 35.5}) == 0);
    CHECK(session.undo() && doc->layer(id)->mask == edge);
    // Fully black masks remain editable, independent of temporary selection.
    auto black = std::make_shared<c::LayerMask>();
    black->coverage = c::SelectionMask::filled({128, 96}, 0);
    black->outside = 0;
    doc->setLayerTransform(id, {});
    doc->setLayerMask(id, black);
    refresh();
    doc->setSelection(c::SelectionMask::filled(doc->canvas().extent, 0));
    refineMask->trigger();
    CHECK(ready());
    panel()->findChild<QComboBox *>("RefineBrush")->setCurrentIndex(1);
    c::NormalizedPointerSample sample;
    sample.documentPosition = {50.5, 40.5};
    sample.buttons = c::PointerButtonPrimary;
    CHECK(canvas->onBrushStrokeBegan(sample));
    CHECK(canvas->onBrushStrokeEnded(sample));
    CHECK(ready());
    apply();
    CHECK(doc->layer(id)->mask->coverage->coverageAtDocumentPixel(50, 40) > 0);
    CHECK(doc->selection()->bounds().empty());
    // Pending jobs cannot escape Cancel or redirect into a different tab.
    refresh();
    refine->trigger();
    CHECK(ready());
    const auto original = doc->selection();
    panel()->findChild<QDoubleSpinBox *>("RefineFeather")->setValue(60);
    cancel();
    QTest::qWait(100);
    CHECK(doc->selection() == original);
    const auto first = w.activeDocumentId();
    CHECK(w.openImageFromPath(path));
    const auto second = w.activeDocumentId();
    CHECK(first != second);
    CHECK(w.activateDocument(first));
    refresh();
    refine->trigger();
    CHECK(ready());
    set("RefineFeather", 5);
    const auto firstSelection = doc->selection();
    const auto firstHistory = session.history().undoDepth();
    auto *firstPanel = panel();
    CHECK(w.menuBar()->isEnabled());
    auto *fileMenu = w.findChild<QMenu *>("FileMenu");
    auto *newAction = w.findChild<QAction *>("NewDocumentAction");
    auto *openAction = w.findChild<QAction *>("OpenDocumentAction");
    CHECK(fileMenu && newAction && openAction);
    CHECK(fileMenu->menuAction()->isEnabled());
    CHECK(!w.findChild<QMenu *>("SelectMenu")->menuAction()->isEnabled());
    const int promptsBeforeNew = refinementPrompts;
    // Exercise the actual menu, not only the programmatic tab-opening API.
    QTest::mousePress(w.menuBar(), Qt::LeftButton, Qt::NoModifier,
                     w.menuBar()->actionGeometry(fileMenu->menuAction()).center());
    CHECK(fileMenu->isVisible());
    QTest::mouseRelease(w.menuBar(), Qt::LeftButton, Qt::NoModifier,
                     w.menuBar()->actionGeometry(fileMenu->menuAction()).center());
    spin();
    CHECK(fileMenu->isVisible());
    fileMenu->setActiveAction(newAction);
    bool newShown = false;
    QTimer::singleShot(0, &w, [&] {
      auto *dialog = w.findChild<QDialog *>("NewDocumentDialog");
      CHECK(dialog && dialog->isVisible());
      if (dialog) {
        newShown = true;
        CHECK(!panel() && w.documentContext(first)->refinement);
        QTest::keyClick(dialog, Qt::Key_Escape);
      }
    });
    QTest::keyClick(fileMenu, Qt::Key_Return);
    spin();
    CHECK(newShown && ready() && panel() == firstPanel);
    CHECK(panel()->findChild<QDoubleSpinBox *>("RefineFeather")->value() == 5);
    QTimer::singleShot(0, &w, [&] {
      auto *dialog = w.findChild<QDialog *>("NewDocumentDialog");
      CHECK(dialog);
      if (dialog)
        dialog->findChild<QDialogButtonBox *>("CanvasDialogButtons")
            ->button(QDialogButtonBox::Ok)->click();
    });
    QTest::keyClick(firstPanel, Qt::Key_N, Qt::ControlModifier);
    const auto created = w.activeDocumentId();
    CHECK(created != first && created != second);
    CHECK(w.documentContext(first)->refinement && !panel());
    CHECK(w.closeDocument(created));
    CHECK(w.activateDocument(first) && ready());
    bool pickerShown = false;
    QTimer::singleShot(0, &w, [&] {
      auto *picker = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
      CHECK(picker);
      if (picker) {
        pickerShown = true;
        picker->reject();
      }
    });
    QTest::keyClick(panel(), Qt::Key_O, Qt::ControlModifier);
    CHECK(pickerShown && ready() && panel() == firstPanel);
    QTimer::singleShot(0, &w, [&] {
      auto *picker = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
      CHECK(picker);
      if (picker) {
        picker->selectFile(path);
        QMetaObject::invokeMethod(picker, "accept", Qt::DirectConnection);
      }
    });
    openAction->trigger();
    const auto opened = w.activeDocumentId();
    CHECK(opened != first && opened != second);
    CHECK(w.documentContext(first)->refinement && !panel());
    CHECK(w.closeDocument(opened));
    CHECK(w.activateDocument(first) && ready() && panel() == firstPanel);
    CHECK(refinementPrompts == promptsBeforeNew);
    CHECK(doc->selection() == firstSelection && session.history().undoDepth() == firstHistory);
    // Navigation is available even with numeric focus inside the workspace.
    QTest::keyClick(firstPanel, Qt::Key_Tab, Qt::ControlModifier);
    CHECK(w.activeDocumentId() == second);
    CHECK(!panel() && firstPanel->isHidden());
    CHECK(w.documentContext(first)->refinement != nullptr);
    CHECK(w.menuBar()->isEnabled());
    CHECK(!workspace->panelsSuppressed() &&
          workspace->panelFrame(colorShell)->isVisible());
    files.chooseSavePath = [&] { return temp.filePath("other-tab.vulkana"); };
    w.setFileInteractions(files);
    const int promptsBeforeSave = refinementPrompts;
    CHECK(w.saveDocument());
    CHECK(refinementPrompts == promptsBeforeSave);
    CHECK(w.documentContext(first)->refinement != nullptr);
    refresh();
    auto &secondSession = const_cast<c::EditorSession &>(w.editorSession());
    const auto secondSelection = secondSession.document()->selection();
    refine->trigger();
    CHECK(ready());
    set("RefineFeather", 2);
    auto *secondPanel = panel();
    CHECK(secondPanel != firstPanel);
    // A late signal from a suspended tab is never routed to this session.
    firstPanel->findChild<QDoubleSpinBox *>("RefineFeather")->setValue(7);
    CHECK(panel()->findChild<QDoubleSpinBox *>("RefineFeather")->value() == 2);
    firstPanel->findChild<QDoubleSpinBox *>("RefineFeather")->setValue(5);
    panel()->findChild<QDoubleSpinBox *>("RefineRadius")->setValue(250);
    spin(); // May leave an in-flight worker when switching away.
    CHECK(w.activateDocument(first));
    CHECK(ready() && panel() == firstPanel);
    CHECK(panel()->findChild<QDoubleSpinBox *>("RefineFeather")->value() == 5);
    w.findChild<QAction *>("UndoAction")->trigger();
    CHECK(ready());
    CHECK(panel()->findChild<QDoubleSpinBox *>("RefineFeather")->value() == 0);
    w.findChild<QAction *>("RedoAction")->trigger();
    CHECK(ready());
    CHECK(panel()->findChild<QDoubleSpinBox *>("RefineFeather")->value() == 5);
    CHECK(doc->selection() == firstSelection &&
          session.history().undoDepth() == firstHistory);
    // Block editing shortcuts locally without disabling document navigation.
    QTest::keyClick(panel(), Qt::Key_A, Qt::ControlModifier);
    QTest::keyClick(panel(), Qt::Key_E);
    CHECK(doc->selection() == firstSelection);
    CHECK(session.activeTool() == c::ToolId::Brush);
    CHECK(w.openImageFromPath(path));
    const auto unrelated = w.activeDocumentId();
    CHECK(w.activateDocument(first));
    const int promptsBeforeClose = refinementPrompts;
    CHECK(w.closeDocument(unrelated));
    CHECK(refinementPrompts == promptsBeforeClose && panel() == firstPanel);
    // Closing another refining tab asks only for that tab, without switching.
    const int beforePrompt = refinementPrompts;
    CHECK(!w.closeDocument(second));
    CHECK(refinementPrompts == beforePrompt + 1 && panel() == firstPanel);
    CHECK(w.documentContext(second)->refinement != nullptr);
    CHECK(secondSession.document()->selection() == secondSelection);
    // The production prompt is recoverable, with the same default as an
    // unsaved-document question. Closing it cannot discard either session.
    auto productionFiles = files;
    productionFiles.confirmCancelRefinement = {};
    w.setFileInteractions(productionFiles);
    bool promptShown = false;
    QTimer::singleShot(0, &w, [&] {
      auto *question =
          qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
      CHECK(question);
      if (question) {
        promptShown = true;
        CHECK(question->defaultButton() ==
              question->button(QMessageBox::Cancel));
        question->reject();
      }
    });
    CHECK(!w.closeDocument(first));
    CHECK(promptShown && panel() == firstPanel);
    w.setFileInteractions(files);
    cancel();
    CHECK(w.activateDocument(second));
    CHECK(ready() && panel() == secondPanel);
    CHECK(panel()->findChild<QDoubleSpinBox *>("RefineFeather")->value() == 2);
    CHECK(w.activateDocument(first));
    discardRefinement = true;
    CHECK(w.closeDocument(second));
    CHECK(w.activeDocumentId() == first && !w.documentContext(second));
    discardRefinement = false;
    CHECK(w.menuBar()->isEnabled() && !panel());
    CHECK(w.activateDocument(first));
    refresh();
    refine->trigger();
    CHECK(ready());
    panel()->findChild<QComboBox *>("RefineOutput")->setCurrentIndex(2);
    CHECK(ready());
    const auto count = doc->layers().size();
    apply();
    CHECK(doc->layers().size() == count + 1);
    CHECK(doc->layer(id)->visible);
    CHECK(session.undo());
    CHECK(doc->layers().size() == count);
    CHECK(session.redo());
    CHECK(doc->layers().size() == count + 1);
    // Native text, shape, and adjustment masks stay independent of content.
    c::TextLayer words;
    words.utf8 = "Editable O/B";
    c::ShapeLayer shape;
    shape.kind = c::ShapeKind::Ellipse;
    shape.size = {80, 65};
    for (auto typed :
         {c::Layer::text("Words", words), c::Layer::shape("Shape", shape),
          c::Layer::adjustment("Tone")}) {
      typed.mask = black;
      typed.localToDocument = {1, .05, 8, .08, 1, 10, .0003, .0001, 1};
      const auto payload = typed.payload;
      CHECK(doc->insertLayer(doc->layers().size(), typed));
      session.setActiveLayer(typed.id);
      refresh();
      const auto oldTemporary = doc->selection();
      refineMask->trigger();
      CHECK(ready());
      panel()->findChild<QComboBox *>("RefineBrush")->setCurrentIndex(1);
      sample.documentPosition = typed.localToDocument.map({50.5, 40.5});
      CHECK(canvas->onBrushStrokeBegan(sample));
      CHECK(canvas->onBrushStrokeEnded(sample));
      CHECK(ready());
      apply();
      CHECK(doc->layer(typed.id)->payload.index() == payload.index());
      if (auto *text = std::get_if<c::TextLayer>(&payload))
        CHECK(std::get<c::TextLayer>(doc->layer(typed.id)->payload) == *text);
      if (auto *geometry = std::get_if<c::ShapeLayer>(&payload))
        CHECK(std::get<c::ShapeLayer>(doc->layer(typed.id)->payload) ==
              *geometry);
      CHECK(doc->layer(typed.id)->localToDocument == typed.localToDocument);
      CHECK(c::layerMaskCoverage(doc->layer(typed.id)->mask, {50.5, 40.5}) >
            .5);
      CHECK(doc->selection() == oldTemporary);
      CHECK(session.undo() && doc->layer(typed.id)->mask == black);
      CHECK(session.redo());
    }
    // Scope/history across a clipped base: keep the original visible and the
    // duplicate in the same container, never retarget an unrelated neighbor.
    auto hierarchy = doc->tree();
    const auto group = c::makeLayerId();
    const auto top = *session.activeLayer();
    std::erase(hierarchy.roots, id);
    std::erase(hierarchy.roots, top);
    hierarchy.roots.push_back(group);
    hierarchy.containers.push_back({group,
                                    "Clip",
                                    c::ContainerKind::ClippingMaskGroup,
                                    c::ColorLabel::None,
                                    {id, top}});
    CHECK(doc->replaceStructure(doc->tree(), hierarchy));
    session.setActiveLayer(id);
    refresh();
    refineMask->trigger();
    CHECK(ready());
    panel()->findChild<QComboBox *>("RefineOutput")->setCurrentIndex(2);
    CHECK(ready());
    apply();
    const auto duplicate = *session.activeLayer();
    CHECK(doc->tree().container(group)->children ==
          std::vector<c::LayerId>{id, duplicate, top});
    CHECK(doc->layer(id)->visible && doc->layer(duplicate)->mask != nullptr);
    CHECK(session.undo() && doc->tree() == hierarchy);
    CHECK(session.redo());
    CHECK(u::saveProject(savedPath, *doc));
    const auto typedReopen = u::loadProject(savedPath);
    CHECK(typedReopen && typedReopen.document->tree() == doc->tree());
    CHECK(typedReopen &&
          c::equivalentLayerMasks(typedReopen.document->layer(duplicate)->mask,
                                  doc->layer(duplicate)->mask));
    refresh();
    refine->trigger();
    CHECK(ready());
    CHECK(!w.close());
    CHECK(panel());
    const int closeKeyPrompts = refinementPrompts;
    QTest::keyClick(panel(), Qt::Key_W, Qt::ControlModifier);
    CHECK(refinementPrompts == closeKeyPrompts + 1 && panel());
    QTest::keyClick(panel(), Qt::Key_Q, Qt::ControlModifier);
    CHECK(refinementPrompts == closeKeyPrompts + 2 && panel());
    // Save asks before leaving the workspace and never stores its preview.
    CHECK(!w.saveDocument(true));
    CHECK(panel());
    discardRefinement = true;
    files.chooseSavePath = [&] {
      return temp.filePath("after-refinement.vulkana");
    };
    w.setFileInteractions(files);
    const auto beforeSave = doc->selection();
    CHECK(w.saveDocument(true));
    CHECK(!panel() && doc->selection() == beforeSave);
    CHECK(w.menuBar()->isEnabled());
    discardRefinement = false;
    if (app.arguments().contains("--profile")) {
      for (auto e : {QSize(3840, 2160), QSize(5120, 2880)}) {
        QImage large(e, QImage::Format_RGBA8888);
        large.fill(QColor(15, 80, 200));
        for (int y = 0; y < e.height(); ++y)
          for (int x = 0; x < e.width() / 2; ++x)
            large.setPixelColor(x, y, QColor(220, 150, 30));
        CHECK(large.save(path));
        CHECK(w.openImageFromPath(path));
        auto &active = const_cast<c::EditorSession &>(w.editorSession());
        refresh();
        active.document()->setSelection(
            c::SelectionMask::rectangle(active.document()->canvas().extent,
                                        {0, 0, e.width() / 2 + 2, e.height()}));
        QElapsedTimer timer;
        timer.start();
        refine->trigger();
        CHECK(ready());
        std::cout << e.width() << "x" << e.height()
                  << " workspace_prepare_ms=" << timer.elapsed() << '\n';
        if (vulkan)
          QTest::qWait(180);
        for (auto name : {"RefineRadius", "RefineFeather", "RefineContrast"}) {
          timer.restart();
          const auto beforeStats = canvas->rendererStats();
          set(name, 6);
          const auto published = timer.elapsed();
          if (vulkan)
            CHECK(wait([&] {
              return canvas->rendererStats().framesSubmitted >
                     beforeStats.framesSubmitted;
            }));
          rusage usage{};
          getrusage(RUSAGE_SELF, &usage);
          std::cout << name << " ready_ms=" << published
                    << " presented_ms=" << timer.elapsed()
                    << " peak_rss_KiB=" << usage.ru_maxrss << ' '
                    << panel()
                           ->findChild<QLabel *>("RefineStatus")
                           ->toolTip()
                           .toStdString()
                    << '\n';
          if (vulkan)
            CHECK(canvas->rendererStats().uploadedBytes ==
                  beforeStats.uploadedBytes);
        }
        cancel();
      }
    }
    // Quit sees suspended sessions as well as the active tab. Declining keeps
    // both; accepting closes normally, even with a pending calculation.
    refresh();
    refine->trigger();
    CHECK(ready());
    const auto suspendedOwner = w.activeDocumentId();
    CHECK(w.openImageFromPath(path));
    refresh();
    refine->trigger();
    CHECK(ready());
    panel()->findChild<QDoubleSpinBox *>("RefineRadius")->setValue(250);
    spin();
    const int beforeQuit = refinementPrompts;
    CHECK(!w.close());
    CHECK(refinementPrompts == beforeQuit + 1);
    CHECK(w.documentContext(suspendedOwner)->refinement && panel());
    discardRefinement = true;
    CHECK(w.close());
    CHECK(!w.documentContext(suspendedOwner)->refinement);
    CHECK(!w.documentContext(w.activeDocumentId())->refinement);
    spin();
  } catch (const std::exception &e) {
    CHECK(false);
    std::cerr << e.what() << '\n';
  }
  std::cout << "Refinement workspace: " << (failures ? "FAILED" : "passed")
            << " · " << app.platformName().toStdString() << '\n';
  return failures ? 1 : 0;
}
