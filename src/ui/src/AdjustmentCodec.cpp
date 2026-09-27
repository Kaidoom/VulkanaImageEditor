#include "AdjustmentCodec.hpp"
#include "TransformCodec.hpp"

#include <QJsonArray>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace imageeditor::ui::detail {
namespace {
void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
double number(const QJsonValue& value)
{
    const auto result = value.toDouble(std::numeric_limits<double>::quiet_NaN());
    require(value.isDouble() && std::isfinite(result), "Invalid adjustment parameter number");
    return result;
}
bool boolean(const QJsonValue& value)
{
    require(value.isBool(), "Invalid adjustment enable or option state");
    return value.toBool();
}
QJsonArray array(const QJsonValue& value, qsizetype count)
{
    require(value.isArray() && value.toArray().size() == count, "Invalid adjustment channel count");
    return value.toArray();
}
QJsonObject object(const QJsonValue& value)
{
    require(value.isObject(), "Missing or invalid adjustment parameters");
    return value.toObject();
}
std::uint32_t dimension(const QJsonValue& value)
{
    const auto result = number(value);
    require(result >= 1 && result <= 32768 && std::floor(result) == result, "Invalid adjustment mask dimension");
    return std::uint32_t(result);
}
std::uint8_t byte(const QJsonValue& value)
{
    const auto result = number(value);
    require(result >= 0 && result <= 255 && std::floor(result) == result, "Invalid adjustment tint channel");
    return std::uint8_t(result);
}
QJsonObject parameters(const core::Adjustment& adjustment)
{
    using namespace core;
    switch (adjustment.type) {
    case AdjustmentType::Exposure:
        return {{"stops", std::get<ExposureParameters>(adjustment.parameters).stops}};
    case AdjustmentType::BrightnessContrast: {
        const auto& p = std::get<BrightnessContrastParameters>(adjustment.parameters);
        return {{"brightness", p.brightness}, {"contrast", p.contrast}};
    }
    case AdjustmentType::Levels: {
        QJsonArray channels;
        for (const auto& p : std::get<LevelsParameters>(adjustment.parameters).channels)
            channels.append(QJsonObject {{"inputBlack", p.inputBlack}, {"gamma", p.gamma},
                {"inputWhite", p.inputWhite}, {"outputBlack", p.outputBlack}, {"outputWhite", p.outputWhite}});
        return {{"channels", channels}};
    }
    case AdjustmentType::Curves: {
        QJsonArray channels;
        for (const auto& channel : std::get<CurvesParameters>(adjustment.parameters).channels) {
            QJsonArray points;
            for (const auto& point : channel.points) points.append(QJsonArray {point.input, point.output});
            channels.append(points);
        }
        return {{"channels", channels}};
    }
    case AdjustmentType::HueSaturation: {
        const auto& p = std::get<HueSaturationParameters>(adjustment.parameters);
        QJsonArray ranges;
        for (const auto& r : p.ranges)
            ranges.append(QJsonObject {{"hue", r.hue}, {"saturation", r.saturation}, {"lightness", r.lightness}});
        return {{"ranges", ranges}, {"colorize", p.colorize}, {"colorizeHue", p.colorizeHue},
            {"colorizeSaturation", p.colorizeSaturation}};
    }
    case AdjustmentType::Vibrance:
        return {{"amount", std::get<VibranceParameters>(adjustment.parameters).amount}};
    case AdjustmentType::ColorBalance: {
        const auto& p = std::get<ColorBalanceParameters>(adjustment.parameters);
        QJsonArray tones;
        for (const auto& t : p.tones) tones.append(QJsonArray {t[0], t[1], t[2]});
        return {{"tones", tones}, {"preserveLuminosity", p.preserveLuminosity}};
    }
    case AdjustmentType::WarmthTint: {
        const auto& p = std::get<WarmthTintParameters>(adjustment.parameters);
        return {{"warmth", p.warmth}, {"tint", p.tint}};
    }
    case AdjustmentType::BlackWhite: {
        const auto& p = std::get<BlackWhiteParameters>(adjustment.parameters);
        QJsonArray contributions;
        for (const auto contribution : p.contributions) contributions.append(contribution);
        return {{"contributions", contributions}, {"tintStrength", p.tintStrength},
            {"tintRgba", QJsonArray {p.tintColor.red, p.tintColor.green, p.tintColor.blue, p.tintColor.alpha}}};
    }
    case AdjustmentType::Invert: return {};
    }
    throw std::runtime_error("Unsupported essential adjustment type");
}
core::AdjustmentParameters parameters(core::AdjustmentType type, const QJsonObject& o)
{
    using namespace core;
    switch (type) {
    case AdjustmentType::Exposure: return ExposureParameters {number(o["stops"])};
    case AdjustmentType::BrightnessContrast:
        return BrightnessContrastParameters {number(o["brightness"]), number(o["contrast"])};
    case AdjustmentType::Levels: {
        LevelsParameters p;
        const auto channels = array(o["channels"], 4);
        for (std::size_t i = 0; i < p.channels.size(); ++i) {
            const auto c = object(channels[qsizetype(i)]);
            p.channels[i] = {number(c["inputBlack"]), number(c["gamma"]), number(c["inputWhite"]),
                number(c["outputBlack"]), number(c["outputWhite"])};
        }
        return p;
    }
    case AdjustmentType::Curves: {
        CurvesParameters p;
        const auto channels = array(o["channels"], 4);
        for (std::size_t i = 0; i < p.channels.size(); ++i) {
            require(channels[qsizetype(i)].isArray(), "Missing curve control points");
            const auto points = channels[qsizetype(i)].toArray();
            require(points.size() >= 2 && points.size() <= 16, "Invalid curve control-point count");
            p.channels[i].points.clear();
            for (const auto& value : points) {
                const auto point = array(value, 2);
                p.channels[i].points.push_back({number(point[0]), number(point[1])});
            }
        }
        return p;
    }
    case AdjustmentType::HueSaturation: {
        HueSaturationParameters p;
        const auto ranges = array(o["ranges"], 7);
        for (std::size_t i = 0; i < p.ranges.size(); ++i) {
            const auto r = object(ranges[qsizetype(i)]);
            p.ranges[i] = {number(r["hue"]), number(r["saturation"]), number(r["lightness"])};
        }
        p.colorize = boolean(o["colorize"]);
        p.colorizeHue = number(o["colorizeHue"]);
        p.colorizeSaturation = number(o["colorizeSaturation"]);
        return p;
    }
    case AdjustmentType::Vibrance: return VibranceParameters {number(o["amount"])};
    case AdjustmentType::ColorBalance: {
        ColorBalanceParameters p;
        const auto tones = array(o["tones"], 3);
        for (std::size_t i = 0; i < p.tones.size(); ++i) {
            const auto tone = array(tones[qsizetype(i)], 3);
            p.tones[i] = {number(tone[0]), number(tone[1]), number(tone[2])};
        }
        p.preserveLuminosity = boolean(o["preserveLuminosity"]);
        return p;
    }
    case AdjustmentType::WarmthTint: return WarmthTintParameters {number(o["warmth"]), number(o["tint"])};
    case AdjustmentType::BlackWhite: {
        BlackWhiteParameters p;
        const auto contributions = array(o["contributions"], 6);
        for (std::size_t i = 0; i < p.contributions.size(); ++i) p.contributions[i] = number(contributions[qsizetype(i)]);
        const auto color = array(o["tintRgba"], 4);
        p.tintColor = {byte(color[0]), byte(color[1]), byte(color[2]), byte(color[3])};
        p.tintStrength = number(o["tintStrength"]);
        return p;
    }
    case AdjustmentType::Invert: return InvertParameters {};
    }
    throw std::runtime_error("Unsupported essential adjustment type");
}
} // namespace

