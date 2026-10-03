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
#include <QSignalBlocker>
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
  QLabel *summary, *status, *fontNote;
  PreviewLabel *preview;
  QCheckBox *attention, *allRaster;
  QComboBox *mode, *destination, *fontRequest;
  QFontComboBox *replacement;
  QPlainTextEdit *details;
  QProgressBar *progress;
  QPushButton *import;
  std::vector<QTreeWidgetItem *> rows;
  std::vector<QComboBox *> routes;
  QPointer<QWidget> confirmation;
  QList<QPointer<QWidget>> confirmationDisabled;
  bool refreshing{}, closed{};
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
    attention = new QCheckBox("Attention needed", &owner);
    attention->setObjectName("PsdAttention");
    policies->addWidget(attention);
    allRaster = new QCheckBox("Import all text as raster", &owner);
    allRaster->setObjectName("PsdRasterText");
    policies->addWidget(allRaster);
    policies->addStretch();
    layout->addLayout(policies);
    auto *body = new QHBoxLayout;
    tree = new QTreeWidget(&owner);
    tree->setObjectName("PsdLayers");
    tree->setHeaderLabels({"Layer / detected type", "Conversion", "Action"});
    tree->setRootIsDecorated(true);
    tree->setMinimumWidth(475);
    tree->setColumnWidth(0, 175);
    tree->setColumnWidth(1, 130);
    tree->header()->setStretchLastSection(true);
    tree->setMinimumHeight(120);
    body->addWidget(tree, 3);
    preview = new PreviewLabel(&owner);
    preview->message("Choose settings, then validate to preview the import.");
    preview->setObjectName("PsdPreview");
    preview->setAlignment(Qt::AlignCenter);
    preview->setMinimumSize(220, 120);
    preview->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    preview->setMaximumWidth(310);
    preview->setWordWrap(true);
    body->addWidget(preview, 2);
    layout->addLayout(body, 1);
    auto *fontRow = new QHBoxLayout;
    fontRequest = new QComboBox(&owner);
    fontRequest->setObjectName("PsdFontRequest");
    replacement = new QFontComboBox(&owner);
    replacement->setObjectName("PsdReplacementFont");
    fontRow->addWidget(fontRequest, 1);
    fontRow->addWidget(replacement, 1);
    layout->addLayout(fontRow);
    fontNote = new QLabel(&owner);
    fontNote->setWordWrap(true);
    fontNote->setTextFormat(Qt::PlainText);
    layout->addWidget(fontNote);
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
    QObject::connect(mode, &QComboBox::currentIndexChanged, &owner, [this] {
      options.composite = mode->currentData().toBool();
      tree->setEnabled(!options.composite);
      allRaster->setEnabled(!options.composite);
      changed();
    });
    QObject::connect(allRaster, &QCheckBox::toggled, &owner, [this](bool on) {
      refreshing = true;
      for (size_t i = 0; i < inspection.layers.size(); ++i)
        if (inspection.layers[i].type == "Text") {
          auto route = on ? PsdRoute::Raster : inspection.layers[i].suggested;
          auto index = routes[i]->findData(int(route));
          if (index >= 0) {
            routes[i]->setCurrentIndex(index);
            options.layers[i].route = route;
          }
        }
      refreshing = false;
      changed();
    });
    QObject::connect(tree, &QTreeWidget::currentItemChanged, &owner,
                     [this] { fonts(); });
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
    fonts();
  }
  int selected() const {
    return tree->currentItem()
               ? tree->currentItem()->data(0, Qt::UserRole).toInt()
               : -1;
  }
  void fonts() {
    refreshing = true;
    QSignalBlocker a(fontRequest), b(replacement);
    fontRequest->clear();
    auto i = selected();
    bool visible = i >= 0 && !inspection.layers[size_t(i)].fonts.empty();
    fontNote->setText(i >= 0 ? inspection.layers[size_t(i)].issues.join('\n')
                             : QString{});
    if (visible) {
      fontRequest->addItems(inspection.layers[size_t(i)].fonts);
      replacement->setCurrentFont(
          QFont(options.layers[size_t(i)].replacements.value(
              fontRequest->currentText())));
    }
    fontRequest->setVisible(visible);
    replacement->setVisible(visible);
    refreshing = false;
  }
  void filter() {
    for (size_t i = 0; i < rows.size(); ++i)
      rows[i]->setHidden(attention->isChecked() &&
                         inspection.layers[i].issues.empty());
    for (auto *row : rows)
      if (!row->isHidden())
        for (auto *parent = row->parent(); parent; parent = parent->parent())
          parent->setHidden(false);
  }
  void changed() {
    if (refreshing)
      return;
    for (size_t i = 0; i < rows.size(); ++i) {
      bool excluded = false;
      for (auto *parent = rows[i]->parent(); parent; parent = parent->parent())
        if (options.layers[size_t(parent->data(0, Qt::UserRole).toInt())]
                .route == PsdRoute::Skip)
          excluded = true;
      routes[i]->setEnabled(!excluded);
      rows[i]->setText(1, excluded ? "Ancestor excluded" : statusFor(i));
      if(!excluded && missingClippingBase(i))rows[i]->setText(1,"Include clipping base, skip this layer, or choose Base pixels only");
    }
    ++generation;
    dismissConfirmation();
    if (render)
      render->cancelled = true;
    result = {};
    preview->message("Choose settings, then validate to preview the import.");
    details->setPlainText(inspection.report);
    status->setText(reviewed() ? "Ready to validate your import settings."
                              : "Choose an action for each visible unsupported layer.");
    updateValidationButton();
  }
  void updateValidationButton() {
    import->setEnabled(inspection.source && !closed && !renderJob.valid() && reviewed());
  }
  void validate() {
    if (closed || !inspection.source || renderJob.valid() || !reviewed())
      return;
    if (result.document) {
      showConfirmation();
      return;
    }
    render = std::make_shared<PsdJob>();
    renderGeneration = generation;
    progress->show();
    progress->setRange(0, 0);
    status->setText("Validating and preparing proposed import…");
    preview->message("Preparing preview…");
    renderJob = convertPsdAsync(inspection, options, render, limits, true);
    updateValidationButton();
    timer.start(60);
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
    auto *image = new QLabel(card);
    image->setAlignment(Qt::AlignCenter);
    image->setPixmap(displayPreview(result.preview).scaled(
        380, 180, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    content->addWidget(image);
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
    for(size_t i=0;i<rows.size();++i)if(routes[i]->isEnabled()&&missingClippingBase(i))return false;
    return std::none_of(routes.begin(), routes.end(), [](auto *route) {
      return route->isEnabled() && route->currentData().toInt() < 0;
    });
  }
  bool missingClippingBase(size_t i) const {
    if(inspection.layers[i].clippingBase<0 || options.layers[i].route==PsdRoute::Skip || options.layers[i].route==PsdRoute::BasePixels)return false;
    const auto base=std::ranges::find_if(inspection.layers,[&](const auto& l){return l.sourceIndex==inspection.layers[i].clippingBase;});
    return base==inspection.layers.end() || options.layers[size_t(base-inspection.layers.begin())].route==PsdRoute::Skip;
  }
  QString statusFor(size_t i) const {
    const auto &info = inspection.layers[i];
    return info.editable && !info.issues.empty()
               ? QStringLiteral("Substitution")
               : info.status;
  }
  void populate() {
    summary->setText(inspection.summary);
    details->setPlainText(inspection.report);
    options = defaultPsdOptions(inspection);
    QMap<int, QTreeWidgetItem *> parents;
    rows.resize(inspection.layers.size());
    routes.resize(rows.size());
    refreshing = true;
    for (size_t j = inspection.layers.size(); j > 0; --j) {
      auto i = j - 1;
      const auto &info = inspection.layers[i];
      auto *row = new QTreeWidgetItem;
      row->setText(0, info.name + " · " + info.type);
      row->setData(0, Qt::UserRole, int(i));
      row->setText(1, statusFor(i));
      row->setToolTip(1, info.issues.join('\n'));
      if (info.parent >= 0 && parents.contains(info.parent))
        parents[info.parent]->addChild(row);
      else
        tree->addTopLevelItem(row);
      parents[info.sourceIndex] = row;
      rows[i] = row;
      auto *combo = new QComboBox(tree);
      combo->setObjectName(QStringLiteral("PsdRoute%1").arg(i));
      if (info.suggested == PsdRoute::Skip && info.visible)
        combo->addItem("Choose action…", -1);
      if (info.editable)
        combo->addItem(info.issues.empty() ? "Import editable"
                                           : "Editable (adapted)",
                       int(PsdRoute::Editable));
      if (info.raster && info.type != "Raster")
        combo->addItem("Import as raster", int(PsdRoute::Raster));
      if (info.basePixels && (!info.raster || info.clippingBase>=0))
        combo->addItem("Base pixels only", int(PsdRoute::BasePixels));
      combo->addItem("Don’t import", int(PsdRoute::Skip));
      combo->setCurrentIndex(
          combo->findData(info.suggested == PsdRoute::Skip && info.visible
                              ? -1
                              : int(info.suggested)));
      tree->setItemWidget(row, 2, combo);
      routes[i] = combo;
      QObject::connect(combo, &QComboBox::currentIndexChanged, &owner,
                       [this, i, combo] {
                         options.layers[i].route =
                             combo->currentData().toInt() < 0
                                 ? PsdRoute::Skip
                                 : PsdRoute(combo->currentData().toInt());
                         changed();
                       });
    }
    tree->expandAll();
    if (inspection.savedComposite)
      mode->addItem("Saved composite as one image", true);
    refreshing = false;
    attention->setChecked(
        std::any_of(inspection.layers.begin(), inspection.layers.end(),
                    [](const auto &info) { return !info.issues.empty(); }));
    filter();
    for (auto it = rows.rbegin(); it != rows.rend(); ++it)
      if (!(*it)->isHidden()) {
        tree->setCurrentItem(*it);
        break;
      }
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
      if (renderGeneration == generation && !converted.cancelled) {
        if (!converted.error.isEmpty()) {
          status->setText(converted.error);
          progress->hide();
        } else {
          result = std::move(converted);
          preview->setImage(result.preview);
          details->setPlainText(inspection.report + "\nProposed conversion\n" +
                                result.report);
          status->setText(QStringLiteral(
                        "%1 layers ready · %2 MiB layer/mask pixels · %3 MiB "
                        "estimated working memory. Review "
                        "substitutions and omitted content above.")
                        .arg(result.document->layers().size())
                        .arg(double(result.rasterBytes) / 1048576, 0, 'f', 1)
                        .arg(double(result.estimatedWorkingBytes) / 1048576, 0, 'f', 0)
              + (options.composite ? "\nOne saved image; layer choices are ignored. No editable text or shapes." : ""));
          progress->hide();
          updateValidationButton();
          showConfirmation();
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
