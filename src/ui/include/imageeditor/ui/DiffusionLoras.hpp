#pragma once

#include <QObject>
#include <QJsonArray>
#include <QString>
#include <memory>
#include <functional>

class QWidget;

namespace imageeditor::ui {
// Configuration defaults and session-only generation overrides share stable
// entry IDs. Editing a generation control never writes into the defaults.
class DiffusionLoras final : public QObject {
public:
  explicit DiffusionLoras(QWidget *parent);
  ~DiffusionLoras() override;
  QWidget *configurationSection() const;
  QWidget *generationSection() const;
  void setConfiguration(const QString &key, const QJsonArray &defaults);
  void retainOverridesFor(const QString &key);
  void forgetConfiguration(const QString &key);
  QJsonArray defaults() const;
  QJsonArray enabledLoras() const;
  void setBusy(bool busy);
  std::function<void()> onDefaultsChanged;

private:
  struct Impl;
  std::unique_ptr<Impl> d;
};
}
