#include "imageeditor/ui/Localization.hpp"
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
    search->setObjectName("ShortcutSearch"); i18n::setPlaceholderText(search, i18n::text("shortcuts.panel.find.a.command"));
    layout->addWidget(search);
    commands_ = new QTreeWidget(this);
    commands_->setObjectName("ShortcutCommands");
    commands_->setColumnCount(3); commands_->setHeaderLabels({i18n::text("shortcuts.panel.command"), i18n::text("shortcuts.panel.category"), i18n::text("shortcuts.panel.binding")});
    commands_->setRootIsDecorated(false); commands_->setUniformRowHeights(true);
    commands_->setSelectionMode(QAbstractItemView::SingleSelection);
    commands_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    commands_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    commands_->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    commands_->verticalScrollBar()->setProperty("editorScrollBar", true);
    for (const auto& d : shortcutDefinitions()) {
        auto* row = new QTreeWidgetItem(commands_, {d.label.resolved(), d.category.resolved(), {}});
        row->setData(0, Qt::UserRole, d.id);
        row->setToolTip(0, d.label + "\n" + (d.hold ? i18n::text("shortcuts.panel.hold.to.activate.release.restores.the.previous.state")
            : d.id.startsWith("Nudge") ? i18n::text("shortcuts.panel.shared.by.layers.and.pixel.selections.hold.shift.for")
            : d.id == "RemovePointAction" ? i18n::text("shortcuts.panel.lasso.anchor.polygon.vertex.or.focused.curve.point.text")
            : d.id == "FinishOperationAction" ? i18n::text("shortcuts.panel.apply.transform.crop.or.finish.a.lasso.polygon.enter")
            : i18n::text("shortcuts.panel.focused.text.fields.and.panel.navigation.keep.their.ordinary")));
    }
    layout->addWidget(commands_, 1);
    auto* buttons = new QHBoxLayout;
    const auto button = [this, buttons](const char* id, const i18n::Message& text) {
        auto* b = i18n::make<QPushButton>(text, this); b->setObjectName(id); b->setAutoDefault(false); buttons->addWidget(b); return b;
    };
    auto* change = button("ShortcutChange", i18n::text("shortcuts.panel.change"));
    auto* clear = button("ShortcutClear", i18n::text("shortcuts.panel.clear.binding"));
    buttons->addStretch();
    auto* reset = button("ShortcutDefaults", i18n::text("shortcuts.panel.restore.defaults"));
    layout->addLayout(buttons);
    notice_ = new QLabel(this); notice_->setObjectName("ShortcutNotice"); notice_->setWordWrap(true);
    layout->addWidget(notice_);
    auto* conflict = new QHBoxLayout;
    confirm_ = i18n::make<QPushButton>(i18n::text("shortcuts.panel.assign.and.clear.conflicts"), this); confirm_->setObjectName("ShortcutConfirmConflict"); confirm_->setAutoDefault(false);
    cancel_ = i18n::make<QPushButton>(i18n::text("shortcuts.panel.cancel.binding"), this); cancel_->setAutoDefault(false);
    conflict->addWidget(confirm_); conflict->addWidget(cancel_); conflict->addStretch(); layout->addLayout(conflict);
    auto* hint = i18n::make<QLabel>(i18n::text("shortcuts.panel.escape.always.cancels.and.cannot.be.rebound.gesture.modifiers"), this);
    hint->setObjectName("MutedLabel"); hint->setWordWrap(true); layout->addWidget(hint);
    connect(search, &QLineEdit::textChanged, this, [this](const i18n::Message& text) {
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
    connect(&i18n::Localization::instance(), &i18n::Localization::languageChanged, this, [this] { refresh(); });
    refresh(); cancelPending();
    qApp->installEventFilter(this);
}
QString ShortcutsPanel::selectedId() const
{ return commands_->currentItem() ? commands_->currentItem()->data(0, Qt::UserRole).toString() : QString{}; }
void ShortcutsPanel::refresh()
{
    commands_->setHeaderLabels({i18n::text("shortcuts.panel.command"), i18n::text("shortcuts.panel.category"), i18n::text("shortcuts.panel.binding")});
    for (int i = 0; i < commands_->topLevelItemCount(); ++i) {
        auto* row = commands_->topLevelItem(i);
        if (const auto* d = shortcutDefinition(row->data(0, Qt::UserRole).toString())) { row->setText(0, d->label.resolved()); row->setText(1, d->category.resolved()); }
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
    i18n::setText(notice_, i18n::text("shortcuts.panel.press.a.key.combination.for.1.escape.cancels").arg(shortcutDefinition(pendingId_)->label));
}
void ShortcutsPanel::propose(const QKeySequence& key)
{
    const auto error = validateShortcut(pendingId_, key);
    if (!error.isEmpty()) { i18n::setText(notice_, error + i18n::text("shortcuts.panel.try.another.key.or.escape.to.cancel")); return; }
    pendingKey_ = key;
    const auto conflicts = shortcutConflicts(bindings_, pendingId_, key);
    if (conflicts.isEmpty()) { assign(false); return; }
    capturing_ = false;
    QStringList names; for (const auto& id : conflicts) names.append(shortcutDefinition(id)->label);
    i18n::setText(notice_, i18n::text("shortcuts.panel.1.is.already.assigned.to.2.in.an.overlapping")
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
