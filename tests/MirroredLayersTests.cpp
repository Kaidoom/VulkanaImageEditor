#include "imageeditor/core/MirroredLayers.hpp"
#include "imageeditor/core/EditorSession.hpp"
#include "imageeditor/core/LayerCommands.hpp"
#include "imageeditor/core/EffectCommands.hpp"
#include "imageeditor/core/SpatialFilterCache.hpp"
#include <array>
#include <iostream>
#include <stdexcept>
using namespace imageeditor::core;
namespace {
int failures=0;
#define CHECK(x) do { if (!(x)) {std::cerr<<"FAIL "<<__LINE__<<": " #x "\n";++failures;} } while(false)
std::vector<std::byte> pixels(const Layer& layer) {
    const auto& surface=std::get<RasterLayer>(layer.payload).surface;
    const auto e=surface->extent();std::vector<std::byte> result(std::size_t(e.width)*e.height*4);
    surface->copyRgba8({0,0,int(e.width),int(e.height)},result,std::size_t(e.width)*4);return result;
}
struct Fixture {
    EditorSession session; LayerId source{},mirror{};
    Fixture() {
        auto document=std::make_unique<Document>(CanvasSpec{{32,32}});
        auto layer=Layer::raster("Source",std::make_shared<ContiguousRasterSurface>(Extent2u{8,8},Rgba8{37,72,143,0}));
        source=layer.id;layer.localToDocument={.m02=3,.m12=4};
        document->insertLayer(0,std::move(layer));session.replaceDocument(std::move(document));
        CHECK(session.execute(createMirroredLayer(doc(),session.layerSelectionState())));
        mirror=*session.activeLayer();
    }
    Document& doc(){return *session.document();}
    Layer& base(){return *doc().layer(source);}
    Layer& linked(){return *doc().layer(mirror);}
};
void pixelsAndIndependentGeometry() {
    Fixture f;auto& d=f.doc();
    CHECK(f.linked().mirror && f.linked().mirror->source==f.source);
    CHECK(pixels(f.base())==pixels(f.linked()));
    CHECK(std::get<RasterLayer>(f.base().payload).surface->id()==std::get<RasterLayer>(f.linked().payload).surface->id());
    const auto transform=f.linked().localToDocument;
    CHECK(d.setLayerTransform(f.source,{.m02=20,.m12=20}));CHECK(f.linked().localToDocument==transform);
    CHECK(d.setLayerTransform(f.mirror,{.m02=-7,.m12=11}));CHECK(f.base().localToDocument.m02==20);
    const std::array<std::byte,4> color{std::byte{254},std::byte{12},std::byte{90},std::byte{0}};
    const auto revision=d.revision();
    const auto surface=std::get<RasterLayer>(f.base().payload).surface;
    const auto previous=surface->revision();surface->replaceRgba8({0,0,1,1},color,4);
    CHECK(d.revision()==revision);CHECK(pixels(f.base())==pixels(f.linked()));
    CHECK(!std::get<RasterLayer>(f.linked().payload).surface->dirtySince(previous).empty());
    bool refused=false;try {std::get<RasterLayer>(f.linked().payload).surface->replaceRgba8({0,0,1,1},color,4);}catch(const std::logic_error&){refused=true;}
    CHECK(refused);
    auto grown=std::make_shared<ContiguousRasterSurface>(Extent2u{12,10},Rgba8{255,0,0,128});
    CHECK(d.setLayerRasterStorage(f.source,grown,{-2,-1},std::nullopt));
    CHECK(f.linked().rasterOrigin==(Vec2d{-2,-1}));CHECK(pixels(f.base())==pixels(f.linked()));
    const auto snapshot=d.snapshot();
    CHECK(std::get<RasterLayerSnapshot>(snapshot.layersBottomToTop[0].payload).surface->id()
        ==std::get<RasterLayerSnapshot>(snapshot.layersBottomToTop[1].payload).surface->id());
}
void inheritedValuesAndUndo() {
    Fixture f;auto& s=f.session;auto& d=f.doc();
    CHECK(s.execute(std::make_unique<SetLayerOpacityCommand>(f.source,.6F)));CHECK(f.linked().opacity==.6F);
    CHECK(s.execute(std::make_unique<SetLayerOpacityCommand>(f.mirror,.3F)));CHECK(f.linked().mirror->opacityOverride);
    CHECK(s.undo());CHECK(!f.linked().mirror->opacityOverride && f.linked().opacity==.6F);
    CHECK(s.redo());CHECK(f.linked().mirror->opacityOverride && f.linked().opacity==.3F);
    CHECK(s.execute(std::make_unique<SetLayerOpacityCommand>(f.source,.8F)));CHECK(f.linked().opacity==.3F);
    CHECK(s.execute(std::make_unique<SyncMirroredLayerCommand>(f.mirror,MirrorSync::Opacity)));CHECK(f.linked().opacity==.8F);
    CHECK(s.undo());CHECK(f.linked().opacity==.3F && f.linked().mirror->opacityOverride);
    CHECK(s.redo());CHECK(f.linked().opacity==.8F && !f.linked().mirror->opacityOverride);
    CHECK(s.execute(std::make_unique<SetLayerFillOpacityCommand>(f.mirror,.4F)));CHECK(f.linked().mirror->fillOpacityOverride);
    CHECK(s.undo());CHECK(!f.linked().mirror->fillOpacityOverride && f.linked().fillOpacity==1);
    CHECK(s.redo());CHECK(f.linked().fillOpacity==.4F);
    CHECK(s.execute(std::make_unique<SetLayerBlendModeCommand>(f.mirror,BlendMode::Multiply)));CHECK(f.linked().mirror->blendModeOverride);
    CHECK(s.undo());CHECK(!f.linked().mirror->blendModeOverride);
    CHECK(s.redo());CHECK(f.linked().blendMode==BlendMode::Multiply);
    const auto transform=f.linked().localToDocument;
    CHECK(s.execute(std::make_unique<SyncMirroredLayerCommand>(f.mirror)));
    CHECK(f.linked().fillOpacity==f.base().fillOpacity && f.linked().blendMode==f.base().blendMode);
    CHECK(f.linked().localToDocument==transform);
    CHECK(!s.execute(std::make_unique<SyncMirroredLayerCommand>(f.mirror)));
}
void effectOverridesAndTransactions() {
    Fixture f;auto& d=f.doc();auto& s=f.session;
    auto base=std::make_shared<LayerEffectStack>();base->items[0].enabled=true;base->items[0].size=7;
    CHECK(d.setLayerEffects(f.source,base));CHECK(f.linked().effects->items[0].size==7);
    auto custom=std::make_shared<LayerEffectStack>(*f.linked().effects);custom->items[0].size=13;
    {EffectEditTransaction edit(d,f.mirror);CHECK(edit.update(custom));CHECK(edit.commit(s.history()));}
    CHECK(f.linked().mirror->effects[0]==MirrorEffectMode::Override);
    auto off=std::make_shared<LayerEffectStack>(*f.linked().effects);off->items[0].enabled=false;
    {EffectEditTransaction edit(d,f.mirror);CHECK(edit.update(off));CHECK(edit.commit(s.history()));}
    CHECK(f.linked().mirror->effects[0]==MirrorEffectMode::Inherit);
    CHECK(f.linked().effects->items[0].enabled && f.linked().effects->items[0].size==7);
    CHECK(s.undo());CHECK(f.linked().mirror->effects[0]==MirrorEffectMode::Override && f.linked().effects->items[0].size==13);
    CHECK(s.redo());CHECK(f.linked().mirror->effects[0]==MirrorEffectMode::Inherit);
    off=std::make_shared<LayerEffectStack>(*f.linked().effects);off->items[0].enabled=false;
    CHECK(s.execute(std::make_unique<SetLayerEffectsCommand>(f.mirror,f.linked().effects,off)));
    CHECK(f.linked().mirror->effects[0]==MirrorEffectMode::Disabled && !f.linked().effects->items[0].enabled);
    auto on=std::make_shared<LayerEffectStack>(*f.linked().effects);on->items[0].enabled=true;
    CHECK(s.execute(std::make_unique<SetLayerEffectsCommand>(f.mirror,f.linked().effects,on)));
    CHECK(f.linked().mirror->effects[0]==MirrorEffectMode::Inherit);
    {EffectEditTransaction edit(d,f.mirror);CHECK(edit.update(custom));CHECK(edit.cancel());}
    CHECK(f.linked().mirror->effects[0]==MirrorEffectMode::Inherit && f.linked().effects->items[0].size==7);
    const auto same=f.linked().effects;
    CHECK(s.execute(std::make_unique<SetLayerEffectsCommand>(f.mirror,same,same,true)));
    CHECK(f.linked().mirror->effects[0]==MirrorEffectMode::Override);
    CHECK(s.undo());CHECK(f.linked().mirror->effects[0]==MirrorEffectMode::Inherit);
    CHECK(s.redo());CHECK(f.linked().mirror->effects[0]==MirrorEffectMode::Override);
}
void deletionPromotionAndTransfer() {
    Fixture f;auto& s=f.session;auto& d=f.doc();const auto original=pixels(f.linked());
    CHECK(s.execute(createMirroredLayer(d,s.layerSelectionState())));const auto child=*s.activeLayer();
    CHECK(d.layer(child)->mirror->source==f.mirror);
    CHECK(s.execute(std::make_unique<RemoveLayerCommand>(f.source)));
    CHECK(!d.layer(f.source));CHECK(!f.linked().mirror);CHECK(pixels(f.linked())==original);
    CHECK(d.layer(child)->mirror->source==f.mirror);CHECK(pixels(*d.layer(child))==original);
    CHECK(s.undo());CHECK(f.linked().mirror && f.linked().mirror->source==f.source);
    CHECK(s.redo());CHECK(!f.linked().mirror);CHECK(s.undo());
    s.setActiveLayer(f.mirror);
    CHECK(s.execute(promoteMirroredLayer(d,s.layerSelectionState())));CHECK(!f.linked().mirror);
    CHECK(std::get<RasterLayer>(f.base().payload).surface->id()!=std::get<RasterLayer>(f.linked().payload).surface->id());
    CHECK(s.undo());CHECK(f.linked().mirror);
    const std::array both{f.source,f.mirror};auto transfer=captureLayerTransfer(d,both);
    CHECK(transfer.layers.size()==2);
    CHECK(transfer.layers[1].mirror && transfer.layers[1].mirror->source==transfer.layers[0].id);
    const std::array one{f.mirror};auto independent=captureLayerTransfer(d,one);
    CHECK(independent.layers.size()==1 && !independent.layers[0].mirror && pixels(independent.layers[0])==original);
    s.setLayerSelection(both,f.mirror,f.source);
    CHECK(s.execute(duplicateLayerItems(d,s.layerSelectionState())));
    const auto duplicate=*s.activeLayer();CHECK(d.layer(duplicate)->mirror && d.layer(duplicate)->mirror->source!=f.source);
}
void invalidLinksAndTypedSources() {
    Fixture f;auto& d=f.doc();const auto before=d.revision();
    CHECK(!d.setLayerMirrorState(f.source,LayerMirror{.source=f.mirror}));CHECK(d.revision()==before);
    CHECK(!d.setLayerMirrorState(f.mirror,LayerMirror{.source=999999}));CHECK(d.revision()==before);
    CHECK(!d.setLayerMirrorState(f.mirror,std::nullopt));CHECK(d.revision()==before);
    Layer source=Layer::text("Text",TextLayer{.utf8="Hello"});const auto id=source.id;
    CHECK(d.insertLayer(d.layers().size(),source));f.session.setActiveLayer(id);
    CHECK(f.session.execute(createMirroredLayer(d,f.session.layerSelectionState())));const auto mirror=*f.session.activeLayer();
    auto text=std::get<TextLayer>(d.layer(id)->payload);text.utf8="Changed";
    CHECK(d.setLayerText(id,text));CHECK(std::get<TextLayer>(d.layer(mirror)->payload).utf8=="Changed");
    CHECK(!d.setLayerText(mirror,TextLayer{}));
}
void sharedEffectCaches() {
    Fixture f; auto& d=f.doc();
    auto effects=std::make_shared<LayerEffectStack>();
    effects->items[0].enabled=true; effects->items[0].size=3;
    CHECK(d.setLayerEffects(f.source,effects));
    f.base().effectCache=prepareLayerEffects(f.base(), {});
    const auto cache=f.base().effectCache;
    CHECK(cache && f.linked().effectCache==cache && layerEffectCacheValid(f.linked()));
    CHECK(d.setLayerOpacity(f.source,.5F));
    CHECK(f.linked().effectCache==cache && layerEffectCacheValid(f.linked()));
    CHECK(d.setLayerTransform(f.mirror,{.m02=12,.m12=15}));
    CHECK(f.linked().effectCache==cache && layerEffectCacheValid(f.linked()));
    auto recolored=std::make_shared<LayerEffectStack>(*effects);recolored->items[0].color={0,255,0,255};
    CHECK(d.setLayerEffects(f.source,recolored));
    CHECK(f.linked().effectCache==cache && layerEffectCacheValid(f.linked()));
    const std::array<std::byte,4> pixel{std::byte{255},std::byte{0},std::byte{0},std::byte{255}};
    std::get<RasterLayer>(f.base().payload).surface->replaceRgba8({0,0,1,1},pixel,4);
    CHECK(!layerEffectCacheValid(f.linked()));
    f.linked().effectCache=prepareLayerEffects(f.linked(), {});
    CHECK(f.linked().effectCache!=cache && layerEffectCacheValid(f.linked()));
}
}
int main(){try{pixelsAndIndependentGeometry();inheritedValuesAndUndo();effectOverridesAndTransactions();deletionPromotionAndTransfer();invalidLinksAndTypedSources();sharedEffectCaches();}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}std::cout<<failures<<" mirrored layer failures\n";return failures?1:0;}
