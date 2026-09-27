#pragma once

#include "imageeditor/core/Document.hpp"

#include <QDialog>

class QDoubleSpinBox;
class QComboBox;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QLabel;

namespace imageeditor::ui {

class RecentFiles;

class NewDocumentDialog final : public QDialog {
public:
    enum class Mode {
        CreateDocument,
        ChangeCanvasSize,
    };
    enum Result { OpenImage = 2, OpenProject = 3, OpenRecent = 4, OpenDropped = 5 };

    explicit NewDocumentDialog(QWidget* parent = nullptr);
    NewDocumentDialog(Mode mode, core::CanvasSpec initialSpec, QWidget* parent = nullptr);

    [[nodiscard]] core::CanvasSpec canvasSpec() const;
    [[nodiscard]] core::Rgba8 backgroundColor() const;
    // The caller owns the recent-files store and handles the returned open
    // request. The dialog never reads image/project content or mutates a document.
    void setRecentFiles(RecentFiles* recentFiles);
    void refreshRecentFiles();
    [[nodiscard]] QString selectedRecentPath() const { return selectedRecentPath_; }
    [[nodiscard]] QStringList droppedFiles() const { return droppedFiles_; }
    void openDroppedFiles(const QStringList& paths);

protected:
    void dragEnterEvent(QDragEnterEvent*) override;
    void dropEvent(QDropEvent*) override;

private:
    void activateRecentFile(QListWidgetItem* item);
    Mode mode_ {Mode::CreateDocument};
    QDoubleSpinBox* width_ {nullptr};
    QDoubleSpinBox* height_ {nullptr};
    QDoubleSpinBox* dpi_ {nullptr};
    QComboBox* background_ {nullptr};
    RecentFiles* recentFiles_ {nullptr};
    QWidget* recentSection_ {nullptr};
    QListWidget* recentList_ {nullptr};
    QLabel* recentEmptyLabel_ {nullptr};
    QPushButton* clearRecentButton_ {nullptr};
    QString selectedRecentPath_;
    QStringList droppedFiles_;
    bool openingRecent_ {false};
};

} // namespace imageeditor::ui
