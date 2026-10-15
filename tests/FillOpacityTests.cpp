#include "imageeditor/core/LayerCommands.hpp"
#include "imageeditor/core/SelectionCommands.hpp"
#include "imageeditor/core/LayerEffects.hpp"
#include "imageeditor/core/ColorSampler.hpp"
#include "imageeditor/core/SpatialFilterCache.hpp"
#include "imageeditor/ui/FlattenedDocument.hpp"
#include "imageeditor/ui/ProjectFile.hpp"
#include "imageeditor/ui/NativeRasterBake.hpp"
#include "imageeditor/ui/PsdImport.hpp"
#include "imageeditor/ui/PsdExport.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/CompactValueControl.hpp"
#include "imageeditor/ui/Theme.hpp"
#include <QApplication>
#include <QSettings>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTabBar>
#include <QTest>
#include <cmath>
#include <iostream>

namespace c=imageeditor::core;
namespace u=imageeditor::ui;
namespace {
int failures=0;
#define CHECK(x) do { if(!(x)){++failures;std::cerr<<"FAIL "<<__LINE__<<": "<<#x<<'\n';} }while(false)
bool close(c::PremultipliedColor a,c::PremultipliedColor b,float tolerance=1e-6F)
{for(std::size_t i=0;i<4;++i)if(std::abs(a[i]-b[i])>tolerance)return false;return true;}
c::Layer cutout()
{
    auto surface=std::make_shared<c::ContiguousRasterSurface>(c::Extent2u{24,24},c::Rgba8{200,80,30,255});
    auto layer=c::Layer::raster("Cutout",surface);layer.localToDocument.m02=8;layer.localToDocument.m12=8;
    return layer;
}
void blendMath()
{
    const auto b=c::decodeColor({153,153,153,255}),s=c::decodeColor({204,204,204,255});
    const std::array special{c::BlendMode::ColorDodge,c::BlendMode::ColorBurn,c::BlendMode::LinearDodge,
        c::BlendMode::LinearBurn,c::BlendMode::Difference,c::BlendMode::VividLight,c::BlendMode::LinearLight,c::BlendMode::HardMix};
    const std::array expected{.75,.578947368,.8,.55,.4,.705882353,.75,.733333333};
    for(std::size_t i=0;i<special.size();++i) {
        const auto fill=c::compositeLayer(b,s,1,special[i],{},c::defaultBlendSeed,false,c::CompositingSpace::EncodedSRgb,.25F);
        const auto opacity=c::compositeLayer(b,s,.25F,special[i],{},c::defaultBlendSeed,false,c::CompositingSpace::EncodedSRgb);
        CHECK(std::abs(int(c::encodeColor(fill).red)-int(std::lround(expected[i]*255)))<=1);
        if(special[i]!=c::BlendMode::LinearBurn)CHECK(!close(fill,opacity));
    }
    const auto dark=c::decodeColor({25,25,25,255}),low=c::decodeColor({51,51,51,255});
    CHECK(!close(c::compositeLayer(dark,low,1,c::BlendMode::LinearBurn,{},c::defaultBlendSeed,false,c::CompositingSpace::EncodedSRgb,.25F),
        c::compositeLayer(dark,low,.25F,c::BlendMode::LinearBurn,{},c::defaultBlendSeed,false,c::CompositingSpace::EncodedSRgb)));
    for(auto mode:c::allBlendModes)for(auto space:{c::CompositingSpace::LinearSRgb,c::CompositingSpace::EncodedSRgb}) {
        const auto a=c::decodeColor({10,230,127,173}),z=c::decodeColor({199,54,80,91});
        CHECK(c::compositeLayer(a,z,.37F,mode,{},c::defaultBlendSeed,false,space,1)==c::compositeLayer(a,z,.37F,mode,{},c::defaultBlendSeed,false,space));
        CHECK(c::compositeLayer(a,z,.37F,mode,{},c::defaultBlendSeed,false,space,0)==a);
        for(float fill:{.01F,.25F,.5F,.99F}) {
            const auto result=c::compositeLayer(a,z,.37F,mode,{},c::defaultBlendSeed,false,space,fill);
            for(auto channel:result)CHECK(std::isfinite(channel)&&channel>=0&&channel<=1);
            if(!c::blend_detail::bSpecialFill(int(mode)))
                CHECK(result==c::compositeLayer(a,z,.37F*fill,mode,{},c::defaultBlendSeed,false,space));
        }
    }
}
void effectsAndHistory()
{
    c::EditorSession session;
    auto document=std::make_unique<c::Document>(c::CanvasSpec{{40,40}});
    auto layer=cutout();const auto id=layer.id;const auto surface=std::get<c::RasterLayer>(layer.payload).surface;
    auto effects=std::make_shared<c::LayerEffectStack>();effects->items[0].enabled=true;
    effects->items[0].size=3;effects->items[0].color={255,255,255,255};layer.effects=effects;
    layer=c::prepareSpatialFilterLayer(layer);const auto cache=layer.effectCache;
    CHECK(document->insertLayer(0,layer));session.replaceDocument(std::move(document));session.document()->markSaved();
    const auto token=session.document()->contentState(),revision=surface->revision();
    CHECK(!session.execute(std::make_unique<c::SetLayerFillOpacityCommand>(id,1)));
    CHECK(session.execute(std::make_unique<c::SetLayerFillOpacityCommand>(id,.5F,8)));
    CHECK(session.execute(std::make_unique<c::SetLayerFillOpacityCommand>(id,0,8)));
    CHECK(session.history().undoDepth()==1 && session.document()->isModified());
    CHECK(session.document()->layer(id)->effectCache==cache && surface->revision()==revision);
    auto image=u::flattenDocument(*session.document());CHECK(image);
    CHECK(image.image.pixelColor(15,15).alpha()==0); // Content disappears.
    CHECK(image.image.pixelColor(7,15).alpha()>240); // Outside stroke survives.
    CHECK(session.undo());CHECK(session.document()->contentState()==token&&!session.document()->isModified());
    CHECK(session.document()->layer(id)->fillOpacity==1);CHECK(session.redo());
    CHECK(session.document()->layer(id)->fillOpacity==0);
    // Every effect can contribute without content paint, using original coverage.
    for(std::size_t i=0;i<c::layerEffectCount;++i) {
        auto state=std::make_shared<c::LayerEffectStack>();state->items[i].enabled=true;
        auto sample=cutout();sample.effects=state;sample.fillOpacity=0;sample=c::prepareSpatialFilterLayer(sample);
        c::Document doc({{40,40}});CHECK(doc.insertLayer(0,sample));auto out=u::flattenDocument(doc);CHECK(out);
        int coverage=0;for(int y=0;y<40;++y)for(int x=0;x<40;++x)coverage+=out.image.pixelColor(x,y).alpha();
        CHECK(coverage>0);
        doc.setLayerOpacity(sample.id,0);out=u::flattenDocument(doc);
        CHECK(out.image.pixelColor(8,15).alpha()==0);
    }
}
void persistenceAndPsd()
{
    QTemporaryDir files;
    c::Document doc({{40,40},96,c::CompositingSpace::EncodedSRgb});auto layer=cutout();layer.fillOpacity=94.F/255;layer.opacity=173.F/255;
    const auto id=layer.id;CHECK(doc.insertLayer(0,layer));
    const auto expected=u::flattenDocument(doc);CHECK(expected);
    const auto path=files.filePath("fill.vulkana");CHECK(u::saveProject(path,doc));
    const auto reopened=u::loadProject(path);CHECK(reopened);
    CHECK(reopened.document->layer(id)->fillOpacity==layer.fillOpacity);
    CHECK(u::flattenDocument(*reopened.document).image==expected.image);
    std::atomic_bool cancel=false;auto snapshot=u::capturePsdExport(doc,22);auto plan=u::planPsdExport(snapshot,{},cancel);CHECK(plan);
    const auto psd=files.filePath("fill.psd");CHECK(u::writePsdExport(snapshot,plan,psd,cancel));
    auto job=std::make_shared<u::PsdJob>();auto inspection=u::inspectPsd(psd,job);CHECK(inspection.error.isEmpty());
    CHECK(inspection.layers.size()==1 && inspection.layers[0].issues.isEmpty());
    const auto converted=u::convertPsd(inspection,u::defaultPsdOptions(inspection),job);CHECK(converted.document);
    if(converted.document) {
        CHECK(converted.document->layers()[0].fillOpacity==layer.fillOpacity);
        CHECK(converted.document->layers()[0].opacity==layer.opacity);
        CHECK(u::flattenDocument(*converted.document).image==expected.image);
    }
}
void samplingAndBaking()
{
    for(bool styled:{false,true}) {
        c::Document doc({{40,40},96,c::CompositingSpace::EncodedSRgb});auto layer=cutout();
        layer.fillOpacity=.25F;layer.opacity=.6F;
        if(styled) {
            auto effects=std::make_shared<c::LayerEffectStack>();effects->items[0].enabled=true;
            effects->items[0].size=3;layer.effects=effects;
        } else {
            auto mask=std::make_shared<c::LayerMask>();mask->coverage=c::SelectionMask::filled({24,24},200);
            layer.mask=mask;
        }
        layer=c::prepareSpatialFilterLayer(layer);const auto id=layer.id;CHECK(doc.insertLayer(0,layer));
        CHECK(c::PreparedLayerSampler(layer,false).sample({15,15})[3]>.7F);
        CHECK(c::PreparedLayerSampler(layer,true).sample({15,15})[3]<.3F);
        const auto before=u::flattenDocument(doc);CHECK(before);
        auto bake=u::prepareRasterizeLayers(doc,{{id},id,id},64*1024*1024);CHECK(bake.command);
        if(bake.command) {
            CHECK(bake.command->apply(doc));
            CHECK(doc.layer(id)->fillOpacity==(styled?1:.25F));
            const auto after=u::flattenDocument(doc);CHECK(after);
            for(int y=0;y<40;++y)for(int x=0;x<40;++x) {
                const auto a=before.image.pixelColor(x,y),b=after.image.pixelColor(x,y);
                CHECK(std::abs(a.alpha()-b.alpha())<=1);
                if(a.alpha()>0&&b.alpha()>0)CHECK(std::abs(a.red()-b.red())<=1&&std::abs(a.green()-b.green())<=1&&std::abs(a.blue()-b.blue())<=1);
            }
            CHECK(bake.command->undo(doc));CHECK(doc.layer(id)->fillOpacity==.25F);
        }
        c::LayerViaCopyCommand copy(id,id);CHECK(copy.apply(doc));
        CHECK(doc.layer(*copy.createdLayerId())->fillOpacity==.25F);CHECK(copy.undo(doc));
    }
}
void controls()
{
    u::MainWindow window(nullptr,false);window.setUnsavedPromptEnabled(false);window.show();QTest::qWait(20);
    auto* value=dynamic_cast<u::CompactValueControl*>(window.findChild<QDoubleSpinBox*>("LayerOpacitySlider"));
    auto* toggle=window.findChild<QToolButton*>("LayerOpacityMode");CHECK(value&&toggle);if(!value||!toggle)return;
    auto& session=const_cast<c::EditorSession&>(window.editorSession());const auto id=*session.activeLayer();
    const auto original=session.document()->contentState();CHECK(value->prefix()=="Layer opacity: ");
    toggle->click();CHECK(value->prefix()=="Pixel opacity: ");CHECK(session.document()->contentState()==original&&session.history().undoDepth()==0);
    value->setValue(35);CHECK(std::abs(session.document()->layer(id)->fillOpacity-.35F)<1e-6F);
    CHECK(session.document()->layer(id)->opacity==1);
    toggle->click();CHECK(value->value()==100);value->setValue(70);
    CHECK(std::abs(session.document()->layer(id)->opacity-.7F)<1e-6F);
    toggle->click();CHECK(value->value()==35);
    const auto review=qEnvironmentVariable("IMAGEEDITOR_FILL_REVIEW");
    if(!review.isEmpty())CHECK(value->parentWidget()->grab().save(review));
    QTemporaryDir files;QImage image(8,8,QImage::Format_RGBA8888);image.fill(Qt::red);
    const auto path=files.filePath("second.png");CHECK(image.save(path));
    CHECK(window.openImageFromPath(path));CHECK(value->value()==100&&toggle->isChecked());
    auto* tabs=window.findChild<QTabBar*>("DocumentTabs");CHECK(tabs&&tabs->count()==2);
    if(tabs)tabs->setCurrentIndex(0);
    CHECK(value->value()==35&&session.document()->layer(id)->fillOpacity==.35F);
    CHECK(session.undo());CHECK(session.undo());CHECK(session.document()->contentState()==original);
    window.close();
}
}
int main(int argc,char** argv)
{
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);QApplication app(argc,argv);
    app.setOrganizationName("ImageEditorTests");app.setApplicationName("FillOpacity");
    QTemporaryDir settings;QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings.path());
    u::applyEditorTheme(app);blendMath();effectsAndHistory();persistenceAndPsd();samplingAndBaking();controls();
    std::cout<<"Fill opacity failures="<<failures<<'\n';return failures?1:0;
}
