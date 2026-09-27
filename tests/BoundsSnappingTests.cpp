#include "imageeditor/core/BoundsSnapping.hpp"
#include "imageeditor/core/LayerGeometry.hpp"
#include "imageeditor/core/LayerTransform.hpp"
#include <chrono>
#include <cstdlib>
#include <iostream>

using namespace imageeditor::core;
namespace {
int failures = 0;
#define CHECK(v) do { if (!(v)) { std::cerr << "FAIL " << __LINE__ << ": " #v "\n"; ++failures; } } while(false)
bool near(double a, double b) { return std::abs(a-b) < 1e-8; }
LayerId shape(Document& d, double x, double y, double w=21, double h=13)
{
    ShapeLayer geometry; geometry.size={w,h};
    auto layer=Layer::shape("Bounds",geometry);
    layer.localToDocument.m02=x; layer.localToDocument.m12=y;
    const auto id=layer.id; CHECK(d.insertLayer(d.layers().size(),std::move(layer))); return id;
}
SnapResult solve(BoundsSnapping& snap,const Document& d,double x,double y,SnapOptions options={})
{
    return snap.resolve(d,{x,y},options);
}
void canvasAndFractionalCoordinates()
{
    Document d({.extent={200,100}}); const auto id=shape(d,30,20);
    BoundsSnapping snap; CHECK(snap.begin(d,std::array{id}));
    const auto edge=solve(snap,d,-28,-17);
    CHECK(near(edge.translation.x,-30) && near(edge.translation.y,-20));
    CHECK(edge.guides[0] && edge.guides[1]);
    CHECK(edge.guides[0]->a.x==0 && edge.guides[1]->a.y==0);
    CHECK(snap.begin(d,std::array{id}));
    const auto center=solve(snap,d,60,24);
    CHECK(near(center.translation.x,59.5) && near(center.translation.y,23.5));
    CHECK(center.guides[0]->a.x==100 && center.guides[1]->a.y==50);
    CHECK(snap.begin(d,std::array{id}));
    const auto far=solve(snap,d,146,64);
    CHECK(near(far.translation.x,149) && near(far.translation.y,67));
    CHECK(far.guides[0]->a.x==200 && far.guides[1]->a.y==100);
    // Unclipped bounds prevent an off-canvas edge from sticking at zero.
    CHECK(d.setLayerTransform(id,{1,0,-50,0,1,-40}));
    CHECK(snap.begin(d,std::array{id}));
    const auto off=solve(snap,d,2,3);
    CHECK((off.translation==Vec2d{2,3}) && !off.guides[0] && !off.guides[1]);
    CHECK(d.setLayerTransform(id,{1,0,1000,0,1,1000}));
    for (double scale : {.125,.5,1.,1.5,2.,8.}) {
        CHECK(snap.begin(d,std::array{id}));
        SnapOptions options; options.logicalScale=scale;
        // Minimum edge near maximum canvas edge avoids other competing anchors.
        const auto r=solve(snap,d,-800+5/scale,-900+5/scale,options);
        CHECK(near(r.translation.x,-800) && near(r.translation.y,-900));
    }
}
void layersHysteresisAndConstraints()
{
    Document d({.extent={1000,800}});
    const auto id=shape(d,10,10,20,20), target=shape(d,100,120,40,60);
    BoundsSnapping snap; SnapOptions o; o.canvas=false;
    CHECK(snap.begin(d,std::array{id}));
    auto r=solve(snap,d,68,87,o); // Opposing edges: next to/above target.
    CHECK(near(r.translation.x,70) && near(r.translation.y,90));
    CHECK(r.guides[0]->target==target && r.guides[1]->target==target);
    CHECK(r.guides[0]->a.x==100 && r.guides[0]->a.y==100 && r.guides[0]->b.y==180);
    // A nearer alignment does not replace an active one inside release radius.
    const auto nearby=shape(d,104,124,40,60);
    CHECK(snap.begin(d,std::array{id}));
    r=solve(snap,d,68,88,o);
    CHECK(r.guides[0]->target==target);
    r=solve(snap,d,77,97,o);
    CHECK(near(r.translation.x,70) && r.guides[0]->target==target);
    r=solve(snap,d,80,100,o);
    CHECK(near(r.translation.x,74) && r.guides[0]->target==nearby);
    r=solve(snap,d,300,300,o);
    CHECK((r.translation==Vec2d{300,300}) && !r.guides[0] && !r.guides[1]);
    CHECK(snap.begin(d,std::array{id}));
    o.allowX=false; r=solve(snap,d,68,88,o);
    CHECK(r.translation.x==68 && !r.guides[0] && r.guides[1]);
    o.allowY=false; r=solve(snap,d,68,88,o);
    CHECK((r.translation==Vec2d{68,88}) && !r.guides[1]);
    o.allowX=o.allowY=true; o.bypass=true;
    r=solve(snap,d,68,88,o); CHECK((r.translation==Vec2d{68,88}) && !r.guides[0]);
    o.bypass=false; r=solve(snap,d,68,88,o); CHECK(r.translation.x==70);
    o.enabled=false; r=solve(snap,d,68,88,o); CHECK(r.translation.x==68 && !r.guides[0]);
    o.enabled=true; o.layers=false; r=solve(snap,d,68,88,o); CHECK(r.translation.x==68 && !r.guides[0]);
    // Stable ID, not insertion/selection order, settles equidistant ties.
    o.layers=true; CHECK(snap.begin(d,std::array{id}));
    r=solve(snap,d,72,92,o); CHECK(r.guides[0]->target==target);
}
void hierarchyVisibilityAndInvalidation()
{
    Document d({.extent={1000,800}});
    const auto moving=shape(d,10,10), sibling=shape(d,100,120), hidden=shape(d,105,125);
    const auto folder=makeLayerId(), group=makeLayerId();
    LayerTree tree{{folder},{{folder,"Folder",ContainerKind::Folder,ColorLabel::None,{group,hidden}},
        {group,"Group",ContainerKind::Group,ColorLabel::None,{moving,sibling}}}};
    CHECK(d.replaceStructure(d.tree(),tree)); CHECK(d.setLayerVisibility(hidden,false));
    BoundsSnapping snap; SnapOptions o; o.canvas=false;
    CHECK(snap.begin(d,std::array{moving}));
    auto r=solve(snap,d,88,108,o);
    CHECK(r.guides[0] && r.guides[0]->target==sibling); // Never ancestor group.
    CHECK(d.setLayerVisibility(sibling,false));
    r=solve(snap,d,88,108,o); CHECK(!r.guides[0] && !r.guides[1]);
    CHECK(d.setLayerVisibility(sibling,true));
    CHECK(d.setItemVisibilities(std::array{ItemVisibilityUpdate{folder,true,false}}));
    r=solve(snap,d,88,108,o); CHECK(!r.guides[0]);
    CHECK(d.setItemVisibilities(std::array{ItemVisibilityUpdate{folder,false,true}}));
    r=solve(snap,d,88,108,o); CHECK(r.guides[0]->target==sibling);
    CHECK(d.takeLayer(sibling)); r=solve(snap,d,88,108,o); CHECK(!r.guides[0]);
    CHECK(snap.targetBuildCount()==5);
    CHECK(d.takeLayer(moving)); r=solve(snap,d,88,108,o); CHECK(!r.guides[0]);
}
void croppedRotatedAndSharedTransform()
{
    Document d({.extent={200,150}});
    const auto a=shape(d,20,30,80,40), b=shape(d,120,40,20,20);
    auto cache=std::make_shared<LayerRenderCache>();
    cache->logicalExtent={80,40}; cache->localSourceBounds=RectD{0,0,80,40};
    d.layer(a)->renderCache=cache;
    CHECK(d.setLayerCrop(a,LayerCrop{RectD{10,5,30,20}}));
    CHECK(d.setLayerTransform(a,{0,-2,90,-1,.5,85}));
    const auto bound=layerDocumentBounds(*d.layer(a));
    CHECK(bound && near(bound->minimum.x,40) && near(bound->maximum.x,80));
    CHECK(near(bound->minimum.y,47.5) && near(bound->maximum.y,87.5));
    const auto beforeA=d.layer(a)->localToDocument, beforeB=d.layer(b)->localToDocument;
    const auto group=makeLayerId();
    CHECK(d.replaceStructure(d.tree(),LayerTree{{group},{{group,"Group",ContainerKind::Group,ColorLabel::None,{a,b}}}}));
    BoundsSnapping self; CHECK(self.begin(d,std::array{group,a}));
    const auto isolated=solve(self,d,1,2,{.canvas=false});
    CHECK((isolated.translation==Vec2d{1,2}) && !isolated.guides[0] && !isolated.guides[1]);
    LayerTransformSession move(d,std::array{group,a});
    CHECK(move.beginDrag(TransformHandle::Move,{135,57}));
    SnapOptions options; options.layers=false;
    CHECK(move.dragTo({97,12},{},false,options));
    CHECK(near(d.layer(a)->localToDocument.m02,beforeA.m02-40));
    CHECK(near(d.layer(b)->localToDocument.m02,beforeB.m02-40));
    CHECK(move.snapGuides()[0] && move.snapGuides()[1]);
    CHECK(move.dragTo({97,12},{.control=true},false,options));
    CHECK(near(d.layer(a)->localToDocument.m02,beforeA.m02-38) && !move.snapGuides()[0]);
    CHECK(move.dragTo({97,12},{},false,options));
    move.endDrag(); CHECK(!move.snapGuides()[0]);
    History history; CHECK(move.commit(history)==TransformCommitResult::Committed);
    const auto after=d.layer(a)->localToDocument;
    CHECK(history.undo(d)); CHECK(d.layer(a)->localToDocument==beforeA && d.layer(b)->localToDocument==beforeB);
    CHECK(history.redo(d)); CHECK(d.layer(a)->localToDocument==after);
    // Same bound alignment despite a completely different pointer grab point.
    CHECK(history.undo(d));
    LayerTransformSession other(d,std::array{group}); CHECK(other.beginDrag(TransformHandle::Move,{2,7}));
    CHECK(other.dragTo({-36,-38},{},false,options)); CHECK(d.layer(a)->localToDocument==after);
    other.cancel(); CHECK(d.layer(a)->localToDocument==beforeA && history.canRedo());
}
void edgeResizeSnappingAndLiveCornerDistortion()
{
    Document d({.extent={200,100}});
    const auto id=shape(d,30,20,40,20);
    const auto original=d.layer(id)->localToDocument;
    SnapOptions options; options.layers=false;
    LayerTransformSession edit(d,id);
    const auto handles=[&]{return geometryTransformHandles(edit.transform(),edit.geometryExtent());};
    CHECK(edit.beginDrag(TransformHandle::Right,handles()[3]));
    CHECK(edit.dragTo({98,30},{},false,options));
    CHECK(near(handles()[3].x,100) && near(handles()[7].x,30));
    CHECK(edit.snapGuides()[0] && !edit.snapGuides()[1]);
    CHECK(edit.dragTo({108,30},{},false,options)); // Retain inside release radius.
    CHECK(near(handles()[3].x,100));
    CHECK(edit.dragTo({110,30},{},false,options));
    CHECK(near(handles()[3].x,110) && !edit.snapGuides()[0]);
    CHECK(edit.dragTo({98,30},{.control=true},false,options));
    CHECK(near(handles()[3].x,98) && !edit.snapGuides()[0]);
    CHECK(edit.dragTo({98,30},{},false,options));
    CHECK(near(handles()[3].x,100));
    CHECK(edit.dragTo({98,30},{},false,{.enabled=false}));
    CHECK(near(handles()[3].x,98) && !edit.snapGuides()[0]);
    edit.cancelDrag(); CHECK(d.layer(id)->localToDocument==original);

    CHECK(edit.beginDrag(TransformHandle::Right,handles()[3]));
    CHECK(edit.dragTo({94,30},{.alt=true},true,options));
    CHECK(near(handles()[3].x,100) && near(handles()[7].x,0));
    CHECK(near(edit.values().center.x,50) && near(edit.values().center.y,30));
    CHECK(near(edit.values().scaleX,edit.values().scaleY));
    edit.cancelDrag();
    CHECK(edit.beginDrag(TransformHandle::Bottom,handles()[5]));
    CHECK(edit.dragTo({50,98},{},false,options));
    CHECK(near(handles()[5].y,100) && near(handles()[1].y,20));
    CHECK(!edit.snapGuides()[0] && edit.snapGuides()[1]);
    edit.endDrag(); CHECK(!edit.snapGuides()[1]);
    CHECK(edit.undo()); CHECK(d.layer(id)->localToDocument==original);
    CHECK(edit.redo()); CHECK(near(handles()[5].y,100));
    CHECK(edit.undo());

    // Corners never attract; Ctrl is evaluated live from the original press.
    const auto before=handles();
    CHECK(edit.beginDrag(TransformHandle::BottomRight,before[4]));
    CHECK(edit.dragTo({98,48},{},false,options));
    CHECK(near(handles()[4].x,98) && near(handles()[4].y,48));
    CHECK(!edit.snapGuides()[0] && !edit.snapGuides()[1]);
    const auto resized=edit.transform();
    CHECK(edit.dragTo({98,48},{.control=true},false,options));
    CHECK(!edit.transform().isAffine());
    for (std::size_t corner:{0u,2u,6u}) CHECK(std::hypot(handles()[corner].x-before[corner].x,handles()[corner].y-before[corner].y)<1e-8);
    CHECK(edit.dragTo({98,48},{},false,options)); CHECK(edit.transform()==resized);
    edit.cancel(); CHECK(d.layer(id)->localToDocument==original);
}

void transformedEdgeResizeUsesFeasibleDirectionAndCachedTargets()
{
    for (bool projected:{false,true}) for (bool flipped:{false,true}) for (double angle:{0.,37.,90.,145.}) {
        Document d({.extent={1000,800}});
        const auto id=shape(d,0,0,80,40);
        TransformValues values{{160,120},flipped?-1.:1.,1.,angle,.3};
        auto matrix=transformFromValues(values,{80,40});
        if(projected)matrix=composeTransform(matrix,{1,0,0,0,1,0,.001,.002,1});
        CHECK(d.setLayerTransform(id,matrix));
        LayerTransformSession edit(d,id);
        const auto before=geometryTransformHandles(edit.transform(),edit.geometryExtent());
        // Resize still pins the established opposite layer-local anchor. Under
        // perspective that point is not the visual edge midpoint used by the
        // marker and snapping; do not change the resize pivot as part of this fix.
        const auto fixedAnchor=edit.transform().map({0,20});
        CHECK(edit.beginDrag(TransformHandle::Right,before[3]));
        const auto pointer=before[3]+Vec2d{17,11};
        CHECK(edit.dragTo(pointer,{},false));
        const auto raw=geometryTransformHandles(edit.transform(),edit.geometryExtent());
        const auto direction=raw[3]-before[3];
        const bool x=std::abs(direction.x)>std::abs(direction.y);
        const auto target=shape(d,raw[3].x+(x?2:300),raw[3].y+(x?300:2),40,20);
        CHECK(edit.dragTo(pointer,{},false,{.canvas=false}));
        const auto snapped=geometryTransformHandles(edit.transform(),edit.geometryExtent());
        const auto visibleMiddle=(snapped[2]+snapped[4])*.5;
        CHECK(near(snapped[3].x,visibleMiddle.x) && near(snapped[3].y,visibleMiddle.y));
        const auto guide=edit.snapGuides()[x?0:1]; CHECK(guide && guide->target==target);
        CHECK(near(x?snapped[3].x:snapped[3].y,(x?raw[3].x:raw[3].y)+2));
        const auto anchorAfter=edit.transform().map({0,20});
        CHECK(near(anchorAfter.x,fixedAnchor.x) && near(anchorAfter.y,fixedAnchor.y));
        if(!projected) CHECK(near(snapped[7].x,before[7].x) && near(snapped[7].y,before[7].y));
        CHECK(std::abs((snapped[3].x-raw[3].x)*direction.y-(snapped[3].y-raw[3].y)*direction.x)<1e-7);
        edit.cancel();
    }
    Document d({.extent={200,100}}); const auto id=shape(d,30,20,40,20);
    BoundsSnapping snap; CHECK(snap.begin(d,std::array{id}));
    const auto builds=snap.targetBuildCount();
    for(double zoom:{.5,1.,1.5,4.}) {
        SnapOptions o; o.logicalScale=zoom;
        const auto picked=snap.resolveHandle(d,{200-5/zoom,35},{1,0},o);
        CHECK(picked.guides[0] && near(picked.position.x,200) && near(picked.position.y,35));
    }
    CHECK(snap.targetBuildCount()==builds);
}

void manyLayersCacheAndNoPixelWork()
{
    Document d({.extent={5120,2880}});
    const auto mover=shape(d,500,500);
    for(int i=0;i<2000;++i) shape(d,i*3.25,i*.75,11.5,9.25);
    BoundsSnapping snap;
    const auto buildStart=std::chrono::steady_clock::now();
    CHECK(snap.begin(d,std::array{mover}));
    const auto buildMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-buildStart).count();
    const auto revision=d.revision(), content=d.contentState();
    const auto started=std::chrono::steady_clock::now();
    for(int i=0;i<10000;++i) {
        const auto result=solve(snap,d,i*.125,i*.375);
        CHECK(std::isfinite(result.translation.x));
    }
    const auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
    CHECK(snap.targetBuildCount()==1 && d.revision()==revision && d.contentState()==content);
    for(const auto& layer:d.layers()) CHECK(!layer.renderCache);
    auto transform=d.layer(mover)->localToDocument; transform.m02+=2;
    CHECK(d.setLayerTransform(mover,transform)); snap.acknowledgeTranslation(d.revision());
    (void)solve(snap,d,3,5); CHECK(snap.targetBuildCount()==1);
    std::cout<<"2000 target layers: metadata "<<buildMs<<" ms, 10000 resolves "<<ms<<" ms; one build, no raster caches\n";
}
}
int main()
{
    canvasAndFractionalCoordinates(); layersHysteresisAndConstraints(); hierarchyVisibilityAndInvalidation();
    croppedRotatedAndSharedTransform(); manyLayersCacheAndNoPixelWork();
    edgeResizeSnappingAndLiveCornerDistortion(); transformedEdgeResizeUsesFeasibleDirectionAndCachedTargets();
    if(failures)return EXIT_FAILURE;
    std::cout<<"Bounds snapping passed\n"; return EXIT_SUCCESS;
}
