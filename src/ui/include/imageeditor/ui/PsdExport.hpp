#pragma once
#include "imageeditor/ui/FlattenedDocument.hpp"
#include <QMap>
#include <QStringList>
#include <atomic>

namespace imageeditor::ui {
enum class PsdExportMode { Layered, Flattened };
enum class PsdExportAction { Automatic, Editable, Pixels, Omit };
struct PsdExportOptions {
  PsdExportMode mode{PsdExportMode::Layered};
  bool preserveText{true};
  std::uint64_t instanceId{};
  QMap<qulonglong, PsdExportAction> actions;
  bool operator==(const PsdExportOptions &) const = default;
};
struct PsdExportLimits {
  std::uint64_t existingBytes{}, workingBytes{4ULL * 1024 * 1024 * 1024};
  std::uint64_t fileBytes{2ULL * 1024 * 1024 * 1024 - 1};
};
struct PsdExportSnapshot {
  std::shared_ptr<const core::Document> document;
  std::uint64_t instanceId{}, sourceBytes{};
  core::Revision revision{};
  PsdExportLimits limits;
};
struct PsdExportEntry {
  core::LayerId id{}, parent{};
  QString name, type;
  QStringList reasons, fonts;
  bool editable{true}, pixels{true}, attention{false}, container{false};
  PsdExportAction action{PsdExportAction::Editable};
};
struct PsdExportPlan {
  std::uint64_t instanceId{};
  core::Revision revision{};
  PsdExportOptions options;
  std::vector<PsdExportEntry> entries; // Top to bottom, including containers.
  QStringList notes;
  QString error;
  bool cancelled{false}, needsReview{false};
  explicit operator bool() const { return error.isEmpty() && !cancelled; }
};
struct PsdExportResult {
  QString error;
  bool cancelled{false};
  qint64 bytes{}, milliseconds{};
  int layers{};
  explicit operator bool() const {
    return error.isEmpty() && !cancelled && bytes > 0;
  }
};
using PsdExportProgress = std::function<bool(int, int, const QString &)>;
// Capture on the document owner thread. Workers own frozen pixels and immutable
// masks; neither a current tab nor a viewport cache participates in export.
[[nodiscard]] PsdExportSnapshot capturePsdExport(const core::Document &,
                                                 std::uint64_t instanceId,
                                                 PsdExportLimits = {});
[[nodiscard]] PsdExportPlan planPsdExport(const PsdExportSnapshot &,
                                          PsdExportOptions,
                                          const std::atomic_bool &cancel);
[[nodiscard]] FlattenedDocumentResult
previewPsdExport(const PsdExportSnapshot &, const PsdExportPlan &,
                 const std::atomic_bool &cancel);
[[nodiscard]] PsdExportResult writePsdExport(const PsdExportSnapshot &,
                                             const PsdExportPlan &,
                                             const QString &,
                                             const std::atomic_bool &cancel,
                                             PsdExportProgress = {});
} // namespace imageeditor::ui
