#include "imageeditor/ui/MirrorCodec.hpp"
#include <QJsonArray>
#include <stdexcept>
namespace imageeditor::ui {
QJsonObject encodeLayerMirror(const core::LayerMirror& mirror)
{
    QJsonArray effects;
    for (const auto mode:mirror.effects) effects.append(int(mode));
    return {{"version",1},{"source",QString::number(mirror.source)},
        {"opacityOverride",mirror.opacityOverride},{"fillOpacityOverride",mirror.fillOpacityOverride},
        {"blendModeOverride",mirror.blendModeOverride},
        {"pixelAlphaShapesEffectsOverride",mirror.pixelAlphaShapesEffectsOverride},
        {"maskEffectsAfterRenderingOverride",mirror.maskEffectsAfterRenderingOverride},{"effects",effects}};
}
core::LayerMirror decodeLayerMirror(const QJsonValue& value)
{
    const auto require=[](bool valid){if(!valid)throw std::invalid_argument("Invalid mirrored-layer descriptor");};
    require(value.isObject());const auto object=value.toObject();
    require(object["version"]==1 && object["source"].isString());
    bool valid=false;core::LayerMirror result;
    result.source=object["source"].toString().toULongLong(&valid);
    require(valid && result.source && QString::number(result.source)==object["source"].toString());
    const auto boolean=[&](const char* key){require(object[key].isBool());return object[key].toBool();};
    result.opacityOverride=boolean("opacityOverride");result.fillOpacityOverride=boolean("fillOpacityOverride");
    result.blendModeOverride=boolean("blendModeOverride");
    result.pixelAlphaShapesEffectsOverride=boolean("pixelAlphaShapesEffectsOverride");
    result.maskEffectsAfterRenderingOverride=boolean("maskEffectsAfterRenderingOverride");
    require(object["effects"].isArray() && object["effects"].toArray().size()==int(core::layerEffectCount));
    const auto modes=object["effects"].toArray();
    for (int i=0;i<modes.size();++i) {
        const int mode=modes[i].toInt(-1);require(modes[i].isDouble() && modes[i].toDouble()==mode && mode>=0 && mode<=2);
        result.effects[std::size_t(i)]=core::MirrorEffectMode(mode);
    }
    return result;
}
}
