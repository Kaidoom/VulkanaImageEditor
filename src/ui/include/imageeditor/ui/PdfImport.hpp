#pragma once
#include "imageeditor/core/Layer.hpp"
#include <QImage>
#include <QSizeF>
#include <QString>
#include <atomic>
#include <future>
#include <memory>
#include <vector>

namespace imageeditor::ui {
// Import-only data. No PDF object, path dependency or password enters a Document.
struct PdfSource;
struct PdfPageInfo { QSizeF points; QString label; };
struct PdfMetadata {
    std::shared_ptr<const PdfSource> source;
    std::vector<PdfPageInfo> pages;
    QString error;
    bool passwordRequired {false};
};
enum class PdfDestination { NewDocument, SeparateDocuments, CurrentDocument };
struct PdfOptions {
    double ppi {300};
    bool whitePaper {true}, annotations {true};
    PdfDestination destination {PdfDestination::NewDocument};
    std::vector<int> pages; // Zero-based physical pages, strictly ascending.
};
struct PdfLimits {
    std::uint32_t maximumDimension {16384};
    std::uint64_t rasterBytes {256ULL * 1024 * 1024};
    std::uint64_t workingBytes {1536ULL * 1024 * 1024};
    std::uint64_t existingBytes {0};
    std::size_t availableLayers {4096};
    std::size_t currentDocumentLayers {4096};
    std::uint64_t currentDocumentRasterBytes {256ULL * 1024 * 1024};
};
struct PdfPlan {
    std::vector<QSize> sizes;
    QSize canvas;
    std::uint64_t rasterBytes {0}, estimatedWorkingBytes {0};
    QString error;
    explicit operator bool() const { return error.isEmpty() && !sizes.empty(); }
};
struct PdfJobState {
    std::atomic<bool> cancelled {false};
    std::atomic<int> completed {0}, physicalPage {0};
};
struct PdfRenderedPage { int page; core::Layer layer; QSize size; qint64 milliseconds; };
struct PdfRenderedPages {
    std::vector<PdfRenderedPage> pages;
    QString error;
    bool cancelled {false};
};
struct PdfThumbnail { QImage image; QString error; };

[[nodiscard]] bool isPdfFile(const QString& path);
[[nodiscard]] std::vector<int> parsePdfPageRange(const QString&, int count, QString& error);
[[nodiscard]] QString formatPdfPageRange(const std::vector<int>&);
[[nodiscard]] PdfPlan planPdfImport(const PdfMetadata&, const PdfOptions&, const PdfLimits& = {});
// All engine objects are created, used and destroyed on ONE shared worker.
// Futures are packaged tasks, not std::async: discarding one never blocks the UI.
[[nodiscard]] std::future<PdfMetadata> readPdfMetadata(QString path, QString password,
    std::shared_ptr<PdfJobState>);
[[nodiscard]] std::future<PdfThumbnail> renderPdfThumbnail(std::shared_ptr<const PdfSource>,
    int page, QSize size, PdfOptions, std::shared_ptr<PdfJobState>);
[[nodiscard]] std::future<PdfRenderedPages> renderPdfPages(PdfMetadata, PdfOptions, PdfLimits,
    std::shared_ptr<PdfJobState>);
[[nodiscard]] PdfOptions pdfImportPreferences();
void savePdfImportPreferences(const PdfOptions&);
} // namespace imageeditor::ui
