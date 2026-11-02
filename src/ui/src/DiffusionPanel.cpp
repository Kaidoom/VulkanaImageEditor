#include "imageeditor/ui/DiffusionPanel.hpp"
#include "imageeditor/diffusion/ModelProfiles.h"
#include "imageeditor/diffusion/PromptPresets.h"
#include "imageeditor/ui/CollapsibleSection.hpp"
#include "imageeditor/ui/OverlayDockWorkspace.hpp"
#include "imageeditor/ui/PopupOwnership.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/ui/ToolIcons.hpp"
#include "imageeditor/ui/WorkspaceDialog.hpp"
#include <QAbstractSpinBox>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QRandomGenerator>
#include <QScreen>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTabWidget>
#include <QTextCursor>
#include <QTimer>
#include <QToolButton>
#include <QUuid>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

namespace imageeditor::ui {
namespace {
template <class Base> class ScrollSafe final : public Base {
public:
  using Base::Base;

protected:
  void wheelEvent(QWheelEvent *event) override {
    for (auto *parent = this->parentWidget(); parent;
         parent = parent->parentWidget()) {
      if (auto *area = qobject_cast<QScrollArea *>(parent)) {
        QWheelEvent scroll(
            area->viewport()->mapFromGlobal(event->globalPosition()),
            event->globalPosition(), event->pixelDelta(), event->angleDelta(),
            event->buttons(), event->modifiers(), event->phase(),
            event->inverted(), event->source());
        QCoreApplication::sendEvent(area->viewport(), &scroll);
        event->accept();
        return;
      }
    }
    event->ignore();
  }
};
class SubtleItemDelegate : public QStyledItemDelegate {
public:
  using QStyledItemDelegate::QStyledItemDelegate;
  void paint(QPainter *painter, const QStyleOptionViewItem &option,
             const QModelIndex &index) const override {
    auto item = option;
    const bool selected = item.state.testFlag(QStyle::State_Selected);
    const bool hovered = item.state.testFlag(QStyle::State_MouseOver);
    if (selected || hovered) {
      auto tint = selected ? themeColor(ThemeColor::LayerSelection)
                           : item.palette.color(QPalette::Text);
      tint.setAlphaF(selected ? .22F : .04F);
      painter->save();
      painter->setRenderHint(QPainter::Antialiasing);
      painter->setPen(Qt::NoPen);
      painter->setBrush(tint);
      painter->drawRoundedRect(item.rect.adjusted(0, 2, 0, -2), 7, 7);
      painter->restore();
    }
    item.state &= ~(QStyle::State_Selected | QStyle::State_MouseOver |
                    QStyle::State_HasFocus);
    if (selected)
      item.palette.setColor(QPalette::Text,
                            themeColor(ThemeColor::SelectedText));
    QStyledItemDelegate::paint(painter, item, index);
  }
};
class InputDelegate final : public SubtleItemDelegate {
public:
  using SubtleItemDelegate::SubtleItemDelegate;
  std::function<void(int)> remove;
  std::function<void()> add;
  QSize sizeHint(const QStyleOptionViewItem &,
                 const QModelIndex &index) const override {
    return {120, index.data(Qt::UserRole).toInt() < 0 ? 30 : 48};
  }
  void paint(QPainter *painter, const QStyleOptionViewItem &option,
             const QModelIndex &index) const override {
    if (index.data(Qt::UserRole).toInt() < 0) {
      toolGlyph(ToolGlyph::Plus)
          .paint(painter, QRect(option.rect.center().x() - 8,
                                option.rect.center().y() - 8, 16, 16));
      return;
    }
    auto item = option;
    item.rect.adjust(0, 0, -30, 0);
    SubtleItemDelegate::paint(painter, item, index);
    if (!index.data(Qt::UserRole + 1).toBool())
      toolGlyph(ToolGlyph::Trash)
          .paint(painter, QRect(option.rect.right() - 24,
                                option.rect.center().y() - 8, 16, 16));
  }
  bool editorEvent(QEvent *event, QAbstractItemModel *,
                   const QStyleOptionViewItem &option,
                   const QModelIndex &index) override {
    if (event->type() != QEvent::MouseButtonRelease)
      return false;
    const auto *mouse = static_cast<QMouseEvent *>(event);
    if (mouse->button() != Qt::LeftButton)
      return false;
    if (index.data(Qt::UserRole).toInt() < 0) {
      if (add)
        add();
      return true;
    }
    if (!index.data(Qt::UserRole + 1).toBool() &&
        mouse->position().x() >= option.rect.right() - 30) {
      if (remove)
        remove(index.row());
      return true;
    }
    return false;
  }
};
class ClickableImage final : public QLabel {
public:
  using QLabel::QLabel;
  std::function<void()> clicked;

protected:
  void enterEvent(QEnterEvent *e) override {
    QLabel::enterEvent(e);
    update();
  }
  void leaveEvent(QEvent *e) override {
    QLabel::leaveEvent(e);
    update();
  }
  void paintEvent(QPaintEvent *e) override {
    QLabel::paintEvent(e);
    if (!underMouse() || pixmap().isNull())
      return;
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRect badge(rect().center() - QPoint(20, 20), QSize(40, 40));
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(15, 15, 15, 190));
    painter.drawEllipse(badge);
    toolGlyph(ToolGlyph::ZoomActual, QColor(Qt::white))
        .paint(&painter, badge.adjusted(10, 10, -10, -10));
  }
  void mouseReleaseEvent(QMouseEvent *event) override {
    if (event->button() == Qt::LeftButton &&
        rect().contains(event->position().toPoint()) && clicked) {
      clicked();
      event->accept();
      return;
    }
    QLabel::mouseReleaseEvent(event);
  }
};
class GenerateButton final : public QPushButton {
public:
  using QPushButton::QPushButton;

protected:
  void paintEvent(QPaintEvent *e) override {
    QPushButton::paintEvent(e);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    auto color = themeColor(ThemeColor::Accent);
    if (!isEnabled())
      color.setAlphaF(.3F);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawRoundedRect(QRectF(1, 5, 3, height() - 10), 1.5, 1.5);
  }
};
QString promptText(QWidget *origin, const QString &title, const QString &label,
                   const QString &initial, bool *accepted,
                   bool multiline = false) {
  auto *host = popupTopLevelOwner(origin);
  OverlayDockWorkspace *workspace = nullptr;
  if (host) {
    for (auto *child : host->findChildren<QWidget *>())
      if ((workspace = dynamic_cast<OverlayDockWorkspace *>(child)))
        break;
  }
  // Declare the presenter first: a stack dialog must be destroyed before
  // its QObject parent, otherwise the parent would delete a stack object.
  std::unique_ptr<WorkspaceDialog> presenter;
  if (workspace)
    presenter = std::make_unique<WorkspaceDialog>(*workspace, *host);
  QInputDialog dialog(presenter ? presenter.get() : origin,
                      (presenter ? Qt::SubWindow : Qt::Dialog) |
                          Qt::FramelessWindowHint);
  dialog.setObjectName("DiffusionTextPrompt");
  dialog.setWindowTitle(title);
  dialog.setWindowFlag(Qt::WindowMinMaxButtonsHint, false);
  dialog.setLabelText(title + "\n" + label);
  dialog.setTextValue(initial);
  if (multiline)
    dialog.setOption(QInputDialog::UsePlainTextEditForTextInput);
  if (!presenter)
    preparePopupOwnership(&dialog, origin);
  *accepted = (presenter ? presenter->exec(dialog) : dialog.exec()) ==
              QDialog::Accepted;
  return dialog.textValue();
}

QString chooseFile(QWidget *origin, const QString &title,
                   const QString &initial, const QString &filter) {
  QFileDialog dialog(origin, title, initial, filter);
  dialog.setFileMode(QFileDialog::ExistingFile);
  preparePopupOwnership(&dialog, origin);
  return dialog.exec() == QDialog::Accepted ? dialog.selectedFiles().value(0)
                                            : QString{};
}
QToolButton *iconButton(ToolGlyph icon, const QString &tip, QWidget *parent) {
  auto *b = new QToolButton(parent);
  b->setIcon(toolGlyph(icon));
  b->setToolTip(tip);
  b->setAccessibleName(tip);
  b->setIconSize({16, 16});
  b->setFixedSize(28, 28);
  b->setAutoRaise(true);
  return b;
}
QComboBox *combo(const QStringList &labels, const QStringList &values,
                 QWidget *parent) {
  auto *c = new ScrollSafe<QComboBox>(parent);
  c->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  c->setMinimumContentsLength(6);
  for (int i = 0; i < labels.size(); ++i)
    c->addItem(labels[i], values.value(i, labels[i]));
  return c;
}
void select(QComboBox *c, const QString &value) {
  const int index = c->findData(value);
  if (index >= 0)
    c->setCurrentIndex(index);
}
QJsonValue controlValue(QWidget *w) {
  if (auto *c = qobject_cast<QComboBox *>(w))
    return c->currentData().toString();
  if (auto *c = qobject_cast<QCheckBox *>(w))
    return c->isChecked();
  if (auto *c = qobject_cast<QSpinBox *>(w))
    return c->value();
  if (auto *c = qobject_cast<QDoubleSpinBox *>(w))
    return c->value();
  if (auto *c = qobject_cast<QLineEdit *>(w))
    return c->text();
  return {};
}
void setControl(QWidget *w, const QJsonValue &value) {
  const QSignalBlocker block(w);
  if (auto *c = qobject_cast<QComboBox *>(w)) {
    if (c->findData(value.toString()) < 0)
      c->addItem(value.toString() + " (unavailable)", value.toString());
    select(c, value.toString());
  } else if (auto *c = qobject_cast<QCheckBox *>(w))
    c->setChecked(value.toBool());
  else if (auto *c = qobject_cast<QSpinBox *>(w))
    c->setValue(value.toInt());
  else if (auto *c = qobject_cast<QDoubleSpinBox *>(w))
    c->setValue(value.toDouble());
  else if (auto *c = qobject_cast<QLineEdit *>(w))
    c->setText(value.toString());
}
QJsonObject controls(const QMap<QString, QWidget *> &controls) {
  QJsonObject object;
  for (auto it = controls.begin(); it != controls.end(); ++it)
    object[it.key()] = controlValue(it.value());
  return object;
}
void putControls(const QMap<QString, QWidget *> &controls,
                 const QJsonObject &object) {
  for (auto it = object.begin(); it != object.end(); ++it)
    if (controls.contains(it.key()))
      setControl(controls[it.key()], it.value());
}
QWidget *section(const QString &title, const QString &id, QVBoxLayout *&layout,
                 bool open = false) {
  auto *w = collapsibleSection(i18n::Message(title), id, layout);
  if (auto *header = w->findChild<QToolButton *>(id + "SectionHeader"))
    header->setChecked(open);
  w->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
  if (auto *body = w->findChild<QWidget *>(id + "SectionBody"))
    body->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
  layout->setAlignment(Qt::AlignTop);
  return w;
}
QFormLayout *form(QVBoxLayout *layout) {
  auto *f = new QFormLayout;
  f->setContentsMargins(0, 0, 0, 0);
  f->setHorizontalSpacing(8);
  f->setVerticalSpacing(6);
  f->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  f->setRowWrapPolicy(QFormLayout::WrapLongRows);
  layout->addLayout(f);
  return f;
}
QImage thumbnail(const QImage &source, QSize size) {
  if (source.isNull())
    return {};
  return source.scaled(size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}
} // namespace

struct DiffusionPanel::Impl {
  DiffusionPanel &q;
  QComboBox *mode{}, *outputDestination{}, *backend{}, *models{},
      *configModels{}, *profiles{}, *presets{};
  QTabWidget *tabs{};
  QWidget *configurationPage{};
  QLineEdit *seed{};
  QString workerOverride;
  QComboBox *seedMode{};
  QPushButton *unload{};
  QPlainTextEdit *prompt{}, *negative{}, *sigmas{};
  QSpinBox *width{}, *height{};
  QCheckBox *imageToImage{};
  QLabel *status{}, *regionHint{}, *resultImage{};
  QListWidget *inputList{}, *resultList{};
  QListWidgetItem *pendingItem{};
  QPushButton *generate{}, *apply{}, *discard{}, *cancel{}, *regenerate{},
      *regionsButton{}, *saveModel{}, *deleteModel{};
  QProgressBar *progress{};
  QToolButton *clearRegions{};
  QMap<QString, QWidget *> resource, sampling;
  QMap<QString, QLineEdit *> files;
  QJsonArray modelConfigs, profileList;
  QVector<Input> inputs;
  QVector<Result> results;
  QImage draftPreview;
  QVector<DiffusionRegion> regions;
  QPointer<QDialog> lightbox;
  QPointer<DiffusionPreview> lightboxPreview;
  QPointer<QLabel> lightboxTitle, previewPosition;
  QPointer<QToolButton> previousPreview, nextPreview;
  bool previewShowsResult{};
  QPointer<QPushButton> compareToggle;
  QPointer<QComboBox> compareAlignment;
  QImage comparisonReference;
  QTransform comparisonTransform;
  bool canCompare{};
  bool busy{}, canReplaceSelected{}, updating{};
  diffusion::PromptPresetStore presetStore{
      diffusion::PromptPresetStore::defaultFilePath()};
  QVector<diffusion::PromptPreset> presetList;
  explicit Impl(DiffusionPanel &panel) : q(panel) {}
  QJsonObject profile() const {
    const auto id = profiles->currentData().toString();
    for (const auto &p : profileList)
      if (p.toObject()["id"].toString() == id)
        return p.toObject();
    return {};
  }
  int maxInputs() const {
    const bool annotation =
        profile().value("spatialGuidance").toString() == "annotation" &&
        !regions.isEmpty();
    return profile().value("maxReferenceImages").toInt(8) -
           (annotation ? 1 : 0);
  }
  void updateActions() {
    const bool ready = q.selectedResult() != nullptr;
    const bool configured = !files["diffusion"]->text().trimmed().isEmpty() &&
                            !files["textEncoder"]->text().trimmed().isEmpty() &&
                            !files["vae"]->text().trimmed().isEmpty();
    models->setItemText(0, configured ? "Unsaved configuration"
                                      : "<Missing configuration>");
    const bool hasInputs = q.mode() == Mode::Generate || !inputs.isEmpty();
    generate->setEnabled(!busy && configured && hasInputs);
    unload->setEnabled(!busy);
    generate->setToolTip(
        configured ? QString{} : "Add model files in the Configuration tab.");
    regenerate->setEnabled(!busy && ready && configured && hasInputs);
    apply->setEnabled(!busy && ready);
    cancel->setVisible(busy);
    discard->setEnabled(!busy && ready);
    resultImage->setVisible(true);
    tabs->setTabText(2, results.isEmpty()
                            ? "Preview"
                            : QString("Preview · %1").arg(results.size()));
    outputDestination->setVisible(ready);
    resultList->setVisible(resultList->count() > 0);
    if (previousPreview)
      previousPreview->setEnabled(resultList->count() > 1);
    if (nextPreview)
      nextPreview->setEnabled(resultList->count() > 1);
    if (previewPosition)
      previewPosition->setText(QString("%1 / %2")
                                   .arg(resultList->currentRow() + 1)
                                   .arg(resultList->count()));
    if (compareToggle)
      compareToggle->setEnabled(canCompare && ready);
    if (compareAlignment)
      compareAlignment->setEnabled(ready && compareToggle &&
                                   compareToggle->isChecked());
    if (previewShowsResult && lightboxPreview && compareToggle)
      lightboxPreview->setComparisonEnabled(ready &&
                                            compareToggle->isChecked());
    apply->setVisible(ready);
    regenerate->setVisible(ready);
    discard->setVisible(ready);
    inputList->setVisible(true);
    configurationPage->setEnabled(!busy);
    models->setEnabled(!busy);
    seedMode->setEnabled(!busy);
    seed->setEnabled(!busy && seedMode->currentData() != "random");
    profiles->setEnabled(!busy);
    mode->setEnabled(!busy);
    inputList->setEnabled(!busy);
    if (auto *add = inputList->item(int(inputs.size())))
      add->setFlags(!busy && inputs.size() < maxInputs() ? Qt::ItemIsEnabled
                                                         : Qt::NoItemFlags);
    clearRegions->setEnabled(!busy && !regions.isEmpty());
    if (auto *header =
            q.findChild<QToolButton *>("DiffusionRegionsSectionHeader"))
      header->setText(regions.isEmpty()
                          ? QString("Regions")
                          : QString("Regions · %1").arg(regions.size()));
    regionsButton->setEnabled(
        !busy && profile().value("spatialGuidance").toString() != "none");

    if (auto *model =
            qobject_cast<QStandardItemModel *>(outputDestination->model()))
      model->item(int(Destination::ReplaceSelected))
          ->setEnabled(canReplaceSelected);
    if (!canReplaceSelected && q.destination() == Destination::ReplaceSelected)
      q.setDestination(Destination::NewLayer);
  }
  bool pendingSelected() const {
    return pendingItem && resultList->currentItem() == pendingItem;
  }
  void syncPending() {
    const QSignalBlocker block(resultList);
    if (busy && !pendingItem) {
      const int previous = resultList->currentRow();
      pendingItem = new QListWidgetItem(toolGlyph(ToolGlyph::Effects),
                                        "Generating…", resultList);
      pendingItem->setToolTip(
          "Generation in progress. A live draft appears here when available. "
          "Pending previews cannot be applied.");
      if (previous < 0)
        resultList->setCurrentItem(pendingItem);
    } else if (!busy && pendingItem) {
      const bool selected = pendingSelected();
      delete resultList->takeItem(resultList->row(pendingItem));
      pendingItem = nullptr;
      if (selected)
        resultList->setCurrentRow(int(results.size()) - 1);
    }
  }
  void refreshDisplayedPreview() {
    const auto *result = q.selectedResult();
    const QImage image = result              ? result->image
                         : pendingSelected() ? draftPreview
                                             : QImage{};
    const QString title =
        result              ? "AI image preview — " + result->label
        : pendingSelected() ? (image.isNull() ? "Generation in progress"
                                              : "Pending preview — approximate")
                            : "AI image preview";
    if (image.isNull()) {
      resultImage->clear();
      resultImage->setText(
          pendingSelected() ? "Generating…" : "Generated previews appear here");
    } else
      resultImage->setPixmap(QPixmap::fromImage(
          thumbnail(image, {std::max(80, resultImage->width()), 180})));
    resultImage->setToolTip(
        pendingSelected()
            ? "Live draft only. Final detail appears when generation finishes."
            : "Click to inspect full resolution");
    if (previewShowsResult && lightboxPreview) {
      lightboxPreview->setEmptyText(pendingSelected() ? "Generating…"
                                                      : "No image");
      lightboxPreview->setImage(image, true);
      if (lightboxTitle)
        lightboxTitle->setText(title + (image.isNull()
                                            ? QString{}
                                            : QString(" · %1 × %2 px")
                                                  .arg(image.width())
                                                  .arg(image.height())));
    }
    updateActions();
  }
  void stepPreview(int direction) {
    const int count = resultList->count();
    if (count < 2)
      return;
    const int current = std::max(0, resultList->currentRow());
    const int next = (current + direction + count) % count;
    resultList->setCurrentRow(next);
    resultList->scrollToItem(resultList->item(next));
  }
  void updateCapabilities(bool defaults) {
    const auto p = profile();
    const bool qwen = p.value("family").toString() == "qwen-image-2.1";
    const bool manual =
        qobject_cast<QComboBox *>(resource["weights"])->currentData() ==
        "manual";
    for (const auto &name :
         {QString("diffusion"), QString("textEncoder"), QString("vae")})
      resource["placement_" + name]->setEnabled(manual);
    for (const auto &key : {QString("prefixCache"), QString("prefixCacheType")})
      resource[key]->setEnabled(qwen);
    files["vision"]->parentWidget()->setEnabled(
        p.value("visionRequiredForReferences").toBool());
    if (defaults) {
      putControls(sampling,
                  {{"steps", p.value("steps")},
                   {"guidance", p.value("guidance")},
                   {"sampler", p.value("sampler")},
                   {"scheduler", p.value("scheduler").toString("auto")},
                   {"schedule", p.value("schedule")},
                   {"flowShift", 0.0},
                   {"strength", .75},
                   {"batch", 1},
                   {"vaeTiling", false},
                   {"vaeTileSize", 256},
                   {"vaeTileOverlap", .25},
                   {"preview", true},
                   {"previewInterval", 1},
                   {"eta", -1},
                   {"cacheMode", "disabled"},
                   {"cacheReuseThreshold", .2},
                   {"cacheStart", .15},
                   {"cacheEnd", .95}});
      sigmas->clear();
    }
    const double cfg =
        qobject_cast<QDoubleSpinBox *>(sampling["guidance"])->value();
    negative->setEnabled(cfg > 1);
    negative->setVisible(cfg > 1);
    const auto schedule = qobject_cast<QComboBox *>(sampling["schedule"])
                              ->currentData()
                              .toString();
    sigmas->setEnabled(schedule == "custom");
    sampling["scheduler"]->setEnabled(schedule == "auto");
    sampling["flowShift"]->setEnabled(schedule == "auto");
    auto *scheduleCombo = qobject_cast<QComboBox *>(sampling["schedule"]);
    if (auto *items =
            qobject_cast<QStandardItemModel *>(scheduleCombo->model()))
      items->item(scheduleCombo->findData("turbo"))
          ->setEnabled(p.value("id") == "qwen-image-2.1-turbo");
    const auto spatial = p.value("spatialGuidance").toString();
    regionHint->setText(
        spatial == "structured-caption"
            ? "Semantic placement guidance (experimental). Regions do not "
              "protect pixels."
        : spatial == "annotation"
            ? "Regions are supplied as visual annotations. Use Generative Fill "
              "to protect pixels outside a selection."
            : "This model does not expose region guidance.");
    sampling["strength"]->setEnabled(
        q.mode() == Mode::Fill ||
        (q.mode() == Mode::Edit && imageToImage->isChecked()));
    imageToImage->setEnabled(q.mode() == Mode::Edit && !busy);
    const auto sampler = qobject_cast<QComboBox *>(sampling["sampler"])
                             ->currentData()
                             .toString();
    const bool stochastic =
        sampler.contains("_a") || sampler.contains("sde") ||
        QStringList{"ddim_trailing", "tcd", "res_multistep", "res_2s"}.contains(
            sampler);
    sampling["eta"]->setEnabled(stochastic);
    const bool caching = qobject_cast<QComboBox *>(sampling["cacheMode"])
                             ->currentData()
                             .toString() != "disabled";
    for (const auto &key : {QString("cacheReuseThreshold"),
                            QString("cacheStart"), QString("cacheEnd")})
      sampling[key]->setEnabled(caching);
    updateActions();
  }
  void rebuildModels() {
    const auto selected = models->currentData().toString();
    for (auto *box : {models, configModels}) {
      const QSignalBlocker block(box);
      box->clear();
      box->addItem(box == models ? "<Missing configuration>"
                                 : "New configuration",
                   QString{});
      for (const auto &entry : modelConfigs) {
        const auto o = entry.toObject();
        box->addItem(o["name"].toString(), o["id"].toString());
      }
      select(box, selected);
    }
  }
  void rebuildInputs() {
    const QSignalBlocker block(inputList);
    const int selected = inputList->currentRow();
    inputList->clear();
    for (int i = 0; i < inputs.size(); ++i) {
      const auto &in = inputs[i];
      auto *item = new QListWidgetItem(
          QIcon(QPixmap::fromImage(thumbnail(in.image, {48, 40}))),
          QString("Image %1%2\n%3")
              .arg(i + 1)
              .arg(i == 0 && q.mode() != Mode::Generate ? " · Edit image" : "")
              .arg(in.label),
          inputList);
      item->setData(Qt::UserRole, i);
      item->setData(Qt::UserRole + 1, false);
      item->setToolTip(QString("%1 · %2 × %3 px\n%4")
                           .arg(in.source)
                           .arg(in.image.width())
                           .arg(in.image.height())
                           .arg(in.label));
    }
    auto *add = new QListWidgetItem(inputList);
    add->setData(Qt::UserRole, -1);
    add->setToolTip("Add input image");
    inputList->setFixedHeight(std::min(5, int(inputs.size())) * 48 + 32);
    inputList->setCurrentRow(std::min(selected, int(inputs.size()) - 1));
    updateActions();
  }
  void rebuildPresets() {
    presetList = presetStore.allPresets();
    const QSignalBlocker block(presets);
    presets->clear();
    presets->addItem("Prompt preset…");
    for (const auto &p : presetList)
      presets->addItem(p.name, p.id);
  }
  void showPreview(const QImage &image, const QImage &, const QString &title,
                   bool editRegions = false, bool resultPreview = false) {
    if (image.isNull() && !resultPreview)
      return;
    if (lightbox)
      lightbox->close();
    auto *dialog = new QDialog(&q, Qt::Dialog | Qt::FramelessWindowHint);
    lightbox = dialog;
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setObjectName("DiffusionPreviewDialog");
    dialog->setWindowTitle(title);
    dialog->setMinimumSize(480, 320);
    const auto available = q.screen()->availableGeometry().size();
    dialog->resize(std::min(1000, int(available.width() * .85)),
                   std::min(760, int(available.height() * .85)));
    preparePopupOwnership(dialog, &q);
    auto *root = new QVBoxLayout(dialog);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(8);
    auto *bar = new QHBoxLayout;
    auto *label = new QLabel(
        title +
        QString(" · %1 × %2 px").arg(image.width()).arg(image.height()));
    lightboxTitle = label;
    label->setWordWrap(true);
    bar->addWidget(label, 1);
    if (resultPreview) {
      auto *previous = new QToolButton(dialog);
      previousPreview = previous;
      previous->setObjectName("DiffusionPreviousPreview");
      previous->setArrowType(Qt::LeftArrow);
      previous->setToolTip("Previous preview");
      previous->setAccessibleName("Previous preview");
      auto *next = new QToolButton(dialog);
      nextPreview = next;
      next->setObjectName("DiffusionNextPreview");
      next->setArrowType(Qt::RightArrow);
      next->setToolTip("Next preview");
      next->setAccessibleName("Next preview");
      for (auto *button : {previous, next}) {
        button->setAutoRaise(true);
        button->setFixedSize(28, 28);
      }
      previewPosition = new QLabel(dialog);
      bar->addWidget(previous);
      bar->addWidget(previewPosition);
      bar->addWidget(next);
      QObject::connect(previous, &QToolButton::clicked, &q,
                       [this] { stepPreview(-1); });
      QObject::connect(next, &QToolButton::clicked, &q,
                       [this] { stepPreview(1); });
    }
    auto *fit = iconButton(ToolGlyph::FitCanvas, "Fit to view", dialog);
    auto *actual =
        iconButton(ToolGlyph::ZoomActual, "Actual pixels (1:1)", dialog);
    auto *close = iconButton(ToolGlyph::Close, "Close preview", dialog);
    auto *zoom = new QLabel;
    bar->addWidget(zoom);
    bar->addWidget(fit);
    bar->addWidget(actual);
    bar->addWidget(close);
    root->addLayout(bar);
    auto *preview = new DiffusionPreview(dialog);
    lightboxPreview = preview;
    previewShowsResult = resultPreview;
    preview->setImage(image);
    preview->setRegionEditing(editRegions);
    preview->setRegions(regions);
    root->addWidget(preview, 1);
    QObject::connect(fit, &QToolButton::clicked, preview,
                     &DiffusionPreview::fitView);
    QObject::connect(actual, &QToolButton::clicked, preview,
                     &DiffusionPreview::actualSize);
    QObject::connect(close, &QToolButton::clicked, dialog, &QDialog::close);
    preview->onViewChanged = [zoom, preview] {
      zoom->setText(QString::number(preview->zoom() * 100, 'f', 0) + "%");
    };
    preview->onViewChanged();
    auto *bottom = new QHBoxLayout;
    auto *hint =
        new QLabel(editRegions ? "Drag to draw or move · Shift-drag adds a "
                                 "region · Middle mouse pans"
                               : "Mouse wheel zooms · Middle mouse pans");
    hint->setWordWrap(true);
    bottom->addWidget(hint, 1);
    if (resultPreview) {
      auto *compare = new QPushButton("Compare selected layer");
      compare->setObjectName("DiffusionCompareSelected");
      compare->setCheckable(true);
      compare->setEnabled(canCompare && q.selectedResult());
      compareToggle = compare;
      bottom->addWidget(compare);
      auto *alignment =
          combo({"Canvas Alignment", "Center Alignment", "Fit to Result"},
                {"canvas", "center", "fit"}, dialog);
      alignment->setObjectName("DiffusionCompareAlignment");
      alignment->setMinimumWidth(180);
      alignment->setEnabled(false);
      alignment->setToolTip(
          "Canvas: document positions and scale. Center: native pixel sizes. "
          "Fit: proportional reference scaling.");
      compareAlignment = alignment;
      bottom->addWidget(alignment);
      QObject::connect(compare, &QPushButton::toggled, &q,
                       [this, preview, alignment](bool on) {
                         alignment->setEnabled(on);
                         preview->setComparisonEnabled(on);
                         if (on && q.onCompareRequest)
                           q.onCompareRequest();
                       });
      QObject::connect(alignment, &QComboBox::currentIndexChanged, preview,
                       [preview](int index) {
                         preview->setComparisonAlignment(
                             DiffusionPreview::Alignment(index));
                       });
    }
    if (editRegions) {
      auto *regionName = new QLineEdit;
      regionName->setPlaceholderText("Region label");
      regionName->setMaxLength(200);
      regionName->setMaximumWidth(240);
      bottom->addWidget(regionName);
      auto *remove =
          iconButton(ToolGlyph::Trash, "Remove selected region", dialog);
      bottom->addWidget(remove);
      preview->onRegionSelected = [regionName, preview](int index) {
        QSignalBlocker b(regionName);
        regionName->setText(index >= 0 ? preview->regions().value(index).label
                                       : QString{});
      };
      preview->onRegionsChanged = [this, preview] {
        regions = preview->regions();
        updateActions();
        if (inputs.size() > maxInputs())
          q.setStatus(
              QString("The region guide uses one reference slot. Remove an "
                      "input image "
                      "before generating (maximum %1 images with regions).")
                  .arg(maxInputs()),
              true);
      };
      QObject::connect(regionName, &QLineEdit::textEdited, preview,
                       &DiffusionPreview::setSelectedRegionLabel);
      QObject::connect(remove, &QToolButton::clicked, preview,
                       &DiffusionPreview::removeSelectedRegion);
    }
    root->addLayout(bottom);
    if (resultPreview)
      refreshDisplayedPreview();
    dialog->show();
    dialog->raise();
    preview->setFocus();
  }
};

DiffusionPanel::DiffusionPanel(QWidget *parent)
    : QWidget(parent), d(std::make_unique<Impl>(*this)) {
  setObjectName("DiffusionPanel");
  setMinimumWidth(460);
  auto *outer = new QVBoxLayout(this);
  outer->setContentsMargins(8, 8, 8, 8);
  outer->setSpacing(6);
  auto *titleRow = new QHBoxLayout;
  auto *title = new QLabel("AI Image Editor");
  titleRow->addWidget(title, 1);
  auto *close = iconButton(ToolGlyph::Close, "Close AI Image Editor", this);
  titleRow->addWidget(close);
  outer->addLayout(titleRow);
  connect(close, &QToolButton::clicked, this, [this] {
    if (onClose)
      onClose();
  });
  d->tabs = new QTabWidget(this);
  d->tabs->setObjectName("DiffusionTabs");
  outer->addWidget(d->tabs, 1);
  auto page = [&](const QString &title, const QString &name,
                  QVBoxLayout *&layout) {
    auto *scroll = new QScrollArea;
    scroll->setObjectName(name);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *content = new QWidget;
    layout = new QVBoxLayout(content);
    layout->setContentsMargins(4, 8, 4, 4);
    layout->setSpacing(7);
    scroll->setWidget(content);
    d->tabs->addTab(scroll, title);
    return content;
  };
  QVBoxLayout *layout{}, *configLayout{}, *outputLayout{};
  d->configurationPage =
      page("Configuration", "DiffusionConfigurationPage", configLayout);
  auto *content = page("Generate", "DiffusionGenerationPage", layout);
  page("Preview", "DiffusionOutputPage", outputLayout);
  auto *footer = new QWidget(this);
  auto *footerLayout = new QVBoxLayout(footer);
  footerLayout->setContentsMargins(0, 0, 0, 0);
  footerLayout->setSpacing(6);
  outer->addWidget(footer);
  auto *top = new QHBoxLayout;
  d->mode = combo({"Generate", "Edit layer", "Generative Fill"},
                  {"generate", "edit", "fill"}, content);
  d->mode->setObjectName("DiffusionMode");
  top->addWidget(d->mode, 1);
  layout->addLayout(top);
  d->models = new ScrollSafe<QComboBox>;
  d->models->setObjectName("DiffusionModels");
  d->models->setSizeAdjustPolicy(
      QComboBox::AdjustToMinimumContentsLengthWithIcon);
  d->models->setMinimumContentsLength(6);
  top->addWidget(d->models, 2);
  QVBoxLayout *modelLayout{};
  d->configModels = new ScrollSafe<QComboBox>;
  d->configModels->setObjectName("DiffusionConfigurationModels");
  configLayout->addWidget(d->configModels);
  configLayout->addWidget(
      section("Model files", "DiffusionModel", modelLayout, true));
  auto *modelForm = form(modelLayout);
  d->profiles = new ScrollSafe<QComboBox>;
  d->profiles->setObjectName("DiffusionProfile");
  modelForm->addRow("Family", d->profiles);
  auto fileRow = [this, content](const QString &key,
                                 const QString &placeholder) {
    auto *row = new QWidget(content);
    auto *h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(3);
    auto *edit = new QLineEdit;
    edit->setPlaceholderText(placeholder);
    edit->setObjectName("DiffusionFile_" + key);
    h->addWidget(edit, 1);
    auto *browse = iconButton(ToolGlyph::Folder, "Choose file", row);
    h->addWidget(browse);
    connect(browse, &QToolButton::clicked, this, [this, edit] {
      const auto path =
          chooseFile(this, "Choose model component", edit->text(),
                     "Models (*.gguf *.safetensors);;All files (*)");
      if (!path.isEmpty())
        edit->setText(path);
    });
    return std::pair{row, edit};
  };
  for (const auto &pair :
       QVector<QPair<QString, QString>>{{"diffusion", "Diffusion weights"},
                                        {"textEncoder", "Text encoder"},
                                        {"vae", "VAE"},
                                        {"vision", "Vision (optional)"}}) {
    auto [row, edit] = fileRow(pair.first, pair.second);
    d->files[pair.first] = edit;
    modelForm->addRow(pair.second, row);
  }
  auto *saveModel = d->saveModel = new QPushButton("Save configuration…");
  saveModel->setObjectName("DiffusionSaveConfiguration");
  auto *deleteModel = d->deleteModel = new QPushButton("Delete");
  auto *modelButtons = new QHBoxLayout;
  modelButtons->addWidget(saveModel, 1);
  modelButtons->addWidget(deleteModel);
  modelLayout->addLayout(modelButtons);
  d->backend =
      combo({"Auto", "Vulkan", "CUDA"}, {"auto", "vulkan", "cuda"}, this);
  d->backend->setObjectName("DiffusionBackend");
  d->backend->setToolTip(
      "Auto prefers an available CUDA worker, then Vulkan. Explicit backends "
      "require the matching installed worker.");
  modelForm->addRow("Backend", d->backend);
  auto *presetRow = new QHBoxLayout;
  d->presets = new ScrollSafe<QComboBox>;
  d->presets->setObjectName("DiffusionPresets");
  presetRow->addWidget(d->presets, 1);
  auto *savePreset =
      iconButton(ToolGlyph::Save, "Save custom prompt preset", content);
  auto *deletePreset =
      iconButton(ToolGlyph::Trash, "Delete custom prompt preset", content);
  presetRow->addWidget(savePreset);
  presetRow->addWidget(deletePreset);
  layout->addLayout(presetRow);
  d->prompt = new QPlainTextEdit;
  d->prompt->setObjectName("DiffusionPrompt");
  d->prompt->setPlaceholderText("Describe the image or the changes you want…");
  d->prompt->setMinimumHeight(80);
  d->prompt->setMaximumHeight(100);
  layout->addWidget(d->prompt);
  auto *essentials = new QVBoxLayout;
  auto *outputRow = new QHBoxLayout;
  outputRow->setSpacing(4);
  outputRow->addWidget(new QLabel("Size"));
  d->width = new ScrollSafe<QSpinBox>;
  d->height = new ScrollSafe<QSpinBox>;
  for (auto *s : {d->width, d->height}) {
    s->setRange(32, 8192);
    s->setSingleStep(32);
    s->setValue(1024);
    s->setFixedWidth(88);
    s->setKeyboardTracking(false);
  }
  d->width->setObjectName("DiffusionWidth");
  d->height->setObjectName("DiffusionHeight");
  d->width->setToolTip("Output width in pixels");
  d->height->setToolTip("Output height in pixels");
  outputRow->addWidget(d->width);
  outputRow->addWidget(new QLabel("×"));
  outputRow->addWidget(d->height);
  auto *resetSize =
      iconButton(ToolGlyph::FitCanvas, "Match layer / canvas size", content);
  resetSize->setObjectName("DiffusionResetSize");
  outputRow->addWidget(resetSize);
  outputRow->addStretch();
  connect(resetSize, &QToolButton::clicked, this, [this] {
    if (onResetSize)
      onResetSize();
  });
  essentials->addLayout(outputRow);
  auto *seedRow = new QHBoxLayout;
  seedRow->addWidget(new QLabel("Seed"));
  d->seed = new QLineEdit("0");
  d->seed->setObjectName("DiffusionSeed");
  d->seed->setToolTip("Use the same seed to reproduce a result.");
  seedRow->addWidget(d->seed, 1);
  d->seedMode = combo({"Random", "Fixed", "Increment"},
                      {"random", "fixed", "increment"}, content);
  d->seedMode->setObjectName("DiffusionSeedMode");
  d->seedMode->setToolTip(
      "Random: new seed per job. Fixed: reuse the seed. Increment: advance it "
      "by the number of generated images.");
  seedRow->addWidget(d->seedMode);
  connect(d->seedMode, &QComboBox::currentIndexChanged, this, [this] {
    d->seed->setEnabled(d->seedMode->currentData() != "random");
    saveSettings();
  });
  essentials->addLayout(seedRow);
  layout->addLayout(essentials);
  QVBoxLayout *inputsLayout{};
  layout->addWidget(
      section("Input images", "DiffusionInputs", inputsLayout, true));
  d->inputList = new QListWidget;
  d->inputList->setObjectName("DiffusionInputsList");
  d->inputList->setIconSize({48, 40});
  d->inputList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  d->inputList->setTextElideMode(Qt::ElideRight);

  d->inputList->setDragDropMode(QAbstractItemView::InternalMove);
  d->inputList->setDefaultDropAction(Qt::MoveAction);
  auto *inputDelegate = new InputDelegate(d->inputList);
  d->inputList->setItemDelegate(inputDelegate);
  inputsLayout->addWidget(d->inputList);
  inputDelegate->add = [this] {
    if (d->busy || d->inputs.size() >= d->maxInputs())
      return;
    auto *menu = new QMenu(d->inputList);
    const QVector<QPair<QString, InputSource>> sources{
        {"Current layer — appearance", InputSource::CurrentLayerAppearance},
        {"Current layer — raw pixels", InputSource::CurrentLayerPixels},
        {"Another document layer — appearance",
         InputSource::DocumentLayerAppearance},
        {"Another document layer — raw pixels",
         InputSource::DocumentLayerPixels},
        {"Rendered visible canvas", InputSource::VisibleCanvas},
        {"Image file…", InputSource::File}};
    for (const auto &s : sources) {
      auto *a = menu->addAction(s.first);
      connect(a, &QAction::triggered, this, [this, source = s.second] {
        if (onAddInput)
          onAddInput(source);
      });
    }
    connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
    showOwnedPopupMenu(menu, d->inputList);
  };
  inputDelegate->remove = [this](int row) {
    if (d->busy || row < 0 || row >= d->inputs.size())
      return;
    d->inputs.removeAt(row);
    d->updateActions();
    // Rebuild after the delegate finishes dispatching the click.
    QTimer::singleShot(0, this, [this] {
      d->rebuildInputs();
      if (onInputsChanged)
        onInputsChanged();
    });
  };
  connect(d->inputList, &QListWidget::currentRowChanged, this,
          [this] { d->updateActions(); });
  connect(d->inputList, &QListWidget::itemDoubleClicked, this,
          [this](QListWidgetItem *item) {
            if (d->inputList->row(item) >= d->inputs.size())
              return;
            const auto &in = d->inputs[d->inputList->row(item)];
            d->showPreview(in.image, {}, in.label);
          });
  connect(d->inputList->model(), &QAbstractItemModel::rowsMoved, this, [this] {
    QVector<Input> reordered;
    for (int row = 0; row < d->inputList->count(); ++row) {
      const int i = d->inputList->item(row)->data(Qt::UserRole).toInt();
      if (i >= 0 && i < d->inputs.size())
        reordered.push_back(d->inputs[i]);
    }
    d->inputs = std::move(reordered);
    d->rebuildInputs();
    if (onInputsChanged)
      onInputsChanged();
  });
  QVBoxLayout *regionsLayout{};
  layout->addWidget(section("Regions", "DiffusionRegions", regionsLayout));
  d->regionHint = new QLabel;
  d->regionHint->setWordWrap(true);
  regionsLayout->addWidget(d->regionHint);
  d->regionsButton = new QPushButton("Edit regions…");
  d->regionsButton->setObjectName("DiffusionRegionsButton");
  auto *regionButtons = new QHBoxLayout;
  regionButtons->addWidget(d->regionsButton, 1);
  d->clearRegions = iconButton(ToolGlyph::Trash, "Remove all regions", content);
  regionButtons->addWidget(d->clearRegions);
  regionsLayout->addLayout(regionButtons);
  connect(d->clearRegions, &QToolButton::clicked, this, [this] {
    setRegions({});
    if (d->lightboxPreview)
      d->lightboxPreview->setRegions({});
  });
  connect(d->regionsButton, &QPushButton::clicked, this, [this] {
    QImage image;
    if (!d->inputs.isEmpty())
      image = d->inputs.front().image;
    else {
      image = QImage(d->width->value(), d->height->value(),
                     QImage::Format_RGBA8888);
      image.fill(Qt::white);
    }
    d->showPreview(image, {}, "Semantic regions", true);
  });
  QVBoxLayout *advanced{};
  configLayout->addWidget(
      section("Advanced generation", "DiffusionGeneration", advanced));
  auto *samplingForm = form(advanced);
  auto addSpin = [content](QFormLayout *f, QMap<QString, QWidget *> &map,
                           const QString &key, const QString &label, double low,
                           double high, double initial, int decimals = 0) {
    auto *s = new ScrollSafe<QDoubleSpinBox>(content);
    s->setDecimals(decimals);
    s->setRange(low, high);
    s->setValue(initial);
    s->setKeyboardTracking(false);
    s->setObjectName("Diffusion_" + key);
    if (decimals)
      s->setSingleStep(.1);
    f->addRow(label, s);
    map[key] = s;
    return s;
  };
  auto addChoice = [content](QFormLayout *f, QMap<QString, QWidget *> &map,
                             const QString &key, const QString &label,
                             const QStringList &values,
                             const QString &initial) {
    auto *c = combo(values, values, content);
    select(c, initial);
    c->setObjectName("Diffusion_" + key);
    f->addRow(label, c);
    map[key] = c;
    return c;
  };
  auto addFlag = [content](QFormLayout *f, QMap<QString, QWidget *> &map,
                           const QString &key, const QString &label,
                           bool initial) {
    auto *c = new QCheckBox(content);
    c->setChecked(initial);
    c->setObjectName("Diffusion_" + key);
    f->addRow(label, c);
    map[key] = c;
    return c;
  };
  addSpin(samplingForm, d->sampling, "steps", "Steps", 1, 1000, 8);
  auto *guidance = addSpin(samplingForm, d->sampling, "guidance",
                           "Guidance (CFG)", 0, 100, 1, 2);
  addChoice(
      samplingForm, d->sampling, "sampler", "Sampler",
      {"euler", "euler_a", "heun", "dpm2", "dpm++2m", "lcm", "ddim_trailing"},
      "euler");
  addChoice(samplingForm, d->sampling, "scheduler", "Scheduler",
            {"auto", "discrete", "karras", "exponential", "simple", "flux"},
            "auto");
  auto *schedule =
      addChoice(samplingForm, d->sampling, "schedule", "Sigma schedule",
                {"auto", "turbo", "custom"}, "turbo");
  d->sigmas = new QPlainTextEdit;
  d->sigmas->setObjectName("DiffusionSigmas");
  d->sigmas->setPlaceholderText("1.0, 0.9, …, 0.0");
  d->sigmas->setMaximumHeight(65);
  samplingForm->addRow("Custom sigmas", d->sigmas);
  auto *shift = addSpin(samplingForm, d->sampling, "flowShift", "Flow shift", 0,
                        100, 0, 3);
  shift->setSpecialValueText("Auto");
  shift->setToolTip("Auto uses the model schedule. FLUX.3 recommends 6.93. "
                    "Other values are experimental.");
  d->imageToImage = new QCheckBox("Use image-to-image denoising");
  d->imageToImage->setToolTip("Off uses reference editing. On also starts from "
                              "the target image's latents.");
  advanced->addWidget(d->imageToImage);
  connect(d->imageToImage, &QCheckBox::toggled, this,
          [this] { d->updateCapabilities(false); });
  addSpin(samplingForm, d->sampling, "strength", "Denoising strength", 0, 1,
          .75, 2);
  addSpin(samplingForm, d->sampling, "batch", "Images per job", 1, 8, 1);
  addFlag(samplingForm, d->sampling, "vaeTiling", "VAE tiling", false);
  addSpin(samplingForm, d->sampling, "vaeTileSize", "VAE tile size", 64, 2048,
          256);
  addSpin(samplingForm, d->sampling, "vaeTileOverlap", "VAE tile overlap", 0,
          .5, .25, 2);
  addFlag(samplingForm, d->sampling, "preview", "Progress previews", true);
  addSpin(samplingForm, d->sampling, "previewInterval", "Preview every N steps",
          1, 1000, 1);
  auto *eta = addSpin(samplingForm, d->sampling, "eta", "Sampler noise (eta)",
                      -1, 10, -1, 2);
  eta->setSpecialValueText("Auto");
  auto *cacheMode = addChoice(samplingForm, d->sampling, "cacheMode",
                              "Approximate cache (experimental)",
                              {"disabled", "easycache", "ucache"}, "disabled");
  addSpin(samplingForm, d->sampling, "cacheReuseThreshold",
          "Cache reuse threshold", 0, 1, .2, 3);
  addSpin(samplingForm, d->sampling, "cacheStart", "Cache start fraction", 0, 1,
          .15, 3);
  addSpin(samplingForm, d->sampling, "cacheEnd", "Cache end fraction", 0, 1,
          .95, 3);
  connect(cacheMode, &QComboBox::currentIndexChanged, this,
          [this] { d->updateCapabilities(false); });
  connect(qobject_cast<QComboBox *>(d->sampling["sampler"]),
          &QComboBox::currentIndexChanged, this,
          [this] { d->updateCapabilities(false); });
  d->negative = new QPlainTextEdit;
  d->negative->setObjectName("DiffusionNegativePrompt");
  d->negative->setPlaceholderText("Negative prompt");
  d->negative->setMaximumHeight(70);
  layout->addWidget(d->negative);
  auto *defaults = new QPushButton("Restore recommended generation settings");
  advanced->addWidget(defaults);
  connect(defaults, &QPushButton::clicked, this,
          [this] { d->updateCapabilities(true); });
  connect(guidance, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
          [this] { d->updateCapabilities(false); });
  connect(schedule, &QComboBox::currentIndexChanged, this,
          [this] { d->updateCapabilities(false); });
  QVBoxLayout *resources{};
  configLayout->addWidget(
      section("Memory and performance", "DiffusionResources", resources));
  auto *resourceForm = form(resources);
  auto *device = addChoice(resourceForm, d->resource, "device",
                           "Inference device", {"Auto"}, "Auto");
  device->setItemData(0, QString{});
  auto *budget = addSpin(resourceForm, d->resource, "maxVramGiB",
                         "VRAM budget (GiB)", 0, 1024, 10, 2);
  budget->setSpecialValueText("Auto");
  budget->setToolTip("Budget for managed weights/workspaces, not a physical "
                     "VRAM cap. Leave room for "
                     "Vulkana and the desktop.");
  auto *threads =
      addSpin(resourceForm, d->resource, "threads", "CPU threads", 0, 1024, 0);
  threads->setSpecialValueText("Auto");
  auto *weights =
      addChoice(resourceForm, d->resource, "weights", "Keep weights in",
                {"auto", "gpu", "ram", "disk", "manual"}, "auto");
  for (const auto &name :
       {QString("diffusion"), QString("textEncoder"), QString("vae")})
    addChoice(resourceForm, d->resource, "placement_" + name,
              (name == "diffusion"     ? "Diffusion weights"
               : name == "textEncoder" ? "Text encoder weights"
                                       : "VAE weights"),
              {"gpu", "ram", "disk"}, name == "diffusion" ? "gpu" : "ram");
  for (const auto &name :
       {QString("textEncoderCompute"), QString("vaeCompute")})
    addChoice(resourceForm, d->resource, name,
              name == "textEncoderCompute" ? "Text encoder compute"
                                           : "VAE compute",
              {"gpu", "cpu"}, "gpu");
  for (const auto &name :
       {QString("flashAttention"), QString("directConvolution")})
    addChoice(resourceForm, d->resource, name,
              name == "flashAttention" ? "Flash attention"
                                       : "Direct convolution",
              {"auto", "on", "off"}, "auto");
  for (const auto &pair : QVector<QPair<QString, QString>>{
           {"sageAttention", "Sage attention (experimental)"},
           {"mmap", "Memory mapped loading"},
           {"prefetch", "Prefetch weights"},
           {"segmentedCompute", "Segmented computation"},
           {"eagerLoad", "Prepare weights at load"}})
    addFlag(resourceForm, d->resource, pair.first, pair.second,
            pair.first == "mmap" || pair.first == "prefetch" ||
                pair.first == "segmentedCompute");
  addSpin(resourceForm, d->resource, "conditioningCacheSize", "Cached prompts",
          0, 64, 1);
  addFlag(resourceForm, d->resource, "prefixCache", "Qwen prefix cache", true);
  addChoice(resourceForm, d->resource, "prefixCacheType", "Prefix cache format",
            {"auto", "f32", "f16", "bf16", "q8_0", "q4_0", "q4_1", "q5_0",
             "q5_1", "q4_K", "q6_K"},
            "auto");
  auto updatePlacement = [this] {
    const bool manual =
        qobject_cast<QComboBox *>(d->resource["weights"])->currentData() ==
        "manual";
    for (const auto &name :
         {QString("diffusion"), QString("textEncoder"), QString("vae")})
      d->resource["placement_" + name]->setEnabled(manual);
  };
  connect(weights, &QComboBox::currentIndexChanged, this, updatePlacement);
  updatePlacement();
  d->unload = new QPushButton("Unload all models");
  d->unload->setObjectName("DiffusionUnloadAll");
  configLayout->addWidget(d->unload);
  connect(d->unload, &QPushButton::clicked, this, [this] {
    if (onUnload)
      onUnload();
  });
  d->generate = new GenerateButton(toolGlyph(ToolGlyph::Effects), "Generate");
  d->generate->setObjectName("DiffusionGenerate");
  layout->addWidget(d->generate);
  d->progress = new QProgressBar;
  d->progress->setObjectName("DiffusionProgress");
  d->progress->setRange(0, 0);
  d->progress->setVisible(false);
  footerLayout->addWidget(d->progress);
  d->status = new QLabel("Choose model files and enter a prompt to begin.");
  d->status->setWordWrap(true);
  d->status->setTextInteractionFlags(Qt::TextSelectableByMouse);
  footerLayout->addWidget(d->status);
  d->cancel = new QPushButton("Cancel generation");
  d->cancel->setVisible(false);
  footerLayout->addWidget(d->cancel);
  auto *resultThumbnail = new ClickableImage("Generated previews appear here");
  resultThumbnail->clicked = [this] { showResultPreview(); };
  resultThumbnail->setCursor(Qt::PointingHandCursor);
  resultThumbnail->setToolTip("Click to inspect full resolution");
  d->resultImage = resultThumbnail;
  d->resultImage->setAlignment(Qt::AlignCenter);
  d->resultImage->setMinimumHeight(96);
  d->resultImage->setMaximumHeight(200);
  d->resultImage->setMinimumWidth(1);
  d->resultImage->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  d->resultImage->setObjectName("DiffusionResultThumbnail");
  outputLayout->addWidget(d->resultImage);
  d->resultList = new QListWidget;
  d->resultList->setObjectName("DiffusionResultsList");
  d->resultList->setItemDelegate(new SubtleItemDelegate(d->resultList));
  d->resultList->setViewMode(QListView::IconMode);
  d->resultList->setFlow(QListView::LeftToRight);
  d->resultList->setWrapping(false);
  d->resultList->setIconSize({64, 48});
  d->resultList->setMaximumHeight(95);
  outputLayout->addWidget(d->resultList);
  d->outputDestination =
      combo({"Add new layer", "Replace selected layer", "Open new document"},
            {"newLayer", "replaceSelected", "newDocument"}, this);
  d->outputDestination->setObjectName("DiffusionOutputDestination");
  outputLayout->addWidget(d->outputDestination);
  connect(d->outputDestination, &QComboBox::currentIndexChanged, this, [this] {
    if (onDestinationChanged)
      onDestinationChanged();
  });
  auto *resultsActions = new QHBoxLayout;
  d->apply = new QPushButton("Apply");
  d->apply->setObjectName("DiffusionApply");
  d->regenerate = new QPushButton(toolGlyph(ToolGlyph::Refresh), "Regenerate");
  d->discard = new QPushButton("Discard");
  resultsActions->addWidget(d->apply);
  resultsActions->addWidget(d->regenerate);
  resultsActions->addWidget(d->discard);
  outputLayout->addLayout(resultsActions);
  layout->addStretch();
  configLayout->addStretch();
  outputLayout->addStretch();
  connect(d->generate, &QPushButton::clicked, this, [this] {
    saveSettings();
    if (onGenerate)
      onGenerate();
  });
  connect(d->regenerate, &QPushButton::clicked, this, [this] {
    saveSettings();
    if (onGenerate)
      onGenerate();
  });
  connect(d->apply, &QPushButton::clicked, this, [this] {
    if (onApply)
      onApply();
  });
  connect(d->discard, &QPushButton::clicked, this, [this] {
    if (onDiscard)
      onDiscard();
  });
  connect(d->cancel, &QPushButton::clicked, this, [this] {
    if (onCancel)
      onCancel();
  });
  connect(d->resultList, &QListWidget::itemDoubleClicked, this,
          [this] { showResultPreview(); });
  connect(d->resultList, &QListWidget::currentRowChanged, this, [this] {
    d->refreshDisplayedPreview();
    if (onResultSelected)
      onResultSelected(selectedResultIndex());
  });
  connect(d->mode, &QComboBox::currentIndexChanged, this, [this] {
    d->updateCapabilities(false);
    saveSettings();
    if (onModeChanged)
      onModeChanged(mode());
  });
  connect(d->profiles, &QComboBox::currentIndexChanged, this, [this] {
    d->updateCapabilities(true);
    if (!d->updating && onConfigurationChanged)
      onConfigurationChanged();
  });
  connect(d->configModels, &QComboBox::currentIndexChanged, this,
          [this](int index) { d->models->setCurrentIndex(index); });
  connect(d->models, &QComboBox::currentIndexChanged, this, [this] {
    const QSignalBlocker block(d->configModels);
    d->configModels->setCurrentIndex(d->models->currentIndex());
    if (d->updating)
      return;
    const auto id = d->models->currentData().toString();
    if (id.isEmpty())
      setConfiguration({{"profile", d->profiles->currentData().toString()},
                        {"files", QJsonObject{}}});
    for (const auto &value : d->modelConfigs) {
      const auto c = value.toObject();
      if (c["id"] == id) {
        setConfiguration(c["configuration"].toObject());
        putControls(d->sampling, c["generation"].toObject());
        d->sigmas->setPlainText(c.value("customSigmas").toString());
        break;
      }
    }
    d->updateCapabilities(false);
    if (onConfigurationChanged)
      onConfigurationChanged();
  });
  connect(saveModel, &QPushButton::clicked, this, [this] {
    bool ok = false;
    const auto name =
        promptText(this, "Save model configuration", "Name",
                   d->models->currentIndex() > 0 ? d->models->currentText()
                                                 : QString{},
                   &ok)
            .trimmed();
    if (!ok || name.isEmpty())
      return;
    QJsonObject entry{
        {"id", QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {"name", name},
        {"configuration", configuration()},
        {"generation", controls(d->sampling)},
        {"customSigmas", d->sigmas->toPlainText()}};
    bool replaced = false;
    for (qsizetype i = 0; i < d->modelConfigs.size(); ++i)
      if (d->modelConfigs[i].toObject()["name"].toString() == name) {
        entry["id"] = d->modelConfigs[i].toObject()["id"];
        d->modelConfigs[i] = entry;
        replaced = true;
        break;
      }
    if (!replaced)
      d->modelConfigs.append(entry);
    d->rebuildModels();
    select(d->models, entry["id"].toString());
    saveSettings();
  });
  connect(deleteModel, &QPushButton::clicked, this, [this] {
    const auto id = d->models->currentData();
    if (id.toString().isEmpty())
      return;
    for (qsizetype i = 0; i < d->modelConfigs.size(); ++i)
      if (d->modelConfigs[i].toObject()["id"].toString() == id.toString()) {
        d->modelConfigs.removeAt(i);
        break;
      }
    d->rebuildModels();
    saveSettings();
  });
  connect(d->presets, &QComboBox::activated, this, [this](int row) {
    if (row <= 0 || row > d->presetList.size())
      return;
    const auto &p = d->presetList[row - 1];
    const QMap<QString, QString> values{
        {"instruction",
         p.task == "generate" ? "A small cabin beside a lake at sunrise" : "X"},
        {"background", "a quiet forest"},
        {"subject", "the main subject"},
        {"style", "soft natural lighting"}};
    diffusion::ExpandedPrompt expanded;
    QString error;
    if (!diffusion::PromptPresets::expand(p, values, &expanded, &error)) {
      setStatus(error, true);
      return;
    }
    const auto replaceText = [](QPlainTextEdit *field, const QString &text) {
      auto cursor = field->textCursor();
      cursor.beginEditBlock();
      cursor.select(QTextCursor::Document);
      cursor.insertText(text);
      cursor.endEditBlock();
      field->setTextCursor(cursor);
    };
    replaceText(d->prompt, expanded.prompt);
    replaceText(d->negative, expanded.negativePrompt);
    setStatus(p.description);
  });
  connect(savePreset, &QToolButton::clicked, this, [this] {
    bool ok = false;
    const auto name =
        promptText(this, "Save prompt preset", "Name", {}, &ok).trimmed();
    if (!ok || name.isEmpty())
      return;
    diffusion::PromptPreset preset{
        QUuid::createUuid().toString(QUuid::WithoutBraces),
        name,
        mode() == Mode::Generate ? "generate"
        : mode() == Mode::Fill   ? "fill"
                                 : "edit",
        {},
        d->prompt->toPlainText(),
        d->negative->toPlainText()};
    QString error;
    if (!d->presetStore.upsert(preset, &error)) {
      setStatus(error, true);
      return;
    }
    d->rebuildPresets();
    setStatus("Prompt preset saved.");
  });
  connect(deletePreset, &QToolButton::clicked, this, [this] {
    const auto id = d->presets->currentData().toString();
    if (id.isEmpty() || id.startsWith("builtin.")) {
      setStatus("Choose a custom preset to remove.");
      return;
    }
    QString error;
    if (!d->presetStore.remove(id, &error))
      setStatus(error, true);
    else
      d->rebuildPresets();
  });
  setProfiles(diffusion::modelProfiles());
  QString error;
  if (!d->presetStore.load(&error))
    setStatus(error, true);
  d->rebuildPresets();
  QSettings settings;
  const auto stored = QJsonDocument::fromJson(
      settings.value("diffusion/modelConfigurations").toByteArray());
  if (stored.isArray())
    d->modelConfigs = stored.array();
  d->rebuildModels();
  select(d->models, settings.value("diffusion/modelId").toString());
  const auto savedConfig = QJsonDocument::fromJson(
      settings.value("diffusion/currentConfiguration").toByteArray());
  if (savedConfig.isObject())
    setConfiguration(savedConfig.object());
  select(d->backend, settings.value("diffusion/backend", "auto").toString());
  const auto savedGeneration = QJsonDocument::fromJson(
      settings.value("diffusion/generation").toByteArray());
  if (savedGeneration.isObject())
    putControls(d->sampling, savedGeneration.object());
  d->prompt->setPlainText(settings.value("diffusion/prompt").toString());
  d->negative->setPlainText(
      settings.value("diffusion/negativePrompt").toString());
  const auto savedSeed = settings.value("diffusion/seed", "0").toString();
  d->seed->setText(savedSeed == "-1" ? "0" : savedSeed);
  {
    QSignalBlocker block(d->seedMode);
    select(d->seedMode,
           settings.value("diffusion/seedMode", "random").toString());
  }
  d->seed->setEnabled(d->seedMode->currentData() != "random");
  d->sigmas->setPlainText(settings.value("diffusion/sigmas").toString());
  d->width->setValue(settings.value("diffusion/width", 1024).toInt());
  d->height->setValue(settings.value("diffusion/height", 1024).toInt());
  setMode(Mode(std::clamp(settings.value("diffusion/mode", 0).toInt(), 0, 2)));
  d->updateCapabilities(false);
  d->rebuildInputs();
  connect(d->backend, &QComboBox::currentIndexChanged, this, [this] {
    // Device IDs belong to a backend, so don't carry a CUDA ID into Vulkan.
    select(qobject_cast<QComboBox *>(d->resource["device"]), QString{});
    saveSettings();
  });
  for (auto *edit : d->files)
    connect(edit, &QLineEdit::textChanged, this,
            [this] { d->updateActions(); });
  if (d->files["diffusion"]->text().isEmpty())
    if (auto *header = findChild<QToolButton *>("DiffusionModelSectionHeader"))
      header->setChecked(true);
}
DiffusionPanel::~DiffusionPanel() {
  saveSettings();
  if (d->lightbox)
    d->lightbox->close();
}
DiffusionPanel::Mode DiffusionPanel::mode() const {
  return Mode(d->mode->currentIndex());
}
void DiffusionPanel::showGeneration() { d->tabs->setCurrentIndex(1); }
void DiffusionPanel::showResults() { d->tabs->setCurrentIndex(2); }
void DiffusionPanel::setMode(Mode value, bool notify) {
  const QSignalBlocker b(d->mode);
  d->mode->setCurrentIndex(int(value));
  d->updateCapabilities(false);
  d->rebuildInputs();
  if (notify && onModeChanged)
    onModeChanged(value);
}
DiffusionPanel::Destination DiffusionPanel::destination() const {
  return Destination(d->outputDestination->currentIndex());
}
void DiffusionPanel::setDestination(Destination value) {
  d->outputDestination->setCurrentIndex(int(value));
}
bool DiffusionPanel::imageToImageEnabled() const {
  return mode() == Mode::Fill ||
         (mode() == Mode::Edit && d->imageToImage->isChecked());
}
QString DiffusionPanel::workerExecutable() const { return d->workerOverride; }
void DiffusionPanel::setWorkerExecutable(const QString &path) {
  d->workerOverride = path;
}
QString DiffusionPanel::backend() const {
  return d->backend->currentData().toString();
}
QJsonObject DiffusionPanel::configuration() const {
  QJsonObject files;
  for (auto it = d->files.begin(); it != d->files.end(); ++it)
    if (!it.value()->text().trimmed().isEmpty())
      files[it.key()] = it.value()->text().trimmed();
  auto resources = controls(d->resource);
  QJsonObject placements;
  for (const auto &key :
       {QString("diffusion"), QString("textEncoder"), QString("vae")}) {
    placements[key] = resources.take("placement_" + key);
  }
  if (resources["weights"] == "manual")
    resources["placements"] = placements;
  if (resources["device"].toString().isEmpty())
    resources.remove("device");
  if (d->profile()["family"] != "qwen-image-2.1") {
    resources.remove("prefixCache");
    resources.remove("prefixCacheType");
    files.remove("vision");
  }
  return {{"profile", d->profiles->currentData().toString()},
          {"files", files},
          {"settings", resources}};
}
QJsonObject DiffusionPanel::generationParameters() const {
  auto params = controls(d->sampling);
  if (!d->sampling["eta"]->isEnabled())
    params.remove("eta");
  if (!d->sampling["strength"]->isEnabled())
    params.remove("strength");
  params["prompt"] = d->prompt->toPlainText();
  if (d->negative->isEnabled() &&
      !d->negative->toPlainText().trimmed().isEmpty())
    params["negativePrompt"] = d->negative->toPlainText();
  params["width"] = d->width->value();
  params["height"] = d->height->value();
  bool valid = false;
  const auto seed = d->seed->text().toLongLong(&valid);
  params["seed"] = valid ? QJsonValue(seed) : QJsonValue(d->seed->text());
  if (params["schedule"] == "custom") {
    QString text = d->sigmas->toPlainText().trimmed();
    if (!text.startsWith('['))
      text = '[' + text + ']';
    const auto doc = QJsonDocument::fromJson(text.toUtf8());
    params["sigmas"] = doc.isArray() ? QJsonValue(doc.array())
                                     : QJsonValue(d->sigmas->toPlainText());
  }
  return params;
}
QJsonObject DiffusionPanel::prepareGeneration() {
  if (d->seedMode->currentData() == "random")
    d->seed->setText(QString::number(QRandomGenerator::global()->generate()));
  bool valid = false;
  const auto seed = d->seed->text().toULongLong(&valid);
  if (!valid || seed > 9007199254740991ULL)
    throw std::invalid_argument("Enter a seed from 0 to 9007199254740991.");
  return generationParameters();
}
void DiffusionPanel::generationAccepted() {
  if (d->seedMode->currentData() == "increment") {
    bool valid = false;
    const auto seed = d->seed->text().toULongLong(&valid);
    const auto count = quint64(generationParameters()["batch"].toInt(1));
    if (valid)
      d->seed->setText(QString::number((seed + count) % 9007199254740992ULL));
  }
  saveSettings();
}
void DiffusionPanel::setProfiles(const QJsonArray &profiles) {
  const bool initial = d->profileList.isEmpty();
  d->profileList = profiles;
  const auto current = d->profiles->currentData().toString();
  const QSignalBlocker b(d->profiles);
  d->profiles->clear();
  for (const auto &value : profiles) {
    const auto p = value.toObject();
    d->profiles->addItem(p["name"].toString(), p["id"].toString());
  }
  select(d->profiles, current);
  if (d->resource.contains("prefixCache"))
    d->updateCapabilities(initial);
}
void DiffusionPanel::setDevices(const QJsonArray &devices) {
  auto *c = qobject_cast<QComboBox *>(d->resource["device"]);
  const auto current = c->currentData().toString();
  const QSignalBlocker b(c);
  c->clear();
  c->addItem("Auto", QString{});
  for (const auto &value : devices) {
    const auto v = value.toObject();
    c->addItem(v["name"].toString(), v["id"].toString());
  }
  if (!current.isEmpty() && c->findData(current) < 0)
    c->addItem(current + " (unavailable)", current);
  select(c, current);
}
void DiffusionPanel::setSamplers(const QJsonArray &samplers,
                                 const QJsonArray &schedulers) {
  for (const auto &pair : QVector<QPair<QString, QJsonArray>>{
           {"sampler", samplers}, {"scheduler", schedulers}}) {
    auto *c = qobject_cast<QComboBox *>(d->sampling[pair.first]);
    const auto current = c->currentData().toString();
    const QSignalBlocker b(c);
    c->clear();
    if (pair.first == "scheduler")
      c->addItem("Auto", "auto");
    for (const auto &value : pair.second)
      c->addItem(value.toString(), value.toString());
    if (!current.isEmpty() && c->findData(current) < 0)
      c->addItem(current + " (unavailable)", current);
    select(c, current);
  }
}
void DiffusionPanel::setConfiguration(const QJsonObject &config,
                                      const QString &) {
  d->updating = true;
  select(d->profiles, config["profile"].toString());
  const auto files = config["files"].toObject();
  for (auto it = d->files.begin(); it != d->files.end(); ++it)
    it.value()->setText(files[it.key()].toString());
  const auto resources = config["settings"].toObject();
  putControls(d->resource, resources);
  const auto placements = resources["placements"].toObject();
  for (auto it = placements.begin(); it != placements.end(); ++it)
    if (d->resource.contains("placement_" + it.key()))
      setControl(d->resource["placement_" + it.key()], it.value());
  if (resources.contains("device")) {
    auto *c = qobject_cast<QComboBox *>(d->resource["device"]);
    if (c->findData(resources["device"].toString()) < 0)
      c->addItem(resources["device"].toString(),
                 resources["device"].toString());
    select(c, resources["device"].toString());
  }
  d->updating = false;
  d->updateCapabilities(false);
}
void DiffusionPanel::setResolution(QSize size) {
  const int a = d->profile().value("alignment").toInt(32);
  d->width->setValue(std::max(a, ((size.width() + a - 1) / a) * a));
  d->height->setValue(std::max(a, ((size.height() + a - 1) / a) * a));
}
const QVector<DiffusionPanel::Input> &DiffusionPanel::inputs() const {
  return d->inputs;
}
bool DiffusionPanel::addInput(Input input) {
  if (input.image.isNull())
    return false;
  if (d->inputs.size() >= d->maxInputs()) {
    setStatus(QString("This model accepts up to %1 input images.")
                  .arg(d->maxInputs()),
              true);
    return false;
  }
  input.image = input.image.copy();
  d->inputs.append(std::move(input));
  d->rebuildInputs();
  if (onInputsChanged)
    onInputsChanged();
  return true;
}
void DiffusionPanel::setInputs(QVector<Input> inputs) {
  d->inputs = std::move(inputs);
  d->rebuildInputs();
}
void DiffusionPanel::clearInputs() {
  d->inputs.clear();
  d->regions.clear();
  d->rebuildInputs();
}
QJsonArray DiffusionPanel::regionsJson() const {
  QJsonArray array;
  for (const auto &r : d->regions)
    if (!r.bounds.isEmpty())
      array.append(QJsonObject{{"x", r.bounds.x()},
                               {"y", r.bounds.y()},
                               {"width", r.bounds.width()},
                               {"height", r.bounds.height()},
                               {"label", r.label}});
  return array;
}
void DiffusionPanel::setRegions(QVector<DiffusionRegion> regions) {
  d->regions = std::move(regions);
  d->updateActions();
  if (d->inputs.size() > d->maxInputs())
    setStatus(QString("The region guide uses one reference slot. Remove an "
                      "input image before generating "
                      "(maximum %1 images with regions).")
                  .arg(d->maxInputs()),
              true);
}
QVector<DiffusionRegion> DiffusionPanel::regions() const { return d->regions; }
void DiffusionPanel::appendResult(Result result) {
  if (result.image.isNull())
    return;
  d->resultImage->setToolTip("Click to inspect full resolution");
  d->results.append(std::move(result));
  const auto &r = d->results.back();
  auto *item = new QListWidgetItem(
      QIcon(QPixmap::fromImage(thumbnail(r.image, {64, 48}))),
      r.label.isEmpty() ? QString("%1").arg(d->results.size()) : r.label);
  d->resultList->insertItem(int(d->results.size()) - 1, item);
  QString details =
      QString("%1 × %2 px").arg(r.image.width()).arg(r.image.height());
  const auto request = r.metadata["request"].toObject();
  if (request.contains("seed"))
    details +=
        QString(" · Seed %1").arg(request["seed"].toVariant().toString());
  const QSize requested(request["width"].toInt(), request["height"].toInt());
  if (requested.isValid() && requested != r.image.size())
    details += QString("\nRequested: %1 × %2 px")
                   .arg(requested.width())
                   .arg(requested.height());
  item->setToolTip(details);
  d->resultList->setCurrentRow(int(d->results.size()) - 1);
  d->tabs->setCurrentIndex(2);
  d->updateActions();
}
void DiffusionPanel::setResultPreview(int index, QImage image) {
  if (index < 0 || index >= d->results.size() || image.isNull())
    return;
  d->results[index].image = std::move(image);
  d->resultList->item(index)->setIcon(
      QIcon(QPixmap::fromImage(thumbnail(d->results[index].image, {64, 48}))));
  if (index == selectedResultIndex())
    d->refreshDisplayedPreview();
}
const QVector<DiffusionPanel::Result> &DiffusionPanel::results() const {
  return d->results;
}
int DiffusionPanel::selectedResultIndex() const {
  const int row = d->resultList->currentRow();
  return row >= 0 && row < d->results.size() ? row : -1;
}
const DiffusionPanel::Result *DiffusionPanel::selectedResult() const {
  const int row = selectedResultIndex();
  return row >= 0 && row < d->results.size() ? &d->results[row] : nullptr;
}
void DiffusionPanel::clearResults() {
  d->results.clear();
  d->draftPreview = {};
  d->pendingItem = nullptr;
  d->resultList->clear();
  d->syncPending();
  d->resultImage->clear();
  d->resultImage->setText("Generated previews appear here");
  if (d->lightbox)
    d->lightbox->close();
  d->updateActions();
}
void DiffusionPanel::removeSelectedResult() {
  const int index = selectedResultIndex();
  if (index < 0 || index >= d->results.size())
    return;
  if (d->lightbox)
    d->lightbox->close();
  const QSignalBlocker block(d->resultList);
  d->results.removeAt(index);
  delete d->resultList->takeItem(index);
  if (d->results.isEmpty()) {
    d->resultImage->clear();
    d->resultImage->setText("Generated previews appear here");
  } else {
    const int next = std::min(index, int(d->results.size()) - 1);
    d->resultList->setCurrentRow(next);
    d->resultImage->setPixmap(
        QPixmap::fromImage(thumbnail(d->results[next].image, {420, 180})));
  }
  d->updateActions();
  if (onResultSelected)
    onResultSelected(selectedResultIndex());
}
void DiffusionPanel::showResultPreview() {
  if (const auto *result = selectedResult())
    d->showPreview(result->image, result->before,
                   "AI image preview — " + result->label, false, true);
  else if (d->pendingSelected())
    d->showPreview(d->draftPreview, {}, "Pending preview — approximate", false,
                   true);
}
void DiffusionPanel::setBusy(bool busy) {
  if (busy != d->busy)
    d->draftPreview = {};
  d->busy = busy;
  d->syncPending();
  d->progress->setVisible(busy);
  if (busy)
    d->progress->setRange(0, 0);
  d->refreshDisplayedPreview();
  if (onResultSelected)
    onResultSelected(selectedResultIndex());
}
bool DiffusionPanel::isBusy() const { return d->busy; }
void DiffusionPanel::setStatus(const QString &text, bool error) {
  d->status->setText(text);
  d->status->setStyleSheet(error ? "color: #e88a79;" : QString{});
}
void DiffusionPanel::setProgress(int step, int steps, double elapsed) {
  if (steps > 0) {
    d->progress->setRange(0, steps);
    d->progress->setValue(step);
  }
  d->progress->setFormat(QString("%1 / %2 · %3 s")
                             .arg(step)
                             .arg(steps)
                             .arg(elapsed / 1000., 0, 'f', 1));
}
void DiffusionPanel::setProgressPreview(QImage image) {
  if (!d->busy || image.isNull())
    return;
  d->draftPreview = std::move(image);
  d->syncPending();
  d->pendingItem->setText("Pending");
  d->pendingItem->setIcon(
      QIcon(QPixmap::fromImage(thumbnail(d->draftPreview, {64, 48}))));
  d->pendingItem->setToolTip("Live draft — approximate. Final detail appears "
                             "after generation finishes.");
  if (d->pendingSelected())
    d->refreshDisplayedPreview();
  else
    d->updateActions();
}
void DiffusionPanel::setTargetCompatible(bool selected) {
  d->canReplaceSelected = selected;
  d->updateActions();
}
void DiffusionPanel::setComparisonAvailable(bool available) {
  d->canCompare = available;
  if (d->compareToggle) {
    d->compareToggle->setEnabled(available && selectedResult());
    if (!available)
      d->compareToggle->setChecked(false);
  }
}
bool DiffusionPanel::comparisonActive() const {
  return d->previewShowsResult && selectedResult() && d->lightbox && d->lightbox->isVisible() &&
         d->compareToggle && d->compareToggle->isChecked();
}
void DiffusionPanel::setComparisonReference(QImage image, QTransform toDocument,
                                            const QString &label) {
  d->comparisonReference = std::move(image);
  d->comparisonTransform = toDocument;
  if (d->previewShowsResult && d->lightboxPreview) {
    const auto *result = selectedResult();
    d->lightboxPreview->setComparison(d->comparisonReference, toDocument,
                                      result ? result->toDocument
                                             : QTransform{});
    if (d->compareToggle)
      d->compareToggle->setToolTip(
          "Left: " + label + " · Right: Result. Drag the divider to compare.");
  }
}
void DiffusionPanel::saveSettings() {
  QSettings s;
  s.setValue("diffusion/modelId", d->models->currentData().toString());
  s.setValue("diffusion/modelConfigurations",
             QJsonDocument(d->modelConfigs).toJson(QJsonDocument::Compact));
  s.setValue("diffusion/currentConfiguration",
             QJsonDocument(configuration()).toJson(QJsonDocument::Compact));
  s.setValue(
      "diffusion/generation",
      QJsonDocument(controls(d->sampling)).toJson(QJsonDocument::Compact));
  s.setValue("diffusion/backend", backend());
  s.remove("diffusion/workerExecutable");
  s.setValue("diffusion/prompt", d->prompt->toPlainText());
  s.setValue("diffusion/negativePrompt", d->negative->toPlainText());
  s.setValue("diffusion/seed", d->seed->text());
  s.setValue("diffusion/seedMode", d->seedMode->currentData());
  s.setValue("diffusion/sigmas", d->sigmas->toPlainText());
  s.setValue("diffusion/width", d->width->value());
  s.setValue("diffusion/height", d->height->value());
  s.setValue("diffusion/mode", int(mode()));
}
} // namespace imageeditor::ui
