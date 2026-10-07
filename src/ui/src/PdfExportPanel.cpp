#include "imageeditor/ui/PdfExportPanel.hpp"
#include "imageeditor/ui/PdfImport.hpp"
#include "imageeditor/ui/PopupOwnership.hpp"
#include "imageeditor/ui/ToolOptionsNumber.hpp"
#include <QCheckBox>
#include "imageeditor/ui/ColorDialog.hpp"
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QVBoxLayout>
#include <algorithm>

namespace imageeditor::ui {
struct PdfExportPanel::State {
  PdfExportPanel &owner;
  PdfExportOptions &options;
  std::vector<PdfExportEntry> entries;
  std::vector<core::LayerId> current;
  double documentPpi{96};
  bool updating{false}, hasSource{false};
  QString error;
  std::uint64_t instanceId{0};
  QWidget *items, *navigation;
  QListWidget *list;
  QComboBox *mode, *selection, *pageSize, *text, *page;
  QCheckBox *hidden, *reverse, *matte;
  QPushButton *color;
  ToolOptionsNumber *ppi;
  QLineEdit *range;
  QLabel *summary, *reasons;
  State(PdfExportPanel &o, PdfExportOptions &v) : owner(o), options(v) {}
  static bool has(const std::vector<core::LayerId> &ids, core::LayerId id) {
    return std::ranges::find(ids, id) != ids.end();
  }
  std::vector<core::LayerId> chosen() const {
    if (options.selection == PdfExportSelection::Current)
      return current;
    if (options.selection == PdfExportSelection::Custom)
      return options.chosen;
    std::vector<core::LayerId> ids;
    for (const auto &entry : entries)
      ids.push_back(entry.id);
    return ids;
  }
  void syncSelection() {
    const QSignalBlocker block(list), rangeBlock(range);
    const auto ids = chosen();
    std::vector<int> numbers;
    for (std::size_t i = 0; i < entries.size(); ++i) {
      const bool checked = has(ids, entries[i].id);
      list->item(int(i))->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
      if (checked)
        numbers.push_back(int(i));
    }
    range->setText(formatPdfPageRange(numbers));
  }
  void notify(bool emitChange = true) {
    if (updating)
      return;
    pageSize->setEnabled(options.mode == PdfExportMode::Pages);
    reverse->setEnabled(options.mode == PdfExportMode::Pages);
    color->setEnabled(options.matte);
    color->setText(options.matteColor.name().toUpper());
    QPixmap chip(20, 20);
    chip.fill(options.matteColor);
    color->setIcon(QIcon(chip));
    if (emitChange && owner.changed)
      owner.changed();
  }
};
PdfExportPanel::PdfExportPanel(PdfExportOptions &options, QWidget *parent,
                               QWidget *previewParent)
    : QWidget(parent), state_(std::make_unique<State>(*this, options)) {
  auto &s = *state_;
  setObjectName(QStringLiteral("PdfExportPanel"));
  auto *form = new QVBoxLayout(this);
  form->setContentsMargins(0, 0, 0, 0);
  form->setSpacing(9);
  const auto combo = [&](const char *name, QStringList values, QWidget *host) {
    auto *field = new QComboBox(host);
    field->setObjectName(QString::fromLatin1(name));
    field->setItemDelegate(new QStyledItemDelegate(field));
    field->addItems(values);
    return field;
  };
  s.mode = combo("PdfExportMode",
                 {tr("Single Composite"), tr("Layers as Pages")}, this);
  form->addWidget(s.mode);
  s.pageSize =
      combo("PdfExportPageSize",
            {tr("Canvas page size"), tr("Fit Each Layer / Group")}, this);
  form->addWidget(s.pageSize);
  s.text = combo("PdfExportText",
                 {tr("Preserve text where possible"), tr("Rasterize all text")},
                 this);
  form->addWidget(s.text);
  auto *density = new QHBoxLayout;
  s.ppi = new ToolOptionsNumber(this);
  s.ppi->setObjectName(QStringLiteral("PdfExportPpi"));
  s.ppi->setRange(1, 2400);
  s.ppi->setDecimals(2);
  s.ppi->setSuffix(tr(" PPI"));
  density->addWidget(s.ppi, 1);
  auto *presets = combo("PdfExportPpiPresets",
                        {tr("PPI…"), "72", "150", "300", "600"}, this);
  density->addWidget(presets);
  form->addLayout(density);
  connect(presets, &QComboBox::activated, this, [&s, presets](int index) {
    if (index) {
      s.ppi->setValue(presets->itemText(index).toDouble());
      presets->setCurrentIndex(0);
    }
  });
  s.hidden = new QCheckBox(tr("Ignore Hidden Layers"), this);
  s.hidden->setObjectName(QStringLiteral("PdfExportIgnoreHidden"));
  form->addWidget(s.hidden);
  s.reverse = new QCheckBox(tr("Reverse Order"), this);
  s.reverse->setObjectName(QStringLiteral("PdfExportReverse"));
  form->addWidget(s.reverse);
  s.matte = new QCheckBox(tr("Replace Transparency with Color"), this);
  s.matte->setObjectName(QStringLiteral("PdfExportMatte"));
  form->addWidget(s.matte);
  s.color = new QPushButton(this);
  s.color->setAutoDefault(false);
  s.color->setObjectName(QStringLiteral("PdfExportMatteColor"));
  form->addWidget(s.color);
  auto *help = new QLabel(
      tr("Pages are independent composites over transparency. Raster images "
         "and supported text only; no editable PDF layers. PPI changes raster "
         "detail, not physical page size."),
      this);
  help->setWordWrap(true);
  help->setObjectName(QStringLiteral("MutedLabel"));
  form->addWidget(help);
  s.summary = new QLabel(this);
  s.summary->setObjectName(QStringLiteral("PdfExportSummary"));
  s.summary->setWordWrap(true);
  form->addWidget(s.summary);
  auto *details = new QPushButton(tr("Text details"), this);
  details->setObjectName(QStringLiteral("PdfExportTextDetails"));
  details->setAutoDefault(false);
  details->setCheckable(true);
  form->addWidget(details);
  s.reasons = new QLabel(this);
  s.reasons->setWordWrap(true);
  s.reasons->setTextFormat(Qt::PlainText);
  s.reasons->hide();
  form->addWidget(s.reasons);
  connect(details, &QPushButton::toggled, s.reasons, &QWidget::setVisible);
  form->addStretch();
  s.items = new QWidget(previewParent);
  s.items->setObjectName(QStringLiteral("PdfExportItems"));
  auto *itemLayout = new QVBoxLayout(s.items);
  itemLayout->setContentsMargins(0, 0, 0, 0);
  itemLayout->setSpacing(6);
  auto *selectRow = new QHBoxLayout;
  s.selection = combo(
      "PdfExportSelection",
      {tr("All items"), tr("Current layer selection"), tr("Custom selection")},
      s.items);
  selectRow->addWidget(s.selection);
  s.range = new QLineEdit(s.items);
  s.range->setObjectName(QStringLiteral("PdfExportRange"));
  s.range->setPlaceholderText(tr("1-3, 6, 9-12"));
  s.range->setToolTip(
      tr("Stable source-entry numbers, before hidden filtering or reversal"));
  selectRow->addWidget(s.range, 1);
  itemLayout->addLayout(selectRow);
  s.list = new QListWidget(s.items);
  s.list->setObjectName(QStringLiteral("PdfExportItemList"));
  s.list->setProperty("standardItemCheckIndicators", true);
  s.list->setIconSize({38, 38});
  s.list->setMinimumHeight(124);
  s.list->setMaximumHeight(176);
  s.list->setUniformItemSizes(true);
  s.list->setSelectionMode(QAbstractItemView::SingleSelection);
  itemLayout->addWidget(s.list);
  connect(s.list->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] {
    if (thumbnailsChanged)
      thumbnailsChanged();
  });
  s.navigation = new QWidget(previewParent);
  auto *nav = new QHBoxLayout(s.navigation);
  nav->setContentsMargins(0, 0, 0, 0);
  s.page = combo("PdfExportPreviewPage", {}, s.navigation);
  nav->addWidget(s.page, 1);
  connect(s.page, &QComboBox::currentIndexChanged, this, [this] {
    if (!state_->updating && pageChanged)
      pageChanged();
  });
  connect(s.mode, &QComboBox::currentIndexChanged, this, [&s](int v) {
    s.options.mode = PdfExportMode(v);
    s.notify();
  });
  connect(s.pageSize, &QComboBox::currentIndexChanged, this, [&s](int v) {
    s.options.pageSize = PdfExportPageSize(v);
    s.notify();
  });
  connect(s.text, &QComboBox::currentIndexChanged, this, [&s](int v) {
    s.options.text = PdfExportText(v);
    s.notify();
  });
  connect(s.ppi, &QDoubleSpinBox::valueChanged, this, [&s](double v) {
    s.options.ppi = v;
    s.notify();
  });
  connect(s.hidden, &QCheckBox::toggled, this, [&s](bool v) {
    s.options.ignoreHidden = v;
    s.notify();
  });
  connect(s.reverse, &QCheckBox::toggled, this, [&s](bool v) {
    s.options.reverse = v;
    s.notify();
  });
  connect(s.matte, &QCheckBox::toggled, this, [&s](bool v) {
    s.options.matte = v;
    s.notify();
  });
  connect(s.color, &QPushButton::clicked, this, [this, &s] {
    const auto color = ColorDialog::getColor(
        s.options.matteColor, popupTopLevelOwner(this),
        tr("PDF background color"), ColorDialog::DontUseNativeDialog);
    if (color.isValid()) {
      s.options.matteColor = color;
      s.options.matteColor.setAlpha(255);
      s.notify();
    }
  });
  connect(s.selection, &QComboBox::currentIndexChanged, this, [&s](int v) {
    if (s.updating)
      return;
    s.error.clear();
    if (v == int(PdfExportSelection::Custom))
      s.options.chosen = s.chosen();
    s.options.selection = PdfExportSelection(v);
    s.syncSelection();
    s.notify();
  });
  connect(s.range, &QLineEdit::textEdited, this, [&s](const QString &value) {
    s.options.selection = PdfExportSelection::Custom;
    {
      const QSignalBlocker b(s.selection);
      s.selection->setCurrentIndex(2);
    }
    const auto numbers =
        parsePdfPageRange(value, int(s.entries.size()), s.error);
    s.options.chosen.clear();
    if (s.error.isEmpty())
      for (auto number : numbers)
        s.options.chosen.push_back(s.entries[std::size_t(number)].id);
    {
      const QSignalBlocker b(s.list);
      for (int i = 0; i < s.list->count(); ++i)
        s.list->item(i)->setCheckState(
            State::has(s.options.chosen, s.entries[std::size_t(i)].id)
                ? Qt::Checked
                : Qt::Unchecked);
    }
    s.notify();
  });
  connect(s.list, &QListWidget::itemChanged, this, [&s](QListWidgetItem *) {
    if (s.updating)
      return;
    s.error.clear();
    s.options.selection = PdfExportSelection::Custom;
    s.options.chosen.clear();
    {
      const QSignalBlocker b(s.selection);
      s.selection->setCurrentIndex(2);
    }
    for (int i = 0; i < s.list->count(); ++i)
      if (s.list->item(i)->checkState() == Qt::Checked)
        s.options.chosen.push_back(s.entries[std::size_t(i)].id);
    s.syncSelection();
    s.notify();
  });
  reset();
}
PdfExportPanel::~PdfExportPanel() = default;
QWidget *PdfExportPanel::itemsWidget() const { return state_->items; }
QWidget *PdfExportPanel::navigationWidget() const { return state_->navigation; }
void PdfExportPanel::setSource(const PdfExportSnapshot &source) {
  auto &s = *state_;
  s.entries = source.entries;
  s.current = source.current;
  s.documentPpi = source.document->canvas().dotsPerInch;
  s.hasSource = true;
  s.instanceId = source.instanceId;
  if (s.options.instanceId != 0 && s.options.instanceId != source.instanceId) {
    s.options.selection = PdfExportSelection::All;
    s.options.chosen.clear();
  }
  s.options.instanceId = source.instanceId;
  s.updating = true;
  s.list->clear();
  for (const auto &entry : s.entries) {
    auto *item = new QListWidgetItem(
        QStringLiteral("%1 · %2%3\n%4")
            .arg(entry.number)
            .arg(entry.name, entry.visible ? QString() : tr(" (hidden)"),
                 entry.folder.isEmpty() ? tr("Document") : entry.folder),
        s.list);
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(entry.id));
    item->setSizeHint({0, 48});
    item->setToolTip(item->text());
  }
  s.updating = false;
  reset();
}
void PdfExportPanel::reset() {
  auto &s = *state_;
  s.updating = true;
  s.error.clear();
  if (s.hasSource)
    s.options.instanceId = s.instanceId;
  if (s.options.ppi == 0 && s.hasSource)
    s.options.ppi = s.documentPpi;
  s.mode->setCurrentIndex(int(s.options.mode));
  s.pageSize->setCurrentIndex(int(s.options.pageSize));
  s.text->setCurrentIndex(int(s.options.text));
  s.selection->setCurrentIndex(int(s.options.selection));
  {
    const QSignalBlocker b(s.ppi);
    s.ppi->setValue(s.options.ppi == 0 ? s.documentPpi : s.options.ppi);
  }
  s.hidden->setChecked(s.options.ignoreHidden);
  s.reverse->setChecked(s.options.reverse);
  s.matte->setChecked(s.options.matte);
  s.syncSelection();
  s.updating = false;
  s.notify(false);
}
void PdfExportPanel::setPdfVisible(bool value) {
  setVisible(value);
  state_->items->setVisible(value);
  state_->navigation->setVisible(value);
}
void PdfExportPanel::setPlan(const PdfExportPlan &plan) {
  auto &s = *state_;
  s.updating = true;
  const auto selected = s.page->currentIndex();
  s.page->clear();
  QStringList reasons;
  for (std::size_t i = 0; i < plan.pages.size(); ++i) {
    const auto &p = plan.pages[i];
    s.page->addItem(tr("Page %1 · %2 · %3 × %4 px")
                        .arg(i + 1)
                        .arg(p.name)
                        .arg(p.pixels.width())
                        .arg(p.pixels.height()));
    reasons.append(p.textReasons);
  }
  s.page->setCurrentIndex(
      std::clamp(selected, 0, std::max(0, s.page->count() - 1)));
  s.updating = false;
  s.summary->setText(
      tr("%1 page(s) · %2 MiB raster data\nEstimated peak working memory: %3 "
         "MiB\nText: %4 preserved / %5 rasterized")
          .arg(plan.pages.size())
          .arg(double(plan.rasterBytes) / (1024 * 1024), 0, 'f', 1)
          .arg(double(plan.peakWorkingBytes) / (1024 * 1024), 0, 'f', 0)
          .arg(plan.preserved)
          .arg(plan.rasterized));
  reasons.removeDuplicates();
  s.reasons->setText(
      reasons.isEmpty()
          ? tr("Eligible text uses its resolved fonts and existing layout. "
               "Imported PDF page images remain images.")
          : reasons.join('\n'));
}
int PdfExportPanel::previewPage() const {
  return std::max(0, state_->page->currentIndex());
}
QString PdfExportPanel::inputError() const { return state_->error; }
std::vector<core::LayerId> PdfExportPanel::visibleThumbnails() const {
  std::vector<core::LayerId> ids;
  const auto &s = *state_;
  if (!s.items->isVisible())
    return ids;
  for (int i = 0; i < s.list->count(); ++i)
    if (s.list->visualItemRect(s.list->item(i))
            .intersects(s.list->viewport()->rect()))
      ids.push_back(s.entries[std::size_t(i)].id);
  return ids;
}
void PdfExportPanel::setThumbnail(core::LayerId id, const QImage &image) {
  auto &s = *state_;
  const QSignalBlocker block(s.list);
  const auto it = std::ranges::find_if(
      s.entries, [&](const auto &e) { return e.id == id; });
  if (it == s.entries.end())
    return;
  auto *item = s.list->item(int(it - s.entries.begin()));
  if (image.isNull()) {
    item->setIcon({});
    return;
  }
  QPixmap pixmap(image.size());
  pixmap.fill(palette().color(QPalette::Mid));
  QPainter painter(&pixmap);
  painter.drawImage(0, 0, image);
  painter.end();
  item->setIcon(QIcon(pixmap));
}
} // namespace imageeditor::ui
