#include "imageeditor/ui/DiffusionPanel.hpp"
#include "imageeditor/ui/Localization.hpp"
#include "imageeditor/ui/DiffusionLoras.hpp"
#include "imageeditor/ui/DiffusionSpatial.hpp"
#include "imageeditor/diffusion/ModelProfiles.h"
#include "imageeditor/diffusion/PromptPresets.h"
#include "imageeditor/diffusion/Resolution.h"
#include "imageeditor/ui/CollapsibleSection.hpp"
#include "imageeditor/ui/CompactValueControl.hpp"
#include "imageeditor/ui/OverlayDockWorkspace.hpp"
#include "imageeditor/ui/PopupOwnership.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/ui/ToolIcons.hpp"
#include "imageeditor/ui/WorkspaceDialog.hpp"
#include <QAbstractSpinBox>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDateTime>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLabel>
#include <QKeyEvent>
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
#include <QRegularExpression>
#include <QScreen>
#include <QScopedValueRollback>
#include <QScrollArea>
#include <QSettings>
#include <QSet>
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
#include <QToolTip>
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
  std::function<void(int)> crop;
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
    const bool canCrop = index.data(Qt::UserRole + 2).toBool();
    item.rect.adjust(0, 0, canCrop ? -60 : -30, 0);
    SubtleItemDelegate::paint(painter, item, index);
    if (!index.data(Qt::UserRole + 1).toBool())
      toolGlyph(ToolGlyph::Trash)
          .paint(painter, QRect(option.rect.right() - 24,
                                option.rect.center().y() - 8, 16, 16));
    if (canCrop) {
      const QRect button(option.rect.right() - 56, option.rect.center().y() - 12, 24, 24);
      painter->save();
      if (index.data(Qt::UserRole + 3).toBool()) {
        auto color = themeColor(ThemeColor::Accent); color.setAlphaF(.2);
        painter->setPen(Qt::NoPen); painter->setBrush(color);
        painter->drawRoundedRect(button, 3, 3);
      }
      toolGlyph(ToolGlyph::Crop).paint(painter, button.adjusted(4, 4, -4, -4));
      painter->restore();
    }
  }
  bool helpEvent(QHelpEvent *event, QAbstractItemView *view,
                 const QStyleOptionViewItem &option, const QModelIndex &index) override {
    if (index.data(Qt::UserRole + 2).toBool() && event->pos().x() >= option.rect.right() - 60 &&
        event->pos().x() < option.rect.right() - 30) {
      QToolTip::showText(event->globalPos(), index.data(Qt::UserRole + 3).toBool()
          ? i18n::text("diffusion.ui.restore.the.full.layer.input") : i18n::text("diffusion.ui.crop.this.layer.input.to.its.canvas"), view);
      return true;
    }
    return SubtleItemDelegate::helpEvent(event, view, option, index);
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
    if (index.data(Qt::UserRole + 2).toBool() &&
        mouse->position().x() >= option.rect.right() - 60) {
      if (crop) crop(index.row());
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
class PreviewDialog final : public QDialog {
public:
  using QDialog::QDialog;
  std::function<void(int)> navigate;
  void routeNavigationKeys() {
    for (auto *widget : findChildren<QWidget *>()) widget->installEventFilter(this);
  }
protected:
  bool eventFilter(QObject *watched, QEvent *event) override {
    if (qobject_cast<QLineEdit *>(watched)) return QDialog::eventFilter(watched, event);
    if (event->type() == QEvent::KeyPress && navigateKey(static_cast<QKeyEvent *>(event))) return true;
    return QDialog::eventFilter(watched, event);
  }
  void keyPressEvent(QKeyEvent *event) override {
    if (!navigateKey(event)) QDialog::keyPressEvent(event);
  }
private:
  bool navigateKey(QKeyEvent *event) {
    if (isVisible() && navigate && !QApplication::activePopupWidget() && event->modifiers() == Qt::NoModifier &&
        (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right)) {
      navigate(event->key() == Qt::Key_Left ? -1 : 1);
      event->accept();
      return true;
    }
    return false;
  }
};
QString promptText(QWidget *origin, const i18n::Message &title, const i18n::Message &label,
                   const QString &initial, bool *accepted,
                   bool multiline = false, bool panelLocal = false,
                   const QStringList &existingNames = {}) {
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
  if (workspace) {
    presenter = std::make_unique<WorkspaceDialog>(*workspace, *host);
    if (panelLocal) presenter->setCenterWidget(origin);
  }
  QInputDialog dialog(presenter ? presenter.get() : origin,
                      (presenter ? Qt::SubWindow : Qt::Dialog) |
                          Qt::FramelessWindowHint);
  dialog.setObjectName("DiffusionTextPrompt");
  i18n::setWindowTitle(&dialog, title);
  dialog.setWindowFlag(Qt::WindowMinMaxButtonsHint, false);
  dialog.setLabelText(title + "\n" + label);
  dialog.setTextValue(initial);
  if (panelLocal) {
    dialog.setObjectName("DiffusionConfigurationNamePrompt");
    dialog.setMinimumWidth(std::max(180, std::min(360, origin->width() - 24)));
    dialog.findChild<QLineEdit *>()->setMaxLength(200);
    dialog.setOkButtonText(i18n::text("diffusion.ui.create"));
    const auto validate = [&dialog, title, label, existingNames](const QString &value) {
      const auto name = value.trimmed();
      const bool duplicate = existingNames.contains(name, Qt::CaseInsensitive);
      dialog.setLabelText(title + "\n" + (duplicate ? i18n::text("diffusion.ui.that.name.is.already.in.use") : label));
      if (auto *buttons = dialog.findChild<QDialogButtonBox *>())
        buttons->button(QDialogButtonBox::Ok)->setEnabled(!name.isEmpty() && !duplicate);
    };
    QObject::connect(&dialog, &QInputDialog::textValueChanged, &dialog, validate);
    validate(initial);
  }
  if (multiline)
    dialog.setOption(QInputDialog::UsePlainTextEditForTextInput);
  if (!presenter)
    preparePopupOwnership(&dialog, origin);
  *accepted = (presenter ? presenter->exec(dialog) : dialog.exec()) ==
              QDialog::Accepted;
  return dialog.textValue();
}

bool confirmConfigurationDelete(QWidget *origin, const QString &name) {
  auto *host = popupTopLevelOwner(origin);
  OverlayDockWorkspace *workspace = nullptr;
  if (host) for (auto *child : host->findChildren<QWidget *>())
    if ((workspace = dynamic_cast<OverlayDockWorkspace *>(child))) break;
  std::unique_ptr<WorkspaceDialog> presenter;
  if (workspace) {
    presenter = std::make_unique<WorkspaceDialog>(*workspace, *host);
    presenter->setCenterWidget(origin);
  }
  QMessageBox dialog(QMessageBox::Question, i18n::text("diffusion.ui.delete.configuration"),
                    i18n::text("diffusion.ui.delete.1").arg(name), QMessageBox::Yes | QMessageBox::No,
                    presenter ? presenter.get() : origin);
  dialog.setObjectName("DiffusionConfigurationDeletePrompt");
  dialog.setTextFormat(Qt::PlainText);
  dialog.setWindowFlags((presenter ? Qt::SubWindow : Qt::Dialog) | Qt::FramelessWindowHint);
  dialog.setDefaultButton(QMessageBox::No);
  dialog.setEscapeButton(QMessageBox::No);
  if (!presenter) preparePopupOwnership(&dialog, origin);
  return (presenter ? presenter->exec(dialog) : dialog.exec()) == QMessageBox::Yes;
}

QString chooseFile(QWidget *origin, const QString &title,
                   const QString &initial, const QString &filter) {
  QFileDialog dialog(origin, title, initial, filter);
  dialog.setFileMode(QFileDialog::ExistingFile);
  preparePopupOwnership(&dialog, origin);
  return dialog.exec() == QDialog::Accepted ? dialog.selectedFiles().value(0)
                                            : QString{};
}
// Only presentation text is isolated: aliases in editable prompts stay byte-for-byte unchanged.
i18n::Message referenceHelp(i18n::Message message) {
  const auto resolve = [message] {
    QString result = message.resolved();
    const QRegularExpression aliases("<(?:Image[0-9]+(?:/[^<>]+)?|Output/[^<>]+|Region[0-9]+|Scene)>");
    auto matches = aliases.globalMatch(result);
    QVector<QRegularExpressionMatch> values;
    while (matches.hasNext()) values.append(matches.next());
    for (auto it = values.crbegin(); it != values.crend(); ++it)
      result.replace(it->capturedStart(), it->capturedLength(), i18n::ltrToken(it->captured()).resolved());
    return result;
  };
  return {resolve(), resolve};
}
QToolButton *iconButton(ToolGlyph icon, const i18n::Message &tip, QWidget *parent) {
  auto *b = new QToolButton(parent);
  b->setIcon(toolGlyph(icon));
  i18n::setToolTip(b, tip);
  i18n::setAccessibleName(b, tip);
  b->setIconSize({16, 16});
  b->setFixedSize(28, 28);
  b->setAutoRaise(true);
  return b;
}
QComboBox *combo(const QList<i18n::Message> &labels, const QStringList &values,
                 QWidget *parent) {
  auto *c = new ScrollSafe<QComboBox>(parent);
  c->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  c->setMinimumContentsLength(6);
  for (int i = 0; i < labels.size(); ++i)
    i18n::addItem(c, labels[i], values.value(i, labels[i].source()));
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
  if (auto *c = qobject_cast<QPlainTextEdit *>(w))
    return c->toPlainText();
  return {};
}
void setControl(QWidget *w, const QJsonValue &value) {
  const QSignalBlocker block(w);
  if (auto *c = qobject_cast<QComboBox *>(w)) {
    if (c->findData(value.toString()) < 0)
      i18n::addItem(c, i18n::text("diffusion.ui.1.unavailable").arg(i18n::ltrToken(value.toString())), value.toString());
    select(c, value.toString());
  } else if (auto *c = qobject_cast<QCheckBox *>(w))
    c->setChecked(value.toBool());
  else if (auto *c = qobject_cast<QSpinBox *>(w))
    c->setValue(value.toInt());
  else if (auto *c = qobject_cast<QDoubleSpinBox *>(w))
    c->setValue(value.toDouble());
  else if (auto *c = qobject_cast<QLineEdit *>(w))
    c->setText(value.toString());
  else if (auto *c = qobject_cast<QPlainTextEdit *>(w))
    c->setPlainText(value.toString());
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
QWidget *section(const i18n::Message &title, const QString &id, QVBoxLayout *&layout,
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

QString DiffusionPanel::Result::layerName() const {
  const auto configuration = metadata["configuration"].toObject();
  const auto recordedProfile = metadata["profile"];
  const auto profileId = recordedProfile.isString() ? recordedProfile.toString()
      : recordedProfile.toObject()["id"].toString(configuration["profile"].toString());
  const auto profile = diffusion::modelProfile(profileId);
  auto name = profile["resultName"].toString();
  if (name.isEmpty())
    name = QFileInfo(configuration["files"].toObject()["diffusion"].toString()).completeBaseName();
  if (name.isEmpty()) name = recordedProfile.toObject()["name"].toString(profile["name"].toString("AI"));
  name = name.simplified().replace(' ', '_');
  const auto info = metadata["image"].toObject()["generationInfo"].toObject();
  const auto seed = (info.contains("seed") ? info["seed"] : metadata["request"].toObject()["seed"])
                        .toVariant().toString();
  return seed.isEmpty() || seed == "-1" ? name : name + '_' + seed;
}

struct DiffusionPanel::Impl {
  DiffusionPanel &q;
  QComboBox *mode{}, *outputDestination{}, *backend{}, *models{},
      *configModels{}, *families{}, *profiles{}, *presets{}, *regionSource{};
  QFormLayout *modelForm{};
  QTabWidget *tabs{};
  QWidget *configurationPage{};
  QDoubleSpinBox *seed{};
  QString workerOverride;
  QComboBox *seedMode{};
  QComboBox *outputScene{};
  QWidget *outputSceneRow{};
  QString outputSceneId;
  QToolButton *randomSeed{};
  QVBoxLayout *loadedModelsLayout{};
  QJsonArray loadedModels;
  QVector<QPushButton *> unloadButtons;
  bool canUnloadModels{};
  QPlainTextEdit *prompt{}, *negative{}, *sigmas{};
  QSpinBox *width{}, *height{};
  QComboBox *maxInputSize{};
  QCheckBox *fitOutput{};
  QSize outputCanvasSize;
  QComboBox *aspectRatio{}, *megapixels{}, *resolutionMultiple{};
  QLabel *resolutionRequirement{};
  bool updatingResolution{};
  QCheckBox *imageToImage{};
  QCheckBox *cropGuidance{};
  QWidget *modeSettings{};
  bool editImageToImage{};
  bool referenceLimitShown{};
  bool hasExplicitStrength{}; // A saved or edited value takes precedence over mode defaults.
  QLabel *status{}, *regionHint{}, *resultImage{}, *generationImage{};
  QWidget *resultDetails{};
  QLabel *resultConfig{}, *resultModel{}, *resultSize{}, *resultCreated{};
  QLineEdit *resultSeed{};
  QToolButton *copyResultSeed{};
  QListWidget *inputList{}, *resultList{};
  QListWidgetItem *pendingItem{};
  QPushButton *generate{}, *apply{}, *discard{}, *regenerate{},
      *regionsButton{};
  QToolButton *addModel{}, *duplicateModel{}, *deleteModel{};
  QProgressBar *progress{};
  QToolButton *clearRegions{};
  QMap<QString, QWidget *> resource, sampling;
  QWidget *overridesSection{};
  QFormLayout *overridesForm{};
  QMap<QString, QWidget *> overrideControls, overrideRows;
  QMap<QString, QToolButton *> exposeButtons, overrideResetButtons;
  QMap<QString, QJsonObject> generationOverrides;
  bool syncingOverrides{};
  QMap<QString, QLineEdit *> files;
  QJsonArray modelConfigs, profileList;
  QString currentModelId;
  QJsonObject defaultResources, defaultSampling;
  QTimer *saveTimer{};
  bool initializing{true};
  void queueSave() { if (!initializing && !updating && saveTimer) saveTimer->start(); }
  QString overrideKey() const {
    if (!currentModelId.isEmpty()) return currentModelId;
    return "draft:" + QString::fromUtf8(QJsonDocument(QJsonObject{
        {"profile", profiles->currentData().toString()},
        {"files", q.configuration().value("files")}}).toJson(QJsonDocument::Compact));
  }
  QWidget *samplingDefault(const QString &key) const {
    return key == "sigmas" ? static_cast<QWidget *>(sigmas) : sampling.value(key);
  }
  QJsonArray exposedGeneration() const {
    QJsonArray exposed;
    for (auto it = exposeButtons.begin(); it != exposeButtons.end(); ++it)
      if (it.value()->isChecked()) exposed.append(it.key());
    return exposed;
  }
  void setExposedGeneration(const QJsonArray &exposed) {
    for (auto it = exposeButtons.begin(); it != exposeButtons.end(); ++it) {
      const QSignalBlocker block(it.value());
      it.value()->setChecked(exposed.contains(it.key()));
    }
  }
  QJsonObject effectiveSampling() const {
    auto values = controls(sampling);
    values["sigmas"] = sigmas->toPlainText();
    const auto overrides = generationOverrides.value(overrideKey());
    for (auto it = overrides.begin(); it != overrides.end(); ++it)
      if (exposeButtons.contains(it.key()) && exposeButtons[it.key()]->isChecked())
        values[it.key()] = it.value();
    return values;
  }
  void syncOverrides() {
    if (!overridesSection || overrideControls.isEmpty()) return;
    const QScopedValueRollback guard(syncingOverrides, true);
    const auto values = effectiveSampling();
    bool any = false;
    for (auto it = overrideControls.begin(); it != overrideControls.end(); ++it) {
      auto *source = samplingDefault(it.key());
      auto *target = it.value();
      if (auto *from = qobject_cast<QComboBox *>(source)) {
        auto *to = qobject_cast<QComboBox *>(target);
        const QSignalBlocker block(to);
        bool same = from->count() == to->count();
        for (int i = 0; same && i < from->count(); ++i)
          same = from->itemData(i) == to->itemData(i) && from->itemText(i) == to->itemText(i);
        if (!same) {
          to->clear();
          for (int i = 0; i < from->count(); ++i) to->addItem(from->itemText(i), from->itemData(i));
        }
      }
      if (controlValue(target) != values[it.key()]) setControl(target, values[it.key()]);
      target->setToolTip(source->toolTip());
      if (auto *from = qobject_cast<QDoubleSpinBox *>(source)) {
        const QSignalBlocker block(target);
        qobject_cast<QDoubleSpinBox *>(target)->setSpecialValueText(from->specialValueText());
      }
      const bool shown = exposeButtons[it.key()]->isChecked();
      overridesForm->setRowVisible(overrideRows[it.key()], shown);
      any |= shown;
      const auto baseline = controlValue(source);
      overrideResetButtons[it.key()]->setEnabled(values[it.key()] != baseline);
      i18n::setToolTip(overrideResetButtons[it.key()], i18n::text("diffusion.override.reset.hint").arg(baseline.toVariant().toString()));
    }
    overridesSection->setVisible(any);
    overridesSection->setEnabled(!busy);
  }
  void createOverrideControls(QFormLayout *defaults) {
    for (int row = 0; row < defaults->rowCount(); ++row) {
      auto *source = defaults->itemAt(row, QFormLayout::FieldRole)->widget();
      const auto key = source == sigmas ? QString("sigmas") : sampling.key(source);
      auto *sourceLabel = qobject_cast<QLabel *>(defaults->itemAt(row, QFormLayout::LabelRole)->widget());
      const auto resolveLabel = [sourceLabel] { return sourceLabel->text(); };
      const i18n::Message label(resolveLabel(), resolveLabel);
      auto *wrapper = new QWidget;
      auto *defaultRow = new QHBoxLayout(wrapper);
      defaultRow->setContentsMargins(0, 0, 0, 0);
      defaultRow->setSpacing(4);
      defaults->removeWidget(source);
      defaultRow->addWidget(source, 1);
      auto *expose = iconButton(ToolGlyph::Panels, i18n::text("diffusion.ui.show.1.in.generate").arg(label), wrapper);
      expose->setObjectName("DiffusionExpose_" + key);
      expose->setCheckable(true);
      expose->setFixedSize(24, 24);
      expose->setIconSize({14, 14});
      defaultRow->addWidget(expose, 0, Qt::AlignTop);
      defaults->setWidget(row, QFormLayout::FieldRole, wrapper);
      exposeButtons[key] = expose;

      auto *overrideRow = new QWidget;
      auto *layout = new QHBoxLayout(overrideRow);
      layout->setContentsMargins(0, 0, 0, 0);
      layout->setSpacing(4);
      QWidget *control = nullptr;
      if (auto *spin = qobject_cast<QDoubleSpinBox *>(source)) {
        auto *copy = new ScrollSafe<QDoubleSpinBox>;
        copy->setDecimals(spin->decimals());
        copy->setRange(spin->minimum(), spin->maximum());
        copy->setSingleStep(spin->singleStep());
        copy->setSpecialValueText(spin->specialValueText());
        copy->setKeyboardTracking(false);
        control = copy;
      } else if (qobject_cast<QComboBox *>(source)) {
        control = combo({}, {}, overrideRow);
      } else if (qobject_cast<QCheckBox *>(source)) {
        control = new QCheckBox;
      } else if (auto *text = qobject_cast<QPlainTextEdit *>(source)) {
        auto *copy = new QPlainTextEdit;
        copy->setMaximumHeight(text->maximumHeight());
        copy->setPlaceholderText(text->placeholderText());
        control = copy;
      }
      Q_ASSERT(control);
      control->setObjectName("DiffusionOverride_" + key);
      i18n::setAccessibleName(control, i18n::text("diffusion.ui.1.override").arg(label));
      layout->addWidget(control, 1);
      auto *reset = iconButton(ToolGlyph::Refresh, i18n::text("diffusion.ui.reset.1.to.configuration.default").arg(label), overrideRow);
      reset->setObjectName("DiffusionOverrideReset_" + key);
      reset->setFixedSize(24, 24);
      layout->addWidget(reset, 0, Qt::AlignTop);
      i18n::addRow(overridesForm, label, overrideRow);
      overrideControls[key] = control;
      overrideRows[key] = overrideRow;
      overrideResetButtons[key] = reset;
      const auto changed = [this, key, control] {
        if (syncingOverrides || updating || initializing) return;
        auto &values = generationOverrides[overrideKey()];
        const auto value = controlValue(control);
        if (value == controlValue(samplingDefault(key))) values.remove(key);
        else values[key] = value;
        updateCapabilities(false);
      };
      if (auto *spin = qobject_cast<QDoubleSpinBox *>(control)) QObject::connect(spin, &QDoubleSpinBox::valueChanged, &q, changed);
      else if (auto *combo = qobject_cast<QComboBox *>(control)) QObject::connect(combo, &QComboBox::currentIndexChanged, &q, changed);
      else if (auto *check = qobject_cast<QCheckBox *>(control)) QObject::connect(check, &QCheckBox::toggled, &q, changed);
      else if (auto *text = qobject_cast<QPlainTextEdit *>(control)) QObject::connect(text, &QPlainTextEdit::textChanged, &q, changed);
      QObject::connect(reset, &QToolButton::clicked, &q, [this, key] {
        generationOverrides[overrideKey()].remove(key);
        updateCapabilities(false);
      });
      QObject::connect(expose, &QToolButton::toggled, &q, [this, key](bool shown) {
        if (updating || initializing) return;
        if (!shown) generationOverrides[overrideKey()].remove(key);
        updateCapabilities(false);
        queueSave();
      });
    }
  }
  QJsonObject modelEntry() const {
    auto generation = controls(sampling);
    if (!hasExplicitStrength) generation.remove("strength");
    return {{"configuration", q.configuration()}, {"generation", generation},
            {"editImageToImage", editImageToImage}, {"customSigmas", sigmas->toPlainText()},
            {"backend", q.backend()}, {"exposedGeneration", exposedGeneration()}};
  }
  void loadModel(const QString &id) {
    q.saveSettings();
    const auto previousProfile = profiles->currentData();
    {
      const QScopedValueRollback guard(updating, true);
      currentModelId = id;
      rebuildModels();
      if (id.isEmpty()) {
        setExposedGeneration({});
        hasExplicitStrength = false;
        putControls(resource, defaultResources);
        putControls(sampling, defaultSampling);
        q.setConfiguration({{"profile", profiles->currentData().toString()}, {"files", QJsonObject{}}});
        updateCapabilities(true);
      } else for (const auto &value : modelConfigs) {
        const auto entry = value.toObject();
        if (entry["id"] != id) continue;
        setExposedGeneration(entry["exposedGeneration"].toArray());
        putControls(resource, defaultResources);
        putControls(sampling, defaultSampling);
        q.setConfiguration(entry["configuration"].toObject());
        updateCapabilities(true);
        putControls(sampling, entry["generation"].toObject());
        hasExplicitStrength = entry["generation"].toObject().contains("strength");
        sigmas->setPlainText(entry.value("customSigmas").toString());
        if (entry.contains("editImageToImage")) editImageToImage = entry["editImageToImage"].toBool();
        if (entry.contains("backend")) select(backend, entry["backend"].toString());
        break;
      }
      updateCapabilities(false);
      if (previousProfile != profiles->currentData() && resolutionMultiple->currentData().toInt() == 0)
        applyResolutionPreset();
    }
    if (q.onConfigurationChanged) q.onConfigurationChanged();
    q.saveSettings();
  }
  void createModel(bool duplicate) {
    if (busy) return;
    QStringList names;
    for (const auto &entry : modelConfigs) names << entry.toObject()["name"].toString();
    const QString base = i18n::text("diffusion.ui.1.copy").arg(q.configurationName());
    auto initial = duplicate ? base : QString{};
    for (int n = 2; names.contains(initial, Qt::CaseInsensitive); ++n) initial = base + QString(" %1").arg(n);
    bool accepted = false;
    const auto name = promptText(&q, duplicate ? i18n::text("diffusion.ui.duplicate.configuration") : i18n::text("diffusion.ui.new.configuration"), i18n::text("diffusion.ui.name"),
                                 initial, &accepted, false, true, names).trimmed();
    if (!accepted || name.isEmpty() || names.contains(name, Qt::CaseInsensitive)) return;
    q.saveSettings();
    {
      const QScopedValueRollback guard(updating, true);
      currentModelId = QUuid::createUuid().toString(QUuid::WithoutBraces);
      if (!duplicate) {
        setExposedGeneration({});
        hasExplicitStrength = false;
        editImageToImage = false;
        putControls(resource, defaultResources);
        putControls(sampling, defaultSampling);
        select(backend, "auto");
        q.setConfiguration({{"profile", profiles->currentData().toString()}, {"files", QJsonObject{}}});
        updateCapabilities(true);
      }
      auto entry = modelEntry();
      entry["id"] = currentModelId;
      entry["name"] = name;
      modelConfigs.append(entry);
      loras->setConfiguration(currentModelId, loras->defaults());
      rebuildModels();
      updateCapabilities(false);
    }
    q.saveSettings();
    if (q.onConfigurationChanged) q.onConfigurationChanged();
  }
  std::unique_ptr<DiffusionLoras> loras;
  QVector<Input> inputs;
  QVector<Result> results;
  QImage draftPreview;
  QVector<DiffusionRegion> regions;
  QString regionEditorInput;
  QPointer<QDialog> lightbox;
  QPointer<DiffusionPreview> lightboxPreview;
  QPointer<QLabel> lightboxTitle, previewPosition;
  QPointer<QToolButton> previousPreview, nextPreview;
  bool previewShowsResult{};
  QPointer<QPushButton> compareToggle;
  QPointer<QComboBox> compareAlignment;
  QImage comparisonReference;
  QTransform comparisonTransform;
  QVector<ComparisonLayer> comparisonLayers;
  QString comparisonLayerId;
  QPointer<QListWidget> comparisonList;
  QPointer<QLineEdit> comparisonSearch;
  QPointer<QLabel> comparisonEmpty;
  void filterComparisonLayers() {
    if (!comparisonList || !comparisonSearch) return;
    int visible = 0;
    for (int row = 0; row < comparisonList->count(); ++row) {
      auto *item = comparisonList->item(row);
      const bool matches = item->text().contains(comparisonSearch->text(), Qt::CaseInsensitive);
      item->setHidden(!matches);
      visible += matches;
    }
    if (comparisonEmpty) comparisonEmpty->setVisible(!visible);
  }
  void populateComparisonLayers() {
    if (!comparisonList) return;
    const QSignalBlocker guard(comparisonList);
    comparisonList->clear();
    for (const auto &layer : comparisonLayers) {
      auto *item = new QListWidgetItem(layer.thumbnail, layer.name + "\n" + layer.type, comparisonList);
      item->setData(Qt::UserRole, layer.id);
      item->setToolTip(layer.name);
      item->setSizeHint(QSize(160, 48));
      if (layer.id == comparisonLayerId) comparisonList->setCurrentItem(item);
    }
    filterComparisonLayers();
  }
  bool busy{}, canReplaceSelected{}, updating{};
  diffusion::PromptPresetStore presetStore{
      diffusion::PromptPresetStore::defaultFilePath()};
  QVector<diffusion::PromptPreset> presetList;
  explicit Impl(DiffusionPanel &panel) : q(panel) {}
  static void regionIds(QVector<DiffusionRegion> &regions) {
    QSet<QString> ids;
    for (auto &region : regions) {
      if (region.id.isEmpty() || ids.contains(region.id))
        region.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
      ids.insert(region.id);
    }
  }
  void prepareInput(Input &input) {
    regionIds(input.regions);
    if (input.fullImage.isNull()) {
      input.fullImage = input.image;
      input.fullToDocument = input.toDocument;
      input.fullRegions = input.regions;
    }
    if (input.cropToCanvas && !input.sourceCanvas.isEmpty()) {
      const auto cropped = diffusionCropToCanvas(input.fullImage, input.fullToDocument, input.sourceCanvas);
      input.image = cropped.image; input.toDocument = cropped.toDocument;
    } else {
      input.cropToCanvas = false;
      input.image = input.fullImage; input.toDocument = input.fullToDocument;
    }
    input.outputSize = input.image.size();
    const int limit = maxInputSize->currentData().toInt();
    if (!input.image.isNull() && std::max(input.image.width(), input.image.height()) > limit) {
      const auto oldSize = input.image.size();
      input.image = input.image.scaled(QSize(limit, limit), Qt::KeepAspectRatio, Qt::SmoothTransformation);
      if (input.image.isNull()) throw std::bad_alloc();
      input.toDocument = QTransform::fromScale(double(oldSize.width()) / input.image.width(),
                                               double(oldSize.height()) / input.image.height()) * input.toDocument;
    }
    input.regions = diffusionMapInputRegions(input.fullRegions, input.fullImage.size(),
        input.fullToDocument, input.image.size(), input.toDocument);
    input.visibleRegionIds.clear();
    for (const auto &region : input.regions) input.visibleRegionIds.append(region.id);
  }
  void rememberInputRegions(Input &input) {
    regionIds(input.regions);
    if (!input.cropToCanvas) {
      input.fullRegions = input.regions;
      input.visibleRegionIds.clear();
      for (const auto &r : input.regions) input.visibleRegionIds.append(r.id);
      return;
    }
    QVector<DiffusionRegion> stored;
    QSet<QString> handled;
    for (const auto &old : input.fullRegions) {
      if (!input.visibleRegionIds.contains(old.id)) { stored.append(old); continue; }
      const auto it = std::find_if(input.regions.begin(), input.regions.end(),
          [&](const auto &r) { return r.id == old.id; });
      if (it == input.regions.end()) continue;
      handled.insert(it->id);
      const auto oldView = diffusionMapInputRegions({old}, input.fullImage.size(), input.fullToDocument,
                                                    input.image.size(), input.toDocument);
      if (!oldView.isEmpty() && oldView.front().bounds == it->bounds) {
        auto retained = *it; retained.bounds = old.bounds; stored.append(retained);
      } else {
        stored += diffusionMapInputRegions({*it}, input.image.size(), input.toDocument,
                                           input.fullImage.size(), input.fullToDocument);
      }
    }
    for (const auto &r : input.regions)
      if (!handled.contains(r.id))
        stored += diffusionMapInputRegions({r}, input.image.size(), input.toDocument,
                                           input.fullImage.size(), input.fullToDocument);
    input.fullRegions = stored;
    input.visibleRegionIds.clear();
    for (const auto &r : input.regions) input.visibleRegionIds.append(r.id);
  }
  QJsonObject profile() const {
    const auto id = profiles->currentData().toString();
    for (const auto &p : profileList)
      if (p.toObject()["id"].toString() == id)
        return p.toObject();
    return {};
  }
  static QString familyKey(const QJsonObject &profile) {
    return profile.value("familyGroup").toString(profile.value("family").toString());
  }
  void rebuildVariants(const QString &preferred = {}) {
    const QSignalBlocker block(profiles);
    profiles->clear();
    for (const auto &value : profileList) {
      const auto p = value.toObject();
      if (familyKey(p) == families->currentData().toString())
        profiles->addItem(p.value("variant").toString("Standard"), p.value("id").toString());
    }
    select(profiles, preferred);
    modelForm->setRowVisible(profiles, profiles->count() > 1);
  }
  void selectProfile(const QString &id) {
    const QSignalBlocker block(families);
    for (const auto &value : profileList) {
      const auto p = value.toObject();
      if (p.value("id") == id) {
        select(families, familyKey(p));
        rebuildVariants(id);
        return;
      }
    }
    rebuildVariants();
  }
  void writeResolution(QSize size) {
    QScopedValueRollback<bool> guard(updatingResolution, true);
    width->setValue(size.width());
    height->setValue(size.height());
  }
  void setAlignedSize(QSize size) {
    const int alignment = profile().value("alignment").toInt(32);
    writeResolution({std::max(alignment, ((size.width() + alignment - 1) / alignment) * alignment),
                     std::max(alignment, ((size.height() + alignment - 1) / alignment) * alignment)});
  }
  void detectAspectRatio() {
    const double current = double(width->value()) / height->value();
    int best = aspectRatio->count() - 1; // Custom
    double bestError = .01; // Allow one percent for dimensions rounded to a multiple.
    for (int index = 0; index < aspectRatio->count() - 1; ++index) {
      const auto ratio = aspectRatio->itemData(index).toSize();
      const double error = std::abs(current / (double(ratio.width()) / ratio.height()) - 1);
      if (error <= bestError) {
        best = index;
        bestError = error;
      }
    }
    const QSignalBlocker block(aspectRatio);
    aspectRatio->setCurrentIndex(best);
  }
  void applyResolutionPreset() {
    const auto ratio = aspectRatio->currentData().toSize();
    if (ratio.isEmpty()) return;
    const double pixels = megapixels->currentData().toDouble() * 1024 * 1024;
    const double scale = std::sqrt(pixels / (ratio.width() * ratio.height()));
    const int selected = resolutionMultiple->currentData().toInt();
    const int multiple = selected ? selected : profile().value("alignment").toInt(32);
    const auto rounded = [multiple](double dimension) {
      const double units = dimension / multiple;
      const double lower = std::floor(units);
      // Match the reference's nearest-multiple rounding, including ties to even.
      const double nearest = units - lower == .5 ? lower + std::fmod(lower, 2.0) : std::round(units);
      return int(nearest) * multiple;
    };
    writeResolution({rounded(ratio.width() * scale), rounded(ratio.height() * scale)});
  }
  int maxInputs() const {
    if (!q.supportsImageInputs())
      return 0;
    if (!q.supportsRegions())
      return profile().value("maxReferenceImages").toInt(8);
    const bool annotation =
        profile().value("spatialGuidance").toString() == "annotation" &&
        !regions.isEmpty();
    int crops = 0;
    if (profile().value("family") != "flux3" || (cropGuidance && cropGuidance->isChecked()))
      for (const auto &input : inputs)
        crops += int(input.regions.size());
    return profile().value("maxReferenceImages").toInt(8) -
           (annotation ? 1 : 0) - crops;
  }
  bool referencesFit() const { return !q.supportsImageInputs() || inputs.size() <= maxInputs(); }
  void reportReferenceLimit() {
    if (!referencesFit()) {
      referenceLimitShown = true;
      q.setStatus(i18n::text("diffusion.ui.images.and.region.guides.exceed.this.model")
                      .arg(profile().value("maxReferenceImages").toInt(8)), true);
    } else if (referenceLimitShown) {
      referenceLimitShown = false;
      q.setStatus(i18n::text("diffusion.ui.reference.limit.satisfied"));
    }
  }
  int inputIndex(const QString &id) const {
    for (int i = 0; i < inputs.size(); ++i)
      if (inputs[i].id == id)
        return i;
    return -1;
  }
  bool hasTransfers() const {
    for (const auto& input : inputs)
      for (const auto& region : input.fullRegions)
        if (!region.destinationId.isEmpty()) return true;
    return false;
  }
  QString transferError() const {
    if (!q.supportsRegions()) return {};
    const bool flux = profile().value("family") == "flux3";
    if (hasTransfers() && !flux) return i18n::text("diffusion.ui.linked.region.transfers.are.available.with.flux");
    if (hasTransfers() && inputs.size() < 2) return i18n::text("diffusion.ui.linked.transfers.need.at.least.two.input");
    if ((hasTransfers() || (flux && prompt->toPlainText().contains("<Scene>"))) && inputIndex(outputSceneId) < 0)
      return i18n::text("diffusion.ui.choose.an.available.output.scene.before.generating");
    for (const auto& input : inputs) for (const auto& region : input.fullRegions) {
      if (region.destinationId.isEmpty()) continue;
      if (std::none_of(input.regions.begin(), input.regions.end(), [&](const auto& r) { return r.id == region.id; }))
        return i18n::text("diffusion.ui.source.region.1.is.outside.the.cropped").arg(region.label);
      if (std::none_of(regions.begin(), regions.end(), [&](const auto& r) { return r.id == region.destinationId && r.targetOnly; }))
        return i18n::text("diffusion.ui.the.destination.for.1.is.missing.choose").arg(region.label);
    }
    return {};
  }
  QImage outputLayoutImage() const {
    const int scene = inputIndex(outputSceneId);
    return diffusionOutputLayout(scene < 0 ? QImage{} : inputs[scene].image, {width->value(), height->value()});
  }
  void rebuildScenes() {
    if (!outputScene) return;
    if (outputSceneId.isEmpty() && !inputs.isEmpty()) outputSceneId = inputs.front().id;
    if (inputIndex(outputSceneId) < 0 && inputs.size() == 1 && !hasTransfers()) outputSceneId = inputs.front().id;
    const QSignalBlocker block(outputScene);
    outputScene->clear();
    for (int i = 0; i < inputs.size(); ++i)
      outputScene->addItem(QString("<Image%1> · %2").arg(i + 1).arg(inputs[i].label), inputs[i].id);
    if (inputIndex(outputSceneId) < 0) i18n::addItem(outputScene, i18n::text("diffusion.ui.choose.output.scene"), outputSceneId);
    select(outputScene, outputSceneId);
    if (profile().value("family") == "flux3" && lightbox && lightboxPreview && regionEditorInput.isEmpty() && !previewShowsResult) {
      lightboxPreview->setImage(outputLayoutImage(), true);
      i18n::setText(lightboxTitle, i18n::text("diffusion.ui.output.layout.1.2.px").arg(width->value()).arg(height->value()));
    }
  }
  void rebuildRegionSources() {
    rebuildScenes();
    if (!regionSource)
      return;
    const auto selected = regionSource->currentData().toString();
    const QSignalBlocker block(regionSource);
    regionSource->clear();
    i18n::addItem(regionSource, i18n::text("diffusion.ui.output.layout"), QString{});
    for (int i = 0; i < inputs.size(); ++i)
      regionSource->addItem(QString("Image%1 · %2").arg(i + 1).arg(inputs[i].label),
                            inputs[i].id);
    select(regionSource, selected);
    if (regionSource->currentIndex() < 0)
      regionSource->setCurrentIndex(0);
  }
  void updateOutputControls() {
    const bool ready = q.selectedResult() != nullptr;
    outputDestination->setVisible(lightbox && previewShowsResult && ready);
    fitOutput->setVisible(lightbox && previewShowsResult && ready);
    if (const auto *result = q.selectedResult()) {
      const bool fill = result->mode == Mode::Fill;
      const bool replace = q.destination() == Destination::ReplaceSelected;
      const bool newLayer = q.destination() == Destination::NewLayer;
      const bool edit = result->mode == Mode::Edit;
      const QSignalBlocker guard(fitOutput);
      i18n::setText(fitOutput, fill ? i18n::text("diffusion.ui.fit.to.canvas") : replace ? i18n::text("diffusion.ui.fit.to.selected.layer")
                                            : edit ? i18n::text("diffusion.ui.fit.to.original.bounds") : i18n::text("diffusion.ui.fit.to.canvas"));
      fitOutput->setChecked(fill || replace || (newLayer && result->fitToBounds.value_or(edit)));
      fitOutput->setEnabled(!busy && newLayer && !fill && (edit || outputCanvasSize.isValid()));
      i18n::setToolTip(fitOutput, fill ? i18n::text("diffusion.ui.generative.fill.stays.aligned.to.its.captured")
          : replace ? i18n::text("diffusion.ui.replacement.fits.the.selected.layer.use.add")
          : !newLayer ? i18n::text("diffusion.ui.new.documents.use.the.generated.pixel.dimensions")
          : edit ? i18n::text("diffusion.ui.fit.the.result.to.the.original.input")
                 : i18n::text("diffusion.ui.scale.the.result.to.the.current.canvas"));
    }
    if (previewShowsResult && lightboxPreview && !comparisonReference.isNull())
      lightboxPreview->setComparison(comparisonReference, comparisonTransform, q.resultPlacement());
  }
  void updateActions() {
    if (inputs.size() == 1 && inputIndex(outputSceneId) < 0 && !hasTransfers())
      rebuildScenes();
    const bool ready = q.selectedResult() != nullptr;
    const bool configured = !files["diffusion"]->text().trimmed().isEmpty() &&
                            (profile().value("checkpoint").toBool() ||
                             (!files["textEncoder"]->text().trimmed().isEmpty() &&
                              !files["vae"]->text().trimmed().isEmpty()));
    addModel->setEnabled(!busy);
    configModels->setEnabled(!busy && !modelConfigs.isEmpty());
    duplicateModel->setEnabled(!busy && (configured || !currentModelId.isEmpty()));
    deleteModel->setEnabled(!busy && !currentModelId.isEmpty());
    models->setItemText(0, configured ? i18n::text("diffusion.ui.current.configuration")
                                      : i18n::text("diffusion.ui.missing.configuration"));
    const bool hasInputs = q.mode() == Mode::Generate || !inputs.isEmpty();
    const bool nonempty = !q.supportsImageInputs() || std::none_of(inputs.begin(), inputs.end(), [](const auto &in) { return in.image.isNull(); });
    const auto linkError = transferError();
    i18n::setText(generate, busy ? i18n::text("diffusion.ui.cancel") : i18n::text("diffusion.ui.generate"));
    generate->setEnabled(busy || (configured && hasInputs && nonempty && referencesFit() && linkError.isEmpty()));
    for (auto *button : unloadButtons)
      button->setEnabled(!busy && canUnloadModels);
    i18n::setToolTip(generate, busy ? i18n::text("diffusion.ui.cancel.generation") : !linkError.isEmpty() ? linkError : !nonempty ? i18n::text("diffusion.ui.a.cropped.layer.is.outside.its.canvas")
        : !referencesFit()
        ? i18n::text("diffusion.ui.remove.an.image.or.region.to.stay")
        : configured ? QString{} : i18n::text("diffusion.ui.add.model.files.in.the.configuration.tab"));
    regenerate->setEnabled(!busy && ready && configured && hasInputs && nonempty && referencesFit() && linkError.isEmpty());
    apply->setEnabled(!busy && ready);
    discard->setEnabled(ready);
    resultImage->setVisible(true);
    tabs->setTabText(2, results.isEmpty()
                            ? i18n::text("diffusion.ui.preview")
                            : i18n::text("diffusion.ui.preview.1").arg(results.size()));
    updateOutputControls();
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
      compareToggle->setEnabled(!comparisonLayers.isEmpty() && ready);
    if (compareAlignment)
      compareAlignment->setEnabled(ready && compareToggle &&
                                   compareToggle->isChecked() && !comparisonReference.isNull());
    if (previewShowsResult && lightboxPreview && compareToggle)
      lightboxPreview->setComparisonEnabled(ready &&
                                            compareToggle->isChecked() && !comparisonReference.isNull());
    if (comparisonList) comparisonList->setEnabled(ready);
    apply->setVisible(lightbox && previewShowsResult && ready);
    regenerate->setVisible(ready);
    discard->setVisible(ready);
    inputList->setVisible(true);
    configurationPage->setEnabled(!busy);
    overridesSection->setEnabled(!busy);
    loras->setBusy(busy);
    models->setEnabled(!busy && !modelConfigs.isEmpty());
    seedMode->setEnabled(!busy);
    randomSeed->setEnabled(!busy);
    seed->setEnabled(!busy && seedMode->currentData() != "random");
    profiles->setEnabled(!busy);
    families->setEnabled(!busy);
    mode->setEnabled(!busy);
    updateModeControls();
    inputList->setEnabled(!busy && q.supportsImageInputs());
    i18n::setToolTip(inputList, q.supportsImageInputs() ? QString{} : i18n::text("diffusion.ui.this.model.generates.from.text.only.stored"));
    maxInputSize->setEnabled(!busy && q.supportsImageInputs());
    if (auto *header = q.findChild<QToolButton *>("DiffusionInputsSectionHeader"))
      header->setToolTip(inputList->toolTip());
    if (auto *add = inputList->item(int(inputs.size())))
      add->setFlags(!busy && inputs.size() < maxInputs() ? Qt::ItemIsEnabled
                                                         : Qt::NoItemFlags);
    const auto regionInput = inputIndex(regionSource->currentData().toString());
    const int regionCount = regionInput < 0 ? int(regions.size())
                                          : int(inputs[regionInput].regions.size());
    clearRegions->setEnabled(!busy && q.supportsRegions() && regionCount > 0);
    regionSource->setEnabled(!busy && q.supportsRegions());
    cropGuidance->setVisible(profile().value("family") == "flux3");
    cropGuidance->setEnabled(!busy);
    if (outputSceneRow) outputSceneRow->setVisible(profile().value("family") == "flux3" && (inputs.size() > 1 || hasTransfers()));
    if (outputScene) outputScene->setEnabled(!busy);
    if (auto *header =
            q.findChild<QToolButton *>("DiffusionRegionsSectionHeader"))
      i18n::setText(header, regionCount == 0
                          ? i18n::text("diffusion.ui.regions")
                          : i18n::text("diffusion.ui.regions.1").arg(regionCount));
    if (regionInput >= 0)
      i18n::setText(regionHint, referenceHelp(i18n::text("diffusion.ui.use.image.1.for.this.image.and").arg(regionInput + 1)
          + (inputs.size() == 1 ? i18n::text("diffusion.ui.with.one.input.region1.also.works") : QString{})));
    else
      i18n::setText(regionHint, profile().value("spatialGuidance").toString() == "structured-caption"
          ? referenceHelp(i18n::text("diffusion.ui.refer.to.output.regions.as.output.region1"))
          : i18n::text("diffusion.ui.output.placement.guide.use.generative.fill.to"));
    regionsButton->setEnabled(!busy && q.supportsRegions());
    i18n::setToolTip(regionsButton, q.supportsRegions() ? QString{} : i18n::text("diffusion.ui.region.guidance.is.unavailable.for.this.model"));
    if (!q.supportsRegions())
      i18n::setText(regionHint, i18n::text("diffusion.ui.region.guidance.is.unavailable.for.this.model"));

    if (auto *model =
            qobject_cast<QStandardItemModel *>(outputDestination->model()))
      model->item(int(Destination::ReplaceSelected))
          ->setEnabled(canReplaceSelected);
    if (!canReplaceSelected && q.destination() == Destination::ReplaceSelected)
      q.setDestination(Destination::NewLayer);
    if (referenceLimitShown || !referencesFit())
      reportReferenceLimit();
  }
  bool pendingSelected() const {
    return pendingItem && resultList->currentItem() == pendingItem;
  }
  void syncPending() {
    const QSignalBlocker block(resultList);
    if (busy && !pendingItem) {
      const int previous = resultList->currentRow();
      pendingItem = new QListWidgetItem(toolGlyph(ToolGlyph::Effects),
                                        i18n::text("diffusion.ui.generating"), resultList);
      pendingItem->setToolTip(
          i18n::text("diffusion.ui.generation.in.progress.a.live.draft.appears"));
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
  void refreshGenerationPreview() {
    const QImage image = busy ? draftPreview : results.isEmpty() ? QImage{} : results.back().image;
    generationImage->setVisible(busy || !image.isNull());
    if (image.isNull()) {
      generationImage->clear();
      i18n::setText(generationImage, i18n::text("diffusion.ui.generating"));
    } else {
      generationImage->setPixmap(QPixmap::fromImage(
          thumbnail(image, {std::max(80, generationImage->width()), 200})));
    }
  }
  void refreshResultDetails() {
    const auto *result = q.selectedResult();
    resultDetails->setVisible(result != nullptr);
    if (!result) return;
    const auto &metadata = result->metadata;
    const auto image = metadata["image"].toObject();
    const auto info = image["generationInfo"].toObject();
    const auto config = metadata["configuration"].toObject();
    const auto request = metadata["request"].toObject();
    i18n::setText(resultConfig, metadata.contains("configurationName") ? i18n::Message(metadata["configurationName"].toString()) : i18n::text("diffusion.ui.unsaved.configuration"));
    const auto model = QFileInfo(config["files"].toObject()["diffusion"].toString()).fileName();
    resultModel->setText(model.isEmpty() ? metadata["profile"].toObject()["name"].toString("—") : model);
    resultSize->setText(QString("%1 × %2 px").arg(image["width"].toInt(result->image.width()))
                                               .arg(image["height"].toInt(result->image.height())));
    // The worker reports each batch image's actual seed, which can differ from
    // the submitted seed. Never read current controls for historical results.
    const auto seedValue = info.contains("seed") ? info["seed"] : request["seed"];
    const auto seedText = seedValue.toVariant().toString();
    resultSeed->setText(seedText);
    copyResultSeed->setEnabled(!seedText.isEmpty() && seedText != "-1");
    const auto date = QDateTime::fromString(metadata["createdAt"].toString(), Qt::ISODateWithMs);
    resultCreated->setText(date.isValid() ? date.toLocalTime().toString("d/M/yyyy HH:mm") : QString("—"));
    resultCreated->setToolTip(date.toLocalTime().toString(Qt::ISODate));
  }
  void refreshDisplayedPreview() {
    const auto *result = q.selectedResult();
    const QImage image = result              ? result->image
                         : pendingSelected() ? draftPreview
                                             : QImage{};
    const i18n::Message title =
        result              ? i18n::text("diffusion.ui.ai.image.preview.1").arg(result->label)
        : pendingSelected() ? (image.isNull() ? i18n::text("diffusion.ui.generation.in.progress")
                                              : i18n::text("diffusion.ui.pending.preview.approximate"))
                            : i18n::text("diffusion.ui.ai.image.preview");
    if (image.isNull()) {
      resultImage->clear();
      i18n::setText(resultImage,
          pendingSelected() ? i18n::text("diffusion.ui.generating") : i18n::text("diffusion.ui.generated.previews.appear.here"));
    } else
      resultImage->setPixmap(QPixmap::fromImage(
          thumbnail(image, {std::max(80, resultImage->width()), 180})));
    if (previewShowsResult && lightboxPreview) {
      lightboxPreview->setEmptyText(pendingSelected() ? i18n::text("diffusion.ui.generating")
                                                      : i18n::text("diffusion.ui.no.image"));
      lightboxPreview->setImage(image, true);
      if (lightboxTitle)
        lightboxTitle->setText(title + (image.isNull()
                                            ? QString{}
                                            : QString(" · %1 × %2 px")
                                                  .arg(image.width())
                                                  .arg(image.height())));
    }
    refreshGenerationPreview();
    refreshResultDetails();
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
  void updateModeControls() {
    const auto mode = q.mode();
    if (!hasExplicitStrength)
      setControl(sampling["strength"], mode == Mode::Fill ? 1.0 : .75);
    modeSettings->setEnabled(!busy);
    {
      const QSignalBlocker block(imageToImage);
      imageToImage->setChecked(mode == Mode::Fill ||
                              (mode == Mode::Edit && editImageToImage));
    }
    imageToImage->setEnabled(mode == Mode::Edit && q.supportsMode(Mode::Edit));
    i18n::setToolTip(imageToImage,
        !q.supportsMode(Mode::Edit) && !q.supportsMode(Mode::Fill)
            ? i18n::text("diffusion.ui.this.model.generates.from.text.only.image")
        : mode == Mode::Fill
            ? i18n::text("diffusion.ui.always.used.for.generative.fill.the.canvas")
        : mode == Mode::Edit
            ? i18n::text("diffusion.ui.off.edit.using.image.references.on.start")
            : i18n::text("diffusion.ui.starts.denoising.from.an.input.image.available"));
    sampling["strength"]->setEnabled(q.imageToImageEnabled());
    i18n::setToolTip(sampling["strength"],
        !q.supportsMode(Mode::Edit) && !q.supportsMode(Mode::Fill)
            ? i18n::text("diffusion.ui.denoising.strength.is.available.for.models.that")
        : q.imageToImageEnabled()
            ? i18n::text("diffusion.ui.lower.values.retain.more.of.the.starting")
        : mode == Mode::Edit
            ? i18n::text("diffusion.ui.enable.image.to.image.denoising.to.use")
            : i18n::text("diffusion.ui.available.in.generative.fill.or.edit.layer"));
  }
  void updateSamplingAvailability(const QMap<QString, QWidget *> &widgets,
                                  QWidget *customSigmas, const QJsonObject &values) {
    const auto p = profile();
    const auto schedule = values["schedule"].toString();
    i18n::setToolTip(widgets["steps"], schedule == "turbo"
        ? i18n::text("diffusion.ui.the.fixed.turbo.sigma.schedule.uses.8")
        : i18n::text("diffusion.ui.number.of.denoising.steps"));
    customSigmas->setEnabled(schedule == "custom");
    i18n::setToolTip(customSigmas, i18n::text("diffusion.ui.enter.steps.1.descending.values.ending.in"));
    widgets["scheduler"]->setEnabled(schedule == "auto");
    i18n::setToolTip(widgets["scheduler"],
        i18n::text("diffusion.ui.controls.how.noise.levels.are.spaced.available"));
    widgets["flowShift"]->setEnabled(schedule == "auto" && p.value("flowSchedule").toBool(true));
    i18n::setToolTip(widgets["flowShift"],
        !p.value("flowSchedule").toBool(true) ? i18n::text("diffusion.ui.flow.shift.does.not.apply.to.this") :
        i18n::text("diffusion.ui.adjusts.the.flow.schedule.available.with.the")
            .arg(p.value("flowShift").toDouble()));
    auto *scheduleCombo = qobject_cast<QComboBox *>(widgets["schedule"]);
    if (auto *items =
            qobject_cast<QStandardItemModel *>(scheduleCombo->model()))
      items->item(scheduleCombo->findData("turbo"))
          ->setEnabled(p.value("id") == "qwen-image-2.1-turbo");
    const auto sampler = values["sampler"].toString();
    const bool stochastic =
        sampler.contains("_a") || sampler.contains("sde") ||
        QStringList{"ddim_trailing", "tcd", "res_multistep", "res_2s"}.contains(
            sampler);
    widgets["eta"]->setEnabled(stochastic);
    i18n::setToolTip(widgets["eta"],
        i18n::text("diffusion.ui.controls.added.sampler.noise.available.for.samplers"));
    const bool caching = values["cacheMode"].toString() != "disabled";
    for (const auto &key : {QString("cacheReuseThreshold"),
                            QString("cacheStart"), QString("cacheEnd")}) {
      widgets[key]->setEnabled(caching);
      i18n::setToolTip(widgets[key], i18n::text("diffusion.ui.available.when.approximate.cache.is.enabled"));
    }
    for (const auto &key : {QString("vaeTileSize"), QString("vaeTileOverlap")}) {
      widgets[key]->setEnabled(values["vaeTiling"].toBool());
      i18n::setToolTip(widgets[key], i18n::text("diffusion.vae.tiling.required"));
    }
    widgets["previewInterval"]->setEnabled(values["preview"].toBool());
    i18n::setToolTip(widgets["previewInterval"], i18n::text("diffusion.progress.preview.required"));
  }
  void updateCapabilities(bool defaults) {
    const auto p = profile();
    const bool checkpoint = p.value("checkpoint").toBool();
    for (const auto &key : {QString("textEncoder"), QString("vision")})
      modelForm->setRowVisible(files[key]->parentWidget(), !checkpoint);
    for (const auto &key : {QString("diffusion"), QString("vae")}) {
      const i18n::Message label = key == "diffusion" ? (checkpoint ? i18n::text("diffusion.ui.checkpoint") : i18n::text("diffusion.ui.diffusion.weights"))
                                               : (checkpoint ? i18n::text("diffusion.ui.vae.override.optional") : "VAE");
      i18n::setText(modelForm->labelForField(files[key]->parentWidget()), label);
      i18n::setPlaceholderText(files[key], checkpoint && key == "vae" ? i18n::text("diffusion.ui.use.checkpoint.vae") : label);
    }
    if (auto *items = qobject_cast<QStandardItemModel *>(mode->model())) {
      for (const auto value : {Mode::Generate, Mode::Edit, Mode::Fill}) {
        const bool supported = q.supportsMode(value);
        items->item(int(value))->setEnabled(supported);
        items->item(int(value))->setToolTip(supported ? QString{} : i18n::text("diffusion.ui.this.mode.is.unavailable.for.the.selected"));
      }
    }
    if (!q.supportsMode(q.mode())) {
      const QSignalBlocker block(mode);
      mode->setCurrentIndex(int(Mode::Generate));
    }
    const int alignment = p.value("alignment").toInt(32);
    width->setSingleStep(alignment);
    height->setSingleStep(alignment);
    // Discovery, configuration and mode changes must not rewrite a size already
    // captured for a job (or one the user is still choosing).
    i18n::setText(resolutionRequirement, i18n::text("diffusion.ui.1.width.and.height.must.be.multiples")
                                      .arg(p.value("name").toString()).arg(alignment));
    resolutionMultiple->setItemText(0, i18n::text("diffusion.ui.auto.1").arg(alignment));
    i18n::setToolTip(resolutionMultiple, i18n::text("diffusion.ui.auto.uses.the.selected.model.s.required"));
    const bool qwen = p.value("family").toString() == "qwen-image-2.1";
    const bool manual =
        qobject_cast<QComboBox *>(resource["weights"])->currentData() ==
        "manual";
    for (const auto &name :
         {QString("diffusion"), QString("textEncoder"), QString("vae")}) {
      resource["placement_" + name]->setEnabled(manual);
      i18n::setToolTip(resource["placement_" + name],
          i18n::text("diffusion.ui.choose.where.weights.are.kept.available.when"));
    }
    for (const auto &key : {QString("prefixCache"), QString("prefixCacheType")}) {
      resource[key]->setEnabled(qwen);
      i18n::setToolTip(resource[key], i18n::text("diffusion.ui.reuse.text.encoder.prefix.data.available.for"));
    }
    files["vision"]->parentWidget()->setEnabled(
        p.value("visionRequiredForReferences").toBool());
    files["vision"]->parentWidget()->setToolTip(
        p.value("visionRequiredForReferences").toBool()
            ? i18n::text("diffusion.ui.for.image.editing.add.a.separate.vision")
            : i18n::text("diffusion.ui.this.model.does.not.need.a.separate"));
    if (defaults) {
      putControls(sampling,
                  {{"steps", p.value("steps")},
                   {"guidance", p.value("guidance")},
                   {"sampler", p.value("sampler")},
                   {"scheduler", p.value("scheduler").toString("auto")},
                   {"schedule", p.value("schedule")},
                   {"flowShift", 0.0},
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
    const double cfg = effectiveSampling()["guidance"].toDouble();
    const bool supportsNegative = p.value("negativePrompt").toBool();
    negative->setEnabled(supportsNegative && cfg > 1);
    i18n::setToolTip(negative,
        !supportsNegative ? i18n::text("diffusion.ui.this.model.does.not.support.a.negative")
        : cfg <= 1 ? i18n::text("diffusion.ui.describe.what.to.avoid.requires.guidance.cfg")
                   : i18n::text("diffusion.ui.describe.what.to.avoid.in.the.generated"));
    if (auto *header = q.findChild<QToolButton *>("DiffusionNegativeSectionHeader"))
      header->setToolTip(negative->toolTip());
    updateSamplingAvailability(sampling, sigmas, controls(sampling));
    syncOverrides();
    updateSamplingAvailability(overrideControls, overrideControls["sigmas"], effectiveSampling());
    updateActions();
  }
  void rebuildModels() {
    const auto selected = currentModelId;
    for (auto *box : {models, configModels}) {
      const QSignalBlocker block(box);
      box->clear();
      if (box == models) i18n::addItem(box, i18n::text("diffusion.ui.missing.configuration"), QString{});
      else i18n::setPlaceholderText(box, modelConfigs.isEmpty() ? i18n::text("diffusion.ui.no.configurations") : i18n::text("diffusion.ui.select.configuration"));
      for (const auto &entry : modelConfigs) {
        const auto o = entry.toObject();
        box->addItem(o["name"].toString(), o["id"].toString());
      }
      box->setCurrentIndex(box->findData(selected));
    }
  }
  QString inputTitle(int index) const {
    const auto &input = inputs[index];
    return QString("Image%1%2\n%3%4").arg(index + 1)
        .arg(index == 0 && q.mode() != Mode::Generate ? i18n::text("diffusion.ui.edit.image") : "")
        .arg(input.label)
        .arg(input.regions.isEmpty() ? QString{} : QString(" · ")
             + i18n::text("diffusion.ui.regions") + ": " + QString::number(input.regions.size()));
  }
  void refreshInputRegionLabels() {
    const QSignalBlocker block(inputList);
    for (int i = 0; i < inputs.size(); ++i)
      if (auto *item = inputList->item(i))
        item->setText(inputTitle(i));
  }
  void rebuildInputs() {
    const QSignalBlocker block(inputList);
    const int selected = inputList->currentRow();
    inputList->clear();
    for (int i = 0; i < inputs.size(); ++i) {
      const auto &in = inputs[i];
      auto *item = new QListWidgetItem(
          QIcon(QPixmap::fromImage(thumbnail(in.image, {48, 40}))),
          inputTitle(i),
          inputList);
      item->setData(Qt::UserRole, i);
      item->setData(Qt::UserRole + 1, false);
      item->setData(Qt::UserRole + 2, !in.sourceCanvas.isEmpty());
      item->setData(Qt::UserRole + 3, in.cropToCanvas);
      item->setToolTip(QString("%1 · %2 × %3 px\n%4")
                           .arg(in.source)
                           .arg(in.image.width())
                           .arg(in.image.height())
                           .arg(in.label) + (in.cropToCanvas ? i18n::text("diffusion.ui.cropped.to.the.source.canvas") : "") +
                                            i18n::text("diffusion.ui.double.click.to.inspect.and.edit.this"));
    }
    auto *add = new QListWidgetItem(inputList);
    add->setData(Qt::UserRole, -1);
    add->setToolTip(i18n::text("diffusion.ui.add.input.image"));
    inputList->setFixedHeight(std::min(5, int(inputs.size())) * 48 + 32);
    inputList->setCurrentRow(std::min(selected, int(inputs.size()) - 1));
    rebuildRegionSources();
    if (!regionEditorInput.isEmpty() && inputIndex(regionEditorInput) < 0 && lightbox)
      lightbox->close();
    else if (!regionEditorInput.isEmpty() && lightbox && lightboxTitle) {
      const int index = inputIndex(regionEditorInput);
      i18n::setText(lightboxTitle, i18n::text("diffusion.ui.regions.image.1.2.3.4.px")
                                .arg(index + 1).arg(inputs[index].label)
                                .arg(inputs[index].image.width()).arg(inputs[index].image.height()));
    }
    updateActions();
  }
  void rebuildPresets() {
    presetList = presetStore.allPresets();
    const QSignalBlocker block(presets);
    presets->clear();
    i18n::addItem(presets, i18n::text("diffusion.ui.prompt.preset"));
    for (const auto &p : presetList) {
      const i18n::Message name = p.id == "builtin.generate" ? i18n::text("diffusion.ui.generate.image.2")
          : p.id == "builtin.fill" ? i18n::text("diffusion.ui.generative.fill")
          : p.id == "builtin.background-replace" ? i18n::text("diffusion.ui.replace.background")
          : p.id == "builtin.background-remove" ? i18n::text("diffusion.ui.remove.background")
          : i18n::Message(p.name);
      i18n::addItem(presets, name, p.id);
    }
  }
  void showPreview(const QImage &image, const QImage &, const i18n::Message &title,
                   bool editRegions = false, bool resultPreview = false,
                   const QString &inputId = {}, const QString &destinationInput = {},
                   const QString &destinationRegion = {}, bool createTarget = false) {
    if (image.isNull() && !resultPreview)
      return;
    if (lightbox)
      lightbox->close();
    auto *dialog = new PreviewDialog(&q, Qt::Dialog | Qt::FramelessWindowHint);
    lightbox = dialog;
    regionEditorInput = editRegions ? inputId : QString{};
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    QObject::connect(dialog, &QDialog::finished, &q, [this, dialog] {
      if (lightbox != dialog) return;
      // These controls retain the destination choice between previews.
      outputDestination->setParent(&q);
      fitOutput->setParent(&q);
      apply->setParent(&q);
      outputDestination->hide();
      fitOutput->hide();
      apply->hide();
      lightbox.clear();
      lightboxPreview.clear();
      previewShowsResult = false;
    });
    dialog->setObjectName("DiffusionPreviewDialog");
    i18n::setWindowTitle(dialog, title);
    dialog->setMinimumSize(480, 320);
    const auto available = q.screen()->availableGeometry().size();
    dialog->resize(std::min(1000, int(available.width() * .85)),
                   std::min(760, int(available.height() * .85)));
    if (!resultPreview || !q.onPresentPreview) preparePopupOwnership(dialog, &q);
    auto *main = new QWidget(dialog);
    main->setObjectName("DiffusionPreviewContent");
    auto *columns = new QHBoxLayout(dialog);
    columns->setContentsMargins(0, 0, 0, 0);
    columns->setSpacing(0);
    auto *chooser = new QFrame(dialog);
    chooser->setObjectName("DiffusionComparisonPanel");
    chooser->setFixedWidth(240);
    chooser->setStyleSheet("QFrame#DiffusionComparisonPanel { border-right: 1px solid " +
                          themeColor(ThemeColor::Border).name() + "; }");
    chooser->hide();
    columns->addWidget(chooser);
    columns->addWidget(main, 1);
    auto *root = new QVBoxLayout(main);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(8);
    auto *bar = new QHBoxLayout;
    auto *label = i18n::make<QLabel>(
        title +
        QString(" · %1 × %2 px").arg(image.width()).arg(image.height()));
    lightboxTitle = label;
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    bar->addWidget(label, 1);
    if (resultPreview) {
      dialog->navigate = [this](int direction) { stepPreview(direction); };
      auto *previous = new QToolButton(dialog);
      previousPreview = previous;
      previous->setObjectName("DiffusionPreviousPreview");
      previous->setArrowType(Qt::LeftArrow);
      i18n::setToolTip(previous, i18n::text("diffusion.ui.previous.preview"));
      i18n::setAccessibleName(previous, i18n::text("diffusion.ui.previous.preview"));
      auto *next = new QToolButton(dialog);
      nextPreview = next;
      next->setObjectName("DiffusionNextPreview");
      next->setArrowType(Qt::RightArrow);
      i18n::setToolTip(next, i18n::text("diffusion.ui.next.preview"));
      i18n::setAccessibleName(next, i18n::text("diffusion.ui.next.preview"));
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
    auto *fit = iconButton(ToolGlyph::FitCanvas, i18n::text("diffusion.ui.fit.to.view"), dialog);
    auto *actual =
        iconButton(ToolGlyph::ZoomActual, i18n::text("diffusion.ui.actual.pixels.1.1"), dialog);
    auto *close = iconButton(ToolGlyph::Close, i18n::text("diffusion.ui.close.preview"), dialog);
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
    preview->setNewRegionsAreTargets(!destinationRegion.isEmpty());
    const int regionIndex = inputIndex(inputId);
    preview->setRegions(regionIndex < 0 ? regions : inputs[regionIndex].regions);
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
        new QLabel(editRegions ? i18n::text("diffusion.ui.drag.to.draw.or.move.shift.drag")
                               : i18n::text("diffusion.ui.mouse.wheel.zooms.middle.mouse.pans"));
    hint->setWordWrap(true);
    if (!resultPreview) bottom->addWidget(hint, 1);
    if (resultPreview) {
      auto *outputColumn = new QVBoxLayout;
      outputColumn->setSpacing(5);
      auto *outputRow = new QHBoxLayout;
      outputRow->addWidget(outputDestination);
      outputRow->addWidget(apply);
      outputColumn->addLayout(outputRow);
      outputColumn->addWidget(fitOutput);
      bottom->addLayout(outputColumn);
      bottom->addStretch();
      auto *compare = i18n::make<QPushButton>(i18n::text("diffusion.ui.compare.with.layer"));
      compare->setObjectName("DiffusionCompareLayer");
      compare->setAutoDefault(false);
      apply->setAutoDefault(false);
      compare->setCheckable(true);
      compare->setEnabled(!comparisonLayers.isEmpty() && q.selectedResult());
      compareToggle = compare;
      bottom->addWidget(compare);
      auto *alignment =
          combo({i18n::text("diffusion.ui.canvas.alignment"), i18n::text("diffusion.ui.center.alignment"), i18n::text("diffusion.ui.fit.to.result")},
                {"canvas", "center", "fit"}, dialog);
      alignment->setObjectName("DiffusionCompareAlignment");
      alignment->setMinimumWidth(180);
      alignment->setEnabled(false);
      i18n::setToolTip(alignment,
          i18n::text("diffusion.ui.canvas.document.positions.and.scale.center.native"));
      compareAlignment = alignment;
      bottom->addWidget(alignment);
      auto *chooseLayout = new QVBoxLayout(chooser);
      chooseLayout->setContentsMargins(10, 10, 10, 10);
      auto *chooseHeader = new QHBoxLayout;
      chooseHeader->addWidget(i18n::make<QLabel>(i18n::text("diffusion.ui.compare.with.layer"), chooser), 1);
      auto *closeChooser = iconButton(ToolGlyph::Close, i18n::text("diffusion.ui.stop.comparing"), chooser);
      chooseHeader->addWidget(closeChooser);
      chooseLayout->addLayout(chooseHeader);
      auto *search = new QLineEdit(chooser);
      comparisonSearch = search;
      search->setObjectName("DiffusionComparisonSearch");
      i18n::setPlaceholderText(search, i18n::text("diffusion.ui.search.layers"));
      search->setClearButtonEnabled(true);
      chooseLayout->addWidget(search);
      auto *list = new QListWidget(chooser);
      comparisonList = list;
      list->setObjectName("DiffusionComparisonLayers");
      list->setIconSize(QSize(36, 36));
      list->setItemDelegate(new SubtleItemDelegate(list));
      list->setSelectionMode(QAbstractItemView::SingleSelection);
      list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
      list->setTextElideMode(Qt::ElideRight);
      list->setSpacing(2);
      chooseLayout->addWidget(list, 1);
      comparisonEmpty = i18n::make<QLabel>(i18n::text("diffusion.ui.no.matching.layers"), chooser);
      comparisonEmpty->setAlignment(Qt::AlignCenter);
      chooseLayout->addWidget(comparisonEmpty);
      populateComparisonLayers();
      QObject::connect(search, &QLineEdit::textChanged, dialog,
                       [this] { filterComparisonLayers(); });
      QObject::connect(list, &QListWidget::currentItemChanged, dialog,
                       [this](QListWidgetItem *item) {
                         comparisonLayerId = item ? item->data(Qt::UserRole).toString() : QString{};
                         q.setComparisonReference({}, {}, {});
                         if (item && q.onCompareRequest) q.onCompareRequest();
                       });
      QObject::connect(closeChooser, &QToolButton::clicked, compare,
                       [compare] { compare->setChecked(false); });
      const auto previewSize = dialog->size();
      QObject::connect(compare, &QPushButton::toggled, dialog,
                       [this, dialog, chooser, search, previewSize](bool on) {
                         chooser->setVisible(on);
                         // Center the preview and its attached chooser as one
                         // owned card, independent of the editor's dock layout.
                         const auto size = previewSize + QSize(on ? chooser->width() : 0, 0);
                         dialog->setProperty("workspacePreferredSize", size);
                         if (dialog->isWindow()) dialog->resize(size);
                         if (on) {
                           if (!comparisonLayerId.isEmpty() && q.onCompareRequest) q.onCompareRequest();
                           search->setFocus();
                         }
                         updateActions();
                       });
      QObject::connect(alignment, &QComboBox::currentIndexChanged, preview,
                       [preview](int index) {
                         preview->setComparisonAlignment(
                             DiffusionPreview::Alignment(index));
                       });
    }
    if (editRegions) {
      if (inputId.isEmpty() && profile().value("family") == "flux3")
        i18n::setText(hint, createTarget ? i18n::text("diffusion.ui.draw.the.destination.box.scene.is.fitted")
                                  : i18n::text("diffusion.ui.scene.is.fitted.as.a.guide.boxes"));
      auto *metadata = new QHBoxLayout;
      auto *regionName = new QLineEdit;
      regionName->setObjectName("DiffusionRegionLabel");
      i18n::setPlaceholderText(regionName, i18n::text("diffusion.ui.region.label"));
      regionName->setMaxLength(200);
      regionName->setMaximumWidth(240);
      metadata->addWidget(regionName);
      auto *description = new QLineEdit;
      description->setObjectName("DiffusionRegionDescription");
      i18n::setPlaceholderText(description, i18n::text("diffusion.ui.description.optional"));
      description->setMaxLength(2000);
      description->setVisible(profile().value("family") == "flux3");
      i18n::setToolTip(description, i18n::text("diffusion.ui.optional.semantic.guidance.sent.as.desc.the"));
      metadata->addWidget(description, 1);
      auto *remove =
          iconButton(ToolGlyph::Trash, i18n::text("diffusion.ui.remove.selected.region"), dialog);
      metadata->addWidget(remove);
      root->addLayout(metadata);
      auto *destinationRow = new QWidget(dialog);
      auto *destLayout = new QHBoxLayout(destinationRow);
      destLayout->setContentsMargins(0, 0, 0, 0);
      destLayout->addWidget(i18n::make<QLabel>(i18n::text("diffusion.ui.destination")));
      auto *destination = new ScrollSafe<QComboBox>;
      destination->setObjectName("DiffusionRegionDestination");
      destLayout->addWidget(destination, 1);
      auto *editTarget = i18n::make<QPushButton>(i18n::text("diffusion.ui.edit"));
      editTarget->setObjectName("DiffusionEditDestination");
      destLayout->addWidget(editTarget);
      destinationRow->setVisible(!inputId.isEmpty() && profile().value("family") == "flux3" && (inputs.size() > 1 || hasTransfers()));
      root->addWidget(destinationRow);
      preview->onRegionSelected = [this, regionName, description, destination, editTarget, preview](int index) {
        QSignalBlocker b(regionName);
        QSignalBlocker c(description), d(destination);
        const auto region = index >= 0 ? preview->regions().value(index) : DiffusionRegion{};
        regionName->setText(region.label);
        regionName->setEnabled(index >= 0);
        description->setText(region.description);
        description->setEnabled(index >= 0 && !region.targetOnly);
        i18n::setPlaceholderText(description, region.targetOnly ? i18n::text("diffusion.ui.target.location.no.object.description") : i18n::text("diffusion.ui.description.optional"));
        destination->clear(); i18n::addItem(destination, i18n::text("diffusion.ui.none"), QString{});
        const bool canLink = inputs.size() > 1 && inputIndex(outputSceneId) >= 0;
        for (const auto& r : regions) if (canLink || r.id == region.destinationId)
          destination->addItem("Output/" + r.label, r.id);
        if (!region.destinationId.isEmpty() && destination->findData(region.destinationId) < 0)
          i18n::addItem(destination, i18n::text("diffusion.ui.missing.target.choose.another"), region.destinationId);
        if (canLink) i18n::addItem(destination, i18n::text("diffusion.ui.new.target"), "__new_target__");
        select(destination, region.destinationId);
        destination->setEnabled(index >= 0);
        editTarget->setEnabled(index >= 0 && std::any_of(regions.begin(), regions.end(), [&](const auto& r) {return r.id == region.destinationId;}));
      };
      preview->onRegionsChanged = [this, preview, inputId] {
        if (inputId.isEmpty())
          regions = preview->regions();
        else {
          const int index = inputIndex(inputId);
          if (index < 0)
            return;
          inputs[index].regions = preview->regions();
          rememberInputRegions(inputs[index]);
        }
        refreshInputRegionLabels();
        updateActions();
        reportReferenceLimit();
      };
      QObject::connect(regionName, &QLineEdit::textEdited, preview,
                       &DiffusionPreview::setSelectedRegionLabel);
      QObject::connect(description, &QLineEdit::textEdited, preview, &DiffusionPreview::setSelectedRegionDescription);
      QObject::connect(destination, &QComboBox::activated, &q, [this, preview, inputId, destination](int) {
        const auto selected = preview->selectedRegion();
        if (selected < 0) return;
        const auto regionId = preview->regions()[selected].id;
        const auto targetId = destination->currentData().toString();
        if (targetId == "__new_target__") {
          QTimer::singleShot(0, &q, [this, inputId, regionId] { editDestination(inputId, regionId, true); });
        } else if (q.setRegionDestination(inputId, regionId, targetId)) {
          preview->setRegions(inputs[inputIndex(inputId)].regions);
          preview->setSelectedRegion(selected);
        }
      });
      QObject::connect(editTarget, &QPushButton::clicked, &q, [this, preview, inputId] {
        if (preview->selectedRegion() < 0) return;
        const auto regionId = preview->regions()[preview->selectedRegion()].id;
        QTimer::singleShot(0, &q, [this, inputId, regionId] { editDestination(inputId, regionId, false); });
      });
      if (createTarget) {
        QSet<QString> previous;
        for (const auto& r : regions) previous.insert(r.id);
        preview->onRegionEditFinished = [this, preview, destinationInput, destinationRegion, previous, pending = true]() mutable {
          if (!pending) return;
          const auto index = preview->selectedRegion();
          if (index < 0 || previous.contains(preview->regions()[index].id)) return;
          const auto& target = preview->regions()[index];
          if (!target.bounds.isEmpty() && q.setRegionDestination(destinationInput, destinationRegion, target.id)) pending = false;
        };
      }
      QObject::connect(remove, &QToolButton::clicked, preview,
                       &DiffusionPreview::removeSelectedRegion);
      preview->setSelectedRegion(preview->selectedRegion());
      if (!createTarget && !destinationRegion.isEmpty()) {
        const int source = inputIndex(destinationInput);
        if (source >= 0) for (const auto& r : inputs[source].regions) if (r.id == destinationRegion)
          for (int i = 0; i < regions.size(); ++i) if (regions[i].id == r.destinationId) preview->setSelectedRegion(i);
      }
    }
    root->addLayout(bottom);
    if (resultPreview) {
      hint->setObjectName("DiffusionPreviewHint");
      i18n::setText(hint, i18n::text("diffusion.ui.mouse.wheel.zooms.middle.mouse.pans.browse"));
      hint->setAlignment(Qt::AlignCenter);
      auto font = hint->font();
      font.setPointSizeF(std::max(7., font.pointSizeF() - 1));
      hint->setFont(font);
      hint->setStyleSheet("color: " + themeColor(ThemeColor::SecondaryText).name());
      root->addWidget(hint);
    }
    if (resultPreview)
      refreshDisplayedPreview();
    if (resultPreview) dialog->routeNavigationKeys();
    if (resultPreview && q.onPresentPreview)
      q.onPresentPreview(*dialog);
    else {
      dialog->show();
      dialog->raise();
    }
    preview->setFocus();
  }
  void editDestination(const QString& inputId, const QString& regionId, bool create) {
    const int source = inputIndex(inputId);
    if (busy || source < 0 || inputIndex(outputSceneId) < 0) { q.setStatus(i18n::text("diffusion.ui.choose.an.output.scene.first"), true); return; }
    const auto it = std::find_if(inputs[source].regions.begin(), inputs[source].regions.end(), [&](const auto& r) {return r.id == regionId;});
    if (it == inputs[source].regions.end()) return;
    showPreview(outputLayoutImage(), {}, i18n::text("diffusion.ui.destination.for.image.1.2").arg(source + 1).arg(it->label), true, false, {}, inputId, regionId, create);
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
  auto *title = i18n::make<QLabel>(i18n::text("diffusion.ui.ai.image.editor"));
  titleRow->addWidget(title, 1);
  auto *close = iconButton(ToolGlyph::Close, i18n::text("diffusion.ui.close.ai.image.editor"), this);
  titleRow->addWidget(close);
  outer->addLayout(titleRow);
  connect(close, &QToolButton::clicked, this, [this] {
    if (onClose)
      onClose();
  });
  d->tabs = new QTabWidget(this);
  d->tabs->setObjectName("DiffusionTabs");
  outer->addWidget(d->tabs, 1);
  auto page = [&](const i18n::Message &title, const QString &name,
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
    i18n::addTab(d->tabs, scroll, title);
    return content;
  };
  QVBoxLayout *layout{}, *configLayout{}, *outputLayout{};
  d->configurationPage =
      page(i18n::text("diffusion.ui.configuration"), "DiffusionConfigurationPage", configLayout);
  auto *content = page(i18n::text("diffusion.ui.generate"), "DiffusionGenerationPage", layout);
  page(i18n::text("diffusion.ui.preview"), "DiffusionOutputPage", outputLayout);
  auto *footer = new QWidget(this);
  auto *footerLayout = new QVBoxLayout(footer);
  footerLayout->setContentsMargins(0, 0, 0, 0);
  footerLayout->setSpacing(6);
  outer->addWidget(footer);
  auto *top = new QHBoxLayout;
  d->mode = combo({i18n::text("diffusion.ui.generate"), i18n::text("diffusion.ui.edit.layer"), i18n::text("diffusion.ui.generative.fill")},
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
  d->configModels->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  d->configModels->setMinimumContentsLength(6);
  auto *configurationRow = new QHBoxLayout;
  configurationRow->addWidget(d->configModels, 1);
  d->addModel = iconButton(ToolGlyph::Plus, i18n::text("diffusion.ui.add.configuration"), this);
  d->addModel->setObjectName("DiffusionAddConfiguration");
  d->duplicateModel = iconButton(ToolGlyph::Copy, i18n::text("diffusion.ui.duplicate.configuration"), this);
  d->duplicateModel->setObjectName("DiffusionDuplicateConfiguration");
  d->deleteModel = iconButton(ToolGlyph::Trash, i18n::text("diffusion.ui.delete.configuration"), this);
  d->deleteModel->setObjectName("DiffusionDeleteConfiguration");
  configurationRow->addWidget(d->addModel);
  configurationRow->addWidget(d->duplicateModel);
  configurationRow->addWidget(d->deleteModel);
  configLayout->addLayout(configurationRow);
  configLayout->addWidget(
      section(i18n::text("diffusion.ui.model.files"), "DiffusionModel", modelLayout, true));
  auto *modelForm = d->modelForm = form(modelLayout);
  d->families = new ScrollSafe<QComboBox>;
  d->families->setObjectName("DiffusionFamily");
  i18n::addRow(modelForm, i18n::text("diffusion.ui.family"), d->families);
  d->profiles = new ScrollSafe<QComboBox>;
  d->profiles->setObjectName("DiffusionProfile");
  i18n::addRow(modelForm, i18n::text("diffusion.ui.variant"), d->profiles);
  auto fileRow = [this, content](const QString &key,
                                 const i18n::Message &placeholder) {
    auto *row = new QWidget(content);
    auto *h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(3);
    auto *edit = new QLineEdit;
    i18n::setPlaceholderText(edit, placeholder);
    edit->setObjectName("DiffusionFile_" + key);
    h->addWidget(edit, 1);
    auto *browse = iconButton(ToolGlyph::Folder, i18n::text("diffusion.ui.choose.file"), row);
    h->addWidget(browse);
    connect(browse, &QToolButton::clicked, this, [this, edit, key] {
      const bool checkpoint = d->profile().value("checkpoint").toBool() && key == "diffusion";
      const auto path =
          chooseFile(this, checkpoint ? i18n::text("diffusion.ui.choose.checkpoint") : i18n::text("diffusion.ui.choose.model.component"), edit->text(),
                     checkpoint ? i18n::text("diffusion.ui.checkpoints.safetensors.all.files") :
                                  i18n::text("diffusion.ui.models.gguf.safetensors.all.files"));
      if (!path.isEmpty())
        edit->setText(path);
    });
    return std::pair{row, edit};
  };
  for (const auto &pair :
       QVector<QPair<QString, i18n::Message>>{{"diffusion", i18n::text("diffusion.ui.diffusion.weights")},
                                        {"textEncoder", i18n::text("diffusion.ui.text.encoder")},
                                        {"vae", "VAE"},
                                        {"vision", i18n::text("diffusion.ui.vision.optional")}}) {
    auto [row, edit] = fileRow(pair.first, pair.second);
    d->files[pair.first] = edit;
    i18n::addRow(modelForm, pair.second, row);
  }
  d->loras = std::make_unique<DiffusionLoras>(this);
  modelForm->addRow(d->loras->configurationSection());
  d->backend =
      combo({i18n::text("diffusion.ui.auto"), "Vulkan", "CUDA"}, {"auto", "vulkan", "cuda"}, this);
  d->backend->setObjectName("DiffusionBackend");
  i18n::setToolTip(d->backend,
      i18n::text("diffusion.ui.auto.prefers.an.available.cuda.worker.then"));
  i18n::addRow(modelForm, i18n::text("diffusion.ui.backend"), d->backend);
  auto *presetRow = new QHBoxLayout;
  d->presets = new ScrollSafe<QComboBox>;
  d->presets->setObjectName("DiffusionPresets");
  presetRow->addWidget(d->presets, 1);
  auto *savePreset =
      iconButton(ToolGlyph::Save, i18n::text("diffusion.ui.save.custom.prompt.preset"), content);
  auto *deletePreset =
      iconButton(ToolGlyph::Trash, i18n::text("diffusion.ui.delete.custom.prompt.preset"), content);
  presetRow->addWidget(savePreset);
  presetRow->addWidget(deletePreset);
  layout->addLayout(presetRow);
  d->prompt = new QPlainTextEdit;
  d->prompt->setObjectName("DiffusionPrompt");
  i18n::setPlaceholderText(d->prompt, i18n::text("diffusion.ui.describe.the.image.or.the.changes.you"));
  d->prompt->setMinimumHeight(80);
  d->prompt->setMaximumHeight(100);
  layout->addWidget(d->prompt);
  QVBoxLayout *negativeLayout{};
  layout->addWidget(section(i18n::text("diffusion.ui.negative.prompt"), "DiffusionNegative", negativeLayout));
  d->negative = new QPlainTextEdit;
  d->negative->setObjectName("DiffusionNegativePrompt");
  i18n::setPlaceholderText(d->negative, i18n::text("diffusion.ui.describe.what.to.avoid"));
  d->negative->setMaximumHeight(70);
  negativeLayout->addWidget(d->negative);
  layout->addWidget(d->loras->generationSection());
  auto *essentials = new QVBoxLayout;
  auto *outputRow = new QHBoxLayout;
  outputRow->setObjectName("DiffusionSizeRow");
  outputRow->setSpacing(4);
  outputRow->addStretch();
  outputRow->addWidget(i18n::make<QLabel>(i18n::text("diffusion.ui.size")));
  d->width = new ScrollSafe<QSpinBox>;
  d->height = new ScrollSafe<QSpinBox>;
  for (auto *s : {d->width, d->height}) {
    s->setLocale(QLocale::c());
    s->setRange(32, 8192);
    s->setSingleStep(32);
    s->setValue(1024);
    s->setFixedWidth(88);
    s->setKeyboardTracking(false);
  }
  d->width->setObjectName("DiffusionWidth");
  d->height->setObjectName("DiffusionHeight");
  i18n::setToolTip(d->width, i18n::text("diffusion.ui.output.width.in.pixels"));
  i18n::setToolTip(d->height, i18n::text("diffusion.ui.output.height.in.pixels"));
  outputRow->addWidget(d->width);
  outputRow->addWidget(new QLabel("×"));
  outputRow->addWidget(d->height);
  auto *resetSize =
      iconButton(ToolGlyph::FitCanvas, i18n::text("diffusion.ui.match.layer.canvas.size"), content);
  resetSize->setObjectName("DiffusionResetSize");
  outputRow->addWidget(resetSize);
  auto *selectedSize = iconButton(ToolGlyph::LayerRaster, i18n::text("diffusion.ui.match.selected.input.layer.size"), content);
  selectedSize->setObjectName("DiffusionMatchSelectedSize");
  i18n::setToolTip(selectedSize, i18n::text("diffusion.ui.match.the.selected.input.including.its.canvas"));
  outputRow->addWidget(selectedSize);
  connect(selectedSize, &QToolButton::clicked, this, [this] {
    if (onMatchSelectedSize) onMatchSelectedSize();
  });
  outputRow->addStretch();
  connect(resetSize, &QToolButton::clicked, this, [this] {
    if (onResetSize)
      onResetSize();
  });
  essentials->addLayout(outputRow);
  QVBoxLayout *resolutionLayout{};
  essentials->addWidget(section(i18n::text("diffusion.ui.resolution.selector"), "DiffusionResolution", resolutionLayout));
  auto *resolutionForm = form(resolutionLayout);
  d->aspectRatio = new ScrollSafe<QComboBox>;
  d->aspectRatio->setObjectName("DiffusionAspectRatio");
  d->aspectRatio->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  d->aspectRatio->setMinimumContentsLength(6);
  const QVector<QPair<i18n::Message, QSize>> ratios{
      {i18n::text("diffusion.ui.1.1.square"), {1, 1}}, {i18n::text("diffusion.ui.2.3.portrait.photo"), {2, 3}},
      {i18n::text("diffusion.ui.3.2.photo"), {3, 2}}, {i18n::text("diffusion.ui.3.4.portrait.standard"), {3, 4}},
      {i18n::text("diffusion.ui.4.3.standard"), {4, 3}}, {i18n::text("diffusion.ui.9.16.portrait.widescreen"), {9, 16}},
      {i18n::text("diffusion.ui.16.9.widescreen"), {16, 9}}, {i18n::text("diffusion.ui.21.9.ultrawide"), {21, 9}}, {i18n::text("diffusion.ui.custom"), {}}};
  for (const auto &[name, ratio] : ratios)
    i18n::addItem(d->aspectRatio, name, ratio);
  i18n::setToolTip(d->aspectRatio, i18n::text("diffusion.ui.choose.a.ratio.to.calculate.the.output"));
  i18n::addRow(resolutionForm, i18n::text("diffusion.ui.aspect.ratio"), d->aspectRatio);
  d->megapixels = new ScrollSafe<QComboBox>;
  d->megapixels->setObjectName("DiffusionMegapixels");
  for (int tenths = 1; tenths <= 160; ++tenths)
    d->megapixels->addItem(QString("%1 MP").arg(tenths / 10.0, 0, 'f', 1), tenths / 10.0);
  d->megapixels->setCurrentIndex(9);
  d->megapixels->setMaxVisibleItems(12);
  i18n::setToolTip(d->megapixels, i18n::text("diffusion.ui.approximate.pixel.count.1.0.mp.is"));
  i18n::addRow(resolutionForm, i18n::text("diffusion.ui.megapixels"), d->megapixels);
  d->resolutionMultiple = new ScrollSafe<QComboBox>;
  d->resolutionMultiple->setObjectName("DiffusionResolutionMultiple");
  i18n::addItem(d->resolutionMultiple, i18n::text("diffusion.ui.auto"), 0);
  for (int multiple = 8; multiple <= 128; multiple += 4)
    d->resolutionMultiple->addItem(QString::number(multiple), multiple);
  d->resolutionMultiple->setMaxVisibleItems(12);
  i18n::setToolTip(d->resolutionMultiple, i18n::text("diffusion.ui.round.each.dimension.to.the.nearest.multiple"));
  i18n::addRow(resolutionForm, i18n::text("diffusion.ui.multiple"), d->resolutionMultiple);
  d->resolutionRequirement = new QLabel;
  d->resolutionRequirement->setObjectName("DiffusionResolutionRequirement");
  d->resolutionRequirement->setWordWrap(true);
  resolutionLayout->addWidget(d->resolutionRequirement);
  connect(d->aspectRatio, &QComboBox::activated, this, [this] { d->applyResolutionPreset(); });
  for (auto *selector : {d->megapixels, d->resolutionMultiple})
    connect(selector, &QComboBox::currentIndexChanged, this, [this] { d->applyResolutionPreset(); });
  for (auto *dimension : {d->width, d->height})
    connect(dimension, &QSpinBox::valueChanged, this, [this] {
      if (d->updatingResolution) return;
      const QSignalBlocker block(d->aspectRatio);
      d->aspectRatio->setCurrentIndex(d->aspectRatio->count() - 1);
    });
  QVBoxLayout *overridesLayout{};
  d->overridesSection = section(i18n::text("diffusion.ui.overrides"), "DiffusionOverrides", overridesLayout);
  d->overridesForm = form(overridesLayout);
  d->overridesSection->hide();
  essentials->addWidget(d->overridesSection);
  auto *seedRow = new QHBoxLayout;
  seedRow->addWidget(i18n::make<QLabel>(i18n::text("diffusion.ui.seed")));
  d->seed = new ScrollSafe<QDoubleSpinBox>;
  // JSON seeds are exact integers up to 2^53-1; QSpinBox would truncate them
  // to 32 bits. Zero decimals keep the familiar integer stepper presentation.
  d->seed->setDecimals(0);
  d->seed->setRange(0, 9007199254740991.0);
  d->seed->setSingleStep(1);
  d->seed->setLocale(QLocale::c());
  d->seed->setGroupSeparatorShown(false);
  d->seed->setKeyboardTracking(false);
  d->seed->setObjectName("DiffusionSeed");
  i18n::setToolTip(d->seed, i18n::text("diffusion.ui.use.the.same.seed.to.reproduce.a"));
  seedRow->addWidget(d->seed, 1);
  d->randomSeed = iconButton(ToolGlyph::Refresh, i18n::text("diffusion.ui.generate.a.random.seed"), content);
  d->randomSeed->setObjectName("DiffusionRandomSeed");
  i18n::setToolTip(d->randomSeed,
      i18n::text("diffusion.ui.generate.a.random.seed.random.mode.also"));
  seedRow->addWidget(d->randomSeed);
  connect(d->randomSeed, &QToolButton::clicked, this, [this] {
    quint32 seed;
    do {
      seed = QRandomGenerator::global()->generate();
    } while (seed == d->seed->value());
    d->seed->setValue(seed);
    saveSettings();
  });
  d->seedMode = combo({i18n::text("diffusion.ui.random"), i18n::text("diffusion.ui.fixed"), i18n::text("diffusion.ui.increment")},
                      {"random", "fixed", "increment"}, content);
  d->seedMode->setObjectName("DiffusionSeedMode");
  i18n::setToolTip(d->seedMode,
      i18n::text("diffusion.ui.random.new.seed.per.job.fixed.reuse"));
  seedRow->addWidget(d->seedMode);
  connect(d->seedMode, &QComboBox::currentIndexChanged, this, [this] {
    d->seed->setEnabled(d->seedMode->currentData() != "random");
    saveSettings();
  });
  essentials->addLayout(seedRow);
  layout->addLayout(essentials);
  d->modeSettings = new QWidget(content);
  d->modeSettings->setObjectName("DiffusionModeSettings");
  auto *modeLayout = new QHBoxLayout(d->modeSettings);
  modeLayout->setContentsMargins(0, 0, 0, 0);
  modeLayout->setSpacing(8);
  layout->addWidget(d->modeSettings);
  QVBoxLayout *inputsLayout{};
  layout->addWidget(
      section(i18n::text("diffusion.ui.input.images"), "DiffusionInputs", inputsLayout, true));
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
  d->maxInputSize = new ScrollSafe<QComboBox>;
  d->maxInputSize->setObjectName("DiffusionMaxInputSize");
  for (const int size : {256, 512, 768, 1024, 1280, 1440, 1536, 2048, 2560, 3072, 4096, 6144, 8192})
    i18n::addItem(d->maxInputSize, i18n::text("diffusion.ui.max.input.1.px").arg(size), size);
  d->maxInputSize->setSizeAdjustPolicy(QComboBox::AdjustToContents);
  d->maxInputSize->setMaximumWidth(300);
  d->maxInputSize->setMaxVisibleItems(12);
  setMaxInputSize(QSettings().value("diffusion/maxInputSize", 1024).toInt());
  i18n::setToolTip(d->maxInputSize, i18n::text("diffusion.ui.limit.the.longest.side.of.input.images"));
  inputsLayout->addWidget(d->maxInputSize, 0, Qt::AlignHCenter);
  connect(d->maxInputSize, &QComboBox::currentIndexChanged, this, [this, previous = maxInputSize()]() mutable {
    if (d->lightbox && !d->regionEditorInput.isEmpty()) d->lightbox->close();
    try {
      auto inputs = d->inputs;
      for (auto &input : inputs) {
        d->rememberInputRegions(input);
        d->prepareInput(input);
      }
      d->inputs = std::move(inputs);
      previous = d->maxInputSize->currentData().toInt();
    } catch (const std::exception &error) {
      const QSignalBlocker blocker(d->maxInputSize);
      d->maxInputSize->setCurrentIndex(d->maxInputSize->findData(previous));
      setStatus(QString::fromUtf8(error.what()), true);
      return;
    }
    d->rebuildInputs();
    if (onInputsChanged) onInputsChanged();
    saveSettings();
  });
  inputDelegate->add = [this] {
    if (d->busy || d->inputs.size() >= d->maxInputs())
      return;
    auto *menu = new QMenu(d->inputList);
    const QVector<QPair<QString, InputSource>> sources{
        {i18n::text("diffusion.ui.current.layer.appearance"), InputSource::CurrentLayerAppearance},
        {i18n::text("diffusion.ui.current.layer.raw.pixels"), InputSource::CurrentLayerPixels},
        {i18n::text("diffusion.ui.another.document.layer.appearance"),
         InputSource::DocumentLayerAppearance},
        {i18n::text("diffusion.ui.another.document.layer.raw.pixels"),
         InputSource::DocumentLayerPixels},
        {i18n::text("diffusion.ui.rendered.visible.canvas"), InputSource::VisibleCanvas},
        {i18n::text("diffusion.ui.image.file"), InputSource::File}};
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
  inputDelegate->crop = [this](int row) {
    if (d->busy || row < 0 || row >= d->inputs.size()) return;
    const auto id = d->inputs[row].id;
    const bool enabled = !d->inputs[row].cropToCanvas;
    // The delegate is still dispatching this item; defer the model rebuild.
    QTimer::singleShot(0, this, [this, id, enabled] { setInputCropToCanvas(id, enabled); });
  };
  connect(d->inputList, &QListWidget::currentRowChanged, this,
          [this](int row) {
            if (d->regionSource && row >= 0 && row < d->inputs.size())
              select(d->regionSource, d->inputs[row].id);
            d->updateActions();
          });
  connect(d->inputList, &QListWidget::itemDoubleClicked, this,
          [this](QListWidgetItem *item) {
            if (d->inputList->row(item) >= d->inputs.size())
              return;
            const auto &in = d->inputs[d->inputList->row(item)];
            if (!d->busy)
              showInputRegions(in.id);
            else
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
  layout->addWidget(section(i18n::text("diffusion.ui.regions"), "DiffusionRegions", regionsLayout));
  d->outputSceneRow = new QWidget;
  auto *sceneLayout = new QHBoxLayout(d->outputSceneRow);
  sceneLayout->setContentsMargins(0, 0, 0, 0);
  sceneLayout->addWidget(i18n::make<QLabel>(i18n::text("diffusion.ui.output.scene")));
  d->outputScene = new ScrollSafe<QComboBox>;
  d->outputScene->setObjectName("DiffusionOutputScene");
  i18n::setToolTip(d->outputScene, referenceHelp(i18n::text("diffusion.ui.use.scene.in.your.prompt.to.reference")));
  sceneLayout->addWidget(d->outputScene, 1);
  regionsLayout->addWidget(d->outputSceneRow);
  connect(d->outputScene, &QComboBox::activated, this, [this] {
    setOutputSceneId(d->outputScene->currentData().toString());
  });
  for (auto *size : {d->width, d->height})
    connect(size, &QSpinBox::valueChanged, this, [this] { d->rebuildScenes(); });
  d->regionSource = new ScrollSafe<QComboBox>;
  d->regionSource->setObjectName("DiffusionRegionSource");
  i18n::setToolTip(d->regionSource, i18n::text("diffusion.ui.choose.an.input.image.to.identify.objects"));
  regionsLayout->addWidget(d->regionSource);
  d->rebuildRegionSources();
  connect(d->regionSource, &QComboBox::currentIndexChanged, this,
          [this] { d->updateActions(); });
  d->regionHint = new QLabel;
  d->regionHint->setTextFormat(Qt::PlainText);
  d->regionHint->setWordWrap(true);
  regionsLayout->addWidget(d->regionHint);
  d->regionsButton = i18n::make<QPushButton>(i18n::text("diffusion.ui.edit.regions"));
  d->regionsButton->setObjectName("DiffusionRegionsButton");
  auto *regionButtons = new QHBoxLayout;
  regionButtons->addWidget(d->regionsButton, 1);
  d->clearRegions = iconButton(ToolGlyph::Trash, i18n::text("diffusion.ui.remove.regions.from.this.image.or.output"), content);
  d->clearRegions->setObjectName("DiffusionClearRegions");
  regionButtons->addWidget(d->clearRegions);
  regionsLayout->addLayout(regionButtons);
  d->cropGuidance = i18n::make<QCheckBox>(i18n::text("diffusion.ui.extra.crop.guidance.experimental"));
  d->cropGuidance->setObjectName("DiffusionRegionCropGuidance");
  d->cropGuidance->setChecked(QSettings().value("diffusion/fluxCropGuidance", false).toBool());
  i18n::setToolTip(d->cropGuidance, i18n::text("diffusion.ui.optional.vulkana.guidance.also.send.each.input"));
  regionsLayout->addWidget(d->cropGuidance);
  connect(d->cropGuidance, &QCheckBox::toggled, this, [this] {
    d->updateActions(); d->reportReferenceLimit(); saveSettings();
  });
  connect(d->clearRegions, &QToolButton::clicked, this, [this] {
    const auto id = d->regionSource->currentData().toString();
    if (id.isEmpty())
      setRegions({});
    else
      setInputRegions(id, {});
    if (d->lightboxPreview && d->regionEditorInput == id && !d->previewShowsResult)
      d->lightboxPreview->setRegions({});
  });
  connect(d->regionsButton, &QPushButton::clicked, this, [this] {
    const auto id = d->regionSource->currentData().toString();
    if (!id.isEmpty()) {
      showInputRegions(id);
      return;
    }
    showOutputRegions();
  });
  QVBoxLayout *advanced{};
  configLayout->addWidget(
      section(i18n::text("diffusion.ui.advanced.generation"), "DiffusionGeneration", advanced));
  auto *samplingForm = form(advanced);
  auto addSpin = [content](QFormLayout *f, QMap<QString, QWidget *> &map,
                           const QString &key, const i18n::Message &label, double low,
                           double high, double initial, int decimals = 0) {
    auto *s = new ScrollSafe<QDoubleSpinBox>(content);
    s->setLocale(QLocale::c());
    s->setDecimals(decimals);
    s->setRange(low, high);
    s->setValue(initial);
    s->setKeyboardTracking(false);
    s->setObjectName("Diffusion_" + key);
    if (decimals)
      s->setSingleStep(.1);
    i18n::addRow(f, label, s);
    map[key] = s;
    return s;
  };
  auto addChoice = [content](QFormLayout *f, QMap<QString, QWidget *> &map,
                             const QString &key, const i18n::Message &label,
                             const QStringList &values,
                             const QString &initial) {
    QList<i18n::Message> labels;
    for (const auto &value : values) {
      labels.append(value == "auto" || value == "Auto" ? i18n::text("diffusion.ui.auto")
          : value == "custom" ? i18n::text("diffusion.ui.custom")
          : value == "disabled" ? i18n::text("diffusion.ui.disabled")
          : value == "disk" ? i18n::text("diffusion.ui.disk")
          : value == "manual" ? i18n::text("diffusion.ui.manual")
          : value == "on" ? i18n::text("diffusion.ui.on")
          : value == "off" ? i18n::text("diffusion.ui.off") : i18n::Message(value));
    }
    auto *c = combo(labels, values, content);
    select(c, initial);
    c->setObjectName("Diffusion_" + key);
    i18n::addRow(f, label, c);
    map[key] = c;
    return c;
  };
  auto addFlag = [content](QFormLayout *f, QMap<QString, QWidget *> &map,
                           const QString &key, const i18n::Message &label,
                           bool initial) {
    auto *c = new QCheckBox(content);
    c->setChecked(initial);
    c->setObjectName("Diffusion_" + key);
    i18n::addRow(f, label, c);
    map[key] = c;
    return c;
  };
  addSpin(samplingForm, d->sampling, "steps", i18n::text("diffusion.ui.steps"), 1, 1000, 8);
  auto *guidance = addSpin(samplingForm, d->sampling, "guidance",
                           i18n::text("diffusion.ui.guidance.cfg"), 0, 100, 1, 2);
  addChoice(
      samplingForm, d->sampling, "sampler", i18n::text("diffusion.ui.sampler"),
      {"euler", "euler_a", "heun", "dpm2", "dpm++2s_a", "dpm++2m", "dpm++2mv2",
       "ipndm", "ipndm_v", "lcm", "ddim_trailing", "tcd", "res_multistep", "res_2s",
       "er_sde", "euler_cfg_pp", "euler_a_cfg_pp", "euler_ge", "dpm++2m_sde", "dpm++2m_sde_bt", "lms"},
      "euler");
  addChoice(samplingForm, d->sampling, "scheduler", i18n::text("diffusion.ui.scheduler"),
            {"auto", "discrete", "karras", "exponential", "ays", "gits", "sgm_uniform", "simple",
             "smoothstep", "kl_optimal", "lcm", "bong_tangent", "ltx2", "logit_normal", "flux2", "flux", "beta", "llada_image"},
            "auto");
  auto *schedule =
      addChoice(samplingForm, d->sampling, "schedule", i18n::text("diffusion.ui.sigma.schedule"),
                {"auto", "turbo", "custom"}, "turbo");
  d->sigmas = new QPlainTextEdit;
  d->sigmas->setObjectName("DiffusionSigmas");
  d->sigmas->setPlaceholderText("1.0, 0.9, …, 0.0");
  d->sigmas->setMaximumHeight(65);
  i18n::addRow(samplingForm, i18n::text("diffusion.ui.custom.sigmas"), d->sigmas);
  auto *shift = addSpin(samplingForm, d->sampling, "flowShift", i18n::text("diffusion.ui.flow.shift"), 0,
                        100, 0, 3);
  i18n::property(shift, "specialValueText", i18n::text("diffusion.ui.auto"));
  d->imageToImage = i18n::make<QCheckBox>(i18n::text("diffusion.ui.use.image.to.image.denoising"));
  d->imageToImage->setObjectName("DiffusionImageToImage");
  modeLayout->addWidget(d->imageToImage);
  connect(d->imageToImage, &QCheckBox::toggled, this,
          [this](bool enabled) {
            if (mode() == Mode::Edit)
              d->editImageToImage = enabled;
            d->updateModeControls();
          });
  auto *strength = new CompactValueControl(d->modeSettings);
  strength->setObjectName("Diffusion_strength");
  i18n::setAccessibleName(strength, i18n::text("diffusion.ui.denoising.strength"));
  strength->setDecimals(2);
  strength->setRange(0, 1);
  strength->setSingleStep(.01);
  strength->setValue(.75);
  strength->setFixedHeight(30);
  strength->setMinimumWidth(90);
  strength->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  modeLayout->addWidget(strength, 1);
  d->sampling["strength"] = strength;
  connect(strength, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
          [this] { d->hasExplicitStrength = true; });
  addSpin(samplingForm, d->sampling, "batch", i18n::text("diffusion.ui.images.per.job"), 1, 8, 1);
  addFlag(samplingForm, d->sampling, "vaeTiling", i18n::text("diffusion.ui.vae.tiling"), false);
  addSpin(samplingForm, d->sampling, "vaeTileSize", i18n::text("diffusion.ui.vae.tile.size"), 64, 2048,
          256);
  addSpin(samplingForm, d->sampling, "vaeTileOverlap", i18n::text("diffusion.ui.vae.tile.overlap"), 0,
          .5, .25, 2);
  addFlag(samplingForm, d->sampling, "preview", i18n::text("diffusion.ui.progress.previews"), true);
  addSpin(samplingForm, d->sampling, "previewInterval", i18n::text("diffusion.ui.preview.every.n.steps"),
          1, 1000, 1);
  auto *eta = addSpin(samplingForm, d->sampling, "eta", i18n::text("diffusion.ui.sampler.noise.eta"),
                      -1, 10, -1, 2);
  i18n::property(eta, "specialValueText", i18n::text("diffusion.ui.auto"));
  auto *cacheMode = addChoice(samplingForm, d->sampling, "cacheMode",
                              i18n::text("diffusion.ui.approximate.cache.experimental"),
                              {"disabled", "easycache", "ucache"}, "disabled");
  addSpin(samplingForm, d->sampling, "cacheReuseThreshold",
          i18n::text("diffusion.ui.cache.reuse.threshold"), 0, 1, .2, 3);
  addSpin(samplingForm, d->sampling, "cacheStart", i18n::text("diffusion.ui.cache.start.fraction"), 0, 1,
          .15, 3);
  addSpin(samplingForm, d->sampling, "cacheEnd", i18n::text("diffusion.ui.cache.end.fraction"), 0, 1,
          .95, 3);
  connect(cacheMode, &QComboBox::currentIndexChanged, this,
          [this] { d->updateCapabilities(false); });
  connect(qobject_cast<QComboBox *>(d->sampling["sampler"]),
          &QComboBox::currentIndexChanged, this,
          [this] { d->updateCapabilities(false); });
  auto *defaults = i18n::make<QPushButton>(i18n::text("diffusion.ui.restore.recommended.generation.settings"));
  advanced->addWidget(defaults);
  connect(defaults, &QPushButton::clicked, this,
          [this] {
            d->hasExplicitStrength = false;
            d->updateCapabilities(true);
          });
  connect(guidance, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
          [this] { d->updateCapabilities(false); });
  connect(schedule, &QComboBox::currentIndexChanged, this,
          [this] { d->updateCapabilities(false); });
  d->createOverrideControls(samplingForm);
  QVBoxLayout *resources{};
  configLayout->addWidget(
      section(i18n::text("diffusion.ui.memory.and.performance"), "DiffusionResources", resources));
  auto *resourceForm = form(resources);
  auto *device = addChoice(resourceForm, d->resource, "device",
                           i18n::text("diffusion.ui.inference.device"), {"Auto"}, "Auto");
  device->setItemData(0, QString{});
  auto *budget = addSpin(resourceForm, d->resource, "maxVramGiB",
                         i18n::text("diffusion.ui.vram.budget.gib"), 0, 1024, 10, 2);
  i18n::property(budget, "specialValueText", i18n::text("diffusion.ui.auto"));
  i18n::setToolTip(budget, i18n::text("diffusion.ui.budget.for.managed.weights.workspaces.not.a"));
  auto *threads =
      addSpin(resourceForm, d->resource, "threads", i18n::text("diffusion.ui.cpu.threads"), 0, 1024, 0);
  i18n::property(threads, "specialValueText", i18n::text("diffusion.ui.auto"));
  auto *weights =
      addChoice(resourceForm, d->resource, "weights", i18n::text("diffusion.ui.keep.weights.in"),
                {"auto", "gpu", "ram", "disk", "manual"}, "auto");
  for (const auto &name :
       {QString("diffusion"), QString("textEncoder"), QString("vae")})
    addChoice(resourceForm, d->resource, "placement_" + name,
              (name == "diffusion"     ? i18n::text("diffusion.ui.diffusion.weights")
               : name == "textEncoder" ? i18n::text("diffusion.ui.text.encoder.weights")
                                       : i18n::text("diffusion.ui.vae.weights")),
              {"gpu", "ram", "disk"}, name == "diffusion" ? "gpu" : "ram");
  for (const auto &name :
       {QString("textEncoderCompute"), QString("vaeCompute")})
    addChoice(resourceForm, d->resource, name,
              name == "textEncoderCompute" ? i18n::text("diffusion.ui.text.encoder.compute")
                                           : i18n::text("diffusion.ui.vae.compute"),
              {"gpu", "cpu"}, "gpu");
  for (const auto &name :
       {QString("flashAttention"), QString("directConvolution")})
    addChoice(resourceForm, d->resource, name,
              name == "flashAttention" ? i18n::text("diffusion.ui.flash.attention")
                                       : i18n::text("diffusion.ui.direct.convolution"),
              {"auto", "on", "off"}, "auto");
  for (const auto &pair : QVector<QPair<QString, i18n::Message>>{
           {"sageAttention", i18n::text("diffusion.ui.sage.attention.experimental")},
           {"mmap", i18n::text("diffusion.ui.memory.mapped.loading")},
           {"prefetch", i18n::text("diffusion.ui.prefetch.weights")},
           {"segmentedCompute", i18n::text("diffusion.ui.segmented.computation")},
           {"eagerLoad", i18n::text("diffusion.ui.prepare.weights.at.load")}})
    addFlag(resourceForm, d->resource, pair.first, pair.second,
            pair.first == "mmap" || pair.first == "prefetch" ||
                pair.first == "segmentedCompute");
  addSpin(resourceForm, d->resource, "conditioningCacheSize", i18n::text("diffusion.ui.cached.prompts"),
          0, 64, 1);
  addFlag(resourceForm, d->resource, "prefixCache", i18n::text("diffusion.ui.qwen.prefix.cache"), true);
  addChoice(resourceForm, d->resource, "prefixCacheType", i18n::text("diffusion.ui.prefix.cache.format"),
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
  configLayout->addSpacing(10);
  auto *sessionHeader = new QHBoxLayout;
  sessionHeader->setContentsMargins(4, 2, 4, 2);
  sessionHeader->setSpacing(10);
  auto *sessionHeading = i18n::make<QLabel>(i18n::text("diffusion.ui.session"));
  sessionHeading->setStyleSheet("color: " + themeColor(ThemeColor::SecondaryText).name());
  auto sessionFont = sessionHeading->font();
  sessionFont.setPointSizeF(std::max(7., sessionFont.pointSizeF() - 1));
  sessionHeading->setFont(sessionFont);
  sessionHeader->addWidget(sessionHeading);
  auto *sessionDivider = new QFrame;
  sessionDivider->setObjectName("DiffusionSessionDivider");
  sessionDivider->setFixedHeight(1);
  const auto border = themeColor(ThemeColor::Border);
  sessionDivider->setStyleSheet(QString("border: none; background: qlineargradient(x1: 0, y1: 0, x2: 1, y2: 0, "
      "stop: 0 rgba(%1, %2, %3, 110), stop: 1 rgba(%1, %2, %3, 0));")
      .arg(border.red()).arg(border.green()).arg(border.blue()));
  sessionHeader->addWidget(sessionDivider, 1);
  configLayout->addLayout(sessionHeader);
  configLayout->addWidget(section(i18n::text("diffusion.ui.loaded.models"), "DiffusionLoadedModels",
                                 d->loadedModelsLayout, true));
  auto *empty = i18n::make<QLabel>(i18n::text("diffusion.ui.no.models.loaded"));
  empty->setObjectName("DiffusionLoadedModelsEmpty");
  d->loadedModelsLayout->addWidget(empty);
  d->generate = i18n::make<GenerateButton>(toolGlyph(ToolGlyph::Effects), i18n::text("diffusion.ui.generate"));
  d->generate->setObjectName("DiffusionGenerate");
  layout->addWidget(d->generate);
  auto *generationThumbnail = i18n::make<ClickableImage>(i18n::text("diffusion.ui.generating"));
  generationThumbnail->setObjectName("DiffusionGenerationThumbnail");
  generationThumbnail->setAlignment(Qt::AlignCenter);
  generationThumbnail->setMinimumHeight(96);
  generationThumbnail->setMaximumHeight(220);
  generationThumbnail->setMinimumWidth(1);
  generationThumbnail->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  generationThumbnail->setCursor(Qt::PointingHandCursor);
  generationThumbnail->clicked = [this] { showLatestPreview(); };
  d->generationImage = generationThumbnail;
  generationThumbnail->hide();
  layout->addWidget(generationThumbnail);
  d->progress = new QProgressBar;
  d->progress->setObjectName("DiffusionProgress");
  d->progress->setRange(0, 0);
  d->progress->setVisible(false);
  footerLayout->addWidget(d->progress);
  d->status = i18n::make<QLabel>(i18n::text("diffusion.ui.choose.model.files.and.enter.a.prompt"));
  d->status->setWordWrap(true);
  d->status->setTextInteractionFlags(Qt::TextSelectableByMouse);
  footerLayout->addWidget(d->status);
  auto *resultThumbnail = i18n::make<ClickableImage>(i18n::text("diffusion.ui.generated.previews.appear.here"));
  resultThumbnail->clicked = [this] { showResultPreview(); };
  resultThumbnail->setCursor(Qt::PointingHandCursor);
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
  d->resultList->setMovement(QListView::Static);
  d->resultList->setDragDropMode(QAbstractItemView::NoDragDrop);
  d->resultList->setFlow(QListView::LeftToRight);
  d->resultList->setWrapping(false);
  d->resultList->setIconSize({64, 48});
  d->resultList->setMaximumHeight(95);
  outputLayout->addWidget(d->resultList);
  d->outputDestination =
      combo({i18n::text("diffusion.ui.add.new.layer"), i18n::text("diffusion.ui.replace.selected.layer"), i18n::text("diffusion.ui.open.new.document")},
            {"newLayer", "replaceSelected", "newDocument"}, this);
  d->outputDestination->setObjectName("DiffusionOutputDestination");
  d->outputDestination->setMinimumWidth(200);
  d->fitOutput = i18n::make<QCheckBox>(i18n::text("diffusion.ui.fit.to.original.bounds"), this);
  d->fitOutput->setObjectName("DiffusionFitOutput");
  d->fitOutput->hide();
  connect(d->fitOutput, &QCheckBox::toggled, this, [this](bool checked) {
    const auto index = selectedResultIndex();
    if (index < 0 || destination() != Destination::NewLayer || d->results[index].mode == Mode::Fill) return;
    d->results[index].fitToBounds = checked;
    d->updateOutputControls();
  });
  connect(d->outputDestination, &QComboBox::currentIndexChanged, this, [this] {
    d->updateOutputControls();
    if (onDestinationChanged)
      onDestinationChanged();
  });
  d->apply = i18n::make<QPushButton>(i18n::text("diffusion.ui.apply"), this);
  d->apply->setObjectName("DiffusionApply");
  d->outputDestination->hide();
  d->apply->hide();
  d->regenerate = new QPushButton(toolGlyph(ToolGlyph::Refresh), i18n::text("diffusion.ui.generate.again"));
  d->regenerate->setObjectName("DiffusionRegenerate");
  d->discard = i18n::make<QPushButton>(i18n::text("diffusion.ui.discard"));
  d->discard->setObjectName("DiffusionDiscard");
  auto *resultsActions = new QHBoxLayout;
  resultsActions->addStretch();
  resultsActions->addWidget(d->regenerate);
  resultsActions->addWidget(d->discard);
  resultsActions->addStretch();
  outputLayout->addLayout(resultsActions);
  QVBoxLayout *detailsLayout;
  d->resultDetails = section(i18n::text("diffusion.ui.image.details"), "DiffusionResultDetails", detailsLayout, true);
  outputLayout->addWidget(d->resultDetails);
  auto *detailsForm = form(detailsLayout);
  const auto detail = [&](const i18n::Message &title, const QString &name) {
    auto *label = new QLabel;
    label->setObjectName(name);
    label->setWordWrap(true);
    label->setTextFormat(Qt::PlainText);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    i18n::addRow(detailsForm, title, label);
    return label;
  };
  d->resultConfig = detail(i18n::text("diffusion.ui.configuration"), "DiffusionResultConfig");
  d->resultModel = detail(i18n::text("diffusion.ui.model"), "DiffusionResultModel");
  d->resultSize = detail(i18n::text("diffusion.ui.resolution"), "DiffusionResultSize");
  d->resultSize->setLayoutDirection(Qt::LeftToRight);
  auto *resultSeedRow = new QHBoxLayout;
  d->resultSeed = new QLineEdit;
  d->resultSeed->setObjectName("DiffusionResultSeed");
  d->resultSeed->setReadOnly(true);
  d->resultSeed->setLayoutDirection(Qt::LeftToRight);
  d->copyResultSeed = iconButton(ToolGlyph::Copy, i18n::text("diffusion.ui.copy.seed"), this);
  d->copyResultSeed->setObjectName("DiffusionCopyResultSeed");
  resultSeedRow->addWidget(d->resultSeed);
  resultSeedRow->addWidget(d->copyResultSeed);
  detailsForm->addRow(i18n::make<QLabel>(i18n::text("diffusion.ui.seed")), resultSeedRow);
  d->resultCreated = detail(i18n::text("diffusion.ui.generated"), "DiffusionResultCreated");
  d->resultDetails->hide();
  connect(d->copyResultSeed, &QToolButton::clicked, this, [this] {
    QApplication::clipboard()->setText(d->resultSeed->text());
  });
  layout->addStretch();
  configLayout->addStretch();
  outputLayout->addStretch();
  connect(d->generate, &QPushButton::clicked, this, [this] {
    if (d->busy) {
      if (onCancel) onCancel();
      return;
    }
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
    if (d->lightbox) d->lightbox->accept();
    if (onApply)
      onApply();
  });
  connect(d->discard, &QPushButton::clicked, this, [this] {
    if (onDiscard)
      onDiscard();
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
  const auto profileChanged = [this] {
    d->updateCapabilities(true);
    if (!d->updating && d->resolutionMultiple->currentData().toInt() == 0)
      d->applyResolutionPreset();
    if (!d->updating && onConfigurationChanged)
      onConfigurationChanged();
  };
  connect(d->profiles, &QComboBox::currentIndexChanged, this, profileChanged);
  connect(d->families, &QComboBox::currentIndexChanged, this, [this, profileChanged] {
    d->rebuildVariants();
    profileChanged();
  });
  connect(d->addModel, &QToolButton::clicked, this, [this] { d->createModel(false); });
  connect(d->configModels, &QComboBox::currentIndexChanged, this, [this](int index) {
    if (!d->updating && index >= 0) d->loadModel(d->configModels->itemData(index).toString());
  });
  connect(d->models, &QComboBox::currentIndexChanged, this, [this] {
    if (!d->updating) d->loadModel(d->models->currentData().toString());
  });
  connect(d->duplicateModel, &QToolButton::clicked, this, [this] { d->createModel(true); });
  connect(d->deleteModel, &QToolButton::clicked, this, [this] {
    if (d->busy || d->currentModelId.isEmpty()) return;
    const auto id = d->currentModelId;
    if (!confirmConfigurationDelete(this, configurationName())) return;
    saveSettings();
    d->loras->forgetConfiguration(id);
    d->generationOverrides.remove(id);
    for (qsizetype i = 0; i < d->modelConfigs.size(); ++i)
      if (d->modelConfigs[i].toObject()["id"] == id) {
        d->modelConfigs.removeAt(i);
        break;
      }
    d->loadModel(d->modelConfigs.isEmpty() ? QString{} : d->modelConfigs.first().toObject()["id"].toString());
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
        promptText(this, i18n::text("diffusion.ui.save.prompt.preset"), i18n::text("diffusion.ui.name"), {}, &ok).trimmed();
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
    setStatus(i18n::text("diffusion.ui.prompt.preset.saved"));
  });
  connect(deletePreset, &QToolButton::clicked, this, [this] {
    const auto id = d->presets->currentData().toString();
    if (id.isEmpty() || id.startsWith("builtin.")) {
      setStatus(i18n::text("diffusion.ui.choose.a.custom.preset.to.remove"));
      return;
    }
    QString error;
    if (!d->presetStore.remove(id, &error))
      setStatus(error, true);
    else
      d->rebuildPresets();
  });
  d->defaultResources = controls(d->resource);
  d->defaultSampling = controls(d->sampling);
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
  if (savedGeneration.isObject()) {
    putControls(d->sampling, savedGeneration.object());
    if (savedGeneration.object().contains("strength"))
      d->hasExplicitStrength = true;
  }
  d->editImageToImage = settings.value("diffusion/editImageToImage",
                                      d->editImageToImage).toBool();
  d->prompt->setPlainText(settings.value("diffusion/prompt").toString());
  d->negative->setPlainText(
      settings.value("diffusion/negativePrompt").toString());
  const auto savedSeed = settings.value("diffusion/seed", "0").toString();
  d->seed->setValue(savedSeed.toDouble());
  {
    QSignalBlocker block(d->seedMode);
    select(d->seedMode,
           settings.value("diffusion/seedMode", "random").toString());
  }
  d->seed->setEnabled(d->seedMode->currentData() != "random");
  d->sigmas->setPlainText(settings.value("diffusion/sigmas").toString());
  if (settings.contains("diffusion/exposedGeneration"))
    d->setExposedGeneration(QJsonDocument::fromJson(
        settings.value("diffusion/exposedGeneration").toByteArray()).array());
  d->width->setValue(settings.value("diffusion/width", 1024).toInt());
  d->height->setValue(settings.value("diffusion/height", 1024).toInt());
  setMode(Mode(std::clamp(settings.value("diffusion/mode", 0).toInt(), 0, 2)));
  d->updateCapabilities(false);
  d->rebuildInputs();
  d->detectAspectRatio();
  connect(d->backend, &QComboBox::currentIndexChanged, this, [this] {
    if (d->updating) return;
    // Device IDs belong to a backend, so don't carry a CUDA ID into Vulkan.
    select(qobject_cast<QComboBox *>(d->resource["device"]), QString{});
    saveSettings();
  });
  for (auto *edit : d->files)
    connect(edit, &QLineEdit::textChanged, this,
            [this] { d->updateActions(); });
  d->saveTimer = new QTimer(this);
  d->saveTimer->setSingleShot(true);
  d->saveTimer->setInterval(200);
  connect(d->saveTimer, &QTimer::timeout, this, &DiffusionPanel::saveSettings);
  QSet<QWidget *> observed{d->families, d->profiles, d->backend, d->sigmas, d->imageToImage, d->seed};
  for (auto *widget : d->resource) observed.insert(widget);
  for (auto *widget : d->sampling) observed.insert(widget);
  for (auto *widget : d->files) observed.insert(widget);
  for (auto *widget : observed) {
    const bool samplingControl = d->sampling.values().contains(widget) || widget == d->sigmas;
    const auto changed = [this, samplingControl] {
      if (samplingControl && !d->updating && !d->initializing) d->updateCapabilities(false);
      d->queueSave();
    };
    if (auto *value = qobject_cast<QDoubleSpinBox *>(widget)) connect(value, &QDoubleSpinBox::valueChanged, this, changed);
    else if (auto *value = qobject_cast<QSpinBox *>(widget)) connect(value, &QSpinBox::valueChanged, this, changed);
    else if (auto *value = qobject_cast<QComboBox *>(widget)) connect(value, &QComboBox::currentIndexChanged, this, changed);
    else if (auto *value = qobject_cast<QCheckBox *>(widget)) connect(value, &QCheckBox::toggled, this, changed);
    else if (auto *value = qobject_cast<QLineEdit *>(widget)) connect(value, &QLineEdit::textChanged, this, changed);
    else if (auto *value = qobject_cast<QPlainTextEdit *>(widget)) connect(value, &QPlainTextEdit::textChanged, this, changed);
  }
  d->loras->onDefaultsChanged = [this] { d->queueSave(); };
  connect(&i18n::Localization::instance(), &i18n::Localization::languageChanged, this, [this] {
    const QScopedValueRollback guard(d->updating, true);
    d->refreshInputRegionLabels();
    {
      const QSignalBlocker block(d->maxInputSize);
      for (int i = 0; i < d->maxInputSize->count(); ++i)
        d->maxInputSize->setItemText(i, i18n::text("diffusion.ui.max.input.1.px").arg(d->maxInputSize->itemData(i).toInt()));
    }
    d->updateCapabilities(false);
    if (d->pendingItem) d->pendingItem->setText(i18n::text("diffusion.ui.generating"));
  });
  d->initializing = false;
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
bool DiffusionPanel::supportsMode(Mode mode) const {
  const auto modes = d->profile().value("modes");
  if (!modes.isArray()) return true; // Older worker profiles supported all three modes.
  const QStringList names{"generate", "edit", "fill"};
  return modes.toArray().contains(names.value(int(mode)));
}
bool DiffusionPanel::supportsImageInputs() const {
  return d->profile().value("referenceImages").toBool(true);
}
bool DiffusionPanel::supportsRegions() const {
  return d->profile().value("spatialGuidance").toString("none") != "none";
}
void DiffusionPanel::showGeneration() { d->tabs->setCurrentIndex(1); }
void DiffusionPanel::showResults() { d->tabs->setCurrentIndex(2); }
void DiffusionPanel::setMode(Mode value, bool notify) {
  const QSignalBlocker b(d->mode);
  d->mode->setCurrentIndex(int(value));
  d->updateCapabilities(false);
  d->rebuildInputs();
  if (notify && onModeChanged)
    onModeChanged(mode());
}
DiffusionPanel::Destination DiffusionPanel::destination() const {
  return Destination(d->outputDestination->currentIndex());
}
QString DiffusionPanel::configurationName() const {
  return d->models->currentIndex() > 0 ? d->models->currentText()
                                      : i18n::text("diffusion.ui.current.configuration");
}
void DiffusionPanel::setDestination(Destination value) {
  d->outputDestination->setCurrentIndex(int(value));
}
void DiffusionPanel::setOutputCanvasSize(QSize size) {
  if (d->outputCanvasSize == size) return;
  d->outputCanvasSize = size;
  d->updateOutputControls();
}
QTransform DiffusionPanel::resultPlacement() const {
  const auto *result = selectedResult();
  if (!result || destination() == Destination::NewDocument) return {};
  if (result->mode == Mode::Fill || destination() == Destination::ReplaceSelected)
    return result->toDocument;
  if (result->fitToBounds.value_or(result->mode == Mode::Edit)) {
    if (result->mode == Mode::Edit) return result->toDocument;
    if (d->outputCanvasSize.isValid() && !result->image.isNull())
      return QTransform::fromScale(double(d->outputCanvasSize.width()) / result->image.width(),
                                   double(d->outputCanvasSize.height()) / result->image.height());
  }
  // Remove only scale: keep the captured pixel origin, rotation and reflection.
  const auto &t = result->toDocument;
  const double x = std::hypot(t.m11(), t.m12()), y = std::hypot(t.m21(), t.m22());
  if (x <= 0 || y <= 0) return QTransform::fromTranslate(t.dx(), t.dy());
  return QTransform(t.m11() / x, t.m12() / x, t.m21() / y, t.m22() / y, t.dx(), t.dy());
}
bool DiffusionPanel::imageToImageEnabled() const {
  return (supportsMode(Mode::Fill) && mode() == Mode::Fill) ||
         (supportsMode(Mode::Edit) && mode() == Mode::Edit && d->editImageToImage);
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
  if (d->profile().value("checkpoint").toBool())
    files.remove("textEncoder");
  QJsonObject result{{"profile", d->profiles->currentData().toString()},
                     {"files", files}, {"settings", resources}};
  if (!d->loras->defaults().isEmpty()) result["loras"] = d->loras->defaults();
  return result;
}
QJsonObject DiffusionPanel::generationParameters() const {
  auto params = d->effectiveSampling();
  const auto customSigmas = params.take("sigmas").toString();
  if (!d->profile().value("flowSchedule").toBool(true))
    params.remove("flowShift");
  const auto loras = d->loras->enabledLoras();
  if (!loras.isEmpty()) params["loras"] = loras;
  if (supportsImageInputs())
    params["referenceMaxSize"] = d->maxInputSize->currentData().toInt();
  const auto sampler = params["sampler"].toString();
  if (!(sampler.contains("_a") || sampler.contains("sde") ||
        QStringList{"ddim_trailing", "tcd", "res_multistep", "res_2s"}.contains(sampler)))
    params.remove("eta");
  if (!imageToImageEnabled())
    params.remove("strength");
  params["prompt"] = d->prompt->toPlainText();
  if (d->negative->isEnabled() &&
      !d->negative->toPlainText().trimmed().isEmpty())
    params["negativePrompt"] = d->negative->toPlainText();
  params["width"] = d->width->value();
  params["height"] = d->height->value();
  params["seed"] = qint64(d->seed->value());
  if (params["schedule"] == "custom") {
    QString text = customSigmas.trimmed();
    if (!text.startsWith('['))
      text = '[' + text + ']';
    const auto doc = QJsonDocument::fromJson(text.toUtf8());
    params["sigmas"] = doc.isArray() ? QJsonValue(doc.array())
                                     : QJsonValue(customSigmas);
  }
  return params;
}
QJsonObject DiffusionPanel::prepareGeneration() {
  const auto profile = d->profile();
  const int alignment = profile.value("alignment").toInt(32);
  const auto sizeError = diffusion::resolutionError(d->width->value(), d->height->value(),
      alignment, profile.value("name").toString().toStdString());
  if (!sizeError.empty())
    throw std::invalid_argument(sizeError);
  if (const auto error = d->transferError(); !error.isEmpty())
    throw std::invalid_argument(error.toStdString());
  if (supportsImageInputs() && std::any_of(d->inputs.begin(), d->inputs.end(), [](const auto &in) { return in.image.isNull(); }))
    throw std::invalid_argument(i18n::text("diffusion.ui.a.cropped.layer.is.outside.its.canvas").toStdString());
  if (!d->referencesFit())
    throw std::invalid_argument(i18n::text("diffusion.ui.images.and.region.guides.exceed.the.model").toStdString());
  if (d->seedMode->currentData() == "random")
    d->seed->setValue(QRandomGenerator::global()->generate());
  else
    d->seed->interpretText();
  return generationParameters();
}
void DiffusionPanel::generationAccepted() {
  if (d->seedMode->currentData() == "increment") {
    const auto seed = quint64(d->seed->value());
    const auto count = quint64(generationParameters()["batch"].toInt(1));
    d->seed->setValue(double((seed + count) % 9007199254740992ULL));
  }
  saveSettings();
}
void DiffusionPanel::setProfiles(const QJsonArray &profiles) {
  const bool initial = d->profileList.isEmpty();
  d->profileList = profiles;
  const auto current = initial ? QString("qwen-image-2.1-turbo") : d->profiles->currentData().toString();
  const QSignalBlocker b(d->families);
  d->families->clear();
  for (const auto &value : profiles) {
    const auto p = value.toObject();
    const auto family = Impl::familyKey(p);
    if (d->families->findData(family) < 0)
      d->families->addItem(p.value("familyName").toString(p.value("name").toString()), family);
  }
  d->selectProfile(current);
  if (d->resource.contains("prefixCache"))
    d->updateCapabilities(initial);
}
void DiffusionPanel::setLoadedModels(const QJsonArray &models, bool canUnload) {
  d->canUnloadModels = canUnload;
  if (models != d->loadedModels) {
    d->loadedModels = models;
    d->unloadButtons.clear();
    while (auto *item = d->loadedModelsLayout->takeAt(0)) {
      delete item->widget();
      delete item;
    }
    if (models.isEmpty()) {
      auto *empty = i18n::make<QLabel>(i18n::text("diffusion.ui.no.models.loaded"));
      empty->setObjectName("DiffusionLoadedModelsEmpty");
      d->loadedModelsLayout->addWidget(empty);
    }
    for (const auto &value : models) {
      const auto model = value.toObject();
      auto *row = new QWidget;
      row->setObjectName("DiffusionLoadedModel");
      auto *layout = new QVBoxLayout(row);
      layout->setContentsMargins(0, 0, 0, 0);
      layout->setSpacing(4);
      auto *header = new QHBoxLayout;
      const auto profile = model.value("profile").toObject();
      auto *title = new QLabel(profile.value("name").toString() + " · " +
                              model.value("backend").toString().toUpper());
      title->setObjectName("DiffusionLoadedModelTitle");
      title->setTextFormat(Qt::PlainText);
      title->setWordWrap(true);
      header->addWidget(title, 1);
      auto *unload = i18n::make<QPushButton>(i18n::text("diffusion.ui.unload"));
      unload->setObjectName("DiffusionUnloadModel");
      const auto id = model.value("id").toString();
      unload->setProperty("modelId", id);
      i18n::setToolTip(unload, i18n::text("diffusion.ui.unload.this.configuration.and.its.components.to"));
      connect(unload, &QPushButton::clicked, this, [this, id] {
        if (!d->busy && d->canUnloadModels && onUnloadModel)
          onUnloadModel(id);
      });
      d->unloadButtons.push_back(unload);
      header->addWidget(unload);
      layout->addLayout(header);
      const auto files = model.value("files").toObject();
      for (const auto &[key, label] : QVector<QPair<QString, i18n::Message>>{
               {"diffusion", "Diffusion"}, {"textEncoder", i18n::text("diffusion.ui.text.encoder")},
               {"vae", "VAE"}, {"vision", "Vision"}}) {
        const auto path = files.value(key).toString();
        if (path.isEmpty())
          continue;
        auto *file = i18n::make<QLabel>(label + " · " + QFileInfo(path).fileName());
        file->setObjectName("DiffusionLoadedFile_" + key);
        file->setTextFormat(Qt::PlainText);
        file->setWordWrap(true);
        file->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        file->setTextInteractionFlags(Qt::TextSelectableByMouse);
        file->setToolTip(path);
        layout->addWidget(file);
      }
      d->loadedModelsLayout->addWidget(row);
    }
  }
  d->updateActions();
}
void DiffusionPanel::setDevices(const QJsonArray &devices) {
  auto *c = qobject_cast<QComboBox *>(d->resource["device"]);
  const auto current = c->currentData().toString();
  const QSignalBlocker b(c);
  c->clear();
  i18n::addItem(c, i18n::text("diffusion.ui.auto"), QString{});
  for (const auto &value : devices) {
    const auto v = value.toObject();
    c->addItem(v["name"].toString(), v["id"].toString());
  }
  if (!current.isEmpty() && c->findData(current) < 0)
    i18n::addItem(c, i18n::text("diffusion.ui.1.unavailable").arg(i18n::ltrToken(current)), current);
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
      i18n::addItem(c, i18n::text("diffusion.ui.auto"), "auto");
    for (const auto &value : pair.second)
      c->addItem(value.toString(), value.toString());
    if (!current.isEmpty() && c->findData(current) < 0)
      i18n::addItem(c, i18n::text("diffusion.ui.1.unavailable").arg(i18n::ltrToken(current)), current);
    select(c, current);
  }
  d->updateCapabilities(false);
}
void DiffusionPanel::setConfiguration(const QJsonObject &config,
                                      const QString &) {
  const QScopedValueRollback guard(d->updating, true);
  const auto previous = d->profiles->currentData();
  d->selectProfile(config["profile"].toString());
  d->updateCapabilities(previous != d->profiles->currentData());
  auto key = d->currentModelId;
  if (key.isEmpty()) {
    const QJsonObject identity{{"profile", config.value("profile")}, {"files", config.value("files")}};
    key = "draft:" + QString::fromUtf8(QJsonDocument(identity).toJson(QJsonDocument::Compact));
  }
  d->loras->setConfiguration(key, config.value("loras").toArray());
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
  d->updateCapabilities(false);
}
void DiffusionPanel::setResolution(QSize size) {
  d->setAlignedSize(size);
  d->detectAspectRatio();
}
const QVector<DiffusionPanel::Input> &DiffusionPanel::inputs() const {
  return d->inputs;
}
bool DiffusionPanel::addInput(Input input) {
  if (!supportsImageInputs()) {
    setStatus(i18n::text("diffusion.ui.this.model.generates.from.text.only.choose"));
    return false;
  }
  if (input.image.isNull())
    return false;
  if (d->inputs.size() >= d->maxInputs()) {
    setStatus(i18n::text("diffusion.ui.this.model.accepts.1.references.in.total")
                  .arg(d->profile().value("maxReferenceImages").toInt(8)),
              true);
    return false;
  }
  input.image = input.image.copy();
  if (input.id.isEmpty() || d->inputIndex(input.id) >= 0)
    input.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
  d->prepareInput(input);
  d->inputs.append(std::move(input));
  d->rebuildInputs();
  if (onInputsChanged)
    onInputsChanged();
  return true;
}
void DiffusionPanel::setInputs(QVector<Input> inputs) {
  QSet<QString> ids;
  for (auto &input : inputs) {
    if (input.id.isEmpty() || ids.contains(input.id))
      input.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    ids.insert(input.id);
    d->prepareInput(input);
  }
  d->inputs = std::move(inputs);
  d->rebuildInputs();
}
void DiffusionPanel::clearInputs() {
  d->inputs.clear();
  d->outputSceneId.clear();
  d->rebuildInputs();
}
const DiffusionPanel::Input *DiffusionPanel::selectedInput() const {
  const int row = d->inputList->currentRow();
  return row >= 0 && row < d->inputs.size() ? &d->inputs[row] : nullptr;
}
int DiffusionPanel::maxInputSize() const { return d->maxInputSize->currentData().toInt(); }
void DiffusionPanel::setMaxInputSize(int size) {
  size = std::clamp(size, 64, 8192);
  int index = d->maxInputSize->findData(size);
  if (index < 0) {
    // Preserve a previously saved/custom limit without rounding it to a preset.
    index = 0;
    while (index < d->maxInputSize->count() && d->maxInputSize->itemData(index).toInt() < size) ++index;
    d->maxInputSize->insertItem(index, i18n::text("diffusion.ui.max.input.1.px").arg(size), size);
  }
  d->maxInputSize->setCurrentIndex(index);
}
bool DiffusionPanel::setInputCropToCanvas(const QString &id, bool cropped) {
  const int index = d->inputIndex(id);
  if (d->busy || index < 0 || d->inputs[index].sourceCanvas.isEmpty()) return false;
  try {
    auto input = d->inputs[index];
    d->rememberInputRegions(input);
    input.cropToCanvas = cropped;
    d->prepareInput(input);
    if (d->lightbox && d->regionEditorInput == id) d->lightbox->close();
    d->inputs[index] = std::move(input);
    d->rebuildInputs();
    if (d->inputs[index].image.isNull())
      setStatus(i18n::text("diffusion.ui.this.layer.is.outside.its.source.canvas"), true);
    else if (d->inputs[index].regions.size() < d->inputs[index].fullRegions.size())
      setStatus(i18n::text("diffusion.ui.regions.outside.the.canvas.are.hidden.until"));
    if (onInputsChanged) onInputsChanged();
    return true;
  } catch (const std::exception &error) {
    setStatus(QString::fromUtf8(error.what()), true);
    return false;
  }
}
QJsonArray DiffusionPanel::regionsJson() const {
  QJsonArray array;
  for (const auto &r : d->regions)
    if (!r.bounds.isEmpty()) {
      if (r.targetOnly && d->profile().value("family") != "flux3") continue;
      QJsonObject row{{"x", r.bounds.x()},
                               {"y", r.bounds.y()},
                               {"width", r.bounds.width()},
                               {"height", r.bounds.height()},
                               {"label", r.label}};
      if (d->profile().value("family") == "flux3") {
        row["id"] = r.id; row["description"] = r.description;
        if (r.targetOnly) row["targetOnly"] = true;
      }
      array.append(row);
    }
  return array;
}
QJsonArray DiffusionPanel::inputRegionsJson() const {
  QJsonArray groups;
  for (int i = 0; i < d->inputs.size(); ++i) {
    QJsonArray regions;
    for (const auto &r : d->inputs[i].regions) {
      QJsonObject row{{"x", r.bounds.x()}, {"y", r.bounds.y()},
                                  {"width", r.bounds.width()}, {"height", r.bounds.height()},
                                  {"label", r.label}};
      if (d->profile().value("family") == "flux3") {
        row["id"] = r.id; row["description"] = r.description;
        if (!r.destinationId.isEmpty()) row["targetId"] = r.destinationId;
      }
      regions.append(row);
    }
    if (!regions.isEmpty())
      groups.append(QJsonObject{{"image", i + 1}, {"regions", regions}});
  }
  return groups;
}
bool DiffusionPanel::regionCropGuidance() const {
  return d->profile().value("family") == "flux3" && d->cropGuidance->isChecked();
}
QString DiffusionPanel::outputSceneId() const { return d->outputSceneId; }
int DiffusionPanel::outputSceneIndex() const { return d->inputIndex(d->outputSceneId) + 1; }
bool DiffusionPanel::setOutputSceneId(const QString &id) {
  if (d->busy || d->inputIndex(id) < 0) return false;
  d->outputSceneId = id; d->rebuildScenes(); d->updateActions(); return true;
}
QJsonObject DiffusionPanel::spatialParameters() const {
  if (!supportsRegions()) return {};
  const auto output = regionsJson(), inputs = inputRegionsJson();
  const bool flux = d->profile().value("family") == "flux3";
  if (output.isEmpty() && inputs.isEmpty() && !(flux && outputSceneIndex() > 0)) return {};
  QJsonObject spatial{{"version", flux ? 3 : inputs.isEmpty() ? 1 : 2}, {"regions", output}};
  if (!inputs.isEmpty()) spatial["inputRegions"] = inputs;
  if (flux) {
    spatial["cropGuidance"] = regionCropGuidance();
    if (outputSceneIndex() > 0) spatial["outputScene"] = outputSceneIndex();
  }
  return spatial;
}
bool DiffusionPanel::setRegionDestination(const QString &inputId, const QString &regionId, const QString &targetId) {
  const int input = d->inputIndex(inputId);
  if (d->busy || input < 0) return false;
  auto source = std::find_if(d->inputs[input].regions.begin(), d->inputs[input].regions.end(), [&](const auto &r) {return r.id == regionId;});
  if (source == d->inputs[input].regions.end()) return false;
  if (!targetId.isEmpty()) {
    if (d->profile().value("family") != "flux3" || d->inputs.size() < 2 || outputSceneIndex() < 1) return false;
    auto target = std::find_if(d->regions.begin(), d->regions.end(), [&](const auto& r) {return r.id == targetId;});
    if (target == d->regions.end()) return false;
    target->targetOnly = true;
  }
  source->destinationId = targetId;
  d->rememberInputRegions(d->inputs[input]);
  d->rebuildScenes();
  d->updateActions(); return true;
}
void DiffusionPanel::showOutputRegions() {
  if (d->busy || !supportsRegions()) return;
  if (d->profile().value("family") == "flux3")
    d->showPreview(d->outputLayoutImage(), {}, i18n::text("diffusion.ui.output.layout"), true);
  else {
    QImage image;
    if (!d->inputs.isEmpty()) image = d->inputs.front().image;
    else {
      image = QImage(d->width->value(), d->height->value(), QImage::Format_RGBA8888);
      image.fill(Qt::white);
    }
    d->showPreview(image, {}, i18n::text("diffusion.ui.output.layout.regions"), true);
  }
}
bool DiffusionPanel::setInputRegions(const QString &id, QVector<DiffusionRegion> regions) {
  const int index = d->inputIndex(id);
  if (index < 0 || d->busy)
    return false;
  d->inputs[index].regions = std::move(regions);
  d->rememberInputRegions(d->inputs[index]);
  d->refreshInputRegionLabels();
  d->updateActions();
  d->reportReferenceLimit();
  return true;
}
void DiffusionPanel::showInputRegions(const QString &id) {
  const int index = d->inputIndex(id);
  if (index < 0 || d->busy || !supportsRegions())
    return;
  select(d->regionSource, id);
  d->showPreview(d->inputs[index].image, {},
                 i18n::text("diffusion.ui.regions.image.1.2").arg(index + 1).arg(d->inputs[index].label),
                 true, false, id);
}
void DiffusionPanel::setRegions(QVector<DiffusionRegion> regions) {
  Impl::regionIds(regions);
  d->regions = std::move(regions);
  d->updateActions();
  d->reportReferenceLimit();
}
QVector<DiffusionRegion> DiffusionPanel::regions() const { return d->regions; }
void DiffusionPanel::appendResult(Result result) {
  if (result.image.isNull())
    return;
  if (!result.metadata.contains("createdAt"))
    result.metadata["createdAt"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
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
        i18n::text("diffusion.ui.seed.1").arg(request["seed"].toVariant().toString());
  const QSize requested(request["width"].toInt(), request["height"].toInt());
  if (requested.isValid() && requested != r.image.size())
    details += i18n::text("diffusion.ui.requested.1.2.px")
                   .arg(requested.width())
                   .arg(requested.height());
  item->setToolTip(details);
  d->resultList->setCurrentRow(int(d->results.size()) - 1);
  d->refreshDisplayedPreview();
}
void DiffusionPanel::setResultPreview(int index, QImage image) {
  if (index < 0 || index >= d->results.size() || image.isNull())
    return;
  d->results[index].image = std::move(image);
  d->resultList->item(index)->setIcon(
      QIcon(QPixmap::fromImage(thumbnail(d->results[index].image, {64, 48}))));
  if (index == selectedResultIndex())
    d->refreshDisplayedPreview();
  else if (index == d->results.size() - 1)
    d->refreshGenerationPreview();
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
  i18n::setText(d->resultImage, i18n::text("diffusion.ui.generated.previews.appear.here"));
  if (d->lightbox)
    d->lightbox->close();
  d->refreshDisplayedPreview();
}
void DiffusionPanel::removeSelectedResult() {
  const int index = selectedResultIndex();
  if (index < 0 || index >= d->results.size())
    return;
  if (d->lightbox)
    d->lightbox->close();
  {
    const QSignalBlocker block(d->resultList);
    d->results.removeAt(index);
    delete d->resultList->takeItem(index);
    if (d->results.isEmpty())
      d->resultList->setCurrentItem(d->pendingItem);
    else
      d->resultList->setCurrentRow(std::min(index, int(d->results.size()) - 1));
  }
  d->refreshDisplayedPreview();
  if (onResultSelected)
    onResultSelected(selectedResultIndex());
}
void DiffusionPanel::showLatestPreview() {
  if (d->busy && d->pendingItem) d->resultList->setCurrentItem(d->pendingItem);
  else if (!d->results.isEmpty()) d->resultList->setCurrentRow(int(d->results.size()) - 1);
  showResultPreview();
}
void DiffusionPanel::showResultPreview() {
  if (const auto *result = selectedResult())
    d->showPreview(result->image, result->before,
                   i18n::text("diffusion.ui.ai.image.preview.1").arg(result->label), false, true);
  else if (d->pendingSelected())
    d->showPreview(d->draftPreview, {}, i18n::text("diffusion.ui.pending.preview.approximate"), false,
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
void DiffusionPanel::setStatus(const i18n::Message &text, bool error) {
  i18n::setText(d->status, text);
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
  d->pendingItem->setText(i18n::text("diffusion.ui.pending"));
  d->pendingItem->setIcon(
      QIcon(QPixmap::fromImage(thumbnail(d->draftPreview, {64, 48}))));
  d->pendingItem->setToolTip(i18n::text("diffusion.ui.live.draft.approximate.final.detail.appears.after"));
  d->refreshGenerationPreview();
  if (d->pendingSelected())
    d->refreshDisplayedPreview();
  else
    d->updateActions();
}
void DiffusionPanel::setTargetCompatible(bool selected) {
  d->canReplaceSelected = selected;
  d->updateActions();
}
void DiffusionPanel::setComparisonLayers(QVector<ComparisonLayer> layers) {
  if (d->comparisonLayers == layers) return;
  d->comparisonLayers = std::move(layers);
  if (std::none_of(d->comparisonLayers.begin(), d->comparisonLayers.end(),
                   [this](const auto &layer) { return layer.id == d->comparisonLayerId; })) {
    d->comparisonLayerId.clear();
    setComparisonReference({}, {}, {});
  }
  d->populateComparisonLayers();
  if (d->comparisonLayers.isEmpty() && d->compareToggle) d->compareToggle->setChecked(false);
  d->updateActions();
}
QString DiffusionPanel::comparisonLayerId() const { return d->comparisonLayerId; }
bool DiffusionPanel::comparisonActive() const {
  return d->previewShowsResult && selectedResult() && d->lightbox && d->lightbox->isVisible() &&
         d->compareToggle && d->compareToggle->isChecked() && !d->comparisonLayerId.isEmpty();
}
void DiffusionPanel::setComparisonReference(QImage image, QTransform toDocument,
                                            const QString &label) {
  d->comparisonReference = std::move(image);
  d->comparisonTransform = toDocument;
  if (d->previewShowsResult && d->lightboxPreview) {
    d->lightboxPreview->setComparison(d->comparisonReference, toDocument, resultPlacement());
    if (d->compareToggle)
      i18n::setToolTip(d->compareToggle, d->comparisonReference.isNull() ? i18n::Message{} :
          i18n::text("diffusion.ui.left.1.right.result.drag.the.divider").arg(label));
  }
  d->updateActions();
}
void DiffusionPanel::saveSettings() {
  if (d->initializing || d->updating) return;
  if (d->saveTimer) d->saveTimer->stop();
  for (qsizetype i = 0; i < d->modelConfigs.size(); ++i) {
    auto entry = d->modelConfigs[i].toObject();
    if (entry["id"] != d->currentModelId) continue;
    const auto values = d->modelEntry();
    for (auto it = values.begin(); it != values.end(); ++it) entry[it.key()] = it.value();
    d->modelConfigs[i] = entry;
    break;
  }
  QSettings s;
  s.setValue("diffusion/maxInputSize", d->maxInputSize->currentData().toInt());
  s.setValue("diffusion/fluxCropGuidance", d->cropGuidance->isChecked());
  s.setValue("diffusion/modelId", d->currentModelId);
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
  s.setValue("diffusion/seed", QString::number(quint64(d->seed->value())));
  s.setValue("diffusion/seedMode", d->seedMode->currentData());
  s.setValue("diffusion/sigmas", d->sigmas->toPlainText());
  s.setValue("diffusion/exposedGeneration",
             QJsonDocument(d->exposedGeneration()).toJson(QJsonDocument::Compact));
  s.setValue("diffusion/width", d->width->value());
  s.setValue("diffusion/height", d->height->value());
  s.setValue("diffusion/mode", int(mode()));
  s.setValue("diffusion/editImageToImage", d->editImageToImage);
}
} // namespace imageeditor::ui
