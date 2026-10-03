#include "imageeditor/core/BasicPixelBrushStroke.hpp"
#include "imageeditor/core/LayerGeometry.hpp"
#include "imageeditor/core/LayerTransform.hpp"
#include "imageeditor/core/Measurement.hpp"
#include <iostream>
using namespace imageeditor::core;
namespace {
int failures=0;
#define CHECK(...) do {if(!(__VA_ARGS__)) {std::cerr<<__LINE__<<": " #__VA_ARGS__ "\n";++failures;}} while(false)
std::vector<std::byte> bytes(const RasterSurface& s) {
    const auto e=s.extent();std::vector<std::byte> b(std::size_t(e.width)*e.height*4);
    s.copyRgba8({0,0,int(e.width),int(e.height)},b,std::size_t(e.width)*4);return b;
}
NormalizedPointerSample sample(Vec2d p,std::uint64_t t=0) {
    NormalizedPointerSample s;s.documentPosition=p;s.pressure=1;s.buttons=PointerButtonPrimary;s.timestampMicroseconds=t;return s;
}
BrushSettings brush() {
    auto s=proceduralBrushPreset(ProceduralBrushPreset::HardRound);
    s.sizePixels=7;s.foreground={210,51,32,255};s.spacingPercent=15;return s;
}
struct Fixture {
    Document doc{{{256,256},72}};History history;
    std::shared_ptr<RasterSurface> original=std::make_shared<ContiguousRasterSurface>(Extent2u{16,12},Rgba8{80,100,120,255});
    LayerId id;
    Fixture() {
        auto l=Layer::raster("Compact",original);id=l.id;l.rasterOrigin={48,55};
        l.rasterEffectFrame=RectD{0,0,256,256};
        auto mask=std::make_shared<LayerMask>();mask->coverage=SelectionMask::rectangle({256,256},{0,0,256,256},128);l.mask=mask;
        CHECK(doc.insertLayer(0,std::move(l)));doc.markSaved();
    }
    std::shared_ptr<RasterSurface> surface() {return std::get<RasterLayer>(doc.layer(id)->payload).surface;}
    std::array<std::byte,4> pixel(int x,int y) {
        auto o=doc.layer(id)->rasterOrigin;std::array<std::byte,4>b{};
        surface()->copyRgba8({x-int(o.x),y-int(o.y),1,1},b,4);return b;
    }
};
void bounds() {
    auto s=std::make_shared<ContiguousRasterSurface>(Extent2u{130,129});
    CHECK(s->contentBounds().empty());
    std::array<std::byte,4> faint{std::byte{255},std::byte{},std::byte{},std::byte{1}};
    s->replaceRgba8({66,70,1,1},faint,4);
    CHECK(s->contentBounds()==RectI{66,70,1,1});
    auto l=Layer::raster("Bounds",s);l.rasterOrigin={-20,9};
    CHECK(layerInteractionBounds(l)==RectD{46,79,1,1});
    LayerSnapshot snapshot;
    snapshot.payload=RasterLayerSnapshot{s};snapshot.rasterOrigin=l.rasterOrigin;
    CHECK(layerInteractionBounds(snapshot)==layerInteractionBounds(l));
    l.opacity=0;l.mask=std::make_shared<LayerMask>();
    CHECK(layerInteractionBounds(l)==RectD{46,79,1,1});
    faint[3]=std::byte{0};s->replaceRgba8({66,70,1,1},faint,4);
    CHECK(s->contentBounds().empty()); // hidden RGB isn't visible geometry
    CHECK(layerInteractionBounds(l)==RectD{-20,9,130,129}); // empty remains editable
    CHECK(layerInteractionBounds(snapshot)==layerInteractionBounds(l));
    faint[3]=std::byte{255};s->replaceRgba8({2,3,1,1},faint,4);
    s->replaceRgba8({128,128,1,1},faint,4);
    CHECK(s->contentBounds()==RectI{2,3,127,126});
    std::array<std::byte,4> zero{};s->replaceRgba8({128,128,1,1},zero,4);
    CHECK(s->contentBounds()==RectI{2,3,1,1});
    CHECK(s->contentBounds()==s->contentBounds());
}
void growth() {
    Fixture f;const auto oldBytes=bytes(*f.original);const auto mask=f.doc.layer(f.id)->mask;
    const auto state=f.doc.contentState();
    BasicPixelBrushStroke s(f.doc,f.id,brush());
    CHECK(s.begin(sample({55,60})));
    CHECK(s.append(sample({10,15},100000)));
    CHECK(s.append(sample({140,120},200000)));
    CHECK(s.end(sample({140,120},210000),f.history)==RasterEditCommitResult::Committed);
    CHECK(f.history.undoDepth()==1);CHECK(f.doc.isModified());
    const auto grown=f.surface();const auto grownBytes=bytes(*grown);const auto origin=f.doc.layer(f.id)->rasterOrigin;
    CHECK(grown!=f.original);CHECK(bytes(*f.original)==oldBytes);
    CHECK(f.doc.layer(f.id)->mask==mask);
    CHECK(layerEffectReferenceFrame(*f.doc.layer(f.id))==RectD{0,0,256,256});
    CHECK(f.pixel(10,15)[3]==std::byte{255});CHECK(f.pixel(140,120)[3]==std::byte{255});
    CHECK(f.history.latestUndoMemoryCost()<512*1024);
    CHECK(f.history.undo(f.doc));CHECK(f.surface()==f.original);CHECK(bytes(*f.surface())==oldBytes);
    CHECK(f.doc.contentState()==state);CHECK(!f.doc.isModified());
    CHECK(f.doc.layer(f.id)->rasterOrigin==Vec2d{48,55});
    CHECK(f.history.redo(f.doc));CHECK(f.surface()==grown);CHECK(bytes(*grown)==grownBytes);
    CHECK(f.doc.layer(f.id)->rasterOrigin==origin);
    { BasicPixelBrushStroke cancel(f.doc,f.id,brush());CHECK(cancel.begin(sample({240,230})));cancel.cancel(); }
    CHECK(f.surface()==grown);CHECK(bytes(*grown)==grownBytes);
    CHECK(f.history.undo(f.doc));
    { BasicPixelBrushStroke cancel(f.doc,f.id,brush());CHECK(cancel.begin(sample({5,5})));cancel.cancel(); }
    CHECK(f.surface()==f.original);CHECK(f.history.canRedo());CHECK(!f.doc.isModified());
    CHECK(f.history.redo(f.doc));
    // Another grown stroke followed by an ordinary in-bounds stroke must keep
    // independent COW states through repeated undo/redo.
    for(auto p:{Vec2d{240,230},Vec2d{100,100}}) {
        BasicPixelBrushStroke t(f.doc,f.id,brush());CHECK(t.begin(sample(p)));
        CHECK(t.end(sample(p,1),f.history)==RasterEditCommitResult::Committed);
    }
    const auto final=bytes(*f.surface());
    for(int i=0;i<3;++i)CHECK(f.history.undo(f.doc));
    CHECK(f.surface()==f.original);CHECK(bytes(*f.original)==oldBytes);
    for(int i=0;i<3;++i)CHECK(f.history.redo(f.doc));
    CHECK(bytes(*f.surface())==final);
}
void restrictions() {
    Fixture f;auto unchanged=bytes(*f.original);
    {
        RasterEditTransaction zero(f.doc,f.id,"No-op growth",{.allowGrowth=true});
        const std::array<std::byte,4> empty{};
        CHECK(zero.writeRgba8({-8,-8,1,1},empty,4).empty());
        CHECK(zero.commit(f.history)==RasterEditCommitResult::NoChanges);
        CHECK(f.surface()==f.original && !f.history.canUndo());
    }
    f.doc.setSelection(SelectionMask::rectangle({256,256},{100,100,10,10},128));
    {
        BasicPixelBrushStroke s(f.doc,f.id,brush());CHECK(s.begin(sample({10,10})));
        CHECK(s.end(sample({10,10},1),f.history)==RasterEditCommitResult::NoChanges);
        CHECK(f.surface()==f.original);CHECK(!f.history.canUndo());
    }
    {
        BasicPixelBrushStroke s(f.doc,f.id,brush());CHECK(s.begin(sample({105,105})));
        CHECK(s.end(sample({105,105},1),f.history)==RasterEditCommitResult::Committed);
        CHECK(f.pixel(105,105)[3]==std::byte{128});CHECK(f.history.undo(f.doc));
    }
    f.doc.setSelection({});
    f.doc.setLayerCrop(f.id,LayerCrop{45,50,30,30});
    {
        BasicPixelBrushStroke s(f.doc,f.id,brush());CHECK(s.begin(sample({10,10})));
        CHECK(s.end(sample({10,10},1),f.history)==RasterEditCommitResult::NoChanges);
        CHECK(f.surface()==f.original);CHECK(f.history.canRedo());
    }
    CHECK(bytes(*f.original)==unchanged);
    f.doc.setLayerCrop(f.id,{});
    f.doc.setLayerTransform(f.id,{-1,.2,230,.1,1,20});
    auto point=f.doc.layer(f.id)->localToDocument.map({100,100});
    {
        BasicPixelBrushStroke s(f.doc,f.id,brush());CHECK(s.begin(sample(point)));
        CHECK(s.end(sample(point,1),f.history)==RasterEditCommitResult::Committed);
        CHECK(f.pixel(100,100)[3]!=std::byte{0});
        CHECK(f.doc.layer(f.id)->localToDocument==AffineTransform{-1,.2,230,.1,1,20});
    }
}
}
int main() {bounds();growth();restrictions();std::cout<<"Raster bounds failures: "<<failures<<'\n';return failures?1:0;}
