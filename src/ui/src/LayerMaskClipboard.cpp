#include "imageeditor/ui/LayerMaskClipboard.hpp"
#include <QDataStream>
#include <QIODevice>

namespace imageeditor::ui {
namespace {
constexpr quint32 magic=0x564d534b, version=1;
constexpr qsizetype headerBytes=4*4+2+18*8;
constexpr quint64 maxPixels=64ULL*1024*1024;
void configure(QDataStream& stream)
{
    stream.setVersion(QDataStream::Qt_6_0);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.setFloatingPointPrecision(QDataStream::DoublePrecision);
}
void writeTransform(QDataStream& stream,const core::AffineTransform& t)
{
    stream<<t.m00<<t.m01<<t.m02<<t.m10<<t.m11<<t.m12<<t.m20<<t.m21<<t.m22;
}
void readTransform(QDataStream& stream,core::AffineTransform& t)
{
    stream>>t.m00>>t.m01>>t.m02>>t.m10>>t.m11>>t.m12>>t.m20>>t.m21>>t.m22;
}
}
QByteArray encodeLayerMaskClipboard(const core::Layer& layer)
{
    if(!layer.mask||!core::validLayerMask(layer.mask)||!layer.localToDocument.inverted())return {};
    const auto e=layer.mask->coverage->extent();
    QByteArray bytes;
    bytes.reserve(headerBytes+qsizetype(quint64(e.width)*e.height));
    QDataStream stream(&bytes,QIODevice::WriteOnly);configure(stream);
    stream<<magic<<version<<quint32(e.width)<<quint32(e.height)<<quint8(layer.mask->outside)<<quint8(layer.mask->enabled);
    writeTransform(stream,layer.mask->localToMask);writeTransform(stream,layer.localToDocument);
    if(stream.status()!=QDataStream::Ok||bytes.size()!=headerBytes)return {};
    bytes.resize(headerBytes+qsizetype(quint64(e.width)*e.height));
    auto* data=bytes.data()+headerBytes;
    for(quint32 y=0;y<e.height;++y)for(quint32 x=0;x<e.width;++x)
        data[qsizetype(y)*e.width+x]=char(layer.mask->coverage->coverageAtDocumentPixel(int(x),int(y)));
    return bytes;
}
std::optional<LayerMaskClipboard> decodeLayerMaskClipboard(const QByteArray& bytes)
{
    if(bytes.size()<headerBytes||quint64(bytes.size())>quint64(headerBytes)+maxPixels)return {};
    QDataStream stream(bytes);configure(stream);
    quint32 tag{},schema{},width{},height{};quint8 outside{},enabled{};
    stream>>tag>>schema>>width>>height>>outside>>enabled;
    if(tag!=magic||schema!=version||!width||!height||width>32768||height>32768||enabled>1
        ||quint64(width)*height>maxPixels||quint64(bytes.size()-headerBytes)!=quint64(width)*height)return {};
    core::LayerMask mask;core::AffineTransform source;
    readTransform(stream,mask.localToMask);readTransform(stream,source);
    if(stream.status()!=QDataStream::Ok||!mask.localToMask.inverted()||!source.inverted())return {};
    try {
        mask.outside=outside;mask.enabled=enabled!=0;
        mask.coverage=core::SelectionMask::fromR8({width,height},
            {reinterpret_cast<const std::uint8_t*>(bytes.constData()+headerBytes),std::size_t(width)*height},width);
        auto result=std::make_shared<core::LayerMask>(std::move(mask));
        if(!core::validLayerMask(result))return {};
        return LayerMaskClipboard{std::move(result),source};
    }catch(const std::exception&){return {};}
}
core::LayerMaskState pastedLayerMask(const LayerMaskClipboard& source,const core::AffineTransform& target,
                                    core::RectD destinationBounds)
{
    if(!source.mask||!core::validLayerMask(source.mask)||!target.inverted())return {};
    const auto inverse=source.sourceToDocument.inverted();if(!inverse)return {};
    auto result=std::make_shared<core::LayerMask>(*source.mask);
    if(target!=source.sourceToDocument)
        result->localToMask=core::composeTransform(source.mask->localToMask,core::composeTransform(*inverse,target));
    return core::validLayerMask(result)?core::expandedLayerMask(result,destinationBounds):core::LayerMaskState{};
}
}
