#include "imageeditor/core/SpatialFilters.hpp"
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>

namespace {
using namespace imageeditor::core;
double residentMiB()
{
    // ru_maxrss can include a large pre-exec launcher image. Measure this
    // executable's resident set while its input/output/scratch are live.
    std::ifstream status("/proc/self/status");std::string key;
    while(status>>key) {
        if(key=="VmRSS:") {double kib=0;status>>kib;return kib/1024;}
        std::string remainder;std::getline(status,remainder);
    }
    return 0;
}
std::array<LayerSpatialFilter,4> filters()
{
    std::array<LayerSpatialFilter,4> f{defaultSpatialFilter(SpatialFilterType::Gaussian),
        defaultSpatialFilter(SpatialFilterType::Motion),defaultSpatialFilter(SpatialFilterType::Lens),defaultSpatialFilter(SpatialFilterType::Lens)};
    f[0].parameters=GaussianBlurParameters{12,12};f[1].parameters=MotionBlurParameters{24,25};
    f[2].parameters=LensBlurParameters{12,0,0};f[3].parameters=LensBlurParameters{12,6,15};
    for(auto& filter:f)filter.enabled=true;
    return f;
}
SpatialPlane decodeImage(const QImage& image)
{
    SpatialPlane p{{0,0,image.width(),image.height()},4,{}};
    p.pixels.resize(std::size_t(image.width())*std::size_t(image.height())*4);
    for(int y=0;y<image.height();++y)for(int x=0;x<image.width();++x) {
        const auto color=image.pixelColor(x,y);const auto c=decodeColor({std::uint8_t(color.red()),std::uint8_t(color.green()),std::uint8_t(color.blue()),std::uint8_t(color.alpha())});
        std::copy(c.begin(),c.end(),p.pixels.begin()+std::ptrdiff_t((std::size_t(y)*std::size_t(image.width())+std::size_t(x))*4));
    }
    return p;
}
QImage displayImage(SpatialPlaneView plane,bool boost=false)
{
    QImage image(plane.bounds.width,plane.bounds.height,QImage::Format_RGBA8888);
    for(int y=0;y<image.height();++y)for(int x=0;x<image.width();++x) {
        const auto i=(std::size_t(y)*std::size_t(image.width())+std::size_t(x))*4;
        PremultipliedColor c{};for(std::size_t ch=0;ch<4;++ch)c[ch]=plane.pixels[i+ch];
        if(boost) {const float alpha=std::clamp(c[3]*128,0.0F,1.0F);for(std::size_t ch=0;ch<3;++ch)c[ch]=c[3]>0?c[ch]/c[3]*alpha:0;c[3]=alpha;}
        const std::uint8_t shade=((x/8+y/8)%2)?64:48;const auto background=decodeColor({shade,shade,shade,255});
        const auto rgba=encodeColor(compositeLayer(background,c,1,BlendMode::Normal));
        image.setPixelColor(x,y,QColor(rgba.red,rgba.green,rgba.blue));
    }
    return image;
}
int sheet(const QString& path)
{
    constexpr int width=224,height=144,gap=12,header=38;
    QImage source(width,height,QImage::Format_RGBA8888);source.fill(Qt::transparent);
    {
        QPainter p(&source);p.setRenderHint(QPainter::Antialiasing);p.setPen(Qt::NoPen);
        p.setBrush(QColor(255,72,24,205));p.drawEllipse(QRectF(12.3,16.5,52,52));
        p.setBrush(QColor(36,140,255,155));p.drawRoundedRect(QRectF(54.5,35.5,54,45),8,8);
        p.setPen(QPen(QColor(255,255,255,200),1));
        for(int y=12;y<84;y+=4)p.drawLine(126,y,204,y);
        p.setFont(QFont(QStringLiteral("sans-serif"),18));p.drawText(QPointF(13,124),QStringLiteral("Editable Aa"));
    }
    auto content=decodeImage(source);
    SpatialPlane impulse{{0,0,width,height},4,std::vector<float>(std::size_t(width*height*4))};
    const auto at=std::size_t((height/2)*width+width/2)*4;
    for(std::size_t i=0;i<4;++i)impulse.pixels[at+i]=1;
    QImage result(5*width+6*gap,2*height+header+3*gap,QImage::Format_RGB32);result.fill(QColor(25,26,29));
    QPainter painter(&result);painter.setPen(QColor(233,233,235));painter.setFont(QFont(QStringLiteral("sans-serif"),10));
    const QStringList labels{QStringLiteral("Original"),QStringLiteral("Gaussian · radius 12"),QStringLiteral("Motion · 24 px / 25°"),QStringLiteral("Lens · circle r12"),QStringLiteral("Lens · 6 blades r12")};
    const auto fs=filters();
    for(int i=0;i<5;++i) {
        const int x=gap+i*(width+gap);painter.drawText(QRect(x,4,width,header-4),Qt::AlignCenter,labels[i]);
        if(i==0) {painter.drawImage(x,header,displayImage(content.view()));painter.drawImage(x,header+height+gap,displayImage(impulse.view(),true));}
        else {
            auto a=filterSpatialRegion(content.view(),content.bounds,fs[std::size_t(i-1)]);
            auto b=filterSpatialRegion(impulse.view(),impulse.bounds,fs[std::size_t(i-1)]);
            if(!a||!b){std::cerr<<"Comparison filtering failed\n";return 1;}
            painter.drawImage(x,header,displayImage(a.output.view()));painter.drawImage(x,header+height+gap,displayImage(b.output.view(),true));
        }
    }
    painter.end();
    if(!result.save(path)){std::cerr<<"Unable to write comparison sheet\n";return 1;}
    std::cout<<"Comparison sheet: "<<path.toStdString()<<" (impulse row alpha gain128 for visibility)\n";return 0;
}
int benchmark(int width,int height)
{
    if(width<=0||height<=0||width>8192||height>8192)return 2;
    SpatialPlane input{{0,0,width,height},4,std::vector<float>(std::size_t(width)*std::size_t(height)*4)};
    for(int y=0;y<height;++y)for(int x=0;x<width;++x) {
        const auto i=(std::size_t(y)*std::size_t(width)+std::size_t(x))*4;
        const float alpha=(x%53<46&&y%41<35)?.8F:0;
        input.pixels[i]=alpha*float(x%257)/256;input.pixels[i+1]=alpha*float(y%257)/256;input.pixels[i+2]=alpha*.3F;input.pixels[i+3]=alpha;
    }
    std::cout<<"width,height,filter,milliseconds,working_MiB,input_MiB,observed_peak_RSS_MiB,checksum,cancel_ms\n";
    for(const auto& f:filters()) {
        double peakRss=residentMiB(),nextSample=0;SpatialFilterOptions run;
        run.progress=[&](double p) {if(p>=nextSample){peakRss=std::max(peakRss,residentMiB());nextSample=p+.01;}};
        const auto start=std::chrono::steady_clock::now();auto result=filterSpatialRegion(input.view(),input.bounds,f,run);
        const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        if(!result){std::cerr<<result.error<<'\n';return 1;}
        double checksum=0;for(std::size_t i=0;i<result.output.pixels.size();i+=997)checksum+=result.output.pixels[i];
        peakRss=std::max(peakRss,residentMiB());
        result.output.pixels.clear();result.output.pixels.shrink_to_fit();
        const auto cancelStart=std::chrono::steady_clock::now();SpatialFilterOptions options;
        options.cancelled=[&]{return std::chrono::steady_clock::now()-cancelStart>std::chrono::milliseconds(20);};
        const auto cancelled=filterSpatialRegion(input.view(),input.bounds,f,options);
        const double cancelMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-cancelStart).count()-20;
        std::cout<<width<<','<<height<<','<<spatialFilterIdentifier(f.type)<<','<<std::fixed<<std::setprecision(2)<<ms<<','
            <<double(result.peakWorkingBytes)/(1024*1024)<<','<<double(input.pixels.size()*sizeof(float))/(1024*1024)<<','
            <<peakRss<<','<<std::setprecision(5)<<checksum<<','<<std::setprecision(2)<<cancelMs<<'\n';
        if(cancelled.status!=SpatialFilterStatus::Cancelled){std::cerr<<"Expected cancellation\n";return 1;}
    }
    return 0;
}
}
int main(int argc,char** argv)
{
    if(argc==4&&std::string_view(argv[1])=="--benchmark")return benchmark(std::atoi(argv[2]),std::atoi(argv[3]));
    if(argc==3&&std::string_view(argv[1])=="--sheet") {QGuiApplication app(argc,argv);return sheet(QString::fromLocal8Bit(argv[2]));}
    std::cerr<<"Usage: imageeditor_spatial_filter_benchmark --benchmark WIDTH HEIGHT | --sheet OUTPUT.png\n";return 2;
}
