#pragma once
#include "imageeditor/ui/PdfImport.hpp"
#include <QDialog>
#include <memory>

namespace imageeditor::ui {
// A card hosted by WorkspaceDialog; never a separate native application window.
class PdfImportDialog final : public QDialog {
public:
    PdfImportDialog(QString path, bool currentAvailable, bool intoCurrent, PdfLimits,
        QWidget* parent = nullptr);
    ~PdfImportDialog() override;
    [[nodiscard]] PdfOptions options() const;
    [[nodiscard]] PdfPlan plan() const;
    PdfRenderedPages takePages();
    void reject() override;
protected:
    bool eventFilter(QObject*, QEvent*) override;
private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace imageeditor::ui
