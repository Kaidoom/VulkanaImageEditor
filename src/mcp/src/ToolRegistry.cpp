#include "imageeditor/mcp/ToolRegistry.hpp"
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <cmath>
#include <stdexcept>

namespace imageeditor::mcp {
namespace {
QString invalid(const QString& path, const QString& reason)
{
    return path + ": " + reason;
}
bool typeMatches(const QString& type, const QJsonValue& value)
{
    if (type == "object") return value.isObject();
    if (type == "array") return value.isArray();
    if (type == "string") return value.isString();
    if (type == "boolean") return value.isBool();
    if (type == "null") return value.isNull();
    if (type == "number") return value.isDouble() && std::isfinite(value.toDouble());
    if (type == "integer") return value.isDouble() && std::isfinite(value.toDouble())
        && std::floor(value.toDouble()) == value.toDouble();
    return false;
}
QString check(const QJsonObject& schema, const QJsonValue& value, const QString& path, int depth)
{
    if (depth > 32) return invalid(path, "maximum nesting depth exceeded");
    if (schema.contains("type")) {
        const auto type = schema.value("type");
        bool matches = type.isString() && typeMatches(type.toString(), value);
        for (const auto item : type.toArray()) matches |= typeMatches(item.toString(), value);
        if (!matches) return invalid(path, "unexpected value type");
    }
    if (schema.contains("enum") && !schema.value("enum").toArray().contains(value))
        return invalid(path, "value is not one of the supported choices");
    if (schema.contains("const") && schema.value("const") != value)
        return invalid(path, "value does not match the required constant");
    for (const auto* combinator : {"anyOf", "oneOf", "allOf"}) {
        if (!schema.contains(combinator)) continue;
        const auto alternatives = schema.value(combinator).toArray();
        qsizetype matches = 0;
        for (const auto alternative : alternatives)
            if (check(alternative.toObject(), value, path, depth + 1).isEmpty()) ++matches;
        if ((QStringView(u"anyOf") == QLatin1StringView(combinator) && matches == 0)
            || (QStringView(u"oneOf") == QLatin1StringView(combinator) && matches != 1)
            || (QStringView(u"allOf") == QLatin1StringView(combinator) && matches != alternatives.size()))
            return invalid(path, QStringLiteral("value does not satisfy %1").arg(combinator));
    }
    if (value.isObject()) {
        const auto object = value.toObject();
        const auto properties = schema.value("properties").toObject();
        for (const auto required : schema.value("required").toArray())
            if (!object.contains(required.toString()))
                return invalid(path + '.' + required.toString(), "required argument is missing");
        // An unconstrained combinator branch must not override its sibling's
        // object validation. Ordinary object schemas are closed by default.
        const bool alternatives = schema.contains("anyOf") || schema.contains("oneOf") || schema.contains("allOf");
        const bool constrained = schema.contains("properties") || schema.contains("additionalProperties")
            || (schema.value("type") == QJsonValue("object") && !alternatives);
        for (auto it = object.begin(); it != object.end(); ++it) {
            QJsonObject child;
            if (properties.contains(it.key())) child = properties.value(it.key()).toObject();
            else if (schema.value("additionalProperties").isObject())
                child = schema.value("additionalProperties").toObject();
            else if (constrained && !schema.value("additionalProperties").toBool(false))
                return invalid(path + '.' + it.key(), "unknown argument");
            const auto error = check(child, it.value(), path + '.' + it.key(), depth + 1);
            if (!error.isEmpty()) return error;
        }
    }
    if (value.isArray()) {
        const auto array = value.toArray();
        if (schema.contains("minItems") && double(array.size()) < schema.value("minItems").toDouble())
            return invalid(path, "too few items");
        if (schema.contains("maxItems") && double(array.size()) > schema.value("maxItems").toDouble())
            return invalid(path, "too many items");
        QSet<QByteArray> seen;
        for (qsizetype i = 0; i < array.size(); ++i) {
            if (schema.value("uniqueItems").toBool()) {
                const auto key = QJsonDocument(QJsonArray {array[i]}).toJson(QJsonDocument::Compact);
                if (seen.contains(key)) return invalid(path, "duplicate items are not allowed");
                seen.insert(key);
            }
            const auto error = check(schema.value("items").toObject(), array[i],
                path + '[' + QString::number(i) + ']', depth + 1);
            if (!error.isEmpty()) return error;
        }
    }
    if (value.isDouble()) {
        const double number = value.toDouble();
        if (!std::isfinite(number)) return invalid(path, "number must be finite");
        if (schema.contains("minimum") && number < schema.value("minimum").toDouble())
            return invalid(path, "number is below the minimum");
        if (schema.contains("maximum") && number > schema.value("maximum").toDouble())
            return invalid(path, "number exceeds the maximum");
        if (schema.contains("exclusiveMinimum") && number <= schema.value("exclusiveMinimum").toDouble())
            return invalid(path, "number must exceed the minimum");
        if (schema.contains("exclusiveMaximum") && number >= schema.value("exclusiveMaximum").toDouble())
            return invalid(path, "number must be below the maximum");
    }
    if (value.isString()) {
        const auto text = value.toString();
        const auto length = text.toUcs4().size();
        if (schema.contains("minLength") && double(length) < schema.value("minLength").toDouble())
            return invalid(path, "text is too short");
        if (schema.contains("maxLength") && double(length) > schema.value("maxLength").toDouble())
            return invalid(path, "text is too long");
        if (schema.contains("pattern") && !QRegularExpression(schema.value("pattern").toString()).match(text).hasMatch())
            return invalid(path, "text does not match the required pattern");
    }
    return {};
}
void checkSchema(const QJsonObject& schema, int depth = 0)
{
    if (depth > 32) throw std::invalid_argument("MCP input schema is too deeply nested");
    static const QSet<QString> keywords {"type", "properties", "required", "additionalProperties",
        "enum", "const", "items", "minItems", "maxItems", "uniqueItems", "minimum", "maximum",
        "exclusiveMinimum", "exclusiveMaximum", "minLength", "maxLength", "pattern", "anyOf",
        "oneOf", "allOf", "title", "description", "default", "$schema"};
    static const QSet<QString> types {"object", "array", "string", "boolean", "null", "number", "integer"};
    for (auto it = schema.begin(); it != schema.end(); ++it) {
        if (!keywords.contains(it.key()))
            throw std::invalid_argument(("Unsupported MCP schema keyword: " + it.key()).toStdString());
        if (it.key() == "type") {
            const auto values = it->isArray() ? it->toArray() : QJsonArray {*it};
            if (values.isEmpty()) throw std::invalid_argument("MCP schema type cannot be empty");
            for (const auto type : values)
                if (!type.isString() || !types.contains(type.toString()))
                    throw std::invalid_argument("Unsupported MCP schema type");
        } else if (it.key() == "properties") {
            if (!it->isObject()) throw std::invalid_argument("MCP schema properties must be an object");
            const auto properties = it->toObject();
            for (const auto property : properties) {
                if (!property.isObject()) throw std::invalid_argument("MCP property schema must be an object");
                checkSchema(property.toObject(), depth + 1);
            }
        } else if (it.key() == "items" || it.key() == "additionalProperties") {
            if (it.key() == "additionalProperties" && it->isBool()) continue;
            if (!it->isObject()) throw std::invalid_argument("MCP nested schema must be an object");
            checkSchema(it->toObject(), depth + 1);
        } else if (it.key() == "anyOf" || it.key() == "oneOf" || it.key() == "allOf") {
            if (!it->isArray() || it->toArray().isEmpty()) throw std::invalid_argument("Empty MCP schema alternatives");
            for (const auto branch : it->toArray()) {
                if (!branch.isObject()) throw std::invalid_argument("MCP schema alternative must be an object");
                checkSchema(branch.toObject(), depth + 1);
            }
        } else if (it.key() == "pattern") {
            if (!it->isString() || !QRegularExpression(it->toString()).isValid())
                throw std::invalid_argument("Invalid MCP schema regular expression");
        } else if (it.key() == "required") {
            if (!it->isArray()) throw std::invalid_argument("MCP schema required must be an array");
            for (const auto entry : it->toArray())
                if (!entry.isString() || !schema.value("properties").toObject().contains(entry.toString()))
                    throw std::invalid_argument("MCP required property is not declared");
        } else if (it.key() == "enum" && (!it->isArray() || it->toArray().isEmpty())) {
            throw std::invalid_argument("MCP schema enum must contain choices");
        } else if (it.key() == "uniqueItems" && !it->isBool()) {
            throw std::invalid_argument("MCP schema uniqueItems must be boolean");
        } else if (it.key().startsWith("min") || it.key().startsWith("max") || it.key().startsWith("exclusive")) {
            if (!it->isDouble() || !std::isfinite(it->toDouble()))
                throw std::invalid_argument("MCP schema bound must be a finite number");
            if ((it.key().endsWith("Items") || it.key().endsWith("Length"))
                && (it->toDouble() < 0 || std::floor(it->toDouble()) != it->toDouble()))
                throw std::invalid_argument("MCP schema item/text limits must be nonnegative integers");
        }
    }
}
} // namespace

QJsonObject toolSuccess(QJsonObject data, const QString& message)
{
    const auto text = message.isEmpty() ? QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Compact)) : message;
    return {{"content", QJsonArray {QJsonObject {{"type", "text"}, {"text", text}}}},
        {"structuredContent", std::move(data)}, {"isError", false}};
}
QJsonObject toolFailure(const QString& code, const QString& message, QJsonObject details)
{
    QJsonObject data {{"ok", false}, {"error", QJsonObject {{"code", code}, {"message", message}, {"details", std::move(details)}}}};
    auto result = toolSuccess(std::move(data), message);
    result["isError"] = true;
    return result;
}
QString validateArguments(const QJsonObject& schema, const QJsonObject& arguments)
{
    return check(schema, arguments, "arguments", 0);
}
void ToolRegistry::add(Tool tool)
{
    if (!QRegularExpression(QStringLiteral("^[A-Za-z0-9_.-]{1,128}$")).match(tool.name).hasMatch()
        || contains(tool.name) || !tool.handler || tool.description.isEmpty())
        throw std::invalid_argument("Invalid or duplicate MCP tool registration");
    if (tool.inputSchema.value("type") != QJsonValue("object"))
        throw std::invalid_argument("MCP tool input schema must have object type");
    checkSchema(tool.inputSchema);
    tools_.push_back(std::move(tool));
}
QJsonArray ToolRegistry::list() const
{
    QJsonArray result;
    for (const auto& tool : tools_) result.append(QJsonObject {{"name", tool.name},
        {"description", tool.description}, {"inputSchema", tool.inputSchema}, {"annotations", tool.annotations}});
    return result;
}
bool ToolRegistry::contains(const QString& name) const
{
    for (const auto& tool : tools_) if (tool.name == name) return true;
    return false;
}
QJsonObject ToolRegistry::call(const QString& name, const QJsonObject& arguments) const
{
    for (const auto& tool : tools_) if (tool.name == name) {
        const auto invalid = validateArguments(tool.inputSchema, arguments);
        if (!invalid.isEmpty()) return toolFailure("invalid_arguments", invalid);
        try {
            const auto result = tool.handler(arguments);
            if (!result.value("content").isArray()
                || (result.contains("isError") && !result.value("isError").isBool())
                || (result.contains("structuredContent") && !result.value("structuredContent").isObject()))
                return toolFailure("invalid_tool_result", "The tool returned an invalid MCP result; query editor state before retrying");
            return result;
        } catch (const std::exception& error) {
            return toolFailure("operation_failed", QString::fromUtf8(error.what()));
        } catch (...) {
            return toolFailure("operation_failed", "Unexpected editor operation failure");
        }
    }
    return toolFailure("unknown_tool", "Unknown tool: " + name);
}

} // namespace imageeditor::mcp
