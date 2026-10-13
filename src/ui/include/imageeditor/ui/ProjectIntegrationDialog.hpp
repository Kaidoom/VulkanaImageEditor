#pragma once
#include <QDialog>
namespace imageeditor::ui {
class ProjectIntegrationDialog final : public QDialog {
public:
    explicit ProjectIntegrationDialog(bool firstRun, QWidget* parent);
};
}
