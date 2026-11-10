#include "imageeditor/core/BlendCompositing.hpp"
#include "imageeditor/core/ChannelGroups.hpp"
#include "imageeditor/core/ColorSampler.hpp"
#include "imageeditor/core/History.hpp"
#include "imageeditor/core/ScalarRasterSurface.hpp"

#include <array>
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {
using namespace imageeditor::core;
int failures=0;
void check(bool value,const char* expression,int line)
{if(!value){std::cerr<<"FAIL "<<line<<": "<<expression<<'\n';++failures;}}
#define CHECK(...) check(bool((__VA_ARGS__)),#__VA_ARGS__,__LINE__)

LayerId channels(Document& doc,std::vector<std::byte> bytes,Extent2u extent)
{
    auto layer=Layer::raster("Packed",std::make_shared<ContiguousRasterSurface>(extent,std::move(bytes)));
    const auto id=layer.id;CHECK(doc.insertLayer(0,std::move(layer)));
    History history;auto command=convertToChannelGroup(doc,LayerSelectionState{{id},id,id});
    CHECK(command);if(!command)return 0;
    CHECK(history.execute(doc,std::move(command)));
    for(const auto& c:doc.tree().containers)if(c.kind==ContainerKind::ChannelGroup)return c.id;
    return 0;
}

void packingUsesRolesAndRetainsAllByteValues()
{
    Document doc({{256,2},96});
    std::vector<std::byte> source(256*2*4);
    for(int y=0;y<2;++y)for(int x=0;x<256;++x) {
        const auto i=std::size_t(y*256+x)*4;
        source[i]=std::byte(x);source[i+1]=std::byte(255-x);source[i+2]=std::byte((x*73)%256);
        source[i+3]=std::byte(y?255:0);
    }
    const auto group=channels(doc,source,{256,2});CHECK(group);
    PinnedDocumentSampler sampler(doc,{},ColorSampleSource::MergedVisible);
    std::array<PremultipliedColor,256> row;
    sampler.sampleRow(0,1,row);
    for(int x=0;x<256;++x) {
        const Rgba8 expected{std::uint8_t(x),std::uint8_t(255-x),std::uint8_t((x*73)%256),255};
        CHECK(sampler.sample({x+.5,1.5})==expected);
        CHECK(sampleDocumentColor(doc,{},Vec2d{x+.5,1.5},ColorSampleSource::MergedVisible).color==expected);
        CHECK(encodeColor(row[std::size_t(x)])==expected);
        CHECK(sampler.sample({x+.5,.5})==Rgba8{});
    }
    const auto roles=doc.tree().container(group)->channelRoles;
    CHECK(doc.renameLayer(roles[0],"Roughness"));
    auto tree=doc.tree();std::reverse(tree.container(group)->children.begin(),tree.container(group)->children.end());
    CHECK(doc.replaceStructure(doc.tree(),tree));
    PinnedDocumentSampler reordered(doc,{},ColorSampleSource::MergedVisible);
    CHECK(reordered.sample({91.5,1.5})==sampler.sample({91.5,1.5}));
    PinnedDocumentSampler red(doc,roles[0],ColorSampleSource::ActiveLayer);
    CHECK(red.sample({91.5,.5})==(Rgba8{91,91,91,255}));
    PinnedDocumentSampler alpha(doc,roles[3],ColorSampleSource::ActiveLayer);
    CHECK(alpha.sample({91.5,.5})==(Rgba8{0,0,0,255}));
}

void dataInterpolationIsNumericBeforeDisplay()
{
    const std::array<std::byte,2> values{std::byte{0},std::byte{255}};
    auto layer=Layer::raster("Scalar",std::make_shared<ScalarRasterSurface>(Extent2u{2,1},values,2));
    PreparedLayerSampler scalar(layer,false);
    CHECK(encodeColor(scalar.sample({1,.5}))==(Rgba8{128,128,128,255}));

    const std::vector<std::byte> hidden{std::byte{255},std::byte{255},std::byte{255},std::byte{0},
        std::byte{0},std::byte{0},std::byte{0},std::byte{255}};
    auto data=Layer::raster("Packed",std::make_shared<ContiguousRasterSurface>(Extent2u{2,1},hidden,RasterDataKind::PackedData));
    PreparedLayerSampler packed(data,false);
    CHECK(encodeColor(packed.sample({1,.5}))==(Rgba8{128,128,128,128}));
    auto colour=Layer::raster("Colour",std::make_shared<ContiguousRasterSurface>(Extent2u{2,1},hidden));
    CHECK(encodeColor(PreparedLayerSampler(colour,false).sample({1,.5}))==(Rgba8{0,0,0,128}));
}

void transformedGroupAndRowsAgree()
{
    Document doc({{16,8},96});
    std::vector<std::byte> pixels(8*4*4);
    for(std::size_t i=0;i<32;++i) {
        pixels[i*4]=std::byte((i*7)%256);pixels[i*4+1]=std::byte(211);
        pixels[i*4+2]=std::byte(41);pixels[i*4+3]=std::byte((i*23)%256);
    }
    const auto group=channels(doc,std::move(pixels),{8,4});
    std::vector<LayerTransformUpdate> updates;
    for(const auto id:doc.tree().container(group)->children)
        updates.push_back({id,doc.layer(id)->localToDocument,{1.5,.1,1.25,0,1.4,.25}});
    CHECK(doc.setLayerTransforms(updates));
    PinnedDocumentSampler sampler(doc,{},ColorSampleSource::MergedVisible);
    std::array<PremultipliedColor,16> row;
    for(int y=0;y<8;++y) {
        sampler.sampleRow(0,y,row);
        for(int x=0;x<16;++x)CHECK(encodeColor(row[std::size_t(x)])==sampler.sample({x+.5,y+.5}));
    }
}
}
int main()
{
    try {packingUsesRolesAndRetainsAllByteValues();dataInterpolationIsNumericBeforeDisplay();transformedGroupAndRowsAgree();}
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return EXIT_FAILURE;}
    return failures?EXIT_FAILURE:EXIT_SUCCESS;
}
