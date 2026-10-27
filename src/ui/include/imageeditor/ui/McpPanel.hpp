#pragma once
#include "imageeditor/platform/McpSettings.hpp"
#include <QPointer>
#include <QTimer>
#include <QWidget>
#include <functional>
class QCheckBox;
class QLineEdit;
class QLabel;
class QPushButton;
namespace imageeditor::ui {
class McpController;
class McpPanel final : public QWidget {
public:
    explicit McpPanel(QWidget* parent = nullptr);
    void setController(McpController*);
    void setSettings(const platform::McpSettings&);
    platform::McpSettings settings() const;
    bool validate(QString* error = nullptr) const;
    void refreshStatus();
    std::function<void()> onChanged;
private:
    void changed();
    QString connectionEndpoint() const;
    QPointer<McpController> controller_;
    QCheckBox *autoStart_{}, *custom_{};
    QLineEdit *endpoint_{}, *command_{}, *connection_{};
    QLabel *status_{}, *error_{}, *pending_{}, *copied_{};
    QPushButton *toggle_{}, *copy_{};
    QTimer timer_;
};
}
