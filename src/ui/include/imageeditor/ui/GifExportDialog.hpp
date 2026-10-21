#pragma once
#include "imageeditor/ui/GifExport.hpp"
#include <QDialog>
#include <memory>

namespace imageeditor::ui
{
class GifExportDialog final : public QDialog
{
  public:
    explicit GifExportDialog(core::SnapshotTimelineState, QWidget *parent);
    ~GifExportDialog() override;
    GifExportSettings settings() const;
    bool previewReady() const;
    void writeTo(const QString &path);
    void reject() override;
    std::function<void()> onExportRequested;
    std::function<void(const QString &)> onExported;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace imageeditor::ui
