#include "imageeditor/core/BasicPixelBrushStroke.hpp"
#include "imageeditor/core/ColorSampler.hpp"
#include "imageeditor/core/FillOperation.hpp"
#include "imageeditor/core/LayerGeometry.hpp"
#include "imageeditor/core/LayerMaskEdit.hpp"
#include "imageeditor/core/LayerStructureCommands.hpp"
#include "imageeditor/core/SelectionCommands.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/CrossWindowPointerRouter.hpp"
#include "imageeditor/ui/FlattenedDocument.hpp"
#include "imageeditor/ui/LayerListModel.hpp"
#include "imageeditor/ui/LayerListView.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/NativeRasterBake.hpp"
#include "imageeditor/ui/PixelPreview.hpp"
#include "imageeditor/ui/PopupOwnership.hpp"
#include "imageeditor/ui/ProjectFile.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/ui/ToolIcons.hpp"
#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QHelpEvent>
#include <QMenu>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolTip>
#include <QVulkanInstance>
#include <iostream>
namespace c = imageeditor::core;
namespace u = imageeditor::ui;
namespace r = imageeditor::render;
int failures = 0;
QVulkanInstance *nativeInstance = nullptr;
#define CHECK(...)                                                             \
  do {                                                                         \
    if (!(__VA_ARGS__)) {                                                      \
      ++failures;                                                              \
      std::cerr << __LINE__ << ": " #__VA_ARGS__ "\n";                         \
    }                                                                          \
  } while (false)
