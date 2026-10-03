#pragma once
#include "imageeditor/ui/PsdExport.hpp"
#include <QWidget>
class QComboBox;
class QTreeWidget;
class QCheckBox;
class QLabel;
namespace imageeditor::ui {
class PsdExportPanel final : public QWidget {
public:
  PsdExportPanel(PsdExportOptions &, QWidget *parent, QWidget *itemsParent);
  void setPlan(const PsdExportPlan &);
  void setPsdVisible(bool);
  void reset();
  QWidget *itemsWidget() const;
  std::function<void()> changed;

private:
  void notify();
  void selectionChanged();
  void updateDescription();
  void filter();
  PsdExportOptions &options_;
  PsdExportPlan plan_;
  QWidget *items_;
  QComboBox *mode_, *text_, *action_;
  QTreeWidget *tree_;
  QCheckBox *attention_, *hideGroups_;
  QLabel *detail_, *summary_;
  bool updating_{false};
};
} // namespace imageeditor::ui
