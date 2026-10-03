#pragma once
#include "imageeditor/ui/PsdImport.hpp"
#include <QDialog>
namespace imageeditor::ui {
class PsdImportDialog final : public QDialog {
public:
  PsdImportDialog(QString, bool currentAvailable, bool intoCurrent, PsdLimits,
                  QWidget * = nullptr);
  ~PsdImportDialog() override;
  void reject() override;
  bool intoCurrent() const;
  PsdConversion takeResult();

private:
  void resizeEvent(QResizeEvent *) override;
  struct State;
  std::unique_ptr<State> state_;
};
} // namespace imageeditor::ui
