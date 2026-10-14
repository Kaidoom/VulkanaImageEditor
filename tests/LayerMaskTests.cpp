#include "imageeditor/core/BasicPixelBrushStroke.hpp"
#include "imageeditor/core/ColorSampler.hpp"
#include "imageeditor/core/FillOperation.hpp"
#include "imageeditor/core/LayerGeometry.hpp"
#include "imageeditor/core/LayerCommands.hpp"
#include "imageeditor/core/LayerMaskEdit.hpp"
#include "imageeditor/core/LayerStructureCommands.hpp"
#include "imageeditor/core/SelectionCommands.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/CrossWindowPointerRouter.hpp"
#include "imageeditor/ui/FlattenedDocument.hpp"
#include "imageeditor/ui/LayerListModel.hpp"
#include "imageeditor/ui/LayerListView.hpp"
#include "imageeditor/ui/LayerMaskClipboard.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/NativeRasterBake.hpp"
#include "imageeditor/ui/PixelPreview.hpp"
#include "imageeditor/ui/PopupOwnership.hpp"
#include "imageeditor/ui/ProjectFile.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/ui/ToolIcons.hpp"
#include "imageeditor/ui/AdjustmentsPanel.hpp"
#include "imageeditor/ui/EffectsPanel.hpp"
#include "imageeditor/ui/OverlayDockWorkspace.hpp"
#include "imageeditor/ui/WorkspacePanel.hpp"
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QMimeData>
#include <QDataStream>
#include <QContextMenuEvent>
#include <QComboBox>
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
#include <limits>
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
void checkMaskFrame(const c::LayerMaskState& state, c::RectD bounds) {
  const auto e=state->coverage->extent();
  for(const auto p:std::array{c::Vec2d{bounds.x,bounds.y},c::Vec2d{bounds.right(),bounds.y},
                             c::Vec2d{bounds.x,bounds.bottom()},c::Vec2d{bounds.right(),bounds.bottom()}}) {
    const auto q=state->localToMask.map(p);
    CHECK(q.x>=-1.e-8&&q.y>=-1.e-8&&q.x<=e.width+1.e-8&&q.y<=e.height+1.e-8);
  }
}
void paint(c::Document&,c::LayerId,c::History&,c::Rgba8,bool cancel=false,c::Vec2d point={10.5,10.5});
void clipboardMappingContracts() {
  std::vector<uint8_t> values(8*6);
  for(size_t i=0;i<values.size();++i)values[i]=uint8_t((i*37)%256);
  auto original=std::make_shared<c::LayerMask>();
  original->coverage=c::SelectionMask::fromR8({8,6},values,8);
  original->outside=73;original->localToMask={1.25,0,3.5,0,.8,-2.25};
  const std::array<c::AffineTransform,4> transforms{
    c::AffineTransform{}, {1,0,15.35,0,1,-9.25}, {-.8,.6,20,.6,.8,10},
    {1.1,.1,12,-.2,.9,7,.001,-.0005,1}};
  for(const auto& sourceTransform:transforms) {
    auto source=raster();source.localToDocument=sourceTransform;source.mask=original;
    const auto bytes=u::encodeLayerMaskClipboard(source);
    const auto copied=u::decodeLayerMaskClipboard(bytes);CHECK(copied);if(!copied)return;
    CHECK(c::equivalentLayerMasks(copied->mask,source.mask));
    CHECK(copied->sourceToDocument==sourceTransform);
    for(const auto& destination:transforms) {
      const auto pasted=u::pastedLayerMask(*copied,destination);CHECK(pasted);if(!pasted)return;
      CHECK(pasted!=copied->mask&&pasted->coverage==copied->mask->coverage);
      CHECK(pasted->outside==73&&pasted->enabled);
      for(int y=-15;y<45;++y)for(int x=-20;x<50;++x) {
        const c::Vec2d point{x+.37,y+.61};
        CHECK(std::abs(c::layerMaskCoverage(source.mask,sourceTransform.inverted()->map(point))
          -c::layerMaskCoverage(pasted,destination.inverted()->map(point)))<1.e-5F);
      }
      if(destination==sourceTransform)CHECK(c::equivalentLayerMasks(pasted,source.mask));
      const c::RectD frame{-7.25,-3.5,49.75,34.25};
      const auto expanded=u::pastedLayerMask(*copied,destination,frame);CHECK(expanded);
      if(!expanded)return;
      checkMaskFrame(expanded,frame);
      // Padding preserves the original bitmap byte-for-byte, including its
      // interpolation at the old outer edge and arbitrary outside coverage.
      const auto toExpanded=c::composeTransform(expanded->localToMask,*pasted->localToMask.inverted());
      for(int y=0;y<6;++y)for(int x=0;x<8;++x) {
        const auto p=toExpanded.map({x+.5,y+.5});
        CHECK(expanded->coverage->coverageAtDocumentPixel(int(std::floor(p.x)),int(std::floor(p.y)))==values[size_t(y*8+x)]);
      }
      for(int y=-15;y<45;++y)for(int x=-20;x<50;++x)
        CHECK(std::abs(c::layerMaskCoverage(pasted,{x+.37,y+.61})
          -c::layerMaskCoverage(expanded,{x+.37,y+.61}))<1.e-5F);
      CHECK(c::expandedLayerMask(expanded,frame)==expanded);
    }
    CHECK(!u::pastedLayerMask(*copied,{0,0,0,0,0,0}));
    for(auto damaged:{QByteArray{},bytes.first(bytes.size()-1),bytes+QByteArray("x")})
      CHECK(!u::decodeLayerMaskClipboard(damaged));
    for(int index:{0,4,8,17}) {auto damaged=bytes;damaged[index]=char(255);CHECK(!u::decodeLayerMaskClipboard(damaged));}
    auto damaged=bytes;
    QDataStream stream(&damaged,QIODevice::ReadWrite);stream.setByteOrder(QDataStream::LittleEndian);
    stream.device()->seek(18);stream<<std::numeric_limits<double>::quiet_NaN();
    CHECK(!u::decodeLayerMaskClipboard(damaged));
  }
  original->enabled=false;
  auto source=raster();source.mask=original;
  const auto copy=u::decodeLayerMaskClipboard(u::encodeLayerMaskClipboard(source));
  CHECK(copy&&!copy->mask->enabled);
  // A stale second target must not leave the first target half-updated.
  c::Document doc({{32,24},96});auto first=raster(),second=raster();
  CHECK(doc.insertLayer(0,first)&&doc.insertLayer(1,second));
  c::History history;
  c::LayerSelectionState selected{{first.id,second.id},second.id,first.id};
  const auto state=doc.contentState();
  CHECK(!history.execute(doc,std::make_unique<c::PasteLayerMasksCommand>(
    std::vector<c::LayerMaskReplacement>{{first.id,nullptr,original},{second.id,original,original->enabled?original:mask({8,6},10)}},selected,false)));
  CHECK(!doc.layer(first.id)->mask&&!doc.layer(second.id)->mask&&doc.contentState()==state);
}