QString adjustmentMaskPath(core::LayerId id, core::AdjustmentType type)
{
    return QStringLiteral("adjustments/%1/%2.r8").arg(id)
        .arg(QString::fromUtf8(core::adjustmentIdentifier(type).data(), qsizetype(core::adjustmentIdentifier(type).size())));
}
QJsonObject encodeAdjustments(const core::AdjustmentStack& stack, core::LayerId id)
{
    std::string error;
    require(core::validAdjustments(stack, &error), "Invalid adjustment stack; project was not saved");
    QJsonArray items;
    for (const auto& adjustment : stack.items) {
        const auto identifier = core::adjustmentIdentifier(adjustment.type);
        QJsonObject item {{"type", QString::fromUtf8(identifier.data(), qsizetype(identifier.size()))},
            {"algorithmVersion", int(stack.algorithmVersion)}, {"enabled", adjustment.enabled},
            {"parameters", parameters(adjustment)}};
        if (adjustment.mask) {
            const auto e = adjustment.mask->coverage->extent();
            const auto& t = adjustment.mask->localToMask;
            item["mask"] = QJsonObject {{"width", int(e.width)}, {"height", int(e.height)},
                {"path", adjustmentMaskPath(id, adjustment.type)},
                {"localToMask", encodeTransform(t)}};
        }
        items.append(item);
    }
    return {{"version", 1}, {"algorithmVersion", int(stack.algorithmVersion)}, {"items", items}};
}
AdjustmentPlan decodeAdjustments(const QJsonObject& o, core::LayerId id)
{
    require(o["version"].isDouble() && o["version"].toDouble() == 1, "Unsupported adjustment descriptor version");
    require(o["algorithmVersion"].isDouble() && o["algorithmVersion"].toDouble() == core::adjustmentAlgorithmVersion,
        "Unsupported adjustment algorithm version");
    const auto items = array(o["items"], qsizetype(core::adjustmentCount));
    AdjustmentPlan result {std::make_shared<core::AdjustmentStack>(), {}};
    for (std::size_t i = 0; i < core::adjustmentCount; ++i) {
        const auto item = object(items[qsizetype(i)]);
        require(item["type"].isString(), "Missing essential adjustment type");
        const auto type = core::adjustmentFromIdentifier(item["type"].toString().toStdString());
        require(type.has_value(), "Unsupported essential adjustment type; project was not opened");
        require(*type == core::allAdjustmentTypes[i], "Unsupported adjustment order or duplicate adjustment type");
        require(item["algorithmVersion"].isDouble() && item["algorithmVersion"].toDouble() == core::adjustmentAlgorithmVersion,
            "Unsupported essential adjustment algorithm version");
        auto& adjustment = result.stack->items[i];
        adjustment.enabled = boolean(item["enabled"]);
        adjustment.parameters = parameters(*type, object(item["parameters"]));
        if (item.contains("mask")) {
            const auto mask = object(item["mask"]);
            const core::Extent2u e {dimension(mask["width"]), dimension(mask["height"])};
            require(std::uint64_t(e.width) * e.height <= 64ULL * 1024 * 1024, "Adjustment mask exceeds 64 megapixel limit");
            const auto path = adjustmentMaskPath(id, *type);
            require(mask["path"].isString() && mask["path"].toString() == path, "Invalid adjustment mask payload path");
            const auto transform = decodeTransform(mask["localToMask"]);
            result.masks.push_back({i, path, e, transform});
        }
    }
    require(core::validAdjustments(*result.stack), "Invalid adjustment parameters; project was not opened");
    return result;
}
} // namespace imageeditor::ui::detail
