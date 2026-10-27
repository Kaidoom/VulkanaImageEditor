#pragma once

#include "imageeditor/mcp/ToolRegistry.hpp"
#include <QSet>
#include <optional>

namespace imageeditor::mcp {

inline constexpr qsizetype maximumMessageBytes = 8 * 1024 * 1024;

// A connection has an independent handshake and request-ID namespace.
class ProtocolSession {
public:
    ProtocolSession(ToolRegistry& registry, QString serverVersion);
    [[nodiscard]] std::optional<QJsonObject> handleLine(const QByteArray& line);
    [[nodiscard]] bool initialized() const noexcept { return ready_; }
private:
    ToolRegistry& registry_;
    QString version_;
    bool negotiated_ {};
    bool ready_ {};
    QSet<QByteArray> recentIds_;
};

} // namespace imageeditor::mcp
