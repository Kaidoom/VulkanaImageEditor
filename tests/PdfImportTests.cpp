#include "imageeditor/ui/PdfImport.hpp"
#include "imageeditor/ui/ProjectFile.hpp"
#include "imageeditor/ui/ImageExport.hpp"
#include <QApplication>
#include <QCryptographicHash>
#include <QColorSpace>
#include <QFile>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QDir>
#include <iostream>
#include <cmath>
#include <thread>
namespace u=imageeditor::ui;
namespace c=imageeditor::core;
int failures=0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { std::cerr<<"FAIL "<<__LINE__<<": " #__VA_ARGS__ "\n"; ++failures; } } while(false)
QString fixture(const QString& file) { return QDir(qEnvironmentVariable("VULKANA_PDF_FIXTURES",IMAGEEDITOR_SOURCE_DIR "/tests/fixtures/pdf")).filePath(file); }
auto job() { return std::make_shared<u::PdfJobState>(); }
u::PdfMetadata load(const QString& path, QString password={}) {
    auto result=u::readPdfMetadata(path,password,job()).get();
    if (!result.source) std::cerr<<"PDF load: "<<result.error.toStdString()<<'\n';
    return result;
}
QImage image(const c::Layer& layer) {
    const auto surface=std::get<c::RasterLayer>(layer.payload).surface;
    QImage result(int(surface->extent().width),int(surface->extent().height),QImage::Format_RGBA8888);
    result.setColorSpace(QColorSpace::SRgb);
    surface->copyRgba8({0,0,int(surface->extent().width),int(surface->extent().height)},
        {reinterpret_cast<std::byte*>(result.bits()),std::size_t(result.sizeInBytes())},std::size_t(result.bytesPerLine()));
    return result;
}
u::PdfRenderedPages render(const u::PdfMetadata& metadata, u::PdfOptions options) {
    auto result=u::renderPdfPages(metadata,options,{},job()).get();
    CHECK(result.error.isEmpty());CHECK(!result.cancelled);CHECK(result.pages.size()==options.pages.size());
    return result;
}
void coreTests() {
    QString error;
    CHECK(u::parsePdfPageRange("1, 3-5, 4, 8",8,error)==std::vector<int>({0,2,3,4,7}));CHECK(error.isEmpty());
    CHECK(u::formatPdfPageRange({0,2,3,4,7})=="1, 3-5, 8");
    for (const auto* value:{"0","9","5-3","1,","1,,3","1-","a","1.5","999999999999999","-1","1 2"}) {
        CHECK(u::parsePdfPageRange(value,8,error).empty());CHECK(!error.isEmpty());
    }
    CHECK(u::parsePdfPageRange(" ",8,error).empty());CHECK(error.isEmpty());
    auto metadata=load(fixture("pages.pdf"));CHECK(metadata.pages.size()==4);if(!metadata.source)return;
    CHECK(metadata.pages[0].label=="i"); CHECK(metadata.pages[1].label=="ii"); CHECK(metadata.pages[2].label=="1");
    u::PdfOptions options; options.ppi=72;options.pages={0,1,2,3};
    auto plan=u::planPdfImport(metadata,options);CHECK(plan);CHECK(plan.canvas==QSize(120,160));
    CHECK(plan.sizes==std::vector<QSize>({{120,80},{80,160},{72,36},{64,64}}));
    options.ppi=300; plan=u::planPdfImport(metadata,options);CHECK(plan.sizes[2]==QSize(302,151));
    options.ppi=150; CHECK(u::planPdfImport(metadata,options).sizes[2]==QSize(151,75));
    options.ppi=72; options.whitePaper=false;
    auto rendered=render(metadata,options);if(rendered.pages.size()!=4)return;
    const auto first=image(rendered.pages[0].layer);
    CHECK(first.pixelColor(10,70)==QColor(Qt::red)); // RGBA order + top-left/Y-down.
    CHECK(first.pixelColor(40,40)==QColor(Qt::blue));
    CHECK(first.pixelColor(110,10).alpha()==0);
    CHECK(image(rendered.pages[3].layer).pixelColor(20,20).alpha()==0);
    auto thumb=u::renderPdfThumbnail(metadata.source,0,{120,80},options,job()).get();
    CHECK(thumb.error.isEmpty()); CHECK(thumb.image.convertToFormat(QImage::Format_RGBA8888)==first);
    u::PdfLimits limits;limits.rasterBytes=1;CHECK(!u::planPdfImport(metadata,options,limits));
    limits={};limits.existingBytes=limits.workingBytes;CHECK(!u::planPdfImport(metadata,options,limits));
    limits={};limits.maximumDimension=100;CHECK(!u::planPdfImport(metadata,options,limits));
    limits={};limits.availableLayers=2;CHECK(!u::planPdfImport(metadata,options,limits));
    limits={};limits.currentDocumentLayers=0;CHECK(u::planPdfImport(metadata,options,limits));
    options.destination=u::PdfDestination::CurrentDocument;CHECK(!u::planPdfImport(metadata,options,limits));
    options.destination=u::PdfDestination::NewDocument;
    options.pages={};CHECK(!u::planPdfImport(metadata,options)); options.pages={1,0};CHECK(!u::planPdfImport(metadata,options));
    options.pages={0}; options.ppi=NAN;CHECK(!u::planPdfImport(metadata,options)); options.ppi=72;
    auto rotated=load(fixture("rotated-crop.pdf"));CHECK(rotated.pages.size()==1);
    if(rotated.source) { CHECK(rotated.pages[0].points==QSizeF(70,120));
        auto output=render(rotated,options);CHECK(output.pages[0].size==QSize(70,120));
        const auto pixels=image(output.pages[0].layer);CHECK(pixels.pixelColor(10,10)==QColor(Qt::red));CHECK(pixels.pixelColor(60,110)==QColor(Qt::blue));
    }
    auto unit=load(fixture("user-unit.pdf"));CHECK(unit.pages.size()==1);
    if(unit.source) { CHECK(unit.pages[0].points==QSizeF(160,120));
        auto output=render(unit,options);CHECK(output.pages[0].size==QSize(160,120));
        CHECK(image(output.pages[0].layer).pixelColor(60,100)==QColor(Qt::green));
    }
    auto transparent=load(fixture("transparency.pdf"));
    auto transparency=render(transparent,options);
    const auto alpha=image(transparency.pages[0].layer);
    CHECK(alpha.pixelColor(5,5).alpha()==0);
    CHECK(alpha.pixelColor(10,90)==QColor(Qt::white));
    const auto half=alpha.pixelColor(40,50);CHECK(half.red()==255&&half.green()==0&&half.blue()==0);CHECK(half.alpha()>=126&&half.alpha()<=128);
    options.whitePaper=true;
    auto paper=render(transparent,options);const auto white=image(paper.pages[0].layer);
    CHECK(white.pixelColor(5,5)==QColor(Qt::white));CHECK(white.pixelColor(40,50).alpha()==255);
    options.whitePaper=false;
    auto annotation=load(fixture("annotations.pdf"));options.annotations=false;
    CHECK(image(render(annotation,options).pages[0].layer).pixelColor(30,70).alpha()==0);
    options.annotations=true;CHECK(image(render(annotation,options).pages[0].layer).pixelColor(30,70)==QColor(Qt::blue));
    // QPdfDocument's annotation pass excludes interactive form widgets. Do not
    // promise form filling or synthesize appearances with unrelated fonts.
    auto form=load(fixture("form-appearance.pdf"));
    CHECK(image(render(form,options).pages[0].layer).pixelColor(30,70).alpha()==0);
    auto locked=u::readPdfMetadata(fixture("password.pdf"),{},job()).get();CHECK(locked.passwordRequired);CHECK(!locked.source);
    locked=u::readPdfMetadata(fixture("password.pdf"),"wrong",job()).get();CHECK(locked.passwordRequired);CHECK(!locked.source);
    CHECK(load(fixture("password.pdf"),"test-password").source);
    auto malformed=u::readPdfMetadata(fixture("malformed.pdf"),{},job()).get();CHECK(!malformed.source);CHECK(!malformed.error.isEmpty());
    CHECK(!u::readPdfMetadata(fixture("missing.pdf"),{},job()).get().error.isEmpty());
    auto oversized=load(fixture("oversized.pdf"));CHECK(oversized.source);CHECK(!u::planPdfImport(oversized,options));
    auto cancel=job();cancel->cancelled=true;auto cancelled=u::renderPdfPages(metadata,options,{},cancel).get();
    CHECK(cancelled.cancelled&&cancelled.pages.empty());
    // Cancellation after a completed page must discard the WHOLE staged batch.
    auto inFlight=job();auto large=options;large.pages={0,1,2,3};large.ppi=2400;
    auto future=u::renderPdfPages(metadata,large,{},inFlight);
    while(future.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready
          &&inFlight->completed.load()==0)std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(inFlight->completed.load()>0);inFlight->cancelled=true;
    auto interrupted=future.get();CHECK(interrupted.cancelled&&interrupted.pages.empty());
    // Snapshot no longer depends on the source path, even before final rendering.
    QTemporaryDir temporary;const auto copy=temporary.filePath("source.pdf");CHECK(QFile::copy(fixture("pages.pdf"),copy));
    auto detached=load(copy);CHECK(QFile::remove(copy));options.pages={0,1};auto independent=render(detached,options);
    c::Document document({{120,160},72});
    for(auto& page:independent.pages) CHECK(document.insertLayer(0,std::move(page.layer)));
    const auto project=temporary.filePath("self-contained.vulkana");CHECK(u::saveProject(project,document));
    auto reopened=u::loadProject(project);CHECK(reopened);if(!reopened)return;
    CHECK(reopened.document->canvas().dotsPerInch==72);CHECK(reopened.document->layers().size()==2);
    for(std::size_t i=0;i<document.layers().size();++i) CHECK(image(document.layers()[i])==image(reopened.document->layers()[i]));
    for(auto format:{u::ExportFormat::Png,u::ExportFormat::Jpeg,u::ExportFormat::WebP}) {
        u::ExportSettings output; output.format=format;output.size={120,160};output.webpLossless=true;
        auto flat=u::renderExport(*reopened.document,output);CHECK(flat);
        std::atomic_bool stop=false;auto encoded=u::encodeExport(flat.image,output,stop);CHECK(encoded);CHECK(encoded.decoded.size()==QSize(120,160));
    }
}
int main(int argc,char** argv) {
    QApplication app(argc,argv);
    coreTests();
    // Optional private reference measurement, never required or installed.
    for(int i=1;i<argc;++i) {
        const QString path=QString::fromLocal8Bit(argv[i]);if(!path.endsWith(".pdf",Qt::CaseInsensitive))continue;
        auto metadata=load(path); if(!metadata.source){CHECK(false);continue;}
        u::PdfOptions options;for(std::size_t n=0;n<metadata.pages.size();++n)options.pages.push_back(int(n));
        auto output=render(metadata,options);const auto plan=u::planPdfImport(metadata,options);
        std::cout<<QFileInfo(path).fileName().toStdString()<<" canvas "<<plan.canvas.width()<<'x'<<plan.canvas.height()
            <<" raster bytes "<<plan.rasterBytes<<" estimated working "<<plan.estimatedWorkingBytes<<'\n';
        for(const auto& page:output.pages) {
            std::cout<<"page "<<page.page+1<<" "<<page.size.width()<<'x'<<page.size.height()<<" ms "<<page.milliseconds<<'\n';
            const auto out=qEnvironmentVariable("VULKANA_PDF_TEST_OUTPUT");
            if(!out.isEmpty())CHECK(image(page.layer).save(QDir(out).filePath(QFileInfo(path).completeBaseName()+QString("-%1.png").arg(page.page+1))));
            auto single=options;single.pages={page.page};
            auto alone=render(metadata,single);
            CHECK(alone.pages[0].size==page.size);CHECK(image(alone.pages[0].layer)==image(page.layer));
        }
        if(output.pages.size()>2) {
            auto subset=options;subset.pages={0,2};auto selected=render(metadata,subset);
            CHECK(image(selected.pages[0].layer)==image(output.pages[0].layer));
            CHECK(image(selected.pages[1].layer)==image(output.pages[2].layer));
        }
    }
    return failures ? 1:0;
}
