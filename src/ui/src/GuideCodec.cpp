#include "imageeditor/ui/GuideCodec.hpp"
#include <QJsonArray>
#include <cmath>
#include <stdexcept>
namespace imageeditor::ui {
namespace {
    void require(bool valid)
    {
        if (!valid)
            throw std::invalid_argument("Invalid document guides");
    }
    std::uint64_t guideId(const QJsonValue& value)
    {
        bool ok = false;
        const auto id = value.toString().toULongLong(&ok);
        require(value.isString() && ok && id && QString::number(id) == value.toString());
        return id;
    }
}
QJsonObject encodeDocumentGuides(const core::DocumentGuides& guides)
{
    require(core::validDocumentGuides(guides));
    QJsonArray items;
    for (const auto& guide : guides.items)
        items.append(QJsonObject { { "id", QString::number(guide.id) },
            { "orientation",
                guide.orientation == core::GuideOrientation::Horizontal ? "horizontal" : "vertical" },
            { "position", guide.position } });
    return { { "version", 1 }, { "nextId", QString::number(guides.nextId) }, { "items", items } };
}
core::DocumentGuides decodeDocumentGuides(const QJsonValue& value)
{
    require(value.isObject());
    const auto object = value.toObject();
    require(
        object["version"] == 1 && object["items"].isArray() && object["items"].toArray().size() <= 100000);
    core::DocumentGuides guides;
    guides.nextId = guideId(object["nextId"]);
    for (const auto& value : object["items"].toArray()) {
        require(value.isObject());
        const auto item = value.toObject();
        require(item["orientation"] == "horizontal" || item["orientation"] == "vertical");
        require(item["position"].isDouble());
        guides.items.push_back({ guideId(item["id"]),
            item["orientation"] == "horizontal" ? core::GuideOrientation::Horizontal
                                                : core::GuideOrientation::Vertical,
            item["position"].toDouble() });
    }
    require(core::validDocumentGuides(guides));
    return guides;
}
}
