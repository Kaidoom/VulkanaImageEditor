#pragma once
#include "imageeditor/ui/FlattenedDocument.hpp"
#include <QColor>
#include <QSizeF>
#include <QStringList>
#include <atomic>

namespace imageeditor::ui {
enum class PdfExportMode { Composite, Pages };
enum class PdfExportSelection { All, Current, Custom };
enum class PdfExportPageSize { Canvas, Fit };
enum class PdfExportText { Preserve, Rasterize };
struct PdfExportOptions {
  PdfExportMode mode{PdfExportMode::Composite};
  PdfExportSelection selection{PdfExportSelection::All};
  PdfExportPageSize pageSize{PdfExportPageSize::Canvas};
  PdfExportText text{PdfExportText::Preserve};
  bool reverse{false}, ignoreHidden{true}, matte{false};
  QColor matteColor{Qt::white};
  double ppi{0}; // Uninitialized: use the owning document's PPI, not a global
                 // last PPI.
  std::uint64_t instanceId{0};
  std::vector<core::LayerId> chosen;
  bool operator==(const PdfExportOptions &) const = default;
};
struct PdfExportEntry {
  int number{0};
  core::LayerId id{0};
  QString name, folder;
  bool visible{true};
  std::vector<core::LayerId> leaves; // Bottom to top, already normalized.
};
struct PdfExportLimits {
  std::uint64_t existingBytes{
      0}; // Open documents, histories and retained caches.
  std::uint64_t workingBytes{2ULL * 1024 * 1024 * 1024};
};
struct PdfExportSnapshot {
  std::shared_ptr<const core::Document> document;
  std::uint64_t instanceId{0}, sourceBytes{0};
  core::Revision revision{0};
  std::vector<PdfExportEntry> entries; // Stable top-to-bottom source numbering.
  std::vector<core::LayerId> current;
  PdfExportLimits limits;
};
struct PdfExportPage {
  std::vector<core::LayerId> entries, leaves; // Leaves remain bottom to top.
  core::RectI rect;
  QSizeF points;
  QSize pixels;
  QString name;
  bool blankFallback{false};
  std::vector<core::LayerId> preservedText;
  QStringList textReasons;
};
struct PdfExportPlan {
  std::uint64_t instanceId{0};
  core::Revision revision{0};
  PdfExportOptions options;
  std::vector<PdfExportPage> pages;
  std::uint64_t rasterBytes{0}, peakWorkingBytes{0};
  int preserved{0}, rasterized{0};
  QString error;
  bool cancelled{false};
  explicit operator bool() const {
    return error.isEmpty() && !cancelled && !pages.empty();
  }
};
struct PdfExportResult {
  QString error;
  bool cancelled{false};
  qint64 bytes{0}, milliseconds{0};
  int pages{0};
  int preserved{0}, rasterized{0};
  QStringList textWarnings;
  std::vector<std::size_t> rasterizedPages;
  std::vector<qint64> pageMilliseconds;
  qint64 finalizationMilliseconds{0};
  explicit operator bool() const {
    return error.isEmpty() && !cancelled && pages > 0;
  }
};
using PdfExportProgress =
    std::function<bool(int page, int total, const QString &stage)>;
// Owner-thread deep freeze of mutable source pixels. No display caches,
// selection previews or mutable document owners cross to an export worker.
[[nodiscard]] PdfExportSnapshot
capturePdfExport(const core::Document &, std::uint64_t instanceId,
                 std::span<const core::LayerId> selection,
                 PdfExportLimits = {});
[[nodiscard]] PdfExportPlan planPdfExport(const PdfExportSnapshot &,
                                          PdfExportOptions,
                                          const std::atomic_bool &cancel);
[[nodiscard]] FlattenedDocumentResult
renderPdfExportPage(const PdfExportSnapshot &, const PdfExportPlan &,
                    std::size_t page, const std::atomic_bool &cancel,
                    std::optional<QSize> previewSize = {});
[[nodiscard]] PdfExportResult writePdfExport(const PdfExportSnapshot &,
                                             const PdfExportPlan &,
                                             const QString &destination,
                                             const std::atomic_bool &cancel,
                                             PdfExportProgress = {});
} // namespace imageeditor::ui
