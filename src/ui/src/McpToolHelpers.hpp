#pragma once
#include <QColor>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <cstdint>
#include <stdexcept>
#include "imageeditor/core/Geometry.hpp"

namespace imageeditor::ui::mcpdetail {
inline void require(bool value, const QString& error)
{ if (!value) throw std::invalid_argument(error.toStdString()); }
inline QString id(std::uint64_t value) { return QString::number(value); }
inline std::uint64_t id(const QJsonObject& args, const char* key)
{
    const auto value = args.value(QLatin1String(key));
    bool ok = false;
    const auto result = value.toString().toULongLong(&ok);
    require(value.isString() && ok && result != 0, QStringLiteral("Expected a nonzero decimal-string ID: ") + QLatin1String(key));
    return result;
}
inline QJsonObject string(QString description = {})
{ return {{"type", "string"}, {"description", description}, {"maxLength", 65536}}; }
inline QJsonObject identifier(QString description = {})
{ return {{"type", "string"}, {"description", description}, {"pattern", "^[1-9][0-9]{0,19}$"}}; }
inline QJsonObject number(double minimum, double maximum, QString description = {})
{ return {{"type", "number"}, {"minimum", minimum}, {"maximum", maximum}, {"description", description}}; }
inline QJsonObject integer(int minimum, int maximum, QString description = {})
{ auto schema = number(minimum, maximum, description); schema["type"] = "integer"; return schema; }
inline QJsonObject boolean(QString description = {})
{ return {{"type", "boolean"}, {"description", description}}; }
inline QJsonObject choice(QJsonArray values, QString description = {})
{ return {{"type", "string"}, {"enum", values}, {"description", description}}; }
inline QJsonObject object(QJsonObject properties, QJsonArray required = {})
{ return {{"type", "object"}, {"properties", properties}, {"required", required}, {"additionalProperties", false}}; }
inline QJsonObject array(QJsonObject items, int minimum = 0, int maximum = 4096)
{ return {{"type", "array"}, {"items", items}, {"minItems", minimum}, {"maxItems", maximum}}; }
inline QJsonObject colorSchema()
{ return {{"type", "string"}, {"pattern", "^#[0-9a-fA-F]{6}([0-9a-fA-F]{2})?$"}, {"description", "sRGB #RRGGBB or #RRGGBBAA (alpha last)."}}; }
inline core::Rgba8 color(const QString& value)
{
    require(value.size() == 7 || value.size() == 9, "Expected #RRGGBB or #RRGGBBAA");
    bool ok = false;
    const auto bits = value.mid(1).toULongLong(&ok, 16);
    require(value.startsWith('#') && ok, "Invalid color");
    if (value.size() == 7) return {std::uint8_t(bits >> 16), std::uint8_t(bits >> 8), std::uint8_t(bits), 255};
    return {std::uint8_t(bits >> 24), std::uint8_t(bits >> 16), std::uint8_t(bits >> 8), std::uint8_t(bits)};
}
inline QString color(core::Rgba8 value)
{ return QStringLiteral("#%1%2%3%4").arg(value.red, 2, 16, QChar('0')).arg(value.green, 2, 16, QChar('0')).arg(value.blue, 2, 16, QChar('0')).arg(value.alpha, 2, 16, QChar('0')); }
} // namespace imageeditor::ui::mcpdetail
