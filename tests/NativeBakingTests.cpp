#include "imageeditor/ui/NativeRasterBake.hpp"
#include "imageeditor/ui/PixelPreview.hpp"
#include "imageeditor/ui/ProjectFile.hpp"
#include "imageeditor/ui/QtShapeRenderService.hpp"
#include "imageeditor/ui/QtTextLayoutService.hpp"
#include "imageeditor/core/ColorSampler.hpp"
#include "imageeditor/core/EditorSession.hpp"
#include "imageeditor/core/LayerCommands.hpp"
#include "imageeditor/core/RichText.hpp"
#include "imageeditor/core/SpatialFilterCache.hpp"
#include <QApplication>
#include <QBuffer>
#include <QElapsedTimer>
#include <QFontDatabase>
#include <QTemporaryDir>
#include <QThread>
#include <algorithm>
#include <iostream>
#include <iomanip>

namespace c=imageeditor::core;
namespace u=imageeditor::ui;
namespace {
int failures=0;
#define CHECK(...) do{if(!(__VA_ARGS__)){++failures;std::cerr<<"FAIL "<<__LINE__<<": "<<#__VA_ARGS__<<'\n';}}while(false)
c::Layer raster(std::string name,c::Extent2u size,c::Rgba8 color)
{return c::Layer::raster(std::move(name),std::make_shared<c::ContiguousRasterSurface>(size,color));}
c::LayerId add(c::Document& d,c::Layer l){const auto id=l.id;CHECK(d.insertLayer(d.layers().size(),std::move(l)));return id;}
QImage image(const c::RasterSurface& surface)
{
    const auto e=surface.extent();QImage result(int(e.width),int(e.height),QImage::Format_RGBA8888);
    surface.copyRgba8({0,0,int(e.width),int(e.height)},
        {reinterpret_cast<std::byte*>(result.bits()),std::size_t(result.sizeInBytes())},std::size_t(result.bytesPerLine()));return result;
}
QImage png(const QImage& source)
{QByteArray bytes;QBuffer b(&bytes);CHECK(b.open(QIODevice::WriteOnly));CHECK(source.save(&b,"PNG"));return QImage::fromData(bytes,"PNG").convertToFormat(QImage::Format_RGBA8888);}
bool near(const QImage& a,const QImage& b,int tolerance=1)
{
    if(a.size()!=b.size())return false;
    for(int y=0;y<a.height();++y)for(int i=0;i<a.width()*4;++i)
        if(std::abs(int(a.constScanLine(y)[i])-int(b.constScanLine(y)[i]))>tolerance)return false;
    return true;
}
bool wait(const std::function<bool()>& condition,int timeout=20000)
{QElapsedTimer timer;timer.start();while(!condition() && timer.elapsed()<timeout){QApplication::processEvents();QThread::msleep(1);}return condition();}
constexpr std::size_t budget=256ULL*1024*1024;

void isolatedModes()
{
    for(auto mode:c::allBlendModes)for(bool disconnected:{false,true}) {
        c::Document doc({{32,24},96});
        const auto background=add(doc,raster("Unselected background",{32,24},{120,210,80,200}));
        auto low=raster("Selected Normal",{9,7},{140,60,180,120});low.opacity=.63F;
        low.localToDocument.m02=-3;low.localToDocument.m12=2;
        const auto lower=add(doc,low);
        const auto middle=disconnected?add(doc,raster("Unselected interleaved",{32,24},{250,40,30,255})):c::LayerId{};
        auto top=raster("Selected mode",{5,6},{240,130,20,150});top.blendMode=mode;top.opacity=.57F;
        top.localToDocument.m02=disconnected?15:2;top.localToDocument.m12=3;
        const auto upper=add(doc,top);const std::array ids{upper,lower};
        const auto baked=u::flattenLayerItems(doc,ids);CHECK(baked);if(!baked)continue;
        const c::PreparedLayerSampler a(low),b(top);
        // Explicit selected-only source-over accumulation, beginning at alpha
        // zero. The full scene and the merge API do not construct this oracle.
        QImage reference(baked.image.size(),QImage::Format_RGBA8888);
        for(int y=0;y<reference.height();++y)for(int x=0;x<reference.width();++x) {
            const c::Vec2d point{baked.origin.x+x+.5,baked.origin.y+y+.5};
            auto value=c::compositeLayer({},a.sample(point),low.opacity,low.blendMode);
            value=c::compositeLayer(value,b.sample(point),top.opacity,top.blendMode,top.localToDocument.inverted()->map(point),top.blendSeed);
            auto color=c::encodeColor(value);if(color.alpha==0)color={};
            auto* pixel=reference.scanLine(y)+4*x;pixel[0]=color.red;pixel[1]=color.green;pixel[2]=color.blue;pixel[3]=color.alpha;
        }
        CHECK(png(baked.image)==png(reference));
        auto bg=std::get<c::RasterLayer>(doc.layer(background)->payload).surface;
        const std::array changed{std::byte(0),std::byte(0),std::byte(255),std::byte(255)};
        (void)bg->replaceRgba8({0,0,1,1},changed,4);
        CHECK(doc.setLayerVisibility(background,false));if(middle)CHECK(doc.setLayerOpacity(middle,.13F));
        CHECK(u::flattenLayerItems(doc,ids).image==baked.image);
        CHECK(doc.takeLayer(background).has_value());if(middle)CHECK(doc.takeLayer(middle).has_value());
        CHECK(u::flattenLayerItems(doc,ids).image==baked.image);
        // A lone source over transparency is source-over, NOT B(black,source).
        const auto alone=u::flattenLayerItems(doc,std::array{upper});CHECK(alone);
        if(alone) {const auto aloneExpected=c::compositeLayer({},c::decodeColor({240,130,20,150}),top.opacity,mode,
            top.localToDocument.inverted()->map(alone.origin+c::Vec2d{.5,.5}),top.blendSeed);
        CHECK(alone.image.constScanLine(0)[3]==c::alphaToByte(aloneExpected[3]));}
    }
}

void rasterizeHistoryAndEffects(const std::string& family)
{
    auto document=std::make_unique<c::Document>(c::CanvasSpec{{96,80},144});auto& doc=*document;
    auto down=raster("Downscaled original",{80,60},{160,50,210,150});
    down.localToDocument={.25,0,-4,0,.25,5};down.opacity=.45F;down.blendMode=c::BlendMode::ColorDodge;down.colorLabel=3;
    const auto lower=add(doc,down);
    const auto intervening=add(doc,raster("Unselected",{96,80},{50,140,190,255}));
    c::ShapeLayer shape;shape.kind=c::ShapeKind::RoundedRectangle;shape.size={24,17};shape.cornerRadius=4;
    shape.fillColor={220,180,90,170};shape.strokeEnabled=true;shape.strokeWidth=3;
    auto s=c::Layer::shape("Shape",shape);s.localToDocument={.9,-.2,20,.2,.9,-4};s.opacity=.71F;s.blendMode=c::BlendMode::Multiply;
    auto filters=std::make_shared<c::SpatialFilterStack>();filters->items[0].enabled=true;filters->items[0].parameters=c::GaussianBlurParameters{2,3};s.filters=filters;
    auto adjustments=std::make_shared<c::AdjustmentStack>();adjustments->items[0].enabled=true;adjustments->items[0].parameters=c::ExposureParameters{.5};s.adjustments=adjustments;
    const auto shaped=add(doc,s);
    c::TextLayer text;text.utf8="Editable ffi\ntext";text.defaultStyle.font.family=family;text.defaultStyle.sizePixels=16;text.defaultStyle.color={220,50,20,160};
    auto t=c::Layer::text("Text",c::normalizedText(text));t.localToDocument={-.8,.1,69,.1,.8,35};t.opacity=.37F;t.blendMode=c::BlendMode::Screen;t.visible=false;
    const auto typed=add(doc,t);
    auto styles=std::make_shared<c::LayerEffectStack>();styles->items[5].enabled=true;styles->items[5].color={40,150,210,130};
    for(auto id:{lower,shaped,typed})doc.setLayerEffects(id,styles);
    const auto folder=c::makeLayerId();auto tree=doc.tree();tree.roots={lower,intervening,folder};
    tree.containers={{folder,"Preserved folder",c::ContainerKind::Folder,c::ColorLabel::Blue,{shaped,typed}}};
    CHECK(doc.replaceStructure(doc.tree(),std::move(tree),{},{}));
    c::EditorSession session;session.replaceDocument(std::move(document));
    session.setLayerSelection(std::array{typed,lower,folder,shaped},lower,typed);
    const auto selected=session.layerSelectionState();const auto originalTree=doc.tree();
    const auto originals=doc.layers();const auto revision=doc.revision();doc.markSaved();
    const auto before=u::flattenDocument(doc);CHECK(before);
    auto failed=u::prepareRasterizeLayers(doc,selected,1);CHECK(!failed.error.isEmpty() && !failed.command && doc.revision()==revision);
    auto cancel=u::prepareRasterizeLayers(doc,selected,budget,[](auto,auto){return false;});CHECK(cancel.cancelled && !cancel.command && doc.revision()==revision);
    auto prepared=u::prepareRasterizeLayers(doc,selected,budget);CHECK(prepared.command && prepared.error.isEmpty());
    CHECK(doc.revision()==revision && !doc.isModified());
    if(!prepared.command)return;
    CHECK(session.execute(std::move(prepared.command)));CHECK(session.history().undoDepth()==1 && doc.isModified());
    CHECK(doc.tree()==originalTree && session.layerSelectionState()==selected);
    for(auto id:{lower,shaped,typed}) {
        const auto* l=doc.layer(id);CHECK(l && std::holds_alternative<c::RasterLayer>(l->payload));
        const auto& old=*std::ranges::find(originals,id,&c::Layer::id);
        CHECK(l->name==old.name && l->opacity==old.opacity && l->blendMode==old.blendMode
            && l->visible==old.visible && l->colorLabel==old.colorLabel);
        CHECK(!u::needsRasterization(*l));
        CHECK(l->localToDocument.m00==1 && l->localToDocument.m11==1 && l->localToDocument.m01==0 && l->localToDocument.m10==0);
    }
    CHECK(std::get<c::RasterLayer>(doc.layer(lower)->payload).surface->extent()==c::Extent2u(20,15));
    const auto after=u::flattenDocument(doc);CHECK(after && near(png(after.image),png(before.image),2));
    QTemporaryDir dir;const auto path=dir.filePath("rasterized.vulkana");CHECK(u::saveProject(path,doc));
    auto loaded=u::loadProject(path);CHECK(loaded && loaded.document->tree()==originalTree);
    if(loaded){CHECK(png(u::flattenDocument(*loaded.document).image)==png(after.image));for(auto id:{lower,shaped,typed})CHECK(std::holds_alternative<c::RasterLayer>(loaded.document->layer(id)->payload));}
    auto noop=u::prepareRasterizeLayers(doc,selected,budget);CHECK(!noop.command && noop.error.isEmpty());
    CHECK(session.undo());CHECK(!doc.isModified() && session.layerSelectionState()==selected && doc.tree()==originalTree);
    for(const auto& old:originals) {
        const auto* l=doc.layer(old.id);CHECK(l && l->localToDocument==old.localToDocument && l->payload.index()==old.payload.index());
        CHECK(l->filters==old.filters && l->adjustments==old.adjustments && l->crop==old.crop && l->effects==old.effects);
        if(auto* r=std::get_if<c::RasterLayer>(&old.payload))CHECK(std::get<c::RasterLayer>(l->payload).surface==r->surface);
        if(auto* v=std::get_if<c::TextLayer>(&old.payload))CHECK(std::get<c::TextLayer>(l->payload)==*v);
        if(auto* v=std::get_if<c::ShapeLayer>(&old.payload))CHECK(std::get<c::ShapeLayer>(l->payload)==*v);
    }
    const auto redo=session.history().redoDepth();
    auto unchanged=u::prepareRasterizeLayers(doc,c::LayerSelectionState{{intervening},intervening,intervening},budget);
    CHECK(!unchanged.command && unchanged.error.isEmpty() && session.history().redoDepth()==redo);
    CHECK(session.redo());CHECK(png(u::flattenDocument(doc).image)==png(after.image));
}

void previewCacheAndCancellation(const std::string& family)
{
    c::Document doc({{96,80},96});const auto base=add(doc,raster("Raster",{96,80},{90,30,170,140}));
    c::TextLayer text;text.utf8="Pixel ffi";text.defaultStyle.font.family=family;text.defaultStyle.sizePixels=18;
    auto t=c::Layer::text("Text",c::normalizedText(text));t.localToDocument={.8,.2,9,-.2,.8,20};t.blendMode=c::BlendMode::ColorDodge;add(doc,t);
    c::ShapeLayer shape;shape.kind=c::ShapeKind::Ellipse;shape.size={24,18};shape.fillColor={220,130,20,160};
    auto s=c::Layer::shape("Shape",shape);s.localToDocument.m02=30;s.opacity=.68F;add(doc,s);
    auto effects=std::make_shared<c::LayerEffectStack>();effects->items[0].enabled=true;effects->items[0].size=3;
    effects->items[6].enabled=true;effects->items[6].secondColor={230,190,20,100};doc.setLayerEffects(base,effects);
    const auto expected=png(u::flattenDocument(doc).image);const auto revision=doc.revision(),state=doc.contentState();
    const auto surface=std::get<c::RasterLayer>(doc.layer(base)->payload).surface;
    u::PixelPreview preview;QString error;std::shared_ptr<const c::RasterSurface> output;
    preview.onReady=[&](auto value,QString why){output=std::move(value);error=why;};
    preview.setEnabled(true);preview.request(doc.snapshot());CHECK(wait([&]{return !preview.busy();}));
    CHECK(error.isEmpty() && output);if(output)CHECK(png(image(*output))==expected);
    CHECK(doc.revision()==revision && doc.contentState()==state && surface==std::get<c::RasterLayer>(doc.layer(base)->payload).surface);
    const auto count=preview.completedRenders(),sourceBytes=preview.frozenBytes();auto cached=output;
    for(c::Revision i=0;i<10;++i){auto snapshot=doc.snapshot();snapshot.documentRevision+=i;snapshot.selectionRevision+=i;
        snapshot.layersBottomToTop.front().name="Metadata only";preview.request(snapshot);}
    QApplication::processEvents();CHECK(!preview.busy() && preview.completedRenders()==count && output==cached);
    CHECK(preview.frozenBytes()==sourceBytes);
    CHECK(doc.setLayerOpacity(base,.27F));preview.request(doc.snapshot());CHECK(wait([&]{return !preview.busy();}));
    CHECK(output && png(image(*output))==png(u::flattenDocument(doc).image));CHECK(preview.completedRenders()==count+1);
    // Coalesced changes and cancellation never publish an obsolete generation.
    for(int i=0;i<20;++i){CHECK(doc.setLayerOpacity(base,.3F+float(i)*.01F));preview.request(doc.snapshot());}
    preview.setEnabled(false);CHECK(wait([&]{return !preview.busy();}));CHECK(!output);
    preview.setEnabled(true);preview.request(doc.snapshot());CHECK(wait([&]{return !preview.busy();}));
    CHECK(output && png(image(*output))==png(u::flattenDocument(doc).image));
    // Mutating source bytes must invalidate even if the document revision did
    // not change (live brush transactions advance the surface revision).
    const std::array pixel{std::byte(255),std::byte(0),std::byte(0),std::byte(128)};
    (void)surface->replaceRgba8({0,0,1,1},pixel,4);preview.request(doc.snapshot());
    CHECK(wait([&]{return !preview.busy();}));CHECK(output && png(image(*output))==png(u::flattenDocument(doc).image));
    const auto completed=preview.completedRenders();
    c::Document large({{2048,2048},96});add(large,raster("Slow native composite",{2048,2048},{170,30,90,130}));
    preview.request(large.snapshot());CHECK(wait([&]{return preview.frozenBytes()>8ULL*1024*1024;}));
    // Replace the target while the worker is evaluating immutable old pixels.
    // The old job may finish, but cannot publish into this new document view.
    preview.request(doc.snapshot());CHECK(wait([&]{return !preview.busy();}));
    CHECK(preview.completedRenders()==completed); // Hot native pixels reuse the content-keyed cache.
    CHECK(output && png(image(*output))==png(u::flattenDocument(doc).image));
    // Filtered raster caches are canonical and can be re-keyed to frozen
    // source pixels; typed display caches must never supply the native bake.
    auto adjustments=std::make_shared<c::AdjustmentStack>();
    adjustments->items[0].enabled=true;adjustments->items[0].parameters=c::ExposureParameters{.7};
    adjustments->items[0].mask=c::captureAdjustmentMask(c::SelectionMask::rectangle({96,80},{5,4,42,36},150),{});
    auto filters=std::make_shared<c::SpatialFilterStack>();filters->items[0].enabled=true;
    filters->items[0].parameters=c::GaussianBlurParameters{2,3};
    CHECK(doc.setLayerAdjustments(base,adjustments));CHECK(doc.setLayerFilters(base,filters));
    doc.layer(base)->filterCache=c::prepareLayerSpatialFilters(*doc.layer(base));
    CHECK(c::layerSpatialFilterCacheValid(*doc.layer(base)));
    CHECK(doc.setLayerFilters(t.id,filters));
    doc.layer(t.id)->renderCache=u::QtTextLayout(std::get<c::TextLayer>(doc.layer(t.id)->payload)).rasterize(.5);
    c::LayerCrop crop{3,2,80,70};crop.corners={4,2,3,1};CHECK(doc.setLayerCrop(base,crop));
    preview.request(doc.snapshot());CHECK(wait([&]{return !preview.busy();}));
    CHECK(error.isEmpty() && output && png(image(*output))==png(u::flattenDocument(doc).image));
}

void rasterizeMasksCropAndStaleTargets()
{
    auto document=std::make_unique<c::Document>(c::CanvasSpec{{40,32},96});auto& doc=*document;
    auto source=raster("Cropped adjusted",{32,24},{150,80,210,170});
    source.localToDocument={.8,-.2,-5,.2,.8,2};source.opacity=.3F;source.blendMode=c::BlendMode::ColorBurn;
    source.crop=c::LayerCrop{2,3,20,16};source.crop->corners={3,2,1,4};
    auto adjustments=std::make_shared<c::AdjustmentStack>();
    adjustments->items[0].enabled=true;adjustments->items[0].parameters=c::ExposureParameters{1};
    adjustments->items[0].mask=c::captureAdjustmentMask(c::SelectionMask::rectangle({40,32},{0,0,12,28},130),source.localToDocument);
    // Disabled settings weren't evaluated; keep their mask's document location.
    adjustments->items[1].mask=adjustments->items[0].mask;
    source.adjustments=adjustments;
    const auto first=add(doc,source);
    auto secondLayer=raster("Second",{16,16},{90,40,210,100});secondLayer.localToDocument.m02=.4;
    const auto second=add(doc,secondLayer);
    c::EditorSession session;session.replaceDocument(std::move(document));session.setLayerSelection(std::array{first,second},first,second);
    const auto selection=session.layerSelectionState();const auto before=u::flattenDocument(doc);
    const auto previous=doc.layer(first)->localToDocument;
    auto bake=u::prepareRasterizeLayers(doc,selection,budget);CHECK(bake.command && bake.error.isEmpty());
    if(bake.command) {
        CHECK(session.execute(std::move(bake.command)));
        const auto* layer=doc.layer(first);CHECK(!layer->crop && !c::compileAdjustmentStack(layer->adjustments).active);
        CHECK(layer->adjustments && layer->adjustments->items[1].mask.has_value());
        const auto& mask=*layer->adjustments->items[1].mask;
        CHECK(mask.coverage==adjustments->items[1].mask->coverage);
        const auto docPoint=layer->localToDocument.map({3,4});
        CHECK(std::abs(mask.localToMask.map({3,4}).x-docPoint.x)<1e-8);
        CHECK(std::abs(mask.localToMask.map({3,4}).y-docPoint.y)<1e-8);
        CHECK(near(png(u::flattenDocument(doc).image),png(before.image),2));
        CHECK(session.undo());CHECK(doc.layer(first)->localToDocument==previous && doc.layer(first)->crop==source.crop);
    }
    const auto depth=session.history().undoDepth(),redo=session.history().redoDepth();
    bool changed=false;auto original=std::get<c::RasterLayer>(doc.layer(first)->payload).surface;
    const auto stale=u::prepareRasterizeLayers(doc,selection,budget,[&](auto done,auto){
        if(done>1000 && !changed) {
            const std::array px{std::byte(255),std::byte(255),std::byte(255),std::byte(255)};
            (void)original->replaceRgba8({3,3,1,1},px,4);changed=true;
        }
        return true;
    });
    CHECK(changed && !stale.command && stale.error.contains("changed"));
    CHECK(session.history().undoDepth()==depth && session.history().redoDepth()==redo);
    CHECK(std::get<c::RasterLayer>(doc.layer(first)->payload).surface==original && doc.layer(first)->crop==source.crop);
    // A fully cropped layer and an empty text layer become ordinary transparent
    // rasters, instead of retaining invisible session-only originals.
    CHECK(doc.setLayerCrop(first,c::LayerCrop{0,0,0,0}));
    auto empty=u::rasterizeLayerContent(doc,first);CHECK(empty && empty.image.size()==QSize(1,1) && empty.image.constBits()[3]==0);
    auto text=c::Layer::text("Empty text",{});text.localToDocument.m02=-7.3;
    const auto id=add(doc,text);auto emptyText=u::rasterizeLayerContent(doc,id);CHECK(emptyText && emptyText.image.size()==QSize(1,1));
}

void benchmark()
{
    std::cout<<std::fixed<<std::setprecision(3);
    for(const auto size:{c::Extent2u{3840,2160},c::Extent2u{5120,2880}}) {
        c::Document doc({size,96});const auto low=add(doc,raster("Lower",size,{220,50,90,170}));
        const auto high=add(doc,raster("Upper",size,{50,110,210,140}));
        u::PixelPreview preview;QString error;std::shared_ptr<const c::RasterSurface> output;
        preview.onReady=[&](auto result,QString why){output=std::move(result);error=why;};
        QElapsedTimer timer;timer.start();preview.setEnabled(true);preview.request(doc.snapshot());
        CHECK(wait([&]{return !preview.busy();},60000));CHECK(output && error.isEmpty());
        auto p=preview.lastProfile();
        std::cout<<"PIXEL_PREVIEW "<<size.width<<'x'<<size.height<<" latency_ms="<<double(timer.nsecsElapsed())/1e6
            <<" freeze_ms="<<p.freezeMs<<" sample_blend_ms="<<p.evaluation.samplingBlendMs
            <<" encode_ms="<<p.evaluation.encodingMs<<" transfer_ms="<<p.transferMs
            <<" frozen_bytes="<<preview.frozenBytes()<<'\n';
        const auto rendered=preview.completedRenders();auto stable=output;
        for(int i=0;i<100;++i)preview.request(doc.snapshot());
        CHECK(!preview.busy() && preview.completedRenders()==rendered && output==stable);
        stable.reset();
        CHECK(doc.setLayerOpacity(high,.8F));preview.request(doc.snapshot());CHECK(wait([&]{return !preview.busy();},60000));
        p=preview.lastProfile();std::cout<<"PREVIEW_REUSE freeze_ms="<<p.freezeMs<<" redundant_view_renders="<<0<<'\n';
        preview.setEnabled(false);output.reset();
        CHECK(doc.setLayerTransform(low,{.5,0,-5,0,.5,3}));timer.restart();
        auto bake=u::prepareRasterizeLayers(doc,c::LayerSelectionState{{low},low,low},budget);CHECK(bake.command && bake.error.isEmpty());
        std::cout<<"RASTERIZE "<<size.width<<'x'<<size.height<<" to_half_scale_ms="<<double(timer.nsecsElapsed())/1e6
            <<" history_bytes="<<(bake.command?bake.command->memoryCost():0)<<'\n';
    }
}
}
int main(int argc,char** argv)
{
    QApplication app(argc,argv);
    try {
        if(argc>1 && std::string_view(argv[1])=="--benchmark"){benchmark();return failures?1:0;}
        const auto font=QFontDatabase::addApplicationFont(QStringLiteral(IMAGEEDITOR_BAKING_FONT_DIR "/NotoSans-Regular.ttf"));
        CHECK(font>=0);const auto families=QFontDatabase::applicationFontFamilies(font);if(families.isEmpty())return 1;
        isolatedModes();rasterizeHistoryAndEffects(families.front().toStdString());previewCacheAndCancellation(families.front().toStdString());
        rasterizeMasksCropAndStaleTargets();
    }catch(const std::exception& e){++failures;std::cerr<<e.what()<<'\n';}
    if(!failures)std::cout<<"Isolated merge, per-layer rasterization and native Pixel Preview passed\n";
    return failures?1:0;
}
