#include "imageeditor/core/LayerGeometry.hpp"
#include "imageeditor/core/LayerTransform.hpp"
#include <cmath>
#include <iostream>
using namespace imageeditor::core;
int failures = 0;
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        if (!(x)) {                                                                                          \
            std::cerr << "FAIL " << __LINE__ << ": " #x "\n";                                                \
            ++failures;                                                                                      \
        }                                                                                                    \
    } while (false)
bool close(Vec2d a, Vec2d b, double tolerance = 1e-8) { return std::hypot(a.x - b.x, a.y - b.y) < tolerance; }
void visibleEdgeHandlesAfterDistortion()
{
    // Independent visible quad, matching the strongly foreshortened report.
    // The GPU overlay places side handles halfway between projected corners.
    const std::array<Vec2d, 4> quad{{{63,85},{579,85},{659,190},{63,563}}};
    for (bool flipped : {false, true}) for (double angle : {0., 37., 90.}) {
        auto ordered = quad;
        if (flipped) { std::swap(ordered[0],ordered[1]); std::swap(ordered[2],ordered[3]); }
        const auto perspective = rectangleToQuad({0,0,520.5,480.25},ordered);
        CHECK(perspective);
        if (!perspective) continue;
        const auto rotation = transformFromValues({{0,0},1,1,angle},{1,1});
        const auto matrix = composeTransform(rotation,*perspective);
        const auto handles = geometryTransformHandles(matrix,{520.5,480.25});
        for (std::size_t edge=0; edge<4; ++edge) {
            const auto a=rotation.map(ordered[edge]), b=rotation.map(ordered[(edge+1)%4]);
            CHECK(close(handles[edge*2],a));
            const auto visible=(a+b)*.5;
            CHECK(close(handles[edge*2+1],visible));
            for (double zoom : {.5,1.,1.25,4.}) {
                auto logical=handles;
                for (auto& p:logical) p=p*zoom+Vec2d{31,17};
                CHECK(hitTestTransform(logical,visible*zoom+Vec2d{31,17})==TransformHandle(edge*2+1));
            }
        }
    }
}
void resizingDoesNotInverseProjectThePointerAcrossAHorizon()
{
    const Extent2u extent{1024,768};
    // Three corners almost align, as in the reported thin triangular frame.
    const auto matrix=rectangleToQuad({0,0,1024,768},
        std::array<Vec2d,4>{{{254,179},{624,145},{817,132},{96,393}}});
    CHECK(matrix);
    if (!matrix) return;
    const auto values=valuesFromTransform(*matrix,extent);
    const auto inverse=matrix->inverted();
    CHECK(values && inverse);
    if (!values || !inverse) return;
    const auto handles=transformHandles(*matrix,extent);
    const auto press=handles[1];
    const double horizonOffset=-inverse->denominator(press)/inverse->m21;
    CHECK(std::isfinite(horizonOffset) && std::abs(horizonOffset)<50);
    TransformDrag drag(extent,*matrix,*values,TransformHandle::Top,press);
    double greatestCornerTravel=0;
    for (double offset : {horizonOffset-1.,horizonOffset-.01,horizonOffset,
             horizonOffset+.01,horizonOffset+1.}) {
        const auto result=drag.resolve(press+Vec2d{0,offset},{.shift=true},true,*values);
        CHECK(result);
        if (!result) continue;
        const auto next=transformHandles(transformFromValues(*result,extent),extent);
        for (std::size_t i : {0u,2u,4u,6u}) {
            const auto travel=std::hypot(next[i].x-handles[i].x,next[i].y-handles[i].y);
            greatestCornerTravel=std::max(greatestCornerTravel,travel);
            CHECK(std::isfinite(travel) && travel<1000);
        }
    }
    std::cout << "Near-triangle resize: inverse horizon " << horizonOffset
              << " px from press; greatest corner travel " << greatestCornerTravel << " px\n";

    // The same session stays usable beyond that screen-space horizon, returns
    // without drift, and records only geometry (not rebuilt source pixels).
    Document document({{1024,768},96});
    auto source=std::make_shared<ContiguousRasterSurface>(extent,Rgba8{200,70,20,255});
    auto layer=Layer::raster("Thin quad",source);layer.localToDocument=*matrix;
    const auto id=layer.id;
    CHECK(document.insertLayer(0,std::move(layer)));
    LayerTransformSession session(document,id);
    CHECK(session.active() && session.beginDrag(TransformHandle::Top,press));
    CHECK(session.dragTo(press+Vec2d{0,horizonOffset-.01},{.shift=true},true));
    const auto resized=session.transform();
    CHECK(resized!=*matrix && !resized.isAffine());
    CHECK(session.dragTo(press,{.shift=true},true));
    const auto returned=transformHandles(session.transform(),extent);
    for(std::size_t i=0;i<handles.size();++i)CHECK(close(returned[i],handles[i],1e-6));
    CHECK(session.dragTo(press+Vec2d{0,horizonOffset-.01},{.shift=true},true));
    CHECK(session.transform()==resized);
    session.endDrag();CHECK(session.undo());CHECK(session.transform()==*matrix);
    CHECK(session.redo());CHECK(session.transform()==resized);
    session.cancel();CHECK(document.layer(id)->localToDocument==*matrix);
    CHECK(std::get<RasterLayer>(document.layer(id)->payload).surface==source);
}

