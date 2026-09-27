#pragma once

#include <QDialog>
#include "imageeditor/ui/FeedbackService.hpp"

class QCheckBox;
class QLineEdit;
class QPlainTextEdit;
class QComboBox;
class QLabel;
class QPushButton;

namespace imageeditor::ui {

// Explicit opt-in submission; confirmation stays on the same widget surface.
class FeedbackDialog final : public QDialog {
public:
    static constexpr int maximumCharacters = FeedbackService::maximumCharacters;
    explicit FeedbackDialog(QWidget* parent, const QString& systemPreview, FeedbackService* service = nullptr);
    ~FeedbackDialog() override;
    [[nodiscard]] QString feedback() const;
    [[nodiscard]] QString email() const;
    // Opt-in is enforced at the data boundary, not just visually.
    [[nodiscard]] QString includedSystemInfo() const;
    static QString basicSystemInfo(const QString& rendererDeviceName);
    void done(int result) override;

protected:
    void resizeEvent(QResizeEvent*) override;

private:
    void refreshSubmission();
    void showThanks();
    void clearFields();
    FeedbackService* service_;
    QWidget* form_;
    QWidget* thanks_ {nullptr};
    QComboBox* category_;
    QLabel* status_;
    QPushButton* submit_;
    QPlainTextEdit* feedback_;
    QLineEdit* email_;
    QCheckBox* includeSystem_;
    QPlainTextEdit* system_;
};

} // namespace imageeditor::ui