c::LayerMaskState mask(c::Extent2u e, uint8_t value, bool enabled = true) {
  return std::make_shared<c::LayerMask>(
      c::LayerMask{c::SelectionMask::filled(e, value), {}, 255, enabled});
}
c::Layer raster() {
  return c::Layer::raster(
      "Pixels", std::make_shared<c::ContiguousRasterSurface>(
                    c::Extent2u{32, 24}, c::Rgba8{80, 130, 220, 128}));
}
void paint(c::Document &doc, c::LayerId id, c::History &history, c::Rgba8 color,
           bool cancel = false) {
  c::LayerMaskEdit edit(doc, id, "Paint layer mask");
  auto settings = c::proceduralBrushPreset(c::ProceduralBrushPreset::HardRound);
  settings.foreground = color;
  settings.sizePixels = 10;
  settings.opacity = 1;
  settings.flow = 1;
  settings.pressureToSize = false;
  settings.pressureToFlow = false;
  c::RasterEditTransactionOptions options;
  options.coverageValues = true;
  c::BasicPixelBrushStroke stroke(
      edit.proxy(), id, settings, c::BrushCompositeMode::MaskCoverage,
      std::make_unique<c::BasicPixelBrushEngine>(), {}, {}, nullptr, options);
  c::NormalizedPointerSample sample{.documentPosition = {10.5, 10.5},
                                    .pressure = 1,
                                    .buttons = c::PointerButtonPrimary};
  CHECK(stroke.begin(sample));
  if (cancel) {
    stroke.cancel();
    edit.cancel();
    return;
  }
  auto result = stroke.end(sample, edit.provisionalHistory());
  CHECK(result == c::RasterEditCommitResult::Committed ||
        result == c::RasterEditCommitResult::NoChanges);
  result = edit.commit(history);
  CHECK(result == c::RasterEditCommitResult::Committed ||
        result == c::RasterEditCommitResult::NoChanges);
}
void coreContracts() {
  c::Document doc({{32, 24}, 96});
  auto layer = raster();
  const auto id = layer.id;
  const auto source = std::get<c::RasterLayer>(layer.payload).surface;
  const auto sourceRevision = source->revision();
  CHECK(doc.insertLayer(0, layer));
  c::History history;
  const auto original = u::flattenDocument(doc);
  CHECK(original);
  const auto white = mask({32, 24}, 255);
  CHECK(history.execute(doc, std::make_unique<c::LayerMaskCommand>(
                                 id, nullptr, white, "Add mask")));
  CHECK(u::flattenDocument(doc).image == original.image);
  CHECK(history.undo(doc));
  CHECK(!doc.layer(id)->mask);
  CHECK(history.redo(doc));
  const auto gray = mask({32, 24}, 128);
  CHECK(history.execute(doc, std::make_unique<c::LayerMaskCommand>(
                                 id, white, gray, "Gray mask")));
  const auto color = c::sampleDocumentColor(doc, id, {10.5, 10.5},
                                            c::ColorSampleSource::MergedVisible)
                         .color;
  CHECK(color == c::Rgba8{80, 130, 220, 64});
  CHECK(c::sampleDocumentColor(doc, id, {10.5, 10.5},
                               c::ColorSampleSource::ActiveLayer)
            .color == c::Rgba8{80, 130, 220, 128});
  auto black = mask({32, 24}, 0);
  CHECK(history.execute(doc, std::make_unique<c::LayerMaskCommand>(
                                 id, gray, black, "Black mask")));
  CHECK(c::sampleDocumentColor(doc, id, {10.5, 10.5},
                               c::ColorSampleSource::MergedVisible)
            .color.alpha == 0);
  const auto selection =
      c::SelectionMask::rectangle({32, 24}, {8, 8, 6, 6}, 128);
  doc.setSelection(selection);
  paint(doc, id, history, {255, 255, 255, 255});
  const auto painted = doc.layer(id)->mask;
  CHECK(painted->coverage->coverageAtDocumentPixel(10, 10) == 128);
  CHECK(painted->coverage->coverageAtDocumentPixel(6, 10) == 0);
  CHECK(doc.selection() == selection);
  CHECK(source->revision() == sourceRevision);
  CHECK(history.undo(doc));
  CHECK(doc.layer(id)->mask == black);
  CHECK(history.redo(doc));
  CHECK(doc.layer(id)->mask == painted);
  CHECK(history.undo(doc));
  paint(doc, id, history, {255, 255, 255, 255}, true);
  CHECK(doc.layer(id)->mask == black);
  CHECK(history.canRedo());
  paint(doc, id, history, {0, 0, 0, 255});
  CHECK(history.canRedo());
  CHECK(history.redo(doc));
  doc.setSelection({});
  auto disabled = std::make_shared<c::LayerMask>(*painted);
  disabled->enabled = false;
  CHECK(history.execute(doc, std::make_unique<c::LayerMaskCommand>(
                                 id, painted, disabled, "Disable mask")));
  CHECK(u::flattenDocument(doc).image == original.image);
  CHECK(history.undo(doc));
  {
    c::LayerMaskEdit edit(doc, id, "Fill mask");
    c::FillOptions options;
    options.coverageValues = true;
    options.color = {64, 64, 64, 255};
    c::FillOperation fill(edit.proxy(), id, options);
    while (fill.state() == c::FillState::Discovering ||
           fill.state() == c::FillState::Applying)
      fill.step();
    CHECK(fill.commit(edit.provisionalHistory()) ==
          c::RasterEditCommitResult::Committed);
    CHECK(edit.commit(history) == c::RasterEditCommitResult::Committed);
    CHECK(doc.layer(id)->mask->coverage->coverageAtDocumentPixel(10, 10) == 64);
  }
  // Duplicate shares immutable coverage, then painting replaces only the copy.
  const c::LayerSelectionState selected{{id}, id, id};
  auto duplicate = c::duplicateLayerItems(doc, selected);
  CHECK(duplicate);
  CHECK(history.execute(doc, std::move(duplicate)));
  const auto copied = doc.layers().back().id;
  const auto originalMask = doc.layer(id)->mask;
  paint(doc, copied, history, {255, 255, 255, 255});
  CHECK(doc.layer(id)->mask == originalMask);
  CHECK(doc.layer(copied)->mask != originalMask);
  CHECK(source->revision() == sourceRevision);
  // Stale ownership cannot publish into an unrelated changed target.
  {
    c::LayerMaskEdit edit(doc, id, "Stale");
    doc.setLayerMask(id, white);
    CHECK(!edit.valid());
    CHECK(edit.commit(history) == c::RasterEditCommitResult::TargetUnavailable);
    CHECK(doc.layer(id)->mask == white);
  }
}
void persistenceAndBake() {
  c::Document doc({{32, 24}, 144});
  auto r = raster();
  r.mask = mask({32, 24}, 128);
  CHECK(doc.insertLayer(0, r));
  c::TextLayer text;
  text.utf8 = "Mask";
  auto t = c::Layer::text("Text", text);
  t.mask = mask({32, 24}, 0, false);
  CHECK(doc.insertLayer(1, t));
  c::ShapeLayer shape;
  shape.size = {25, 20};
  auto s = c::Layer::shape("Shape", shape);
  s.mask = mask({32, 24}, 99);
  CHECK(doc.insertLayer(2, s));
  s.localToDocument = {1, 0, 0, 0, 1, 0, .001, .002, 1};
  doc.setLayerTransform(s.id, s.localToDocument);
  QTemporaryDir directory;
  const auto path = directory.filePath("mask.vulkana");
  CHECK(u::saveProject(path, doc));
  auto loaded = u::loadProject(path);
  CHECK(loaded);
  if (!loaded) {
    std::cerr << loaded.error.toStdString() << '\n';
    return;
  }
  for (const auto &l : doc.layers())
    CHECK(c::equivalentLayerMasks(l.mask, loaded.document->layer(l.id)->mask));
  CHECK(std::holds_alternative<c::TextLayer>(
      loaded.document->layer(t.id)->payload));
  CHECK(std::holds_alternative<c::ShapeLayer>(
      loaded.document->layer(s.id)->payload));
  const auto before = u::flattenDocument(doc);
  CHECK(before);
  CHECK(u::flattenDocument(*loaded.document).image == before.image);
  c::History history;
  const c::LayerSelectionState selected{{r.id, t.id, s.id}, s.id, s.id};
  auto bake = u::prepareRasterizeLayers(doc, selected, history.memoryBudget(),
                                        {}, s.id);
  CHECK(bake.command);
  CHECK(history.execute(doc, std::move(bake.command)));
  CHECK(!doc.layer(s.id)->mask);
  CHECK(doc.layer(r.id)->mask == r.mask);
  CHECK(doc.layer(t.id)->mask == t.mask);
  CHECK(history.undo(doc));
  CHECK(std::holds_alternative<c::ShapeLayer>(doc.layer(s.id)->payload));
  CHECK(doc.layer(s.id)->mask == s.mask);
  CHECK(history.redo(doc));
  CHECK(u::saveProject(directory.filePath("applied.vulkana"), doc,
                       loaded.metadata));
  auto applied = u::loadProject(directory.filePath("applied.vulkana"));
  CHECK(applied);
  if (applied)
    CHECK(!applied.document->layer(s.id)->mask);
  // The single-raster optimized bake cannot bypass coverage.
  c::Document only({{32, 24}, 96});
  CHECK(only.insertLayer(0, r));
  const std::array ids{r.id};
  const auto flattened = u::flattenLayerItems(only, ids);
  CHECK(flattened);
  CHECK(flattened.image.pixelColor(10, 10).alpha() == 64);
  u::PixelPreview preview;
  preview.setDocumentInstance(1);
  preview.setEnabled(true);
  std::shared_ptr<const c::RasterSurface> pixels;
  preview.onReady = [&](auto p, QString error) {
    CHECK(error.isEmpty());
    pixels = std::move(p);
  };
  preview.request(only.snapshot());
  for (int i = 0; i < 300 && !pixels; ++i)
    QTest::qWait(10);
  CHECK(pixels);
  if (pixels) {
    std::array<std::byte, 4> b;
    pixels->copyRgba8({10, 10, 1, 1}, b, 4);
    CHECK(b[3] == std::byte{64});
  }
  pixels.reset();
  only.setLayerMask(r.id, mask({32, 24}, 0));
  preview.request(only.snapshot());
  for (int i = 0; i < 300 && !pixels; ++i)
    QTest::qWait(10);
  CHECK(pixels);
  if (pixels) {
    std::array<std::byte, 4> b;
    pixels->copyRgba8({10, 10, 1, 1}, b, 4);
    CHECK(b[3] == std::byte{0});
  }
}
void uiContracts() {
  QTemporaryDir directory;
  QImage image(32, 24, QImage::Format_RGBA8888);
  image.fill(QColor(80, 130, 220));
  const auto path = directory.filePath("source.png");
  CHECK(image.save(path));
  u::MainWindow window(nativeInstance, false, false);
  window.setUnsavedPromptEnabled(false);
  window.resize(1400, 850);
  window.show();
  CHECK(window.openImageFromPath(path));
  QTest::qWait(40);
  auto &session = const_cast<c::EditorSession &>(window.editorSession());
  auto *view = dynamic_cast<u::LayerListView *>(
      window.findChild<QListView *>("LayerList"));
  auto *model = dynamic_cast<u::LayerListModel *>(view->model());
  auto *add = window.findChild<QAction *>("AddLayerMaskAction");
  CHECK(add && add->isEnabled());
  const auto plainIndex =
      model->index(model->rowForLayer(*session.activeLayer()));
  const auto plainPreview =
      qvariant_cast<QIcon>(plainIndex.data(Qt::DecorationRole))
          .pixmap(QSize(34, 34), 1.0)
          .toImage();
  CHECK(plainPreview.pixelColor(25, 27) == plainPreview.pixelColor(17, 27));
  const auto noThumbnailTooltip = [&](const QModelIndex& row, bool mask) {
    const auto point=view->thumbnailRect(row,mask).center();
    QHelpEvent hover(QEvent::ToolTip,point,view->viewport()->mapToGlobal(point));
    QApplication::sendEvent(view->viewport(),&hover);
    CHECK(!QToolTip::isVisible());
    QToolTip::hideText();
  };
  noThumbnailTooltip(plainIndex,false);
  CHECK(session.document()->setLayerCrop(*session.activeLayer(),
                                         c::LayerCrop{2, 3, 20, 15}));
  add->trigger();
  CHECK(session.editingLayerMask());
  const auto id = *session.activeLayer();
  CHECK(session.document()->layer(id)->mask);
  CHECK(session.document()->layer(id)->mask->coverage->extent() ==
        c::Extent2u{32, 24});
  const auto index = model->index(model->rowForLayer(id));
  u::CrossWindowPointerRouter *router = nullptr;
  for (auto *object : window.findChildren<QObject *>())
    if (auto *candidate = dynamic_cast<u::CrossWindowPointerRouter *>(object))
      router = candidate;
  CHECK(router);
  for(bool mask:{false,true}) {
    noThumbnailTooltip(index,mask);
    const auto point=view->thumbnailRect(index,mask).center();
    QTest::mousePress(view->viewport(),Qt::LeftButton,{},point);
    // Switching content/mask must not synthesize a release inside this press.
    if(router)CHECK(router->captureDomain()==u::CrossWindowPointerRouter::CaptureDomain::Widget);
    QTest::mouseRelease(view->viewport(),Qt::LeftButton,{},point);
    if(router)CHECK(router->captureDomain()==u::CrossWindowPointerRouter::CaptureDomain::None);
    CHECK(session.editingLayerMask()==mask);
  }
  const auto contextMenu = [&](bool mask) {
    const auto point =
        view->thumbnailRect(model->index(model->rowForLayer(id)), mask)
            .center();
    const auto history = session.history().undoDepth();
    QTest::mousePress(view->viewport(), Qt::RightButton, {}, point);
    if (router)
      CHECK(router->captureDomain() ==
            u::CrossWindowPointerRouter::CaptureDomain::Widget);
    QTest::mouseRelease(view->viewport(), Qt::RightButton, {}, point);
    bool inspected = false;
    QTimer::singleShot(0, &window, [&] {
      auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
      CHECK(menu);
      if (!menu)
        return;
      inspected = true;
      CHECK(u::popupLogicalParent(menu) == view);
      CHECK(menu->parentWidget() == u::popupTopLevelOwner(view));
      CHECK(menu->objectName() ==
            (mask ? "LayerMaskContextMenu" : "LayerContextMenu"));
      QStringList names;
      for (auto *action : menu->actions())
        names << action->objectName();
      if (mask)
        CHECK(names ==
              QStringList({"DeleteLayerMaskAction", "EnableLayerMaskAction",
                           "ApplyLayerMaskAction", "MaskToSelectionAction"}));
      else
        CHECK(names.contains("DuplicateLayersAction") &&
              names.contains("DeleteSelectedLayersAction") &&
              names.contains("ContextNewLayerFolderAction"));
      menu->close();
    });
    // QTest does not synthesize a platform context-menu event from right-click.
    QContextMenuEvent event(QContextMenuEvent::Mouse, point,
                            view->viewport()->mapToGlobal(point));
    QApplication::sendEvent(view->viewport(), &event);
    CHECK(inspected);
    CHECK(session.editingLayerMask() == mask);
    CHECK(session.history().undoDepth() == history);
  };
  // The first right-click immediately after selecting the mask must work.
  contextMenu(true);
  contextMenu(false);
  contextMenu(true);
  r::CanvasWindow *canvas = nullptr;
  for (auto *w : QGuiApplication::allWindows())
    if (w->objectName() == "VulkanCanvasWindow")
      canvas = dynamic_cast<r::CanvasWindow *>(w);
  CHECK(canvas);
  const auto source =
      std::get<c::RasterLayer>(session.document()->layer(id)->payload).surface;
  const auto revision = source->revision();
  const auto white = session.document()->layer(id)->mask;
  session.setActiveTool(c::ToolId::Brush);
  session.setForegroundColor({0, 0, 0, 255});
  c::NormalizedPointerSample point{.documentPosition = {10.5, 10.5},
                                   .pressure = 1,
                                   .buttons = c::PointerButtonPrimary};
  CHECK(canvas->onBrushStrokeBegan(point));
  CHECK(canvas->onBrushStrokeEnded(point));
  CHECK(session.document()->layer(id)->mask != white);
  CHECK(source->revision() == revision);
  CHECK(canvas->scene().document.layersBottomToTop.back().mask ==
        session.document()->layer(id)->mask);
  QTest::qWait(40);
  if (qEnvironmentVariableIsSet("VULKANA_MASK_SCREENSHOT"))
    view->grab().save(qEnvironmentVariable("VULKANA_MASK_SCREENSHOT"));
  for (const auto *name : {"AddLayerButton", "DeleteLayerButton",
                           "NewFolderButton", "AddLayerMaskButton"}) {
    const auto *button = window.findChild<QPushButton *>(name);
    CHECK(button && button->text().isEmpty() && !button->icon().isNull() &&
          button->size() == QSize(32, 30));
  }
  const auto *addButton = window.findChild<QPushButton *>("AddLayerButton");
  const auto *folderButton = window.findChild<QPushButton *>("NewFolderButton");
  const auto *maskButton =
      window.findChild<QPushButton *>("AddLayerMaskButton");
  const auto *deleteButton =
      window.findChild<QPushButton *>("DeleteLayerButton");
  CHECK(folderButton->x() - addButton->geometry().right() == 7);
  CHECK(maskButton->x() - folderButton->geometry().right() == 7);
  CHECK(deleteButton->x() - maskButton->geometry().right() > 20);
  CHECK(deleteButton->parentWidget()->width() -
            deleteButton->geometry().right() ==
        11);
  CHECK(addButton->icon().pixmap(24, 24).toImage() ==
        u::toolGlyph(u::ToolGlyph::NewLayer).pixmap(24, 24).toImage());
  window.findChild<QAction *>("EnableLayerMaskAction")->trigger();
  CHECK(!session.document()->layer(id)->mask->enabled);
  QTest::keyClick(view, Qt::Key_Z, Qt::ControlModifier);
  CHECK(session.document()->layer(id)->mask->enabled);
  window.findChild<QAction *>("DeleteLayerMaskAction")->trigger();
  CHECK(!session.document()->layer(id)->mask);
  QTest::keyClick(view, Qt::Key_Z, Qt::ControlModifier);
  CHECK(session.document()->layer(id)->mask);
  const auto retainedMask = session.document()->layer(id)->mask;
  CHECK(!session.document()->selection());
  window.findChild<QAction *>("MaskToSelectionAction")->trigger();
  CHECK(session.document()->selection());
  if (session.document()->selection())
    CHECK(session.document()->selection()->coverageAtDocumentPixel(10, 10) ==
          retainedMask->coverage->coverageAtDocumentPixel(10, 10));
  CHECK(session.document()->layer(id)->mask == retainedMask);
  QTest::keyClick(view, Qt::Key_Z, Qt::ControlModifier);
  CHECK(!session.document()->selection());
  window.findChild<QAction *>("NewLayerFolderAction")->trigger();
  CHECK(!add->isEnabled());
  // Typed mask painting remains a bitmap edit without replacing the shape
  // model.
  c::ShapeLayer shape;
  shape.size = {24, 20};
  auto typed = c::Layer::shape("Editable shape", shape);
  const auto typedId = typed.id;
  CHECK(session.document()->insertLayer(session.document()->layers().size(),
                                        typed));
  model->refresh();
  view->onRowSelectionRequested(model->rowForLayer(typedId), Qt::NoModifier);
  const auto selection =
      c::SelectionMask::rectangle({32, 24}, {5, 5, 12, 12}, 128);
  session.document()->setSelection(selection);
  // Refresh action eligibility without changing the selection.
  view->onRowSelectionRequested(model->rowForLayer(typedId), Qt::NoModifier);
  auto *fromSelection = window.findChild<QAction *>("MaskFromSelectionAction");
  CHECK(fromSelection->isEnabled());
  fromSelection->trigger();
  CHECK(session.editingLayerMask());
  CHECK(session.document()->layer(typedId)->mask);
  if (!session.document()->layer(typedId)->mask)
    return;
  CHECK(session.document()
            ->layer(typedId)
            ->mask->coverage->coverageAtDocumentPixel(10, 10) == 128);
  CHECK(canvas->onBrushStrokeBegan(point));
  CHECK(canvas->onBrushStrokeEnded(point));
  CHECK(std::get<c::ShapeLayer>(session.document()->layer(typedId)->payload) ==
        shape);
  CHECK(session.document()->selection() == selection);
  const auto owner = window.activeDocumentId();
  const auto committed = session.document()->layer(typedId)->mask;
  session.setForegroundColor({255, 255, 255, 255});
  CHECK(canvas->onBrushStrokeBegan(point));
  CHECK(window.openImageFromPath(path));
  CHECK(window.activeDocumentId() != owner);
  CHECK(session.document()->layer(typedId)->mask == committed);
  CHECK(!window.editorSession().editingLayerMask());
  CHECK(window.activateDocument(owner));
  CHECK(window.editorSession().editingLayerMask());
  CHECK(session.document()->layer(typedId)->mask == committed);
  // Hidden text has no display cache; creating a mask still uses its model.
  c::TextLayer text;
  text.utf8 = "Editable text";
  auto textLayer = c::Layer::text("Hidden text", text);
  textLayer.visible = false;
  const auto textId = textLayer.id;
  CHECK(session.document()->insertLayer(session.document()->layers().size(),
                                        textLayer));
  model->refresh();
  view->onRowSelectionRequested(model->rowForLayer(textId), Qt::NoModifier);
  add->trigger();
  CHECK(session.document()->layer(textId)->mask);
  CHECK(
      std::get<c::TextLayer>(session.document()->layer(textId)->payload).utf8 ==
      text.utf8);
  window.close();
}
int main(int argc, char **argv) {
  QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
  QApplication app(argc, argv);
  QStandardPaths::setTestModeEnabled(true);
  QTemporaryDir settings;
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                     settings.path());
  u::applyEditorTheme(app);
  QVulkanInstance instance;
  if (app.arguments().contains("--native")) {
    instance.setApiVersion(QVersionNumber(1, 2));
    instance.setLayers({"VK_LAYER_KHRONOS_validation"});
    CHECK(instance.create());
    if (!instance.isValid())
      return 1;
    nativeInstance = &instance;
    instance.installDebugOutputFilter(
        [](QVulkanInstance::DebugMessageSeverityFlags severity,
           QVulkanInstance::DebugMessageTypeFlags type, const void *data) {
          if (type.testFlag(QVulkanInstance::ValidationMessage) &&
              (severity.testFlag(QVulkanInstance::WarningSeverity) ||
               severity.testFlag(QVulkanInstance::ErrorSeverity))) {
            ++failures;
            std::cerr
                << static_cast<const VkDebugUtilsMessengerCallbackDataEXT *>(
                       data)
                       ->pMessage
                << '\n';
          }
          return false;
        });
  }
  try {
    coreContracts();
    persistenceAndBake();
    uiContracts();
  } catch (const std::exception &e) {
    CHECK(false);
    std::cerr << e.what() << '\n';
  }
  std::cout << "Layer masks: " << (failures ? "FAILED" : "passed")
            << "; platform=" << app.platformName().toStdString()
            << "; Vulkan validation="
            << (nativeInstance ? "enabled" : "separate GPU test") << '\n';
  return failures ? 1 : 0;
}
