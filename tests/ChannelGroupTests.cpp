#include "imageeditor/core/ChannelGroups.hpp"
#include "imageeditor/core/ScalarClipboard.hpp"
#include "imageeditor/core/EffectCommands.hpp"
#include "imageeditor/core/History.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>

using namespace imageeditor::core;
namespace {
void check(bool ok, const char* reason) { if (!ok) throw std::runtime_error(reason); }
std::vector<std::byte> bytes(const RasterSurface& surface)
{
    const auto e = surface.extent(); const auto stride = std::size_t(e.width) * surface.nativeBytesPerPixel();
    std::vector<std::byte> result(stride * e.height);
    surface.copyNative({0, 0, int(e.width), int(e.height)}, result, stride); return result;
}
void conversionAndStructure()
{
    Document document({.extent = {256, 4}}); History history;
    std::vector<std::byte> original(256 * 4 * 4);
    for (std::size_t i = 0; i < original.size(); ++i) original[i] = std::byte((i * 73 + i / 4) % 256);
    for (std::size_t i = 3; i < original.size(); i += 20) original[i] = std::byte{0};
    auto layer = Layer::raster("Packed material", std::make_shared<ContiguousRasterSurface>(Extent2u{256, 4}, original));
    layer.rasterOrigin = {-11, 9}; layer.localToDocument = {1, 0, 20, 0, 1, -7};
    const auto id = layer.id; check(document.insertLayer(0, layer), "insert");
    const LayerSelectionState selected{{id}, id, id};
    check(history.execute(document, convertToChannelGroup(document, selected)), "convert");
    check(document.layers().size() == 4, "four independent leaves");
    const auto group = *document.tree().container(id);
    check(bytes(*packChannelGroup(document, id)) == original, "all bytes preserved including zero-alpha RGB");
    const auto red = group.channelRoles[0];
    check(document.renameLayer(red, "Roughness"), "custom label");
    check(document.tree().container(id)->channelRoles[0] == red, "role independent of label");
    check(!document.setLayerOpacity(red, .5F) && !document.setLayerFillOpacity(red, .5F), "scalar layer opacity rejected");
    check(!document.setLayerBlendMode(red, BlendMode::Multiply), "blend rejected");
    check(!document.setLayerVisibility(red, false), "role visibility cannot discard data");
    check(!document.takeLayer(red), "cannot delete fixed role");
    check(!document.setLayerTransform(red, {}), "individual role geometry fixed");
    check(!document.setLayerRasterStorage(red, std::make_shared<ContiguousRasterSurface>(Extent2u{256,4}), {}, {}), "reject colour replacement");
    auto tree = document.tree(); check(!tree.dissolve(id), "no ordinary ungroup");
    auto bad = document.tree(); bad.container(id)->channelRoles[1] = red;
    bool rejected = false; try { (void)document.replaceStructure(document.tree(), bad); } catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "duplicate roles invalid");
    std::vector<LayerTransformUpdate> transforms;
    for (auto role : group.channelRoles) transforms.push_back({role, document.layer(role)->localToDocument, {1,0,34,0,1,27}});
    check(document.setLayerTransforms(transforms), "whole-group transform");
    std::reverse(transforms.begin(), transforms.end());
    for (auto& t : transforms) std::swap(t.before, t.after);
    check(document.setLayerTransforms(transforms), "whole-group transform restore");
    check(history.execute(document, duplicateLayerItems(document, selected)), "duplicate group");
    const auto duplicateId = history.layerSelectionHint()->primary.value();
    check(duplicateId != id && bytes(*packChannelGroup(document, duplicateId)) == original, "duplicate exact");
    const auto duplicateRed = document.tree().container(duplicateId)->channelRoles[0];
    auto duplicateSurface = std::get<RasterLayer>(document.layer(duplicateRed)->payload).surface;
    std::array<std::byte,1> value{std::byte{77}};
    const RasterPatch patch{{0,0,1,1},value,1}; (void)duplicateSurface->replaceNativeBatch(std::span(&patch,1));
    check(bytes(*packChannelGroup(document,id)) == original, "duplicate edit independent");
    check(history.undo(document), "undo duplicate");
    check(history.execute(document, convertChannelGroupToRaster(document, selected)), "recombine");
    const auto surface = std::get<RasterLayer>(document.layer(id)->payload).surface;
    check(surface->dataKind() == RasterDataKind::PackedData && bytes(*surface) == original, "recombine exact data interpretation");
    check(document.layer(id)->rasterOrigin == layer.rasterOrigin && document.layer(id)->localToDocument == layer.localToDocument, "native frame retained");
    check(history.undo(document), "undo recombine");
    check(history.redo(document), "redo recombine");
    check(bytes(*std::get<RasterLayer>(document.layer(id)->payload).surface) == original, "redo exact");
}
void clipboard()
{
    Document document({.extent = {4,1}}); History history;
    const std::array<std::byte,4> sourceValues{std::byte{0},std::byte{128},std::byte{255},std::byte{64}};
    auto source = Layer::raster("Source", std::make_shared<ScalarRasterSurface>(Extent2u{4,1},sourceValues,4));
    auto destination = Layer::raster("Destination", std::make_shared<ScalarRasterSurface>(Extent2u{4,1}));
    const auto sourceId = source.id, targetId = destination.id;
    check(document.insertLayer(0,source) && document.insertLayer(1,destination), "clipboard layers");
    const std::array<std::uint8_t,4> coverage{255,128,128,0};
    const auto selection = SelectionMask::fromR8({4,1}, coverage,4);
    (void)document.setSelection(selection);
    const auto fragment = captureScalarFragment(document,sourceId);
    check(fragment && fragment->extent == Extent2u{3,1}, "trim to coverage");
    check(fragment->values[1] == std::byte{128} && fragment->coverage[1] == std::byte{128}, "unassociated values");
    check(pasteScalarFragment(document,targetId,*fragment,history) == RasterEditCommitResult::Committed, "paste");
    const auto& surface = *std::get<RasterLayer>(document.layer(targetId)->payload).surface;
    check(bytes(surface) == std::vector<std::byte>{std::byte{0},std::byte{64},std::byte{128},std::byte{0}}, "coverage applied once");
    check(document.selection() == selection, "unrelated selection preserved");
    check(bytes(*std::get<RasterLayer>(document.layer(sourceId)->payload).surface) == std::vector(sourceValues.begin(),sourceValues.end()), "source unchanged");
    check(history.undo(document) && bytes(surface) == std::vector<std::byte>(4), "paste undo");
    check(history.redo(document), "paste redo");
    auto malformed = *fragment; malformed.coverage.pop_back();
    check(!validScalarFragment(malformed), "invalid payload");
}
}
int main()
{
    try { conversionAndStructure(); clipboard(); std::cout << "Channel Group core tests passed\n"; }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
