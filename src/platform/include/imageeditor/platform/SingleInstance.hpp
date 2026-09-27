#pragma once

#include <QObject>
#include <QStringList>
#include <functional>
#include <memory>

namespace imageeditor::platform {
inline constexpr auto singleInstanceRestartOption = "wait-for-instance-exit";
// One editor per login user, independent of executable, package, version or
// display backend. Keep alive until after the window/device are destroyed.
class SingleInstance final : public QObject {
public:
    enum class Result { Primary, Forwarded, Error };
    // An explicit directory is a test seam; production always uses the stable
    // per-user runtime location. No preferences or document files are involved.
    explicit SingleInstance(QString directory = {}, QObject* parent = nullptr);
    ~SingleInstance() override;
    Result start(const QStringList& files, bool waitForPreviousExit = false,
        int timeoutMs = 5000);
    QString errorString() const;
    // Requests accepted during startup are drained once the UI is ready.
    void setOpenHandler(std::function<void(const QStringList&)> handler);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace imageeditor::platform
