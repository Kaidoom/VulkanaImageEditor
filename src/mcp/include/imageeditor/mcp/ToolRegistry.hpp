#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <functional>
#include <vector>

namespace imageeditor::mcp {

// Handlers execute on the editor thread. Return a complete MCP tool result;
// domain failures use isError rather than throwing a JSON-RPC protocol error.
struct Tool {
    QString name;
    QString description;
    QJsonObject inputSchema;
    QJsonObject annotations;
    std::function<QJsonObject(const QJsonObject&)> handler;
};

[[nodiscard]] QJsonObject toolSuccess(QJsonObject data, const QString& message = {});
[[nodiscard]] QJsonObject toolFailure(const QString& code, const QString& message,
    QJsonObject details = {});
// Validates the supported JSON Schema subset, recursively and without coercion.
// Unsupported schema keywords are rejected at registration, not silently ignored.
[[nodiscard]] QString validateArguments(const QJsonObject& schema, const QJsonObject& arguments);

class ToolRegistry {
public:
    void add(Tool tool);
    [[nodiscard]] QJsonArray list() const;
    [[nodiscard]] bool contains(const QString& name) const;
    [[nodiscard]] QJsonObject call(const QString& name, const QJsonObject& arguments) const;
private:
    std::vector<Tool> tools_;
};

} // namespace imageeditor::mcp
