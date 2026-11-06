#include "imageeditor/ui/DiffusionLoras.hpp"
#include "imageeditor/ui/CollapsibleSection.hpp"
#include "imageeditor/ui/CompactValueControl.hpp"
#include "imageeditor/ui/PopupOwnership.hpp"
#include "imageeditor/ui/ToolIcons.hpp"
#include <QCheckBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QSet>
#include <QTimer>
#include <QToolButton>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace imageeditor::ui {
namespace {
QToolButton *button(ToolGlyph glyph, const QString &tip, QWidget *parent) {
  auto *result = new QToolButton(parent);
  result->setIcon(toolGlyph(glyph));
  result->setIconSize({16, 16});
  result->setFixedSize(28, 28);
  result->setAutoRaise(true);
  result->setToolTip(tip);
  result->setAccessibleName(tip);
  return result;
}
CompactValueControl *strengthControl(double strength, QWidget *parent) {
  auto *value = new CompactValueControl(parent);
  value->setRange(-4, 4);
  value->setDecimals(2);
  value->setSingleStep(.05);
  value->setSuffix("×");
  value->setValue(strength);
  value->setFixedWidth(120);
  value->setToolTip("LoRA strength. 1.00 uses its full effect; 0.00 has no effect.");
  value->setAccessibleName("LoRA strength");
  return value;
}
void clearLayout(QVBoxLayout *layout) {
  while (auto *item = layout->takeAt(0)) {
    delete item->widget();
    delete item;
  }
}
}

struct DiffusionLoras::Impl {
  struct Entry {
    QString id, path, name;
    double strength{1};
    bool enabled{true};
    QString displayName() const {
      const auto friendly = name.trimmed();
      const auto label = friendly.isEmpty() ? QFileInfo(path).completeBaseName() : friendly;
      return label.isEmpty() ? QString("Unnamed LoRA") : label;
    }
  };
  DiffusionLoras &owner;
  QWidget *config{}, *generation{}, *parent{};
  QVBoxLayout *configRows{}, *generationRows{};
  QVector<Entry> entries;
  QString activeKey;
  QMap<QString, QMap<QString, QJsonObject>> overrides;
  bool busy{};

