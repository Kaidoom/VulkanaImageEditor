#include "imageeditor/ui/DiffusionTransaction.hpp"
#include "imageeditor/ui/DiffusionSpatial.hpp"
#include "imageeditor/core/LayerCommands.hpp"
#include "imageeditor/core/ColorMath.hpp"
#include <QGuiApplication>
#include <QColorSpace>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace c = imageeditor::core;
namespace u = imageeditor::ui;
namespace {
int failures = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { ++failures; std::cerr << __LINE__ << ": " #__VA_ARGS__ "\n"; } } while (false)
QImage image(int width, int height, c::Rgba8 color)
{
    QImage result(width, height, QImage::Format_RGBA8888);
    for (int y=0; y<height; ++y) for(int x=0; x<width; ++x) {
        auto* p=result.scanLine(y)+x*4;
        p[0]=color.red; p[1]=color.green; p[2]=color.blue; p[3]=color.alpha;
    }
    result.setColorSpace(QColorSpace::SRgb);
    return result;
}
QImage gray(int width,int height, unsigned char value)
{
    QImage result(width,height,QImage::Format_Grayscale8);result.fill(value);return result;
}
std::unique_ptr<c::Document> document(c::Layer* copy = nullptr)
{
    auto doc=std::make_unique<c::Document>(c::CanvasSpec{{8,6}});
    auto layer=c::Layer::raster("Source",std::make_shared<c::ContiguousRasterSurface>(c::Extent2u{4,3},c::Rgba8{80,140,220,128}));
    layer.localToDocument={1,0,2,0,1,1};
    if(copy)*copy=layer;
    CHECK(doc->insertLayer(0,std::move(layer)));
    return doc;
}
void captureAndStaleGuards()
{
    c::Layer layer;auto doc=document(&layer);
    auto target=u::captureDiffusionTarget(*doc,layer.id,false);
    CHECK(u::diffusionTargetCurrent(*doc,target));
    CHECK(target.originalPixels.size()==QSize(4,3));
    const auto original=target.originalPixels.copy();
    const std::array<std::byte,4> pixel{std::byte{1},std::byte{2},std::byte{3},std::byte{255}};
    auto surface=std::get<c::RasterLayer>(doc->layer(layer.id)->payload).surface;
    (void)surface->replaceRgba8({0,0,1,1},pixel,4);
    CHECK(target.originalPixels==original);
    QString error;
    CHECK(!u::diffusionTargetCurrent(*doc,target,&error));CHECK(error.contains("layer has changed"));
    CHECK(!u::diffusionReplaceCommand(*doc,target,original,false,false,&error));
    target=u::captureDiffusionTarget(*doc,layer.id,false);
    CHECK(doc->setSelection(c::SelectionMask::rectangle(doc->canvas().extent,{0,0,2,2})));
    CHECK(!u::diffusionTargetCurrent(*doc,target,&error));
    target=u::captureDiffusionTarget(*doc,layer.id,true);
    auto other=document();CHECK(!u::diffusionTargetCurrent(*other,target,&error));
    CHECK(doc->renameLayer(layer.id,"Changed"));CHECK(!u::diffusionTargetCurrent(*doc,target,&error));
    target=u::captureDiffusionTarget(*doc,layer.id,true);
    auto prepared=u::diffusionReplaceCommand(*doc,target,image(4,3,{220,20,20,255}),false,false,&error);
    CHECK(prepared);
    CHECK(doc->renameLayer(layer.id,"Changed after preparation"));
    c::History history;
    CHECK(!history.execute(*doc,std::move(prepared)));
    CHECK(!history.canUndo() && u::diffusionRawLayer(*doc->layer(layer.id))==target.originalPixels);
}
void selectionMappingAndSoftEdges()
{
    c::Layer layer;auto doc=document(&layer);
    std::array<std::uint8_t,48> selection{};
    selection[1*8+2]=255;selection[1*8+3]=128;selection[1*8+4]=1;
    CHECK(doc->setSelection(c::SelectionMask::fromR8({8,6},selection,8)));
    const auto target=u::captureDiffusionTarget(*doc,layer.id,true);
    const auto mask=u::diffusionSelectionMask(target);
    CHECK(mask.size()==QSize(4,3));CHECK(mask.constScanLine(0)[0]==255);
    CHECK(mask.constScanLine(0)[1]==128 && mask.constScanLine(0)[2]==1 && mask.constScanLine(0)[3]==0);
    const auto docMask=u::diffusionSelectionMask(target,{},true);
    CHECK(docMask.size()==QSize(8,6));CHECK(docMask.constScanLine(1)[3]==128);
    CHECK(u::diffusionSelectionMask(target,{16,12},true).size()==QSize(16,12));
    const auto result=u::diffusionResolvedPixels(target,image(4,3,{250,10,20,255}),false,true);
    CHECK(std::memcmp(result.constScanLine(0),image(4,3,{250,10,20,255}).constScanLine(0),4)==0);
    CHECK(result.constScanLine(0)[7]==192); // interpolated alpha at half coverage
    CHECK(result.constScanLine(0)[4]>140 && result.constScanLine(0)[4]<250);
    CHECK(std::memcmp(result.constScanLine(0)+12,target.originalPixels.constScanLine(0)+12,4)==0);
    CHECK(std::memcmp(result.constScanLine(1),target.originalPixels.constScanLine(1),16)==0);
    CHECK(doc->selection()==target.selection); // applying/previewing doesn't consume selection
}
void replacePreservesStateAndUndo()
{
    c::Layer layer;c::EditorSession session;session.replaceDocument(document(&layer));
    auto* doc=session.document();
    auto originalMask=std::make_shared<c::LayerMask>();
    originalMask->coverage=c::SelectionMask::filled({4,3},210);
    CHECK(doc->setLayerMask(layer.id,originalMask));
    CHECK(doc->setLayerOpacity(layer.id,.65F));
    CHECK(doc->setLayerBlendMode(layer.id,c::BlendMode::Multiply));
    CHECK(doc->setSelection(c::SelectionMask::rectangle({8,6},{2,1,2,2})));
    const auto target=u::captureDiffusionTarget(*doc,layer.id,true);
    const auto selection=doc->selection();const auto transform=doc->layer(layer.id)->localToDocument;
    const auto originalSurface=std::get<c::RasterLayer>(doc->layer(layer.id)->payload).surface;
    QString error;
    auto command=u::diffusionReplaceCommand(*doc,target,image(8,6,{200,30,40,255}),false,true,&error);
    CHECK(command && error.isEmpty());
    CHECK(!u::diffusionReplaceCommand(*doc,target,target.originalPixels,false,false,&error));
    CHECK(error.contains("does not change"));
    error.clear();
    CHECK(u::diffusionRawLayer(*doc->layer(layer.id))==target.originalPixels);
    CHECK(doc->revision()==target.documentRevision);
    CHECK(session.execute(std::move(command)));
    const auto changed=u::diffusionRawLayer(*doc->layer(layer.id));
    CHECK(changed!=target.originalPixels);
    CHECK(doc->layer(layer.id)->mask==originalMask && doc->layer(layer.id)->opacity==.65F);
    CHECK(doc->layer(layer.id)->localToDocument==transform && doc->layer(layer.id)->blendMode==c::BlendMode::Multiply);
    CHECK(doc->selection()==selection && session.history().undoDepth()==1);
    CHECK(session.undo());CHECK(std::get<c::RasterLayer>(doc->layer(layer.id)->payload).surface==originalSurface);
    CHECK(u::diffusionRawLayer(*doc->layer(layer.id))==target.originalPixels);
    CHECK(session.redo());CHECK(u::diffusionRawLayer(*doc->layer(layer.id))==changed);
    CHECK(doc->selection()==selection);
}
void transformedDocumentResults()
{
    c::Layer layer;auto doc=document(&layer);
    // A quarter turn maps native centres to exact document pixel centres.
    CHECK(doc->setLayerTransform(layer.id,{0,-1,5,1,0,1}));
    CHECK(doc->setSelection(c::SelectionMask::filled({8,6},255)));
    const auto target=u::captureDiffusionTarget(*doc,layer.id,true);
    auto generated=image(8,6,{0,0,0,255});
    for(int y=0;y<6;++y) for(int x=0;x<8;++x) {
        auto* p=generated.scanLine(y)+x*4;p[0]=static_cast<unsigned char>(x*30);p[1]=static_cast<unsigned char>(y*40);
    }
    const auto result=u::diffusionResolvedPixels(target,generated,true,true);
    CHECK(result.size()==QSize(4,3));
    CHECK(result.constScanLine(0)[0]==120 && result.constScanLine(0)[1]==40);
    CHECK(result.constScanLine(2)[3*4]==60 && result.constScanLine(2)[3*4+1]==160);
}
void regionalRasterOriginIsPreserved()
{
    c::Layer layer;c::EditorSession session;session.replaceDocument(document(&layer));
    auto* doc=session.document();
    const auto surface=std::get<c::RasterLayer>(doc->layer(layer.id)->payload).surface;
    CHECK(doc->setLayerRasterStorage(layer.id,surface,{-2,-1},{}));
    CHECK(doc->setLayerTransform(layer.id,{1,0,4,0,1,3}));
    CHECK(doc->setSelection(c::SelectionMask::rectangle({8,6},{2,2,1,1})));
    const auto target=u::captureDiffusionTarget(*doc,layer.id,true);
    const auto mask=u::diffusionSelectionMask(target);
    CHECK(mask.constScanLine(0)[0]==255 && mask.constScanLine(0)[1]==0);
    auto command=u::diffusionReplaceCommand(*doc,target,image(8,6,{250,80,10,255}),true,true);
    CHECK(command && session.execute(std::move(command)));
    CHECK(doc->layer(layer.id)->rasterOrigin==c::Vec2d{-2,-1});
    CHECK(doc->layer(layer.id)->localToDocument==target.localToDocument);
    CHECK(session.undo());
    CHECK(u::diffusionRawLayer(*doc->layer(layer.id))==target.originalPixels);
}
void newLayerFramesAndMask()
{
    c::Layer layer;auto doc=document(&layer);
    CHECK(doc->setSelection(c::SelectionMask::rectangle({8,6},{2,1,2,2},128)));
    auto target=u::captureDiffusionTarget(*doc,layer.id,true);
    QString error;
    auto result=u::diffusionResultLayer(&target,image(4,3,{30,70,90,255}),false,true,"AI edit",&error);
    CHECK(result && error.isEmpty());
    CHECK(result->localToDocument==target.localToDocument && result->rasterOrigin==target.rasterOrigin);
    const auto pixels=u::diffusionRawLayer(*result);
    CHECK(pixels.constScanLine(0)[3]==128 && pixels.constScanLine(0)[2*4+3]==0);
    auto full=u::diffusionResultLayer(&target,image(4,3,{30,70,90,255}),true,true,"AI fill",&error);
    CHECK(full && full->localToDocument==c::AffineTransform{});
    CHECK(u::diffusionRawLayer(*full).size()==QSize(8,6));
    CHECK(u::diffusionRawLayer(*full).constScanLine(0)[3]==0);
    auto generated=u::diffusionResultLayer(nullptr,image(5,2,{30,70,90,170}),false,false,"Generated",&error);
    CHECK(generated && u::diffusionRawLayer(*generated).size()==QSize(5,2));
    CHECK(u::diffusionRawLayer(*generated).constScanLine(0)[3]==170);
}
void fillLayerPreservesVisibleOutside()
{
    c::Layer layer;auto doc=document(&layer);
    const auto before=u::flattenDocument(*doc);
    CHECK(before);
    CHECK(doc->setSelection(c::SelectionMask::rectangle({8,6},{2,1,2,2},128)));
    const auto target=u::captureDiffusionTarget(*doc,layer.id,true);
    auto added=u::diffusionResultLayer(&target,image(8,6,{255,30,20,255}),true,true,"AI fill");
    CHECK(added && doc->insertLayer(doc->layers().size(),std::move(*added)));
    const auto after=u::flattenDocument(*doc);
    CHECK(after);
    bool changed=false;
    for(int y=0;y<6;++y) for(int x=0;x<8;++x) {
        const bool selected=x>=2 && x<4 && y>=1 && y<3;
        const bool equal=std::memcmp(before.image.constScanLine(y)+x*4,after.image.constScanLine(y)+x*4,4)==0;
        if(!selected)CHECK(equal);
        else changed|=!equal;
    }
    CHECK(changed);
}
void transparentFillPreviewMatchesNewLayer()
{
    for (const auto space : {c::CompositingSpace::LinearSRgb, c::CompositingSpace::EncodedSRgb}) {
        c::Layer layer;auto doc=document(&layer);
        auto canvas=doc->canvas();canvas.compositingSpace=space;
        (void)doc->setCanvas(canvas);
        CHECK(doc->setSelection(c::SelectionMask::rectangle({8,6},{2,1,2,2},128)));
        const auto target=u::captureDiffusionTarget(*doc,layer.id,true);
        const auto before=u::flattenDocument(*doc);
        CHECK(before);
        const auto generated=image(8,6,{255,10,40,0});
        const auto preview=u::diffusionFillPreview(target,before.image,generated,true);
        CHECK(preview==before.image); // A transparent new layer cannot erase its backdrop.
        const auto replacePreview=u::diffusionFillPreview(target,before.image,generated,false);
        CHECK(replacePreview.constScanLine(1)[2*4+3]<before.image.constScanLine(1)[2*4+3]);
        const auto translucent=image(8,6,{255,10,40,80});
        const auto translucentPreview=u::diffusionFillPreview(target,before.image,translucent,true);
        auto patch=u::diffusionResultLayer(&target,translucent,true,true,"AI fill");
        CHECK(patch && doc->insertLayer(doc->layers().size(),std::move(*patch)));
        const auto applied=u::flattenDocument(*doc);
        CHECK(applied && translucentPreview==applied.image);
        for (int y=0;y<6;++y) for(int x=0;x<8;++x) {
            if (x>=2 && x<4 && y>=1 && y<3) continue;
            CHECK(std::memcmp(translucentPreview.constScanLine(y)+x*4,before.image.constScanLine(y)+x*4,4)==0);
        }
    }
}
void transparentFillInputIsOnlyAnInferenceCopy()
{
    auto original = image(5, 2, {40, 80, 120, 0});
    original.setPixelColor(1, 0, QColor(70, 80, 90, 128));
    original.setPixelColor(2, 0, QColor(100, 120, 140, 255));
    auto mask = gray(5, 2, 0);
    mask.scanLine(0)[0] = 255;
    mask.scanLine(0)[1] = mask.scanLine(0)[2] = mask.scanLine(0)[3] = 128;
    const auto frozen = original.copy();
    const auto prepared = u::diffusionPrepareFillInput(original, mask);
    CHECK(original == frozen);
    CHECK(prepared.pixelColor(0, 0) == QColor(Qt::white));
    CHECK(prepared.pixelColor(3, 0) == QColor(Qt::white));
    CHECK(prepared.pixelColor(1, 0) == original.pixelColor(1, 0));
    CHECK(prepared.pixelColor(2, 0) == original.pixelColor(2, 0));
    CHECK(prepared.pixelColor(4, 0) == original.pixelColor(4, 0));
    CHECK(prepared.pixelColor(0, 1) == original.pixelColor(0, 1));
    CHECK(u::diffusionPrepareFillInput(original, gray(5, 2, 0)) == original);
    const auto opaque = image(5, 2, {40, 80, 120, 255});
    CHECK(u::diffusionPrepareFillInput(opaque, mask) == opaque);
    const auto applied = u::diffusionCompositeSelection(original,
        image(5, 2, {20, 170, 50, 255}), mask);
    CHECK(applied.pixelColor(4, 0) == original.pixelColor(4, 0));
    CHECK(applied.pixelColor(3, 0).alpha() == 128);
}
void canvasCroppedInputsAndPlacedReplacement()
{
    auto source = image(12, 8, {0, 200, 80, 255});
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 2; ++x) source.setPixelColor(x, y, Qt::red);
    const auto crop = u::diffusionCropToCanvas(source, QTransform::fromTranslate(-2, -1), QSize(8, 6));
    CHECK(crop.image.size() == QSize(8, 6));
    CHECK(crop.toDocument == QTransform{});
    CHECK(crop.image.pixelColor(0, 0) == QColor(0, 200, 80));
    CHECK(source.pixelColor(0, 0) == QColor(Qt::red));
    QTransform rotated; rotated.translate(8, 0); rotated.rotate(90);
    const auto rotatedCrop = u::diffusionCropToCanvas(image(6, 8, {10, 20, 30, 255}), rotated, QSize(8, 6));
    CHECK(rotatedCrop.image.size() == QSize(8, 6));
    CHECK(rotatedCrop.image.pixelColor(0, 0) == QColor(10, 20, 30));
    c::Layer layer; c::EditorSession session; session.replaceDocument(document(&layer));
    auto* doc = session.document();
    const auto unrelated = c::SelectionMask::rectangle({8, 6}, {0, 0, 1, 1});
    CHECK(doc->setSelection(unrelated));
    const auto target = u::captureDiffusionTarget(*doc, layer.id, false);
    auto command = u::diffusionReplacePlacedCommand(*doc, target,
        image(1, 1, {240, 100, 20, 255}), QTransform::fromTranslate(3, 2));
    CHECK(command && session.execute(std::move(command)));
    const auto after = u::diffusionRawLayer(*doc->layer(layer.id));
    CHECK(after.pixelColor(1, 1) == QColor(240, 100, 20));
    CHECK(after.pixelColor(0, 0) == target.originalPixels.pixelColor(0, 0));
    CHECK(doc->selection() == unrelated);
    CHECK(session.undo());
    CHECK(u::diffusionRawLayer(*doc->layer(layer.id)) == target.originalPixels);
    CHECK(session.redo());
    CHECK(u::diffusionRawLayer(*doc->layer(layer.id)) == after);
}
void renderedAppearanceDiffersFromRaw()
{
    c::Layer layer;auto doc=document(&layer);
    CHECK(doc->setLayerOpacity(layer.id,.5F));
    auto raw=u::diffusionRawLayer(*doc->layer(layer.id));
    auto rendered=u::diffusionRenderedLayer(*doc,layer.id);
    CHECK(rendered && rendered.image.format()==QImage::Format_RGBA8888);
    CHECK(raw.constScanLine(0)[3]==128);
    CHECK(rendered.image.constScanLine(0)[3]==64);
    CHECK(rendered.origin==c::Vec2d{2,1});
}
}
int main(int argc,char** argv)
{
    QGuiApplication app(argc,argv);
    captureAndStaleGuards();selectionMappingAndSoftEdges();
    replacePreservesStateAndUndo();transformedDocumentResults();regionalRasterOriginIsPreserved();newLayerFramesAndMask();fillLayerPreservesVisibleOutside();transparentFillPreviewMatchesNewLayer();transparentFillInputIsOnlyAnInferenceCopy();canvasCroppedInputsAndPlacedReplacement();renderedAppearanceDiffersFromRaw();
    std::cout << (failures?"FAIL":"PASS") << " diffusion transaction checks: " << failures << " failures\n";
    return failures?1:0;
}
