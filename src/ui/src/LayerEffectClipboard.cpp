#include "imageeditor/ui/LayerEffectClipboard.hpp"
#include "LayerEffectCodec.hpp"
#include <QJsonDocument>

namespace imageeditor::ui {
namespace {
constexpr qsizetype maximumBytes = 128 * 1024;
}
QByteArray encodeLayerEffectClipboard(const core::LayerEffectState& effects)
{
    const auto stack = effects ? *effects : core::LayerEffectStack{};
    if (!core::validLayerEffects(stack)) return {};
    const auto bytes = QJsonDocument(QJsonObject{{"version", 1},
        {"effects", detail::encodeLayerEffects(stack)}}).toJson(QJsonDocument::Compact);
    return bytes.size() <= maximumBytes ? bytes : QByteArray{};
}

std::optional<core::LayerEffectState> decodeLayerEffectClipboard(const QByteArray& bytes)
{
    if (bytes.isEmpty() || bytes.size() > maximumBytes) return {};
    const auto document = QJsonDocument::fromJson(bytes);
    if (!document.isObject()) return {};
    const auto object = document.object();
    if (object["version"].toDouble(-1) != 1 || !object["effects"].isObject()) return {};
    try {
        return detail::decodeLayerEffects(object["effects"].toObject());
    } catch (const std::exception&) { return {}; }
}
} // namespace imageeditor::ui
