#include "imageeditor/ui/BrushSymmetryCodec.hpp"
#include <QJsonArray>
#include <stdexcept>

namespace imageeditor::ui {
namespace {
void require(bool valid)
{
    if (!valid)
        throw std::invalid_argument("Invalid brush symmetry descriptor");
}
}

QJsonObject encodeBrushSymmetry(const core::BrushSymmetry& symmetry)
{
    require(core::validBrushSymmetry(symmetry));
    QJsonObject object {{"version",1}, {"enabled",symmetry.enabled},
        {"axis",symmetry.axis == core::BrushSymmetryAxis::Vertical ? "vertical" : "horizontal"},
        {"center",QJsonValue::Null}};
    if (symmetry.center)
        object["center"] = QJsonArray {symmetry.center->x, symmetry.center->y};
    return object;
}

core::BrushSymmetry decodeBrushSymmetry(const QJsonValue& value)
{
    require(value.isObject());
    const auto object = value.toObject();
    require(object["version"] == 1 && object["enabled"].isBool());
    require(object["axis"] == "vertical" || object["axis"] == "horizontal");
    core::BrushSymmetry symmetry;
    symmetry.enabled = object["enabled"].toBool();
    symmetry.axis = object["axis"] == "vertical" ? core::BrushSymmetryAxis::Vertical
                                                : core::BrushSymmetryAxis::Horizontal;
    const auto center = object.value("center");
    if (!center.isNull()) {
        require(center.isArray());
        const auto coordinates = center.toArray();
        require(coordinates.size() == 2 && coordinates[0].isDouble() && coordinates[1].isDouble());
        symmetry.center = core::Vec2d {coordinates[0].toDouble(), coordinates[1].toDouble()};
    }
    require(core::validBrushSymmetry(symmetry));
    return symmetry;
}

} // namespace imageeditor::ui
