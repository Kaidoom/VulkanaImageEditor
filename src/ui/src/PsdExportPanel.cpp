#include "imageeditor/ui/Localization.hpp"
#include "imageeditor/ui/PsdReviewText.hpp"
#include "imageeditor/ui/PsdExportPanel.hpp"
#include "imageeditor/ui/Theme.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QScopedValueRollback>
#include <QStyledItemDelegate>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>

namespace imageeditor::ui {
namespace {
i18n::Message outputName(const PsdExportEntry& entry) {
  if (entry.action == PsdExportAction::Omit) return i18n::text("psd.export.panel.omit");
  if (entry.action == PsdExportAction::Pixels)
    return i18n::text(entry.container ? "psd.export.panel.consolidate" : "psd.export.panel.pixels");
  return i18n::psdContentName(entry.kind, true);
}
}
PsdExportPanel::PsdExportPanel(PsdExportOptions &options, QWidget *parent,
                               QWidget *itemsParent)
    : QWidget(parent), options_(options), items_(new QWidget(itemsParent)) {
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setAlignment(Qt::AlignTop);
  setObjectName("PsdExportOptions");
  auto *formats = new QHBoxLayout;
  layout->addLayout(formats);
  auto combo = [&](const char *name) {
    auto *c = new QComboBox(this);
    c->setObjectName(name);
    c->setItemDelegate(new QStyledItemDelegate(c));
    return c;
  };
  mode_ = combo("PsdExportMode");
  i18n::addItems(mode_, {i18n::text("psd.export.panel.layered.psd"), i18n::text("psd.export.panel.flattened.psd")});
  mode_->setCurrentIndex(int(options.mode));
  formats->addWidget(mode_, 1);
  text_ = combo("PsdExportText");
  i18n::addItems(text_, {i18n::text("psd.export.panel.preserve.editable.text.where.supported"), i18n::text("psd.export.panel.rasterize.all.text")});
  text_->setCurrentIndex(options.preserveText ? 0 : 1);
  formats->addWidget(text_, 1);
  summary_ = new QLabel(this);
  summary_->setWordWrap(true);
  layout->addWidget(summary_);
  items_->setObjectName("PsdExportConversions");
  items_->setMinimumWidth(330);
  auto *listLayout = new QVBoxLayout(items_);
  listLayout->setContentsMargins(0, 0, 0, 0);
  listLayout->setSpacing(8);
  auto *listHeader = new QHBoxLayout;
  auto *title = i18n::make<QLabel>(i18n::text("psd.export.panel.layer.conversion"), items_);
  title->setObjectName("SectionLabel");
  title->setMinimumHeight(32);
  listHeader->addWidget(title);
  listHeader->addStretch();
  attention_ = i18n::make<QCheckBox>(i18n::text("psd.export.panel.attention.needed.only"), items_);
  attention_->setObjectName("PsdExportAttention");
  attention_->setChecked(true);
  listHeader->addWidget(attention_);
  hideGroups_ = i18n::make<QCheckBox>(i18n::text("psd.export.panel.hide.groups"), items_);
  hideGroups_->setObjectName("PsdExportHideGroups");
  hideGroups_->setChecked(true);
  listHeader->addWidget(hideGroups_);
  listLayout->addLayout(listHeader);
  tree_ = new QTreeWidget(items_);
  tree_->setObjectName("PsdExportItems");
  tree_->setHeaderLabels({i18n::text("psd.export.panel.layer"), i18n::text("psd.export.panel.output")});
  connect(&i18n::Localization::instance(), &i18n::Localization::languageChanged, this, [this] {
    QScopedValueRollback guard(updating_, true);
    tree_->setHeaderLabels({i18n::text("psd.export.panel.layer"), i18n::text("psd.export.panel.output")});
    // Keep selection, scroll position and expanded groups. No replanning or
    // rendering is needed to refresh the accepted plan's display labels.
    for (QTreeWidgetItemIterator row(tree_); *row; ++row) {
      const auto id = (*row)->data(0, Qt::UserRole).toULongLong();
      const auto entry = std::ranges::find_if(plan_.entries, [id](const auto& e) { return e.id == id; });
      if (entry != plan_.entries.end()) (*row)->setText(1, outputName(*entry));
    }
    selectionChanged();
  });
  tree_->setRootIsDecorated(true);
  tree_->setMinimumHeight(120);
  tree_->setUniformRowHeights(true);
  tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
  tree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  listLayout->addWidget(tree_, 1);
  action_ = new QComboBox(items_);
  action_->setObjectName("PsdExportAction");
  action_->setItemDelegate(new QStyledItemDelegate(action_));
  listLayout->addWidget(action_);
  detail_ = new QLabel(items_);
  detail_->setObjectName("PsdExportActionDescription");
  detail_->setWordWrap(true);
  detail_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  detail_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  listLayout->addWidget(detail_);
  connect(mode_, &QComboBox::currentIndexChanged, this, [this](int n) {
    options_.mode = PsdExportMode(n);
    notify();
  });
  connect(text_, &QComboBox::currentIndexChanged, this, [this](int n) {
    options_.preserveText = n == 0;
    notify();
  });
  connect(attention_, &QCheckBox::toggled, this, [this] { filter(); });
  connect(hideGroups_, &QCheckBox::toggled, this, [this] { filter(); });
  connect(tree_, &QTreeWidget::currentItemChanged, this,
          [this] { selectionChanged(); });
  connect(action_, &QComboBox::currentIndexChanged, this, [this](int index) {
    if (updating_ || index < 0 || !tree_->currentItem())
      return;
    auto id = tree_->currentItem()->data(0, Qt::UserRole).toULongLong();
    options_.actions[id] = PsdExportAction(action_->itemData(index).toInt());
    updateDescription();
    notify();
  });
}
void PsdExportPanel::notify() {
  if (updating_)
    return;
  if (changed)
    changed();
}
void PsdExportPanel::setPsdVisible(bool on) {
  items_->setVisible(on && options_.mode == PsdExportMode::Layered);
}
QWidget *PsdExportPanel::itemsWidget() const { return items_; }
void PsdExportPanel::reset() {
  QScopedValueRollback guard(updating_, true);
  options_ = {};
  mode_->setCurrentIndex(0);
  text_->setCurrentIndex(0);
}
void PsdExportPanel::setPlan(const PsdExportPlan &p) {
  QScopedValueRollback guard(updating_, true);
  plan_ = p;
  options_.instanceId = p.instanceId;
  int editable = 0, pixels = 0, omitted = 0;
  for (auto &e : p.entries) {
    if (e.action == PsdExportAction::Editable)
      ++editable;
    else if (e.action == PsdExportAction::Pixels)
      ++pixels;
    else
      ++omitted;
  }
  i18n::setText(summary_, p.options.mode == PsdExportMode::Flattened
          ? i18n::text("psd.export.panel.one.visible.composite.pixel.layer.no.hidden.copies")
          : i18n::text("psd.export.panel.1.editable.2.pixel.consolidation.choices.3.omitted")
                .arg(editable)
                .arg(pixels)
                .arg(omitted));
  text_->setEnabled(p.options.mode == PsdExportMode::Layered);
  items_->setVisible(isVisible() && p.options.mode == PsdExportMode::Layered);
  filter();
}
void PsdExportPanel::selectionChanged() {
  QScopedValueRollback guard(updating_, true);
  action_->clear();
  action_->hide();
  detail_->clear();
  detail_->hide();
  auto *row = tree_->currentItem();
  if (!row)
    return;
  auto id = row->data(0, Qt::UserRole).toULongLong();
  auto it =
      std::ranges::find_if(plan_.entries, [&](auto &e) { return e.id == id; });
  if (it == plan_.entries.end())
    return;
  const auto &e = *it;
  // Supported containers map directly. Keep choices only when a real
  // conversion (possibly in a descendant) can require this enclosing span.
  const auto inside = [&](const PsdExportEntry &child) {
    auto parent = child.parent;
    while (parent) {
      if (parent == e.id)
        return true;
      auto entry = std::ranges::find_if(
          plan_.entries, [&](auto &p) { return p.id == parent; });
      if (entry == plan_.entries.end())
        break;
      parent = entry->parent;
    }
    return false;
  };
  const bool conversion =
      !e.container || !e.editable || e.attention ||
      std::ranges::any_of(plan_.entries, [&](auto &child) {
        return inside(child) &&
               ((!child.pixels && (!child.editable ||
                                   child.action == PsdExportAction::Pixels)) ||
                (child.container && !child.editable));
      });
  if (!conversion)
    return;
  action_->show();
  i18n::addItem(action_, i18n::text("psd.export.panel.recommended.conversion"), int(PsdExportAction::Automatic));
  if (e.editable)
    i18n::addItem(action_, i18n::text("psd.export.panel.keep.editable"), int(PsdExportAction::Editable));
  if (e.pixels)
    i18n::addItem(action_, e.container ? i18n::text("psd.export.panel.consolidate.this.subtree.into.pixels")
                                 : i18n::text("psd.export.panel.bake.this.layer.into.pixels"), int(PsdExportAction::Pixels));
  i18n::addItem(action_, i18n::text("psd.export.panel.omit.from.this.export"), int(PsdExportAction::Omit));
  action_->setCurrentIndex(action_->findData(
      int(options_.actions.value(id, PsdExportAction::Automatic))));
  updateDescription();
}
void PsdExportPanel::updateDescription() {
  auto *row = tree_->currentItem();
  if (!row || action_->currentIndex() < 0)
    return;
  const auto id = row->data(0, Qt::UserRole).toULongLong();
  auto entry =
      std::ranges::find_if(plan_.entries, [&](auto &e) { return e.id == id; });
  if (entry == plan_.entries.end())
    return;
  const auto &e = *entry;
  i18n::Message text;
  switch (PsdExportAction(action_->currentData().toInt())) {
  case PsdExportAction::Automatic:
    text = e.container ? i18n::text("psd.export.panel.use.the.recommended.conversion.for.this.group")
                       : i18n::text("psd.export.panel.keep.this.layer.editable.where.possible.otherwise.export.it");
    break;
  case PsdExportAction::Editable:
    text = e.container ? i18n::text("psd.export.panel.keep.this.group.and.its.layers.editable")
                       : i18n::text("psd.export.panel.keep.this.layer.editable");
    text += i18n::text("psd.export.panel.its.appearance.may.differ.outside.vulkana");
    break;
  case PsdExportAction::Pixels:
    text = e.container
               ? i18n::text("psd.export.panel.combine.this.group.into.one.pixel.layer.in.the")
               : i18n::text("psd.export.panel.export.this.layer.s.appearance.as.pixels");
    break;
  case PsdExportAction::Omit:
    text = e.container ? i18n::text("psd.export.panel.leave.this.group.and.its.layers.out.of.the")
                       : i18n::text("psd.export.panel.leave.this.layer.out.of.the.psd");
    break;
  }
  i18n::setText(detail_, text);
  detail_->show();
}
void PsdExportPanel::filter() {
  QScopedValueRollback guard(updating_, true);
  const auto selected =
      tree_->currentItem()
          ? tree_->currentItem()->data(0, Qt::UserRole).toULongLong()
          : 0;
  tree_->clear();
  tree_->setRootIsDecorated(!hideGroups_->isChecked());
  QMap<qulonglong, QTreeWidgetItem *> items;
  QTreeWidgetItem *selectedRow = nullptr;
  for (const auto &e : plan_.entries) {
    auto *parent = items.value(e.parent, nullptr);
    if (e.container && hideGroups_->isChecked()) {
      // This is a view filter, not omission: promote children into the visible
      // list while their authoritative group membership stays untouched.
      items[e.id] = parent;
      continue;
    }
    auto *row =
        parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(tree_);
    items[e.id] = row;
    row->setData(0, Qt::UserRole, QVariant::fromValue(qulonglong(e.id)));
    row->setData(0, Qt::UserRole + 1, e.attention || !e.editable);
    row->setText(0, e.name);
    row->setText(1, outputName(e));
    if (e.container)
      for (int column = 0; column < tree_->columnCount(); ++column)
        row->setForeground(column, themeColor(ThemeColor::SecondaryText));
    row->setExpanded(true);
    if (selected == e.id)
      selectedRow = row;
  }
  const auto visit = [&](auto &&self, QTreeWidgetItem *row) -> bool {
    bool show =
        !attention_->isChecked() || row->data(0, Qt::UserRole + 1).toBool();
    for (int i = 0; i < row->childCount(); ++i)
      show = self(self, row->child(i)) || show;
    row->setHidden(!show);
    return show;
  };
  for (int i = 0; i < tree_->topLevelItemCount(); ++i)
    visit(visit, tree_->topLevelItem(i));
  if (selectedRow && !selectedRow->isHidden())
    tree_->setCurrentItem(selectedRow);
  else
    for (QTreeWidgetItemIterator row(tree_); *row; ++row)
      if (!(*row)->isHidden()) {
        tree_->setCurrentItem(*row);
        break;
      }
  selectionChanged();
}
} // namespace imageeditor::ui
