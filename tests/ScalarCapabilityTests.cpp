#include "imageeditor/core/ChannelGroups.hpp"
#include "imageeditor/core/BasicPixelBrushStroke.hpp"
#include "imageeditor/core/CloneReference.hpp"
#include "imageeditor/core/FillOperation.hpp"
#include "imageeditor/core/History.hpp"
#include <array>
#include <iostream>
#include <stdexcept>

using namespace imageeditor::core;
namespace {
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void packedDataRequiresExplicitChannelEditing()
{
    Document document({.extent = {8,8}}); History history;
    auto surface = std::make_shared<ContiguousRasterSurface>(Extent2u{8,8}, Rgba8{221,32,175,0}, RasterDataKind::PackedData);
    auto raster = Layer::raster("Packed", surface); const auto id = raster.id;
    check(document.insertLayer(0,raster), "insert data"); document.markSaved();
    const auto state = document.contentState(), revision = surface->revision();
    check(!document.setLayerOpacity(id,.5F) && !document.setLayerFillOpacity(id,.5F)
        && !document.setLayerBlendMode(id,BlendMode::Multiply), "no colour styling on data");
    RasterEditTransaction transaction(document,id,"Invalid colour edit");
    check(!transaction.active(), "packed pixels cannot take colour transaction");
    BrushSettings settings; settings.sizePixels=3;
    BasicPixelBrushStroke brush(document,id,settings,BrushCompositeMode::Paint);
    NormalizedPointerSample sample; sample.documentPosition={4,4}; sample.pressure=1;
    check(!brush.begin(sample), "packed pixels cannot take colour brush");
    FillOperation fill(document,id,{});
    check(fill.state()==FillState::Failed, "packed pixels cannot take colour fill");
    std::string diagnostic;
    check(!CloneReference::capture(document,id,id,CloneSampleSource::SourceLayer,diagnostic), "packed data cannot take colour retouch");
    check(diagnostic.find("Channel Group") != std::string::npos, "retouch recovery guidance");
    auto replacement = raster; replacement.payload = RasterLayer{std::make_shared<ContiguousRasterSurface>(Extent2u{8,8})};
    check(!document.replaceStructure(document.tree(),document.tree(),std::array{id},std::span(&replacement,1)), "structural bypass rejected");
    check(document.contentState()==state && surface->revision()==revision && !document.isModified() && !history.canUndo(), "all rejected paths unchanged");
    check(surface->contentBounds()==RectI{0,0,8,8}, "zeroalpha data keeps bounds");
    const LayerSelectionState selected{{id},id,id};
    check(history.execute(document,convertToChannelGroup(document,selected)), "data remains explicitly editable as channels");
    const auto alpha=document.tree().container(id)->channelRoles[3];
    auto numeric=std::get<RasterLayer>(document.layer(alpha)->payload).surface;
    check(numeric->dataKind()==RasterDataKind::Scalar,"alpha is numeric");
    check(history.undo(document), "conversion undo");
    check(std::get<RasterLayer>(document.layer(id)->payload).surface==surface,"original raw surface retained");
}
}
int main()
{
    try { packedDataRequiresExplicitChannelEditing(); std::cout<<"Scalar capability tests passed\n"; }
    catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
