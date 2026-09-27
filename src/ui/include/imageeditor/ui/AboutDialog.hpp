#pragma once
#include <QDialog>
#include "imageeditor/ui/UpdateService.hpp"
#include <functional>

class QNetworkAccessManager;

namespace imageeditor::ui {
class UpdateService;
class AboutDialog final : public QDialog {
public:
    static constexpr int NoticesRequested = 100;
    explicit AboutDialog(QWidget* parent, QNetworkAccessManager* transport = nullptr,
        UpdatePackage package = UpdateService::buildPackage(), QString appImagePath = {},
        UpdateService* sharedUpdates = nullptr);
    ~AboutDialog() override;
    std::function<void(const QString&)> onRestartRequested;
private:
    void refreshLayout();
    UpdateService* updates_;
};
} // namespace imageeditor::ui
