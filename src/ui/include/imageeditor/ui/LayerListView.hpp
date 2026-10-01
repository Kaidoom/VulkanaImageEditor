#pragma once

#include "imageeditor/core/Layer.hpp"

#include <QListView>
#include <QPersistentModelIndex>
#include <QPointer>
#include <array>
#include <functional>

class QEvent;
class QMouseEvent;
class QLineEdit;

namespace imageeditor::ui {

// Keeps layer visibility independent from Qt's checkbox double-click state
// machine. Every physical left-button press on the eye is one toggle, while
// session callbacks own row selection while Qt retains drag/reorder mechanics.
class LayerListView final : public QListView {
public:
    explicit LayerListView(QWidget* parent = nullptr);
    std::function<void(int, Qt::KeyboardModifiers)> onRowSelectionRequested;
    std::function<void(int)> onContextSelectionRequested;
    std::function<void(int,bool)> onEditingTargetRequested;
    [[nodiscard]] QRect thumbnailRect(const QModelIndex&,bool mask) const;
    std::function<void()> onEmptySpacePressed;
    // MainWindow owns the cross-native-surface keyboard bridge and global
    // click-away completion; the row owns this ordinary Qt text editor.
    std::function<void(QLineEdit*)> onRenameEditorChanged;
    bool beginRename(core::LayerId);
    void finishRename(bool commit = true);
    [[nodiscard]] QLineEdit* renameEditor() const noexcept;
    void setModel(QAbstractItemModel*) override;

    [[nodiscard]] QRect visibilityIndicatorRect(
        const QModelIndex& index) const;

protected:
    bool event(QEvent* event) override;
    bool eventFilter(QObject*, QEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void scrollContentsBy(int dx, int dy) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent*) override;
    void dragMoveEvent(QDragMoveEvent*) override;
    void dragLeaveEvent(QDragLeaveEvent*) override;
    void dropEvent(QDropEvent*) override;
    void paintEvent(QPaintEvent*) override;
    void startDrag(Qt::DropActions) override;
    QItemSelectionModel::SelectionFlags selectionCommand(const QModelIndex&, const QEvent*) const override;

private:
    bool beginVisibilityGesture(QMouseEvent* event);
    void cancelVisibilityGesture() noexcept;
    void updateRenameGeometry();

    bool visibilityGestureActive_ { false };
    bool disclosureGesture_ {false};
    QRect dropHint_;
    QString dropText_;
    int dropRow_ {-1};
    QModelIndex dropParent_;
    QPersistentModelIndex deferredSingleIndex_;
    QPointer<QLineEdit> renameEditor_;
    core::LayerId renameId_ {0};
    QString renameOriginal_;
    std::array<QMetaObject::Connection, 4> renameModelConnections_;
};

} // namespace imageeditor::ui