  explicit Impl(DiffusionLoras &owner, QWidget *parent) : owner(owner), parent(parent) {}
  Entry *entry(const QString &id) {
    for (auto &value : entries) if (value.id == id) return &value;
    return nullptr;
  }
  QJsonObject effective(const Entry &value) const {
    const auto override = overrides.value(activeKey).value(value.id);
    return {{"enabled", override.value("enabled").toBool(value.enabled)},
            {"strength", override.value("strength").toDouble(value.strength)}};
  }
  void updateHeader() {
    int enabled = 0;
    for (const auto &value : entries) enabled += effective(value)["enabled"].toBool();
    generation->findChild<QToolButton *>("DiffusionLorasSectionHeader")
        ->setText(QString("LoRAs (%1/%2 enabled)").arg(enabled).arg(entries.size()));
    generation->setVisible(!entries.isEmpty());
    generation->setEnabled(!busy);
  }
  void setOverride(const QString &id, const QString &field, QJsonValue value) {
    auto *item = entry(id);
    if (!item) return;
    const auto baseline = field == "enabled" ? QJsonValue(item->enabled) : QJsonValue(item->strength);
    if (value == baseline) overrides[activeKey][id].remove(field);
    else overrides[activeKey][id][field] = value;
    updateHeader();
  }
  void rebuildGeneration() {
    clearLayout(generationRows);
    for (const auto &item : entries) {
      auto *row = new QWidget(generation);
      row->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
      auto *layout = new QHBoxLayout(row);
      layout->setContentsMargins(0, 0, 0, 0);
      const auto state = effective(item);
      auto *enabled = new QCheckBox(row);
      enabled->setObjectName("DiffusionLoraEnabled");
      enabled->setProperty("loraId", item.id);
      enabled->setToolTip("Enable this LoRA for generation");
      enabled->setChecked(state["enabled"].toBool());
      auto *name = new QLabel(item.displayName(), row);
      name->setObjectName("DiffusionLoraName");
      name->setTextFormat(Qt::PlainText);
      name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
      name->setToolTip(item.displayName());
      enabled->setAccessibleName(item.displayName());
      auto *strength = strengthControl(state["strength"].toDouble(), row);
      strength->setObjectName("DiffusionLoraStrength");
      strength->setProperty("loraId", item.id);
      strength->setEnabled(enabled->isChecked());
      layout->addWidget(enabled);
      layout->addWidget(name, 1);
      layout->addWidget(strength);
      generationRows->addWidget(row);
      QObject::connect(enabled, &QCheckBox::toggled, &owner, [this, id = item.id, strength](bool checked) {
        setOverride(id, "enabled", checked);
        strength->setEnabled(checked);
      });
      QObject::connect(strength, &QDoubleSpinBox::valueChanged, &owner, [this, id = item.id](double value) {
        setOverride(id, "strength", value);
      });
    }
    auto *resetRow = new QWidget(generation);
    auto *resetLayout = new QHBoxLayout(resetRow);
    resetLayout->setContentsMargins(0, 0, 0, 0);
    resetLayout->addStretch();
    auto *reset = button(ToolGlyph::Refresh, "Reset LoRAs to configuration defaults", resetRow);
    reset->setObjectName("DiffusionLorasReset");
    resetLayout->addWidget(reset);
    generationRows->addWidget(resetRow);
    QObject::connect(reset, &QToolButton::clicked, &owner, [this] {
      overrides.remove(activeKey);
      QTimer::singleShot(0, &owner, [this] { rebuildGeneration(); });
    });
    updateHeader();
  }
  void choosePath(QLineEdit *field) {
    QFileDialog chooser(parent, "Choose LoRA", field->text(), "LoRA models (*.safetensors *.gguf);;All files (*)");
    chooser.setFileMode(QFileDialog::ExistingFile);
    preparePopupOwnership(&chooser, parent);
    if (chooser.exec() == QDialog::Accepted && !chooser.selectedFiles().isEmpty())
      field->setText(chooser.selectedFiles().front());
  }
  void rebuildConfiguration() {
    clearLayout(configRows);
    for (const auto &item : entries) {
      auto *row = new QWidget(config);
      auto *layout = new QVBoxLayout(row);
      layout->setContentsMargins(0, 0, 0, 0);
      layout->setSpacing(4);
      auto *fileRow = new QHBoxLayout;
      auto *path = new QLineEdit(item.path, row);
      path->setObjectName("DiffusionLoraPath");
      path->setProperty("loraId", item.id);
      path->setPlaceholderText("LoRA file…");
      path->setMinimumWidth(0);
      fileRow->addWidget(path, 1);
      auto *browse = button(ToolGlyph::Folder, "Choose LoRA file", row);
      auto *remove = button(ToolGlyph::Trash, "Remove LoRA", row);
      remove->setObjectName("DiffusionLoraRemove");
      fileRow->addWidget(browse);
      fileRow->addWidget(remove);
      layout->addLayout(fileRow);
      auto *details = new QHBoxLayout;
      auto *enabled = new QCheckBox(row);
      enabled->setObjectName("DiffusionLoraDefaultEnabled");
      enabled->setToolTip("Enabled by default");
      enabled->setAccessibleName("LoRA enabled by default");
      enabled->setChecked(item.enabled);
      auto *name = new QLineEdit(item.name, row);
      name->setObjectName("DiffusionLoraFriendlyName");
      name->setProperty("loraId", item.id);
      name->setPlaceholderText("Friendly name");
      name->setMaxLength(200);
      name->setMinimumWidth(0);
      auto *strength = strengthControl(item.strength, row);
      strength->setObjectName("DiffusionLoraDefaultStrength");
      details->addWidget(enabled);
      details->addWidget(name, 1);
      details->addWidget(strength);
      layout->addLayout(details);
      configRows->addWidget(row);
      QObject::connect(browse, &QToolButton::clicked, &owner, [this, path] { choosePath(path); });
      QObject::connect(path, &QLineEdit::textChanged, &owner, [this, id = item.id, name](const QString &text) {
        auto *value = entry(id);
        if (!value) return;
        const bool automaticName = value->name.isEmpty() || value->name == QFileInfo(value->path).completeBaseName();
        value->path = text.trimmed();
        overrides[activeKey].remove(id);
        if (automaticName) {
          value->name = QFileInfo(value->path).completeBaseName();
          name->setText(value->name);
        }
        rebuildGeneration();
      });
      QObject::connect(name, &QLineEdit::textEdited, &owner, [this, id = item.id](const QString &text) {
        if (auto *value = entry(id)) value->name = text;
        rebuildGeneration();
      });
      QObject::connect(enabled, &QCheckBox::toggled, &owner, [this, id = item.id](bool checked) {
        if (auto *value = entry(id)) value->enabled = checked;
        rebuildGeneration();
      });
      QObject::connect(strength, &QDoubleSpinBox::valueChanged, &owner, [this, id = item.id](double strength) {
        if (auto *value = entry(id)) value->strength = strength;
        rebuildGeneration();
      });
      QObject::connect(remove, &QToolButton::clicked, &owner, [this, id = item.id] {
        entries.removeIf([&](const Entry &value) { return value.id == id; });
        overrides[activeKey].remove(id);
        QTimer::singleShot(0, &owner, [this] { rebuildConfiguration(); rebuildGeneration(); });
      });
    }
    auto *add = button(ToolGlyph::Plus, "Add LoRA", config);
    add->setObjectName("DiffusionLoraAdd");
    configRows->addWidget(add, 0, Qt::AlignHCenter);
    QObject::connect(add, &QToolButton::clicked, &owner, [this] {
      entries.append({QUuid::createUuid().toString(QUuid::WithoutBraces), {}, {}, 1, true});
      QTimer::singleShot(0, &owner, [this] { rebuildConfiguration(); rebuildGeneration(); });
    });
  }
};

