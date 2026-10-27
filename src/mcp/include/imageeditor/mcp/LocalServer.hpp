#pragma once

#include "imageeditor/mcp/Protocol.hpp"
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QPointer>
#include <memory>
#include <QTemporaryDir>
#include <cstdint>
#include <QHash>
#include <deque>

namespace imageeditor::mcp {

// Owner-only local transport. Each connection has independent protocol state;
// all requests execute serially on this object's (editor) thread.
class LocalServer final : public QObject {
public:
    LocalServer(ToolRegistry& registry, QString serverVersion, QObject* parent = nullptr);
    ~LocalServer() override;
    bool listen(const QString& path, QString* error = nullptr);
    void close();
    [[nodiscard]] QString socketName() const;
    [[nodiscard]] bool connected() const;
    [[nodiscard]] qsizetype clientCount() const { return clients_.size(); }
    [[nodiscard]] bool listening() const { return server_.isListening(); }
    [[nodiscard]] bool stopping() const { return stopRequested_; }
    std::function<void(bool)> onConnectionChanged;
    std::function<void()> onStateChanged;
private:
    void closeListener();
    void finishStop();
    void acceptConnection();
    struct Client;
    using ClientPtr = std::shared_ptr<Client>;
    void receive(ClientPtr);
    void processReady();
    void disconnectClient(ClientPtr, const char* reason);
    ToolRegistry& registry_;
    QString version_;
    QLocalServer server_;
    std::unique_ptr<QLockFile> endpointLock_;
    QHash<QLocalSocket*, ClientPtr> clients_;
    std::deque<ClientPtr> ready_;
    std::uint64_t nextClientId_ {1};
    bool processing_ {};
    bool stopRequested_ {};
    QString endpoint_;
    std::unique_ptr<QTemporaryDir> privateListener_;
    std::uint64_t endpointDevice_ {}, endpointInode_ {};
};

// The Unix socket's parent must belong to this user, have mode0700, and not be
// a symlink. A missing immediate parent is created privately. Existing endpoints
// are never unlinked by startup, including endpoints left by a crashed process.
[[nodiscard]] bool prepareSocketDirectory(const QString& path, QString* error = nullptr);
// Pure validation for preferences; never creates/deletes endpoint resources.
[[nodiscard]] QString validateEndpoint(const QString&);
[[nodiscard]] QString normalizeEndpoint(const QString&);
[[nodiscard]] QString defaultSocketPath();

} // namespace imageeditor::mcp
