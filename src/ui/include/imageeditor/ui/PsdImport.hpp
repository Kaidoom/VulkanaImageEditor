#pragma once
#include "imageeditor/core/Document.hpp"
#include <QImage>
#include <QMap>
#include <QStringList>
#include <atomic>
#include <future>
#include <limits>

namespace imageeditor::ui {
struct PsdSource;
enum class PsdRoute { Editable, Raster, BasePixels, Skip };
struct PsdRasterScope {
  bool available{};
  bool geometryBaked{
      true}; // Saved layer channels are already positioned/resampled content.
  bool vectorShapeBaked{}; // Geometric fill/stroke and vector coverage in saved
                           // shape channels.
  bool bitmapMaskBaked{}, layerOpacityBaked{}, layerEffectsBaked{};
};
struct PsdLayerInfo {
  int sourceIndex{}, parent{-1};
  QString name, type, status, details;
  QStringList issues, fonts;
  QMap<QString, QString> proposedFonts;
  PsdRasterScope savedRaster;
  bool container{}, editable{}, raster{}, basePixels{}, visible{true};
  PsdRoute suggested{PsdRoute::Skip};
};
struct PsdInspection {
  std::shared_ptr<const PsdSource> source;
  QSize size;
  double ppi{72};
  QString summary, report, error;
  std::vector<PsdLayerInfo>
      layers; // Bottom-to-top source order, stable record IDs.
  bool savedComposite{};
};
struct PsdChoice {
  PsdRoute route{PsdRoute::Editable};
  // Requested face -> chosen installed family. Applies only to that face's
  // runs.
  QMap<QString, QString> replacements;
};
struct PsdOptions {
  std::vector<PsdChoice> layers;
  bool composite{};
};
struct PsdJob {
  std::atomic<bool> cancelled{false};
  std::atomic<int> completed{0}, total{0};
  std::atomic<bool> renderingPreview{false};
  std::atomic<int> previewPercent{0};
};
struct PsdLimits {
  std::uint64_t fileBytes{512ULL * 1024 * 1024},
      rasterBytes{std::numeric_limits<std::uint64_t>::max()};
  // Zero selects an available-memory preflight when the job starts. Explicit
  // nonzero limits remain useful for constrained hosts and deterministic tests.
  std::uint64_t workingBytes{}, existingBytes{};
  std::uint32_t dimension{16384};
};
// Leaves one quarter of available RAM for the desktop. Available memory already
// excludes resident documents, so existingBytes is added back to the total cap.
[[nodiscard]] std::uint64_t psdWorkingMemoryLimit(std::uint64_t availableBytes,
                                               std::uint64_t existingBytes = 0);
struct PsdConversion {
  std::unique_ptr<core::Document> document;
  QImage preview;
  QString error, report;
  bool cancelled{};
  std::uint64_t rasterBytes{};
  std::uint64_t estimatedWorkingBytes{};
  qint64 milliseconds{};
  qint64 preparationMilliseconds{}, previewMilliseconds{};
};
[[nodiscard]] bool isPsdFile(const QString &);
[[nodiscard]] PsdInspection
inspectPsd(const QString &, const std::shared_ptr<PsdJob> &, PsdLimits = {});
[[nodiscard]] PsdOptions defaultPsdOptions(const PsdInspection &);
[[nodiscard]] PsdConversion convertPsd(const PsdInspection &,
                                       const PsdOptions &,
                                       const std::shared_ptr<PsdJob> &,
                                       PsdLimits = {}, bool preview = true);
// Shared bounded worker; abandoned futures never wait for decoder completion.
[[nodiscard]] std::future<PsdInspection>
    inspectPsdAsync(QString, std::shared_ptr<PsdJob>, PsdLimits = {});
[[nodiscard]] std::future<PsdConversion>
    convertPsdAsync(PsdInspection, PsdOptions, std::shared_ptr<PsdJob>,
                    PsdLimits = {}, bool preview = true);
} // namespace imageeditor::ui
