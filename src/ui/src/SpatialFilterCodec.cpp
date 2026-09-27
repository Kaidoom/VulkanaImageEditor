#include "SpatialFilterCodec.hpp"
#include "TransformCodec.hpp"
#include <QJsonArray>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace imageeditor::ui::detail {
namespace {
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
double number(const QJsonValue& value)
{
    const auto n=value.toDouble(std::numeric_limits<double>::quiet_NaN());
    require(value.isDouble()&&std::isfinite(n),"Invalid spatial-filter parameter number");return n;
}
bool boolean(const QJsonValue& v){require(v.isBool(),"Invalid spatial-filter option");return v.toBool();}
QJsonObject object(const QJsonValue& v){require(v.isObject(),"Invalid spatial-filter descriptor");return v.toObject();}
QJsonArray array(const QJsonValue& v,qsizetype n)
{require(v.isArray()&&v.toArray().size()==n,"Invalid spatial-filter descriptor array");return v.toArray();}
std::uint32_t dimension(const QJsonValue& v)
{const auto n=number(v);require(n>=1&&n<=32768&&std::floor(n)==n,"Invalid spatial-filter mask extent");return std::uint32_t(n);}
}
QString spatialFilterMaskPath(core::LayerId id,core::SpatialFilterType type)
{return QStringLiteral("filter-masks/%1-%2.r8").arg(id).arg(QString::fromStdString(std::string(core::spatialFilterIdentifier(type))));}
QJsonObject encodeSpatialFilters(const core::SpatialFilterStack& stack,core::LayerId id)
{
    require(core::validSpatialFilters(stack),"Invalid spatial filters cannot be saved");
    QJsonArray items;
    for(const auto& filter:stack.items) {
        QJsonObject parameters;
        if(const auto* p=std::get_if<core::GaussianBlurParameters>(&filter.parameters))
            parameters={{"radiusX",p->radiusX},{"radiusY",p->radiusY}};
        else if(const auto* p=std::get_if<core::MotionBlurParameters>(&filter.parameters))
            parameters={{"distance",p->distance},{"angle",p->angle}};
        else if(const auto* p=std::get_if<core::LensBlurParameters>(&filter.parameters))
            parameters={{"radius",p->radius},{"blades",int(p->blades)},{"rotation",p->rotation}};
        QJsonObject item{{"type",QString::fromStdString(std::string(core::spatialFilterIdentifier(filter.type)))},
            {"algorithmVersion",int(core::spatialFilterAlgorithmVersion)},{"enabled",filter.enabled},
            {"preserveAlpha",filter.preserveAlpha},{"parameters",parameters}};
        if(filter.mask) {
            const auto e=filter.mask->coverage->extent();const auto& t=filter.mask->localToMask;
            item["mask"]=QJsonObject{{"width",int(e.width)},{"height",int(e.height)},
                {"path",spatialFilterMaskPath(id,filter.type)},
                {"localToMask",encodeTransform(t)}};
        }
        items.append(item);
    }
    return {{"version",1},{"algorithmVersion",int(stack.algorithmVersion)},{"items",items}};
}
SpatialFilterPlan decodeSpatialFilters(const QJsonObject& o,core::LayerId id)
{
    require(number(o["version"])==1,"Unsupported spatial-filter descriptor version");
    require(number(o["algorithmVersion"])==core::spatialFilterAlgorithmVersion,"Unsupported spatial-filter algorithm version");
    const auto items=array(o["items"],core::spatialFilterCount);
    SpatialFilterPlan result{std::make_shared<core::SpatialFilterStack>(),{}};
    for(std::size_t i=0;i<core::spatialFilterCount;++i) {
        const auto item=object(items[qsizetype(i)]);
        require(item["type"].isString(),"Missing essential spatial-filter type");
        const auto type=core::spatialFilterFromIdentifier(item["type"].toString().toStdString());
        require(type && *type==core::allSpatialFilterTypes[i],"Unsupported spatial-filter type or order; project was not opened");
        require(number(item["algorithmVersion"])==core::spatialFilterAlgorithmVersion,"Unsupported essential spatial-filter algorithm");
        auto& filter=result.stack->items[i];filter.enabled=boolean(item["enabled"]);
        filter.preserveAlpha=boolean(item["preserveAlpha"]);
        const auto p=object(item["parameters"]);
        switch(*type) {
        case core::SpatialFilterType::Gaussian:filter.parameters=core::GaussianBlurParameters{number(p["radiusX"]),number(p["radiusY"])};break;
        case core::SpatialFilterType::Motion:filter.parameters=core::MotionBlurParameters{number(p["distance"]),number(p["angle"])};break;
        case core::SpatialFilterType::Lens:{
            const auto blades=number(p["blades"]);
            require(blades>=0&&blades<=12&&std::floor(blades)==blades,"Invalid aperture blade count");
            filter.parameters=core::LensBlurParameters{number(p["radius"]),std::uint32_t(blades),number(p["rotation"])};break;
        }}
        if(item.contains("mask")) {
            const auto mask=object(item["mask"]);
            const core::Extent2u e{dimension(mask["width"]),dimension(mask["height"])};
            require(std::uint64_t(e.width)*e.height<=64ULL*1024*1024,"Spatial-filter mask exceeds pixel budget");
            const auto path=spatialFilterMaskPath(id,*type);
            require(mask["path"].isString()&&mask["path"].toString()==path,"Invalid spatial-filter mask path");
            const auto mapping=decodeTransform(mask["localToMask"]);
            result.masks.push_back({i,path,e,mapping});
        }
    }
    require(core::validSpatialFilters(*result.stack),"Invalid spatial-filter parameters; project was not opened");
    return result;
}
}
