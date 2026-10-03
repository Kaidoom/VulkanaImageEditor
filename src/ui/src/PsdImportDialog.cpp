#include "imageeditor/ui/PsdImportDialog.hpp"
#include "imageeditor/ui/Theme.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QFileInfo>
#include <QFontComboBox>
#include <QFrame>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPainter>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPointer>
#include <QPushButton>
#include <QResizeEvent>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <chrono>

namespace imageeditor::ui {
namespace {
template <class T> bool ready(std::future<T> &f) {
  return f.valid() &&
         f.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}
QPixmap displayPreview(const QImage &image) {
  QPixmap p(image.size());
  p.fill(QColor(155, 155, 155));
  QPainter painter(&p);
  for (int y = 0; y < p.height(); y += 12)
    for (int x = 0; x < p.width(); x += 12)
      if ((x / 12 + y / 12) % 2)
        painter.fillRect(x, y, 12, 12, QColor(195, 195, 195));
  painter.drawImage(0, 0, image);
  return p;
}
class PreviewLabel final : public QLabel {
  QPixmap source;
  void fit() {
    if (!source.isNull())
      QLabel::setPixmap(
          source.scaled(size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
  }
  void resizeEvent(QResizeEvent *event) override {
    QLabel::resizeEvent(event);
    fit();
  }

public:
  explicit PreviewLabel(QWidget *owner) : QLabel(owner) {}
  void setImage(const QImage &image) {
    source = displayPreview(image);
    fit();
  }
  void message(const QString &text) {
    source = {};
    setText(text);
  }
};
} // namespace
struct PsdImportDialog::State {
  PsdImportDialog &owner;
  PsdLimits limits;
  PsdInspection inspection;
  PsdOptions options;
  PsdConversion result;
  std::shared_ptr<PsdJob> load = std::make_shared<PsdJob>(), render;
  std::future<PsdInspection> loadJob;
  std::future<PsdConversion> renderJob;
  QTimer timer;
  QTreeWidget *tree;
  QLabel *summary, *status, *description;
  PreviewLabel *preview;
  QCheckBox *attention, *hideGroups, *allRaster, *previewEnabled;
  QComboBox *mode, *destination, *fontRequest, *action;
  QFontComboBox *replacement;
  QPlainTextEdit *details;
  QProgressBar *progress;
  QPushButton *import;
  std::vector<QTreeWidgetItem *> rows;
  std::vector<int> choices; // -1 is an unresolved choice, separate from Skip.
  QMap<int, size_t> sourceIndexes;
  QPointer<QWidget> confirmation;
  QList<QPointer<QWidget>> confirmationDisabled;
  bool refreshing{}, closed{}, previewWork{}, confirmWhenReady{};
  unsigned generation{}, renderGeneration{};
  State(PsdImportDialog &dialog, QString path, bool available, bool current,
        PsdLimits budget)
      : owner(dialog), limits(budget) {
    owner.setObjectName("PsdImportDialog");
    owner.setWindowTitle("Import PSD");
    owner.setWindowFlags(Qt::SubWindow);
    owner.resize(880, 660);
    owner.setProperty("workspacePreferredSize", QSize(880, 660));
    auto *layout = new QVBoxLayout(&owner);
    layout->setContentsMargins(18, 18, 18, 18);
    layout->setSpacing(8);
    auto *title = new QLabel(
        QStringLiteral("Import PSD · %1").arg(QFileInfo(path).fileName()),
        &owner);
    title->setTextFormat(Qt::PlainText);
    title->setWordWrap(true);
    layout->addWidget(title);
    summary = new QLabel("Inspecting document…", &owner);
    summary->setTextFormat(Qt::PlainText);
    summary->setWordWrap(true);
    layout->addWidget(summary);
    auto *policies = new QHBoxLayout;
    attention = new QCheckBox("Attention needed only", &owner);
    attention->setObjectName("PsdAttention");
    attention->setChecked(true);
    policies->addWidget(attention);
    hideGroups = new QCheckBox("Hide groups", &owner);
    hideGroups->setObjectName("PsdHideGroups");
    hideGroups->setChecked(true);
    policies->addWidget(hideGroups);
    allRaster = new QCheckBox("Import all text as raster", &owner);
    allRaster->setObjectName("PsdRasterText");
    policies->addWidget(allRaster);
    policies->addStretch();
    layout->addLayout(policies);
    auto *body = new QHBoxLayout;
    auto *conversions = new QWidget(&owner);
    conversions->setObjectName("PsdConversions");
    auto *list = new QVBoxLayout(conversions);
    list->setContentsMargins(0, 0, 0, 0);
    tree = new QTreeWidget(conversions);
    tree->setObjectName("PsdLayers");
    tree->setHeaderLabels({"Layer / detected type", "Conversion"});
    tree->setRootIsDecorated(true);
    tree->setMinimumWidth(475);
    tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    tree->header()->setStretchLastSection(true);
    tree->setMinimumHeight(120);
    list->addWidget(tree, 1);
    action = new QComboBox(conversions);
    action->setObjectName("PsdImportAction");
    action->setItemDelegate(new QStyledItemDelegate(action));
    list->addWidget(action);
    description = new QLabel(conversions);
    description->setObjectName("PsdImportActionDescription");
    description->setWordWrap(true);
    description->setTextFormat(Qt::PlainText);
    description->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    list->addWidget(description);
    body->addWidget(conversions, 3);
    auto *previewColumn = new QWidget(&owner);
    previewColumn->setMaximumWidth(310);
    auto *previewLayout = new QVBoxLayout(previewColumn);
    previewLayout->setContentsMargins(0, 0, 0, 0);
    previewEnabled = new QCheckBox("Enable preview", previewColumn);
    previewEnabled->setObjectName("PsdImportPreview");
    previewEnabled->setChecked(true);
    previewLayout->addWidget(previewEnabled);
    preview = new PreviewLabel(previewColumn);
    preview->message("Choose settings, then validate to preview the import.");
    preview->setObjectName("PsdPreview");
    preview->setAlignment(Qt::AlignCenter);
    preview->setMinimumSize(220, 120);
    preview->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    preview->setMaximumWidth(310);
    preview->setWordWrap(true);
    previewLayout->addWidget(preview, 1);
    body->addWidget(previewColumn, 2);
    layout->addLayout(body, 1);
    auto *fontRow = new QHBoxLayout;
    fontRequest = new QComboBox(&owner);
    fontRequest->setObjectName("PsdFontRequest");
    replacement = new QFontComboBox(&owner);
    replacement->setObjectName("PsdReplacementFont");
    fontRow->addWidget(fontRequest, 1);
    fontRow->addWidget(replacement, 1);
    layout->addLayout(fontRow);
    auto *output = new QHBoxLayout;
    mode = new QComboBox(&owner);
    mode->setObjectName("PsdMode");
    mode->addItem("Editable conversion", false);
    output->addWidget(mode, 2);
    destination = new QComboBox(&owner);
    destination->setObjectName("PsdDestination");
    destination->addItem("New document tab", false);
    if (available)
      destination->addItem("Layers in the current document", true);
    destination->setCurrentIndex(available && current ? 1 : 0);
    output->addWidget(destination, 2);
    layout->addLayout(output);
    auto *detailButton = new QPushButton("Technical report", &owner);
    detailButton->setObjectName("PsdReport");
    detailButton->setCheckable(true);
    detailButton->setAutoDefault(false);
    layout->addWidget(detailButton, 0, Qt::AlignLeft);
    details = new QPlainTextEdit(&owner);
    details->setReadOnly(true);
    details->setMaximumHeight(130);
    details->hide();
    layout->addWidget(details);
    QObject::connect(detailButton, &QPushButton::toggled, details,
                     &QWidget::setVisible);
    status = new QLabel(
        "Reading metadata and checking saved raster availability…", &owner);
    status->setTextFormat(Qt::PlainText);
    status->setWordWrap(true);
    layout->addWidget(status);
    progress = new QProgressBar(&owner);
    progress->setRange(0, 0);
    layout->addWidget(progress);
    auto *footer = new QHBoxLayout;
    footer->addStretch();
    auto *cancel = new QPushButton("Cancel", &owner);
    cancel->setAutoDefault(false);
    footer->addWidget(cancel);
    import = new QPushButton("Validate Import", &owner);
    import->setObjectName("PsdImport");
    import->setEnabled(false);
    import->setAutoDefault(false);
    footer->addWidget(import);
    layout->addLayout(footer);
    QObject::connect(cancel, &QPushButton::clicked, &owner,
                     &PsdImportDialog::reject);
    QObject::connect(import, &QPushButton::clicked, &owner,
                     [this] { validate(); });
    QObject::connect(attention, &QCheckBox::toggled, &owner,
                     [this] { filter(); });
    QObject::connect(hideGroups, &QCheckBox::toggled, &owner,
                     [this] { filter(); });
    QObject::connect(previewEnabled, &QCheckBox::toggled, &owner, [this](bool on) {
      if (!on) {
        if (previewWork && render) render->cancelled = true;
        preview->message("Preview disabled.");
        if (result.document && !renderJob.valid() && !result.error.isEmpty()) {
          result.error.clear();
          prepared();
        }
      } else if (!result.preview.isNull()) {
        preview->setImage(result.preview);
      } else {
        preview->message(renderJob.valid() ? "Preparing preview…"
            : "Choose settings, then validate to preview the import.");
        if (!renderJob.valid())
          prepare(false); // An explicit request; ordinary option edits stay lazy.
      }
    });
    QObject::connect(mode, &QComboBox::currentIndexChanged, &owner, [this] {
      options.composite = mode->currentData().toBool();
      tree->setEnabled(!options.composite);
      allRaster->setEnabled(!options.composite);
      changed();
    });
    QObject::connect(allRaster, &QCheckBox::toggled, &owner, [this](bool on) {
      for (size_t i = 0; i < inspection.layers.size(); ++i)
        if (inspection.layers[i].type == "Text") {
          auto route = on ? PsdRoute::Raster : inspection.layers[i].suggested;
          if (this->available(i, route)) {
            choices[i] = int(route);
            options.layers[i].route = route;
          }
        }
      changed();
    });
    QObject::connect(tree, &QTreeWidget::currentItemChanged, &owner,
                     [this] { selectionChanged(); });
    QObject::connect(action, &QComboBox::currentIndexChanged, &owner, [this](int n) {
      const auto i = selected();
      if (refreshing || i < 0 || n < 0)
        return;
      choices[size_t(i)] = action->currentData().toInt();
      options.layers[size_t(i)].route = choices[size_t(i)] < 0
          ? PsdRoute::Skip : PsdRoute(choices[size_t(i)]);
      changed();
    });
    QObject::connect(fontRequest, &QComboBox::currentIndexChanged, &owner,
                     [this] {
                       auto i = selected();
                       if (i < 0 || fontRequest->currentText().isEmpty())
                         return;
                       QSignalBlocker block(replacement);
                       replacement->setCurrentFont(
                           QFont(options.layers[size_t(i)].replacements.value(
                               fontRequest->currentText())));
                     });
    QObject::connect(
        replacement, &QFontComboBox::currentFontChanged, &owner,
        [this](const QFont &font) {
          auto i = selected();
          if (refreshing || i < 0 || fontRequest->currentText().isEmpty())
            return;
          options.layers[size_t(i)].replacements[fontRequest->currentText()] =
              font.family();
          changed();
        });
    loadJob = inspectPsdAsync(std::move(path), load, limits);
    QObject::connect(&timer, &QTimer::timeout, &owner, [this] { poll(); });
    timer.start(60);
    selectionChanged();
  }
  int selected() const {
    return tree->currentItem()
               ? tree->currentItem()->data(0, Qt::UserRole).toInt()
               : -1;
  }
  void fonts() {
    QScopedValueRollback guard(refreshing, true);
    QSignalBlocker a(fontRequest), b(replacement);
    const auto requestedFace = fontRequest->currentText();
    fontRequest->clear();
    auto i = selected();
    bool visible = i >= 0 && !options.composite &&
        options.layers[size_t(i)].route == PsdRoute::Editable &&
        !inspection.layers[size_t(i)].fonts.empty();
    if (visible) {
      fontRequest->addItems(inspection.layers[size_t(i)].fonts);
      if (const auto index = fontRequest->findText(requestedFace); index >= 0)
        fontRequest->setCurrentIndex(index);
      replacement->setCurrentFont(
          QFont(options.layers[size_t(i)].replacements.value(
              fontRequest->currentText())));
    }
    fontRequest->setVisible(visible);
    replacement->setVisible(visible);
  }
  bool available(size_t i, PsdRoute route) const {
    const auto &info = inspection.layers[i];
    switch (route) {
    case PsdRoute::Editable: return info.editable;
    case PsdRoute::Raster: return info.raster && info.type != "Raster";
    case PsdRoute::BasePixels: return info.basePixels && (!info.raster || info.clippingBase >= 0);
    case PsdRoute::Skip: return true;
    }
    return false;
  }
  bool excluded(size_t i) const {
    auto parent = inspection.layers[i].parent;
    while (parent >= 0 && sourceIndexes.contains(parent)) {
      const auto index = sourceIndexes.value(parent);
      if (options.layers[index].route == PsdRoute::Skip)
        return true;
      parent = inspection.layers[index].parent;
    }
    return false;
  }
  void selectionChanged() {
    QScopedValueRollback guard(refreshing, true);
    action->clear();
    action->hide();
    description->clear();
    description->hide();
    fonts();
    const auto selectedIndex = selected();
    if (selectedIndex < 0 || options.composite)
      return;
    const auto i = size_t(selectedIndex);
    const auto &info = inspection.layers[i];
    if (info.container && info.editable)
      return;
    if (info.suggested == PsdRoute::Skip && info.visible)
      action->addItem("Choose action…", -1);
    if (available(i, PsdRoute::Editable))
      action->addItem("Import editable", int(PsdRoute::Editable));
    if (available(i, PsdRoute::Raster))
      action->addItem("Import as raster", int(PsdRoute::Raster));
    if (available(i, PsdRoute::BasePixels))
      action->addItem("Base pixels only", int(PsdRoute::BasePixels));
    action->addItem("Don’t import", int(PsdRoute::Skip));
    action->setCurrentIndex(action->findData(choices[i]));
    action->setEnabled(!excluded(i));
    action->show();
    QString text;
    if (excluded(i))
      text = "This layer is excluded with its group.";
    else if (choices[i] < 0)
      text = "Choose how to bring this content into Vulkana.";
    else switch (PsdRoute(choices[i])) {
    case PsdRoute::Editable:
      text = "Keep this layer editable in Vulkana.";
      if (!info.issues.empty())
        text += " Its appearance may differ from the original.";
      break;
    case PsdRoute::Raster:
      text = "Import the saved layer image as pixels instead of editable text or shapes.";
      break;
    case PsdRoute::BasePixels:
      text = "Import only the original pixels, without unsupported effects or clipping.";
      break;
    case PsdRoute::Skip:
      text = info.container ? "Leave this group and its layers out of the import."
                            : "Leave this layer out of the import.";
      break;
    }
    if (!excluded(i) && missingClippingBase(i))
      text += " Include the clipping base, or choose Base pixels only or Don’t import.";
    description->setText(text);
    description->show();
  }
  void filter() {
    QScopedValueRollback guard(refreshing, true);
    const auto previous = selected();
    tree->clear();
    rows.assign(inspection.layers.size(), nullptr);
    tree->setRootIsDecorated(!hideGroups->isChecked());
    QMap<int, QTreeWidgetItem *> parents;
    for (size_t j = inspection.layers.size(); j > 0; --j) {
      const auto i = j - 1;
      const auto &info = inspection.layers[i];
      auto *parent = parents.value(info.parent, nullptr);
      if (info.container && hideGroups->isChecked()) {
        parents[info.sourceIndex] = parent;
        continue;
      }
      auto *row = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(tree);
      rows[i] = row;
      parents[info.sourceIndex] = row;
      row->setData(0, Qt::UserRole, int(i));
      row->setData(0, Qt::UserRole + 1, choices[i]);
      row->setData(0, Qt::UserRole + 2,
          !info.issues.empty() || choices[i] < 0 || missingClippingBase(i));
      row->setText(0, info.name + " · " + info.type);
      row->setText(1, excluded(i) ? "Ancestor excluded" : missingClippingBase(i)
          ? "Clipping base omitted" : statusFor(i));
      if (info.container)
        for (int column = 0; column < tree->columnCount(); ++column)
          row->setForeground(column, themeColor(ThemeColor::SecondaryText));
      row->setExpanded(true);
    }
    const auto visit = [&](auto &&self, QTreeWidgetItem *row) -> bool {
      bool show = !attention->isChecked() || row->data(0, Qt::UserRole + 2).toBool();
      for (int i = 0; i < row->childCount(); ++i)
        show = self(self, row->child(i)) || show;
      row->setHidden(!show);
      return show;
    };
    for (int i = 0; i < tree->topLevelItemCount(); ++i)
      visit(visit, tree->topLevelItem(i));
    if (previous >= 0 && rows[size_t(previous)] && !rows[size_t(previous)]->isHidden())
      tree->setCurrentItem(rows[size_t(previous)]);
    else
      for (QTreeWidgetItemIterator row(tree); *row; ++row)
        if (!(*row)->isHidden()) {
          tree->setCurrentItem(*row);
          break;
        }
    selectionChanged();
  }
  void changed() {
    if (refreshing)
      return;
    filter();
    ++generation;
    confirmWhenReady = false;
    dismissConfirmation();
    if (render)
      render->cancelled = true;
    result = {};
    preview->message(previewEnabled->isChecked()
        ? "Choose settings, then validate to preview the import." : "Preview disabled.");
    details->setPlainText(inspection.report);
    status->setText(reviewed() ? "Ready to validate your import settings."
                              : "Choose an action for each unsupported layer or group.");
    updateValidationButton();
  }
  void updateValidationButton() {
    import->setEnabled(inspection.source && !closed && !renderJob.valid() && reviewed());
  }
  void validate() {
    prepare(true);
  }
  void prepare(bool confirm) {
    if (closed || !inspection.source || renderJob.valid() || !reviewed())
      return;
    confirmWhenReady = confirm;
    if (result.document) {
      if (previewEnabled->isChecked() && result.preview.isNull())
        startPreview();
      else if (confirm) {
        result.error.clear();
        showConfirmation();
      }
      return;
    }
    render = std::make_shared<PsdJob>();
    renderGeneration = generation;
    progress->show();
    progress->setRange(0, 0);
    status->setText("Validating and preparing proposed import…");
    if (previewEnabled->isChecked()) preview->message("Preparing preview…");
    previewWork = false;
    renderJob = convertPsdAsync(inspection, options, render, limits, false);
    updateValidationButton();
    timer.start(60);
  }
  void startPreview() {
    render = std::make_shared<PsdJob>();
    renderGeneration = generation;
    previewWork = true;
    progress->show();
    progress->setRange(0, 0);
    status->setText("Rendering proposed preview…");
    preview->message("Preparing preview…");
    renderJob = previewPsdImportAsync(std::move(result), render, limits);
    updateValidationButton();
    timer.start(60);
  }
  void prepared() {
    if (previewEnabled->isChecked() && !result.preview.isNull())
      preview->setImage(result.preview);
    else
      preview->message("Preview disabled.");
    details->setPlainText(inspection.report + "\nProposed conversion\n" + result.report);
    status->setText(QStringLiteral(
        "%1 layers ready · %2 MiB layer/mask pixels · %3 MiB estimated working memory.")
        .arg(result.document->layers().size())
        .arg(double(result.rasterBytes) / 1048576, 0, 'f', 1)
        .arg(double(result.estimatedWorkingBytes) / 1048576, 0, 'f', 0)
        + (options.composite ? "\nOne saved image; layer choices are ignored." : ""));
    progress->hide();
    updateValidationButton();
    if (confirmWhenReady) {
      confirmWhenReady = false;
      showConfirmation();
    }
  }
  void dismissConfirmation() {
    if (!confirmation)
      return;
    confirmation->hide();
    confirmation->deleteLater();
    confirmation = nullptr;
    for (const auto &child : confirmationDisabled)
      if (child)
        child->setEnabled(true);
    confirmationDisabled.clear();
    import->setFocus(Qt::OtherFocusReason);
  }
  void showConfirmation() {
    if (closed || confirmation || !result.document)
      return;
    for (auto *child : owner.findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly))
      if (child->isEnabled() && !child->isWindow()) {
        confirmationDisabled.append(child);
        child->setEnabled(false);
      }
    auto *overlay = new QFrame(&owner);
    confirmation = overlay;
    overlay->setObjectName("PsdConfirmationOverlay");
    overlay->setGeometry(owner.rect());
    auto veil = themeColor(ThemeColor::Background);
    veil.setAlpha(190);
    overlay->setStyleSheet(QStringLiteral(
        "QFrame#PsdConfirmationOverlay { background: %1; }"
        "QFrame#PsdConfirmationCard { background: %2; border: 1px solid %3; border-radius: 8px; }")
        .arg(veil.name(QColor::HexArgb), themeColor(ThemeColor::Surface).name(),
             themeColor(ThemeColor::Accent).name()));
    auto *outer = new QVBoxLayout(overlay);
    outer->setContentsMargins(20, 20, 20, 20);
    outer->addStretch();
    auto *card = new QFrame(overlay);
    card->setObjectName("PsdConfirmationCard");
    card->setMaximumWidth(470);
    auto *content = new QVBoxLayout(card);
    content->setContentsMargins(20, 18, 20, 18);
    content->setSpacing(12);
    auto *heading = new QLabel("Import ready", card);
    auto font = heading->font();
    font.setBold(true);
    heading->setFont(font);
    content->addWidget(heading);
    if (previewEnabled->isChecked() && !result.preview.isNull()) {
      auto *image = new QLabel(card);
      image->setAlignment(Qt::AlignCenter);
      image->setPixmap(displayPreview(result.preview).scaled(
          380, 180, Qt::KeepAspectRatio, Qt::SmoothTransformation));
      content->addWidget(image);
    }
    auto *stats = new QLabel(status->text() + "\n\n" + destination->currentText(), card);
    stats->setObjectName("PsdConfirmationStats");
    stats->setTextFormat(Qt::PlainText);
    stats->setWordWrap(true);
    content->addWidget(stats);
    auto *buttons = new QHBoxLayout;
    buttons->addStretch();
    auto *back = new QPushButton("Cancel", card);
    back->setObjectName("PsdConfirmationCancel");
    back->setAutoDefault(false);
    auto *confirm = new QPushButton("Confirm", card);
    confirm->setObjectName("PsdConfirmImport");
    confirm->setAutoDefault(false);
    buttons->addWidget(back);
    buttons->addWidget(confirm);
    content->addLayout(buttons);
    outer->addWidget(card, 0, Qt::AlignHCenter);
    outer->addStretch();
    QObject::connect(back, &QPushButton::clicked, &owner, [this] { dismissConfirmation(); });
    QObject::connect(confirm, &QPushButton::clicked, &owner, [this] {
      if (!closed && result.document && renderGeneration == generation) {
        closed = true;
        owner.accept();
      }
    });
    overlay->show();
    overlay->raise();
    confirm->setFocus(Qt::OtherFocusReason);
  }
  bool reviewed() const {
    if (options.composite)
      return true;
    for (size_t i = 0; i < choices.size(); ++i)
      if (!excluded(i) && (choices[i] < 0 || missingClippingBase(i)))
        return false;
    return true;
  }
  bool missingClippingBase(size_t i) const {
    if(inspection.layers[i].clippingBase<0 || options.layers[i].route==PsdRoute::Skip || options.layers[i].route==PsdRoute::BasePixels)return false;
    const auto base=std::ranges::find_if(inspection.layers,[&](const auto& l){return l.sourceIndex==inspection.layers[i].clippingBase;});
    return base==inspection.layers.end() || options.layers[size_t(base-inspection.layers.begin())].route==PsdRoute::Skip;
  }
  QString statusFor(size_t i) const {
    const auto &info = inspection.layers[i];
    if (choices[i] < 0)
      return "Choose action";
    if (options.layers[i].route == PsdRoute::Skip)
      return "Don’t import";
    if (options.layers[i].route == PsdRoute::BasePixels)
      return "Base pixels only";
    if (options.layers[i].route == PsdRoute::Raster)
      return "Raster";
    if (info.container && info.editable)
      return "Automatic";
    return info.editable && !info.issues.empty()
               ? QStringLiteral("Substitution")
               : info.status;
  }
  void populate() {
    summary->setText(inspection.summary);
    details->setPlainText(inspection.report);
    options = defaultPsdOptions(inspection);
    choices.resize(inspection.layers.size());
    sourceIndexes.clear();
    for (size_t i = 0; i < inspection.layers.size(); ++i) {
      const auto &info = inspection.layers[i];
      sourceIndexes[info.sourceIndex] = i;
      choices[i] = info.suggested == PsdRoute::Skip && info.visible
          ? -1 : int(info.suggested);
    }
    if (inspection.savedComposite)
      mode->addItem("Saved composite as one image", true);
    changed();
  }
  void poll() {
    if (closed)
      return;
    if (ready(loadJob)) {
      inspection = loadJob.get();
      if (!inspection.error.isEmpty()) {
        status->setText(inspection.error);
        progress->hide();
        timer.stop();
        return;
      }
      populate();
      progress->hide();
    }
    if (ready(renderJob)) {
      auto converted = renderJob.get();
      const bool wasPreview = previewWork;
      previewWork = false;
      if (renderGeneration == generation && (!converted.cancelled || wasPreview)) {
        // Cancelling only the optional preview retains already prepared layers.
        // Generation changes still discard all stale work.
        converted.cancelled = false;
        if (!converted.error.isEmpty()) {
          status->setText(converted.error);
          if (wasPreview) result = std::move(converted);
          progress->hide();
        } else if (converted.document) {
          result = std::move(converted);
          if (previewEnabled->isChecked() && result.preview.isNull()) {
            startPreview();
            return;
          }
          prepared();
        }
      }
      progress->hide();
      if (!confirmation)
        updateValidationButton();
    }
    if (renderJob.valid() && render && renderGeneration == generation) {
      if (render->renderingPreview) {
        status->setText("Rendering proposed preview…");
        progress->setRange(0, 100);
        progress->setValue(render->previewPercent);
      } else if (render->total > 0) {
        progress->setRange(0, render->total);
        progress->setValue(render->completed);
      }
    }
    if (!loadJob.valid() && !renderJob.valid())
      timer.stop();
  }
};
PsdImportDialog::PsdImportDialog(QString p, bool a, bool c, PsdLimits l,
                                 QWidget *parent)
    : QDialog(parent) {
  state_ = std::make_unique<State>(*this, std::move(p), a, c, l);
}
PsdImportDialog::~PsdImportDialog() {
  state_->load->cancelled = true;
  if (state_->render)
    state_->render->cancelled = true;
}
void PsdImportDialog::reject() {
  if (state_->confirmation) {
    state_->dismissConfirmation();
    return;
  }
  state_->closed = true;
  state_->timer.stop();
  state_->load->cancelled = true;
  if (state_->render)
    state_->render->cancelled = true;
  QDialog::reject();
}
void PsdImportDialog::resizeEvent(QResizeEvent *event) {
  QDialog::resizeEvent(event);
  if (state_ && state_->confirmation)
    state_->confirmation->setGeometry(rect());
}
bool PsdImportDialog::intoCurrent() const {
  return state_->destination->currentData().toBool();
}
PsdConversion PsdImportDialog::takeResult() {
  return std::move(state_->result);
}
} // namespace imageeditor::ui
