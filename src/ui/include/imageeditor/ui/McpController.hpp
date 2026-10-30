#pragma once

#include "imageeditor/core/EditorSession.hpp"
#include "imageeditor/mcp/LocalServer.hpp"
#include "imageeditor/mcp/ToolRegistry.hpp"
#include <QObject>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <functional>

class QLabel;
class QPushButton;
namespace imageeditor::ui {
class MainWindow;

// All document automation enters through this boundary on the GUI thread.
// Individual tool modules use ordinary commands and share the same guards.
class McpController final : public QObject {
public:
    enum class Status { Stopped, Listening, Connected, Stopping, Error };
    explicit McpController(MainWindow&);
    ~McpController() override;
    bool start(const QString& socketPath, QString* error = nullptr);
    void stop();
    [[nodiscard]] Status status() const;
    [[nodiscard]] QString error() const { return error_; }
    [[nodiscard]] QString endpoint() const { return server_.socketName(); }
    [[nodiscard]] bool running() const { return server_.listening() || server_.stopping(); }
    [[nodiscard]] qsizetype clientCount() const { return server_.clientCount(); }
    [[nodiscard]] mcp::ToolRegistry& registry() { return registry_; }
    [[nodiscard]] QJsonObject state(bool includeLayers = true) const;

private:
    enum class Access { Query, Session, Document, DocumentQuery, Interaction };
    using Handler = std::function<QJsonObject(const QJsonObject&)>;
    void add(QString name, QString description, QJsonObject properties,
        QJsonArray required, Access, Handler, bool destructive = false);
    [[nodiscard]] QJsonArray blockers() const;
    [[nodiscard]] core::EditorSession& session() const;
    [[nodiscard]] core::Document& document() const;
    [[nodiscard]] core::Layer& layer(const QJsonObject&, const char* key = "layerId") const;
    bool execute(std::unique_ptr<core::Command>);
    void changed(bool layers = true);
    [[nodiscard]] QJsonObject result(bool changed = true) const;
    void registerDocumentTools();
    void registerLayerTools();
    void registerPaintingTools();
    void registerLayoutTools();
    void refreshStatus();
    MainWindow& window_;
    mcp::ToolRegistry registry_;
    mcp::LocalServer server_;
    QPointer<QLabel> indicator_;
    QPointer<QPushButton> stopButton_;
    QString error_;
    bool executing_ {false};
};
} // namespace imageeditor::ui