void projectedResizeFollowsTheForwardHandleTrajectory()
{
    const Extent2u extent{1024,768};
    const auto distortion=rectangleToQuad({0,0,1024,768},
        std::array<Vec2d,4>{{{254,179},{624,145},{817,132},{96,393}}});
    CHECK(distortion);if(!distortion)return;
    for (bool flip:{false,true}) for (double angle:{0.,37.}) {
        const auto matrix=composeTransform(transformFromValues({{500,400},flip?-1.:1.,1.,angle},extent),*distortion);
        const auto values=valuesFromTransform(matrix,extent);CHECK(values);if(!values)continue;
        const auto handles=transformHandles(matrix,extent);
        for (std::size_t i=0;i<8;++i) for(bool uniform:{false,true}) for(bool alt:{false,true}) {
            const Vec2d unit{(i==0||i==6||i==7)?0.:(i==1||i==5)?.5:1.,
                i<=2?0.:(i==3||i==7)?.5:1.};
            const Vec2d anchor=alt?Vec2d{512,384}:Vec2d{(1-unit.x)*1024,(1-unit.y)*768};
            for(double factor:{.8,1.2,-.5}) {
                auto expected=*values;
                if(uniform||unit.x!=.5)expected.scaleX*=factor;
                if(uniform||unit.y!=.5)expected.scaleY*=factor;
                expected.center={};
                expected.center=matrix.map(anchor)-transformFromValues(expected,extent).map(anchor);
                const auto expectedMatrix=transformFromValues(expected,extent);
                const auto expectedHandles=transformHandles(expectedMatrix,extent);
                TransformDrag drag(extent,matrix,*values,TransformHandle(i),handles[i]);
                const auto result=drag.resolve(expectedHandles[i],{.shift=!uniform,.alt=alt},true,*values);
                CHECK(result);if(!result)continue;
                CHECK(result->distortion==values->distortion);
                const auto actual=transformHandles(transformFromValues(*result,extent),extent);
                for(std::size_t corner:{0u,2u,4u,6u})CHECK(close(actual[corner],expectedHandles[corner],1e-6));
                // Live Shift/Alt reevaluate from the same press-time geometry.
                const auto repeated=drag.resolve(expectedHandles[i],{.shift=!uniform,.alt=alt},true,*result);
                CHECK(repeated && transformFromValues(*repeated,extent)==transformFromValues(*result,extent));
            }
        }
    }
}
int main()
{
    resizingDoesNotInverseProjectThePointerAcrossAHorizon();
    projectedResizeFollowsTheForwardHandleTrajectory();
    visibleEdgeHandlesAfterDistortion();
    const RectD box{0, 0, 120, 80};
    const std::array<Vec2d, 4> corners{Vec2d{0, 0}, Vec2d{120, 0}, Vec2d{120, 80}, Vec2d{0, 80}};
    const std::array<Vec2d, 4> quad{Vec2d{13, 9}, Vec2d{119, 0}, Vec2d{120, 80}, Vec2d{0, 80}};
    auto h = rectangleToQuad(box, quad);
    CHECK(h && !h->isAffine());
    if (!h)
        return 1;
    for (std::size_t i = 0; i < 4; ++i)
        CHECK(close(h->map(corners[i]), quad[i]));
    const auto inverse = h->inverted();
    CHECK(inverse);
    for (int y = 0; y <= 80; y += 4)
        for (int x = 0; x <= 120; x += 4) {
            const Vec2d p{double(x), double(y)};
            CHECK(close(inverse->map(h->map(p)), p));
            CHECK(close(composeTransform(*h, *inverse).map(p), p));
            const auto d = h->derivatives(p);
            CHECK(close(d[0], (h->map(p + Vec2d{1e-4, 0}) - h->map(p)) * (1e4), 1e-5));
        }
    // A project's flipped winding is valid; a corner crossing its neighbours is
    // not.
    auto flipped = quad;
    std::swap(flipped[0], flipped[1]);
    std::swap(flipped[2], flipped[3]);
    CHECK(rectangleToQuad(box, flipped));
    auto crossed = quad;
    std::swap(crossed[1], crossed[2]);
    CHECK(!rectangleToQuad(box, crossed));
    auto concave = quad;
    concave[0] = {100, 70};
    CHECK(!rectangleToQuad(box, concave));
    auto flat = corners;
    flat[0] = flat[1];
    CHECK(!rectangleToQuad(box, flat));
    CHECK(!(ProjectiveTransform{1, 0, 0, 0, 1, 0, -.02, 0, 1}).validOver(box));
    // Numeric reconstruction preserves perspective. Affine edits compose with
    // it rather than discarding its bottom row during QR decomposition.
    auto values = valuesFromTransform(*h, {120, 80});
    CHECK(values);
    for (auto p : corners)
        CHECK(close(transformFromValues(*values, {120, 80}).map(p), h->map(p)));
    auto moved = *values;
    moved.center = moved.center + Vec2d{7, -4};
    for (auto p : corners)
        CHECK(close(transformFromValues(moved, {120, 80}).map(p), h->map(p) + Vec2d{7, -4}));
    const TransformValues original{{60, 40}};
    TransformDrag drag({120, 80}, {}, original, TransformHandle::TopLeft, {0, 0});
    auto next = drag.resolve({13, 9}, {.control = true}, true, original);
    CHECK(next);
    for (std::size_t i = 0; i < 4; ++i)
        CHECK(close(transformFromValues(*next, {120, 80}).map(corners[i]), i == 0 ? quad[0] : corners[i]));
    CHECK(!drag.resolve({110, 75}, {.control = true}, true, *next));
    auto scaled = drag.resolve({13,9}, {}, true, *next);
    CHECK(scaled && transformFromValues(*scaled, {120,80}).isAffine());
    auto distortedAgain = drag.resolve({13,9}, {.control = true}, true, *scaled);
    CHECK(distortedAgain && transformFromValues(*distortedAgain, {120,80}) == transformFromValues(*next, {120,80}));
    Document document({{300, 200}, 96});
    auto layer = Layer::raster(
        "Grid", std::make_shared<ContiguousRasterSurface>(Extent2u{120, 80}, Rgba8{255, 0, 0, 255}));
    const auto id = layer.id;
    CHECK(document.insertLayer(0, layer));
    History history;
    LayerTransformSession edit(document, id);
    CHECK(edit.beginDrag(TransformHandle::TopLeft, {0, 0}));
    CHECK(edit.dragTo({13, 9}, {.control = true}, true));
    edit.endDrag();
    CHECK(edit.undo());
    CHECK(document.layer(id)->localToDocument == ProjectiveTransform{});
    CHECK(edit.redo());
    CHECK(!document.layer(id)->localToDocument.isAffine());
    CHECK(edit.commit(history) == TransformCommitResult::Committed);
    CHECK(history.undo(document));
    CHECK(document.layer(id)->localToDocument == ProjectiveTransform{});
    CHECK(history.redo(document));
    CHECK(std::get<RasterLayer>(document.layer(id)->payload).surface ==
          std::get<RasterLayer>(layer.payload).surface);
    std::cout << "Projective transforms: " << failures << " failures\n";
    return failures ? 1 : 0;
}
