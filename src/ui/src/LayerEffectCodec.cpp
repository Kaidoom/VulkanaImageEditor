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
} // namespace
QJsonObject encodeLayerEffects(const core::LayerEffectStack &stack) {
  require(core::validLayerEffects(stack));
  QJsonArray items;
  for (std::size_t i = 0; i < core::layerEffectCount; ++i) {
    const auto &e = stack.items[i];
    items.append(QJsonObject{
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
        {"reverse", e.reverse}});
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
  }
  require(seen.size() == core::layerEffectCount &&
          core::validLayerEffects(*result));
  return result;
}
} // namespace imageeditor::ui::detail