DiffusionLoras::DiffusionLoras(QWidget *parent) : QObject(parent), d(std::make_unique<Impl>(*this, parent)) {
  d->config = collapsibleSection(i18n::Message("LoRAs (optional)"), "DiffusionLoraDefaults", d->configRows);
  d->generation = collapsibleSection(i18n::Message("LoRAs"), "DiffusionLoras", d->generationRows);
  for (auto *section : {d->config, d->generation}) {
    section->setParent(parent);
    section->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
  }
  d->config->findChild<QToolButton *>("DiffusionLoraDefaultsSectionHeader")->setChecked(false);
  d->generation->findChild<QToolButton *>("DiffusionLorasSectionHeader")->setChecked(false);
  d->config->findChild<QWidget *>("DiffusionLoraDefaultsSectionBody")->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
  d->generation->findChild<QWidget *>("DiffusionLorasSectionBody")->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
  d->configRows->setAlignment(Qt::AlignTop);
  d->generationRows->setAlignment(Qt::AlignTop);
  d->rebuildConfiguration();
  d->rebuildGeneration();
}
DiffusionLoras::~DiffusionLoras() = default;
QWidget *DiffusionLoras::configurationSection() const { return d->config; }
QWidget *DiffusionLoras::generationSection() const { return d->generation; }
void DiffusionLoras::setConfiguration(const QString &key, const QJsonArray &defaults) {
  d->activeKey = key;
  d->entries.clear();
  QSet<QString> ids;
  for (const auto &value : defaults) {
    const auto object = value.toObject();
    auto id = object.value("id").toString();
    if (id.isEmpty() || ids.contains(id)) id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    ids.insert(id);
    const double strength = object.value("strength").toDouble(1);
    d->entries.append({id, object.value("path").toString(), object.value("name").toString(),
                       std::isfinite(strength) ? std::clamp(strength, -4.0, 4.0) : 1.0,
                       object.value("enabled").toBool(true)});
  }
  auto &saved = d->overrides[key];
  for (auto it = saved.begin(); it != saved.end();) {
    if (!ids.contains(it.key())) it = saved.erase(it);
    else ++it;
  }
  d->rebuildConfiguration();
  d->rebuildGeneration();
}
void DiffusionLoras::retainOverridesFor(const QString &key) {
  d->overrides[key] = d->overrides.value(d->activeKey);
  d->activeKey = key;
}
void DiffusionLoras::forgetConfiguration(const QString &key) { d->overrides.remove(key); }
QJsonArray DiffusionLoras::defaults() const {
  QJsonArray result;
  for (const auto &entry : d->entries)
    result.append(QJsonObject{{"id", entry.id}, {"path", entry.path}, {"name", entry.name},
                              {"strength", entry.strength}, {"enabled", entry.enabled}});
  return result;
}
QJsonArray DiffusionLoras::enabledLoras() const {
  QJsonArray result;
  for (const auto &entry : d->entries) {
    const auto effective = d->effective(entry);
    if (effective["enabled"].toBool() && effective["strength"].toDouble() != 0)
      result.append(QJsonObject{{"path", entry.path}, {"name", entry.displayName()}, {"strength", effective["strength"]}});
  }
  return result;
}
void DiffusionLoras::setBusy(bool busy) {
  d->busy = busy;
  d->config->setEnabled(!busy);
  d->generation->setEnabled(!busy);
}
}
