#pragma once

#include "imageeditor/ui/DiffusionPreview.hpp"
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QWidget>
#include <functional>
#include <memory>

namespace imageeditor::ui {
class DiffusionPanel final : public QWidget {
public:
  enum class Mode { Generate, Edit, Fill };
  enum class Destination { NewLayer, ReplaceSelected, NewDocument };
  enum class InputSource {
    CurrentLayerAppearance,
    CurrentLayerPixels,
    DocumentLayerAppearance,
    DocumentLayerPixels,
    VisibleCanvas,
    File
  };
  struct Input {
    QImage image;
    QString label;
    QString source;
    bool target{};
    QJsonObject metadata;
    QTransform toDocument;
    QString id; // Stable ownership while inputs are reordered.
    QVector<DiffusionRegion> regions;
    QSize sourceCanvas;
    bool cropToCanvas{};
    QImage fullImage;
    QTransform fullToDocument;
    QVector<DiffusionRegion> fullRegions;
    QStringList visibleRegionIds;
    QSize outputSize; // Before the reference-only size limit.
  };
  struct Result {
    QImage image;
    QImage before;
    QString label;
    QJsonObject metadata;
    QTransform toDocument;
  };

  explicit DiffusionPanel(QWidget *parent = nullptr);
  ~DiffusionPanel() override;
  Mode mode() const;
  void showGeneration();
  void showResults();
  QJsonObject prepareGeneration();
  void generationAccepted();
  void setMode(Mode mode, bool notify = false);
  Destination destination() const;
  void setDestination(Destination destination);
  QString workerExecutable()
      const; // Test/development override; not exposed or persisted.
  QString backend() const;
  bool imageToImageEnabled() const;
  void setWorkerExecutable(const QString &path);
  QJsonObject
  configuration() const; // {profile, files, settings}: worker load parameters.
  QJsonObject generationParameters() const;
  QJsonObject request() const { return generationParameters(); }
  void setProfiles(const QJsonArray &profiles);
  void setDevices(const QJsonArray &devices);
  void setLoadedModels(const QJsonArray &models, bool canUnload);
  void setSamplers(const QJsonArray &samplers, const QJsonArray &schedulers);
  void setConfiguration(const QJsonObject &configuration,
                        const QString &name = {});
  void setResolution(QSize size);
  const QVector<Input> &inputs() const;
  bool
  addInput(Input input); // Maximum is the selected profile's reference limit.
  void setInputs(QVector<Input> inputs);
  void clearInputs();
  bool setInputCropToCanvas(const QString &inputId, bool cropped);
  const Input *selectedInput() const;
  int maxInputSize() const;
  void setMaxInputSize(int size);
  QJsonArray inputRegionsJson() const;
  bool regionCropGuidance() const;
  QString outputSceneId() const;
  bool setOutputSceneId(const QString &inputId);
  int outputSceneIndex() const; // One-based request reference number, or zero.
  QJsonObject spatialParameters() const;
  bool setRegionDestination(const QString &inputId, const QString &regionId, const QString &targetId);
  void showOutputRegions();
  bool setInputRegions(const QString &inputId, QVector<DiffusionRegion> regions);
  void showInputRegions(const QString &inputId);
  QJsonArray regionsJson() const;
  void setRegions(QVector<DiffusionRegion> regions);
  QVector<DiffusionRegion> regions() const;
  void appendResult(Result result);
  void setResultPreview(int index, QImage image);
  const QVector<Result> &results() const;
  int selectedResultIndex() const;
  const Result *selectedResult() const;
  void clearResults();
  void removeSelectedResult();
  void showResultPreview();
  void setBusy(bool busy);
  bool isBusy() const;
  void setStatus(const QString &text, bool error = false);
  void setProgress(int step, int steps, double elapsedMs = 0);
  void setProgressPreview(QImage image);
  void setTargetCompatible(bool selected);
  void setComparisonAvailable(bool available);
  bool comparisonActive() const;
  void setComparisonReference(QImage image, QTransform toDocument,
                              const QString &label);
  void saveSettings();

  std::function<void()> onGenerate;
  std::function<void()> onApply;
  std::function<void()> onDiscard;
  std::function<void()> onCancel;
  std::function<void(const QString &)> onUnloadModel;
  std::function<void()> onResetSize;
  std::function<void()> onMatchSelectedSize;
  std::function<void()> onCompareRequest;
  std::function<void()> onClose;
  std::function<void(InputSource)> onAddInput;
  std::function<void()> onInputsChanged;
  std::function<void(Mode)> onModeChanged;
  std::function<void()> onConfigurationChanged;
  std::function<void()> onDestinationChanged;
  std::function<void(int)> onResultSelected;

private:
  struct Impl;
  std::unique_ptr<Impl> d;
};
} // namespace imageeditor::ui
