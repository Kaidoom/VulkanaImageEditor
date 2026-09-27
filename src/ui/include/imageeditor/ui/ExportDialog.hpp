#pragma once
#include "imageeditor/ui/ImageExport.hpp"
#include <QDialog>
#include <functional>
#include <memory>
namespace imageeditor::ui {
// Controls/display only. Source access, job cancellation, encoding and atomic
// publication belong to the owner coordinator, never to a paint event.
class ExportDialog final : public QDialog {
public:
    ExportDialog(ExportSettings initial, QSize canvasSize, QWidget* parent = nullptr);
    ~ExportDialog() override;
    [[nodiscard]] ExportSettings settings() const;
    void setDestination(const QString&);
    void setProgress(const QString& stage, bool writing = false);
    void setPreview(const QImage& decoded, qint64 actualBytes);
    void setError(const QString&);
    void setExported(const QString& destination, QSize size, qint64 bytes);
    std::function<void()> onSettingsChanged;
    std::function<void()> onExportRequested;
    std::function<void()> onCancelRequested;
    void reject() override;

private:
    bool eventFilter(QObject*, QEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void closeSuccessOverlay();
    void openExportedLocation(bool image);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace imageeditor::ui
