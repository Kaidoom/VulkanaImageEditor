// Include at file scope (outside another namespace) and call
// imageeditor::tests::adjustmentMaskOutlineChecks(check), where check accepts
// (bool, std::string_view). Pure core checks; no Qt/Vulkan or standalone target.
#include "imageeditor/core/SelectionMask.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string_view>
#include <thread>
#include <vector>

namespace imageeditor::tests {
template<class Check>
void adjustmentMaskOutlineChecks(Check&& check)
{
    using namespace imageeditor::core;
    const auto perimeter=[](const std::vector<SelectionEdge>& edges) {
        double length=0;
        for(const auto& e:edges)length+=std::abs(e.to.x-e.from.x)+std::abs(e.to.y-e.from.y);
        return length;
    };
    const auto hasEdge=[](const std::vector<SelectionEdge>& edges, SelectionEdge edge) {
        return std::find(edges.begin(),edges.end(),edge)!=edges.end();
    };
    // Entirely sub-threshold coverage must remain invisible to ordinary ants
    // while its support contour is still available for captured-mask feedback.
    const auto faint=SelectionMask::rectangle({16,12},{2,3,7,5},20);
    const auto revision=faint->revision();
    const auto initialMemory=faint->memoryCost();
    check(faint->boundaryEdges().empty(),"20/255 mask does not alter ordinary >=128 selection boundary semantics");
    const auto& faintEdges=faint->nonzeroBoundaryEdges();
    check(faintEdges.size()==4 && perimeter(faintEdges)==24,"Sub-threshold rectangle gets exact four-sided support contour");
    check(hasEdge(faintEdges,{{2,3},{9,3}})&&hasEdge(faintEdges,{{2,8},{9,8}}),"Support contour uses document pixel edges");
    check(faint->memoryCost()==initialMemory+faintEdges.capacity()*sizeof(SelectionEdge),"Only constructed nonzero cache is charged");
    check(faint->revision()==revision && faint->bounds()==RectI{2,3,7,5},"Contour construction changes neither revision nor cached coverage bounds");
    const auto* cached=faintEdges.data();
    const auto cachedMemory=faint->memoryCost();
    for(int i=0;i<20;++i)check(faint->nonzeroBoundaryEdges().data()==cached,"Repeated outline requests reuse immutable cached geometry");
    check(faint->memoryCost()==cachedMemory,"Repeated requests do not allocate or increase memory accounting");
    check(faint->coverageAtDocumentPixel(3,4)==20,"Support contour never modifies actual partial coverage");

    // Ring plus disconnected faint island: the inner zero hole must stay open,
    // and merely varying positive coverage must not create internal outlines.
    constexpr Extent2u extent{14,10};
    std::array<std::uint8_t,extent.width*extent.height> pixels{};
    for(int y=1;y<8;++y)for(int x=1;x<8;++x)
        if(!(x>=3&&x<6&&y>=3&&y<6))pixels[std::size_t(y)*extent.width+std::size_t(x)]=20;
    for(int y=2;y<5;++y)for(int x=10;x<12;++x)pixels[std::size_t(y)*extent.width+std::size_t(x)]=1;
    pixels[extent.width+1]=255; // An internal coverage change is not a support edge.
    const auto islands=SelectionMask::fromR8(extent,pixels,extent.width);
    const auto& support=islands->nonzeroBoundaryEdges();
    check(support.size()==12 && perimeter(support)==50,"Support extraction preserves outer ring, zero hole, and disconnected island");
    check(hasEdge(support,{{3,3},{6,3}})&&hasEdge(support,{{3,6},{6,6}}),"Zero-coverage hole receives its own contour");
    check(hasEdge(support,{{10,2},{12,2}})&&hasEdge(support,{{10,5},{12,5}}),"One-byte coverage island remains visible");
    check(islands->boundaryEdges().size()==4 && perimeter(islands->boundaryEdges())==4,"Ordinary boundary remains just the opaque pixel");

    // Solid coverage crosses uniform tile seams and incomplete final tiles.
    // No contours may appear at the internal compression-grid boundaries.
    const auto tiled=SelectionMask::filled({259,131},20);
    check(tiled->boundaryEdges().empty(),"Sub-threshold uniform tiles skip ordinary selection extraction");
    check(tiled->nonzeroBoundaryEdges().size()==4 && perimeter(tiled->nonzeroBoundaryEdges())==780,
        "Uniform support contour joins across tile seams and clipped final tiles");
    const auto empty=SelectionMask::filled({32,17},0);
    const auto emptyMemory=empty->memoryCost();
    check(empty->boundaryEdges().empty()&&empty->nonzeroBoundaryEdges().empty(),"Empty masks have no contour of either kind");
    check(empty->memoryCost()==emptyMemory,"Empty lazy caches retain no edge allocation");

    // First-call races exercise both independent once_flags on the same frozen
    // mask. Worker threads only write disjoint result slots; the owner validates.
    const auto concurrent=SelectionMask::fromR8(extent,pixels,extent.width);
    const auto before=concurrent->memoryCost(),beforeRevision=concurrent->revision();
    std::array<const std::vector<SelectionEdge>*,8> ordinary{},nonzero{};
    std::array<std::thread,8> workers;
    for(std::size_t i=0;i<workers.size();++i)workers[i]=std::thread([&,i] {
        if(i%2){ordinary[i]=&concurrent->boundaryEdges();nonzero[i]=&concurrent->nonzeroBoundaryEdges();}
        else {nonzero[i]=&concurrent->nonzeroBoundaryEdges();ordinary[i]=&concurrent->boundaryEdges();}
        (void)concurrent->memoryCost(); // Concurrent accounting reads are atomic.
    });
    for(auto& worker:workers)worker.join();
    for(std::size_t i=0;i<workers.size();++i) {
        check(ordinary[i]==ordinary[0]&&nonzero[i]==nonzero[0],"Concurrent callers receive the same immutable cache instances");
        check(*ordinary[i]==islands->boundaryEdges()&&*nonzero[i]==support,"Concurrent extraction remains deterministic");
    }
    check(concurrent->memoryCost()==before+(ordinary[0]->capacity()+nonzero[0]->capacity())*sizeof(SelectionEdge),
        "Memory accounting includes both edge caches exactly once after concurrent construction");
    check(concurrent->revision()==beforeRevision&&concurrent->equivalent(*islands),"Concurrent lazy visualization leaves immutable coverage/revision unchanged");
}
} // namespace imageeditor::tests