void clipboardWorkflow(const QString& project={}) {
  if(QGuiApplication::platformName().startsWith("wayland")) {
    // QTest events cannot supply a compositor input serial for clipboard
    // ownership. Exercise this workflow on offscreen/XCB/Windows; the remaining
    // mask editing/rendering checks still run in native Wayland validation.
    std::cout<<"SKIP synthetic Wayland clipboard workflow (requires real input serial).\n";
    return;
  }
  QTemporaryDir temp;
  QString input=project;
  if(input.isEmpty()) {
    c::Document doc({{96,80},96});
    c::TextLayer text;text.utf8="Hello";
    auto source=c::Layer::text("Text",text);
    source.localToDocument={1.2,.1,17,-.1,.9,8};
    auto coverage=std::make_shared<c::LayerMask>();
    coverage->coverage=c::SelectionMask::rectangle({40,30},{5,7,17,12},128);
    coverage->localToMask={1,0,2,0,1,3};coverage->outside=255;source.mask=coverage;
    auto first=raster();first.name="Layer 3";first.localToDocument={.7,-.2,-6,.3,.8,15};
    auto second=raster();second.name="Layer 4";second.localToDocument={-1,0,90,0,1,-12};
    for(auto* target:{&first,&second})std::get<c::RasterLayer>(target->payload).surface=
      std::make_shared<c::ContiguousRasterSurface>(c::Extent2u{96,80},c::Rgba8{80,130,220,128});
    CHECK(doc.insertLayer(0,first)&&doc.insertLayer(1,second)&&doc.insertLayer(2,source));
    input=temp.filePath("clipboard-source.vulkana");CHECK(u::saveProject(input,doc));
  }
  u::MainWindow window(nativeInstance,false,false);window.setUnsavedPromptEnabled(false);
  window.resize(1400,850);window.show();CHECK(window.openImageFromPath(input));QTest::qWait(30);
  auto& session=const_cast<c::EditorSession&>(window.editorSession());
  const auto instance=window.activeDocumentId();auto& doc=*session.document();
  const auto find=[&](const char* name){for(const auto& layer:doc.layers())if(layer.name==name)return layer.id;return c::LayerId{};};
  const auto sourceId=find("Text"),first=find("Layer 3"),second=find("Layer 4");
  CHECK(sourceId&&first&&second);if(!sourceId||!first||!second)return;
  auto* view=dynamic_cast<u::LayerListView*>(window.findChild<QListView*>("LayerList"));
  auto* model=dynamic_cast<u::LayerListModel*>(view->model());
  auto* copy=window.findChild<QAction*>("CopyLayerMaskAction");
  auto* paste=window.findChild<QAction*>("PasteLayerMaskAction");CHECK(copy&&paste);
  const auto choose=[&](c::LayerId id,Qt::KeyboardModifiers modifiers=Qt::NoModifier){view->onRowSelectionRequested(model->rowForLayer(id),modifiers);};
  // Match the actual context-menu path, not only the action callback.
  const auto menuAction=[&](c::LayerId id,bool thumbnail,const char* name,const std::function<void()>& before={}) {
    const auto point=view->thumbnailRect(model->index(model->rowForLayer(id)),thumbnail).center();
    bool triggered=false;
    QTimer::singleShot(0,&window,[&]{
      auto* menu=qobject_cast<QMenu*>(QApplication::activePopupWidget());CHECK(menu);if(!menu)return;
      for(auto* action:menu->actions())if(action->objectName()==name) {
        CHECK(action->isEnabled());if(before)before();action->trigger();triggered=true;break;
      }
      menu->close();
    });
    QContextMenuEvent event(QContextMenuEvent::Mouse,point,view->viewport()->mapToGlobal(point));
    QApplication::sendEvent(view->viewport(),&event);CHECK(triggered);
    QTest::qWait(30); // Native clipboard-change notifications can be queued.
  };
  QApplication::clipboard()->setText("Unrelated clipboard data");choose(sourceId);CHECK(!paste->isEnabled());
  const auto sourceMask=doc.layer(sourceId)->mask;CHECK(sourceMask);if(!sourceMask)return;
  const auto sourceTransform=doc.layer(sourceId)->localToDocument;
  const auto review=project.isEmpty()?QString{}:qEnvironmentVariable("VULKANA_TEST_MASK_CLIPBOARD_REVIEW");
  if(!review.isEmpty()){QDir().mkpath(review);CHECK(u::flattenDocument(doc).image.save(review+"/before.png"));}
  doc.markSaved();const auto state=doc.contentState(),depth=session.history().undoDepth();
  menuAction(sourceId,true,"CopyLayerMaskAction");
  const auto* clipboard=QApplication::clipboard()->mimeData();
  CHECK(clipboard&&u::decodeLayerMaskClipboard(clipboard->data(u::layerMaskClipboardMime)));
  CHECK(paste->isEnabled()&&!doc.isModified()&&doc.contentState()==state&&session.history().undoDepth()==depth);
  if(!paste->isEnabled())return;
  const auto priorFirst=doc.layer(first)->mask;
  const auto priorSecond=mask({5,7},32);CHECK(doc.setLayerMask(second,priorSecond));
  const auto pixels=std::get<c::RasterLayer>(doc.layer(second)->payload).surface;
  const auto revision=pixels->revision();const auto targetTransform=doc.layer(second)->localToDocument;
  const auto selection=c::SelectionMask::rectangle(doc.canvas().extent,{2,3,7,8});doc.setSelection(selection);
  // Later source edits do not change the clipboard snapshot.
  CHECK(doc.setLayerMask(sourceId,mask({8,8},255)));
  choose(first);choose(second,Qt::ControlModifier);
  const auto targets=session.layerSelectionState();CHECK(targets.ids.size()==2);
  menuAction(second,false,"PasteLayerMaskAction");
  const auto afterFirst=doc.layer(first)->mask,afterSecond=doc.layer(second)->mask;
  if(!review.isEmpty()) {
    const auto changedSource=doc.layer(sourceId)->mask;
    CHECK(doc.setLayerMask(sourceId,sourceMask));
    CHECK(u::flattenDocument(doc).image.save(review+"/after.png"));
    CHECK(doc.setLayerMask(sourceId,changedSource));
  }
  CHECK(afterFirst&&afterSecond&&afterFirst!=afterSecond&&session.editingLayerMask());
  if(!afterFirst||!afterSecond)return;
  CHECK(session.history().undoDepth()==depth+1&&session.layerSelectionState()==targets);
  CHECK(doc.selection()==selection&&pixels->revision()==revision&&doc.layer(second)->localToDocument==targetTransform);
  CHECK(std::holds_alternative<c::TextLayer>(doc.layer(sourceId)->payload));
  for(auto id:{first,second}) {
    const auto* layer=doc.layer(id);
    checkMaskFrame(layer->mask,c::layerMaskEditingBounds(*layer,doc.canvas().extent));
    const auto inverse=layer->localToDocument.inverted();CHECK(inverse);
    for(int y=-5;y<int(doc.canvas().extent.height)+5;y+=3)for(int x=-5;x<int(doc.canvas().extent.width)+5;x+=3) {
      const c::Vec2d point{x+.5,y+.5};
      CHECK(std::abs(c::layerMaskCoverage(sourceMask,sourceTransform.inverted()->map(point))
        -c::layerMaskCoverage(layer->mask,inverse->map(point)))<1.e-5F);
    }
    // Paint a genuine stroke outside the old text bitmap, on both newly
    // pasted and previously saved small-frame masks. Optional project fixtures
    // follow this same path without changing their on-disk files.
    const auto raw=u::pastedLayerMask({sourceMask,sourceTransform},layer->localToDocument);
    const auto bounds=c::layerMaskEditingBounds(*layer,doc.canvas().extent);
    const auto rawExtent=raw->coverage->extent();
    std::optional<c::Vec2d> point;
    for(int y=0;y<9&&!point;++y)for(int x=0;x<9&&!point;++x) {
      const c::Vec2d local{bounds.x+bounds.width*(x+1)/10,bounds.y+bounds.height*(y+1)/10};
      const auto m=raw->localToMask.map(local);
      const auto d=layer->localToDocument.map(local);
      if(d.x>6&&d.y>6&&d.x<doc.canvas().extent.width-6&&d.y<doc.canvas().extent.height-6
          &&(m.x < -10||m.y < -10||m.x > rawExtent.width+10||m.y > rawExtent.height+10))point=local;
    }
    CHECK(point);
    if(point) {
      const auto pasted=layer->mask;
      const auto documentPoint=layer->localToDocument.map(*point);
      const uint8_t value=raw->outside==255?0:255;
      doc.setSelection({});
      for(const auto& inputMask:{pasted,raw}) {
        if(doc.layer(id)->mask!=inputMask)CHECK(doc.setLayerMask(id,inputMask));
        c::History brushHistory;
        paint(doc,id,brushHistory,{value,value,value,255},false,documentPoint);
        if(std::abs(c::layerMaskCoverage(doc.layer(id)->mask,*point)-float(value)/255)>=.02F)
          std::cerr<<"Expanded-mask paint: "<<layer->name<<" at "<<point->x<<','<<point->y
            <<" doc "<<documentPoint.x<<','<<documentPoint.y<<" got "<<c::layerMaskCoverage(doc.layer(id)->mask,*point)
            <<" expected "<<int(value)<<" undo="<<brushHistory.undoDepth()<<'\n';
        CHECK(std::abs(c::layerMaskCoverage(doc.layer(id)->mask,*point)-float(value)/255)<.02F);
        CHECK(brushHistory.undoDepth()==1&&brushHistory.undo(doc));
        CHECK(doc.layer(id)->mask==inputMask);
      }
      CHECK(doc.setLayerMask(id,pasted));doc.setSelection(selection);
    }
  }
  QTest::keyClick(view,Qt::Key_Z,Qt::ControlModifier);
  CHECK(doc.layer(first)->mask==priorFirst&&doc.layer(second)->mask==priorSecond&&!session.editingLayerMask());
  CHECK(doc.selection()==selection&&session.layerSelectionState()==targets);
  QTest::keyClick(view,Qt::Key_Z,Qt::ControlModifier|Qt::ShiftModifier);
  CHECK(doc.layer(first)->mask==afterFirst&&doc.layer(second)->mask==afterSecond&&session.editingLayerMask());
  // An identical paste is not a content edit and cannot consume redo.
  CHECK(session.execute(std::make_unique<c::SetLayerOpacityCommand>(second,.25F)));CHECK(session.undo());
  doc.markSaved();const auto savedState=doc.contentState();
  paste->trigger();CHECK(session.history().canRedo()&&!doc.isModified()&&doc.contentState()==savedState);
  CHECK(doc.layer(first)->mask==afterFirst&&doc.layer(second)->mask==afterSecond);
  const auto output=temp.filePath("pasted.vulkana");CHECK(u::saveProject(output,doc));
  auto reopened=u::loadProject(output);CHECK(reopened);if(reopened)for(auto id:{first,second})
    CHECK(c::equivalentLayerMasks(reopened.document->layer(id)->mask,doc.layer(id)->mask));
  QImage other(64,48,QImage::Format_RGBA8888);other.fill(Qt::white);
  const auto otherPath=temp.filePath("other.png");CHECK(other.save(otherPath));CHECK(window.openImageFromPath(otherPath));
  const auto otherInstance=window.activeDocumentId();const auto otherId=*window.editorSession().activeLayer();
  // Keep a copy of the selection-bound mask independent of the source tab's lifetime.
  CHECK(window.activateDocument(instance));choose(second);
  const auto oldOtherMask=window.documentContext(otherInstance)->session.document()->layer(otherId)->mask;
  menuAction(second,false,"PasteLayerMaskAction",[&]{CHECK(window.activateDocument(otherInstance));});
  CHECK(window.editorSession().document()->layer(otherId)->mask==oldOtherMask);
  CHECK(window.closeDocument(instance));
  paste->trigger();
  const auto* otherLayer=window.editorSession().document()->layer(otherId);CHECK(otherLayer&&otherLayer->mask);
  if(otherLayer&&otherLayer->mask) {
    checkMaskFrame(otherLayer->mask,c::layerMaskEditingBounds(*otherLayer,{64,48}));
    for(int y=-10;y<70;++y)for(int x=-10;x<80;++x) {
      const c::Vec2d p{x+.5,y+.5};
      CHECK(std::abs(c::layerMaskCoverage(otherLayer->mask,p)
        -c::layerMaskCoverage(sourceMask,sourceTransform.inverted()->map(p)))<1.e-5F);
    }
  }
  QApplication::clipboard()->setText("Clipboard replaced");QTest::qWait(30);CHECK(!paste->isEnabled());
  window.close();
}
void paint(c::Document &doc, c::LayerId id, c::History &history, c::Rgba8 color,
           bool cancel, c::Vec2d point) {
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
  c::NormalizedPointerSample sample{.documentPosition = point,
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
void expandedEditingContracts() {
  // Existing small-frame masks must be editable outside their stored support.
  // Keep raster origins, crops and transforms intact.
  for(uint8_t outside:{uint8_t(0),uint8_t(73),uint8_t(255)}) {
    c::Document doc({{96,80},96});
    auto layer=raster();layer.rasterOrigin={-8,-6};
    layer.localToDocument={-.9,.15,47,.2,1.1,19,.0002,.0001,1};
    layer.crop=c::LayerCrop{0,0,8,8};
    auto small=std::make_shared<c::LayerMask>();
    small->coverage=c::SelectionMask::filled({4,3},128);
    small->localToMask={1,0,-1,0,1,-2};small->outside=outside;
    layer.mask=small;CHECK(doc.insertLayer(0,layer));
    const auto source=std::get<c::RasterLayer>(layer.payload).surface;
    const auto sourceRevision=source->revision();
    const c::Vec2d local{16.5,12.5};const auto point=layer.localToDocument.map(local);
    c::History history;
    CHECK(history.execute(doc,std::make_unique<c::SetLayerOpacityCommand>(layer.id,.5F)));
    CHECK(history.undo(doc));doc.markSaved();const auto saved=doc.contentState();
    {
      c::LayerMaskEdit edit(doc,layer.id,"No mask edit");
      CHECK(doc.layer(layer.id)->mask==small&&doc.contentState()==saved);
      CHECK(edit.commit(history)==c::RasterEditCommitResult::NoChanges);
    }
    paint(doc,layer.id,history,{outside,outside,outside,255},false,point);
    CHECK(doc.layer(layer.id)->mask==small&&history.canRedo()&&!doc.isModified());
    paint(doc,layer.id,history,{255,255,255,255},true,point);
    CHECK(doc.layer(layer.id)->mask==small&&history.canRedo()&&doc.contentState()==saved);
    const uint8_t value=outside==255?0:255;
    paint(doc,layer.id,history,{value,value,value,255},false,point);
    const auto edited=doc.layer(layer.id)->mask;
    CHECK(edited!=small&&history.undoDepth()==1&&!history.canRedo());
    checkMaskFrame(edited,c::layerMaskEditingBounds(layer,doc.canvas().extent));
    CHECK(std::abs(c::layerMaskCoverage(edited,local)-float(value)/255)<.01F);
    CHECK(std::abs(c::layerMaskCoverage(edited,{2.5,3.5})-128.F/255)<1.e-5F);
    CHECK(edited->outside==outside&&source->revision()==sourceRevision);
    CHECK(history.undo(doc)&&doc.layer(layer.id)->mask==small);
    CHECK(!doc.isModified());
    CHECK(history.redo(doc)&&doc.layer(layer.id)->mask==edited);
    CHECK(history.undo(doc));
    // Fill uses the same expanded proxy, including default coverage that was
    // previously unstored. It remains one atomic history step.
    c::LayerMaskEdit edit(doc,layer.id,"Fill expanded mask");
    c::FillOptions options;options.coverageValues=true;options.color={42,42,42,255};
    options.mode=c::FillMode::SelectionOrLayer;
    c::FillOperation fill(edit.proxy(),layer.id,options);
    while(fill.state()==c::FillState::Discovering||fill.state()==c::FillState::Applying)fill.step();
    CHECK(fill.commit(edit.provisionalHistory())==c::RasterEditCommitResult::Committed);
    CHECK(edit.commit(history)==c::RasterEditCommitResult::Committed);
    CHECK(std::abs(c::layerMaskCoverage(doc.layer(layer.id)->mask,local)-42.F/255)<1.e-5F);
    CHECK(history.undo(doc)&&doc.layer(layer.id)->mask==small);
    // A provisional edit that returns to its initial expanded coverage must
    // not store padding or consume the restored redo entry.
    {
      c::LayerMaskEdit roundTrip(doc,layer.id,"No net mask change");
      const auto proxyLayer=roundTrip.proxy().layer(layer.id);
      const auto surface=std::get<c::RasterLayer>(proxyLayer->payload).surface;
      const auto pixel=proxyLayer->localToDocument.inverted()->map(point);
      const c::RectI area{int(std::floor(pixel.x)),int(std::floor(pixel.y)),1,1};
      std::array<std::byte,4> original;
      surface->copyRgba8(area,original,4);
      const std::array<std::byte,4> changed{std::byte{19},std::byte{19},std::byte{19},std::byte{255}};
      const c::RasterPatch change{area,changed,4},restore{area,original,4};
      surface->replaceRgba8Batch(std::span(&change,1));
      surface->replaceRgba8Batch(std::span(&restore,1));
      CHECK(roundTrip.commit(history)==c::RasterEditCommitResult::NoChanges);
      CHECK(doc.layer(layer.id)->mask==small&&history.canRedo());
    }
  }
  // Oversize/projective frames fail before mutating a document or allocating
  // a huge pixel plane.
  const auto small=mask({4,3},0);
  for(const auto bounds:{c::RectD{0,0,40000,4},c::RectD{0,0,10000,10000},
                          c::RectD{0,0,std::numeric_limits<double>::infinity(),4}}) {
    bool rejected=false;
    try{(void)c::expandedLayerMask(small,bounds);}catch(const std::exception&){rejected=true;}
    CHECK(rejected);
  }
  auto horizon=std::make_shared<c::LayerMask>(*small);
  horizon->localToMask={1,0,0,0,1,0,.1,0,1};CHECK(c::validLayerMask(horizon));
  bool rejected=false;
  try{(void)c::expandedLayerMask(horizon,{-20,0,25,4});}catch(const std::exception&){rejected=true;}
  CHECK(rejected);
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
  // Both default entry points capture selection coverage, without consuming
  // the selection or changing source pixels. An absent/empty selection is white.
  auto *createMaskButton = window.findChild<QPushButton *>("AddLayerMaskButton");
  CHECK(createMaskButton && createMaskButton->toolTip() == "Add layer mask");
  const auto rasterId = *session.activeLayer();
  const auto rasterSource =
      std::get<c::RasterLayer>(session.document()->layer(rasterId)->payload).surface;
  const auto rasterRevision = rasterSource->revision();
  std::vector<uint8_t> softCoverage(32 * 24);
  for (int y = 5; y < 17; ++y)
    for (int x = 7; x < 21; ++x)
      softCoverage[size_t(y * 32 + x)] = x < 10 ? 128 : 255;
  softCoverage[10 * 32 + 15] = 0; // A hole remains hidden, not filled by bounds.
  const std::array<c::SelectionState, 4> selections{
      nullptr, c::SelectionMask::filled({32, 24}, 0),
      c::SelectionMask::rectangle({32, 24}, {7, 5, 14, 12}),
      c::SelectionMask::fromR8({32, 24}, softCoverage, 32)};
  for (size_t i = 0; i < selections.size(); ++i) {
    const int dx = i == 3 ? 4 : 0, dy = i == 3 ? 3 : 0;
    session.document()->setLayerTransform(rasterId, {1, 0, double(dx), 0, 1, double(dy)});
    session.document()->setSelection(selections[i]);
    view->onRowSelectionRequested(model->rowForLayer(rasterId), Qt::NoModifier);
    const auto depth = session.history().undoDepth();
    CHECK(add->isEnabled() && createMaskButton->isEnabled());
    if (i % 2)
      createMaskButton->click();
    else
      add->trigger();
    const auto created = session.document()->layer(rasterId)->mask;
    CHECK(created && session.editingLayerMask());
    if (!created)
      return;
    const bool selected = selections[i] && !selections[i]->bounds().empty();
    CHECK(created->outside == (selected ? 0 : 255));
    CHECK(created->coverage->extent() == c::Extent2u{32, 24});
    for (int y = 0; y < 24; ++y)
      for (int x = 0; x < 32; ++x)
        CHECK(created->coverage->coverageAtDocumentPixel(x, y) ==
              (selected ? selections[i]->coverageAtDocumentPixel(x + dx, y + dy) : 255));
    CHECK(session.document()->selection() == selections[i]);
    CHECK(rasterSource->revision() == rasterRevision);
    CHECK(session.history().undoDepth() == depth + 1);
    QTest::keyClick(view, Qt::Key_Z, Qt::ControlModifier);
    CHECK(!session.document()->layer(rasterId)->mask);
    CHECK(session.document()->selection() == selections[i]);
    CHECK(session.history().undoDepth() == depth);
    QTest::keyClick(view, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
    CHECK(session.document()->layer(rasterId)->mask == created);
    CHECK(session.document()->selection() == selections[i]);
    QTest::keyClick(view, Qt::Key_Z, Qt::ControlModifier);
    CHECK(!session.document()->layer(rasterId)->mask);
  }
  session.document()->setLayerTransform(rasterId, {});
  session.document()->setSelection({});
  view->onRowSelectionRequested(model->rowForLayer(rasterId), Qt::NoModifier);
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
                           "ApplyLayerMaskAction", "MaskToSelectionAction", "InvertLayerMaskAction", "CopyLayerMaskAction", "PasteLayerMaskAction", "RefineMaskAction"}));
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
  CHECK(folderButton->x() - addButton->geometry().right() == 5);
  CHECK(maskButton->x() - folderButton->geometry().right() == 5);
  CHECK(deleteButton->x() - maskButton->geometry().right() > 20);
  CHECK(deleteButton->parentWidget()->width() -
            deleteButton->geometry().right() ==
        7);
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
  CHECK(!window.findChild<QPushButton *>("LayerEffectsButton")->isEnabled());
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
  // The fx menu is navigation, not an enable/edit action. It also reveals a
  // hidden/inactive Adjustments tab inside a grouped workspace panel.
  u::OverlayDockWorkspace* workspace = nullptr;
  for (auto* widget : window.findChildren<QWidget*>())
    if (auto* candidate = dynamic_cast<u::OverlayDockWorkspace*>(widget)) workspace = candidate;
  CHECK(workspace);
  auto* adjustments = dynamic_cast<u::WorkspacePanel*>(window.findChild<QWidget*>("AdjustmentsPanelShell"));
  auto* properties = dynamic_cast<u::WorkspacePanel*>(window.findChild<QWidget*>("PropertiesPanelShell"));
  CHECK(adjustments && properties);
  if (workspace && adjustments && properties) {
    workspace->tabifyPanel(adjustments, properties);
    workspace->activatePanel(properties);
    window.findChild<QAction*>("AdjustmentsPanelVisibilityAction")->trigger();
    CHECK(!workspace->panelVisible(adjustments));
    const auto effectsBefore = session.document()->layer(textId)->effects;
    const auto selectionBefore = session.document()->selection();
    const auto depth = session.history().undoDepth();
    auto* fx = window.findChild<QPushButton*>("LayerEffectsButton");
    CHECK(fx && fx->isEnabled() && !fx->icon().isNull() && fx->text().isEmpty());
    CHECK(fx->x() - maskButton->geometry().right() == 5);
    fx->click();
    auto* menu = window.findChild<QMenu*>("LayerEffectsMenu");
    CHECK(menu && menu->actions().size() == int(c::layerEffectCount));
    if (menu) {
      CHECK(menu->parentWidget() == u::popupTopLevelOwner(fx));
      auto* panel = dynamic_cast<u::AdjustmentsPanel*>(adjustments->contentWidget());
      for (auto* action : menu->actions()) {
        action->trigger();
        CHECK(panel->effectsCategoryActive());
        CHECK(panel->findChild<QComboBox*>("EffectNavigation")->currentIndex() == action->data().toInt());
        CHECK(session.document()->layer(textId)->effects == effectsBefore);
        CHECK(session.history().undoDepth() == depth);
      }
      menu->close();
    }
    CHECK(adjustments->isVisible() && !properties->isVisible());
    CHECK(session.document()->selection() == selectionBefore);
    CHECK(workspace->panelTabs(adjustments).size() == 2);
  }
  window.close();
}
void panelLayoutPersistence() {
  const auto workspaceFor = [](u::MainWindow& window) -> u::OverlayDockWorkspace* {
    for (auto* widget : window.findChildren<QWidget*>())
      if (auto* workspace = dynamic_cast<u::OverlayDockWorkspace*>(widget)) return workspace;
    return nullptr;
  };
  const auto panel = [](u::MainWindow& window, const char* name) {
    return dynamic_cast<u::WorkspacePanel*>(window.findChild<QWidget*>(name));
  };
  QByteArray saved;
  QRect floating;
  {
    u::MainWindow window(nativeInstance);
    window.setUnsavedPromptEnabled(false);
    window.resize(1400, 900); window.show(); QTest::qWait(40);
    auto* workspace = workspaceFor(window);
    auto* layers = panel(window, "LayersPanel");
    auto* color = panel(window, "ColorPanelShell");
    auto* adjustments = panel(window, "AdjustmentsPanelShell");
    auto* properties = panel(window, "PropertiesPanelShell");
    CHECK(workspace && layers && color && adjustments && properties);
    if (!workspace || !layers || !color || !adjustments || !properties) return;
    auto* swatches = panel(window, "SwatchesPanelShell");
    CHECK(swatches && workspace->panelTabs(color) == std::vector<u::WorkspacePanel*>({color, swatches}));
    CHECK(color->isVisible() && !swatches->isVisible());
    CHECK(workspace->panelTabs(properties) == std::vector<u::WorkspacePanel*>({properties, adjustments}));
    CHECK(properties->isVisible() && !adjustments->isVisible());
    workspace->tabifyPanel(color, layers);
    workspace->tabifyPanel(properties, adjustments);
    // Fit the small offscreen test display as well as native desktop screens.
    workspace->floatPanel(adjustments, {320, 40, 360, 400});
    auto* visibility = window.findChild<QAction*>("PropertiesPanelVisibilityAction");
    CHECK(visibility && visibility->isChecked());
    if (visibility) visibility->trigger();
    workspace->activatePanel(adjustments);
    QTest::qWait(40);
    saved = workspace->savePanelTabs();
    floating = workspace->floatingPanelGeometry(adjustments);
    CHECK(color->isVisible() && !layers->isVisible());
    CHECK(window.close());
    CHECK(QSettings().value("window/panel-tabs-v1").toByteArray() == saved);
  }
  {
    u::MainWindow window(nativeInstance);
    window.setUnsavedPromptEnabled(false);
    window.show(); QTest::qWait(60);
    auto* workspace = workspaceFor(window);
    auto* layers = panel(window, "LayersPanel");
    auto* color = panel(window, "ColorPanelShell");
    auto* adjustments = panel(window, "AdjustmentsPanelShell");
    auto* properties = panel(window, "PropertiesPanelShell");
    CHECK(workspace->panelTabs(layers).size() == 2);
    CHECK(workspace->panelTabs(adjustments).size() == 2);
    CHECK(workspace->panelPlacement(layers) == u::OverlayDockWorkspace::PanelPlacement::DockedLeft);
    CHECK(workspace->floatingPanelGeometry(adjustments) == floating);
    CHECK(workspace->panelVisible(layers) && !layers->isVisible() && color->isVisible());
    CHECK(!workspace->panelVisible(properties) && adjustments->isVisible());
    CHECK(workspace->savePanelTabs() == saved);
    CHECK(window.close());
  }
  {
    u::MainWindow window(nativeInstance, true, false, {}, nullptr,
                         u::MainWindow::PanelLayoutMode::SessionDefaults);
    window.setUnsavedPromptEnabled(false);
    window.show(); QTest::qWait(40);
    auto* workspace = workspaceFor(window);
    CHECK(workspace->panelTabs(panel(window, "LayersPanel")).size() == 1);
    CHECK(workspace->panelTabs(panel(window, "PropertiesPanelShell")) == std::vector<u::WorkspacePanel*>({
        panel(window, "PropertiesPanelShell"), panel(window, "AdjustmentsPanelShell")}));
    CHECK(workspace->panelTabs(panel(window, "ColorPanelShell")).size() == 2);
    CHECK(window.close());
    CHECK(QSettings().value("window/panel-tabs-v1").toByteArray() == saved);
  }
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
    clipboardMappingContracts();
    clipboardWorkflow();
    expandedEditingContracts();
    const auto projectIndex=app.arguments().indexOf("--copy-mask-project");
    if(projectIndex>=0&&projectIndex+1<app.arguments().size())clipboardWorkflow(app.arguments()[projectIndex+1]);
    coreContracts();
    persistenceAndBake();
    uiContracts();
    panelLayoutPersistence();
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
