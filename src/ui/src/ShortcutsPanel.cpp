#include "imageeditor/ui/ShortcutsPanel.hpp"
#include <QApplication>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QTreeWidget>
#include <QVBoxLayout>
namespace imageeditor::ui {
ShortcutsPanel::ShortcutsPanel(ShortcutBindings bindings, QWidget* parent)
    : QWidget(parent), bindings_(std::move(bindings))
{
    setObjectName("ShortcutsPanel");
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);
    auto* search = new QLineEdit(this);
    search->setObjectName("ShortcutSearch"); search->setPlaceholderText(tr("Find a command…"));
    layout->addWidget(search);
    commands_ = new QTreeWidget(this);
    commands_->setObjectName("ShortcutCommands");
    commands_->setColumnCount(3); commands_->setHeaderLabels({tr("Command"), tr("Category"), tr("Binding")});
    commands_->setRootIsDecorated(false); commands_->setUniformRowHeights(true);
    commands_->setSelectionMode(QAbstractItemView::SingleSelection);
    commands_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    commands_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    commands_->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    commands_->verticalScrollBar()->setProperty("editorScrollBar", true);
    for (const auto& d : shortcutDefinitions()) {
        auto* row = new QTreeWidgetItem(commands_, {d.label, d.category, {}});
        row->setData(0, Qt::UserRole, d.id);
        row->setToolTip(0, d.label + "\n" + (d.hold ? tr("Hold to activate; release restores the previous state.")
            : d.id.startsWith("Nudge") ? tr("Shared by layers and pixel selections. Hold Shift for the larger step.")
            : d.id == "RemovePointAction" ? tr("Lasso anchor, polygon vertex, or focused curve point. Text deletion is unchanged.")
            : d.id == "FinishOperationAction" ? tr("Apply Transform / Crop or finish a lasso / polygon. Enter in text still inserts a new line.")
            : tr("Focused text fields and panel navigation keep their ordinary editing keys.")));
    }
    layout->addWidget(commands_, 1);
    auto* buttons = new QHBoxLayout;
    const auto button = [this, buttons](const char* id, const QString& text) {
        auto* b = new QPushButton(text, this); b->setObjectName(id); b->setAutoDefault(false); buttons->addWidget(b); return b;
    };
    auto* change = button("ShortcutChange", tr("Change…"));
    auto* clear = button("ShortcutClear", tr("Clear binding"));
    buttons->addStretch();
    auto* reset = button("ShortcutDefaults", tr("Restore defaults"));
    layout->addLayout(buttons);
    notice_ = new QLabel(this); notice_->setObjectName("ShortcutNotice"); notice_->setWordWrap(true);
    layout->addWidget(notice_);
    auto* conflict = new QHBoxLayout;
    confirm_ = new QPushButton(tr("Assign and clear conflicts"), this); confirm_->setObjectName("ShortcutConfirmConflict"); confirm_->setAutoDefault(false);
    cancel_ = new QPushButton(tr("Cancel binding"), this); cancel_->setAutoDefault(false);
    conflict->addWidget(confirm_); conflict->addWidget(cancel_); conflict->addStretch(); layout->addLayout(conflict);
    auto* hint = new QLabel(tr("Escape always cancels and cannot be rebound. Gesture modifiers (Shift, Alt, Ctrl) and field navigation stay fixed. The same key is allowed in non-overlapping editing contexts."), this);
    hint->setObjectName("MutedLabel"); hint->setWordWrap(true); layout->addWidget(hint);
    connect(search, &QLineEdit::textChanged, this, [this](const QString& text) {
        for (int i = 0; i < commands_->topLevelItemCount(); ++i) {
            auto* row = commands_->topLevelItem(i);
            row->setHidden(!(row->text(0) + " " + row->text(1) + " " + row->text(2)).contains(text, Qt::CaseInsensitive));
        }
    });
    connect(change, &QPushButton::clicked, this, [this] { capture(); });
    connect(commands_, &QTreeWidget::itemDoubleClicked, this, [this] { capture(); });
    connect(clear, &QPushButton::clicked, this, [this] {
        const auto id = selectedId(); cancelPending(); if (id.isEmpty()) return;
        assignShortcut(bindings_, id, {}, false); refresh(); if (onChanged) onChanged(bindings_);
    });
    connect(reset, &QPushButton::clicked, this, [this] {
        cancelPending(); bindings_ = defaultShortcutBindings(); refresh(); if (onChanged) onChanged(bindings_);
    });
    connect(confirm_, &QPushButton::clicked, this, [this] { assign(true); });
    connect(cancel_, &QPushButton::clicked, this, [this] { cancelPending(); });
    commands_->setCurrentItem(commands_->topLevelItem(0));
    refresh(); cancelPending();
    qApp->installEventFilter(this);
}
QString ShortcutsPanel::selectedId() const
{ return commands_->currentItem() ? commands_->currentItem()->data(0, Qt::UserRole).toString() : QString{}; }
void ShortcutsPanel::refresh()
{
    for (int i = 0; i < commands_->topLevelItemCount(); ++i) {
        auto* row = commands_->topLevelItem(i);
        row->setText(2, shortcutLabel(bindings_, row->data(0, Qt::UserRole).toString()));
    }
}
void ShortcutsPanel::cancelPending()
{
    capturing_ = false; pendingId_.clear(); pendingKey_ = {};
    notice_->clear(); confirm_->hide(); cancel_->hide(); commands_->setEnabled(true);
}
void ShortcutsPanel::capture()
{
    cancelPending(); pendingId_ = selectedId(); if (pendingId_.isEmpty()) return;
    capturing_ = true; commands_->setEnabled(false); cancel_->show();
    notice_->setText(tr("Press a key combination for %1. Escape cancels.").arg(shortcutDefinition(pendingId_)->label));
}
void ShortcutsPanel::propose(const QKeySequence& key)
{
    const auto error = validateShortcut(pendingId_, key);
    if (!error.isEmpty()) { notice_->setText(error + tr(" Try another key, or Escape to cancel.")); return; }
    pendingKey_ = key;
    const auto conflicts = shortcutConflicts(bindings_, pendingId_, key);
    if (conflicts.isEmpty()) { assign(false); return; }
    capturing_ = false;
    QStringList names; for (const auto& id : conflicts) names.append(shortcutDefinition(id)->label);
    notice_->setText(tr("%1 is already assigned to %2 in an overlapping context. Assigning it here clears those bindings.")
        .arg(key.toString(QKeySequence::NativeText), names.join(", ")));
    confirm_->show();
}
void ShortcutsPanel::assign(bool clearConflicts)
{
    if (pendingId_.isEmpty()) return;
    assignShortcut(bindings_, pendingId_, pendingKey_, clearConflicts);
    cancelPending(); refresh(); if (onChanged) onChanged(bindings_);
}
bool ShortcutsPanel::eventFilter(QObject*, QEvent* event)
{
    if (!isVisible() || !pending()) return false;
    if (event->type() != QEvent::ShortcutOverride && event->type() != QEvent::KeyPress && event->type() != QEvent::KeyRelease) return false;
    auto* key = static_cast<QKeyEvent*>(event);
    if (key->key() == Qt::Key_Escape) {
        if (event->type() == QEvent::KeyPress) cancelPending();
        event->accept(); return true;
    }
    if (!capturing_) return false;
    if (event->type() == QEvent::KeyPress && !key->isAutoRepeat()
        && key->key() != Qt::Key_Shift && key->key() != Qt::Key_Control && key->key() != Qt::Key_Alt
        && key->key() != Qt::Key_Meta && key->key() != Qt::Key_AltGr) propose(shortcutKey(*key));
    event->accept(); return true;
}
}
