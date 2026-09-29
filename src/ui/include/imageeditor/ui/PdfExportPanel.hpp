#pragma once
#include "imageeditor/ui/PdfExport.hpp"
#include <QWidget>
#include <memory>

namespace imageeditor::ui {
// Export-only controls. IDs come from the frozen owner, never the active Layers
// model.
class PdfExportPanel final : public QWidget {
public:
  PdfExportPanel(PdfExportOptions &, QWidget *parent, QWidget *previewParent);
  ~PdfExportPanel() override;
  QWidget *itemsWidget() const;
  QWidget *navigationWidget() const;
  void setSource(const PdfExportSnapshot &);
  void reset();
  void setPdfVisible(bool);
  void setPlan(const PdfExportPlan &);
  int previewPage() const;
  QString inputError() const;
  std::vector<core::LayerId> visibleThumbnails() const;
  void setThumbnail(core::LayerId, const QImage &);
  std::function<void()> changed, pageChanged, thumbnailsChanged;

private:
  struct State;
  std::unique_ptr<State> state_;
};
} // namespace imageeditor::ui
