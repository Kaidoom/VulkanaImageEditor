#include "LayerEffectCodec.hpp"
#include <QJsonArray>
#include <limits>
#include <set>
#include <stdexcept>
namespace imageeditor::ui::detail {
namespace {
void require(bool ok) {
  if (!ok)
    throw std::runtime_error("Unsupported or invalid essential layer effects; "
                             "project was not opened");
}
double number(const QJsonValue &v) {
  const double n = v.toDouble(std::numeric_limits<double>::quiet_NaN());
  require(v.isDouble() && std::isfinite(n));
  return n;
}
bool boolean(const QJsonValue &v) {
  require(v.isBool());
  return v.toBool();
}
int integer(const QJsonValue &v, int maximum) {
  const double n = number(v);
  require(n >= 0 && n <= maximum && std::floor(n) == n);
  return int(n);
}
QJsonArray color(core::Rgba8 c) { return {c.red, c.green, c.blue, c.alpha}; }
core::Rgba8 color(const QJsonValue &v) {
  require(v.isArray() && v.toArray().size() == 4);
  const auto a = v.toArray();
  return {std::uint8_t(integer(a[0], 255)), std::uint8_t(integer(a[1], 255)),
          std::uint8_t(integer(a[2], 255)), std::uint8_t(integer(a[3], 255))};
}
QJsonObject contour(const core::EffectContour& c) {
  QJsonArray points;
  for(const auto& p:c.points)points.append(QJsonArray{p.input,p.output,p.corner});
  return {{"enabled",c.enabled},{"interpolation",int(c.interpolation)},{"points",points}};
}
core::EffectContour contour(const QJsonValue& value) {
  require(value.isObject());const auto o=value.toObject();core::EffectContour c;
  c.enabled=boolean(o["enabled"]);
  c.interpolation=core::EffectContourInterpolation(integer(o["interpolation"],1));
  require(o["points"].isArray());c.points.clear();
  const auto points=o["points"].toArray();require(points.size()>=2&&points.size()<=16);
  for(const auto& v:points) {
    require(v.isArray()&&v.toArray().size()==3);const auto p=v.toArray();
    c.points.push_back({number(p[0]),number(p[1]),boolean(p[2])});
  }
  require(core::validEffectContour(c));return c;
}
} // namespace
QJsonObject encodeLayerEffects(const core::LayerEffectStack &stack) {
  require(core::validLayerEffects(stack));
  QJsonArray items;
  for (std::size_t i = 0; i < core::layerEffectCount; ++i) {
    const auto &e = stack.items[i];
    if(i==7&&e==core::defaultLayerEffect(core::LayerEffectType::BevelEmboss))continue;
    QJsonObject item{
        {"type", QString::fromUtf8(
                     core::layerEffectIdentifier(core::LayerEffectType(i)))},
        {"enabled", e.enabled},
        {"blendMode", QString::fromUtf8(core::blendModeId(e.blendMode))},
        {"opacity", e.opacity},
        {"color", color(e.color)},
        {"secondColor", color(e.secondColor)},
        {"size", e.size},
        {"angle", e.angle},
        {"distance", e.distance},
        {"spread", e.spread},
        {"position", int(e.position)},
        {"gradient", int(e.gradient)},
        {"scale", e.scale},
        {"reverse", e.reverse}};
    if(i==std::size_t(core::LayerEffectType::BevelEmboss)) {
      const auto& b=e.bevel;
      item["bevel"]=QJsonObject{{"version",int(b.version)},{"style",int(b.style)},
        {"depth",b.depth},{"soften",b.soften},{"altitude",b.altitude},{"down",b.down},
        {"shadowOpacity",b.shadowOpacity},{"shadowBlend",QString::fromUtf8(core::blendModeId(b.shadowBlend))},
        {"surface",contour(b.surface)},{"gloss",contour(b.gloss)}};
    }
    items.append(item);
  }
  return {{"version", int(core::layerEffectVersion)}, {"items", items}};
}
core::LayerEffectState decodeLayerEffects(const QJsonObject &o) {
  require(integer(o["version"], int(core::layerEffectVersion)) ==
          int(core::layerEffectVersion));
  require(o["items"].isArray());
  auto result = std::make_shared<core::LayerEffectStack>();
  std::set<core::LayerEffectType> seen;
  for (const auto v : o["items"].toArray()) {
    require(v.isObject());
    const auto item = v.toObject();
    const auto type =
        core::layerEffectFromIdentifier(item["type"].toString().toStdString());
    require(type.has_value() && seen.insert(*type).second);
    auto &e = result->items[std::size_t(*type)];
    const auto mode =
        core::blendModeFromId(item["blendMode"].toString().toStdString());
    require(mode.has_value());
    e.blendMode = *mode;
    e.enabled = boolean(item["enabled"]);
    e.opacity = number(item["opacity"]);
    e.color = color(item["color"]);
    e.secondColor = color(item["secondColor"]);
    e.size = number(item["size"]);
    e.angle = number(item["angle"]);
    e.distance = number(item["distance"]);
    e.spread = number(item["spread"]);
    e.position = core::StrokePosition(integer(item["position"], 2));
    e.gradient = core::GradientType(integer(item["gradient"], 1));
    e.scale = number(item["scale"]);
    e.reverse = boolean(item["reverse"]);
    if(*type==core::LayerEffectType::BevelEmboss) {
      require(item["bevel"].isObject());const auto b=item["bevel"].toObject();
      require(integer(b["version"],1)==1);
      e.bevel.style=core::BevelStyle(integer(b["style"],2));e.bevel.depth=number(b["depth"]);
      e.bevel.soften=number(b["soften"]);e.bevel.altitude=number(b["altitude"]);
      e.bevel.down=boolean(b["down"]);e.bevel.shadowOpacity=number(b["shadowOpacity"]);
      const auto mode=core::blendModeFromId(b["shadowBlend"].toString().toStdString());require(mode.has_value());
      e.bevel.shadowBlend=*mode;e.bevel.surface=contour(b["surface"]);e.bevel.gloss=contour(b["gloss"]);
    }
  }
  for(std::size_t i=0;i<7;++i)require(seen.contains(core::LayerEffectType(i)));
  require(core::validLayerEffects(*result));
  return result;
}
} // namespace imageeditor::ui::detail
