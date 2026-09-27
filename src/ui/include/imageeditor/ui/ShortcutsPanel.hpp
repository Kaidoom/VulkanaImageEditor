#pragma once
#include "imageeditor/ui/EditorShortcuts.hpp"
#include <QWidget>
#include <functional>
class QTreeWidget;
class QLabel;
class QPushButton;
namespace imageeditor::ui {
// Capture and conflict confirmation are inline children of Preferences.
class ShortcutsPanel final : public QWidget {
public:
    explicit ShortcutsPanel(ShortcutBindings, QWidget* parent = nullptr);
    std::function<void(const ShortcutBindings&)> onChanged;
    bool pending() const { return capturing_ || !pendingId_.isEmpty(); }
    void cancelPending();
    const ShortcutBindings& bindings() const { return bindings_; }
protected:
    bool eventFilter(QObject*, QEvent*) override;
private:
    void refresh();
    void capture();
    void propose(const QKeySequence&);
    void assign(bool clearConflicts);
    QString selectedId() const;
    ShortcutBindings bindings_;
    QTreeWidget* commands_{};
    QLabel* notice_{};
    QPushButton* confirm_{};
    QPushButton* cancel_{};
    bool capturing_{};
    QString pendingId_;
    QKeySequence pendingKey_;
};
}
