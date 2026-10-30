#include "imageeditor/mcp/Protocol.hpp"
#include <QJsonDocument>
#include <QJsonParseError>
#include <cmath>

namespace imageeditor::mcp {
namespace {
QJsonObject error(const QJsonValue& id, int code, const QString& message)
{
    return {{"jsonrpc", "2.0"}, {"id", id},
        {"error", QJsonObject {{"code", code}, {"message", message}}}};
}
QJsonObject result(const QJsonValue& id, QJsonObject value)
{
    return {{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(value)}};
}
bool validId(const QJsonValue& id)
{
    return (id.isString() && id.toString().size() <= 256)
        || (id.isDouble() && std::isfinite(id.toDouble()) && std::floor(id.toDouble()) == id.toDouble()
            && std::abs(id.toDouble()) <= 9007199254740991.0);
}
bool onlyKeys(const QJsonObject& object, const QStringList& keys)
{
    for (auto it = object.begin(); it != object.end(); ++it) if (!keys.contains(it.key())) return false;
    return true;
}
} // namespace

ProtocolSession::ProtocolSession(ToolRegistry& registry, QString serverVersion)
    : registry_(registry), version_(std::move(serverVersion)) {}

std::optional<QJsonObject> ProtocolSession::handleLine(const QByteArray& line)
{
    if (line.size() > maximumMessageBytes) return error({}, -32600, "MCP message exceeds 8MiB limit");
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(line, &parse);
    if (parse.error != QJsonParseError::NoError) return error({}, -32700, "Invalid JSON");
    if (!document.isObject()) return error({}, -32600, "Expected one JSON-RPC request, not a batch");
    const auto request = document.object();
    const bool hasId = request.contains("id");
    const auto id = request.value("id");
    if (request.value("jsonrpc") != QJsonValue("2.0") || !request.value("method").isString()
        || (hasId && !validId(id))
        || !onlyKeys(request, {"jsonrpc", "id", "method", "params"}))
        return error(validId(id) ? id : QJsonValue(), -32600, "Invalid JSON-RPC request");
    if (request.contains("params") && !request.value("params").isObject())
        return hasId ? std::optional(error(id, -32602, "params must be an object")) : std::nullopt;
    const auto method = request.value("method").toString();
    const auto params = request.value("params").toObject();
    if (!hasId) {
        if (method == "notifications/initialized" && negotiated_) ready_ = true;
        // Notifications never invoke editing tools. Synchronous operations are
        // atomic: a late cancellation does not reverse an already applied edit.
        return {};
    }
    const auto idKey = QJsonDocument(QJsonArray {id}).toJson(QJsonDocument::Compact);
    if (recentIds_.contains(idKey)) return error(id, -32600, "Duplicate request ID; use a new ID for every request");
    // A finite connection budget bounds the replay ledger without ever allowing
    // an old request ID to execute again after eviction. Reconnect to continue.
    if (recentIds_.size() >= 65536) return error(id, -32000, "Session request limit reached; reconnect");
    recentIds_.insert(idKey);
    if (method == "ping") return result(id, {});
    if (method == "initialize") {
        if (negotiated_) return error(id, -32600, "This connection is already initialized");
        if (!params.value("protocolVersion").isString() || !params.value("capabilities").isObject()
            || !params.value("clientInfo").isObject()
            || !params.value("clientInfo").toObject().value("name").isString()
            || !params.value("clientInfo").toObject().value("version").isString())
            return error(id, -32602, "initialize requires protocolVersion, capabilities and clientInfo name/version");
        const auto requested = params.value("protocolVersion").toString();
        const QString protocol = requested == "2025-06-18" || requested == "2024-11-05"
            ? requested : QStringLiteral("2025-11-25");
        negotiated_ = true;
        return result(id, {{"protocolVersion", protocol},
            {"capabilities", QJsonObject {{"tools", QJsonObject {{"listChanged", false}}}}},
            {"serverInfo", QJsonObject {{"name", "vulkana"}, {"version", version_}}},
            {"instructions", "Query editor state before editing. Use returned document/layer identifiers and revision guards. "
                "Coordinates are document pixels unless a tool says otherwise. Busy/modal UI is reported, never automatically dismissed. "
                "No shell execution or application settings are exposed. Tool errors do not imply that an edit succeeded."}});
    }
    if (!ready_) return error(id, -32002, "Initialize and send notifications/initialized before using tools");
    if (method == "tools/list") {
        if (!onlyKeys(params, {"cursor", "_meta"})
            || (params.contains("cursor") && (!params.value("cursor").isString() || !params.value("cursor").toString().isEmpty())))
            return error(id, -32602, "This server returns its full tool list; no cursor is supported");
        return result(id, {{"tools", registry_.list()}});
    }
    if (method == "tools/call") {
        if (!onlyKeys(params, {"name", "arguments", "_meta"}) || !params.value("name").isString()
            || (params.contains("arguments") && !params.value("arguments").isObject()))
            return error(id, -32602, "tools/call requires a tool name and object arguments");
        const auto name = params.value("name").toString();
        if (!registry_.contains(name)) return error(id, -32602, "Unknown tool: " + name + "; call tools/list for available tool names");
        return result(id, registry_.call(name, params.value("arguments").toObject()));
    }
    return error(id, -32601, "Method not found: " + method);
}

} // namespace imageeditor::mcp
